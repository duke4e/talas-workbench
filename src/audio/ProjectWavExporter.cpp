#include "talas/audio/ProjectWavExporter.h"

#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/AudioFilePublication.h"
#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/runtime/ProjectRenderSettings.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace talas::audio
{
namespace
{
[[nodiscard]] bool cancellationRequested(const ProjectWavExportOptions& options)
{
    return options.shouldCancel && options.shouldCancel();
}

[[nodiscard]] std::vector<std::vector<double>> takeAudioChannels(
    talas::runtime::OfflineRenderResult& rendered)
{
    return std::move(rendered.channels);
}

void appendLoadedAssets(
    ProjectResourceSnapshotsLoadResult loaded,
    talas::runtime::RenderSettings& settings)
{
    settings.resourceSnapshots.insert(
        settings.resourceSnapshots.end(),
        std::make_move_iterator(loaded.snapshots.begin()),
        std::make_move_iterator(loaded.snapshots.end()));
}
}

ProjectWavExportResult exportProjectToPcm16Wav(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    ProjectWavExportOptions options)
{
    ProjectWavExportResult result;
    if (options.outputPath.empty())
    {
        result.error = "WAV output path cannot be empty";
        return result;
    }
    if (!std::isfinite(options.sampleRate) || options.sampleRate <= 0.0)
    {
        result.error = "WAV export sample rate must be finite and greater than zero";
        return result;
    }
    if (!std::isfinite(options.durationSeconds) || options.durationSeconds <= 0.0)
    {
        result.error = "WAV export duration must be finite and greater than zero";
        return result;
    }
    if (options.maxBlockFrames == 0)
    {
        result.error = "WAV export block size must be greater than zero";
        return result;
    }

    // An export must never replace the project, its package, or an asset it
    // reads, even when the user picks such a file as the destination.
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
    if (audioFileDestinationConflicts(options.outputPath, protectedFiles, protectedDirectories))
    {
        result.error = "WAV export cannot replace the project, its package, or one of its assets";
        return result;
    }

    const auto exactFrameCount =
        static_cast<long double>(options.durationSeconds)
        * static_cast<long double>(options.sampleRate);
    const auto roundedFrameCount = std::ceil(exactFrameCount);
    if (roundedFrameCount < 1.0L
        || roundedFrameCount > static_cast<long double>(std::numeric_limits<std::size_t>::max()))
    {
        result.error = "WAV export duration is too large for this platform";
        return result;
    }
    result.frameCount = static_cast<std::size_t>(roundedFrameCount);

    if (cancellationRequested(options))
    {
        result.outcome = ProjectWavExportOutcome::Cancelled;
        result.error = "WAV export cancelled";
        return result;
    }

    result.diagnostics =
        talas::runtime::projectComponentExecutionDiagnostics(
            document,
            registry);
    if (!result.diagnostics.empty())
    {
        result.error = "Project compilation failed";
        return result;
    }

    talas::runtime::RenderSettings settings;
    settings.sampleRate = options.sampleRate;
    settings.maxBlockFrames = options.maxBlockFrames;
    settings.scorePlaybackEndFrameExclusive =
        static_cast<std::uint64_t>(result.frameCount);
    auto loadedAssets = loadReferencedProjectResourceSnapshots(
        document,
        registry,
        options.projectFilePath,
        {},
        options.shouldCancel);
    if (!loadedAssets.ok())
    {
        if (loadedAssets.wasCancelled)
        {
            result.outcome = ProjectWavExportOutcome::Cancelled;
            result.error = "WAV export cancelled";
            return result;
        }
        const auto& failure = *loadedAssets.failure;
        result.error = describeProjectAssetLoadFailure(failure);
        return result;
    }
    appendLoadedAssets(std::move(loadedAssets), settings);

    auto preparedSettings = talas::runtime::prepareProjectRenderSettings(
        document,
        registry,
        std::move(settings));
    if (!preparedSettings.ok())
    {
        result.error = std::move(preparedSettings.error);
        return result;
    }

    auto compiled = talas::runtime::compileProject(
        document,
        registry,
        std::move(*preparedSettings.settings));
    if (!compiled.ok())
    {
        result.diagnostics = std::move(compiled.diagnostics);
        result.error = "Project compilation failed";
        return result;
    }
    if (cancellationRequested(options))
    {
        result.outcome = ProjectWavExportOutcome::Cancelled;
        result.error = "WAV export cancelled";
        return result;
    }

    const auto parent = options.outputPath.parent_path();
    if (!parent.empty())
    {
        std::error_code directoryError;
        std::filesystem::create_directories(parent, directoryError);
        if (directoryError)
        {
            result.error = "Unable to create WAV output directory: " + directoryError.message();
            return result;
        }
    }

    auto publication = AudioFilePublication::prepare(
        options.outputPath,
        ExistingAudioFilePolicy::Replace,
        result.error);
    if (!publication)
        return result;
    const auto temporaryPath = publication->temporaryPath();
    bool writeSucceeded = false;
    bool cancelled = false;
    {
        talas::runtime::OfflineRenderSession session{*compiled.graph};
        std::unique_ptr<Pcm16WavStreamWriter> writer;
        std::size_t renderedFrames = 0;
        while (renderedFrames < result.frameCount)
        {
            if (cancellationRequested(options))
            {
                cancelled = true;
                break;
            }

            const auto blockFrames = std::min(
                options.maxBlockFrames,
                result.frameCount - renderedFrames);
            auto rendered = session.render(blockFrames);
            if (!rendered.ok())
            {
                result.diagnostics = std::move(rendered.diagnostics);
                result.error = "Project render failed";
                break;
            }
            auto channels = takeAudioChannels(rendered);
            if (channels.empty())
            {
                result.error = "Project render produced no audio channels";
                break;
            }
            if (!writer)
            {
                result.channelCount = channels.size();
                writer = std::make_unique<Pcm16WavStreamWriter>(
                    temporaryPath,
                    options.sampleRate,
                    result.channelCount,
                    result.frameCount);
                if (!writer->ready())
                {
                    result.error = std::string(writer->error());
                    break;
                }
            }
            if (channels.size() != result.channelCount)
            {
                result.error = "Project render changed channel count between blocks";
                break;
            }
            if (!writer->write(channels))
            {
                result.error = std::string(writer->error());
                break;
            }

            renderedFrames += blockFrames;
            if (options.reportProgress)
            {
                options.reportProgress(
                    static_cast<double>(renderedFrames)
                    / static_cast<double>(result.frameCount));
            }
        }

        if (!cancelled && result.error.empty() && writer && writer->finish())
            writeSucceeded = true;
        else if (!cancelled && result.error.empty() && writer)
            result.error = std::string(writer->error());
    }

    if (cancelled)
    {
        std::string cleanupError;
        (void)publication->discard(cleanupError);
        result.outcome = ProjectWavExportOutcome::Cancelled;
        result.error = cleanupError.empty()
            ? "WAV export cancelled"
            : "WAV export cancelled; " + cleanupError;
        return result;
    }
    if (!writeSucceeded)
    {
        std::string cleanupError;
        (void)publication->discard(cleanupError);
        if (result.error.empty())
            result.error = "WAV export failed before writing completed";
        if (!cleanupError.empty())
            result.error += "; " + cleanupError;
        return result;
    }
    if (cancellationRequested(options))
    {
        std::string cleanupError;
        (void)publication->discard(cleanupError);
        result.outcome = ProjectWavExportOutcome::Cancelled;
        result.error = cleanupError.empty()
            ? "WAV export cancelled"
            : "WAV export cancelled; " + cleanupError;
        return result;
    }
    if (options.admitPublication && !options.admitPublication())
    {
        std::string cleanupError;
        (void)publication->discard(cleanupError);
        result.outcome = ProjectWavExportOutcome::Cancelled;
        result.error = cleanupError.empty()
            ? "WAV export invalidated before publication"
            : "WAV export invalidated before publication; " + cleanupError;
        return result;
    }
    if (!publication->publish(result.error))
        return result;

    result.outcome = ProjectWavExportOutcome::Succeeded;
    return result;
}
}
