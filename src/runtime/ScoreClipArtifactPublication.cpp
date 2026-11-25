#include "talas/runtime/ScoreClipArtifactPublication.h"

#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectAssetInspection.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <new>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif

namespace talas::runtime
{
namespace
{
std::atomic<std::uint64_t> stagingSequence = 0;

[[nodiscard]] bool cancelled(
    const ScoreClipArtifactPublicationOptions& options)
{
    return options.shouldCancel && options.shouldCancel();
}

void progress(
    const ScoreClipArtifactPublicationOptions& options,
    double value)
{
    if (options.reportProgress)
        options.reportProgress(value);
}

[[nodiscard]] std::filesystem::path createStagingDirectory(
    const std::filesystem::path& destination,
    std::string& error)
{
    for (std::size_t attempt = 0; attempt < 64; ++attempt)
    {
        auto candidate = destination;
        candidate += ".staging-"
            + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch().count())
            + "-" + std::to_string(
                stagingSequence.fetch_add(1, std::memory_order_relaxed));
        std::error_code createError;
        if (std::filesystem::create_directory(candidate, createError))
            return candidate;
        if (!createError || createError == std::errc::file_exists)
            continue;
        error = "Unable to create private Score clip staging directory: "
            + createError.message();
        return {};
    }
    error = "Unable to reserve a private Score clip staging directory";
    return {};
}

enum class DirectoryPublishResult
{
    Published,
    DestinationExists,
    Failed,
};

[[nodiscard]] DirectoryPublishResult publishDirectoryNoReplace(
    const std::filesystem::path& staging,
    const std::filesystem::path& destination,
    std::string& error)
{
#if defined(_WIN32)
    if (::MoveFileExW(
            staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)
        != 0)
    {
        return DirectoryPublishResult::Published;
    }
    const auto nativeError = ::GetLastError();
    if (nativeError == ERROR_FILE_EXISTS
        || nativeError == ERROR_ALREADY_EXISTS)
    {
        return DirectoryPublishResult::DestinationExists;
    }
    error = std::error_code{
        static_cast<int>(nativeError), std::system_category()}.message();
    return DirectoryPublishResult::Failed;
#elif defined(__APPLE__)
    if (::renamex_np(staging.c_str(), destination.c_str(), RENAME_EXCL) == 0)
        return DirectoryPublishResult::Published;
    if (errno == EEXIST || errno == ENOTEMPTY)
        return DirectoryPublishResult::DestinationExists;
    error = std::error_code{errno, std::generic_category()}.message();
    return DirectoryPublishResult::Failed;
#elif defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int renameNoReplace = 1;
    if (::syscall(
            SYS_renameat2,
            AT_FDCWD,
            staging.c_str(),
            AT_FDCWD,
            destination.c_str(),
            renameNoReplace) == 0)
    {
        return DirectoryPublishResult::Published;
    }
    if (errno == EEXIST || errno == ENOTEMPTY)
        return DirectoryPublishResult::DestinationExists;
    error = std::error_code{errno, std::generic_category()}.message();
    return DirectoryPublishResult::Failed;
#else
    error = "Atomic no-replace directory publication is unsupported on this platform";
    return DirectoryPublishResult::Failed;
#endif
}

struct StagingCleanup
{
    std::filesystem::path path;
    ~StagingCleanup()
    {
        if (path.empty())
            return;
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

[[nodiscard]] bool validOptions(
    const ScoreClipArtifactPublicationOptions& options) noexcept
{
    return !options.clipId.empty()
        && !options.destinationDirectory.empty()
        && options.destinationDirectory.is_absolute()
        && !options.storedPathPrefix.empty()
        && !options.assetIdPrefix.empty()
        && options.maximumAggregateSamples != 0
        && options.maximumAggregateRenderSamples != 0;
}
}

ScoreClipArtifactPublicationResult publishScoreClipArtifact(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    ScoreClipArtifactPublicationOptions options)
{
    ScoreClipArtifactPublicationResult result;
    if (!validOptions(options))
    {
        result.error = "Score clip artifact publication options are invalid";
        return result;
    }
    try
    {
        if (cancelled(options))
        {
            result.cancelled = true;
            return result;
        }
        std::error_code parentError;
        std::filesystem::create_directories(
            options.destinationDirectory.parent_path(), parentError);
        if (parentError)
        {
            result.error = "Unable to create Score clip artifact parent directory: "
                + parentError.message();
            return result;
        }
        std::error_code destinationError;
        const auto destinationStatus = std::filesystem::symlink_status(
            options.destinationDirectory, destinationError);
        if (destinationError
            && destinationError != std::errc::no_such_file_or_directory)
        {
            result.error = "Unable to inspect Score clip artifact destination: "
                + destinationError.message();
            return result;
        }
        if (!destinationError
            && destinationStatus.type()
                != std::filesystem::file_type::not_found)
        {
            result.error = "Score clip artifact destination already exists";
            return result;
        }
        std::string stagingError;
        auto staging = createStagingDirectory(
            options.destinationDirectory, stagingError);
        if (staging.empty())
        {
            result.error = std::move(stagingError);
            return result;
        }
        StagingCleanup cleanup{staging};
        progress(options, 0.05);

        ScoreClipMaterializationOptions materializationOptions;
        materializationOptions.shouldCancel = options.shouldCancel;
        materializationOptions.forceLiveSource = true;
        // The cache is recorded pre-fader.
        materializationOptions.preFader = true;
        materializationOptions.maximumAggregateSamples =
            options.maximumAggregateSamples;
        materializationOptions.maximumAggregateRenderSamples =
            options.maximumAggregateRenderSamples;
        auto materialized = materializeProjectScoreClip(
            document,
            registry,
            sourceSettings,
            options.clipId,
            std::move(materializationOptions));
        if (!materialized.ok())
        {
            result.cancelled = materialized.cancelled || cancelled(options);
            result.error = result.cancelled ? std::string{} : materialized.error;
            return result;
        }
        if (materialized.clips.size() != 1
            || materialized.clips.front() == nullptr
            || !materialized.clips.front()->valid())
        {
            result.error = "Score clip artifact materialization returned no exact clip";
            return result;
        }
        const auto& audio = *materialized.clips.front();
        result.artifact.mode = options.mode;
        result.artifact.sourceFingerprint = audio.sourceFingerprint;
        result.artifact.sampleRate = audio.sampleRate;
        result.artifact.frameCount = audio.frameCount;
        result.assets.reserve(audio.channels.size());
        result.artifact.channelAssetIds.reserve(audio.channels.size());
        for (std::size_t channel = 0; channel < audio.channels.size(); ++channel)
        {
            if (cancelled(options))
            {
                result.cancelled = true;
                result.assets.clear();
                result.artifact = {};
                return result;
            }
            const auto filename = "channel-" + std::to_string(channel + 1U)
                + ".wav";
            const auto stagingPath = staging / filename;
            std::string writeError;
            // Each channel a mono WAV at the current
            // sample rate, in the Timeline's word size, 24 bits by default.
            const auto wordSize = document.timeline.cacheWordSize == 24 ? 24 : 16;
            if (!talas::audio::writePcmWav(
                    stagingPath,
                    static_cast<double>(audio.sampleRate),
                    std::span<const std::vector<double>>{
                        audio.channels[channel].get(), 1},
                    talas::audio::WavWordSize{static_cast<std::uint16_t>(wordSize)},
                    writeError))
            {
                result.error = "Score clip channel write failed: " + writeError;
                result.assets.clear();
                result.artifact = {};
                return result;
            }
            talas::project::ProjectAsset manifest;
            manifest.id = options.assetIdPrefix + "-ch"
                + std::to_string(channel + 1U);
            manifest.kind = talas::project::ProjectAssetKind::Audio;
            manifest.path = options.storedPathPrefix + "/" + filename;
            manifest.disposition =
                talas::project::ProjectAssetDisposition::Embedded;
            manifest.channelCount = 1;
            manifest.sampleRate = static_cast<double>(audio.sampleRate);
            manifest.frameCount = audio.frameCount;
            manifest.encoding = std::string{wordSize == 16
                    ? talas::project::pcm16WavAssetEncoding
                    : talas::project::sampleFileAssetEncoding};
            manifest.generation = 1;
            manifest.provenance = "Score clip " + options.clipId
                + (options.mode == talas::project::ScoreClipRenderMode::Frozen
                    ? " frozen artifact"
                    : " cache artifact");
            const auto hash = talas::project::calculateProjectAssetSha256(
                stagingPath, options.shouldCancel);
            if (!hash.ok())
            {
                result.cancelled = hash.error->code
                    == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
                result.error = result.cancelled
                    ? std::string{}
                    : "Score clip channel hashing failed: "
                        + hash.error->message;
                result.assets.clear();
                result.artifact = {};
                return result;
            }
            manifest.contentHash = hash.contentHash;
            auto inspected = talas::project::inspectProjectAsset(
                manifest, stagingPath, options.shouldCancel);
            if (!inspected.ok())
            {
                result.cancelled = inspected.error->code
                    == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
                result.error = result.cancelled
                    ? std::string{}
                    : "Score clip channel verification failed: "
                        + inspected.error->message;
                result.assets.clear();
                result.artifact = {};
                return result;
            }
            result.artifact.channelAssetIds.push_back(inspected.asset.id);
            result.assets.push_back(std::move(inspected.asset));
            progress(
                options,
                0.70 + 0.20
                    * static_cast<double>(channel + 1U)
                    / static_cast<double>(audio.channels.size()));
        }
        if (cancelled(options))
        {
            result.cancelled = true;
            result.assets.clear();
            result.artifact = {};
            return result;
        }
        if (!options.admitPublication || !options.admitPublication())
        {
            result.invalidated = true;
            result.assets.clear();
            result.artifact = {};
            return result;
        }
        std::string publishError;
        const auto published = publishDirectoryNoReplace(
            staging, options.destinationDirectory, publishError);
        if (published != DirectoryPublishResult::Published)
        {
            result.error = published == DirectoryPublishResult::DestinationExists
                ? "Score clip artifact destination already exists"
                : "Score clip artifact directory publication failed: "
                    + publishError;
            result.assets.clear();
            result.artifact = {};
            return result;
        }
        cleanup.path.clear();
        result.publishedDirectory = options.destinationDirectory;
        progress(options, 1.0);
        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.error = "Score clip artifact publication allocation failed";
    }
    catch (const std::length_error&)
    {
        result.error = "Score clip artifact publication exceeded supported limits";
    }
    catch (const std::exception& exception)
    {
        result.error = "Score clip artifact publication failed: ";
        result.error += exception.what();
    }
    return result;
}
}
