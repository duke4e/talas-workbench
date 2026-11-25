#include "talas/audio/TimelineRecording.h"

#include "talas/audio/AudioFilePublication.h"
#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/WavReader.h"
#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectPackage.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/runtime/ProjectRenderSettings.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <utility>

namespace talas::audio
{
namespace
{
[[nodiscard]] bool put(std::FILE* output, const void* bytes, std::size_t count)
{
    return std::fwrite(bytes, 1, count, output) == count;
}

[[nodiscard]] bool putBigEndian(std::FILE* output, std::uint64_t value, std::size_t bytes)
{
    std::array<unsigned char, 8> encoded{};
    for (std::size_t index = 0; index < bytes; ++index)
        encoded[index] = static_cast<unsigned char>((value >> (8 * (bytes - 1 - index))) & 0xffU);
    return put(output, encoded.data(), bytes);
}

// The sample rate as the 80-bit extended float an AIFF COMM chunk holds.
[[nodiscard]] bool putExtended(std::FILE* output, double value)
{
    int exponent = 0;
    const auto fraction = std::frexp(value, &exponent);
    const auto biased = static_cast<std::uint64_t>(exponent + 16382);
    const auto mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 64));
    return putBigEndian(output, biased & 0x7fffU, 2) && putBigEndian(output, mantissa, 8);
}

// One sample in a word of `bits`, as the WAV writer codes it, but signed
// at every size and big-endian.
[[nodiscard]] bool putAiffSample(std::FILE* output, double sample, std::uint16_t bits)
{
    const auto clipped = std::clamp(sample, -1.0, 1.0);
    const auto half = std::int64_t{1} << (bits - 1);
    const auto code = clipped <= -1.0 ? -half : static_cast<std::int64_t>(std::llround(clipped * static_cast<double>(half - 1)));
    return putBigEndian(output, static_cast<std::uint64_t>(code), bits / 8);
}

// A PCM AIFF file whose length is known before its samples are written.
class AiffStreamWriter
{
public:
    AiffStreamWriter(std::FILE* output, double sampleRate, std::size_t channels, std::size_t frames, std::uint16_t bits)
        : output_(output), bits_(bits)
    {
        const auto dataBytes = static_cast<std::uint64_t>(frames) * channels * (bits / 8U);
        pad_ = (dataBytes % 2U) != 0U;
        const auto formBytes = 4U + (8U + 18U) + (8U + 8U + dataBytes + (pad_ ? 1U : 0U));
        if (formBytes > std::numeric_limits<std::uint32_t>::max())
        {
            error_ = "The recording is too long for an AIFF file";
            return;
        }
        ready_ = put(output_, "FORM", 4) && putBigEndian(output_, formBytes, 4) && put(output_, "AIFF", 4)
            && put(output_, "COMM", 4) && putBigEndian(output_, 18, 4) && putBigEndian(output_, channels, 2)
            && putBigEndian(output_, frames, 4) && putBigEndian(output_, bits, 2) && putExtended(output_, sampleRate)
            && put(output_, "SSND", 4) && putBigEndian(output_, 8U + dataBytes, 4) && putBigEndian(output_, 0, 4)
            && putBigEndian(output_, 0, 4);
        if (!ready_)
            error_ = "Could not write the AIFF header";
    }
    ~AiffStreamWriter()
    {
        if (output_ != nullptr)
            std::fclose(output_);
    }
    AiffStreamWriter(const AiffStreamWriter&) = delete;
    AiffStreamWriter& operator=(const AiffStreamWriter&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] bool write(const std::vector<const std::vector<double>*>& channels)
    {
        const auto frames = channels.empty() ? 0U : channels.front()->size();
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            for (const auto* channel : channels)
            {
                if (!putAiffSample(output_, (*channel)[frame], bits_))
                {
                    error_ = "Could not write the AIFF samples";
                    return false;
                }
            }
        }
        return true;
    }
    [[nodiscard]] bool finish()
    {
        const auto padded = !pad_ || put(output_, "", 1);
        const auto closed = std::fclose(output_) == 0;
        output_ = nullptr;
        if (!padded || !closed)
            error_ = "Could not finish the AIFF file";
        return padded && closed;
    }

private:
    std::FILE* output_ = nullptr;
    std::uint16_t bits_ = 16;
    bool pad_ = false;
    bool ready_ = false;
    std::string error_;
};

// One file of the recording: its channels of the Timeline's output and
// where it goes.
struct RecordingFile
{
    std::vector<std::size_t> channels;
    std::unique_ptr<AudioFilePublication> publication;
    std::unique_ptr<Pcm16WavStreamWriter> wav;
    std::unique_ptr<AiffStreamWriter> aiff;
};

[[nodiscard]] std::filesystem::path channelPath(const std::filesystem::path& destination, std::uint32_t channel)
{
    auto path = destination;
    path.replace_filename(destination.stem().string() + " " + std::to_string(channel) + destination.extension().string());
    return path;
}
}

std::vector<std::filesystem::path> timelineRecordingFiles(const TimelineRecordingOptions& options)
{
    if (options.interleavedStereo && options.channels.size() == 2)
        return {options.destination};
    std::vector<std::filesystem::path> files;
    for (const auto channel : options.channels)
        files.push_back(channelPath(options.destination, channel));
    return files;
}

std::uint32_t timelineRecordingChannelCount(const talas::project::ProjectDocument& document)
{
    const auto& tracks = document.timeline.score.tracks;
    const auto master = std::ranges::find(tracks, talas::project::ScoreTrackKind::Master, &talas::project::ScoreTrack::kind);
    return master == tracks.end() ? 0U : master->channelCount;
}

std::string writeAiffFile(
    const std::filesystem::path& destination,
    double sampleRate,
    const std::vector<std::vector<double>>& channels,
    std::uint16_t bits)
{
    if (bits != 8 && bits != 16 && bits != 24)
        return "The bits per sample must be 8, 16 or 24";
    if (channels.empty() || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return "An AIFF file needs channels and a sample rate";
    const auto frames = channels.front().size();
    if (std::ranges::any_of(channels, [frames](const auto& channel) { return channel.size() != frames; }))
        return "The channels differ in length";
    std::string error;
    auto publication = AudioFilePublication::prepare(destination, ExistingAudioFilePolicy::Replace, error);
    if (!publication)
        return error;
    auto* output = publication->openTemporaryFile(error);
    if (output == nullptr)
        return error;
    AiffStreamWriter aiff{output, sampleRate, channels.size(), frames, bits};
    std::vector<const std::vector<double>*> pointers;
    for (const auto& channel : channels)
        pointers.push_back(&channel);
    if (!aiff.ready() || !aiff.write(pointers) || !aiff.finish())
    {
        error = aiff.error();
        std::string ignored;
        (void) publication->discard(ignored);
        return error;
    }
    if (!publication->publish(error))
        return error;
    return {};
}

std::string convertRecordingToAiff(const std::filesystem::path& wav, const std::filesystem::path& aiff, std::uint16_t bits)
{
    return convertRecordingToSampleFile(wav, aiff, SampleFileFormat::Aiff, bits);
}

std::string writeSampleFile(
    const std::filesystem::path& destination,
    double sampleRate,
    const std::vector<std::vector<double>>& channels,
    std::uint16_t bits,
    SampleFileFormat format)
{
    if (auto refusal = sampleFileFormatRefusal(format, channels.size(), bits); !refusal.empty())
        return refusal;
    switch (format)
    {
    case SampleFileFormat::Wave:
    {
        std::string error;
        return writePcmWav(destination, sampleRate, channels, WavWordSize{bits}, error) ? std::string{} : error;
    }
    case SampleFileFormat::Aiff: return writeAiffFile(destination, sampleRate, channels, bits);
    case SampleFileFormat::Ircam: return writeIrcamFile(destination, sampleRate, channels, bits);
    case SampleFileFormat::SoundDesignerI: return writeSoundDesignerIFile(destination, sampleRate, channels, bits);
    case SampleFileFormat::SoundDesignerII: return writeSoundDesignerIIFile(destination, sampleRate, channels, bits);
    }
    return "Unknown sample file format";
}

std::string convertRecordingToSampleFile(
    const std::filesystem::path& wav,
    const std::filesystem::path& destination,
    SampleFileFormat format,
    std::uint16_t bits)
{
    const auto recording = readSampleFile(wav);
    if (!recording.ok())
        return "The recording could not be read: " + recording.error;
    if (wav.lexically_normal() == destination.lexically_normal())
        return "The recording and the file it is saved as must differ";
    if (auto error = writeSampleFile(destination, recording.sampleRate, recording.channels, bits, format); !error.empty())
        return error;
    std::error_code removeError;
    std::filesystem::remove(wav, removeError);
    return {};
}

TimelineRecordingResult recordTimelineToDisk(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    TimelineRecordingOptions options)
{
    TimelineRecordingResult result;
    const auto outputs = timelineRecordingChannelCount(document);
    auto chosen = options.channels;
    std::ranges::sort(chosen);
    if (options.destination.empty() || options.channels.empty() || std::ranges::adjacent_find(chosen) != chosen.end()
        || std::ranges::any_of(options.channels, [&](std::uint32_t channel) { return channel == 0 || channel > outputs; }))
    {
        result.error = "Choose some of the Timeline's output channels and where to record them";
        return result;
    }
    if (options.bits != 8 && options.bits != 16 && options.bits != 24)
    {
        result.error = "A recording is 8, 16 or 24 bits";
        return result;
    }
    if (!(options.sampleRate > 0.0) || !std::isfinite(options.sampleRate) || options.maxBlockFrames == 0
        || options.sampleRate > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
    {
        result.error = "The recording needs a sample rate and a block size";
        return result;
    }
    const auto cancelled = [&] { return options.shouldCancel && options.shouldCancel(); };

    // From the start of the Timeline to the end of its last Sound.
    const auto rate = static_cast<std::uint32_t>(std::llround(options.sampleRate));
    const auto prepared = talas::runtime::prepareProjectScore(
        document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, rate, 1));
    if (!prepared.ok())
    {
        result.error = prepared.error;
        return result;
    }
    std::uint64_t end = 0;
    for (const auto& iteration : prepared.schedule->clipIterations())
        end = std::max(end, iteration.endFrame);
    if (end == 0)
    {
        result.error = "The Timeline has no Sounds to record";
        return result;
    }
    result.frameCount = static_cast<std::size_t>(end);

    // The recording never replaces the project, its package or an asset.
    std::vector<std::filesystem::path> protectedFiles;
    std::vector<std::filesystem::path> protectedDirectories;
    if (options.projectFilePath.has_value())
    {
        protectedFiles.push_back(*options.projectFilePath);
        const auto package = talas::project::projectPackageDisplayPath(*options.projectFilePath);
        if (package != *options.projectFilePath)
            protectedDirectories.push_back(package);
    }
    for (const auto& asset : document.assets)
    {
        const auto resolved = talas::project::resolveProjectAssetPath(asset, options.projectFilePath);
        if (!resolved.empty())
            protectedFiles.push_back(resolved);
    }
    const auto paths = timelineRecordingFiles(options);
    for (const auto& path : paths)
    {
        if (audioFileDestinationConflicts(path, protectedFiles, protectedDirectories))
        {
            result.error = "A recording cannot replace the project, its package or one of its assets";
            return result;
        }
    }

    talas::runtime::RenderSettings settings;
    settings.sampleRate = options.sampleRate;
    settings.maxBlockFrames = options.maxBlockFrames;
    settings.scorePlaybackEndFrameExclusive = end;
    settings.preparedScore = prepared.schedule;
    auto loaded = loadReferencedProjectResourceSnapshots(document, registry, options.projectFilePath, {}, options.shouldCancel);
    if (!loaded.ok())
    {
        result.cancelled = loaded.wasCancelled;
        result.error = loaded.failure.has_value() ? describeProjectAssetLoadFailure(*loaded.failure)
                                                  : std::string{"The Timeline's samples could not be loaded"};
        return result;
    }
    settings.resourceSnapshots.insert(settings.resourceSnapshots.end(), std::make_move_iterator(loaded.snapshots.begin()),
        std::make_move_iterator(loaded.snapshots.end()));
    auto configured = talas::runtime::prepareProjectRenderSettings(document, registry, std::move(settings));
    if (!configured.ok())
    {
        result.error = configured.error;
        return result;
    }
    auto compiled = talas::runtime::compileProject(document, registry, std::move(*configured.settings));
    if (!compiled.ok())
    {
        result.error = compiled.diagnostics.empty() ? "The Timeline could not be compiled" : compiled.diagnostics.front().message;
        return result;
    }

    std::vector<RecordingFile> files;
    const auto stereo = paths.size() == 1 && options.channels.size() == 2;
    for (std::size_t index = 0; index < paths.size(); ++index)
    {
        RecordingFile file;
        if (stereo)
            file.channels = {options.channels[0] - 1U, options.channels[1] - 1U};
        else
            file.channels = {options.channels[index] - 1U};
        file.publication = AudioFilePublication::prepare(paths[index], ExistingAudioFilePolicy::Replace, result.error);
        if (!file.publication)
            return result;
        auto* output = file.publication->openTemporaryFile(result.error);
        if (output == nullptr)
            return result;
        if (options.format == TimelineRecordingFormat::Wav)
        {
            file.wav = std::make_unique<Pcm16WavStreamWriter>(output, paths[index].string(), options.sampleRate,
                file.channels.size(), WavWordSize{options.bits});
            if (!file.wav->ready())
            {
                result.error = std::string(file.wav->error());
                return result;
            }
        }
        else
        {
            file.aiff = std::make_unique<AiffStreamWriter>(output, options.sampleRate, file.channels.size(),
                result.frameCount, options.bits);
            if (!file.aiff->ready())
            {
                result.error = file.aiff->error();
                return result;
            }
        }
        files.push_back(std::move(file));
    }

    {
        talas::runtime::OfflineRenderSession session{*compiled.graph};
        for (std::size_t rendered = 0; rendered < result.frameCount;)
        {
            if (cancelled())
            {
                result.cancelled = true;
                result.error = "The recording was cancelled";
                break;
            }
            const auto frames = std::min(options.maxBlockFrames, result.frameCount - rendered);
            auto block = session.render(frames);
            if (!block.ok() || block.channels.size() < outputs)
            {
                result.error = block.diagnostics.empty() ? "The Timeline did not render its output channels"
                                                         : block.diagnostics.front().message;
                break;
            }
            for (auto& file : files)
            {
                std::vector<std::vector<double>> wavChannels;
                std::vector<const std::vector<double>*> aiffChannels;
                for (const auto channel : file.channels)
                {
                    if (file.wav)
                        wavChannels.push_back(block.channels[channel]);
                    else
                        aiffChannels.push_back(&block.channels[channel]);
                }
                const auto written = file.wav ? file.wav->write(wavChannels) : file.aiff->write(aiffChannels);
                if (!written)
                {
                    result.error = file.wav ? std::string(file.wav->error()) : file.aiff->error();
                    break;
                }
            }
            if (!result.error.empty())
                break;
            rendered += frames;
            if (options.progress)
                options.progress(static_cast<double>(rendered) / static_cast<double>(result.frameCount));
        }
    }
    for (auto& file : files)
    {
        const auto finished = file.wav ? file.wav->finish() : file.aiff->finish();
        if (!finished && result.error.empty())
            result.error = file.wav ? std::string(file.wav->error()) : file.aiff->error();
    }
    if (!result.error.empty())
    {
        for (auto& file : files)
        {
            std::string ignored;
            (void) file.publication->discard(ignored);
        }
        return result;
    }
    for (auto& file : files)
    {
        if (!file.publication->publish(result.error))
            return result;
        result.files.push_back(file.publication->destination());
    }
    return result;
}
}
