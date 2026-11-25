#include "talas/audio/LiveTimelineRecording.h"

#include "talas/audio/AudioFilePublication.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectPackage.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/runtime/RealtimeOutputRecorder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>

namespace talas::audio
{
std::unique_ptr<LiveTimelineRecording> LiveTimelineRecording::start(
    const talas::project::ProjectDocument& document,
    TimelineRecordingOptions options,
    std::string& error)
{
    error.clear();
    const auto outputs = timelineRecordingChannelCount(document);
    auto chosen = options.channels;
    std::ranges::sort(chosen);
    if (options.destination.empty() || options.channels.empty() || std::ranges::adjacent_find(chosen) != chosen.end()
        || std::ranges::any_of(options.channels, [&](std::uint32_t channel) { return channel == 0 || channel > outputs; }))
    {
        error = "Choose some of the Timeline's output channels and where to record them";
        return nullptr;
    }
    if (options.bits != 8 && options.bits != 16 && options.bits != 24)
    {
        error = "A recording is 8, 16 or 24 bits";
        return nullptr;
    }
    if (!(options.sampleRate > 0.0) || !std::isfinite(options.sampleRate)
        || options.sampleRate > static_cast<double>(std::numeric_limits<std::uint32_t>::max())
        || options.sampleRate != std::trunc(options.sampleRate) || options.maxBlockFrames == 0
        || options.maxBlockFrames > talas::runtime::maximumRealtimeOutputRecorderBlockFrames
        || outputs > talas::runtime::maximumRealtimeOutputRecorderChannels)
    {
        error = "A live recording needs the audio device's sample rate and block size";
        return nullptr;
    }

    // The Timeline ends with its last Sound, on its composition clock,
    // which a WaitUntil within it holds.
    const auto rate = static_cast<std::uint32_t>(options.sampleRate);
    const auto prepared = talas::runtime::prepareProjectScore(
        document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, rate, 1));
    if (!prepared.ok())
    {
        error = prepared.error;
        return nullptr;
    }
    std::uint64_t end = 0;
    for (const auto& iteration : prepared.schedule->clipIterations())
        end = std::max(end, iteration.endFrame);
    if (end == 0)
    {
        error = "The Timeline has no Sounds to record";
        return nullptr;
    }

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
            error = "A recording cannot replace the project, its package or one of its assets";
            return nullptr;
        }
    }

    auto recording = std::unique_ptr<LiveTimelineRecording>{new LiveTimelineRecording};
    recording->endFrame_ = end;
    recording->masterChannels_ = outputs;
    recording->scratch_.assign(outputs, std::vector<float>(options.maxBlockFrames, 0.0F));
    recording->silence_.assign(options.maxBlockFrames, 0.0F);
    const auto stereo = paths.size() == 1 && options.channels.size() == 2;
    for (std::size_t index = 0; index < paths.size(); ++index)
    {
        File file;
        file.destination = paths[index];
        if (stereo)
            file.channels = {options.channels[0] - 1U, options.channels[1] - 1U};
        else
            file.channels = {options.channels[index] - 1U};
        // An AIFF records into a WAV beside it, saved as AIFF at the end,
        // as Record to disk's takes are.
        auto target = file.destination;
        if (options.format == TimelineRecordingFormat::Aiff)
        {
            target += ".talas-take.wav";
            file.staging = target;
        }
        RealtimeOutputTakeOptions take{
            target,
            ExistingAudioFilePolicy::Replace,
            options.sampleRate,
            file.channels.size(),
            options.maxBlockFrames,
            talas::runtime::maximumRealtimeOutputRecorderQueueCapacity,
            1,
            1,
            1,
            protectedFiles,
            protectedDirectories,
        };
        take.wordSize.bits = options.format == TimelineRecordingFormat::Aiff ? std::uint16_t{24} : options.bits;
        file.take = RealtimeOutputTake::start(std::move(take), error);
        if (!file.take)
            return nullptr;
        recording->files_.push_back(std::move(file));
    }
    options.shouldCancel = {};
    options.progress = {};
    recording->options_ = std::move(options);
    return recording;
}

LiveTimelineRecording::~LiveTimelineRecording()
{
    if (!finished_)
        (void) cancel();
}

std::size_t LiveTimelineRecording::masterChannelCount() const noexcept
{
    return masterChannels_;
}

std::uint64_t LiveTimelineRecording::endFrame() const noexcept
{
    return endFrame_;
}

void LiveTimelineRecording::lendChannels(std::span<float*> channels, std::size_t frameCount) noexcept
{
    if (frameCount > options_.maxBlockFrames || reachedEnd_.load(std::memory_order_acquire))
        return;
    for (std::size_t channel = 0; channel < std::min(channels.size(), masterChannels_); ++channel)
    {
        if (channels[channel] != nullptr)
            continue;
        std::fill_n(scratch_[channel].data(), frameCount, 0.0F);
        channels[channel] = scratch_[channel].data();
    }
}

void LiveTimelineRecording::startsWithStage(std::uint64_t stageId) noexcept
{
    awaitedStageId_.store(stageId, std::memory_order_release);
}

void LiveTimelineRecording::publishFromAudio(
    float* const* channels,
    std::size_t channelCount,
    std::size_t frameCount,
    std::optional<std::uint64_t> scoreFrameAfter,
    std::uint64_t stageId) noexcept
{
    if (frameCount == 0 || reachedEnd_.load(std::memory_order_acquire) || failed_.load(std::memory_order_acquire))
        return;
    if (!started_.load(std::memory_order_acquire))
    {
        // The Timeline starts from its beginning within this block, in the
        // stage awaited.
        const auto awaited = awaitedStageId_.load(std::memory_order_acquire);
        if (!scoreFrameAfter.has_value() || *scoreFrameAfter > frameCount || stageId == 0
            || (awaited != 0 && stageId != awaited))
            return;
        stageId_.store(stageId, std::memory_order_release);
        started_.store(true, std::memory_order_release);
    }
    if (!scoreFrameAfter.has_value() || stageId != stageId_.load(std::memory_order_acquire))
    {
        rejectRenderFailureFromAudio();
        return;
    }
    scoreFrame_.store(std::min(*scoreFrameAfter, endFrame_), std::memory_order_release);
    auto keep = frameCount;
    const auto ends = *scoreFrameAfter >= endFrame_;
    if (ends)
    {
        const auto overshoot = *scoreFrameAfter - endFrame_;
        keep = overshoot >= frameCount ? 0 : frameCount - static_cast<std::size_t>(overshoot);
    }
    for (std::size_t offset = 0; offset < keep;)
    {
        const auto frames = std::min(options_.maxBlockFrames, keep - offset);
        for (auto& file : files_)
        {
            std::array<float*, 2> pointers{};
            for (std::size_t index = 0; index < file.channels.size(); ++index)
            {
                const auto channel = file.channels[index];
                pointers[index] = channel < channelCount && channels != nullptr && channels[channel] != nullptr
                    ? channels[channel] + offset
                    : silence_.data();
            }
            (void) file.take->publishFromAudio(pointers.data(), file.channels.size(), frames, 1);
        }
        offset += frames;
    }
    recordedFrames_.fetch_add(keep, std::memory_order_acq_rel);
    if (ends)
        reachedEnd_.store(true, std::memory_order_release);
}

void LiveTimelineRecording::rejectRenderFailureFromAudio() noexcept
{
    if (!started_.load(std::memory_order_acquire) || reachedEnd_.load(std::memory_order_acquire))
        return;
    failed_.store(true, std::memory_order_release);
    for (auto& file : files_)
        file.take->rejectRenderFailureFromAudio();
}

bool LiveTimelineRecording::started() const noexcept
{
    return started_.load(std::memory_order_acquire);
}

bool LiveTimelineRecording::reachedEnd() const noexcept
{
    return reachedEnd_.load(std::memory_order_acquire);
}

double LiveTimelineRecording::progress() const noexcept
{
    return endFrame_ == 0 ? 0.0
                          : static_cast<double>(scoreFrame_.load(std::memory_order_acquire))
            / static_cast<double>(endFrame_);
}

std::uint64_t LiveTimelineRecording::recordedFrames() const noexcept
{
    return recordedFrames_.load(std::memory_order_acquire);
}

std::uint64_t LiveTimelineRecording::writtenFrames() const
{
    std::uint64_t fewest = std::numeric_limits<std::uint64_t>::max();
    for (const auto& file : files_)
        fewest = std::min(fewest, file.take->snapshot().writtenFrames);
    return files_.empty() ? 0 : fewest;
}

TimelineRecordingResult LiveTimelineRecording::finish()
{
    TimelineRecordingResult result;
    result.frameCount = static_cast<std::size_t>(recordedFrames());
    if (finished_)
    {
        result.error = "The recording has already finished";
        return result;
    }
    if (!started())
    {
        auto cancelled = cancel();
        cancelled.cancelled = false;
        cancelled.error = "The Timeline did not start playing, so nothing was recorded";
        return cancelled;
    }
    finished_ = true;
    // What this recording wrote: each completed take's WAV, then each AIFF
    // saved from one.
    std::vector<std::filesystem::path> written;
    for (auto& file : files_)
    {
        file.take->requestStop();
        file.take->join();
        const auto snapshot = file.take->snapshot();
        if (snapshot.state == RealtimeOutputTakeState::Completed)
            written.push_back(snapshot.destination);
        else if (result.error.empty())
        {
            result.error = failed_.load(std::memory_order_acquire)
                ? std::string{"The Timeline was compiled again or failed to play while it recorded, so the recording "
                              "is incomplete"}
                : snapshot.diagnostic.empty() ? std::string{"The recording could not be written"} : snapshot.diagnostic;
        }
    }
    for (auto& file : files_)
    {
        if (!result.error.empty())
            break;
        if (file.staging.has_value())
        {
            if (auto converted = convertRecordingToSampleFile(
                    *file.staging, file.destination, SampleFileFormat::Aiff, options_.bits);
                !converted.empty())
            {
                result.error = std::move(converted);
                break;
            }
            written.push_back(file.destination);
        }
        result.files.push_back(file.destination);
    }
    if (!result.error.empty())
    {
        // Every file or none.
        std::error_code ignored;
        for (const auto& path : written)
            std::filesystem::remove(path, ignored);
        result.files.clear();
    }
    return result;
}

TimelineRecordingResult LiveTimelineRecording::cancel()
{
    TimelineRecordingResult result;
    result.frameCount = static_cast<std::size_t>(recordedFrames());
    result.cancelled = true;
    result.error = "The recording was cancelled";
    finished_ = true;
    for (auto& file : files_)
    {
        file.take->requestCancel();
        file.take->join();
    }
    return result;
}
}
