#include "talas/runtime/ScoreClipMaterialization.h"
#include "ScoreTrackPanning.h"
#include "PreparedScoreClipSource.h"

#include "talas/audio/WavReader.h"
#include "talas/audio/ProjectAssetLoader.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectScoreClipFingerprint.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/runtime/ProjectRenderSettings.h"
#include "talas/runtime/PreparedScore.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <ranges>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace talas::runtime
{
namespace
{
[[nodiscard]] bool cancelled(const ScoreClipMaterializationOptions& options)
{
    return options.shouldCancel && options.shouldCancel();
}

// The clip's fades, its track's Replace overlap and its panning (on two
// channels, or the speakers of the Speaker Placement), applied to its audio
// as it plays.
void applyClipGains(const PreparedScoreSchedule& schedule, std::size_t clipIndex, std::vector<std::vector<double>>& channels,
    std::span<const talas::project::SpeakerPosition> speakers)
{
    const auto& clip = schedule.clips()[clipIndex];
    const auto& track = schedule.tracks()[clip.trackIndex];
    const auto replaces = track.overlapMode == talas::project::ScoreOverlapMode::Replace;
    const auto panned = (channels.size() == 2 || !speakers.empty())
        && scoreClipIsPanned(schedule, static_cast<std::uint32_t>(clipIndex), clip.trackIndex);
    for (std::size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex)
    {
        auto& channel = channels[channelIndex];
        for (std::size_t frame = 0; frame < channel.size(); ++frame)
        {
            double gain = !replaces || schedule.clipAudibleAt(clipIndex, clip.startFrame + frame)
                ? 1.0 : 0.0;
            const auto pan = panned
                ? scoreClipChannelGains(schedule, static_cast<std::uint32_t>(clipIndex), clip.trackIndex,
                      clip.startFrame + frame, speakers, channels.size())
                : std::nullopt;
            if (clip.fadeInFrames != 0 && frame < clip.fadeInFrames)
            {
                gain = static_cast<double>(frame)
                    / static_cast<double>(clip.fadeInFrames);
            }
            const auto remaining = channel.size() - frame;
            if (clip.fadeOutFrames != 0 && remaining <= clip.fadeOutFrames)
            {
                gain = std::min(
                    gain,
                    static_cast<double>(remaining - 1U)
                        / static_cast<double>(clip.fadeOutFrames));
            }
            channel[frame] *= std::clamp(gain, 0.0, 1.0)
                * (pan.has_value() ? (channelIndex < pan->size() ? (*pan)[channelIndex] : 0.0) : 1.0);
        }
    }
}

[[nodiscard]] PreparedScoreClipAudioPtr loadPersistedArtifact(
    const talas::project::ScoreClip& sourceClip,
    const PreparedScoreClip& clip,
    std::size_t clipIndex,
    const RenderSettings& settings,
    std::string_view currentFingerprint,
    const ScoreClipMaterializationOptions& options,
    std::string& error)
{
    if (!sourceClip.renderArtifact.has_value())
        return {};
    const auto& artifact = *sourceClip.renderArtifact;
    if (artifact.mode == talas::project::ScoreClipRenderMode::Cached
        && artifact.sourceFingerprint != currentFingerprint)
    {
        return {};
    }
    auto prepared = std::make_shared<PreparedScoreClipAudio>();
    prepared->binding = settings.preparedScore->binding();
    prepared->clipId = clip.clipId;
    prepared->sourceFingerprint = artifact.sourceFingerprint;
    prepared->sampleRate = artifact.sampleRate;
    prepared->startFrame = clip.startFrame;
    prepared->frameCount = artifact.frameCount;
    if (prepared->sampleRate != settings.preparedScore->binding().sampleRate
        || prepared->frameCount != clip.endFrame - clip.startFrame)
    {
        error = "Score clip artifact media binding is stale for '"
            + clip.clipId + "'";
        return {};
    }
    std::vector<std::vector<double>> channels;
    for (const auto& assetId : artifact.channelAssetIds)
    {
        if (cancelled(options))
            return {};
        const auto snapshot = std::ranges::find_if(
            settings.resourceSnapshots,
            [&](const auto& candidate) {
                return candidate != nullptr && candidate->id == assetId;
            });
        if (snapshot == settings.resourceSnapshots.end()
            || (*snapshot)->kind != talas::core::ResourceSnapshotKind::Audio
            || (*snapshot)->channelCount != 1
            || (*snapshot)->sampleRate != artifact.sampleRate
            || (*snapshot)->frameCount != artifact.frameCount
            || ((*snapshot)->encoding != "pcm16-wav"
                && (*snapshot)->encoding != talas::project::sampleFileAssetEncoding))
        {
            error = "Score clip artifact resource is unavailable for '"
                + clip.clipId + "'";
            return {};
        }
        // A cache of 24 bits is a sample file.
        auto loaded = (*snapshot)->encoding == "pcm16-wav"
            ? talas::audio::readPcm16Wav((*snapshot)->resolvedPath, options.shouldCancel)
            : talas::audio::readSampleFile((*snapshot)->resolvedPath, options.shouldCancel);
        if (!loaded.ok() || loaded.channels.size() != 1
            || loaded.sampleRate != artifact.sampleRate
            || loaded.channels.front().size() != artifact.frameCount
            || loaded.contentHash != (*snapshot)->contentHash)
        {
            error = loaded.cancelled
                ? "Score clip artifact loading was cancelled"
                : "Score clip artifact bytes changed for '" + clip.clipId + "'";
            return {};
        }
        channels.push_back(std::move(loaded.channels.front()));
    }
    // The cache holds the Sound before its fades and panning.
    applyClipGains(*settings.preparedScore, clipIndex, channels, settings.speakerPlacement);
    for (auto& channel : channels)
        prepared->channels.push_back(std::make_shared<const std::vector<double>>(std::move(channel)));
    if (!prepared->valid())
    {
        error = "Score clip artifact is invalid for '" + clip.clipId + "'";
        return {};
    }
    return prepared;
}

// A clip as it is heard, at the Levels of its
// track and of the submixes its output route goes through; what it sends,
// and its cache, stay before them. A copy with its own fingerprint, the
// same artifact when no track has a Level.
[[nodiscard]] PreparedScoreClipAudioPtr withRouteLevels(
    const PreparedScoreSchedule& schedule, std::size_t clipIndex, PreparedScoreClipAudioPtr prepared)
{
    if (prepared == nullptr || (!scoreHasTrackLevels(schedule) && !scoreHasClipFades(schedule)))
        return prepared;
    const auto trackIndex = schedule.clips()[clipIndex].trackIndex;
    std::vector<double> levels(static_cast<std::size_t>(prepared->frameCount));
    for (std::size_t frame = 0; frame < levels.size(); ++frame)
        levels[frame] = scoreRouteLevelAt(schedule, trackIndex, prepared->startFrame + frame)
            * scoreClipFadeAt(schedule, clipIndex, prepared->startFrame + frame);
    if (std::ranges::all_of(levels, [](double level) { return level == 1.0; }))
        return prepared;
    auto levelled = std::make_shared<PreparedScoreClipAudio>(*prepared);
    levelled->channels.clear();
    for (const auto& channel : prepared->channels)
    {
        auto samples = *channel;
        for (std::size_t frame = 0; frame < samples.size() && frame < levels.size(); ++frame)
            samples[frame] *= levels[frame];
        levelled->channels.push_back(std::make_shared<const std::vector<double>>(std::move(samples)));
    }
    std::string bytes = prepared->sourceFingerprint + "\nlevel:";
    bytes.append(reinterpret_cast<const char*>(levels.data()), levels.size() * sizeof(double));
    levelled->sourceFingerprint = talas::project::calculateProjectAssetSha256(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    return levelled;
}

// A track's audio input, the sum of the audio
// of the clips on the Sound tracks reaching its submix, over the frames a
// clip's source renders from its first.
[[nodiscard]] std::vector<AudioInputChannel> submixInputChannels(
    const PreparedScoreSchedule& schedule,
    const PreparedScoreTrack& track,
    std::span<const PreparedScoreClipAudioPtr> preparedByClip,
    std::int64_t firstFrame,
    std::size_t frameCount)
{
    const auto& submix = schedule.tracks()[*track.inputTrackIndex];
    std::vector<AudioInputChannel> inputs;
    for (std::uint32_t channel = 0; channel < submix.channelCount; ++channel)
        inputs.push_back({channel + 1U, std::vector<double>(frameCount, 0.0), std::nullopt});
    for (std::size_t index = 0; index < schedule.clips().size() && index < preparedByClip.size(); ++index)
    {
        const auto& audio = preparedByClip[index];
        const auto feeder = std::ranges::find(track.inputFeederTrackIndices, schedule.clips()[index].trackIndex);
        if (audio == nullptr || audio->channels.empty() || feeder == track.inputFeederTrackIndices.end())
            continue;
        // The feeding track's Track_To_Submix control, full when it
        // has no lane.
        const auto entry = track.inputFeederEntryTrackIndices[static_cast<std::size_t>(
            feeder - track.inputFeederTrackIndices.begin())];
        const auto levelName = talas::project::scoreSendLevelName(schedule.tracks()[entry].trackId);
        const auto level = std::ranges::find_if(schedule.automationLanes(), [&](const PreparedScoreAutomationLane& lane) {
            return lane.scope == talas::project::ScoreAutomationScope::Track && lane.ownerIndex == *feeder
                && lane.eventName == levelName;
        });
        // What enters a submix is taken before the feeding
        // track's Level (Trk1Out * Trk1_To_Vox), and passes the Levels of
        // the submixes on the way to this one.
        const auto audioStart = static_cast<std::int64_t>(audio->startFrame);
        const auto from = std::max(firstFrame, audioStart);
        const auto to = std::min(firstFrame + static_cast<std::int64_t>(frameCount),
            audioStart + static_cast<std::int64_t>(audio->frameCount));
        for (std::size_t channel = 0; channel < inputs.size(); ++channel)
        {
            const auto& samples = *audio->channels[std::min(channel, audio->channels.size() - 1U)];
            for (auto frame = from; frame < to; ++frame)
            {
                const auto at = static_cast<std::uint64_t>(frame);
                const auto send = level == schedule.automationLanes().end()
                    ? 1.0
                    : scoreLaneValueAt(schedule, *level, at).value_or(1.0);
                const auto gain = send * scoreRouteLevelAt(schedule, entry, at, *track.inputTrackIndex)
                    * scoreClipFadeAt(schedule, index, at);
                inputs[channel].samples[static_cast<std::size_t>(frame - firstFrame)]
                    += gain * samples[static_cast<std::size_t>(frame - audioStart)];
            }
        }
    }
    return inputs;
}

[[nodiscard]] PreparedScoreClipAudioPtr renderLiveClip(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    const PreparedScoreSchedule& schedule,
    std::size_t clipIndex,
    std::string fingerprint,
    const ScoreClipMaterializationOptions& options,
    std::size_t& aggregateSamples,
    std::size_t& aggregateRenderSamples,
    std::vector<TableResource>* recordedTables,
    std::span<const PreparedScoreClipAudioPtr> preparedByClip,
    bool& wasCancelled,
    std::string& error)
{
    const auto cancellationRequested = [&] {
        if (!cancelled(options))
            return false;
        wasCancelled = true;
        return true;
    };
    const auto& clip = schedule.clips()[clipIndex];
    const auto& track = schedule.tracks()[clip.trackIndex];
    const auto totalFrames = clip.endFrame - clip.startFrame;
    if (track.channelCount == 0
        || totalFrames
            > std::numeric_limits<std::size_t>::max()
        || totalFrames
            > (options.maximumAggregateSamples - aggregateSamples)
                / track.channelCount)
    {
        error = "Score clip materialization exceeds the bounded sample budget";
        return {};
    }
    auto prepared = std::make_shared<PreparedScoreClipAudio>();
    prepared->binding = schedule.binding();
    prepared->clipId = clip.clipId;
    prepared->sourceFingerprint = std::move(fingerprint);
    prepared->sampleRate = schedule.binding().sampleRate;
    prepared->startFrame = clip.startFrame;
    prepared->frameCount = totalFrames;
    std::vector<std::vector<double>> channels(
        track.channelCount,
        std::vector<double>(static_cast<std::size_t>(totalFrames), 0.0));

    std::size_t destinationOffset = 0;
    for (std::uint32_t iteration = 0;
         iteration < clip.repeatCount;
         ++iteration)
    {
        if (cancellationRequested())
            return {};
        const auto iterationIndex = clip.firstIteration + iteration;
        if (iterationIndex >= schedule.clipIterations().size())
        {
            error = "Score clip materialization lost its prepared repeat boundary";
            return {};
        }
        const auto& preparedIteration =
            schedule.clipIterations()[iterationIndex];
        const auto iterationFrames =
            preparedIteration.endFrame - preparedIteration.startFrame;
        if (clip.contentOffsetFrames
                > std::numeric_limits<std::size_t>::max()
            || iterationFrames
                > std::numeric_limits<std::size_t>::max()
            || clip.contentOffsetFrames
                > std::numeric_limits<std::size_t>::max()
                    - iterationFrames)
        {
            error = "Score clip source offset exceeds the offline renderer range";
            return {};
        }
        const auto sourceFrames = static_cast<std::size_t>(
            clip.contentOffsetFrames + iterationFrames);
        if (aggregateRenderSamples
                > options.maximumAggregateRenderSamples
            || sourceFrames
                > (options.maximumAggregateRenderSamples
                    - aggregateRenderSamples) / track.channelCount)
        {
            error = "Score clip materialization exceeds the bounded source-render budget";
            return {};
        }
        aggregateRenderSamples += sourceFrames * track.channelCount;
        auto iterationSettings = sourceSettings;
        if (track.inputTrackIndex.has_value())
            iterationSettings.audioInputChannels = submixInputChannels(schedule, track, preparedByClip,
                static_cast<std::int64_t>(preparedIteration.startFrame)
                    - static_cast<std::int64_t>(clip.contentOffsetFrames),
                sourceFrames);
        auto compiled = prepareScoreClipSourceIteration(
            document, registry, iterationSettings, clipIndex, iteration);
        if (!compiled.ok())
        {
            error = "Score clip source compilation failed for '"
                + clip.clipId + "'";
            for (const auto& diagnostic : compiled.diagnostics)
                error += " | " + diagnostic.message;
            return {};
        }
        OfflineRenderSession session{*compiled.graph};
        const auto renderChunkFrames = std::max<std::size_t>(
            1U,
            std::min<std::size_t>(sourceSettings.maxBlockFrames, 4096U));
        if (clip.contentOffsetFrames != 0)
        {
            auto remaining = static_cast<std::size_t>(
                clip.contentOffsetFrames);
            while (remaining != 0)
            {
                if (cancellationRequested())
                    return {};
                const auto chunk = std::min(remaining, renderChunkFrames);
                auto discarded = session.render(chunk);
                if (!discarded.ok())
                {
                    error = "Score clip source offset render failed for '"
                        + clip.clipId + "'";
                    return {};
                }
                remaining -= chunk;
            }
        }
        std::size_t renderedOffset = 0;
        while (renderedOffset < iterationFrames)
        {
            if (cancellationRequested())
                return {};
            const auto chunk = std::min<std::size_t>(
                renderChunkFrames,
                static_cast<std::size_t>(iterationFrames) - renderedOffset);
            auto rendered = session.render(chunk);
            if (!rendered.ok() || rendered.channels.empty())
            {
                error = "Score clip source render failed for '"
                    + clip.clipId + "'";
                return {};
            }
            if (rendered.channels.size() != track.channelCount
                && rendered.channels.size() != 1)
            {
                error = "Score clip source changed its output-channel contract";
                return {};
            }
            for (std::size_t channel = 0;
                 channel < track.channelCount;
                 ++channel)
            {
                const auto& source = rendered.channels.size() == 1
                    ? rendered.channels.front()
                    : rendered.channels[channel];
                const auto writeOffset = destinationOffset + renderedOffset;
                if (source.size() != chunk
                    || writeOffset > channels[channel].size()
                    || source.size()
                        > channels[channel].size() - writeOffset)
                {
                    error = "Score clip materialization produced an inconsistent repeat length";
                    return {};
                }
                std::ranges::copy(
                    source,
                    channels[channel].begin()
                        + static_cast<std::ptrdiff_t>(writeOffset));
            }
            renderedOffset += chunk;
        }
        if (recordedTables != nullptr)
        {
            auto captured = session.recordedMemoryWriterTables();
            std::unordered_set<std::string> names;
            for (auto& table : captured)
            {
                if (!names.insert(table.id).second)
                {
                    error = "Score clip has multiple MemoryWriters with the same recording name";
                    return {};
                }
                const auto existing = std::ranges::find(
                    *recordedTables, table.id, &TableResource::id);
                if (existing == recordedTables->end())
                    recordedTables->push_back(std::move(table));
                else
                    *existing = std::move(table);
            }
        }
        destinationOffset += static_cast<std::size_t>(iterationFrames);
    }
    if (destinationOffset != totalFrames)
    {
        error = "Score clip materialization did not fill the exact prepared range";
        return {};
    }
    if (!options.preFader)
        applyClipGains(schedule, clipIndex, channels, sourceSettings.speakerPlacement);
    for (auto& channel : channels)
        prepared->channels.push_back(std::make_shared<const std::vector<double>>(std::move(channel)));
    aggregateSamples += static_cast<std::size_t>(totalFrames)
        * track.channelCount;
    if (!prepared->valid())
    {
        error = "Score clip materialization produced invalid immutable audio";
        return {};
    }
    return prepared;
}

// A clip of a group rendered together: its audio so far and the repeat it
// is playing, compiled and rendering, or the next to start.
struct TogetherClip
{
    std::size_t clipIndex = 0;
    std::vector<std::vector<double>> channels;
    std::uint32_t iteration = 0;
    std::uint64_t iterationEnd = 0;
    std::unique_ptr<CompileResult> compiled;
    std::unique_ptr<OfflineRenderSession> session;
};

// Format 156: clips that overlap in time, on
// separate tracks, rendered together block by block, the clips holding a
// MemoryWriter first, so that a reader on one track plays what a writer on
// another records as it records it, as the two would in a Mixer. `group`
// lists the clips in that order; the result holds each one's audio in the
// same order, and `recordedTables` the recordings their writers finished.
[[nodiscard]] std::vector<PreparedScoreClipAudioPtr> renderLiveClipsTogether(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    const PreparedScoreSchedule& schedule,
    std::span<const std::size_t> group,
    std::span<const std::string> fingerprints,
    std::span<const std::uint8_t> preFader,
    const ScoreClipMaterializationOptions& options,
    std::size_t& aggregateSamples,
    std::size_t& aggregateRenderSamples,
    std::span<const PreparedScoreClipAudioPtr> preparedByClip,
    std::vector<TableResource>& recordedTables,
    bool& wasCancelled,
    std::string& error)
{
    const auto cancellationRequested = [&] {
        if (!cancelled(options))
            return false;
        wasCancelled = true;
        return true;
    };
    std::vector<TogetherClip> members(group.size());
    auto groupStart = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t groupEnd = 0;
    for (std::size_t member = 0; member < group.size(); ++member)
    {
        const auto& clip = schedule.clips()[group[member]];
        const auto& track = schedule.tracks()[clip.trackIndex];
        const auto totalFrames = clip.endFrame - clip.startFrame;
        if (track.channelCount == 0
            || totalFrames > std::numeric_limits<std::size_t>::max()
            || totalFrames > (options.maximumAggregateSamples - aggregateSamples) / track.channelCount)
        {
            error = "Score clip materialization exceeds the bounded sample budget";
            return {};
        }
        aggregateSamples += static_cast<std::size_t>(totalFrames) * track.channelCount;
        members[member].clipIndex = group[member];
        members[member].channels.assign(
            track.channelCount, std::vector<double>(static_cast<std::size_t>(totalFrames), 0.0));
        groupStart = std::min(groupStart, clip.startFrame);
        groupEnd = std::max(groupEnd, clip.endFrame);
    }
    // Where a reader finds a recording no writer of its own makes: as
    // another member's writer is making it, else as one that has finished
    // left it.
    const auto liveRecording = [&members, &recordedTables](std::string_view name)
        -> const std::vector<double>* {
        for (const auto& member : members)
        {
            if (member.session == nullptr)
                continue;
            const auto* recording = member.session->memoryWriterRecording(name);
            if (recording != nullptr && !recording->empty())
                return recording;
        }
        const auto finished = std::ranges::find_if(recordedTables,
            [name](const TableResource& table) { return table.id == name; });
        return finished == recordedTables.end() ? nullptr : &finished->samples;
    };
    // A frame at a time, as a Sound renders its own named reads, so that a
    // reader hears only what was written at or before its own sample.
    constexpr std::size_t renderChunkFrames = 1;
    // A repeat's end: the recordings its writers finished stay for the
    // members still playing and the clips after the group.
    const auto finishIteration = [&](TogetherClip& member) {
        std::unordered_set<std::string> names;
        for (auto& table : member.session->recordedMemoryWriterTables())
        {
            if (!names.insert(table.id).second)
            {
                error = "Score clip has multiple MemoryWriters with the same recording name";
                return false;
            }
            const auto existing = std::ranges::find(recordedTables, table.id, &TableResource::id);
            if (existing == recordedTables.end())
                recordedTables.push_back(std::move(table));
            else
                *existing = std::move(table);
        }
        member.session.reset();
        member.compiled.reset();
        ++member.iteration;
        return true;
    };
    // Starts each repeat beginning at `frame`: compiles it, lets its readers
    // read the others' recordings and rolls it past the clip's offset.
    const auto startIterations = [&](std::uint64_t frame) {
        for (auto& member : members)
        {
            const auto& clip = schedule.clips()[member.clipIndex];
            const auto& track = schedule.tracks()[clip.trackIndex];
            while (member.session == nullptr && member.iteration < clip.repeatCount)
            {
                const auto iterationIndex = clip.firstIteration + member.iteration;
                if (iterationIndex >= schedule.clipIterations().size())
                {
                    error = "Score clip materialization lost its prepared repeat boundary";
                    return false;
                }
                const auto& prepared = schedule.clipIterations()[iterationIndex];
                if (prepared.startFrame != frame)
                    break;
                const auto iterationFrames = prepared.endFrame - prepared.startFrame;
                if (clip.contentOffsetFrames > std::numeric_limits<std::size_t>::max() - iterationFrames)
                {
                    error = "Score clip source offset exceeds the offline renderer range";
                    return false;
                }
                const auto sourceFrames = static_cast<std::size_t>(clip.contentOffsetFrames + iterationFrames);
                if (aggregateRenderSamples > options.maximumAggregateRenderSamples
                    || sourceFrames > (options.maximumAggregateRenderSamples - aggregateRenderSamples)
                            / track.channelCount)
                {
                    error = "Score clip materialization exceeds the bounded source-render budget";
                    return false;
                }
                aggregateRenderSamples += sourceFrames * track.channelCount;
                auto iterationSettings = sourceSettings;
                if (track.inputTrackIndex.has_value())
                    iterationSettings.audioInputChannels = submixInputChannels(schedule, track, preparedByClip,
                        static_cast<std::int64_t>(prepared.startFrame)
                            - static_cast<std::int64_t>(clip.contentOffsetFrames),
                        sourceFrames);
                member.compiled = std::make_unique<CompileResult>(prepareScoreClipSourceIteration(
                    document, registry, iterationSettings, member.clipIndex, member.iteration));
                if (!member.compiled->ok())
                {
                    error = "Score clip source compilation failed for '" + clip.clipId + "'";
                    for (const auto& diagnostic : member.compiled->diagnostics)
                        error += " | " + diagnostic.message;
                    return false;
                }
                member.session = std::make_unique<OfflineRenderSession>(*member.compiled->graph);
                member.session->readLiveMemoryWriterRecordings(liveRecording);
                member.iterationEnd = prepared.endFrame;
                for (auto remaining = static_cast<std::size_t>(clip.contentOffsetFrames); remaining != 0;)
                {
                    if (cancellationRequested())
                        return false;
                    const auto chunk = std::min<std::size_t>(remaining, 4096U);
                    if (!member.session->render(chunk).ok())
                    {
                        error = "Score clip source offset render failed for '" + clip.clipId + "'";
                        return false;
                    }
                    remaining -= chunk;
                }
                if (member.iterationEnd == frame && !finishIteration(member))
                    return false;
            }
        }
        return true;
    };
    for (auto frame = groupStart; frame < groupEnd;)
    {
        if (cancellationRequested() || !startIterations(frame))
            return {};
        // The next boundary: a repeat ending or starting.
        auto next = std::min<std::uint64_t>(groupEnd, frame + renderChunkFrames);
        for (const auto& member : members)
        {
            const auto& clip = schedule.clips()[member.clipIndex];
            if (member.session != nullptr)
                next = std::min(next, member.iterationEnd);
            else if (member.iteration < clip.repeatCount
                && clip.firstIteration + member.iteration < schedule.clipIterations().size())
            {
                const auto start = schedule.clipIterations()[clip.firstIteration + member.iteration].startFrame;
                if (start > frame)
                    next = std::min(next, start);
            }
        }
        if (next <= frame)
        {
            error = "Score clips rendered together lost their prepared boundaries";
            return {};
        }
        // Writers first: the group lists them first.
        for (auto& member : members)
        {
            if (member.session == nullptr)
                continue;
            const auto& clip = schedule.clips()[member.clipIndex];
            const auto chunk = static_cast<std::size_t>(next - frame);
            auto rendered = member.session->render(chunk);
            if (!rendered.ok() || rendered.channels.empty())
            {
                error = "Score clip source render failed for '" + clip.clipId + "'";
                return {};
            }
            if (rendered.channels.size() != member.channels.size() && rendered.channels.size() != 1)
            {
                error = "Score clip source changed its output-channel contract";
                return {};
            }
            const auto writeOffset = static_cast<std::size_t>(frame - clip.startFrame);
            for (std::size_t channel = 0; channel < member.channels.size(); ++channel)
            {
                const auto& source = rendered.channels.size() == 1 ? rendered.channels.front()
                                                                    : rendered.channels[channel];
                auto& destination = member.channels[channel];
                if (source.size() != chunk || writeOffset > destination.size()
                    || source.size() > destination.size() - writeOffset)
                {
                    error = "Score clip materialization produced an inconsistent repeat length";
                    return {};
                }
                std::ranges::copy(source, destination.begin() + static_cast<std::ptrdiff_t>(writeOffset));
            }
        }
        frame = next;
        for (auto& member : members)
        {
            if (member.session != nullptr && member.iterationEnd == frame && !finishIteration(member))
                return {};
        }
    }
    std::vector<PreparedScoreClipAudioPtr> audio;
    for (std::size_t member = 0; member < members.size(); ++member)
    {
        auto& together = members[member];
        const auto& clip = schedule.clips()[together.clipIndex];
        if (together.iteration != clip.repeatCount)
        {
            error = "Score clip materialization did not fill the exact prepared range";
            return {};
        }
        if (!preFader[member])
            applyClipGains(schedule, together.clipIndex, together.channels, sourceSettings.speakerPlacement);
        auto prepared = std::make_shared<PreparedScoreClipAudio>();
        prepared->binding = schedule.binding();
        prepared->clipId = clip.clipId;
        prepared->sourceFingerprint = fingerprints[member];
        prepared->sampleRate = schedule.binding().sampleRate;
        prepared->startFrame = clip.startFrame;
        prepared->frameCount = clip.endFrame - clip.startFrame;
        for (auto& channel : together.channels)
            prepared->channels.push_back(std::make_shared<const std::vector<double>>(std::move(channel)));
        if (!prepared->valid())
        {
            error = "Score clip materialization produced invalid immutable audio";
            return {};
        }
        audio.push_back(std::move(prepared));
    }
    return audio;
}
}

ScoreClipMaterializationSettingsResult
prepareScoreClipMaterializationSettings(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>& projectFilePath,
    std::string_view clipId,
    double sampleRate,
    std::size_t maximumBlockFrames,
    std::function<bool()> shouldCancel)
{
    ScoreClipMaterializationSettingsResult result;
    try
    {
        if (clipId.empty() || !std::isfinite(sampleRate)
            || sampleRate <= 0.0 || maximumBlockFrames == 0
            || std::floor(sampleRate) != sampleRate
            || sampleRate > static_cast<double>(
                std::numeric_limits<std::uint32_t>::max()))
        {
            result.error = "Score clip materialization settings are invalid";
            return result;
        }
        const auto prepared = prepareProjectScore(
            document,
            preparedScoreBindingForDocument(
                document,
                1,
                1,
                static_cast<std::uint32_t>(sampleRate),
                1));
        if (!prepared.ok())
        {
            result.error = prepared.error;
            return result;
        }
        const auto preparedClip = std::ranges::find(
            prepared.schedule->clips(),
            clipId,
            &PreparedScoreClip::clipId);
        if (preparedClip == prepared.schedule->clips().end())
        {
            result.error = "Unknown Score clip materialization identity";
            return result;
        }
        const auto sourceDocument = isolatedScoreClipSourceDocument(
            document,
            *preparedClip,
            prepared.schedule->tracks()[preparedClip->trackIndex],
            result.error);
        if (!sourceDocument.has_value())
            return result;
        auto snapshots = talas::audio::loadReferencedProjectResourceSnapshots(
            *sourceDocument,
            registry,
            projectFilePath,
            {},
            shouldCancel);
        if (!snapshots.ok())
        {
            result.cancelled = snapshots.wasCancelled;
            result.error = result.cancelled
                ? std::string{}
                : snapshots.failure.has_value()
                    ? snapshots.failure->message
                    : "Score clip source resource loading failed";
            return result;
        }
        RenderSettings settings;
        settings.sampleRate = sampleRate;
        settings.maxBlockFrames = maximumBlockFrames;
        settings.resourceSnapshots = std::move(snapshots.snapshots);
        auto configured = prepareProjectRenderSettings(
            *sourceDocument, registry, std::move(settings));
        if (!configured.ok())
        {
            result.error = std::move(configured.error);
            return result;
        }
        configured.settings->preparedScore = prepared.schedule;
        result.settings = std::move(configured.settings);
    }
    catch (const std::bad_alloc&)
    {
        result.error = "Score clip materialization settings allocation failed";
    }
    catch (const std::length_error&)
    {
        result.error = "Score clip materialization settings exceeded supported limits";
    }
    catch (const std::exception& exception)
    {
        result.error = "Score clip materialization settings failed: ";
        result.error += exception.what();
    }
    return result;
}

std::string scoreClipSourceFingerprint(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view clipId,
    std::uint32_t sampleRate,
    std::string& error)
{
    return talas::project::projectScoreClipSourceFingerprint(
        document, registry, clipId, sampleRate, error);
}

[[nodiscard]] static ScoreClipMaterializationResult
materializeProjectScoreClipsImpl(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    ScoreClipMaterializationOptions options,
    std::string_view requestedClipId,
    bool onlySharingTracks,
    bool onlyInputTracks = false)
{
    ScoreClipMaterializationResult result;
    if (sourceSettings.preparedScore == nullptr)
    {
        result.error = "Score clip materialization requires an immutable prepared schedule";
        return result;
    }
    try
    {
        std::size_t aggregateSamples = 0;
        std::size_t aggregateRenderSamples = 0;
        const auto clips = sourceSettings.preparedScore->clips();
        const auto tracks = sourceSettings.preparedScore->tracks();
        result.clips.reserve(requestedClipId.empty() ? clips.size() : 1U);
        std::vector<PreparedScoreClipAudioPtr> orderedResults(clips.size());
        // Format 156: shared across the tracks,
        // every track's recordings are in one pool; else each sharing
        // track keeps its own.
        const auto acrossTracks = sourceSettings.preparedScore->sharingMemoryWritersAcrossTracks();
        const auto poolOf = [acrossTracks](std::uint32_t trackIndex) -> std::size_t {
            return acrossTracks ? 0U : trackIndex;
        };
        std::vector<std::vector<TableResource>> sharedByTrack(tracks.size());
        std::vector<std::uint64_t> lastPublishedEnd(tracks.size(), 0);
        std::vector<bool> memoryParticipant(clips.size(), false);
        std::vector<bool> holdsWriter(clips.size(), false);
        if (onlySharingTracks || acrossTracks)
        {
            for (std::size_t index = 0; index < clips.size(); ++index)
            {
                if (clips[index].trackIndex >= tracks.size()
                    || !tracks[clips[index].trackIndex]
                        .sharingMemoryWriters)
                    continue;
                std::string inspectionError;
                const auto sourceDocument = isolatedScoreClipSourceDocument(
                    document, clips[index], tracks[clips[index].trackIndex],
                    inspectionError);
                if (!sourceDocument)
                {
                    result.error = std::move(inspectionError);
                    return result;
                }
                const auto expanded = talas::project::expandProjectSounds(
                    *sourceDocument, registry);
                if (!expanded.ok())
                {
                    result.error = expanded.validation.errors.empty()
                        ? "Score clip source expansion failed"
                        : expanded.validation.errors.front().message;
                    return result;
                }
                memoryParticipant[index] = std::ranges::any_of(
                    expanded.document.graph.nodes,
                    [](const talas::project::ProjectNode& node) {
                        if (node.typeId == "talas.memoryWriter")
                            return true;
                        return (node.typeId == "talas.sample"
                                || node.typeId == "talas.sampleCloud")
                            && std::ranges::any_of(
                                node.parameters,
                                [](const talas::project::ProjectParameterExpression& parameter) {
                                    return parameter.parameterId
                                            == "fromMemoryWriter"
                                        && parameter.boolean == true;
                                });
                    });
                holdsWriter[index] = std::ranges::any_of(expanded.document.graph.nodes,
                    [](const talas::project::ProjectNode& node) { return node.typeId == "talas.memoryWriter"; });
            }
        }
        // The clips on the tracks feeding a
        // track's input render before the clips on that track.
        const auto inputDepth = [&](std::size_t index) {
            return clips[index].trackIndex < tracks.size() ? tracks[clips[index].trackIndex].inputDepth : 0U;
        };
        std::vector<std::size_t> chronologicalIndices(clips.size());
        std::iota(chronologicalIndices.begin(), chronologicalIndices.end(), 0);
        std::ranges::sort(chronologicalIndices, [&](std::size_t left, std::size_t right) {
            if (inputDepth(left) != inputDepth(right))
                return inputDepth(left) < inputDepth(right);
            if (clips[left].startFrame != clips[right].startFrame)
                return clips[left].startFrame < clips[right].startFrame;
            return clips[left].clipId < clips[right].clipId;
        });
        const auto requested = std::ranges::find(
            clips, requestedClipId, &PreparedScoreClip::clipId);
        const auto requestedIndex = requested == clips.end()
            ? clips.size()
            : static_cast<std::size_t>(requested - clips.begin());
        const auto selected = [&](std::size_t index) {
            if (onlySharingTracks)
                return static_cast<bool>(memoryParticipant[index]);
            if (onlyInputTracks)
                return clips[index].trackIndex < tracks.size()
                    && tracks[clips[index].trackIndex].inputTrackIndex.has_value();
            if (requestedClipId.empty() || clips[index].clipId == requestedClipId)
                return true;
            // Shared across the tracks, the clips writing or reading
            // a recording that start before the one asked for ends; a clip
            // asked for that neither writes nor reads one needs no other.
            if (acrossTracks && requestedIndex < clips.size())
                return memoryParticipant[requestedIndex] && memoryParticipant[index]
                    && clips[index].startFrame < clips[requestedIndex].endFrame;
            return requestedIndex < clips.size()
                && clips[requestedIndex].trackIndex < tracks.size()
                && tracks[clips[requestedIndex].trackIndex]
                    .sharingMemoryWriters
                && clips[index].trackIndex
                    == clips[requestedIndex].trackIndex
                && clips[index].startFrame
                    < clips[requestedIndex].startFrame;
        };
        std::vector<bool> feedsSelected(clips.size(), false);
        {
            std::vector<bool> feederTrack(tracks.size(), false);
            std::vector<std::uint32_t> pending;
            for (std::size_t index = 0; index < clips.size(); ++index)
            {
                if (selected(index) && clips[index].trackIndex < tracks.size())
                    pending.push_back(clips[index].trackIndex);
            }
            while (!pending.empty())
            {
                const auto trackIndex = pending.back();
                pending.pop_back();
                for (const auto feeder : tracks[trackIndex].inputFeederTrackIndices)
                {
                    if (feeder < tracks.size() && !feederTrack[feeder])
                    {
                        feederTrack[feeder] = true;
                        pending.push_back(feeder);
                    }
                }
            }
            for (std::size_t index = 0; index < clips.size(); ++index)
                feedsSelected[index] = clips[index].trackIndex < tracks.size() && feederTrack[clips[index].trackIndex];
        }
        std::vector<PreparedScoreClipAudioPtr> preparedByClip(clips.size());
        std::vector<bool> renderedTogether(clips.size(), false);
        const auto beforePlaybackEnd = [&](std::size_t index) {
            return !sourceSettings.scorePlaybackEndFrameExclusive.has_value()
                || clips[index].startFrame < *sourceSettings.scorePlaybackEndFrameExclusive;
        };
        const auto frozenClip = [&](std::size_t index) {
            const auto source = std::ranges::find(
                document.timeline.score.clips, clips[index].clipId, &talas::project::ScoreClip::clipId);
            return source != document.timeline.score.clips.end() && source->renderArtifact.has_value()
                && source->renderArtifact->mode == talas::project::ScoreClipRenderMode::Frozen;
        };
        for (const auto index : chronologicalIndices)
        {
            if (renderedTogether[index])
                continue;
            if (!selected(index) && !feedsSelected[index])
                continue;
            const auto requestedOutput = selected(index)
                && (requestedClipId.empty() || clips[index].clipId == requestedClipId);
            if (sourceSettings.scorePlaybackEndFrameExclusive.has_value()
                && clips[index].startFrame
                    >= *sourceSettings.scorePlaybackEndFrameExclusive)
            {
                continue;
            }
            if (cancelled(options))
            {
                result.cancelled = true;
                result.error = "Score clip materialization cancelled";
                result.clips.clear();
                return result;
            }
            // Format 156: shared across the tracks,
            // a clip writing or reading a recording renders together with
            // the clips doing so that overlap it, on whatever track, writers
            // first, as a reader and its writer would in a Mixer.
            if (acrossTracks && memoryParticipant[index] && !frozenClip(index)
                && clips[index].trackIndex < tracks.size())
            {
                const auto takesPart = [&](std::size_t candidate) {
                    return memoryParticipant[candidate] && !renderedTogether[candidate]
                        && preparedByClip[candidate] == nullptr
                        && (selected(candidate) || feedsSelected[candidate])
                        && beforePlaybackEnd(candidate) && !frozenClip(candidate)
                        && clips[candidate].trackIndex < tracks.size();
                };
                std::vector<std::size_t> group{index};
                auto groupEnd = clips[index].endFrame;
                for (auto position = std::ranges::find(chronologicalIndices, index) + 1;
                     position != chronologicalIndices.end(); ++position)
                {
                    if (inputDepth(*position) != inputDepth(index))
                        break;
                    if (!takesPart(*position))
                        continue;
                    if (clips[*position].startFrame >= groupEnd)
                        break;
                    group.push_back(*position);
                    groupEnd = std::max(groupEnd, clips[*position].endFrame);
                }
                if (group.size() > 1)
                {
                    std::ranges::stable_sort(group, [&](std::size_t left, std::size_t right) {
                        return holdsWriter[left] && !holdsWriter[right];
                    });
                    std::vector<std::string> fingerprints;
                    std::vector<std::uint8_t> preFader;
                    for (const auto member : group)
                    {
                        fingerprints.push_back(scoreClipSourceFingerprint(document, registry, clips[member].clipId,
                            sourceSettings.preparedScore->binding().sampleRate, result.error));
                        if (fingerprints.back().empty())
                        {
                            result.clips.clear();
                            return result;
                        }
                        preFader.push_back(options.preFader && selected(member)
                            && (requestedClipId.empty() || clips[member].clipId == requestedClipId));
                    }
                    if (!sharedByTrack[poolOf(0)].empty() && lastPublishedEnd[poolOf(0)] > clips[index].startFrame)
                    {
                        result.error = "Overlapping Score clips cannot read a future MemoryWriter recording";
                        result.clips.clear();
                        return result;
                    }
                    auto clipSettings = sourceSettings;
                    clipSettings.sharedMemoryWriterTables = sharedByTrack[poolOf(0)];
                    clipSettings.captureMemoryWriterRecordings = true;
                    auto captured = std::vector<TableResource>{};
                    bool renderCancelled = false;
                    auto audio = renderLiveClipsTogether(document, registry, clipSettings,
                        *sourceSettings.preparedScore, group, fingerprints, preFader, options, aggregateSamples,
                        aggregateRenderSamples, preparedByClip, captured, renderCancelled, result.error);
                    if (audio.size() != group.size())
                    {
                        result.cancelled = renderCancelled;
                        if (renderCancelled)
                            result.error = "Score clip materialization cancelled";
                        else if (result.error.empty())
                            result.error = "Score clip materialization failed";
                        result.clips.clear();
                        return result;
                    }
                    if (!captured.empty())
                    {
                        auto& shared = sharedByTrack[poolOf(0)];
                        for (auto& table : captured)
                        {
                            const auto existing = std::ranges::find(shared, table.id, &TableResource::id);
                            if (existing == shared.end())
                                shared.push_back(std::move(table));
                            else
                                *existing = std::move(table);
                        }
                        lastPublishedEnd[poolOf(0)] = groupEnd;
                    }
                    for (std::size_t member = 0; member < group.size(); ++member)
                    {
                        const auto clip = group[member];
                        renderedTogether[clip] = true;
                        preparedByClip[clip] = audio[member];
                        if (selected(clip) && (requestedClipId.empty() || clips[clip].clipId == requestedClipId))
                            orderedResults[clip] = options.preFader
                                ? std::move(audio[member])
                                : withRouteLevels(*sourceSettings.preparedScore, clip, std::move(audio[member]));
                    }
                    continue;
                }
            }
            const auto sourceClip = std::ranges::find(
                document.timeline.score.clips,
                clips[index].clipId,
                &talas::project::ScoreClip::clipId);
            if (sourceClip == document.timeline.score.clips.end())
            {
                result.error = "Score clip materialization lost clip '"
                    + clips[index].clipId + "'";
                result.clips.clear();
                return result;
            }
            if (clips[index].trackIndex >= tracks.size())
            {
                result.error = "Score clip materialization lost its prepared track";
                result.clips.clear();
                return result;
            }
            const auto& track = tracks[clips[index].trackIndex];
            // Shared across the tracks, a clip neither writing nor reading a
            // recording, or one frozen (which plays its frozen audio, reading
            // and recording nothing), plays as on a track that shares none.
            const auto sharesRecordings = track.sharingMemoryWriters
                && (!acrossTracks || (memoryParticipant[index] && !frozenClip(index)));
            if (sharesRecordings
                && !sharedByTrack[poolOf(clips[index].trackIndex)].empty()
                && lastPublishedEnd[poolOf(clips[index].trackIndex)]
                    > clips[index].startFrame)
            {
                result.error = "Overlapping Score clips cannot read a future MemoryWriter recording";
                result.clips.clear();
                return result;
            }
            const auto frameCount = clips[index].endFrame
                - clips[index].startFrame;
            if (aggregateSamples > options.maximumAggregateSamples
                || track.channelCount == 0
                || frameCount > std::numeric_limits<std::size_t>::max()
                || frameCount
                    > (options.maximumAggregateSamples - aggregateSamples)
                        / track.channelCount)
            {
                result.error = "Score clip artifacts exceed the bounded aggregate sample budget";
                result.clips.clear();
                return result;
            }
            const auto frozen = sourceClip->renderArtifact.has_value()
                && sourceClip->renderArtifact->mode
                    == talas::project::ScoreClipRenderMode::Frozen;
            auto fingerprint = frozen
                ? sourceClip->renderArtifact->sourceFingerprint
                : scoreClipSourceFingerprint(
                    document,
                    registry,
                    clips[index].clipId,
                    sourceSettings.preparedScore->binding().sampleRate,
                    result.error);
            if (fingerprint.empty())
            {
                result.clips.clear();
                return result;
            }
            const auto forceLiveForSharing = sharesRecordings
                && !frozen;
            auto prepared = options.forceLiveSource
                || forceLiveForSharing
                ? PreparedScoreClipAudioPtr{}
                : loadPersistedArtifact(
                    *sourceClip,
                    clips[index],
                    index,
                    sourceSettings,
                    fingerprint,
                    options,
                    result.error);
            const auto usedPersistedArtifact = prepared != nullptr;
            if (cancelled(options))
            {
                result.cancelled = true;
                result.error = "Score clip materialization cancelled";
                result.clips.clear();
                return result;
            }
            if (prepared == nullptr && result.error.empty())
            {
                bool renderCancelled = false;
                auto captured = std::vector<TableResource>{};
                auto clipSettings = sourceSettings;
                if (sharesRecordings)
                {
                    clipSettings.sharedMemoryWriterTables =
                        sharedByTrack[poolOf(clips[index].trackIndex)];
                    clipSettings.captureMemoryWriterRecordings = true;
                }
                // Only the clip asked for is recorded pre-fader; the clips
                // feeding its input are heard as they play.
                auto clipOptions = options;
                clipOptions.preFader = options.preFader && requestedOutput;
                prepared = renderLiveClip(
                    document,
                    registry,
                    clipSettings,
                    *sourceSettings.preparedScore,
                    index,
                    std::move(fingerprint),
                    clipOptions,
                    aggregateSamples,
                    aggregateRenderSamples,
                    sharesRecordings ? &captured : nullptr,
                    preparedByClip,
                    renderCancelled,
                    result.error);
                if (renderCancelled)
                    result.cancelled = true;
                if (prepared != nullptr && !captured.empty())
                {
                    auto& shared = sharedByTrack[poolOf(clips[index].trackIndex)];
                    for (auto& table : captured)
                    {
                        const auto existing = std::ranges::find(
                            shared, table.id, &TableResource::id);
                        if (existing == shared.end())
                            shared.push_back(std::move(table));
                        else
                            *existing = std::move(table);
                    }
                    lastPublishedEnd[poolOf(clips[index].trackIndex)] =
                        clips[index].endFrame;
                }
            }
            if (prepared == nullptr)
            {
                if (result.cancelled)
                {
                    result.error = "Score clip materialization cancelled";
                }
                else if (result.error.empty())
                    result.error = "Score clip materialization failed";
                result.clips.clear();
                return result;
            }
            if (usedPersistedArtifact)
            {
                aggregateSamples += static_cast<std::size_t>(frameCount)
                    * track.channelCount;
            }
            preparedByClip[index] = prepared;
            if (requestedOutput)
                orderedResults[index] = options.preFader ? std::move(prepared)
                                                         : withRouteLevels(*sourceSettings.preparedScore, index, std::move(prepared));
        }
        for (auto& prepared : orderedResults)
        {
            if (prepared != nullptr)
                result.clips.push_back(std::move(prepared));
        }
        if (!requestedClipId.empty() && result.clips.empty())
        {
            result.error = "Score clip materialization lost requested clip '"
                + std::string{requestedClipId} + "'";
        }
    }
    catch (const std::exception& exception)
    {
        result.clips.clear();
        result.error = "Score clip materialization failed: ";
        result.error += exception.what();
    }
    return result;
}

ScoreClipMaterializationResult materializeProjectScoreClips(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    ScoreClipMaterializationOptions options)
{
    return materializeProjectScoreClipsImpl(
        document, registry, sourceSettings, std::move(options), {}, false);
}

ScoreClipMaterializationResult
materializeProjectSharedMemoryWriterScoreClips(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    ScoreClipMaterializationOptions options)
{
    return materializeProjectScoreClipsImpl(
        document, registry, sourceSettings, std::move(options), {}, true);
}

ScoreClipMaterializationResult
materializeProjectInputTrackScoreClips(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    ScoreClipMaterializationOptions options)
{
    return materializeProjectScoreClipsImpl(
        document, registry, sourceSettings, std::move(options), {}, false, true);
}

ScoreClipMaterializationResult materializeProjectScoreClip(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const RenderSettings& sourceSettings,
    std::string_view clipId,
    ScoreClipMaterializationOptions options)
{
    if (clipId.empty())
        return {{}, "Score clip materialization requires an exact clip identity", false};
    return materializeProjectScoreClipsImpl(
        document, registry, sourceSettings, std::move(options), clipId, false);
}
}
