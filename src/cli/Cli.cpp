#include "talas/cli/Cli.h"

#include "cli/ExportPluginCommand.h"

#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/CdpProcessToSample.h"
#include "talas/audio/GaAnalysis.h"
#include "talas/audio/ReAnalysisFiles.h"
#include "talas/audio/DiskCacheCapture.h"
#include "talas/audio/ResourceLoader.h"
#include "talas/audio/WavWriter.h"
#include "talas/audio/WavReader.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/core/NodeCatalogJson.h"
#include "talas/dsp/ReAnalysis.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectDiskCache.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectValidation.h"
#include "talas/runtime/ProjectRenderSettings.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/Runtime.h"

#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace talas::cli
{
namespace
{
constexpr int successExitCode = 0;
constexpr int failureExitCode = 1;
constexpr int usageExitCode = 2;
std::atomic<std::uint64_t> diskCacheCaptureSequence = 0;

struct TableResourceBinding
{
    std::string resourceId;
    std::filesystem::path path;
};

struct BinaryResourceBinding
{
    std::string resourceId;
    std::filesystem::path path;
};

struct AudioInputBinding
{
    std::size_t channel = 0;
    std::filesystem::path path;
};

enum class RenderOutputFormat
{
    Csv,
    Json
};

struct RenderCommand
{
    std::filesystem::path projectPath;
    std::optional<std::filesystem::path> outputPath;
    std::vector<TableResourceBinding> tableResources;
    std::vector<BinaryResourceBinding> binaryResources;
    std::vector<AudioInputBinding> audioInputs;
    std::vector<runtime::EventValueBinding> eventValues;
    std::vector<runtime::EventValueChange> eventValueChanges;
    std::size_t frameCount = 0;
    std::optional<double> durationSeconds;
    double sampleRate = 48000.0;
    std::size_t maxBlockFrames = 512;
    RenderOutputFormat format = RenderOutputFormat::Csv;
};

struct ValidateCommand
{
    std::filesystem::path projectPath;
    std::vector<TableResourceBinding> tableResources;
    std::vector<BinaryResourceBinding> binaryResources;
    std::vector<AudioInputBinding> audioInputs;
    std::vector<runtime::EventValueBinding> eventValues;
    std::vector<runtime::EventValueChange> eventValueChanges;
    double sampleRate = 48000.0;
    std::size_t maxBlockFrames = 512;
    bool status = false;
};

struct GraphCommand
{
    std::filesystem::path projectPath;
};

struct InitCommand
{
    std::string templateName;
    std::filesystem::path outputPath;
};

struct DiskCacheCommand
{
    std::filesystem::path projectPath;
    std::string sourcePath;
    std::string assetId;
    double sampleRate = 48000.0;
    std::size_t maxBlockFrames = 512;
    bool capture = false;
};

// The RE Analysis Tool (ADR-0022).
// CDP8's spectral processes that change a sound's length or order, as an
// offline process to a sample (ADR-0018 amendment, 2026-10-19).
// The GA Analysis tool.
struct GaAnalysisCommand
{
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    audio::GaAnalysisSettings settings;
};

struct CdpProcessCommand
{
    bool list = false;
    talas::cdp::OfflineSpectralRequest request;
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path output;
};

struct ReAnalysisCommand
{
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    std::filesystem::path excitationPath;
    dsp::ReAnalysisSettings settings;
};

bool parsePositiveSize(std::string_view text, std::size_t& value);
bool parsePositiveDouble(std::string_view text, double& value);
std::optional<std::string_view> optionValue(
    std::span<const std::string_view> args,
    std::size_t& index,
    std::string_view option,
    std::ostream& error);

void printUsage(std::ostream& output)
{
    output
        << "Usage:\n"
        << "  talas_cli --help\n"
        << "  talas_cli catalog [--format json]\n"
        << "  talas_cli graph --project <path> [--format dot]\n"
        << "  talas_cli init --template oscillator --output <path>\n"
        << "  talas_cli inspect-disk-cache --project <path> --source-path <expanded-path> [--sample-rate <hz>]\n"
        << "  talas_cli capture-disk-cache --project <path> --source-path <expanded-path> --asset-id <id> [--sample-rate <hz>] [--max-block-frames <count>]\n"
        << "  talas_cli re-analysis --input <sample.wav> --output <re-table.txt> [--excitation <ex.wav>] [--order <1-128>] [--updates-per-second <count>] [--averaging-time <seconds>]\n"
        << "    writes an REResonator's time-varying filter (an RE table) and, with --excitation, the input that recreates the sample through it.\n"
        << "  talas_cli ga-analysis --input <sample.wav> --output <analysis GA.wav> [--waveforms <1-64>] [--harmonics <1-1024>] [--frames-per-second <1-1000>]\n"
        << "    writes a GA analysis (complex waveforms, their amplitude envelopes and a frequency deviation envelope) for a GAOscillators.\n"
        << "  talas_cli cdp-process --list\n"
        << "  talas_cli cdp-process --process <id> --input <sample.wav>... --output <out.wav> [--points <2-8192>] [--overlap <1-4>] [--set <field>=<value>]...\n"
        << "    renders samples through `pvoc anal`, a CDP8 spectral process that changes their length or order (stretch time, focus hold, spec cut, grab and magnify, combine interleave, blur drunk, shuffle and weave) and `pvoc synth`, as CDP8 writes it, to a 32-bit float WAV.\n"
        << "  talas_cli validate --project <path> [--status] [--sample-rate <hz>] [--max-block-frames <count>] [--event <name[:domain]=value>]... [--event-change <frame:name[:domain]=value>]... [--table <resourceId=path>]... [--binary-resource <resourceId=path>]... [--audio-input <channel=path.wav>]...\n"
        << "    --status prints the prepared graph contract: outputs/device attachment, portable logical-processor placement, latency alignment, spectral frames, resources, streams, MIDI mappers, and limitations.\n"
        << "  talas_cli render --project <path> (--frames <count>|--duration <seconds>) [--sample-rate <hz>] [--max-block-frames <count>] [--event <name[:domain]=value>]... [--event-change <frame:name[:domain]=value>]... [--format csv|json] [--output <path.wav>] [--table <resourceId=path>]... [--binary-resource <resourceId=path>]... [--audio-input <channel=path.wav>]...\n"
        << "  talas_cli export-plugin --project <path> --name <name> --kind instrument|effect --output <dir> [--format vst3,au] [--manufacturer <name>] [--manufacturer-code <code>] [--plugin-code <code>] [--version <x.y.z>] [--template-dir <dir>]... [--no-sign]\n"
        << "    copies a prebuilt plugin template, embeds the project, and gives the VST3 and AU bundles their own name, codes and parameters (the project's Event Values); see docs/plugin-export.md.\n";
}

std::optional<DiskCacheCommand> parseDiskCacheCommand(
    std::span<const std::string_view> args,
    bool capture,
    std::ostream& error)
{
    DiskCacheCommand command;
    command.capture = capture;
    for (std::size_t index = 1; index < args.size(); ++index)
    {
        const auto option = args[index];
        if (option == "--project")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value()) return std::nullopt;
            command.projectPath = std::filesystem::path{std::string{*value}};
        }
        else if (option == "--source-path")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value()) return std::nullopt;
            command.sourcePath = std::string{*value};
        }
        else if (option == "--asset-id")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value()) return std::nullopt;
            command.assetId = std::string{*value};
        }
        else if (option == "--sample-rate")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value()
                || !parsePositiveDouble(*value, command.sampleRate)
                || std::floor(command.sampleRate) != command.sampleRate
                || command.sampleRate > static_cast<double>(
                    std::numeric_limits<std::uint32_t>::max()))
            {
                error << "--sample-rate must be a whole number of hertz\n";
                return std::nullopt;
            }
        }
        else if (option == "--max-block-frames")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value()
                || !parsePositiveSize(*value, command.maxBlockFrames))
            {
                error << "--max-block-frames must be a positive integer\n";
                return std::nullopt;
            }
        }
        else
        {
            error << "Unknown DiskCache option: " << option << '\n';
            return std::nullopt;
        }
    }
    if (command.projectPath.empty() || command.sourcePath.empty()
        || (capture && command.assetId.empty()))
    {
        error << (capture
            ? "capture-disk-cache requires --project, --source-path, and --asset-id\n"
            : "inspect-disk-cache requires --project and --source-path\n");
        return std::nullopt;
    }
    return command;
}

std::optional<ReAnalysisCommand> parseReAnalysisCommand(
    std::span<const std::string_view> args,
    std::ostream& error)
{
    ReAnalysisCommand command;
    for (std::size_t index = 1; index < args.size(); ++index)
    {
        const auto option = args[index];
        const auto value = optionValue(args, index, option, error);
        if (!value.has_value())
            return std::nullopt;
        if (option == "--input")
            command.inputPath = std::filesystem::path{std::string{*value}};
        else if (option == "--output")
            command.outputPath = std::filesystem::path{std::string{*value}};
        else if (option == "--excitation")
            command.excitationPath = std::filesystem::path{std::string{*value}};
        else if (option == "--order")
        {
            if (!parsePositiveSize(*value, command.settings.order)
                || command.settings.order > 128)
            {
                error << "--order must be a whole number from 1 to 128\n";
                return std::nullopt;
            }
        }
        else if (option == "--updates-per-second")
        {
            if (!parsePositiveDouble(*value, command.settings.updatesPerSecond))
            {
                error << "--updates-per-second must be a positive number\n";
                return std::nullopt;
            }
        }
        else if (option == "--averaging-time")
        {
            if (!parsePositiveDouble(*value, command.settings.averagingTimeSeconds))
            {
                error << "--averaging-time must be a positive number of seconds\n";
                return std::nullopt;
            }
        }
        else
        {
            error << "Unknown RE analysis option: " << option << '\n';
            return std::nullopt;
        }
    }
    if (command.inputPath.empty() || command.outputPath.empty())
    {
        error << "re-analysis requires --input and --output\n";
        return std::nullopt;
    }
    return command;
}

bool parsePositiveSize(std::string_view text, std::size_t& value)
{
    if (text.empty())
        return false;

    unsigned long long parsed = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last || parsed == 0)
        return false;
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        return false;

    value = static_cast<std::size_t>(parsed);
    return true;
}

bool parseNonNegativeSize(std::string_view text, std::size_t& value)
{
    if (text.empty())
        return false;

    unsigned long long parsed = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last)
        return false;
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        return false;

    value = static_cast<std::size_t>(parsed);
    return true;
}

bool parseFiniteDouble(std::string_view text, double& value)
{
    if (text.empty())
        return false;

    const auto* last = text.data() + text.size();
    const auto parsed = std::from_chars(
        text.data(), last, value, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == last
        && std::isfinite(value);
}

bool parsePositiveDouble(std::string_view text, double& value)
{
    return parseFiniteDouble(text, value) && value > 0.0;
}

std::optional<std::string_view> optionValue(
    std::span<const std::string_view> args,
    std::size_t& index,
    std::string_view option,
    std::ostream& error)
{
    if (index + 1 >= args.size())
    {
        error << option << " requires a value\n";
        return std::nullopt;
    }

    ++index;
    return args[index];
}

std::optional<TableResourceBinding> parseTableResourceBinding(std::string_view value, std::ostream& error)
{
    const auto separator = value.find('=');
    if (separator == std::string_view::npos)
    {
        error << "--table requires resourceId=path.wav\n";
        return std::nullopt;
    }

    const auto resourceId = value.substr(0, separator);
    const auto path = value.substr(separator + 1);
    if (resourceId.empty())
    {
        error << "--table resourceId must not be empty\n";
        return std::nullopt;
    }
    if (path.empty())
    {
        error << "--table path must not be empty\n";
        return std::nullopt;
    }

    return TableResourceBinding{
        std::string(resourceId),
        std::filesystem::path(std::string(path)),
    };
}

std::optional<BinaryResourceBinding> parseBinaryResourceBinding(std::string_view value, std::ostream& error)
{
    const auto separator = value.find('=');
    if (separator == std::string_view::npos)
    {
        error << "--binary-resource requires resourceId=path\n";
        return std::nullopt;
    }

    const auto resourceId = value.substr(0, separator);
    const auto path = value.substr(separator + 1);
    if (resourceId.empty())
    {
        error << "--binary-resource resourceId must not be empty\n";
        return std::nullopt;
    }
    if (path.empty())
    {
        error << "--binary-resource path must not be empty\n";
        return std::nullopt;
    }

    return BinaryResourceBinding{
        std::string(resourceId),
        std::filesystem::path(std::string(path)),
    };
}

std::optional<AudioInputBinding> parseAudioInputBinding(std::string_view value, std::ostream& error)
{
    const auto separator = value.find('=');
    if (separator == std::string_view::npos)
    {
        error << "--audio-input requires channel=path.wav\n";
        return std::nullopt;
    }

    const auto channelText = value.substr(0, separator);
    const auto path = value.substr(separator + 1);
    std::size_t channel = 0;
    if (!parsePositiveSize(channelText, channel) || channel > 8)
    {
        error << "--audio-input channel must be an integer from 1 through 8\n";
        return std::nullopt;
    }
    if (path.empty())
    {
        error << "--audio-input path must not be empty\n";
        return std::nullopt;
    }

    return AudioInputBinding{
        channel,
        std::filesystem::path(std::string(path)),
    };
}

bool isEventValueIdentifierStart(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool isEventValueIdentifierContinue(char value)
{
    return isEventValueIdentifierStart(value) || (value >= '0' && value <= '9');
}

bool isEventValueIdentifier(std::string_view value)
{
    if (value.empty() || !isEventValueIdentifierStart(value.front()))
        return false;

    for (const auto character : value.substr(1))
    {
        if (!isEventValueIdentifierContinue(character))
            return false;
    }
    return true;
}

std::optional<talas::expr::ValueDomain> parseEventValueDomain(std::string_view value, std::ostream& error)
{
    if (value == "scalar")
        return talas::expr::ValueDomain::Scalar;
    if (value == "hz")
        return talas::expr::ValueDomain::Hertz;
    if (value == "s")
        return talas::expr::ValueDomain::Seconds;
    if (value == "nn")
        return talas::expr::ValueDomain::NoteNumber;
    if (value == "samples")
        return talas::expr::ValueDomain::Samples;

    error << "Unknown Event Value domain: " << value << '\n';
    return std::nullopt;
}

std::optional<runtime::EventValueBinding> parseEventValueBinding(
    std::string_view value,
    std::string_view option,
    std::ostream& error)
{
    const auto equals = value.find('=');
    if (equals == std::string_view::npos)
    {
        error << option << " requires name[:domain]=value\n";
        return std::nullopt;
    }

    const auto nameAndDomain = value.substr(0, equals);
    const auto valueText = value.substr(equals + 1);
    const auto colon = nameAndDomain.find(':');
    const auto name = nameAndDomain.substr(0, colon);
    if (!isEventValueIdentifier(name))
    {
        error << option << " name must be an Event Value identifier\n";
        return std::nullopt;
    }

    auto domain = talas::expr::ValueDomain::Scalar;
    if (colon != std::string_view::npos)
    {
        const auto domainText = nameAndDomain.substr(colon + 1);
        auto parsedDomain = parseEventValueDomain(domainText, error);
        if (!parsedDomain.has_value())
            return std::nullopt;
        domain = *parsedDomain;
    }

    double parsedValue = 0.0;
    if (!parseFiniteDouble(valueText, parsedValue))
    {
        error << option << " value must be finite\n";
        return std::nullopt;
    }

    return runtime::EventValueBinding{
        std::string(name),
        parsedValue,
        domain,
    };
}

std::optional<runtime::EventValueChange> parseEventValueChange(std::string_view value, std::ostream& error)
{
    const auto colon = value.find(':');
    if (colon == std::string_view::npos)
    {
        error << "--event-change requires frame:name[:domain]=value\n";
        return std::nullopt;
    }

    std::size_t frame = 0;
    if (!parseNonNegativeSize(value.substr(0, colon), frame))
    {
        error << "--event-change frame must be a non-negative integer\n";
        return std::nullopt;
    }

    auto binding = parseEventValueBinding(value.substr(colon + 1), "--event-change", error);
    if (!binding.has_value())
        return std::nullopt;

    return runtime::EventValueChange{
        frame,
        std::move(binding->name),
        binding->value,
        binding->domain,
    };
}

bool hasTableResourceBinding(const RenderCommand& command, std::string_view resourceId)
{
    for (const auto& binding : command.tableResources)
    {
        if (binding.resourceId == resourceId)
            return true;
    }
    return false;
}

bool hasBinaryResourceBinding(const RenderCommand& command, std::string_view resourceId)
{
    for (const auto& binding : command.binaryResources)
    {
        if (binding.resourceId == resourceId)
            return true;
    }
    return false;
}

bool hasAudioInputBinding(const RenderCommand& command, std::size_t channel)
{
    for (const auto& binding : command.audioInputs)
    {
        if (binding.channel == channel)
            return true;
    }
    return false;
}

bool hasBinaryResourceBinding(const ValidateCommand& command, std::string_view resourceId)
{
    for (const auto& binding : command.binaryResources)
    {
        if (binding.resourceId == resourceId)
            return true;
    }
    return false;
}

bool hasAudioInputBinding(const ValidateCommand& command, std::size_t channel)
{
    for (const auto& binding : command.audioInputs)
    {
        if (binding.channel == channel)
            return true;
    }
    return false;
}

bool resolveDurationFrameCount(RenderCommand& command, std::ostream& error)
{
    if (!command.durationSeconds.has_value())
        return true;

    const double frameCount = std::ceil(*command.durationSeconds * command.sampleRate);
    if (!std::isfinite(frameCount) || frameCount <= 0.0)
    {
        error << "--duration and --sample-rate must produce a positive finite frame count\n";
        return false;
    }
    if (frameCount >= static_cast<double>(std::numeric_limits<std::size_t>::max()))
    {
        error << "--duration and --sample-rate produce too many frames\n";
        return false;
    }

    command.frameCount = static_cast<std::size_t>(frameCount);
    return true;
}

std::optional<RenderOutputFormat> parseRenderOutputFormat(std::string_view value, std::ostream& error)
{
    if (value == "csv")
        return RenderOutputFormat::Csv;
    if (value == "json")
        return RenderOutputFormat::Json;

    error << "Unsupported render format: " << value << '\n';
    return std::nullopt;
}

std::optional<RenderCommand> parseRenderCommand(std::span<const std::string_view> args, std::ostream& error)
{
    RenderCommand command;
    bool hasProject = false;
    bool hasFrames = false;
    bool hasDuration = false;
    bool hasFormat = false;

    for (std::size_t index = 1; index < args.size(); ++index)
    {
        const auto option = args[index];
        if (option == "--project")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            command.projectPath = std::filesystem::path(std::string(*value));
            hasProject = true;
        }
        else if (option == "--frames")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            if (!parsePositiveSize(*value, command.frameCount))
            {
                error << "--frames must be greater than zero\n";
                return std::nullopt;
            }
            hasFrames = true;
        }
        else if (option == "--duration")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            double durationSeconds = 0.0;
            if (!parsePositiveDouble(*value, durationSeconds))
            {
                error << "--duration must be finite and greater than zero\n";
                return std::nullopt;
            }
            command.durationSeconds = durationSeconds;
            hasDuration = true;
        }
        else if (option == "--sample-rate")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            if (!parsePositiveDouble(*value, command.sampleRate))
            {
                error << "--sample-rate must be finite and greater than zero\n";
                return std::nullopt;
            }
        }
        else if (option == "--max-block-frames")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            if (!parsePositiveSize(*value, command.maxBlockFrames))
            {
                error << "--max-block-frames must be greater than zero\n";
                return std::nullopt;
            }
        }
        else if (option == "--output")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            command.outputPath = std::filesystem::path(std::string(*value));
        }
        else if (option == "--table")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            auto binding = parseTableResourceBinding(*value, error);
            if (!binding.has_value())
                return std::nullopt;
            if (hasTableResourceBinding(command, binding->resourceId))
            {
                error << "Duplicate table resource: " << binding->resourceId << '\n';
                return std::nullopt;
            }
            command.tableResources.push_back(std::move(*binding));
        }
        else if (option == "--binary-resource")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            auto binding = parseBinaryResourceBinding(*value, error);
            if (!binding.has_value())
                return std::nullopt;
            if (hasBinaryResourceBinding(command, binding->resourceId))
            {
                error << "Duplicate binary resource: " << binding->resourceId << '\n';
                return std::nullopt;
            }
            command.binaryResources.push_back(std::move(*binding));
        }
        else if (option == "--audio-input")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            auto binding = parseAudioInputBinding(*value, error);
            if (!binding.has_value())
                return std::nullopt;
            if (hasAudioInputBinding(command, binding->channel))
            {
                error << "Duplicate audio input channel: " << binding->channel << '\n';
                return std::nullopt;
            }
            command.audioInputs.push_back(std::move(*binding));
        }
        else if (option == "--event")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            auto binding = parseEventValueBinding(*value, option, error);
            if (!binding.has_value())
                return std::nullopt;
            command.eventValues.push_back(std::move(*binding));
        }
        else if (option == "--event-change")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            auto change = parseEventValueChange(*value, error);
            if (!change.has_value())
                return std::nullopt;
            command.eventValueChanges.push_back(std::move(*change));
        }
        else if (option == "--format")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            auto format = parseRenderOutputFormat(*value, error);
            if (!format.has_value())
                return std::nullopt;
            command.format = *format;
            hasFormat = true;
        }
        else if (option == "--help" || option == "-h")
        {
            error << "render help must be requested as talas_cli --help\n";
            return std::nullopt;
        }
        else
        {
            error << "Unknown option: " << option << '\n';
            return std::nullopt;
        }
    }

    if (!hasProject)
    {
        error << "render requires --project <path>\n";
        return std::nullopt;
    }
    if (hasFrames && hasDuration)
    {
        error << "--frames and --duration cannot both be specified\n";
        return std::nullopt;
    }
    if (!hasFrames && !hasDuration)
    {
        error << "render requires either --frames <count> or --duration <seconds>\n";
        return std::nullopt;
    }
    if (hasFormat && command.outputPath.has_value())
    {
        error << "--format selects standard output and cannot be combined with --output <path.wav>\n";
        return std::nullopt;
    }
    if (!resolveDurationFrameCount(command, error))
        return std::nullopt;

    return command;
}

std::optional<ValidateCommand> parseValidateCommand(std::span<const std::string_view> args, std::ostream& error)
{
    ValidateCommand command;
    bool hasProject = false;

    for (std::size_t index = 1; index < args.size(); ++index)
    {
        const auto option = args[index];
        if (option == "--project")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            command.projectPath = std::filesystem::path(std::string(*value));
            hasProject = true;
        }
        else if (option == "--sample-rate")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            if (!parsePositiveDouble(*value, command.sampleRate))
            {
                error << "--sample-rate must be finite and greater than zero\n";
                return std::nullopt;
            }
        }
        else if (option == "--max-block-frames")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            if (!parsePositiveSize(*value, command.maxBlockFrames))
            {
                error << "--max-block-frames must be greater than zero\n";
                return std::nullopt;
            }
        }
        else if (option == "--status")
        {
            command.status = true;
        }
        else if (option == "--table")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            auto binding = parseTableResourceBinding(*value, error);
            if (!binding.has_value())
                return std::nullopt;
            for (const auto& existing : command.tableResources)
            {
                if (existing.resourceId == binding->resourceId)
                {
                    error << "Duplicate table resource: " << binding->resourceId << '\n';
                    return std::nullopt;
                }
            }
            command.tableResources.push_back(std::move(*binding));
        }
        else if (option == "--binary-resource")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            auto binding = parseBinaryResourceBinding(*value, error);
            if (!binding.has_value())
                return std::nullopt;
            if (hasBinaryResourceBinding(command, binding->resourceId))
            {
                error << "Duplicate binary resource: " << binding->resourceId << '\n';
                return std::nullopt;
            }
            command.binaryResources.push_back(std::move(*binding));
        }
        else if (option == "--audio-input")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            auto binding = parseAudioInputBinding(*value, error);
            if (!binding.has_value())
                return std::nullopt;
            if (hasAudioInputBinding(command, binding->channel))
            {
                error << "Duplicate audio input channel: " << binding->channel << '\n';
                return std::nullopt;
            }
            command.audioInputs.push_back(std::move(*binding));
        }
        else if (option == "--event")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            auto binding = parseEventValueBinding(*value, option, error);
            if (!binding.has_value())
                return std::nullopt;
            command.eventValues.push_back(std::move(*binding));
        }
        else if (option == "--event-change")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            auto change = parseEventValueChange(*value, error);
            if (!change.has_value())
                return std::nullopt;
            command.eventValueChanges.push_back(std::move(*change));
        }
        else if (option == "--help" || option == "-h")
        {
            error << "validate help must be requested as talas_cli --help\n";
            return std::nullopt;
        }
        else
        {
            error << "Unknown validate option: " << option << '\n';
            return std::nullopt;
        }
    }

    if (!hasProject)
    {
        error << "validate requires --project <path>\n";
        return std::nullopt;
    }

    return command;
}

std::optional<GraphCommand> parseGraphCommand(std::span<const std::string_view> args, std::ostream& error)
{
    GraphCommand command;
    bool hasProject = false;
    std::string_view format = "dot";

    for (std::size_t index = 1; index < args.size(); ++index)
    {
        const auto option = args[index];
        if (option == "--project")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            command.projectPath = std::filesystem::path(std::string(*value));
            hasProject = true;
        }
        else if (option == "--format")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            format = *value;
        }
        else if (option == "--help" || option == "-h")
        {
            error << "graph help must be requested as talas_cli --help\n";
            return std::nullopt;
        }
        else
        {
            error << "Unknown graph option: " << option << '\n';
            return std::nullopt;
        }
    }

    if (!hasProject)
    {
        error << "graph requires --project <path>\n";
        return std::nullopt;
    }
    if (format != "dot")
    {
        error << "Unsupported graph format: " << format << '\n';
        return std::nullopt;
    }

    return command;
}

std::optional<InitCommand> parseInitCommand(std::span<const std::string_view> args, std::ostream& error)
{
    InitCommand command;
    bool hasTemplate = false;
    bool hasOutput = false;

    for (std::size_t index = 1; index < args.size(); ++index)
    {
        const auto option = args[index];
        if (option == "--template")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            command.templateName = std::string(*value);
            hasTemplate = true;
        }
        else if (option == "--output")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return std::nullopt;
            command.outputPath = std::filesystem::path(std::string(*value));
            hasOutput = true;
        }
        else if (option == "--help" || option == "-h")
        {
            error << "init help must be requested as talas_cli --help\n";
            return std::nullopt;
        }
        else
        {
            error << "Unknown init option: " << option << '\n';
            return std::nullopt;
        }
    }

    if (!hasTemplate)
    {
        error << "init requires --template <name>\n";
        return std::nullopt;
    }
    if (!hasOutput)
    {
        error << "init requires --output <path>\n";
        return std::nullopt;
    }
    if (command.templateName != "oscillator")
    {
        error << "Unsupported init template: " << command.templateName << '\n';
        return std::nullopt;
    }

    return command;
}

bool loadTableResources(std::span<const TableResourceBinding> tableResources, runtime::RenderSettings& settings, std::ostream& error)
{
    for (const auto& binding : tableResources)
    {
        auto loaded = audio::loadTableResource(binding.path);
        if (!loaded.ok())
        {
            error << "Failed to load table resource '" << binding.resourceId << "': " << loaded.error << '\n';
            return false;
        }
        runtime::TableResource table{binding.resourceId, std::move(loaded.samples)};
        table.rightSamples = std::move(loaded.rightSamples);
        settings.tableResources.push_back(std::move(table));
    }
    return true;
}

bool loadBinaryResources(std::span<const BinaryResourceBinding> binaryResources, runtime::RenderSettings& settings, std::ostream& error)
{
    for (const auto& binding : binaryResources)
    {
        auto loaded = audio::loadBinaryResource(binding.path);
        if (!loaded.ok())
        {
            error << "Failed to load binary resource '" << binding.resourceId << "': " << loaded.error << '\n';
            return false;
        }
        settings.binaryResources.push_back({binding.resourceId, std::move(loaded.bytes)});
    }
    return true;
}

bool loadPersistedProjectAssets(
    const project::ProjectDocument& document,
    const core::NodeRegistry& registry,
    const std::filesystem::path& projectPath,
    std::span<const TableResourceBinding> tableOverrides,
    std::span<const BinaryResourceBinding> binaryOverrides,
    runtime::RenderSettings& settings,
    std::ostream& error)
{
    std::vector<std::string> excludedIds;
    excludedIds.reserve(tableOverrides.size() + binaryOverrides.size());
    for (const auto& binding : tableOverrides)
        excludedIds.push_back(binding.resourceId);
    for (const auto& binding : binaryOverrides)
        excludedIds.push_back(binding.resourceId);

    auto loaded = audio::loadReferencedProjectResourceSnapshots(
        document,
        registry,
        projectPath,
        excludedIds);
    if (!loaded.ok())
    {
        const auto& failure = *loaded.failure;
        error << audio::describeProjectAssetLoadFailure(failure)
              << '\n';
        return false;
    }
    settings.resourceSnapshots.insert(
        settings.resourceSnapshots.end(),
        std::make_move_iterator(loaded.snapshots.begin()),
        std::make_move_iterator(loaded.snapshots.end()));
    return true;
}

bool loadAudioInputs(std::span<const AudioInputBinding> audioInputs, runtime::RenderSettings& settings, std::ostream& error)
{
    for (const auto& binding : audioInputs)
    {
        auto read = audio::readPcm16Wav(binding.path);
        if (!read.ok())
        {
            error << "Failed to load audio input channel " << binding.channel << ": " << read.error << '\n';
            return false;
        }
        if (read.channels.size() != 1)
        {
            error << "Failed to load audio input channel " << binding.channel << ": audio input WAV files must be mono\n";
            return false;
        }
        settings.audioInputChannels.push_back({binding.channel, std::move(read.channels[0])});
    }
    return true;
}

std::vector<std::vector<double>> fallbackChannels(const runtime::OfflineRenderResult& rendered)
{
    std::vector<std::vector<double>> channels;
    channels.push_back(rendered.mono);
    if (!rendered.right.empty())
        channels.push_back(rendered.right);
    return channels;
}

std::optional<std::string> readTextFile(const std::filesystem::path& path, std::ostream& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error << "Failed to read project '" << project::pathToUtf8(path) << "'\n";
        return std::nullopt;
    }

    std::ostringstream content;
    content << input.rdbuf();
    if (input.bad())
    {
        error << "Failed to read project '" << project::pathToUtf8(path) << "'\n";
        return std::nullopt;
    }
    return content.str();
}

struct CliProjectInput
{
    project::ProjectDocument document;
    std::filesystem::path projectFilePath;
};

std::optional<CliProjectInput> loadCliProjectInput(
    const std::filesystem::path& inputPath,
    std::ostream& error)
{
    const auto resolved =
        project::resolveProjectPackagePath(inputPath);
    if (!resolved.ok())
    {
        error << "Project package failed: "
              << resolved.error << '\n';
        return std::nullopt;
    }

    const auto json =
        readTextFile(resolved.projectFilePath, error);
    if (!json.has_value())
        return std::nullopt;
    auto loaded = project::parseProjectDocument(*json);
    if (!loaded.ok())
    {
        error << "Project parse failed:\n";
        for (const auto& parseError : loaded.errors)
        {
            error << "  " << parseError.path << ": "
                  << parseError.message << '\n';
        }
        return std::nullopt;
    }

    const auto packageValidation =
        project::validateProjectPackageAssets(
            loaded.document,
            resolved);
    if (!packageValidation.ok())
    {
        error << "Project package asset validation failed";
        if (!packageValidation.assetId.empty())
        {
            error << " for '" << packageValidation.assetId
                  << "'";
        }
        error << ": " << packageValidation.error << '\n';
        return std::nullopt;
    }
    return CliProjectInput{
        std::move(loaded.document),
        resolved.projectFilePath,
    };
}

std::string diagnosticCodeName(runtime::DiagnosticCode code)
{
    switch (code)
    {
    case runtime::DiagnosticCode::InvalidRenderSettings:
        return "InvalidRenderSettings";
    case runtime::DiagnosticCode::GraphValidationFailed:
        return "GraphValidationFailed";
    case runtime::DiagnosticCode::UnsupportedNodeType:
        return "UnsupportedNodeType";
    case runtime::DiagnosticCode::MissingAudioOutput:
        return "MissingAudioOutput";
    case runtime::DiagnosticCode::MultipleAudioOutputs:
        return "MultipleAudioOutputs";
    case runtime::DiagnosticCode::DuplicateInputConnection:
        return "DuplicateInputConnection";
    case runtime::DiagnosticCode::UnknownParameter:
        return "UnknownParameter";
    case runtime::DiagnosticCode::DuplicateParameter:
        return "DuplicateParameter";
    case runtime::DiagnosticCode::ParameterParseError:
        return "ParameterParseError";
    case runtime::DiagnosticCode::ParameterEvaluationError:
        return "ParameterEvaluationError";
    case runtime::DiagnosticCode::NegativeFrequency:
        return "NegativeFrequency";
    case runtime::DiagnosticCode::RenderAllocationFailed:
        return "RenderAllocationFailed";
    }

    return "UnknownDiagnostic";
}

void printRuntimeDiagnostics(
    std::ostream& error,
    std::string_view heading,
    const std::vector<runtime::Diagnostic>& diagnostics)
{
    error << heading << ":\n";
    for (const auto& diagnostic : diagnostics)
    {
        error << "  " << diagnosticCodeName(diagnostic.code);
        if (!diagnostic.nodeId.empty())
            error << " node=" << diagnostic.nodeId;
        if (!diagnostic.portId.empty())
            error << " port=" << diagnostic.portId;
        if (!diagnostic.parameterId.empty())
            error << " parameter=" << diagnostic.parameterId;
        if (!diagnostic.message.empty())
            error << ": " << diagnostic.message;
        error << '\n';
    }
}

[[nodiscard]] bool rejectUnavailableProjectComponents(
    const project::ProjectDocument& document,
    const core::NodeRegistry& registry,
    std::ostream* portableStatus,
    std::ostream& error)
{
    const auto diagnostics =
        runtime::projectComponentExecutionDiagnostics(
            document,
            registry);
    if (diagnostics.empty())
        return false;

    if (portableStatus != nullptr
        && project::validateProjectDocumentForEditing(
               document,
               registry)
               .ok())
    {
        *portableStatus
            << "Portable document preserved: yes\n"
            << "Executable in this registry: no\n";
    }
    printRuntimeDiagnostics(
        error,
        "Compile failed",
        diagnostics);
    return true;
}

std::string statusNumber(double value)
{
    std::ostringstream output;
    output << std::setprecision(12) << value;
    return output.str();
}

std::string statusFrames(
    const std::optional<std::size_t>& value)
{
    return value.has_value()
        ? std::to_string(*value) + " frames"
        : std::string{"unknown"};
}

std::string_view statusWindowName(
    const std::optional<talas::dsp::SpectralWindowKind>& window)
{
    if (!window.has_value())
        return "unknown";
    return *window == talas::dsp::SpectralWindowKind::Hann
        ? std::string_view{"hann"}
        : std::string_view{"rectangular"};
}

void writePreparedGraphStatus(
    const runtime::PreparedGraphSummary& summary,
    std::ostream& output)
{
    output << "Prepared graph status\n"
           << "  sample-rate: "
           << statusNumber(summary.sampleRate) << " Hz\n"
           << "  max-block: " << summary.maxBlockFrames
           << " frames\n"
           << "  outputs: "
           << runtime::preparedOutputLayoutName(
                  summary.outputLayout)
           << " (" << summary.outputs.size() << ")\n";
    for (const auto& channel : summary.outputs)
    {
        output << "    " << channel.projectOrdinal << ": "
               << channel.nodeId << "." << channel.portId
               << " ["
               << (channel.structuralSourcePath.empty()
                       ? channel.nodeId
                       : channel.structuralSourcePath)
               << "] scope=" << channel.scopeId
               << " device=not-attached\n";
    }
    if (!summary.diskRecorderCapture.has_value())
    {
        output << "  disk-recorder: none\n";
    }
    else
    {
        const auto& recorder = *summary.diskRecorderCapture;
        output << "  disk-recorder: "
               << runtime::diskRecorderWriteSupportName(
                      recorder.writeSupport)
               << " node=" << recorder.nodeId
               << " scope=" << recorder.scopeId
               << " buffer=" << recorder.captureBufferIndex
               << " profile="
               << (recorder.channels.empty()
                       ? "default-mono"
                       : recorder.channels)
               << "/"
               << (recorder.format.empty()
                       ? "default-WAV"
                       : recorder.format)
               << "/"
               << (recorder.wordSize.empty()
                       ? "default-16"
                       : recorder.wordSize)
               << " trigger=" << statusNumber(recorder.trigger)
               << " duration="
               << statusNumber(recorder.captureDurationSeconds)
               << "s gated=" << (recorder.gated ? "true" : "false")
               << " owner-action=required path-authority=none"
               << " name-hint=" << recorder.destinationNameHint
               << "\n";
    }

    if (summary.processorPlacements.empty())
    {
        output << "  logical-processors: none\n";
    }
    else
    {
        output << "  logical-processors: "
               << summary.processorPlacements.size()
               << " mode=portable-serial\n";
        for (const auto& domain : summary.processorPlacements)
        {
            output << "    processor " << domain.processorId
                   << " steps=" << domain.steps.size()
                   << " boundaries=" << domain.boundaries.size()
                   << "\n";
            for (const auto& step : domain.steps)
            {
                output << "      step " << step.nodeId << " ["
                       << (step.structuralSourcePath.empty()
                               ? step.nodeId
                               : step.structuralSourcePath)
                       << "] scope=" << step.scopeId
                       << " type=" << step.typeId << "\n";
            }
            for (const auto& boundary : domain.boundaries)
            {
                output << "      boundary " << boundary.nodeId
                       << " ["
                       << (boundary.structuralSourcePath.empty()
                               ? boundary.nodeId
                               : boundary.structuralSourcePath)
                       << "] scope=" << boundary.scopeId << "\n";
            }
        }
    }
    if (summary.processorTransfers.empty())
    {
        output << "  processor-transfers: none\n";
    }
    else
    {
        output << "  processor-transfers: "
               << summary.processorTransfers.size() << "\n";
        for (const auto& transfer : summary.processorTransfers)
        {
            output << "    " << transfer.source.nodeId << "."
                   << transfer.sourcePortId << " ["
                   << (transfer.source.structuralSourcePath.empty()
                           ? transfer.source.nodeId
                           : transfer.source.structuralSourcePath)
                   << "] scope=" << transfer.source.scopeId
                   << " processor=" << transfer.sourceProcessorId
                   << " -> " << transfer.target.nodeId << "."
                   << transfer.targetPortId << " ["
                   << (transfer.target.structuralSourcePath.empty()
                           ? transfer.target.nodeId
                           : transfer.target.structuralSourcePath)
                   << "] scope=" << transfer.target.scopeId
                   << " processor=" << transfer.targetProcessorId
                   << "\n";
        }
    }

    output << "  critical-path: "
           << statusFrames(summary.latency.criticalPathFrames)
           << "\n"
           << "  terminal-output: "
           << statusFrames(summary.latency.terminalOutputFrames)
           << "\n";
    if (summary.latency.compensations.empty())
    {
        output << "  compensations: none\n";
    }
    else
    {
        output << "  compensations: "
               << summary.latency.compensations.size() << "\n";
        for (const auto& compensation :
             summary.latency.compensations)
        {
            output << "    " << compensation.nodeId << "."
                   << compensation.inputPortId << " ["
                   << (compensation.structuralSourcePath.empty()
                           ? compensation.nodeId
                           : compensation.structuralSourcePath)
                   << "] scope=" << compensation.scopeId
                   << " uncompensated="
                   << compensation.uncompensatedFrames
                   << " inserted=" << compensation.insertedFrames
                   << " compensated="
                   << compensation.compensatedFrames << " frames\n";
        }
    }
    if (summary.latency.warnings.empty())
    {
        output << "  latency-warnings: none\n";
    }
    else
    {
        output << "  latency-warnings: "
               << summary.latency.warnings.size() << "\n";
        for (const auto& warning : summary.latency.warnings)
        {
            output << "    " << warning.nodeId << " ["
                   << (warning.structuralSourcePath.empty()
                           ? warning.nodeId
                           : warning.structuralSourcePath)
                   << "] scope=" << warning.scopeId << " kind="
                   << (warning.kind
                               == runtime::LatencyPlanWarningKind::
                                   UncompensatedFeedback
                           ? "uncompensated-feedback"
                           : "uncompensated-unknown")
                   << " message=" << warning.message << "\n";
        }
    }

    if (summary.spectralNodes.empty())
    {
        output << "  spectral-nodes: none\n";
    }
    else
    {
        output << "  spectral-nodes: "
               << summary.spectralNodes.size() << "\n";
        for (const auto& spectral : summary.spectralNodes)
        {
            output << "    " << spectral.nodeId << " ["
                   << (spectral.structuralSourcePath.empty()
                           ? spectral.nodeId
                           : spectral.structuralSourcePath)
                   << "] scope=" << spectral.scopeId
                   << " type=" << spectral.typeId
                   << " format="
                   << runtime::preparedSpectralFormatName(
                          spectral.format)
                   << " role="
                   << runtime::preparedSpectralRoleName(
                          spectral.role)
                   << " N=";
            if (spectral.frameSize.has_value())
                output << *spectral.frameSize;
            else
                output << "unknown";
            output << " H=";
            if (spectral.hopSize.has_value())
                output << *spectral.hopSize;
            else
                output << "unknown";
            output << " window="
                   << statusWindowName(spectral.window)
                   << " cadence="
                   << (spectral.cadenceHz > 0.0
                           ? statusNumber(spectral.cadenceHz)
                           : std::string{"unknown"})
                   << " Hz intrinsic="
                   << statusFrames(
                          spectral.intrinsicLatencyFrames)
                   << " cumulative-in="
                   << statusFrames(
                          spectral.cumulativeInputLatencyFrames)
                   << " cumulative-out="
                   << statusFrames(
                          spectral.cumulativeOutputLatencyFrames)
                   << "\n";
            for (std::size_t index = 0;
                 index < spectral.limitationIds.size();
                 ++index)
            {
                output << "      limitation="
                       << spectral.limitationIds[index];
                if (index
                    < spectral.limitationSummaries.size())
                {
                    output << " message="
                           << spectral.limitationSummaries[index];
                }
                output << "\n";
            }
        }
    }

    if (summary.resources.empty())
    {
        output << "  resources: none\n";
    }
    else
    {
        output << "  resources: " << summary.resources.size()
               << "\n";
        for (const auto& resource : summary.resources)
        {
            output << "    " << resource.id << " kind="
                   << runtime::preparedResourceKindName(
                          resource.kind)
                   << " generation=" << resource.generation
                   << " hash="
                   << (resource.contentHash.empty()
                           ? std::string{"none"}
                           : resource.contentHash)
                   << " health="
                   << runtime::preparedResourceHealthName(
                          resource.health);
            if (resource.kind
                == talas::core::ResourceSnapshotKind::Audio)
            {
                output << " channels=" << resource.channelCount
                       << " sample-rate="
                       << statusNumber(resource.sampleRate)
                       << " frames=" << resource.frameCount
                       << " encoding=" << resource.encoding;
            }
            output << "\n";
        }
    }

    if (summary.streams.empty())
    {
        output << "  stream-contracts: none\n";
    }
    else
    {
        output << "  stream-contracts: "
               << summary.streams.size() << "\n";
        for (const auto& stream : summary.streams)
        {
            output << "    " << stream.nodeId << " ["
                   << (stream.structuralSourcePath.empty()
                           ? stream.nodeId
                           : stream.structuralSourcePath)
                   << "] scope=" << stream.scopeId
                   << " type=" << stream.typeId
                   << " resource=" << stream.resourceId
                   << " role="
                   << runtime::preparedStreamRoleName(stream.role)
                   << " live-state=not-running\n";
            for (std::size_t index = 0;
                 index < stream.limitationIds.size();
                 ++index)
            {
                output << "      limitation="
                       << stream.limitationIds[index];
                if (index < stream.limitationSummaries.size())
                {
                    output << " message="
                           << stream.limitationSummaries[index];
                }
                output << "\n";
            }
        }
    }

    if (summary.midiMappers.empty())
    {
        output << "  midi-mappers: none\n";
    }
    else
    {
        output << "  midi-mappers: "
               << summary.midiMappers.size() << "\n";
        for (const auto& mapper : summary.midiMappers)
        {
            const auto mapped = mapper.configuration.mode
                != runtime::PreparedMidiMapperMode::Passthrough;
            const auto claimed = mapped || mapper.polyphonicInput;
            output << "    " << mapper.nodeId << " ["
                   << (mapper.structuralSourcePath.empty()
                           ? mapper.nodeId
                           : mapper.structuralSourcePath)
                   << "] scope=" << mapper.scopeId
                   << " mode="
                   << (mapper.polyphonicInput
                        ? "polyphonic-input"
                        : runtime::preparedMidiMapperModeName(
                              mapper.configuration.mode))
                   << " channel=";
            if (mapper.configuration.channel == 0)
                output << "any";
            else
                output << static_cast<unsigned>(
                    mapper.configuration.channel);
            output << " pitch="
                   << static_cast<unsigned>(
                          mapper.configuration.lowPitch)
                   << "-"
                   << static_cast<unsigned>(
                          mapper.configuration.highPitch)
                   << " polyphony="
                   << static_cast<unsigned>(
                          mapper.configuration.polyphony)
                   << " input="
                   << (claimed
                           ? "scheduled-midi-only"
                           : "not-claimed")
                   << "\n";
            if (claimed)
            {
                output << "      deferred=source,midiFile,script,shared\n";
            }
        }
    }
}

int writeWavOutput(
    const RenderCommand& command,
    const runtime::OfflineRenderResult& rendered,
    std::ostream& error)
{
    std::string wavError;
    const bool ok = rendered.channels.empty()
        ? audio::writePcm16Wav(*command.outputPath, command.sampleRate, fallbackChannels(rendered), wavError)
        : audio::writePcm16Wav(*command.outputPath, command.sampleRate, rendered.channels, wavError);
    if (!ok)
    {
        error << "WAV export failed: " << wavError << '\n';
        return failureExitCode;
    }

    return successExitCode;
}

void writeCsvOutput(
    const RenderCommand& command,
    const runtime::OfflineRenderResult& rendered,
    std::ostream& output)
{
    const auto& channels = rendered.channels;
    output << std::setprecision(17);
    if (channels.size() > 2)
    {
        output << "frame";
        for (std::size_t channel = 0; channel < channels.size(); ++channel)
            output << ",ch" << (channel + 1);
        output << '\n';

        for (std::size_t frame = 0; frame < command.frameCount; ++frame)
        {
            output << frame;
            for (const auto& channel : channels)
                output << ',' << channel[frame];
            output << '\n';
        }
        return;
    }

    if (!rendered.right.empty())
    {
        output << "frame,left,right\n";
        for (std::size_t frame = 0; frame < command.frameCount; ++frame)
            output << frame << ',' << rendered.mono[frame] << ',' << rendered.right[frame] << '\n';
        return;
    }

    output << "frame,left\n";
    for (std::size_t frame = 0; frame < command.frameCount; ++frame)
        output << frame << ',' << rendered.mono[frame] << '\n';
}

void writeJsonChannel(std::ostream& output, std::string_view name, const std::vector<double>& samples, bool trailingComma)
{
    output << "    {\n";
    output << "      \"name\": \"" << name << "\",\n";
    output << "      \"samples\": [\n";
    output << std::setprecision(17);
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        output << "        " << samples[index];
        if (index + 1 < samples.size())
            output << ',';
        output << '\n';
    }
    output << "      ]\n";
    output << "    }";
    if (trailingComma)
        output << ',';
    output << '\n';
}

void writeJsonMidiEvents(std::ostream& output, const std::vector<runtime::MidiEvent>& events, bool trailingComma)
{
    output << "  \"midiEvents\": [";
    if (events.empty())
    {
        output << "]";
        if (trailingComma)
            output << ',';
        output << '\n';
        return;
    }

    output << '\n';
    for (std::size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex)
    {
        const auto& event = events[eventIndex];
        output << "    {\n";
        output << "      \"frame\": " << event.frame << ",\n";
        output << "      \"bytes\": [";
        for (std::size_t byteIndex = 0; byteIndex < event.bytes.size(); ++byteIndex)
        {
            output << static_cast<unsigned int>(event.bytes[byteIndex]);
            if (byteIndex + 1 < event.bytes.size())
                output << ", ";
        }
        output << "]\n";
        output << "    }";
        if (eventIndex + 1 < events.size())
            output << ',';
        output << '\n';
    }
    output << "  ]";
    if (trailingComma)
        output << ',';
    output << '\n';
}

void writeJsonGlobalControllerEvents(
    std::ostream& output,
    const std::vector<runtime::GlobalControllerEvent>& events)
{
    output << "  \"globalControllerEvents\": [";
    if (events.empty())
    {
        output << "]\n";
        return;
    }

    output << '\n';
    output << std::setprecision(17);
    for (std::size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex)
    {
        const auto& event = events[eventIndex];
        output << "    {\n";
        output << "      \"frame\": " << event.frame << ",\n";
        output << "      \"name\": \"" << event.name << "\",\n";
        output << "      \"value\": " << event.value << '\n';
        output << "    }";
        if (eventIndex + 1 < events.size())
            output << ',';
        output << '\n';
    }
    output << "  ]\n";
}

void writeJsonOutput(
    const RenderCommand& command,
    const runtime::OfflineRenderResult& rendered,
    std::ostream& output)
{
    output << std::setprecision(17);
    output << "{\n";
    output << "  \"sampleRate\": " << command.sampleRate << ",\n";
    output << "  \"frames\": " << command.frameCount << ",\n";
    output << "  \"channels\": [\n";

    if (rendered.channels.size() > 2)
    {
        for (std::size_t channel = 0; channel < rendered.channels.size(); ++channel)
            writeJsonChannel(output, "ch" + std::to_string(channel + 1), rendered.channels[channel], channel + 1 < rendered.channels.size());
    }
    else if (!rendered.right.empty())
    {
        writeJsonChannel(output, "left", rendered.mono, true);
        writeJsonChannel(output, "right", rendered.right, false);
    }
    else
    {
        writeJsonChannel(output, "left", rendered.mono, false);
    }

    output << "  ],\n";
    writeJsonMidiEvents(output, rendered.midiEvents, true);
    writeJsonGlobalControllerEvents(output, rendered.globalControllerEvents);
    output << "}\n";
}

void writeDotQuoted(std::ostream& output, std::string_view value)
{
    output << '"';
    for (const char character : value)
    {
        if (character == '"' || character == '\\')
        {
            output << '\\' << character;
        }
        else if (character == '\n')
        {
            output << "\\n";
        }
        else
        {
            output << character;
        }
    }
    output << '"';
}

int writeGraphDot(const GraphCommand& command, std::ostream& output, std::ostream& error)
{
    auto loaded =
        loadCliProjectInput(command.projectPath, error);
    if (!loaded.has_value())
        return failureExitCode;

    output << "digraph Talas {\n";
    for (const auto& node : loaded->document.graph.nodes)
    {
        output << "  ";
        writeDotQuoted(output, node.instanceId);
        output << " [label=";
        const auto typeLabel = node.soundDefinitionId.has_value()
            ? "Sound: " + *node.soundDefinitionId
            : node.typeId;
        writeDotQuoted(
            output,
            node.instanceId + "\n" + typeLabel);
        output << "];\n";
    }
    for (const auto& connection : loaded->document.graph.connections)
    {
        output << "  ";
        writeDotQuoted(output, connection.fromNode);
        output << " -> ";
        writeDotQuoted(output, connection.toNode);
        output << " [label=";
        writeDotQuoted(output, connection.fromPort + " -> " + connection.toPort);
        output << "];\n";
    }
    output << "}\n";
    return successExitCode;
}

project::ProjectDocument makeOscillatorStarterProject()
{
    project::ProjectDocument document;
    document.metadata.title = "Oscillator Starter";
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            {"frequency", "1 hz"},
            {"amplitude", "1.0"},
        },
    });
    document.graph.connections.push_back({"osc", "audioOut", "out", "audioIn"});
    return document;
}

int initializeProject(const InitCommand& command, std::ostream& output, std::ostream& error)
{
    if (std::filesystem::exists(command.outputPath))
    {
        error << "Output file already exists: "
              << project::pathToUtf8(command.outputPath) << '\n';
        return failureExitCode;
    }

    std::ofstream file(command.outputPath);
    if (!file)
    {
        error << "Failed to create project file: "
              << project::pathToUtf8(command.outputPath) << '\n';
        return failureExitCode;
    }

    file << project::serializeProjectDocument(makeOscillatorStarterProject()) << '\n';
    file.close();
    if (!file.good())
    {
        error << "Failed to write project file: "
              << project::pathToUtf8(command.outputPath) << '\n';
        return failureExitCode;
    }

    output << "Created " << project::pathToUtf8(command.outputPath) << '\n';
    return successExitCode;
}

std::string diskCacheFileToken(std::string_view value)
{
    std::string result;
    result.reserve(std::min<std::size_t>(value.size(), 80));
    for (const auto character : value)
    {
        if (result.size() == 80) break;
        const auto accepted = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '_';
        result.push_back(accepted ? character : '_');
    }
    return result.empty() ? std::string{"cache"} : result;
}

int runDiskCacheCommand(
    const DiskCacheCommand& command,
    std::ostream& output,
    std::ostream& error)
{
    auto input = loadCliProjectInput(command.projectPath, error);
    if (!input.has_value()) return failureExitCode;
    const auto registry = core::makeBuiltinNodeRegistry();
    const auto range = project::diskCacheCaptureRange(
        input->document.timeline.loop, command.sampleRate);
    std::string fingerprintError;
    const auto fingerprint = project::projectDiskCacheSourceFingerprint(
        input->document,
        registry,
        command.sourcePath,
        static_cast<std::uint32_t>(command.sampleRate),
        fingerprintError);
    if (!range.ok() || fingerprint.empty())
    {
        error << "DiskCache source is not capturable";
        if (!fingerprintError.empty())
            error << ": " << fingerprintError;
        error << '\n';
        return failureExitCode;
    }
    output << "DiskCache source=" << command.sourcePath
           << " sample-rate=" << command.sampleRate
           << " start-frame=" << range.range->startFrame
           << " frames=" << range.range->frameCount()
           << " fingerprint=" << fingerprint << '\n';
    if (!command.capture) return successExitCode;

    const auto revision = project::captureProjectFileRevision(
        input->projectFilePath);
    if (!revision.ok())
    {
        error << "Could not capture project-file revision authority\n";
        return failureExitCode;
    }
    const auto generationAuthority =
        project::nextDiskCacheCaptureAssetGeneration(
            input->document, command.assetId);
    if (!generationAuthority.ok())
    {
        error << "DiskCache asset authority is unrelated or exhausted\n";
        return failureExitCode;
    }
    const auto generation = *generationAuthority.generation;
    audio::DiskCacheCaptureOptions options;
    options.projectFilePath = input->projectFilePath;
    options.sourcePath = command.sourcePath;
    options.assetId = command.assetId;
    const auto captureStamp = std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count();
    options.relativeAssetPath = "DiskCache/"
        + diskCacheFileToken(command.assetId)
        + "-g" + std::to_string(generation)
        + "-" + std::to_string(captureStamp)
        + "-" + std::to_string(
            diskCacheCaptureSequence.fetch_add(
                1U, std::memory_order_relaxed))
        + ".wav";
    options.assetGeneration = generation;
    options.expectedDocumentAuthorityGeneration = 1;
    options.expectedCanonicalDocumentBytes =
        project::serializeProjectDocument(input->document);
    options.expectedDeviceGeneration = 1;
    options.sampleRate = command.sampleRate;
    options.maximumBlockFrames = command.maxBlockFrames;

    auto preparedCapture = audio::prepareProjectDiskCacheCapture(
        input->document, registry, std::move(options));
    if (!preparedCapture.ok())
    {
        error << "DiskCache capture failed: "
              << preparedCapture.error << '\n';
        return failureExitCode;
    }
    project::ProjectEditor editor{input->document, registry};
    project::PreparedDiskCacheCaptureEdit edit;
    const auto preparedEdit = editor.prepareDiskCacheCapture(
        preparedCapture.prepared->takeEditorPublication(), edit);
    if (!preparedEdit.ok()
        || !editor.diskCacheCaptureStillCurrent(edit))
    {
        error << "DiskCache project publication rejected: "
              << preparedEdit.message << '\n';
        return failureExitCode;
    }
    const auto terminalRevision = project::captureProjectFileRevision(
        input->projectFilePath);
    if (!terminalRevision.ok()
        || *terminalRevision.revision != *revision.revision)
    {
        error << "DiskCache project file changed before publication\n";
        return failureExitCode;
    }
    std::string publishError;
    if (!preparedCapture.prepared->publishFile(publishError))
    {
        error << "DiskCache file publication failed: "
              << publishError << '\n';
        return failureExitCode;
    }
    if (!editor.commitPreparedDiskCacheCapture(std::move(edit)))
    {
        std::string rollbackError;
        const auto rolledBack =
            preparedCapture.prepared->retractPublishedFile(rollbackError);
        error << "DiskCache project authority changed before commit";
        if (!rolledBack)
            error << "; rollback failed: " << rollbackError;
        error << '\n';
        return failureExitCode;
    }
    const auto saved = project::saveProjectFile(
        input->projectFilePath, editor.document(), revision.revision);
    if (!saved.ok())
    {
        std::string rollbackError;
        const auto rolledBack =
            preparedCapture.prepared->retractPublishedFile(rollbackError);
        error << "DiskCache project save failed: "
              << saved.error->message;
        if (!rolledBack)
            error << "; WAV rollback failed: " << rollbackError;
        error << '\n';
        return failureExitCode;
    }
    output << "Captured DiskCache asset=" << command.assetId
           << " generation=" << generation
           << " file="
           << project::pathToUtf8(
                  preparedCapture.prepared->destination())
           << '\n';
    return successExitCode;
}

int renderProject(const RenderCommand& command, std::ostream& output, std::ostream& error)
{
    auto loaded =
        loadCliProjectInput(command.projectPath, error);
    if (!loaded.has_value())
        return failureExitCode;

    auto registry = core::makeBuiltinNodeRegistry();
    if (rejectUnavailableProjectComponents(
            loaded->document,
            registry,
            nullptr,
            error))
    {
        return failureExitCode;
    }
    runtime::RenderSettings settings;
    settings.sampleRate = command.sampleRate;
    settings.maxBlockFrames = command.maxBlockFrames;
    settings.eventValues = command.eventValues;
    settings.eventValueChanges = command.eventValueChanges;
    if (!loadPersistedProjectAssets(
            loaded->document,
            registry,
            loaded->projectFilePath,
            command.tableResources,
            command.binaryResources,
            settings,
            error))
    {
        return failureExitCode;
    }
    if (!loadTableResources(command.tableResources, settings, error))
        return failureExitCode;
    if (!loadBinaryResources(command.binaryResources, settings, error))
        return failureExitCode;
    if (!loadAudioInputs(command.audioInputs, settings, error))
        return failureExitCode;

    auto preparedSettings = runtime::prepareProjectRenderSettings(
        loaded->document,
        registry,
        std::move(settings));
    if (!preparedSettings.ok())
    {
        error << "Render settings failed: "
              << preparedSettings.error << '\n';
        return failureExitCode;
    }
    settings = std::move(*preparedSettings.settings);

    auto compiled = runtime::compileProject(
        loaded->document,
        registry,
        settings);
    if (!compiled.ok())
    {
        printRuntimeDiagnostics(error, "Compile failed", compiled.diagnostics);
        return failureExitCode;
    }

    auto rendered = runtime::renderOffline(*compiled.graph, command.frameCount);
    if (!rendered.ok())
    {
        printRuntimeDiagnostics(error, "Render failed", rendered.diagnostics);
        return failureExitCode;
    }

    if (command.outputPath.has_value())
        return writeWavOutput(command, rendered, error);

    if (command.format == RenderOutputFormat::Json)
        writeJsonOutput(command, rendered, output);
    else
        writeCsvOutput(command, rendered, output);
    return successExitCode;
}

int validateProject(const ValidateCommand& command, std::ostream& output, std::ostream& error)
{
    auto loaded =
        loadCliProjectInput(command.projectPath, error);
    if (!loaded.has_value())
        return failureExitCode;

    auto registry = core::makeBuiltinNodeRegistry();
    if (rejectUnavailableProjectComponents(
            loaded->document,
            registry,
            &output,
            error))
    {
        return failureExitCode;
    }
    runtime::RenderSettings settings;
    settings.sampleRate = command.sampleRate;
    settings.maxBlockFrames = command.maxBlockFrames;
    settings.eventValues = command.eventValues;
    settings.eventValueChanges = command.eventValueChanges;
    if (!loadPersistedProjectAssets(
            loaded->document,
            registry,
            loaded->projectFilePath,
            command.tableResources,
            command.binaryResources,
            settings,
            error))
    {
        return failureExitCode;
    }
    if (!loadTableResources(command.tableResources, settings, error))
        return failureExitCode;
    if (!loadBinaryResources(command.binaryResources, settings, error))
        return failureExitCode;
    if (!loadAudioInputs(command.audioInputs, settings, error))
        return failureExitCode;

    auto preparedSettings = runtime::prepareProjectRenderSettings(
        loaded->document,
        registry,
        std::move(settings));
    if (!preparedSettings.ok())
    {
        error << "Render settings failed: "
              << preparedSettings.error << '\n';
        return failureExitCode;
    }
    settings = std::move(*preparedSettings.settings);

    auto compiled = runtime::compileProject(
        loaded->document,
        registry,
        settings);
    if (!compiled.ok())
    {
        printRuntimeDiagnostics(error, "Compile failed", compiled.diagnostics);
        return failureExitCode;
    }

    auto rendered = runtime::validateOfflineRuntimeGraph(
        *compiled.graph);
    if (!rendered.ok())
    {
        printRuntimeDiagnostics(error, "Render failed", rendered.diagnostics);
        return failureExitCode;
    }

    std::optional<runtime::PreparedGraphSummary> statusSummary;
    if (command.status)
    {
        auto summary =
            runtime::makePreparedGraphSummary(
                *compiled.graph);
        if (!summary.ok())
        {
            error << "Prepared graph status failed: "
                  << summary.error << '\n';
            return failureExitCode;
        }
        statusSummary = std::move(*summary.summary);
    }
    output << "OK\n";
    if (statusSummary.has_value())
        writePreparedGraphStatus(*statusSummary, output);
    return successExitCode;
}

int runCatalog(std::span<const std::string_view> args, std::ostream& output, std::ostream& error)
{
    std::string_view format = "json";
    for (std::size_t index = 1; index < args.size(); ++index)
    {
        const auto option = args[index];
        if (option == "--format")
        {
            const auto value = optionValue(args, index, option, error);
            if (!value.has_value())
                return usageExitCode;
            format = *value;
        }
        else
        {
            error << "Unknown catalog option: " << option << '\n';
            return usageExitCode;
        }
    }

    if (format != "json")
    {
        error << "Unsupported catalog format: " << format << '\n';
        return usageExitCode;
    }

    output << core::serializeNodeCatalog(core::makeBuiltinNodeRegistry());
    return successExitCode;
}
}

namespace
{
std::optional<GaAnalysisCommand> parseGaAnalysisCommand(
    std::span<const std::string_view> args,
    std::ostream& error)
{
    GaAnalysisCommand command;
    for (std::size_t index = 1; index < args.size(); ++index)
    {
        const auto option = args[index];
        const auto value = optionValue(args, index, option, error);
        if (!value.has_value())
            return std::nullopt;
        if (option == "--input")
            command.inputPath = std::string{*value};
        else if (option == "--output")
            command.outputPath = std::string{*value};
        else if (option == "--waveforms" || option == "--harmonics")
        {
            std::size_t parsed = 0;
            const auto most = option == "--waveforms" ? std::size_t{64} : std::size_t{1024};
            if (!parsePositiveSize(*value, parsed) || parsed > most)
            {
                error << option << " must be a whole number from 1 to " << most << '\n';
                return std::nullopt;
            }
            (option == "--waveforms" ? command.settings.waveforms : command.settings.harmonics) = parsed;
        }
        else if (option == "--frames-per-second")
        {
            if (!parsePositiveDouble(*value, command.settings.framesPerSecond)
                || command.settings.framesPerSecond > 1000.0 || command.settings.framesPerSecond < 1.0)
            {
                error << "--frames-per-second must be from 1 to 1000\n";
                return std::nullopt;
            }
        }
        else
        {
            error << "Unknown GA analysis option: " << option << '\n';
            return std::nullopt;
        }
    }
    if (command.inputPath.empty() || command.outputPath.empty())
    {
        error << "ga-analysis requires --input and --output\n";
        return std::nullopt;
    }
    return command;
}

int runGaAnalysis(const GaAnalysisCommand& command, std::ostream& output, std::ostream& error)
{
    const auto sample = audio::readSampleFile(command.inputPath);
    if (!sample.ok() || sample.channels.empty() || sample.channels.front().empty())
    {
        error << "GA analysis could not read " << command.inputPath.string() << ": "
              << (sample.ok() ? std::string{"the sample has no frames"} : sample.error) << '\n';
        return failureExitCode;
    }
    const auto result = audio::writeGaAnalysisFile(
        sample.channels, static_cast<double>(sample.sampleRate), command.settings, command.outputPath);
    if (!result.ok())
    {
        error << result.error << '\n';
        return failureExitCode;
    }
    output << "GA analysis: " << command.settings.waveforms << " waveforms of " << result.harmonics
           << " harmonics, " << result.envelopeLength << " envelope frames, base pitch " << result.basePitch
           << " Hz, written to " << command.outputPath.string() << '\n';
    return successExitCode;
}

std::optional<CdpProcessCommand> parseCdpProcessCommand(
    std::span<const std::string_view> args,
    std::ostream& error)
{
    CdpProcessCommand command;
    for (std::size_t index = 1; index < args.size(); ++index)
    {
        const auto option = args[index];
        if (option == "--list")
        {
            command.list = true;
            continue;
        }
        const auto value = optionValue(args, index, option, error);
        if (!value.has_value())
            return std::nullopt;
        if (option == "--process")
            command.request.process = std::string{*value};
        else if (option == "--input")
            command.inputs.emplace_back(std::string{*value});
        else if (option == "--output")
            command.output = std::string{*value};
        else if (option == "--points" || option == "--overlap")
        {
            std::size_t parsed = 0;
            if (!parsePositiveSize(*value, parsed) || parsed > 8192)
            {
                error << option << " must be a whole number (points 2 to 8192, overlap 1 to 4)\n";
                return std::nullopt;
            }
            (option == "--points" ? command.request.points : command.request.overlap) = static_cast<int>(parsed);
        }
        else if (option == "--set")
        {
            const auto equals = value->find('=');
            if (equals == std::string_view::npos || equals == 0)
            {
                error << "--set takes <field>=<value>\n";
                return std::nullopt;
            }
            // A breakpoint table or data file's lines may be written with
            // '\n' or ';'.
            std::string text{value->substr(equals + 1)};
            for (std::size_t at = text.find("\\n"); at != std::string::npos; at = text.find("\\n", at))
                text.replace(at, 2, "\n");
            command.request.fields[std::string{value->substr(0, equals)}] = std::move(text);
        }
        else
        {
            error << "Unknown cdp-process option: " << option << '\n';
            return std::nullopt;
        }
    }
    if (!command.list && (command.request.process.empty() || command.inputs.empty() || command.output.empty()))
    {
        error << "cdp-process requires --process, --input and --output (or --list)\n";
        return std::nullopt;
    }
    return command;
}

int runCdpProcess(const CdpProcessCommand& command, std::ostream& output, std::ostream& error)
{
    if (command.list)
    {
        for (const auto& process : talas::cdp::offlineSpectralProcesses())
        {
            output << process.id << " (`" << process.cdpCommand << "`, " << process.label << "): "
                   << process.minimumInputs;
            if (process.maximumInputs != process.minimumInputs)
                output << " to " << process.maximumInputs;
            output << (process.maximumInputs == 1 ? " input\n" : " inputs\n");
            for (const auto& field : process.fields)
            {
                output << "  " << field.id << " = " << (field.defaultValue.empty() ? "(CDP's default)" : field.defaultValue);
                if (!field.choices.empty())
                {
                    output << " [";
                    for (std::size_t choice = 0; choice < field.choices.size(); ++choice)
                        output << (choice == 0 ? "" : " ") << field.choices[choice];
                    output << ']';
                }
                output << ": " << field.help << '\n';
            }
        }
        return successExitCode;
    }
    if (talas::cdp::findOfflineSpectralProcess(command.request.process) == nullptr)
    {
        error << "Unknown CDP process: " << command.request.process << " (see cdp-process --list)\n";
        return usageExitCode;
    }
    const auto result = audio::processSamplesThroughCdp(command.request, command.inputs, command.output);
    if (!result.ok())
    {
        error << result.error << '\n';
        return failureExitCode;
    }
    output << "CDP " << command.request.process << ": " << result.frames << " frames at " << result.sampleRate
           << " Hz, written to " << command.output.string() << '\n';
    return successExitCode;
}

int runReAnalysis(const ReAnalysisCommand& command, std::ostream& output, std::ostream& error)
{
    const auto sample = audio::readPcm16Wav(command.inputPath);
    if (!sample.ok() || sample.channels.empty())
    {
        error << "RE analysis could not read " << command.inputPath.string() << ": "
              << (sample.ok() ? std::string{"the sample has no channels"} : sample.error) << '\n';
        return failureExitCode;
    }
    const auto result = audio::writeReAnalysisFiles(
        sample.channels,
        static_cast<double>(sample.sampleRate),
        command.settings,
        command.outputPath,
        command.excitationPath);
    if (!result.ok())
    {
        error << result.error << '\n';
        return failureExitCode;
    }
    output << "RE table: order " << result.order << ", " << result.coefficientSets
           << " coefficient sets, written to " << command.outputPath.string() << '\n';
    if (!result.excitationWritten)
        return successExitCode;
    output << "EX file written to " << command.excitationPath.string();
    if (result.excitationScale != 1.0)
        output << ", scaled by " << result.excitationScale << " to fit";
    output << '\n';
    return successExitCode;
}

int runCliCommand(std::span<const std::string_view> args, std::ostream& output, std::ostream& error)
{
    if (args.empty() || args[0] == "--help" || args[0] == "-h")
    {
        printUsage(output);
        return args.empty() ? usageExitCode : successExitCode;
    }

    if (args[0] == "catalog")
        return runCatalog(args, output, error);

    if (args[0] == "graph")
    {
        auto command = parseGraphCommand(args, error);
        if (!command.has_value())
            return usageExitCode;
        return writeGraphDot(*command, output, error);
    }

    if (args[0] == "init")
    {
        auto command = parseInitCommand(args, error);
        if (!command.has_value())
            return usageExitCode;
        return initializeProject(*command, output, error);
    }

    if (args[0] == "validate")
    {
        auto command = parseValidateCommand(args, error);
        if (!command.has_value())
            return usageExitCode;
        return validateProject(*command, output, error);
    }

    if (args[0] == "inspect-disk-cache"
        || args[0] == "capture-disk-cache")
    {
        const auto capture = args[0] == "capture-disk-cache";
        auto command = parseDiskCacheCommand(args, capture, error);
        if (!command.has_value()) return usageExitCode;
        return runDiskCacheCommand(*command, output, error);
    }

    if (args[0] == "ga-analysis")
    {
        auto command = parseGaAnalysisCommand(args, error);
        if (!command.has_value())
            return usageExitCode;
        return runGaAnalysis(*command, output, error);
    }

    if (args[0] == "cdp-process")
    {
        auto command = parseCdpProcessCommand(args, error);
        if (!command.has_value())
            return usageExitCode;
        return runCdpProcess(*command, output, error);
    }

    if (args[0] == "export-plugin")
        return runExportPluginCommand(args, output, error);

    if (args[0] == "re-analysis")
    {
        auto command = parseReAnalysisCommand(args, error);
        if (!command.has_value())
            return usageExitCode;
        return runReAnalysis(*command, output, error);
    }

    if (args[0] != "render")
    {
        error << "Unknown command: " << args[0] << '\n';
        printUsage(error);
        return usageExitCode;
    }

    auto command = parseRenderCommand(args, error);
    if (!command.has_value())
        return usageExitCode;

    return renderProject(*command, output, error);
}
}

int runCli(std::span<const std::string_view> args, std::ostream& output, std::ostream& error)
{
    int status = failureExitCode;
    try
    {
        status = runCliCommand(args, output, error);
    }
    catch (const std::exception& exception)
    {
        error << "talas_cli: " << exception.what() << '\n';
        return failureExitCode;
    }
    // A closed pipe or full disk means the command's result never arrived.
    output.flush();
    if (status == successExitCode && !output.good())
    {
        error << "talas_cli: failed to write output\n";
        return failureExitCode;
    }
    return status;
}
}
