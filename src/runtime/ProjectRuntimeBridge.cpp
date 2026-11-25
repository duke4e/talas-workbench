#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/project/MidiDefaultMap.h"
#include "talas/project/ProjectGlobalMap.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/runtime/PreparedLiveSet.h"
#include "talas/runtime/PreparedMidiMapper.h"
#include "talas/runtime/ProjectRenderSettings.h"
#include "runtime/ProjectPreparedScoreGraph.h"
#include "runtime/component_bundle/ComponentBundleReplacementConfirmation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>

namespace talas::runtime
{
namespace
{
class ProducerGate
{
public:
    enum class Mode { Wait, TryOnce };

    ProducerGate(std::atomic_flag& gate, Mode mode) noexcept
        : gate_(gate)
    {
        acquired_ = !gate_.test_and_set(std::memory_order_acquire);
        while (!acquired_ && mode == Mode::Wait)
        {
            std::this_thread::yield();
            acquired_ = !gate_.test_and_set(std::memory_order_acquire);
        }
    }
    ~ProducerGate()
    {
        if (acquired_)
            gate_.clear(std::memory_order_release);
    }
    ProducerGate(const ProducerGate&) = delete;
    ProducerGate& operator=(const ProducerGate&) = delete;

    explicit operator bool() const noexcept
    {
        return acquired_;
    }

private:
    std::atomic_flag& gate_;
    bool acquired_ = false;
};
}

struct ProjectRuntimeBridge::TransportRelocationRequest
{
    std::unique_ptr<RealtimeGraphHost::PreparedTransportRelocation> sources;
    std::uint64_t stageId = 0, midiEpoch = 0, mtcEpoch = 0, resetEpoch = 0;
};

ProjectRuntimeBridge::~ProjectRuntimeBridge()
{
    delete pendingTransportRelocation_.exchange(nullptr, std::memory_order_acq_rel);
    (void) collectRetiredTransportRelocations();
}

bool ProjectRuntimeBridge::collectRetiredTransportRelocations() noexcept
{
    bool collected = false;
    for (auto& slot : retiredTransportRelocations_)
        if (auto* retired = slot.exchange(nullptr, std::memory_order_acq_rel))
        { delete retired; collected = true; }
    return collected;
}

void ProjectRuntimeBridge::adoptInternalTransportRelocation() noexcept
{
    // Two retirement slots cover the request currently being consumed and a
    // new request published while its control producer was preparing. Control
    // collects before every publication. No callback destroys a source owner.
    auto slot = std::find_if(retiredTransportRelocations_.begin(), retiredTransportRelocations_.end(),
        [](const auto& value) { return value.load(std::memory_order_acquire) == nullptr; });
    if (slot == retiredTransportRelocations_.end()) return;
    auto* request = pendingTransportRelocation_.exchange(nullptr, std::memory_order_acq_rel);
    if (!request) return;
    if (midiClockAuthorityGeneration_.load(std::memory_order_acquire) == 0
        && mtcAuthorityGeneration_.load(std::memory_order_acquire) == 0
        && midiClockStopRequestGeneration_.load(std::memory_order_acquire) == request->midiEpoch
        && mtcStopRequestGeneration_.load(std::memory_order_acquire) == request->mtcEpoch
        && internalTransportResetRequest_.load(std::memory_order_acquire) == request->resetEpoch)
    {
        (void) graphHost_.installTransportRelocation(*request->sources, request->stageId);
    }
    slot->store(request, std::memory_order_release);
}

namespace
{
void addSaturatedBounded(
    std::atomic<std::uint64_t>& counter,
    std::uint64_t amount = 1) noexcept
{
    for (std::size_t attempt = 0; attempt < 8; ++attempt)
    {
        auto observed = counter.load(std::memory_order_relaxed);
        const auto next = amount
                > std::numeric_limits<std::uint64_t>::max() - observed
            ? std::numeric_limits<std::uint64_t>::max()
            : observed + amount;
        if (next == observed
            || counter.compare_exchange_weak(
                observed,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed))
        {
            return;
        }
    }
}

std::uint8_t controllerRecordingTerminalPriority(
    talas::project::ControllerRecordingTerminal terminal) noexcept
{
    using Terminal = talas::project::ControllerRecordingTerminal;
    switch (terminal)
    {
    case Terminal::Completed:
        return 1;
    case Terminal::PunchComplete:
        return 2;
    case Terminal::Cancelled:
        return 3;
    case Terminal::Seeked:
        return 4;
    case Terminal::SourceLost:
        return 5;
    case Terminal::DeviceChanged:
        return 6;
    case Terminal::StageChanged:
        return 7;
    case Terminal::CapacityExceeded:
        return 8;
    }
    return 8;
}

constexpr std::uint16_t controllerRecordingTerminalCompletedBit = 1U << 15U;
// A session sends at most 4096 terminal events a callback
// (RenderSettings::maxRealtimeTerminalEvents); a global controller's event
// name is far shorter than this.
constexpr std::size_t maximumTerminalSegmentEvents = 4096;
constexpr std::size_t maximumTerminalSegmentNameBytes = 4096;
constexpr std::uint64_t maximumControllerRecordingTakeGeneration =
    std::numeric_limits<std::uint64_t>::max() >> 16U;
constexpr std::size_t maximumControllerRecordingTerminalCasAttempts = 8;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

[[nodiscard]] constexpr std::uint16_t controllerRecordingTerminalBit(
    talas::project::ControllerRecordingTerminal terminal) noexcept
{
    return static_cast<std::uint16_t>(
        1U << controllerRecordingTerminalPriority(terminal));
}

talas::project::ControllerRecordingTerminal controllerRecordingTerminalFromState(
    std::uint64_t state) noexcept
{
    using Terminal = talas::project::ControllerRecordingTerminal;
    for (auto priority = std::uint8_t{8}; priority > 0; --priority)
    {
        if ((state & (std::uint16_t{1} << priority)) == 0)
            continue;
        switch (priority)
        {
        case 1:
            return Terminal::Completed;
        case 2:
            return Terminal::PunchComplete;
        case 3:
            return Terminal::Cancelled;
        case 4:
            return Terminal::Seeked;
        case 5:
            return Terminal::SourceLost;
        case 6:
            return Terminal::DeviceChanged;
        case 7:
            return Terminal::StageChanged;
        default:
            return Terminal::CapacityExceeded;
        }
    }
    return Terminal::CapacityExceeded;
}

[[nodiscard]] constexpr std::uint64_t controllerRecordingTerminalState(
    std::uint64_t takeGeneration,
    std::uint16_t terminalBits) noexcept
{
    return (takeGeneration << 16U) | terminalBits;
}

[[nodiscard]] constexpr std::uint64_t controllerRecordingTerminalGeneration(
    std::uint64_t state) noexcept
{
    return state >> 16U;
}

[[nodiscard]] std::uint64_t saturatedSum(
    std::uint64_t left,
    std::uint64_t right) noexcept
{
    return right > std::numeric_limits<std::uint64_t>::max() - left
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}

inline constexpr std::uint64_t midiClockPhaseOne =
    std::uint64_t{1} << midiClockPhaseFractionBits;

[[nodiscard]] std::optional<std::uint64_t> midiClockTickRateQ24(
    std::uint64_t intervalNanoseconds,
    double sampleRate) noexcept
{
    if (intervalNanoseconds == 0
        || !std::isfinite(sampleRate)
        || sampleRate < 8000.0
        || sampleRate > 384000.0
        || std::floor(sampleRate) != sampleRate)
    {
        return std::nullopt;
    }
    const auto integralRate = static_cast<std::uint64_t>(sampleRate);
    if (intervalNanoseconds
        > std::numeric_limits<std::uint64_t>::max() / integralRate)
    {
        return std::nullopt;
    }
    const auto denominator = intervalNanoseconds * integralRate;
    constexpr std::uint64_t numerator =
        static_cast<std::uint64_t>(midiClockProjectTicksPerPulse)
        * midiClockNanosecondsPerSecond * midiClockPhaseOne;
    return (numerator
               + denominator / midiClockRoundToNearestDivisor)
        / denominator;
}

[[nodiscard]] bool correctMidiClockPhase(
    std::uint64_t targetQ24,
    std::uint64_t& phaseQ24) noexcept
{
    constexpr auto maximumCorrection =
        std::uint64_t{midiClockMaximumMusicalCorrectionTicks}
        * midiClockPhaseOne;
    if (targetQ24 >= phaseQ24)
    {
        const auto correction = std::min(
            (targetQ24 - phaseQ24) / midiClockMusicalPhaseGainDivisor,
            maximumCorrection);
        if (correction
            > std::numeric_limits<std::uint64_t>::max() - phaseQ24)
        {
            return false;
        }
        phaseQ24 += correction;
    }
    else
    {
        const auto correction = std::min(
            (phaseQ24 - targetQ24) / midiClockMusicalPhaseGainDivisor,
            maximumCorrection);
        phaseQ24 -= correction;
    }
    return true;
}

[[nodiscard]] const talas::project::ProjectSnapshotBank*
findPerformanceBank(
    const talas::project::ProjectDocument& document,
    std::string_view bankId)
{
    const auto found = std::ranges::find(
        document.snapshotLibrary.banks,
        bankId,
        &talas::project::ProjectSnapshotBank::bankId);
    return found == document.snapshotLibrary.banks.end()
        ? nullptr
        : std::addressof(*found);
}

[[nodiscard]] const talas::project::ProjectSnapshot*
findPerformanceSnapshot(
    const talas::project::ProjectSnapshotBank& bank,
    std::string_view snapshotId)
{
    const auto found = std::ranges::find(
        bank.snapshots,
        snapshotId,
        &talas::project::ProjectSnapshot::snapshotId);
    return found == bank.snapshots.end()
        ? nullptr
        : std::addressof(*found);
}

[[nodiscard]] SnapshotPerformanceActionResult
snapshotPerformanceFailure(
    talas::project::SnapshotDiagnosticCode code,
    std::string message,
    const talas::project::SnapshotTargetIdentity* target = nullptr)
{
    SnapshotPerformanceActionResult result;
    result.message = message;
    result.diagnostics.push_back({
        code,
        talas::project::SnapshotDiagnosticSeverity::Error,
        "$.snapshotPerformance",
        target == nullptr
            ? std::string{}
            : talas::project::snapshotTargetIdentityString(*target),
        std::move(message),
    });
    return result;
}

void sortSnapshotPerformanceValues(
    std::vector<talas::project::SnapshotValueEntry>& values)
{
    std::ranges::sort(
        values,
        {},
        [](const auto& value) {
            return talas::project::snapshotTargetIdentityString(
                value.target);
        });
}

void carryForwardLastAvailableStreamMetrics(
    RealtimeMetricsSnapshotV1& current,
    const RealtimeMetricsSnapshotV1& previous) noexcept
{
    if (current.streamHealthAvailable != 0
        || previous.streamHealthAvailable == 0)
    {
        return;
    }
    current.streamHealthAvailable = 1;
    current.streamConsumerCount = previous.streamConsumerCount;
    current.streamDecodedBlocks = previous.streamDecodedBlocks;
    current.streamDecodeFailures = previous.streamDecodeFailures;
    current.streamUnderrunEvents = previous.streamUnderrunEvents;
    current.streamMissingFrames = previous.streamMissingFrames;
    current.streamRequestRejections =
        previous.streamRequestRejections;
    current.streamResponseQueueFull = previous.streamResponseQueueFull;
    current.streamStaleResponseBlocks =
        previous.streamStaleResponseBlocks;
}

struct ScoreGraphEndpoint
{
    std::string nodeId;
    std::string portId;
};

[[nodiscard]] talas::project::ProjectNode scoreNode(
    std::string id,
    std::string typeId)
{
    talas::project::ProjectNode node;
    node.instanceId = std::move(id);
    node.typeId = std::move(typeId);
    return node;
}

void addScoreExpression(
    talas::project::ProjectNode& node,
    std::string id,
    std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    node.parameters.push_back(std::move(parameter));
}

void addScoreResource(
    talas::project::ProjectNode& node,
    std::string id,
    std::string resource)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
    node.parameters.push_back(std::move(parameter));
}

[[nodiscard]] PreparedScoreGraphProjectionResult
projectPreparedScoreGraphImpl(
    const talas::project::ProjectDocument& source,
    const PreparedScoreSchedule& schedule,
    RenderSettings& destinationSettings, bool liveSources)
{
    PreparedScoreGraphProjectionResult result;
    result.document = source;
    result.document.graph = {};
    // Live Sets and Tools are prepared against the authored document by the
    // bridge as independent stage authorities. The Score projection replaces
    // the root graph with immutable clip players, so retaining Live Set source
    // instance targets here would reinterpret them against the synthetic graph
    // and reject an otherwise valid combined project.
    result.document.liveSetLibrary = {};
    // The immutable schedule and clip-audio table in RenderSettings are the
    // only Score authorities after projection. Keeping authored clips beside
    // the synthetic graph would make exact sourceInstanceId references point
    // at root nodes that the projection intentionally replaced.
    result.document.timeline.score = {};
    auto settings = destinationSettings;
    const auto clips = schedule.clips();
    const auto tracks = schedule.tracks();
    std::vector<std::vector<std::vector<ScoreGraphEndpoint>>> trackSources;
    trackSources.resize(tracks.size());
    for (std::size_t index = 0; index < tracks.size(); ++index)
        trackSources[index].resize(tracks[index].channelCount);

    const auto findPreparedClipAudio = [&](std::string_view clipId) {
        return std::ranges::find_if(
            settings.preparedScoreClipAudio,
            [&](const auto& candidate) {
                return candidate != nullptr
                    && candidate->clipId == clipId;
            });
    };
    std::unordered_set<std::string_view> preparedAudioIds;
    preparedAudioIds.reserve(settings.preparedScoreClipAudio.size());
    for (const auto& audio : settings.preparedScoreClipAudio)
    {
        if (audio == nullptr || !audio->valid()
            || audio->binding != schedule.binding()
            || !preparedAudioIds.insert(audio->clipId).second
            || std::ranges::find(
                    clips, audio->clipId, &PreparedScoreClip::clipId)
                == clips.end())
        {
            result.error = "Prepared Score clip audio table has stale, duplicate, or unknown authority";
            return result;
        }
    }

    for (std::size_t clipIndex = 0; clipIndex < clips.size(); ++clipIndex)
    {
        const auto& clip = clips[clipIndex];
        if (!clip.audible
            || (settings.scorePlaybackEndFrameExclusive.has_value()
                && clip.startFrame
                    >= *settings.scorePlaybackEndFrameExclusive))
            continue;
        const auto channelCount = tracks[clip.trackIndex].channelCount;
        if (liveSources && findPreparedClipAudio(clip.clipId) == settings.preparedScoreClipAudio.end())
        {
            for (std::size_t channel = 0; channel < channelCount; ++channel)
            {
                const auto id = "scoreClipPlayer_" + std::to_string(clipIndex) + "_" + std::to_string(channel);
                result.document.graph.nodes.push_back(scoreNode(id, "talas.audioInput"));
                trackSources[clip.trackIndex][channel].push_back({id, "audioOut"});
            }
            continue;
        }
        const auto preparedAudio = findPreparedClipAudio(clip.clipId);
        if (preparedAudio == settings.preparedScoreClipAudio.end())
        {
            result.error = "Prepared Score clip audio table is incomplete for '"
                + clip.clipId + "'";
            return result;
        }
        if (!(*preparedAudio)->valid()
            || (*preparedAudio)->binding != schedule.binding()
            || (*preparedAudio)->sampleRate
                != schedule.binding().sampleRate
            || (*preparedAudio)->startFrame != clip.startFrame
            || (*preparedAudio)->frameCount
                != clip.endFrame - clip.startFrame
            || (*preparedAudio)->channels.size() != channelCount)
        {
            result.error = "Prepared Score clip audio binding is stale for '"
                + clip.clipId + "'";
            return result;
        }
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            const auto resourceId = "score-clip-audio-"
                + std::to_string(clipIndex) + "-"
                + std::to_string(channel);
            auto player = scoreNode(
                "scoreClipPlayer_" + std::to_string(clipIndex)
                    + "_" + std::to_string(channel),
                "talas.diskPlayer");
            addScoreResource(player, "fileName", resourceId);
            result.document.graph.nodes.push_back(std::move(player));

            talas::project::ProjectAsset asset;
            asset.id = resourceId;
            asset.kind = talas::project::ProjectAssetKind::Audio;
            asset.path = resourceId + ".wav";
            asset.contentHash = (*preparedAudio)->sourceFingerprint;
            asset.channelCount = 1;
            asset.sampleRate = (*preparedAudio)->sampleRate;
            asset.frameCount = (*preparedAudio)->frameCount;
            asset.encoding = "pcm16-wav";
            asset.generation = 1;
            asset.provenance = "internal-score-clip-audio";
            result.document.assets.push_back(std::move(asset));

            talas::core::ResourceSnapshot snapshot;
            snapshot.id = resourceId;
            snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
            snapshot.generation = 1;
            snapshot.contentHash = (*preparedAudio)->sourceFingerprint;
            snapshot.channelCount = 1;
            snapshot.sampleRate = (*preparedAudio)->sampleRate;
            snapshot.frameCount = (*preparedAudio)->frameCount;
            snapshot.encoding = "pcm16-wav";
            settings.resourceSnapshots.push_back(
                std::make_shared<const talas::core::ResourceSnapshot>(
                    std::move(snapshot)));
            trackSources[clip.trackIndex][channel].push_back({
                "scoreClipPlayer_" + std::to_string(clipIndex)
                    + "_" + std::to_string(channel),
                "audioOut",
            });
        }
    }

    std::vector<std::vector<ScoreGraphEndpoint>> masterSources;
    const auto master = std::ranges::find_if(
        tracks,
        [](const PreparedScoreTrack& track) {
            return track.kind == talas::project::ScoreTrackKind::Master;
        });
    if (master == tracks.end())
    {
        result.error = "Prepared Score has no Master route";
        return result;
    }
    masterSources.resize(master->channelCount);
    for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex)
    {
        // A track with No output is heard only
        // through the submixes it sends to.
        if (!tracks[trackIndex].audible || tracks[trackIndex].noOutput)
            continue;
        // A track hard-wired to output
        // channels, its channel k to the (k mod n)th of them.
        if (const auto& wired = tracks[trackIndex].outputChannels; !wired.empty())
        {
            for (std::size_t channel = 0; channel < trackSources[trackIndex].size(); ++channel)
            {
                const auto output = wired[channel % wired.size()];
                if (output >= masterSources.size())
                    continue;
                const auto& sources = trackSources[trackIndex][channel];
                masterSources[output].insert(masterSources[output].end(), sources.begin(), sources.end());
            }
            continue;
        }
        for (std::size_t channel = 0; channel < masterSources.size(); ++channel)
        {
            auto& destination = masterSources[channel];
            auto& sources = trackSources[trackIndex][channel];
            destination.insert(
                destination.end(),
                std::make_move_iterator(sources.begin()),
                std::make_move_iterator(sources.end()));
        }
    }

    for (std::size_t channel = 0; channel < masterSources.size(); ++channel)
    {
        auto& sources = masterSources[channel];
        if (sources.empty())
        {
            const auto silenceId = "scoreSilence_" + std::to_string(channel);
            auto silence = scoreNode(silenceId, "talas.oscillator");
            addScoreExpression(silence, "amplitude", "0.0");
            result.document.graph.nodes.push_back(std::move(silence));
            sources.push_back({silenceId, "audioOut"});
        }
        std::size_t level = 0;
        while (sources.size() > 1)
        {
            std::vector<ScoreGraphEndpoint> next;
            next.reserve((sources.size() + 1U) / 2U);
            for (std::size_t index = 0; index < sources.size(); index += 2)
            {
                if (index + 1U == sources.size())
                {
                    next.push_back(std::move(sources[index]));
                    continue;
                }
                const auto mixerId = "scoreMix_" + std::to_string(channel)
                    + "_" + std::to_string(level) + "_"
                    + std::to_string(index / 2U);
                result.document.graph.nodes.push_back(
                    scoreNode(mixerId, "talas.mixer"));
                result.document.graph.connections.push_back({
                    sources[index].nodeId, sources[index].portId,
                    mixerId, "in1"});
                result.document.graph.connections.push_back({
                    sources[index + 1U].nodeId,
                    sources[index + 1U].portId,
                    mixerId,
                    "in2"});
                next.push_back({mixerId, "audioOut"});
            }
            sources = std::move(next);
            ++level;
        }
    }

    std::string terminalType;
    std::vector<std::string> terminalPorts;
    switch (masterSources.size())
    {
    case 1:
        terminalType = "talas.audioOutput";
        terminalPorts = {"audioIn"};
        break;
    case 2:
        terminalType = "talas.audioOutput";
        terminalPorts = {"audioIn", "rightIn"};
        break;
    case 4:
        terminalType = "talas.output4";
        terminalPorts = {"out1", "out2", "out3", "out4"};
        break;
    case 6:
        terminalType = "talas.output6";
        terminalPorts = {"out1", "out2", "out3", "out4", "out5", "out6"};
        break;
    case 8:
        terminalType = "talas.output8";
        terminalPorts = {"out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"};
        break;
    default:
        result.error = "Prepared Score Master requires 1, 2, 4, 6, or 8 channels";
        return result;
    }
    result.document.graph.nodes.push_back(
        scoreNode("scoreOutput", terminalType));
    for (std::size_t channel = 0; channel < masterSources.size(); ++channel)
    {
        result.document.graph.connections.push_back({
            masterSources[channel].front().nodeId,
            masterSources[channel].front().portId,
            "scoreOutput",
            terminalPorts[channel],
        });
    }
    std::ranges::stable_sort(
        settings.eventValueChanges,
        [](const EventValueChange& left, const EventValueChange& right) {
            if (left.frame != right.frame)
                return left.frame < right.frame;
            if (left.targetInstanceId != right.targetInstanceId)
                return left.targetInstanceId < right.targetInstanceId;
            return left.name < right.name;
        });
    std::size_t compacted = 0;
    for (std::size_t read = 0;
         read < settings.eventValueChanges.size();
         ++read)
    {
        auto& change = settings.eventValueChanges[read];
        if (compacted != 0)
        {
            auto& previous = settings.eventValueChanges[compacted - 1U];
            if (previous.frame == change.frame
                && previous.scopeId == change.scopeId
                && previous.targetInstanceId == change.targetInstanceId
                && previous.name == change.name)
            {
                previous = std::move(change);
                continue;
            }
        }
        if (compacted != read)
            settings.eventValueChanges[compacted] = std::move(change);
        ++compacted;
    }
    settings.eventValueChanges.resize(compacted);
    destinationSettings = std::move(settings);
    return result;
}

[[nodiscard]] std::optional<talas::project::SnapshotValueEntry>
preparedVariableSnapshotValue(
    const talas::project::SnapshotTargetIdentity& target,
    const PreparedVariableValueSnapshot& publication)
{
    if (target.kind != talas::project::SnapshotTargetKind::Variable)
        return std::nullopt;
    const talas::project::PreparedProjectVariableIdentity identity{
        target.variableOwnerKind
                == talas::project::SnapshotVariableOwnerKind::WorkspaceSession
            ? talas::project::ProjectVariableOwnerKind::WorkspaceSession
            : (target.instancePath.empty()
                    ? talas::project::ProjectVariableOwnerKind::Project
                    : talas::project::ProjectVariableOwnerKind::SoundDefinition),
        target.variableId,
        target.definitionId,
        target.instancePath,
    };
    const auto found = std::ranges::find(
        publication.values,
        identity,
        &PreparedVariableValue::identity);
    if (found == publication.values.end()
        || !std::isfinite(found->value.value))
    {
        return std::nullopt;
    }
    talas::project::SnapshotValueEntry result;
    result.target = target;
    result.value.kind = talas::project::SnapshotValueKind::Number;
    result.value.domain = found->value.domain;
    result.value.number = found->value.value;
    return result;
}

struct LiveSetPreparationContext
{
    const talas::project::ProjectDocument* document = nullptr;
    const talas::core::NodeRegistry* registry = nullptr;
    const RenderSettings* settings = nullptr;
    const std::unordered_map<std::string, SnapshotPreparedTarget>*
        snapshotTargets = nullptr;
    const std::unordered_map<std::string, talas::project::ProjectDocument>*
        compiledSounds = nullptr;
};

[[nodiscard]] bool prepareLiveSetSound(
    void* opaque,
    const talas::project::ProjectLiveSetSoundTarget& target,
    std::shared_ptr<const PreparedRuntimeGraph>& graph,
    std::string& error)
{
    const auto* context = static_cast<const LiveSetPreparationContext*>(opaque);
    if (context == nullptr || context->document == nullptr
        || context->registry == nullptr || context->settings == nullptr)
    {
        error = "Live Set Sound preparation context is unavailable";
        return false;
    }
    // A compiled Sound plays the program kept for it.
    const auto kept = context->compiledSounds == nullptr
        ? std::unordered_map<std::string, talas::project::ProjectDocument>::const_iterator{}
        : context->compiledSounds->find(target.soundDefinitionId);
    auto source = context->compiledSounds != nullptr
            && kept != context->compiledSounds->end()
        ? talas::project::ProjectSoundPreparationDocumentResult{kept->second, 1, {}}
        : talas::project::projectSoundPreparationDocument(
            *context->document,
            target.soundDefinitionId,
            target.sourceInstanceId.has_value()
                ? std::optional<std::string_view>{*target.sourceInstanceId}
                : std::optional<std::string_view>{});
    if (!source.ok())
    {
        error = std::move(source.error);
        return false;
    }
    auto sourceSettings = *context->settings;
    sourceSettings.preparedScore.reset();
    sourceSettings.preparedScoreClipAudio.clear();
    sourceSettings.scorePlaybackEndFrameExclusive.reset();
    sourceSettings.eventValueChanges.clear();
    auto settings = prepareProjectRenderSettings(
        source.document,
        *context->registry,
        std::move(sourceSettings));
    if (!settings.ok())
    {
        error = std::move(settings.error);
        return false;
    }
    auto compiled = compileProject(
        source.document,
        *context->registry,
        std::move(*settings.settings));
    if (!compiled.ok())
    {
        error = "Live Set Sound graph compilation failed";
        for (const auto& diagnostic : compiled.diagnostics)
            error += " | " + diagnostic.message;
        return false;
    }
    auto candidate = std::make_shared<PreparedRuntimeGraph>(
        std::move(*compiled.graph));
    RealtimeAudioEngine probe{*candidate};
    if (!probe.ready())
    {
        error = std::string{probe.error()};
        return false;
    }
    graph = std::move(candidate);
    return true;
}

[[nodiscard]] bool prepareLiveSetSnapshot(
    void* opaque,
    const talas::project::ProjectSnapshotBank& bank,
    const talas::project::ProjectSnapshot& snapshot,
    PreparedLiveSetSnapshotProgram& program,
    std::string& error)
{
    const auto* context = static_cast<const LiveSetPreparationContext*>(opaque);
    if (context == nullptr || context->document == nullptr
        || context->registry == nullptr || context->settings == nullptr
        || context->snapshotTargets == nullptr || snapshot.values.empty())
    {
        error = "Live Set Snapshot preparation context or values are unavailable";
        return false;
    }
    auto directOnly = true;
    for (const auto& value : snapshot.values)
    {
        const auto prepared = context->snapshotTargets->find(
            talas::project::snapshotTargetIdentityString(value.target));
        if (prepared == context->snapshotTargets->end()
            || prepared->second.target != value.target)
        {
            error = "Live Set Snapshot target is unavailable in the prepared stage: ";
            error += talas::project::snapshotTargetIdentityString(value.target);
            if (prepared != context->snapshotTargets->end())
            {
                error += " (resolved domain ";
                error += std::to_string(static_cast<int>(prepared->second.target.domain));
                error += ", requested domain ";
                error += std::to_string(static_cast<int>(value.target.domain));
                error += ")";
            }
            return false;
        }
        directOnly = directOnly
            && value.value.kind == talas::project::SnapshotValueKind::Number
            && (prepared->second.projection
                    == SnapshotPreparedProjection::DirectEventValues
                || prepared->second.projection
                    == SnapshotPreparedProjection::DirectVariable);
    }
    program.rampFrames = snapshotSmoothingFrames(
        bank.smoothingSeconds,
        context->settings->sampleRate);
    if (!directOnly)
    {
        auto compiled = compileProjectPerformanceOverlay(
            *context->document,
            *context->registry,
            *context->settings,
            snapshot.values);
        if (!compiled.ok())
        {
            error = "Live Set Snapshot overlay compilation failed";
            for (const auto& diagnostic : compiled.diagnostics)
                error += " | " + diagnostic.message;
            return false;
        }
        auto candidate = std::make_shared<PreparedRuntimeGraph>(
            std::move(*compiled.graph));
        RealtimeAudioEngine probe{*candidate};
        if (!probe.ready())
        {
            error = std::string{probe.error()};
            return false;
        }
        program.overlayGraph = std::move(candidate);
        return true;
    }

    program.eventValueUpdates.reserve(snapshot.values.size());
    program.variableUpdates.reserve(snapshot.values.size());
    for (const auto& value : snapshot.values)
    {
        const auto prepared = context->snapshotTargets->find(
            talas::project::snapshotTargetIdentityString(value.target));
        if (prepared->second.projection
            == SnapshotPreparedProjection::DirectVariable)
        {
            if (!prepared->second.variableTargetIndex.has_value()
                || std::ranges::any_of(
                    program.variableUpdates,
                    [&](const auto& existing) {
                        return existing.targetIndex
                            == *prepared->second.variableTargetIndex;
                    }))
            {
                error = "Live Set Snapshot variable target is invalid or duplicated";
                return false;
            }
            program.variableUpdates.push_back({
                *prepared->second.variableTargetIndex,
                value.value.number,
                prepared->second.domain,
            });
            continue;
        }
        if (prepared->second.eventValueTargets.empty())
        {
            error = "Live Set Snapshot Event target is empty";
            return false;
        }
        for (const auto& target : prepared->second.eventValueTargets)
        {
            if (program.eventValueUpdates.size()
                    + program.variableUpdates.size()
                    >= maximumSnapshotDirectEventValueUpdates
                || std::ranges::any_of(
                    program.eventValueUpdates,
                    [&](const auto& existing) {
                        return existing.targetIndex == target.targetIndex;
                    }))
            {
                error = "Live Set Snapshot Event target capacity or uniqueness failed";
                return false;
            }
            program.eventValueUpdates.push_back({
                target.targetIndex,
                value.value.number,
                target.domain,
            });
        }
    }
    return !program.eventValueUpdates.empty()
        || !program.variableUpdates.empty();
}
}

PreparedScoreGraphProjectionResult projectPreparedScoreGraph(
    const talas::project::ProjectDocument& document,
    const PreparedScoreSchedule& schedule,
    RenderSettings& settings, bool liveSources)
{
    return projectPreparedScoreGraphImpl(document, schedule, settings, liveSources);
}

ProjectRuntimeBridge::ProjectRuntimeBridge(talas::project::ProjectDocument document,
                                           const talas::core::NodeRegistry& registry,
                                           std::size_t controlIngressCapacity)
    : editor_(std::move(document), registry)
    , registry_(registry)
    , controlIngress_(controlIngressCapacity)
    , midiClockIngress_(controlIngressCapacity)
    , mtcIngress_(controlIngressCapacity)
    , liveSetIngress_(controlIngressCapacity)
    , liveSetScheduler_(controlIngressCapacity)
    , liveToolIngress_(std::clamp<std::size_t>(
          controlIngressCapacity,
          2U,
          talas::project::maximumRealtimeLiveToolTriggerQueue))
{
    const auto controlCapacity = controlIngress_.capacity();
    const auto callbackCapacity = controlCapacity
            <= std::numeric_limits<std::size_t>::max() / 2
        ? controlCapacity * 2
        : controlCapacity;
    controlDrainScratch_.reserve(callbackCapacity);
    scheduledControlPending_.reserve(controlCapacity);
    scheduledControlSequenceScratch_.reserve(callbackCapacity);
    typedEventScratch_.resize(callbackCapacity);
    typedEventDispatchScratch_.resize(callbackCapacity);
    typedEventCommandScratch_.resize(callbackCapacity);
    controllerRecordingEvents_ =
        std::make_unique<talas::project::ControllerRecordingEvent[]>(
            talas::project::maximumControllerRecordingEvents);
    ownedEventValueNames_.reserve(maxRegisteredEventValues);
    snapshotDirectUpdateScratch_.reserve(
        maximumSnapshotDirectEventValueUpdates);
    snapshotDirectVariableUpdateScratch_.reserve(
        maximumSnapshotDirectEventValueUpdates);
}

talas::project::ProjectEditor& ProjectRuntimeBridge::editor()
{
    return editor_;
}

const talas::project::ProjectEditor& ProjectRuntimeBridge::editor() const
{
    return editor_;
}

ProjectStageResult ProjectRuntimeBridge::compileAndStage(RenderSettings settings)
{
    if (settings.playedScoreClipIds.has_value())
    {
        // The marked Sounds compiled alone.
        auto played = editor_.document();
        played.timeline.score = talas::project::scoreWithPlayedClips(played.timeline.score, *settings.playedScoreClipIds);
        return compileAndStageDocumentImpl(
            played, std::move(settings), {}, 0, {}, nullptr, nullptr, nullptr, true);
    }
    return compileAndStageDocumentImpl(
        editor_.document(), std::move(settings), {}, 0, {}, nullptr,
        nullptr, nullptr, true);
}

ProjectStageResult ProjectRuntimeBridge::compileAndStageDocument(
    const talas::project::ProjectDocument& document,
    RenderSettings settings)
{
    return compileAndStageDocumentImpl(
        document, std::move(settings), {});
}

ProjectStageResult ProjectRuntimeBridge::compileAndStageIsolatedComponent(
    const talas::project::ProjectDocument& document,
    RenderSettings settings,
    std::shared_ptr<PreparedIsolatedComponentBinding> binding)
{
    return compileAndStageDocumentImpl(
        document, std::move(settings), std::move(binding));
}

ProjectStageResult
ProjectRuntimeBridge::compileAndStageIsolatedComponentReplacement(
    const talas::project::ProjectDocument& document,
    RenderSettings settings,
    std::shared_ptr<PreparedIsolatedComponentBinding> binding,
    std::uint64_t documentGeneration,
    std::string_view nodeId,
    const ComponentApprovalFingerprint& candidate,
    component_bundle::ComponentBundleReplacementReceipt&& receipt)
{
    if (binding == nullptr || documentGeneration == 0 || nodeId.empty())
    {
        return {
            ProjectStageOutcome::ReplacementRejected,
            std::nullopt,
            {},
            "Replacement stage is missing its exact preserved-node context",
        };
    }
    try
    {
        const auto originalNode = std::ranges::find(
            document.graph.nodes,
            nodeId,
            &talas::project::ProjectNode::instanceId);
        if (originalNode == document.graph.nodes.end()
            || !originalNode->preservedComponent.has_value())
        {
            return {
                ProjectStageOutcome::ReplacementRejected,
                std::nullopt,
                {},
                "Replacement preserved node is no longer available",
            };
        }
        const auto preservedDigest =
            component_bundle::calculateProjectPreservedComponentDigest(
                originalNode->typeId,
                *originalNode->preservedComponent);
        auto compilationDocument = document;
        auto compilationNode = std::ranges::find(
            compilationDocument.graph.nodes,
            nodeId,
            &talas::project::ProjectNode::instanceId);
        if (compilationNode == compilationDocument.graph.nodes.end()
            || !compilationNode->preservedComponent.has_value())
        {
            return {
                ProjectStageOutcome::ReplacementRejected,
                std::nullopt,
                {},
                "Replacement compilation projection lost its preserved node",
            };
        }
        compilationNode->preservedComponent->typeVersion =
            binding->contract().typeVersion;
        return compileAndStageDocumentImpl(
            compilationDocument,
            std::move(settings),
            std::move(binding),
            documentGeneration,
            nodeId,
            &candidate,
            &receipt,
            &preservedDigest,
            true);
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Replacement compilation projection allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Replacement compilation projection exceeded supported limits",
        };
    }
}

ProjectStageResult ProjectRuntimeBridge::compileAndStageDocumentImpl(
    const talas::project::ProjectDocument& document,
    RenderSettings settings,
    std::shared_ptr<PreparedIsolatedComponentBinding> binding,
    std::uint64_t replacementDocumentGeneration,
    std::string_view replacementNodeId,
    const ComponentApprovalFingerprint* replacementCandidate,
    component_bundle::ComponentBundleReplacementReceipt* replacementReceipt,
    const std::array<std::uint8_t, componentTrustDigestBytes>*
        replacementPreservedDigest,
    bool preserveVariableValueHistory)
{
    if (nextStageGeneration_
            == std::numeric_limits<std::uint64_t>::max()
        || nextEngineStageId_
            == std::numeric_limits<std::uint64_t>::max()
        || nextSnapshotProjectRevision_
            == std::numeric_limits<std::uint64_t>::max())
    {
        return {
            ProjectStageOutcome::StageRejected,
            std::nullopt,
            {},
            "Prepared stage identity space is exhausted",
        };
    }
    auto preparedSettings = prepareProjectRenderSettings(
        document,
        registry_,
        std::move(settings));
    if (!preparedSettings.ok())
    {
        return {
            ProjectStageOutcome::CompileFailed,
            std::nullopt,
            {},
            std::move(preparedSettings.error),
        };
    }
    settings = std::move(*preparedSettings.settings);
    std::shared_ptr<const PreparedScoreSchedule> stagedPreparedScore;
    if (!document.timeline.score.tracks.empty()
        || !document.timeline.score.clips.empty()
        || !document.timeline.score.automation.empty()
        || !document.timeline.score.markers.empty())
    {
        const auto integralSampleRate = std::floor(settings.sampleRate);
        if (!std::isfinite(settings.sampleRate)
            || settings.sampleRate <= 0.0
            || integralSampleRate != settings.sampleRate
            || integralSampleRate
                > static_cast<double>(
                    std::numeric_limits<std::uint32_t>::max()))
        {
            return {
                ProjectStageOutcome::CompileFailed,
                std::nullopt,
                {},
                "Prepared Score requires an exact integral sample rate",
            };
        }
        const auto scoreBinding = preparedScoreBindingForDocument(
            document,
            nextStageGeneration_,
            nextStageGeneration_,
            static_cast<std::uint32_t>(integralSampleRate),
            realtimeControlGeneration_.load(
                std::memory_order_acquire));
        auto preparedScore = prepareProjectScore(document, scoreBinding);
        if (!preparedScore.ok())
        {
            return {
                ProjectStageOutcome::CompileFailed,
                std::nullopt,
                {},
                std::move(preparedScore.error),
            };
        }
        stagedPreparedScore = std::move(preparedScore.schedule);
        settings.preparedScore = stagedPreparedScore;
    }
    decltype(acceptedResourceIdentities_)
        nextAcceptedResourceIdentities;
    std::vector<
        component_bundle::ComponentBundleReplacementResourceTransition>
        replacementTransitions;
    try
    {
        nextAcceptedResourceIdentities =
            acceptedResourceIdentities_;
        if (replacementReceipt != nullptr)
        {
            replacementTransitions.reserve(
                component_bundle::maximumComponentBundleReplacementTransitions);
        }
        for (const auto& snapshot : settings.resourceSnapshots)
        {
            if (snapshot == nullptr || snapshot->generation == 0)
                continue;
            const auto accepted = acceptedResourceIdentities_.find(
                snapshot->id);
            if (accepted != acceptedResourceIdentities_.end())
            {
                const auto identityChanged =
                    snapshot->kind != accepted->second.kind
                    || snapshot->generation != accepted->second.generation
                    || snapshot->contentHash
                        != accepted->second.contentHash;
                if (replacementReceipt != nullptr && identityChanged)
                {
                    if (replacementTransitions.size()
                        >= component_bundle::
                            maximumComponentBundleReplacementTransitions)
                    {
                        return {
                            ProjectStageOutcome::ReplacementRejected,
                            std::nullopt,
                            {},
                            "Replacement transition set exceeds the closed eight-resource limit",
                        };
                    }
                    const auto audioCrossKind =
                        snapshot->kind != accepted->second.kind
                        && (snapshot->kind
                                == talas::core::ResourceSnapshotKind::Audio
                            || accepted->second.kind
                                == talas::core::ResourceSnapshotKind::Audio);
                    replacementTransitions.push_back({
                        {
                            snapshot->id,
                            accepted->second.kind,
                            accepted->second.generation,
                            accepted->second.contentHash,
                        },
                        {
                            snapshot->id,
                            snapshot->kind,
                            snapshot->generation,
                            snapshot->contentHash,
                        },
                        audioCrossKind,
                    });
                }
                if (snapshot->kind != accepted->second.kind)
                {
                    const auto tableOrBinary = [](auto kind) {
                        return kind == talas::core::ResourceSnapshotKind::Table
                            || kind
                                == talas::core::ResourceSnapshotKind::Binary;
                    };
                    if ((!tableOrBinary(snapshot->kind)
                            || !tableOrBinary(accepted->second.kind))
                        && replacementReceipt == nullptr)
                    {
                        return {
                            ProjectStageOutcome::ResourceIdentityConflict,
                            std::nullopt,
                            {},
                            "Resource snapshot kind transition is not approved for '"
                                + snapshot->id + "'",
                        };
                    }
                    if (snapshot->generation
                        <= accepted->second.generation)
                    {
                        return {
                            ProjectStageOutcome::ResourceIdentityConflict,
                            std::nullopt,
                            {},
                            "Resource snapshot kind changed without a generation increment for '"
                                + snapshot->id + "'",
                        };
                    }
                }
                if (snapshot->generation
                    < accepted->second.generation)
                {
                    return {
                        ProjectStageOutcome::ResourceIdentityConflict,
                        std::nullopt,
                        {},
                        "Resource snapshot generation moved backwards for '"
                            + snapshot->id + "'",
                    };
                }
                if (snapshot->generation
                        == accepted->second.generation
                    && snapshot->contentHash
                        != accepted->second.contentHash)
                {
                    return {
                        ProjectStageOutcome::ResourceIdentityConflict,
                        std::nullopt,
                        {},
                        "Resource snapshot content changed without a generation increment for '"
                            + snapshot->id + "'",
                    };
                }
                if (snapshot->generation
                        > accepted->second.generation
                    && snapshot->contentHash
                        == accepted->second.contentHash)
                {
                    return {
                        ProjectStageOutcome::ResourceIdentityConflict,
                        std::nullopt,
                        {},
                        "Resource snapshot generation changed without new content for '"
                            + snapshot->id + "'",
                    };
                }
            }
            nextAcceptedResourceIdentities.insert_or_assign(
                snapshot->id,
                AcceptedResourceIdentity{
                    snapshot->kind,
                    snapshot->generation,
                    snapshot->contentHash,
                });
        }
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Resource identity ledger allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Resource identity ledger exceeded supported limits",
        };
    }
    std::optional<RenderSettings> stagedProjectRenderSettings;
    try
    {
        stagedProjectRenderSettings = settings;
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared project render-settings snapshot allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared project render-settings snapshot exceeded supported limits",
        };
    }
    auto compiled = binding == nullptr
        ? compileProject(document, registry_, std::move(settings))
        : compileProjectWithIsolatedComponent(
              document,
              registry_,
              std::move(settings),
              std::move(binding));
    if (!compiled.ok())
    {
        return {
            ProjectStageOutcome::CompileFailed,
            std::nullopt,
            std::move(compiled.diagnostics),
            "Project compilation failed",
        };
    }
    std::vector<EventValueBinding> realtimeBindings;
    try
    {
        realtimeBindings =
            compiled.graph->renderSettings().eventValues;
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared project render-settings snapshot allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared project render-settings snapshot exceeded supported limits",
        };
    }
    decltype(latestSnapshotPreparedTargets_)
        stagedSnapshotPreparedTargets;
    SnapshotPerformanceState stagedSnapshotPerformanceState;
    std::vector<talas::project::SnapshotValueEntry>
        stagedSnapshotBaselineValues;
    std::string stagedSnapshotAuthoringBytes;
    std::string stagedSnapshotStageMessage;
    const auto stagedSnapshotSampleRate =
        compiled.graph->renderSettings().sampleRate;
    std::optional<SnapshotExecutionTopologyFingerprint>
        stagedExecutionTopology;
    std::optional<SnapshotExecutionTopologyFingerprint>
        stagedAuthoringTopology;
    try
    {
        stagedSnapshotAuthoringBytes =
            talas::project::serializeProjectDocument(document, registry_);
        stagedExecutionTopology =
            snapshotPreparedExecutionTopologyFingerprint(
                *compiled.graph);
        stagedAuthoringTopology =
            snapshotExecutionTopologyFingerprint(document, registry_);
        const auto& manifest =
            compiled.graph->snapshotPreparedTargets();
        stagedSnapshotPreparedTargets.reserve(
            manifest.targets.size());
        stagedSnapshotBaselineValues.reserve(
            manifest.targets.size());
        for (const auto& target : manifest.targets)
        {
            const auto inserted =
                stagedSnapshotPreparedTargets.emplace(
                    talas::project::snapshotTargetIdentityString(
                        target.target),
                    target);
            if (!inserted.second)
            {
                return {
                    ProjectStageOutcome::EngineCreationFailed,
                    std::nullopt,
                    {},
                    "Prepared snapshot target manifest contains a duplicate stable identity",
                };
            }
            if (target.baselineValue.has_value())
            {
                stagedSnapshotBaselineValues.push_back({
                    target.target,
                    *target.baselineValue,
                });
            }
        }
        sortSnapshotPerformanceValues(
            stagedSnapshotBaselineValues);
        stagedSnapshotPerformanceState.currentValues =
            stagedSnapshotBaselineValues;
        stagedSnapshotPerformanceState.projectRevision =
            nextSnapshotProjectRevision_;
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared snapshot target map allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared snapshot target map exceeded supported limits",
        };
    }

    const auto canRebasePerformanceState =
        latestSnapshotExecutionTopology_.has_value()
        && latestSnapshotAuthoringTopology_.has_value()
        && stagedExecutionTopology.has_value()
        && stagedAuthoringTopology.has_value()
        && *latestSnapshotExecutionTopology_
            == *stagedExecutionTopology
        && *latestSnapshotAuthoringTopology_
            == *stagedAuthoringTopology;
    if (canRebasePerformanceState)
    {
        std::vector<talas::project::SnapshotValueEntry>
            carriedOverlayValues;
        std::size_t invalidatedTargetCount = 0;
        try
        {
            stagedSnapshotPerformanceState =
                snapshotPerformanceState_;
            stagedSnapshotPerformanceState.currentValues =
                stagedSnapshotBaselineValues;
            stagedSnapshotPerformanceState.projectRevision =
                nextSnapshotProjectRevision_;

            for (auto& stagedValue :
                 stagedSnapshotPerformanceState.currentValues)
            {
                const auto oldBaseline = std::ranges::find(
                    projectSnapshotBaselineValues_,
                    stagedValue.target,
                    &talas::project::SnapshotValueEntry::target);
                const auto oldCurrent = std::ranges::find(
                    snapshotPerformanceState_.currentValues,
                    stagedValue.target,
                    &talas::project::SnapshotValueEntry::target);
                if (oldBaseline
                        == projectSnapshotBaselineValues_.end()
                    || oldCurrent
                        == snapshotPerformanceState_.currentValues.end())
                {
                    continue;
                }
                if (oldBaseline->value == stagedValue.value)
                {
                    stagedValue.value = oldCurrent->value;
                    if (stagedValue.value != oldBaseline->value)
                        carriedOverlayValues.push_back(stagedValue);
                    continue;
                }
                if (oldCurrent->value != oldBaseline->value)
                    ++invalidatedTargetCount;
            }

            const auto* activeBank = findPerformanceBank(
                document,
                stagedSnapshotPerformanceState.activeBankId);
            if (activeBank == nullptr)
            {
                stagedSnapshotPerformanceState.activeBankId.clear();
                stagedSnapshotPerformanceState.activeSnapshotId.clear();
                stagedSnapshotPerformanceState.morphActive = false;
                stagedSnapshotPerformanceState
                    .morphLeftSnapshotId.clear();
                stagedSnapshotPerformanceState
                    .morphRightSnapshotId.clear();
                stagedSnapshotPerformanceState
                    .morphGestureStartValues.clear();
            }
            else
            {
                if (findPerformanceSnapshot(
                        *activeBank,
                        stagedSnapshotPerformanceState
                            .activeSnapshotId) == nullptr)
                {
                    stagedSnapshotPerformanceState
                        .activeSnapshotId.clear();
                }
                if (invalidatedTargetCount != 0
                    || findPerformanceSnapshot(
                           *activeBank,
                           stagedSnapshotPerformanceState
                               .morphLeftSnapshotId) == nullptr
                    || findPerformanceSnapshot(
                           *activeBank,
                           stagedSnapshotPerformanceState
                               .morphRightSnapshotId) == nullptr)
                {
                    stagedSnapshotPerformanceState.morphActive = false;
                    stagedSnapshotPerformanceState
                        .morphLeftSnapshotId.clear();
                    stagedSnapshotPerformanceState
                        .morphRightSnapshotId.clear();
                    stagedSnapshotPerformanceState
                        .morphGestureStartValues.clear();
                }
                if (invalidatedTargetCount != 0)
                {
                    stagedSnapshotPerformanceState
                        .activeSnapshotId.clear();
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            return {
                ProjectStageOutcome::EngineCreationFailed,
                std::nullopt,
                {},
                "Snapshot performance rebase allocation failed",
            };
        }
        catch (const std::length_error&)
        {
            return {
                ProjectStageOutcome::EngineCreationFailed,
                std::nullopt,
                {},
                "Snapshot performance rebase exceeded supported limits",
            };
        }

        if (!carriedOverlayValues.empty())
        {
            auto rebased = compileProjectPerformanceOverlay(
                document,
                registry_,
                *stagedProjectRenderSettings,
                carriedOverlayValues);
            const auto rebasedTopology = rebased.ok()
                ? std::optional{
                    snapshotPreparedExecutionTopologyFingerprint(
                        *rebased.graph)}
                : std::nullopt;
            if (rebased.ok()
                && rebasedTopology == stagedExecutionTopology)
            {
                compiled = std::move(rebased);
            }
            else
            {
                invalidatedTargetCount +=
                    carriedOverlayValues.size();
                carriedOverlayValues.clear();
                stagedSnapshotPerformanceState.currentValues =
                    stagedSnapshotBaselineValues;
                stagedSnapshotPerformanceState.activeSnapshotId.clear();
                stagedSnapshotPerformanceState.morphActive = false;
                stagedSnapshotPerformanceState
                    .morphLeftSnapshotId.clear();
                stagedSnapshotPerformanceState
                    .morphRightSnapshotId.clear();
                stagedSnapshotPerformanceState
                    .morphGestureStartValues.clear();
            }
        }
        if (!carriedOverlayValues.empty()
            || invalidatedTargetCount != 0)
        {
            stagedSnapshotStageMessage =
                "Snapshot performance rebase preserved "
                + std::to_string(carriedOverlayValues.size())
                + " live target(s); invalidated "
                + std::to_string(invalidatedTargetCount)
                + " conflicting target(s)";
        }
    }
    std::shared_ptr<const PreparedLiveSet> stagedPreparedLiveSet;
    std::shared_ptr<const PreparedLiveTools> stagedPreparedLiveTools;
    if (!document.liveSetLibrary.liveSets.empty()
        || !document.liveSetLibrary.tools.empty())
    {
        const auto integralSampleRate = std::floor(stagedSnapshotSampleRate);
        if (!std::isfinite(stagedSnapshotSampleRate)
            || stagedSnapshotSampleRate <= 0.0
            || integralSampleRate != stagedSnapshotSampleRate
            || integralSampleRate
                > static_cast<double>(
                    std::numeric_limits<std::uint32_t>::max()))
        {
            return {
                ProjectStageOutcome::CompileFailed,
                std::nullopt,
                {},
                "Prepared Live Set requires an exact integral sample rate",
            };
        }
        const auto documentAuthorityGeneration =
            replacementDocumentGeneration != 0
            ? replacementDocumentGeneration
            : preserveVariableValueHistory
                ? editor_.documentAuthorityGeneration()
                : nextStageGeneration_;
        LiveSetPreparationContext liveSetContext{
            &document,
            &registry_,
            &*stagedProjectRenderSettings,
            &stagedSnapshotPreparedTargets,
            &compiledLiveSetSounds_,
        };
        auto liveSet = prepareProjectLiveSet(
            document,
            {
                documentAuthorityGeneration,
                nextStageGeneration_,
                nextEngineStageId_,
                static_cast<std::uint32_t>(integralSampleRate),
                preserveVariableValueHistory,
            },
            {
                &liveSetContext,
                prepareLiveSetSound,
                prepareLiveSetSnapshot,
            });
        if (!liveSet.ok())
        {
            return {
                ProjectStageOutcome::EngineCreationFailed,
                std::nullopt,
                {},
                std::move(liveSet.error),
            };
        }
        stagedPreparedLiveSet = std::move(liveSet.prepared);
        if (!document.liveSetLibrary.tools.empty())
        {
            auto liveTools = prepareProjectLiveTools(
                document, *stagedPreparedLiveSet);
            if (!liveTools.ok())
            {
                return {
                    ProjectStageOutcome::EngineCreationFailed,
                    std::nullopt,
                    {},
                    std::move(liveTools.error),
                };
            }
            stagedPreparedLiveTools = std::move(liveTools.prepared);
        }
    }
    auto preparedSummary = makePreparedGraphSummary(*compiled.graph);
    if (!preparedSummary.ok())
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            std::move(preparedSummary.error),
        };
    }
    const auto stagedAnalysisCapture = compiled.graph->analysisCapture();
    const auto stagedScoreSourceAnalysis = compiled.graph->scoreSourceAnalysisCaptures();
    const auto stagedIsolatedBinding =
        compiled.graph->isolatedComponentBinding();
    talas::project::PreparedProjectVariableTable stagedPreparedVariables;
    talas::project::PreparedProjectSoundVariableTable stagedPreparedSoundVariables;
    PreparedVariableValueSnapshot stagedVariableTransferSource;
    PreparedSoundVariableValueSnapshot stagedSoundVariableTransferSource;
    try
    {
        stagedPreparedVariables = compiled.graph->preparedVariables();
        stagedPreparedSoundVariables = compiled.graph->preparedSoundVariables();
        if (preserveVariableValueHistory
            && latestVariableStageIsEditorDocument_
            && latestPreparedVariableStageGeneration_ != 0)
        {
            stagedVariableTransferSource = graphHost_.preparedVariableValues();
            stagedSoundVariableTransferSource = graphHost_.preparedSoundVariableValues();
        }
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared variable identity allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared variable identity exceeded supported limits",
        };
    }
    struct PendingRegistration
    {
        std::unique_ptr<std::string> name;
        talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    };
    std::vector<PendingRegistration> pendingRegistrations;
    pendingRegistrations.reserve(realtimeBindings.size());
    // A registered name this stage uses, in the domain it takes: one the
    // replaced project left in another domain is rebound when the stage is
    // accepted. Two domains for one name in one stage still conflict.
    struct RegisteredUse
    {
        std::size_t index = 0;
        talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
        bool rebinds = false;
    };
    std::vector<RegisteredUse> registeredUses;
    registeredUses.reserve(realtimeBindings.size());
    const auto documentReplacements = editor_.documentReplacements();
    for (const auto& binding : realtimeBindings)
    {
        if (const auto existing = realtimeEventValueId(binding.name); existing.has_value())
        {
            const auto index = static_cast<std::size_t>(*existing - 1);
            const auto use = std::ranges::find(registeredUses, index, &RegisteredUse::index);
            if (use != registeredUses.end())
            {
                if (use->domain == binding.domain)
                    continue;
            }
            else if (registeredEventValueDomains_[index].load(std::memory_order_relaxed) == binding.domain)
            {
                registeredUses.push_back({index, binding.domain, false});
                continue;
            }
            else if (registeredEventValueDocumentReplacements_[index] != documentReplacements)
            {
                registeredUses.push_back({index, binding.domain, true});
                continue;
            }
        }
        else
        {
            const auto pending = std::ranges::find_if(pendingRegistrations, [&](const auto& candidate) {
                return *candidate.name == binding.name;
            });
            if (pending == pendingRegistrations.end())
            {
                pendingRegistrations.push_back({std::make_unique<std::string>(binding.name), binding.domain});
                continue;
            }
            if (pending->domain == binding.domain)
                continue;
        }
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Realtime Event Value registry capacity or domain conflict",
        };
    }
    if (ownedEventValueNames_.size() + pendingRegistrations.size() > registeredEventValueNames_.size())
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Realtime Event Value registry capacity or domain conflict",
        };
    }

    std::unique_ptr<RealtimeAudioEngine> engine;
    try
    {
        engine = std::make_unique<RealtimeAudioEngine>(*compiled.graph);
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Realtime engine allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Realtime engine allocation exceeded supported limits",
        };
    }

    if (!engine->ready())
    {
        return {
            ProjectStageOutcome::EngineRejected,
            std::nullopt,
            {},
            std::string(engine->error()),
        };
    }

    const auto stagedOutputChannelCount = engine->outputChannelCount();
    const auto engineStageId = nextEngineStageId_;
    const auto stageGeneration = nextStageGeneration_;
    engine->setStageId(engineStageId);
    engine->setSnapshotGeneration(stageGeneration);
    try
    {
        if (!engine->setPreparedLiveSet(stagedPreparedLiveSet))
        {
            return {
                ProjectStageOutcome::EngineCreationFailed,
                std::nullopt,
                {},
                "Prepared Live Set runtime engine is unavailable",
            };
        }
        if (stagedPreparedLiveTools != nullptr)
        {
            auto boundTools = bindPreparedLiveToolTargets(
                *stagedPreparedLiveTools, *engine);
            if (!boundTools.ok())
            {
                return {
                    ProjectStageOutcome::EngineCreationFailed,
                    std::nullopt,
                    {},
                    std::move(boundTools.error),
                };
            }
            stagedPreparedLiveTools = std::move(boundTools.prepared);
        }
        if (!engine->setPreparedLiveTools(stagedPreparedLiveTools))
        {
            return {
                ProjectStageOutcome::EngineCreationFailed,
                std::nullopt,
                {},
                "Prepared Live Tool authority does not match its Live Set runtime",
            };
        }
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared Live Set runtime allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared Live Set runtime exceeded supported limits",
        };
    }
    if (stagedVariableTransferSource.publicationValid
        && stagedVariableTransferSource.stageId != 0
        && stagedVariableTransferSource.stageId
            == graphHost_.activeStageId())
    {
        try
        {
            std::vector<PreparedTypedEvent> transferEvents;
            std::vector<RealtimeTypedEventDispatchResult>
                transferResults;
            transferEvents.reserve(stagedPreparedVariables.cells.size() + stagedPreparedSoundVariables.cells.size()
            );
            for (const auto& cell : stagedPreparedVariables.cells)
            {
                const auto previous = std::ranges::find(
                    stagedVariableTransferSource.values,
                    cell.identity,
                    &PreparedVariableValue::identity);
                if (previous
                        == stagedVariableTransferSource.values.end()
                    || previous->value.domain != cell.domain
                    || previous->value.value == cell.initialValue)
                {
                    continue;
                }
                const auto target = engine->variableTarget(
                    cell.identity,
                    cell.domain,
                    stageGeneration);
                if (!target.has_value())
                    continue;
                PreparedTypedEvent event{};
                event.kind = TypedEventKind::VariableAssignment;
                event.target = *target;
                event.frameOffset = 0;
                event.sequence = transferEvents.size() + 1U;
                event.payload.variable = {
                    previous->value.value,
                    previous->value.domain,
                };
                transferEvents.push_back(event);
            }
            if (stagedSoundVariableTransferSource.publicationValid &&
                stagedSoundVariableTransferSource.stageId == stagedVariableTransferSource.stageId)
            {
                for (const auto& cell : stagedPreparedSoundVariables.cells)
                {
                    if (cell.identity.isVoiceLocal())
                    {
                        continue;
                    }
                    if (cell.candidateDefinitionIds.empty())
                        continue;
                    const auto previous = std::ranges::find(
                        stagedSoundVariableTransferSource.values,
                        cell.identity,
                        &PreparedSoundVariableValue::identity
                    );
                    if (previous == stagedSoundVariableTransferSource.values.end() ||
                        previous->selectedOrdinal >= cell.candidateDefinitionIds.size() ||
                        cell.candidateDefinitionIds[previous->selectedOrdinal] !=
                            previous->selectedDefinitionId ||
                        previous->selectedDefinitionId ==
                            cell.candidateDefinitionIds[cell.initialSelectedOrdinal])
                    {
                        continue;
                    }
                    const auto target = engine->soundVariableTarget(
                        cell.identity, previous->selectedDefinitionId, stageGeneration
                    );
                    if (!target.has_value())
                        continue;
                    PreparedTypedEvent event{};
                    event.kind = TypedEventKind::SoundVariableAssignment;
                    event.target = target->target;
                    event.frameOffset = 0;
                    event.sequence = transferEvents.size() + 1U;
                    event.payload.soundVariable = {
                        target->selectedOrdinal,
                    };
                    transferEvents.push_back(event);
                }
            }
            transferResults.resize(transferEvents.size());
            if (!transferEvents.empty()
                && (!engine->scheduleTypedEvents(
                        transferEvents,
                        transferResults)
                    || std::ranges::any_of(
                        transferResults,
                        [](RealtimeTypedEventDispatchResult result) {
                            return result
                                != RealtimeTypedEventDispatchResult::Handled;
                        })))
            {
                // The transfer queue publishes all-or-nothing. A rejected
                // candidate therefore leaves this new engine at declaration
                // initial values while the active graph remains untouched.
            }
        }
        catch (const std::bad_alloc&)
        {
            // Live values are intentionally non-persistent. Allocation
            // failure falls back to the new declaration initial values.
        }
        catch (const std::length_error&)
        {
            // The bounded manifest remains stageable at initial values.
        }
    }
    std::shared_ptr<const PreparedGraphSummary>
        stagedPreparedSummary;
    try
    {
        preparedSummary.summary->stageId = engineStageId;
        stagedPreparedSummary =
            std::make_shared<const PreparedGraphSummary>(
                std::move(*preparedSummary.summary));
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared graph summary allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Prepared graph summary exceeded supported limits",
        };
    }

    component_bundle::ComponentBundleReplacementConsumptionLease
        replacementLease;
    if (replacementReceipt != nullptr)
    {
        if (stagedIsolatedBinding == nullptr
            || replacementCandidate == nullptr
            || replacementPreservedDigest == nullptr
            || replacementDocumentGeneration == 0
            || replacementNodeId.empty())
        {
            return {
                ProjectStageOutcome::ReplacementRejected,
                std::nullopt,
                {},
                "Replacement stage is missing its exact binding context",
            };
        }
        const auto node = std::ranges::find(
            document.graph.nodes,
            replacementNodeId,
            &talas::project::ProjectNode::instanceId);
        const auto& contract = stagedIsolatedBinding->contract();
        const auto candidateValidity = matchComponentOwnerApproval(
            *replacementCandidate,
            replacementCandidate);
        if (node == document.graph.nodes.end()
            || !node->preservedComponent.has_value()
            || candidateValidity.ownerApproval
                != ComponentOwnerApprovalState::ExactMatch
            || candidateValidity.diagnostic
                != ComponentOwnerApprovalMatchCode::ExactMatch
            || contract.nodeId != replacementNodeId
            || contract.typeId != node->typeId
            || contract.bundleId != replacementCandidate->bundleId
            || contract.typeVersion != replacementCandidate->semanticVersion
            || contract.contentDigest != replacementCandidate->contentDigest
            || contract.signerIdentityDigest
                != replacementCandidate->signerIdentityDigest
            || contract.requestedPermissions
                != replacementCandidate->requestedPermissions
            || contract.policyVersion != replacementCandidate->policyVersion
            || contract.stateSchemaVersion
                != node->preservedComponent->stateSchemaVersion
            || contract.inputs != node->preservedComponent->inputs
            || contract.outputs != node->preservedComponent->outputs
            || contract.parameters != node->preservedComponent->parameters)
        {
            return {
                ProjectStageOutcome::ReplacementRejected,
                std::nullopt,
                {},
                "Prepared replacement binding does not match the confirmed preserved node and candidate",
            };
        }
        replacementLease = replacementReceipt->tryConsume({
            replacementDocumentGeneration,
            replacementNodeId,
            *replacementPreservedDigest,
            *replacementCandidate,
            replacementTransitions,
        });
        if (!replacementLease.approved())
        {
            return {
                ProjectStageOutcome::ReplacementRejected,
                std::nullopt,
                {},
                "Replacement confirmation is stale, mismatched, revoked, or already consumed",
            };
        }
    }

    if (stagedIsolatedBinding != nullptr
        && !stagedIsolatedBinding->tryBeginStage())
    {
        return {
            ProjectStageOutcome::EngineRejected,
            std::nullopt,
            {},
            "Prepared isolated component authorization was revoked before staging",
        };
    }
    struct StageAdmissionScope
    {
        PreparedIsolatedComponentBinding* binding = nullptr;
        void finish() noexcept
        {
            if (binding != nullptr)
            {
                binding->endStage();
                binding = nullptr;
            }
        }
        ~StageAdmissionScope()
        {
            finish();
        }
    } stageAdmission{stagedIsolatedBinding.get()};
    const auto stageTransport = graphHost_.transportSnapshot();
    if ((engine->transportLoop().has_value() || engine->preparedScoreSchedule())
        && graphHost_.activeStageId() != 0
        && stageTransport.sampleRate == engine->sampleRate())
    {
        // A newly staged live Score can be adopted while its clip is already
        // active, so its source history is prepared for one exact frame: the
        // next callback's start. If callbacks advanced past it during the
        // preparation, retry for a frame ahead by four times the observed
        // advance; the callback that reaches that frame splits there and
        // adopts it.
        const auto minimumLead = static_cast<std::uint64_t>(
            std::ceil(engine->sampleRate() * 0.05)) + 2U * engine->maxBlockFrames();
        std::uint64_t lead = stageTransport.playing ? initialStageLeadFrames_ : 0;
        auto origin = stageTransport;
        bool installed = false;
        for (std::size_t attempt = 0; attempt < 3 && !installed; ++attempt)
        {
            if (origin.absoluteFrame > std::numeric_limits<std::uint64_t>::max() - lead)
                break;
            const auto stageFrame = origin.absoluteFrame + lead;
            std::uint64_t remaining = 1000000;
            std::string transportError;
            std::unique_ptr<RealtimeAudioEngine::PreparedTransportRelocation> point;
            try
            {
                point = engine->prepareTransportRelocation(
                    stageFrame, remaining, transportError);
            }
            catch (const std::bad_alloc&)
            {
                transportError = "Stage source preparation ran out of memory";
            }
            catch (const std::length_error&)
            {
                transportError = "Stage source preparation exceeded supported limits";
            }
            if (!point)
            {
                return {
                    ProjectStageOutcome::EngineRejected,
                    std::nullopt,
                    {},
                    transportError.empty()
                        ? "Stage source history could not be prepared"
                        : std::move(transportError),
                };
            }
            auto current = graphHost_.transportSnapshot();
            if (current.absoluteFrame <= stageFrame)
            {
                if (!engine->installTransportRelocation(*point))
                {
                    return {ProjectStageOutcome::EngineRejected, std::nullopt, {},
                        "Stage source history could not be installed"};
                }
                engine->setPreparedStageTransportFrame(stageFrame);
                graphHost_.collectRetired().reset();
                // Checked last, so staging directly follows it.
                current = graphHost_.transportSnapshot();
                installed = current.absoluteFrame <= stageFrame;
            }
            if (current.epoch != stageTransport.epoch
                || current.discontinuitySequence != stageTransport.discontinuitySequence)
            {
                installed = false;
                break;
            }
            if (installed)
                break;
            lead = std::max(lead * 2U,
                (current.absoluteFrame - origin.absoluteFrame) * 4U + minimumLead);
            origin = current;
        }
        if (!installed)
        {
            return {ProjectStageOutcome::EngineRejected, std::nullopt, {},
                "Score transport advanced while preparing replacement source history; retry staging"};
        }
    }
    else
    {
        graphHost_.collectRetired().reset();
    }
    // A terminal callback split at this engine's prepared frame renders each
    // side into storage made here, before the audio thread can see it.
    if (stagedOutputChannelCount == 0
        && !terminalSegmentScratchReady_.load(std::memory_order_acquire))
    {
        try
        {
            for (auto& scratch : terminalSegmentScratch_)
            {
                scratch.midiEvents.resize(maximumTerminalSegmentEvents);
                scratch.globalControllerEvents.resize(maximumTerminalSegmentEvents);
                scratch.globalControllerName.resize(maximumTerminalSegmentNameBytes);
            }
        }
        catch (const std::bad_alloc&)
        {
            return {ProjectStageOutcome::EngineRejected, std::nullopt, {},
                "Terminal event storage could not be allocated"};
        }
        terminalSegmentScratchReady_.store(true, std::memory_order_release);
    }
    const auto stage = graphHost_.tryStage(std::move(engine));
    stageAdmission.finish();
    if (stage != RealtimeGraphStageResult::Accepted)
    {
        return {
            ProjectStageOutcome::StageRejected,
            stage,
            {},
            "Realtime graph staging is not ready for another prepared graph",
        };
    }
    replacementLease.commit();
    acceptedResourceIdentities_.swap(
        nextAcceptedResourceIdentities);
    ++nextEngineStageId_;
    latestAnalysisCapture_ = stagedAnalysisCapture;
    latestScoreSourceAnalysis_ = stagedScoreSourceAnalysis;
    latestAnalysisCaptureStageId_ = engineStageId;
    latestPreparedGraphSummary_ =
        std::move(stagedPreparedSummary);
    latestPreparedScoreSchedule_ =
        std::move(stagedPreparedScore);
    std::atomic_store_explicit(
        &latestPreparedLiveSet_,
        std::move(stagedPreparedLiveSet),
        std::memory_order_release);
    std::atomic_store_explicit(
        &latestPreparedLiveTools_,
        std::move(stagedPreparedLiveTools),
        std::memory_order_release);
    latestSnapshotPreparedTargets_ =
        std::move(stagedSnapshotPreparedTargets);
    latestSnapshotStageId_ = engineStageId;
    latestSnapshotStageGeneration_ = stageGeneration;
    latestPreparedVariables_ = std::move(stagedPreparedVariables);
    latestPreparedVariableStageGeneration_ = stageGeneration;
    latestPreparedSoundVariables_ = std::move(stagedPreparedSoundVariables);
    latestPreparedSoundVariableStageGeneration_ = stageGeneration;
    latestVariableStageIsEditorDocument_ = preserveVariableValueHistory;
    latestSnapshotSampleRate_ = stagedSnapshotSampleRate;
    projectSnapshotRenderSettings_ =
        std::move(stagedProjectRenderSettings);
    latestSnapshotExecutionTopology_ =
        std::move(stagedExecutionTopology);
    latestSnapshotAuthoringTopology_ =
        std::move(stagedAuthoringTopology);
    coalescedSnapshotOverlay_.reset();
    observedSnapshotTransitionCompletionToken_ =
        graphHost_.sameTopologyTransitionCompletionToken();
    snapshotPerformanceState_ =
        std::move(stagedSnapshotPerformanceState);
    projectSnapshotBaselineValues_ =
        std::move(stagedSnapshotBaselineValues);
    stagedSnapshotAuthoringBytes_ =
        std::move(stagedSnapshotAuthoringBytes);
    ++nextSnapshotProjectRevision_;
    acceptedMidiStageId_.store(engineStageId, std::memory_order_release);
    for (auto& registration : pendingRegistrations)
    {
        const auto index = ownedEventValueNames_.size();
        const auto* publishedName = registration.name.get();
        ownedEventValueNames_.push_back(std::move(registration.name));
        registeredEventValueDomains_[index].store(registration.domain, std::memory_order_relaxed);
        registeredEventValueNames_[index].store(publishedName, std::memory_order_release);
    }
    // The name keeps its ID: a value still queued in the old domain is
    // refused by the new graph, which binds the name in the new one only.
    for (const auto& use : registeredUses)
    {
        if (use.rebinds)
            registeredEventValueDomains_[use.index].store(use.domain, std::memory_order_relaxed);
    }
    ++nextStageGeneration_;
    for (const auto& binding : realtimeBindings)
    {
        const auto eventId = realtimeEventValueId(binding.name);
        if (eventId.has_value())
        {
            registeredEventValueStageGenerations_[*eventId - 1].store(stageGeneration, std::memory_order_relaxed);
            registeredEventValueDocumentReplacements_[*eventId - 1] = documentReplacements;
        }
    }
    latestStageGeneration_.store(stageGeneration, std::memory_order_release);
    // Publish the channel only after the new stage is visible. A controller
    // callback holding the old route generation will then fail the exact-stage
    // enqueue check while this pair is being replaced.
    acceptedDefaultMidiChannel_.store(
        projectSnapshotRenderSettings_->defaultMidiChannel,
        std::memory_order_relaxed);
    acceptedTimeCodeOffsetSeconds_.store(document.timeline.timeCodeOffsetSeconds, std::memory_order_relaxed);
    {
        // The global map's associations of the Event Values this stage has.
        std::uint32_t count = 0;
        for (const auto& entry : talas::project::projectGlobalMapEntries(document))
        {
            if (count >= maximumAcceptedGlobalMapEntries)
                break;
            // Another Event Source no MIDI reaches.
            if (!entry.source.empty())
                continue;
            const auto eventId = realtimeEventValueId(entry.eventName);
            if (!eventId.has_value())
                continue;
            auto& slot = acceptedGlobalMap_[count++];
            slot.eventId.store(*eventId, std::memory_order_relaxed);
            slot.controller.store(entry.controller, std::memory_order_relaxed);
            slot.channel.store(entry.channel.value_or(0), std::memory_order_relaxed);
            slot.pitchBend.store(entry.pitchBend, std::memory_order_relaxed);
            slot.logTaper.store(entry.taper == talas::project::MidiMapperMapTaper::Log, std::memory_order_relaxed);
            slot.minimum.store(entry.minimum, std::memory_order_relaxed);
            slot.maximum.store(entry.maximum, std::memory_order_relaxed);
            slot.grid.store(entry.grid, std::memory_order_relaxed);
        }
        acceptedGlobalMapCount_.store(count, std::memory_order_relaxed);
    }
    acceptedMidiControllerStageGeneration_.store(
        stageGeneration, std::memory_order_release);
    for (const auto sourceKind : {
             RealtimeLiveToolSourceKind::EventValue,
             RealtimeLiveToolSourceKind::Variable,
             RealtimeLiveToolSourceKind::Periodic,
             RealtimeLiveToolSourceKind::Offline})
    {
        publishLiveToolSourceAuthority(
            sourceKind, stageGeneration, stageGeneration);
    }
    return {
        ProjectStageOutcome::Staged,
        stage,
        {},
        std::move(stagedSnapshotStageMessage),
        stagedOutputChannelCount,
    };
}

ProjectStageResult
ProjectRuntimeBridge::compileAndStagePerformanceOverlay(
    std::span<const talas::project::SnapshotValueEntry> values,
    double smoothingSeconds)
{
    if (nextStageGeneration_
            == std::numeric_limits<std::uint64_t>::max()
        || nextEngineStageId_
            == std::numeric_limits<std::uint64_t>::max())
    {
        return {
            ProjectStageOutcome::StageRejected,
            std::nullopt,
            {},
            "Performance overlay stage identity space is exhausted",
        };
    }
    if (!projectSnapshotRenderSettings_.has_value()
        || !latestSnapshotExecutionTopology_.has_value())
    {
        return {
            ProjectStageOutcome::CompileFailed,
            std::nullopt,
            {},
            "Performance overlay requires an accepted project stage",
        };
    }
    if (!std::isfinite(smoothingSeconds)
        || smoothingSeconds < 0.0
        || smoothingSeconds
            > talas::project::maximumSnapshotSmoothingSeconds)
    {
        return {
            ProjectStageOutcome::CompileFailed,
            std::nullopt,
            {},
            "Performance overlay smoothing must be between zero and thirty seconds",
        };
    }

    RenderSettings settings;
    try
    {
        settings = *projectSnapshotRenderSettings_;
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Performance overlay render-settings allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Performance overlay render-settings exceeded supported limits",
        };
    }

    std::vector<talas::project::SnapshotValueEntry>
        compilationValues;
    try
    {
        compilationValues.reserve(values.size());
        for (const auto& value : values)
        {
            if (value.target.kind
                != talas::project::SnapshotTargetKind::Variable &&
                value.target.kind != talas::project::SnapshotTargetKind::SoundVariable)
            {
                compilationValues.push_back(value);
            }
        }
    }
    catch (const std::exception&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Performance overlay variable projection allocation failed",
        };
    }
    auto compiled = compileProjectPerformanceOverlay(
        editor_.document(),
        registry_,
        std::move(settings),
        compilationValues);
    if (!compiled.ok())
    {
        return {
            ProjectStageOutcome::CompileFailed,
            std::nullopt,
            std::move(compiled.diagnostics),
            "Performance overlay compilation failed",
        };
    }

    SnapshotExecutionTopologyFingerprint candidateTopology;
    try
    {
        candidateTopology =
            snapshotPreparedExecutionTopologyFingerprint(
                *compiled.graph);
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Performance overlay topology fingerprint allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Performance overlay topology fingerprint exceeded supported limits",
        };
    }
    if (candidateTopology != *latestSnapshotExecutionTopology_)
    {
        return {
            ProjectStageOutcome::TopologyMismatch,
            std::nullopt,
            {},
            "Performance overlay changed the prepared execution topology",
        };
    }

    decltype(latestSnapshotPreparedTargets_)
        stagedSnapshotPreparedTargets;
    std::shared_ptr<const PreparedGraphSummary>
        stagedPreparedSummary;
    talas::project::PreparedProjectVariableTable
        stagedPreparedVariables;
    talas::project::PreparedProjectSoundVariableTable stagedPreparedSoundVariables;
    std::unique_ptr<RealtimeAudioEngine> engine;
    const auto stagedAnalysisCapture =
        compiled.graph->analysisCapture();
    const auto stagedScoreSourceAnalysis = compiled.graph->scoreSourceAnalysisCaptures();
    const auto stagedSnapshotSampleRate =
        compiled.graph->renderSettings().sampleRate;
    const auto stagedOutputChannelCount =
        compiled.graph->plan().outputBufferIndices.size();
    const auto stagedVariableTransferSource =
        graphHost_.preparedVariableValues();
    const auto stagedSoundVariableTransferSource = graphHost_.preparedSoundVariableValues();
    try
    {
        const auto& manifest =
            compiled.graph->snapshotPreparedTargets();
        stagedSnapshotPreparedTargets.reserve(
            manifest.targets.size());
        for (const auto& target : manifest.targets)
        {
            if (!stagedSnapshotPreparedTargets.emplace(
                    talas::project::snapshotTargetIdentityString(
                        target.target),
                    target).second)
            {
                return {
                    ProjectStageOutcome::EngineCreationFailed,
                    std::nullopt,
                    {},
                    "Performance overlay prepared target manifest contains a duplicate stable identity",
                };
            }
        }
        auto summary = makePreparedGraphSummary(*compiled.graph);
        if (!summary.ok())
        {
            return {
                ProjectStageOutcome::EngineCreationFailed,
                std::nullopt,
                {},
                std::move(summary.error),
            };
        }
        summary.summary->stageId = nextEngineStageId_;
        stagedPreparedSummary =
            std::make_shared<const PreparedGraphSummary>(
                std::move(*summary.summary));
        stagedPreparedVariables =
            compiled.graph->preparedVariables();
        stagedPreparedSoundVariables = compiled.graph->preparedSoundVariables();
        engine = std::make_unique<RealtimeAudioEngine>(
            *compiled.graph);
    }
    catch (const std::bad_alloc&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Performance overlay engine allocation failed",
        };
    }
    catch (const std::length_error&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Performance overlay engine exceeded supported limits",
        };
    }
    if (!engine->ready())
    {
        return {
            ProjectStageOutcome::EngineRejected,
            std::nullopt,
            {},
            std::string(engine->error()),
        };
    }

    PreparedSnapshotOverlayStage preparedStage;
    preparedStage.engine = std::move(engine);
    preparedStage.preparedTargets =
        std::move(stagedSnapshotPreparedTargets);
    preparedStage.analysisCapture = stagedAnalysisCapture;
    preparedStage.scoreSourceAnalysis = stagedScoreSourceAnalysis;
    preparedStage.preparedSummary =
        std::move(stagedPreparedSummary);
    preparedStage.preparedVariables =
        std::move(stagedPreparedVariables);
    preparedStage.preparedSoundVariables = std::move(stagedPreparedSoundVariables);
    preparedStage.stageId = nextEngineStageId_;
    preparedStage.stageGeneration = nextStageGeneration_;
    preparedStage.sampleRate = stagedSnapshotSampleRate;
    preparedStage.outputChannelCount = stagedOutputChannelCount;
    preparedStage.transitionFrames = snapshotSmoothingFrames(
        smoothingSeconds,
        stagedSnapshotSampleRate);
    preparedStage.engine->setStageId(preparedStage.stageId);
    preparedStage.engine->setSnapshotGeneration(
        preparedStage.stageGeneration);
    try
    {
        std::vector<PreparedTypedEvent> variableEvents;
        std::vector<RealtimeTypedEventDispatchResult>
            variableResults;
        variableEvents.reserve(
            preparedStage.preparedVariables.cells.size() +
            preparedStage.preparedSoundVariables.cells.size()
        );
        for (const auto& cell :
             preparedStage.preparedVariables.cells)
        {
            if (cell.identity.isVoiceLocal())
            {
                continue;
            }
            auto desiredValue = cell.initialValue;
            if (stagedVariableTransferSource.publicationValid)
            {
                const auto previous = std::ranges::find(
                    stagedVariableTransferSource.values,
                    cell.identity,
                    &PreparedVariableValue::identity);
                if (previous
                        != stagedVariableTransferSource.values.end()
                    && previous->value.domain == cell.domain)
                {
                    desiredValue = previous->value.value;
                }
            }
            for (const auto& value : values)
            {
                if (value.target.kind
                        != talas::project::SnapshotTargetKind::Variable
                    || value.value.kind
                        != talas::project::SnapshotValueKind::Number
                    || value.value.domain != cell.domain)
                {
                    continue;
                }
                talas::project::PreparedProjectVariableIdentity identity;
                identity.ownerKind =
                    value.target.variableOwnerKind
                            == talas::project::SnapshotVariableOwnerKind::WorkspaceSession
                        ? talas::project::ProjectVariableOwnerKind::WorkspaceSession
                        : (value.target.definitionId.empty()
                                ? talas::project::ProjectVariableOwnerKind::Project
                                : talas::project::ProjectVariableOwnerKind::SoundDefinition);
                identity.variableId = value.target.variableId;
                identity.definitionId = value.target.definitionId;
                identity.instancePath = value.target.instancePath;
                if (identity == cell.identity)
                    desiredValue = value.value.number;
            }
            if (desiredValue == cell.initialValue)
                continue;
            const auto target = preparedStage.engine->variableTarget(
                cell.identity,
                cell.domain,
                preparedStage.stageGeneration);
            if (!target.has_value())
            {
                return {
                    ProjectStageOutcome::EngineCreationFailed,
                    std::nullopt,
                    {},
                    "Performance overlay variable target is unavailable",
                };
            }
            PreparedTypedEvent event{};
            event.kind = TypedEventKind::VariableAssignment;
            event.target = *target;
            event.frameOffset = 0;
            event.sequence = variableEvents.size() + 1U;
            event.payload.variable = {desiredValue, cell.domain};
            variableEvents.push_back(event);
        }
        for (const auto& cell : preparedStage.preparedSoundVariables.cells)
        {
            if (cell.identity.isVoiceLocal())
            {
                continue;
            }
            if (cell.candidateDefinitionIds.empty())
                continue;
            auto desiredDefinitionId = cell.candidateDefinitionIds[cell.initialSelectedOrdinal];
            if (stagedSoundVariableTransferSource.publicationValid)
            {
                const auto previous = std::ranges::find(
                    stagedSoundVariableTransferSource.values,
                    cell.identity,
                    &PreparedSoundVariableValue::identity
                );
                if (previous != stagedSoundVariableTransferSource.values.end() &&
                    std::ranges::find(
                        cell.candidateDefinitionIds, previous->selectedDefinitionId
                    ) != cell.candidateDefinitionIds.end())
                {
                    desiredDefinitionId = previous->selectedDefinitionId;
                }
            }
            for (const auto& value : values)
            {
                if (value.target.kind != talas::project::SnapshotTargetKind::SoundVariable ||
                    value.value.kind != talas::project::SnapshotValueKind::Choice)
                {
                    continue;
                }
                talas::project::PreparedProjectSoundVariableIdentity identity;
                identity.ownerKind =
                    value.target.variableOwnerKind ==
                            talas::project::SnapshotVariableOwnerKind::WorkspaceSession
                        ? talas::project::ProjectVariableOwnerKind::WorkspaceSession
                        : (value.target.definitionId.empty()
                               ? talas::project::ProjectVariableOwnerKind::Project
                               : talas::project::ProjectVariableOwnerKind::SoundDefinition);
                identity.variableId = value.target.variableId;
                identity.definitionId = value.target.definitionId;
                identity.instancePath = value.target.instancePath;
                if (identity == cell.identity)
                    desiredDefinitionId = value.value.choice;
            }
            if (desiredDefinitionId == cell.candidateDefinitionIds[cell.initialSelectedOrdinal])
            {
                continue;
            }
            const auto target = preparedStage.engine->soundVariableTarget(
                cell.identity, desiredDefinitionId, preparedStage.stageGeneration
            );
            if (!target.has_value())
            {
                return {
                    ProjectStageOutcome::EngineCreationFailed,
                    std::nullopt,
                    {},
                    "Performance overlay Sound-variable target is unavailable",
                };
            }
            PreparedTypedEvent event{};
            event.kind = TypedEventKind::SoundVariableAssignment;
            event.target = target->target;
            event.frameOffset = 0;
            event.sequence = variableEvents.size() + 1U;
            event.payload.soundVariable = {
                target->selectedOrdinal,
            };
            variableEvents.push_back(event);
        }
        variableResults.resize(variableEvents.size());
        if (!variableEvents.empty()
            && (!preparedStage.engine->scheduleTypedEvents(
                    variableEvents,
                    variableResults)
                || std::ranges::any_of(
                    variableResults,
                    [](RealtimeTypedEventDispatchResult result) {
                        return result
                            != RealtimeTypedEventDispatchResult::Handled;
                    })))
        {
            return {
                ProjectStageOutcome::EngineCreationFailed,
                std::nullopt,
                {},
                "Performance overlay variable transaction was rejected",
            };
        }
    }
    catch (const std::exception&)
    {
        return {
            ProjectStageOutcome::EngineCreationFailed,
            std::nullopt,
            {},
            "Performance overlay variable transfer allocation failed",
        };
    }
    (void) graphHost_.collectRetired();
    if (graphHost_.sameTopologyTransitionActive())
    {
        const auto outputChannelCount =
            preparedStage.outputChannelCount;
        try
        {
            coalescedSnapshotOverlay_ =
                std::make_unique<PreparedSnapshotOverlayStage>(
                    std::move(preparedStage));
        }
        catch (const std::bad_alloc&)
        {
            return {
                ProjectStageOutcome::EngineCreationFailed,
                std::nullopt,
                {},
                "Performance overlay coalescing allocation failed",
            };
        }
        return {
            ProjectStageOutcome::OverlayCoalesced,
            RealtimeGraphStageResult::
                PendingEngineAwaitingActivation,
            {},
            "Performance overlay coalesced to the newest complete candidate",
            outputChannelCount,
        };
    }
    const auto stage = graphHost_.tryStageSameTopologyTransition(
        std::move(preparedStage.engine),
        preparedStage.transitionFrames);
    if (stage != RealtimeGraphStageResult::Accepted)
    {
        return {
            ProjectStageOutcome::StageRejected,
            stage,
            {},
            "Realtime graph host cannot accept the performance overlay transition",
        };
    }

    publishAcceptedSnapshotOverlay(std::move(preparedStage));
    return {
        ProjectStageOutcome::Staged,
        stage,
        {},
        {},
        stagedOutputChannelCount,
    };
}

void ProjectRuntimeBridge::publishAcceptedSnapshotOverlay(
    PreparedSnapshotOverlayStage stage)
{
    ++nextEngineStageId_;
    ++nextStageGeneration_;
    latestAnalysisCapture_ = std::move(stage.analysisCapture);
    latestScoreSourceAnalysis_ = std::move(stage.scoreSourceAnalysis);
    latestAnalysisCaptureStageId_ = stage.stageId;
    latestPreparedGraphSummary_ =
        std::move(stage.preparedSummary);
    latestSnapshotPreparedTargets_ =
        std::move(stage.preparedTargets);
    latestPreparedVariables_ =
        std::move(stage.preparedVariables);
    latestPreparedVariableStageGeneration_ = stage.stageGeneration;
    latestPreparedSoundVariables_ = std::move(stage.preparedSoundVariables);
    latestPreparedSoundVariableStageGeneration_ =
        stage.stageGeneration;
    latestSnapshotStageId_ = stage.stageId;
    latestSnapshotStageGeneration_ = stage.stageGeneration;
    latestSnapshotSampleRate_ = stage.sampleRate;
    acceptedMidiStageId_.store(
        stage.stageId,
        std::memory_order_release);
    for (const auto& binding :
         projectSnapshotRenderSettings_->eventValues)
    {
        const auto eventId = realtimeEventValueId(binding.name);
        if (eventId.has_value())
        {
            registeredEventValueStageGenerations_[*eventId - 1].store(
                stage.stageGeneration,
                std::memory_order_relaxed);
        }
    }
    latestStageGeneration_.store(
        stage.stageGeneration,
        std::memory_order_release);
}

bool ProjectRuntimeBridge::serviceCoalescedSnapshotOverlay() noexcept
{
    if (coalescedSnapshotOverlay_ == nullptr
        || graphHost_.sameTopologyTransitionActive())
    {
        return false;
    }
    const auto completionToken =
        graphHost_.sameTopologyTransitionCompletionToken();
    if (completionToken
            == observedSnapshotTransitionCompletionToken_
        && graphHost_.activeStageId()
            != latestSnapshotStageId_)
    {
        return false;
    }
    observedSnapshotTransitionCompletionToken_ = completionToken;
    auto preparedStage = std::move(coalescedSnapshotOverlay_);
    const auto stage = graphHost_.tryStageSameTopologyTransition(
        std::move(preparedStage->engine),
        preparedStage->transitionFrames);
    if (stage != RealtimeGraphStageResult::Accepted)
        return false;
    publishAcceptedSnapshotOverlay(
        std::move(*preparedStage));
    return true;
}

const SnapshotPerformanceState&
ProjectRuntimeBridge::performanceState() const noexcept
{
    return snapshotPerformanceState_;
}

std::span<const talas::project::SnapshotValueEntry>
ProjectRuntimeBridge::performanceBaselineValues() const noexcept
{
    return projectSnapshotBaselineValues_;
}

bool ProjectRuntimeBridge::snapshotPerformanceDocumentCurrent() const
{
    SnapshotPerformanceActionResult failure;
    return snapshotPerformanceDocumentIsCurrent(failure);
}

talas::project::SnapshotOperationResult
ProjectRuntimeBridge::currentSnapshotScopeValues(
    const talas::project::SnapshotScopeIdentity& scope) const
{
    SnapshotPerformanceActionResult failure;
    if (!snapshotPerformanceDocumentIsCurrent(failure))
        return {{}, std::move(failure.diagnostics)};

    talas::expr::EvaluationContext evaluation;
    evaluation.sampleRate = projectSnapshotRenderSettings_->sampleRate;
    evaluation.tempoBpm = projectSnapshotRenderSettings_->tempoBpm;
    evaluation.rampsStartAtFirstValue = projectSnapshotRenderSettings_->rampsStartAtFirstValue;
    evaluation.rampsHoldUntriggered = projectSnapshotRenderSettings_->rampsHoldUntriggered;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(
        editor_.document(),
        registry_,
        scope,
        evaluation);
    if (!resolved.ok())
        return {{}, resolved.diagnostics};

    std::vector<talas::project::SnapshotValueEntry> scopeCurrentValues;
    try
    {
        scopeCurrentValues.reserve(resolved.targets.size());
        const auto preparedVariables = graphHost_.preparedVariableValues();
        const auto preparedVariablesCurrent =
            preparedVariables.publicationValid
            && preparedVariables.stageId == latestSnapshotStageId_;
        for (const auto& target : resolved.targets)
        {
            if (target.target.kind
                == talas::project::SnapshotTargetKind::Variable)
            {
                if (preparedVariablesCurrent)
                {
                    if (auto current = preparedVariableSnapshotValue(
                            target.target, preparedVariables))
                    {
                        scopeCurrentValues.push_back(
                            std::move(*current));
                    }
                }
                continue;
            }
            const auto current = std::ranges::find(
                snapshotPerformanceState_.currentValues,
                target.target,
                &talas::project::SnapshotValueEntry::target);
            if (current != snapshotPerformanceState_.currentValues.end())
                scopeCurrentValues.push_back(*current);
        }
    }
    catch (const std::bad_alloc&)
    {
        return {{}, {{
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            talas::project::SnapshotDiagnosticSeverity::Error,
            "$.snapshotCapture",
            {},
            "Current snapshot scope allocation failed",
        }}};
    }
    catch (const std::length_error&)
    {
        return {{}, {{
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            talas::project::SnapshotDiagnosticSeverity::Error,
            "$.snapshotCapture",
            {},
            "Current snapshot scope exceeded supported limits",
        }}};
    }
    return talas::project::captureSnapshotScopeValues(
        resolved.targets,
        scopeCurrentValues);
}

bool ProjectRuntimeBridge::snapshotPerformanceDocumentIsCurrent(
    SnapshotPerformanceActionResult& failure) const
{
    if (stagedSnapshotAuthoringBytes_.empty()
        || !projectSnapshotRenderSettings_.has_value())
    {
        failure = snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::
                StalePreparedGeneration,
            "Snapshot performance requires an accepted project stage");
        return false;
    }
    try
    {
        if (talas::project::serializeProjectDocument(
                editor_.document(), registry_)
            == stagedSnapshotAuthoringBytes_)
        {
            return true;
        }
    }
    catch (const std::bad_alloc&)
    {
        failure = snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot performance revision comparison allocation failed");
        return false;
    }
    catch (const std::length_error&)
    {
        failure = snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot performance revision comparison exceeded supported limits");
        return false;
    }
    failure = snapshotPerformanceFailure(
        talas::project::SnapshotDiagnosticCode::
            StalePreparedGeneration,
        "Project edits must be staged before another snapshot performance action");
    return false;
}

bool ProjectRuntimeBridge::resolveSnapshotPerformanceValues(
    const talas::project::ProjectSnapshotBank& bank,
    const talas::project::ProjectSnapshot& snapshot,
    std::span<const talas::project::SnapshotValueEntry> currentValues,
    std::vector<talas::project::SnapshotValueEntry>& values,
    SnapshotPerformanceActionResult& failure) const
{
    talas::expr::EvaluationContext evaluation;
    if (projectSnapshotRenderSettings_.has_value())
    {
        evaluation.sampleRate =
            projectSnapshotRenderSettings_->sampleRate;
        evaluation.tempoBpm =
            projectSnapshotRenderSettings_->tempoBpm;
    }
    const auto resolved = talas::project::resolveSnapshotScopeTargets(
        editor_.document(),
        registry_,
        bank.scope,
        evaluation);
    if (!resolved.ok())
    {
        failure.diagnostics = resolved.diagnostics;
        failure.message = "Snapshot scope cannot resolve";
        return false;
    }
    auto captured = talas::project::captureSnapshotScopeValues(
        resolved.targets,
        snapshot.values);
    if (!captured.ok())
    {
        failure.diagnostics = std::move(captured.diagnostics);
        failure.message =
            "Stored snapshot is incomplete or incompatible with its scope";
        return false;
    }
    values = std::move(captured.values);
    for (auto& value : values)
    {
        const auto prepared = latestSnapshotPreparedTargets_.find(
            talas::project::snapshotTargetIdentityString(
                value.target));
        if (prepared == latestSnapshotPreparedTargets_.end()
            || prepared->second.target != value.target)
        {
            failure = snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::
                    PreparedTargetUnavailable,
                "Snapshot target is unavailable in the latest prepared stage",
                &value.target);
            values.clear();
            return false;
        }
        const auto& target = prepared->second;
        if (target.valueKind != value.value.kind
            || (value.value.kind
                    == talas::project::SnapshotValueKind::Number
                && (target.domain != value.value.domain
                    || !std::isfinite(value.value.number)))
            || (value.value.kind
                    == talas::project::SnapshotValueKind::Choice
                && std::ranges::find(
                       target.choices,
                       value.value.choice)
                    == target.choices.end()))
        {
            failure = snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::KindChanged,
                "Snapshot value no longer matches its prepared target",
                &value.target);
            values.clear();
            return false;
        }
        const auto policy = std::ranges::find_if(
            bank.parameterPolicies,
            [&](const auto& candidate) {
                return candidate.target == value.target;
            });
        if (policy == bank.parameterPolicies.end()
            || !policy->locked)
        {
            continue;
        }
        const auto held = std::ranges::find_if(
            currentValues,
            [&](const auto& candidate) {
                return candidate.target == value.target;
            });
        if (held == currentValues.end()
            || held->value.kind != value.value.kind
            || (value.value.kind
                    == talas::project::SnapshotValueKind::Number
                && held->value.domain != value.value.domain))
        {
            failure = snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::MissingParameter,
                "Locked snapshot target has no compatible current value",
                &value.target);
            values.clear();
            return false;
        }
        value.value = held->value;
    }
    return true;
}

bool ProjectRuntimeBridge::authoritativeSnapshotPerformanceValues(
    std::vector<talas::project::SnapshotValueEntry>& values,
    SnapshotPerformanceActionResult& failure) const
{
    try
    {
        values = snapshotPerformanceState_.currentValues;
        const auto publication = graphHost_.preparedVariableValues();
        const auto publicationCurrent = publication.publicationValid
            && publication.stageId == latestSnapshotStageId_;
        for (auto& value : values)
        {
            if (value.target.kind
                != talas::project::SnapshotTargetKind::Variable)
            {
                continue;
            }
            if (!publicationCurrent)
            {
                failure = snapshotPerformanceFailure(
                    talas::project::SnapshotDiagnosticCode::MissingParameter,
                    "Live variable publication is unavailable for the current snapshot stage",
                    &value.target);
                values.clear();
                return false;
            }
            auto current = preparedVariableSnapshotValue(
                value.target, publication);
            if (!current.has_value())
            {
                failure = snapshotPerformanceFailure(
                    talas::project::SnapshotDiagnosticCode::MissingParameter,
                    "Live variable value is unavailable for the current snapshot target",
                    &value.target);
                values.clear();
                return false;
            }
            value = std::move(*current);
        }
        return true;
    }
    catch (const std::bad_alloc&)
    {
        failure = snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Live snapshot value allocation failed");
        values.clear();
        return false;
    }
    catch (const std::length_error&)
    {
        failure = snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Live snapshot values exceeded supported limits");
        values.clear();
        return false;
    }
}

bool ProjectRuntimeBridge::prepareSnapshotPerformanceState(
    std::span<const talas::project::SnapshotValueEntry> values,
    SnapshotPerformanceState& candidate,
    SnapshotPerformanceActionResult& failure) const
{
    try
    {
        candidate = snapshotPerformanceState_;
        for (const auto& value : values)
        {
            const auto current = std::ranges::find(
                candidate.currentValues,
                value.target,
                &talas::project::SnapshotValueEntry::target);
            if (current == candidate.currentValues.end())
            {
                failure = snapshotPerformanceFailure(
                    talas::project::SnapshotDiagnosticCode::
                        PreparedTargetUnavailable,
                    "Snapshot candidate target is absent from the current performance state",
                    &value.target);
                candidate = {};
                return false;
            }
            current->value = value.value;
        }
        return true;
    }
    catch (const std::bad_alloc&)
    {
        failure = snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot performance candidate allocation failed");
    }
    catch (const std::length_error&)
    {
        failure = snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot performance candidate exceeded supported limits");
    }
    candidate = {};
    return false;
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::publishSnapshotPerformanceValues(
    const talas::project::ProjectSnapshotBank& bank,
    std::vector<talas::project::SnapshotValueEntry> values,
    const SnapshotPerformanceState& candidateState)
{
    if (values.empty()
        || values.size()
            > talas::project::maximumSnapshotValuesPerSnapshot)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot performance candidate is empty or exceeds capacity");
    }
    auto directOnly = true;
    for (const auto& value : values)
    {
        if (globalControllerBlocksLiveOverride(value.target))
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::StageFailed,
                "Snapshot cannot override a generated-only Event Value",
                &value.target);
        }
        const auto prepared = latestSnapshotPreparedTargets_.find(
            talas::project::snapshotTargetIdentityString(
                value.target));
        if (prepared == latestSnapshotPreparedTargets_.end()
            || prepared->second.target != value.target)
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::
                    PreparedTargetUnavailable,
                "Snapshot candidate target is unavailable",
                &value.target);
        }
        directOnly = directOnly
            && (prepared->second.projection
                    == SnapshotPreparedProjection::DirectEventValues
                || prepared->second.projection
                    == SnapshotPreparedProjection::DirectVariable)
            && value.value.kind
                == talas::project::SnapshotValueKind::Number;
    }
    // A detached coalesced engine cannot receive the active stage's direct
    // transaction. Replace it with one complete overlay candidate so its
    // eventual adoption cannot roll back an already accepted live value.
    directOnly = directOnly
        && coalescedSnapshotOverlay_ == nullptr;

    SnapshotPerformanceActionResult result;
    if (directOnly)
    {
        const auto scheduled = scheduleSnapshotDirectValues(
            values,
            bank.smoothingSeconds);
        if (scheduled
            != SnapshotDirectTransactionResult::Accepted)
        {
            const auto code = scheduled
                    == SnapshotDirectTransactionResult::
                        StaleStageOrGeneration
                ? talas::project::SnapshotDiagnosticCode::
                    StalePreparedGeneration
                : scheduled
                        == SnapshotDirectTransactionResult::
                            CapacityExceeded
                        || scheduled
                            == SnapshotDirectTransactionResult::QueueFull
                ? talas::project::SnapshotDiagnosticCode::CapacityExceeded
                : talas::project::SnapshotDiagnosticCode::StageFailed;
            return snapshotPerformanceFailure(
                code,
                "Direct snapshot transaction was rejected by the prepared stage");
        }
        result.outcome =
            SnapshotPerformanceActionOutcome::AppliedDirect;
    }
    else
    {
        if (candidateState.currentValues.size()
                != projectSnapshotBaselineValues_.size())
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::
                    StalePreparedGeneration,
                "Snapshot performance candidate no longer matches the project baseline");
        }
        std::vector<talas::project::SnapshotValueEntry>
            effectiveOverlayValues;
        try
        {
            effectiveOverlayValues.reserve(
                candidateState.currentValues.size());
            for (std::size_t index = 0;
                 index < candidateState.currentValues.size();
                 ++index)
            {
                const auto& current =
                    candidateState.currentValues[index];
                const auto& baseline =
                    projectSnapshotBaselineValues_[index];
                if (current.target != baseline.target)
                {
                    return snapshotPerformanceFailure(
                        talas::project::SnapshotDiagnosticCode::
                            StalePreparedGeneration,
                        "Snapshot performance candidate target order no longer matches the project baseline",
                        &current.target);
                }
                if (current.target.kind
                        == talas::project::SnapshotTargetKind::Variable
                    || current.value != baseline.value)
                    effectiveOverlayValues.push_back(current);
            }
        }
        catch (const std::bad_alloc&)
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::CapacityExceeded,
                "Complete snapshot overlay candidate allocation failed");
        }
        catch (const std::length_error&)
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::CapacityExceeded,
                "Complete snapshot overlay candidate exceeded supported limits");
        }
        const auto staged = compileAndStagePerformanceOverlay(
            effectiveOverlayValues,
            bank.smoothingSeconds);
        if (!staged.ok())
        {
            const auto code = staged.outcome
                    == ProjectStageOutcome::TopologyMismatch
                ? talas::project::SnapshotDiagnosticCode::TopologyMismatch
                : staged.outcome == ProjectStageOutcome::CompileFailed
                ? talas::project::SnapshotDiagnosticCode::StageFailed
                : talas::project::SnapshotDiagnosticCode::StageFailed;
            auto failure = snapshotPerformanceFailure(
                code,
                staged.message.empty()
                    ? "Snapshot overlay stage failed"
                    : staged.message);
            return failure;
        }
        result.outcome = staged.outcome
                == ProjectStageOutcome::OverlayCoalesced
            ? SnapshotPerformanceActionOutcome::CoalescedOverlay
            : SnapshotPerformanceActionOutcome::StagedOverlay;
    }
    return result;
}

SnapshotPerformanceActionResult ProjectRuntimeBridge::recallSnapshot(
    std::string_view bankId,
    std::string_view snapshotId)
{
    SnapshotPerformanceActionResult failure;
    if (!snapshotPerformanceDocumentIsCurrent(failure))
        return failure;
    const auto* bank = findPerformanceBank(
        editor_.document(),
        bankId);
    if (bank == nullptr)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::ScopeMismatch,
            "Snapshot bank does not exist");
    }
    const auto* snapshot = findPerformanceSnapshot(
        *bank,
        snapshotId);
    if (snapshot == nullptr)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::MissingParameter,
            "Snapshot does not exist in the selected bank");
    }
    std::vector<talas::project::SnapshotValueEntry> values;
    std::vector<talas::project::SnapshotValueEntry> currentValues;
    if (!authoritativeSnapshotPerformanceValues(currentValues, failure))
        return failure;
    if (!resolveSnapshotPerformanceValues(
            *bank,
            *snapshot,
            currentValues,
            values,
            failure))
    {
        return failure;
    }
    SnapshotPerformanceState candidate;
    if (!prepareSnapshotPerformanceState(
            values,
            candidate,
            failure))
    {
        return failure;
    }
    try
    {
        candidate.activeBankId = bank->bankId;
        candidate.activeSnapshotId = snapshot->snapshotId;
        candidate.morphActive = false;
        candidate.morphLeftSnapshotId.clear();
        candidate.morphRightSnapshotId.clear();
        candidate.morphGestureStartValues.clear();
    }
    catch (const std::bad_alloc&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot recall state allocation failed");
    }
    catch (const std::length_error&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot recall state exceeded supported limits");
    }
    auto result = publishSnapshotPerformanceValues(
        *bank,
        std::move(values),
        candidate);
    if (result.ok())
        snapshotPerformanceState_ = std::move(candidate);
    return result;
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::beginSnapshotMorph(
    std::string_view bankId,
    std::string_view leftSnapshotId,
    std::string_view rightSnapshotId)
{
    SnapshotPerformanceActionResult failure;
    if (!snapshotPerformanceDocumentIsCurrent(failure))
        return failure;
    const auto* bank = findPerformanceBank(
        editor_.document(),
        bankId);
    if (bank == nullptr)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::ScopeMismatch,
            "Morph bank does not exist");
    }
    const auto* left = findPerformanceSnapshot(
        *bank,
        leftSnapshotId);
    const auto* right = findPerformanceSnapshot(
        *bank,
        rightSnapshotId);
    if (left == nullptr || right == nullptr)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::MissingParameter,
            "Morph endpoint snapshot does not exist");
    }
    std::vector<talas::project::SnapshotValueEntry> leftValues;
    std::vector<talas::project::SnapshotValueEntry> rightValues;
    std::vector<talas::project::SnapshotValueEntry> currentValues;
    if (!authoritativeSnapshotPerformanceValues(currentValues, failure))
        return failure;
    if (!resolveSnapshotPerformanceValues(
            *bank,
            *left,
            currentValues,
            leftValues,
            failure)
        || !resolveSnapshotPerformanceValues(
            *bank,
            *right,
            currentValues,
            rightValues,
            failure))
    {
        return failure;
    }
    talas::project::ProjectSnapshot resolvedLeft = *left;
    talas::project::ProjectSnapshot resolvedRight = *right;
    resolvedLeft.values = leftValues;
    resolvedRight.values = rightValues;
    const auto compatible = talas::project::morphSnapshotValues(
        resolvedLeft,
        resolvedRight,
        currentValues,
        bank->parameterPolicies,
        0.0);
    if (!compatible.ok())
    {
        failure.diagnostics = compatible.diagnostics;
        failure.message = "Morph endpoints are incompatible";
        return failure;
    }

    SnapshotPerformanceState candidate;
    try
    {
        candidate = snapshotPerformanceState_;
        candidate.activeBankId = bank->bankId;
        candidate.activeSnapshotId.clear();
        candidate.morphLeftSnapshotId = left->snapshotId;
        candidate.morphRightSnapshotId = right->snapshotId;
        candidate.morphGestureStartValues.clear();
        candidate.morphGestureStartValues.reserve(
            leftValues.size());
        for (const auto& endpoint : leftValues)
        {
            const auto current = std::ranges::find(
                currentValues,
                endpoint.target,
                &talas::project::SnapshotValueEntry::target);
            candidate.morphGestureStartValues.push_back(
                current == currentValues.end()
                    ? endpoint
                    : *current);
        }
        candidate.morphPosition = 0.0;
        candidate.morphActive = true;
    }
    catch (const std::bad_alloc&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot morph gesture allocation failed");
    }
    catch (const std::length_error&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot morph gesture exceeded supported limits");
    }
    snapshotPerformanceState_ = std::move(candidate);
    SnapshotPerformanceActionResult result;
    result.outcome =
        SnapshotPerformanceActionOutcome::GesturePrepared;
    return result;
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::updateSnapshotMorph(double amount)
{
    SnapshotPerformanceActionResult failure;
    if (!snapshotPerformanceDocumentIsCurrent(failure))
        return failure;
    if (!snapshotPerformanceState_.morphActive)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::StageFailed,
            "No snapshot morph gesture is active");
    }
    const auto* bank = findPerformanceBank(
        editor_.document(),
        snapshotPerformanceState_.activeBankId);
    const auto* left = bank == nullptr
        ? nullptr
        : findPerformanceSnapshot(
            *bank,
            snapshotPerformanceState_.morphLeftSnapshotId);
    const auto* right = bank == nullptr
        ? nullptr
        : findPerformanceSnapshot(
            *bank,
            snapshotPerformanceState_.morphRightSnapshotId);
    if (bank == nullptr || left == nullptr || right == nullptr)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::MissingParameter,
            "Active morph endpoints no longer exist");
    }
    const auto morphed = talas::project::morphSnapshotValues(
        *left,
        *right,
        snapshotPerformanceState_.morphGestureStartValues,
        bank->parameterPolicies,
        amount);
    if (!morphed.ok())
    {
        failure.diagnostics = morphed.diagnostics;
        failure.message = "Snapshot morph failed";
        return failure;
    }
    SnapshotPerformanceState candidate;
    if (!prepareSnapshotPerformanceState(
            morphed.values,
            candidate,
            failure))
    {
        return failure;
    }
    candidate.morphPosition = std::clamp(amount, 0.0, 1.0);
    candidate.activeSnapshotId.clear();
    auto result = publishSnapshotPerformanceValues(
        *bank,
        std::move(morphed.values),
        candidate);
    if (result.ok())
        snapshotPerformanceState_ = std::move(candidate);
    return result;
}

void ProjectRuntimeBridge::endSnapshotMorph() noexcept
{
    snapshotPerformanceState_.morphActive = false;
    snapshotPerformanceState_.morphGestureStartValues.clear();
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::randomizeSnapshotBank(
    std::string_view bankId,
    std::uint64_t seed)
{
    SnapshotPerformanceActionResult failure;
    if (!snapshotPerformanceDocumentIsCurrent(failure))
        return failure;
    const auto* bank = findPerformanceBank(
        editor_.document(),
        bankId);
    if (bank == nullptr)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::ScopeMismatch,
            "Randomization bank does not exist");
    }
    talas::expr::EvaluationContext evaluation;
    evaluation.sampleRate = projectSnapshotRenderSettings_->sampleRate;
    evaluation.tempoBpm = projectSnapshotRenderSettings_->tempoBpm;
    evaluation.rampsStartAtFirstValue = projectSnapshotRenderSettings_->rampsStartAtFirstValue;
    evaluation.rampsHoldUntriggered = projectSnapshotRenderSettings_->rampsHoldUntriggered;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(
        editor_.document(),
        registry_,
        bank->scope,
        evaluation);
    if (!resolved.ok())
    {
        failure.diagnostics = resolved.diagnostics;
        failure.message = "Randomization scope cannot resolve";
        return failure;
    }
    std::vector<talas::project::SnapshotValueEntry> currentValues;
    if (!authoritativeSnapshotPerformanceValues(currentValues, failure))
        return failure;
    std::vector<talas::project::SnapshotRandomizationTarget> targets;
    targets.reserve(resolved.targets.size());
    for (const auto& resolvedTarget : resolved.targets)
    {
        const auto current = std::ranges::find_if(
            currentValues,
            [&](const auto& candidate) {
                return candidate.target == resolvedTarget.target;
            });
        if (current == currentValues.end())
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::MissingParameter,
                "Randomization target has no current performance value",
                &resolvedTarget.target);
        }
        talas::project::SnapshotRandomizationTarget target;
        target.current = *current;
        target.choices = resolvedTarget.choices;
        const auto policy = std::ranges::find_if(
            bank->parameterPolicies,
            [&](const auto& candidate) {
                return candidate.target == resolvedTarget.target;
            });
        if (policy != bank->parameterPolicies.end())
            target.locked = policy->locked;
        // Locking a target without a policy stores one without a range; it
        // keeps the initial range the Workbench shows for that target.
        target.range = policy != bank->parameterPolicies.end()
                && policy->range.has_value()
            ? policy->range
            : resolvedTarget.initialRandomRange;
        targets.push_back(std::move(target));
    }
    auto randomized = talas::project::randomizeSnapshotValues(
        targets,
        seed);
    if (!randomized.ok())
    {
        failure.diagnostics = randomized.diagnostics;
        failure.message = "Snapshot randomization failed";
        return failure;
    }
    SnapshotPerformanceState candidate;
    if (!prepareSnapshotPerformanceState(
            randomized.values,
            candidate,
            failure))
    {
        return failure;
    }
    try
    {
        candidate.activeBankId = bank->bankId;
        candidate.activeSnapshotId.clear();
        candidate.morphActive = false;
        candidate.morphLeftSnapshotId.clear();
        candidate.morphRightSnapshotId.clear();
        candidate.morphGestureStartValues.clear();
        candidate.lastRandomSeed = seed;
    }
    catch (const std::bad_alloc&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot randomization state allocation failed");
    }
    catch (const std::length_error&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot randomization state exceeded supported limits");
    }
    auto result = publishSnapshotPerformanceValues(
        *bank,
        std::move(randomized.values),
        candidate);
    if (result.ok())
        snapshotPerformanceState_ = std::move(candidate);
    return result;
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::randomMergeSnapshotBank(
    std::string_view bankId,
    std::uint64_t seed,
    double amount)
{
    SnapshotPerformanceActionResult failure;
    if (!snapshotPerformanceDocumentIsCurrent(failure))
        return failure;
    const auto* bank = findPerformanceBank(
        editor_.document(),
        bankId);
    if (bank == nullptr)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::ScopeMismatch,
            "Random-merge bank does not exist");
    }
    talas::expr::EvaluationContext evaluation;
    evaluation.sampleRate = projectSnapshotRenderSettings_->sampleRate;
    evaluation.tempoBpm = projectSnapshotRenderSettings_->tempoBpm;
    evaluation.rampsStartAtFirstValue = projectSnapshotRenderSettings_->rampsStartAtFirstValue;
    evaluation.rampsHoldUntriggered = projectSnapshotRenderSettings_->rampsHoldUntriggered;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(
        editor_.document(),
        registry_,
        bank->scope,
        evaluation);
    if (!resolved.ok())
    {
        failure.diagnostics = resolved.diagnostics;
        failure.message = "Random-merge scope cannot resolve";
        return failure;
    }
    std::vector<talas::project::SnapshotValueEntry> currentValues;
    if (!authoritativeSnapshotPerformanceValues(currentValues, failure))
        return failure;
    std::vector<talas::project::SnapshotRandomizationTarget> targets;
    targets.reserve(resolved.targets.size());
    for (const auto& resolvedTarget : resolved.targets)
    {
        const auto current = std::ranges::find_if(
            currentValues,
            [&](const auto& candidate) {
                return candidate.target == resolvedTarget.target;
            });
        if (current == currentValues.end())
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::MissingParameter,
                "Random-merge target has no current performance value",
                &resolvedTarget.target);
        }
        talas::project::SnapshotRandomizationTarget target;
        target.current = *current;
        target.choices = resolvedTarget.choices;
        const auto policy = std::ranges::find_if(
            bank->parameterPolicies,
            [&](const auto& candidate) {
                return candidate.target == resolvedTarget.target;
            });
        if (policy != bank->parameterPolicies.end())
            target.locked = policy->locked;
        // Locking a target without a policy stores one without a range; it
        // keeps the initial range the Workbench shows for that target.
        target.range = policy != bank->parameterPolicies.end()
                && policy->range.has_value()
            ? policy->range
            : resolvedTarget.initialRandomRange;
        targets.push_back(std::move(target));
    }
    auto randomized = talas::project::randomMergeSnapshotValues(
        targets,
        seed,
        amount);
    if (!randomized.ok())
    {
        failure.diagnostics = randomized.diagnostics;
        failure.message = "Snapshot random merge failed";
        return failure;
    }
    SnapshotPerformanceState candidate;
    if (!prepareSnapshotPerformanceState(
            randomized.values,
            candidate,
            failure))
    {
        return failure;
    }
    try
    {
        candidate.activeBankId = bank->bankId;
        candidate.activeSnapshotId.clear();
        candidate.morphActive = false;
        candidate.morphLeftSnapshotId.clear();
        candidate.morphRightSnapshotId.clear();
        candidate.morphGestureStartValues.clear();
        candidate.lastRandomSeed = seed;
    }
    catch (const std::bad_alloc&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot random-merge state allocation failed");
    }
    catch (const std::length_error&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot random-merge state exceeded supported limits");
    }
    auto result = publishSnapshotPerformanceValues(
        *bank,
        std::move(randomized.values),
        candidate);
    if (result.ok())
        snapshotPerformanceState_ = std::move(candidate);
    return result;
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::resetSnapshotPerformance()
{
    SnapshotPerformanceActionResult failure;
    if (!snapshotPerformanceDocumentIsCurrent(failure))
        return failure;
    SnapshotPerformanceState candidate;
    try
    {
        candidate.currentValues = projectSnapshotBaselineValues_;
        candidate.projectRevision =
            snapshotPerformanceState_.projectRevision;
    }
    catch (const std::bad_alloc&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot performance reset state allocation failed");
    }
    catch (const std::length_error&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot performance reset exceeded supported limits");
    }
    const auto staged = compileAndStagePerformanceOverlay(
        {},
        0.0);
    if (!staged.ok())
    {
        return snapshotPerformanceFailure(
            staged.outcome == ProjectStageOutcome::TopologyMismatch
                ? talas::project::SnapshotDiagnosticCode::TopologyMismatch
                : talas::project::SnapshotDiagnosticCode::StageFailed,
            staged.message.empty()
                ? "Snapshot performance reset failed"
                : staged.message);
    }
    snapshotPerformanceState_ = std::move(candidate);
    SnapshotPerformanceActionResult result;
    result.outcome = SnapshotPerformanceActionOutcome::Reset;
    return result;
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::captureSnapshotPerformance(
    std::string_view bankId,
    std::string_view snapshotId)
{
    SnapshotPerformanceActionResult failure;
    if (!snapshotPerformanceDocumentIsCurrent(failure))
        return failure;
    const auto* bank = findPerformanceBank(
        editor_.document(),
        bankId);
    const auto* snapshot = bank == nullptr
        ? nullptr
        : findPerformanceSnapshot(*bank, snapshotId);
    if (bank == nullptr || snapshot == nullptr)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::MissingParameter,
            "Capture bank or snapshot does not exist");
    }
    talas::expr::EvaluationContext evaluation;
    evaluation.sampleRate = projectSnapshotRenderSettings_->sampleRate;
    evaluation.tempoBpm = projectSnapshotRenderSettings_->tempoBpm;
    evaluation.rampsStartAtFirstValue = projectSnapshotRenderSettings_->rampsStartAtFirstValue;
    evaluation.rampsHoldUntriggered = projectSnapshotRenderSettings_->rampsHoldUntriggered;
    const auto resolved = talas::project::resolveSnapshotScopeTargets(
        editor_.document(),
        registry_,
        bank->scope,
        evaluation);
    if (!resolved.ok())
    {
        failure.diagnostics = resolved.diagnostics;
        failure.message = "Capture scope cannot resolve";
        return failure;
    }
    std::vector<talas::project::SnapshotValueEntry> scopeCurrentValues;
    try
    {
        scopeCurrentValues.reserve(resolved.targets.size());
        const auto preparedVariables = graphHost_.preparedVariableValues();
        const auto preparedVariablesCurrent =
            preparedVariables.publicationValid
            && preparedVariables.stageId == latestSnapshotStageId_;
        for (const auto& target : resolved.targets)
        {
            if (target.target.kind
                == talas::project::SnapshotTargetKind::Variable)
            {
                if (preparedVariablesCurrent)
                {
                    if (auto current = preparedVariableSnapshotValue(
                            target.target, preparedVariables))
                    {
                        scopeCurrentValues.push_back(
                            std::move(*current));
                    }
                }
                continue;
            }
            const auto current = std::ranges::find_if(
                snapshotPerformanceState_.currentValues,
                [&](const auto& candidate) {
                    return candidate.target == target.target;
                });
            if (current != snapshotPerformanceState_.currentValues.end())
                scopeCurrentValues.push_back(*current);
        }
    }
    catch (const std::bad_alloc&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Current variable snapshot allocation failed");
    }
    catch (const std::length_error&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Current variable snapshot exceeded supported limits");
    }
    auto captured = talas::project::captureSnapshotScopeValues(
        resolved.targets,
        scopeCurrentValues);
    if (!captured.ok())
    {
        failure.diagnostics = std::move(captured.diagnostics);
        failure.message = "Current performance state is incomplete";
        return failure;
    }
    std::string capturedAuthoringBytes;
    try
    {
        auto candidate = editor_.document();
        auto candidateBank = std::ranges::find(
            candidate.snapshotLibrary.banks,
            bankId,
            &talas::project::ProjectSnapshotBank::bankId);
        if (candidateBank
                == candidate.snapshotLibrary.banks.end())
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::MissingParameter,
                "Capture candidate bank disappeared");
        }
        auto candidateSnapshot = std::ranges::find(
            candidateBank->snapshots,
            snapshotId,
            &talas::project::ProjectSnapshot::snapshotId);
        if (candidateSnapshot == candidateBank->snapshots.end())
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::MissingParameter,
                "Capture candidate snapshot disappeared");
        }
        candidateSnapshot->values = captured.values;
        capturedAuthoringBytes =
            talas::project::serializeProjectDocument(candidate, registry_);
    }
    catch (const std::bad_alloc&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Explicit capture candidate allocation failed");
    }
    catch (const std::length_error&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Explicit capture candidate exceeded supported limits");
    }
    const auto edited = editor_.captureSnapshotValues(
        bankId,
        snapshotId,
        std::move(captured.values));
    if (!edited.ok())
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::StageFailed,
            edited.message.empty()
                ? "Explicit snapshot performance capture failed"
                : edited.message);
    }
    stagedSnapshotAuthoringBytes_ =
        std::move(capturedAuthoringBytes);
    SnapshotPerformanceActionResult result;
    result.outcome =
        SnapshotPerformanceActionOutcome::CapturedProjectEdit;
    return result;
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::adoptSnapshotAuthoringEdit()
{
    if (stagedSnapshotAuthoringBytes_.empty()
        || !latestSnapshotAuthoringTopology_.has_value()
        || !projectSnapshotRenderSettings_.has_value())
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::
                StalePreparedGeneration,
            "Snapshot authoring edits require an accepted project stage");
    }
    if (nextSnapshotProjectRevision_
        == std::numeric_limits<std::uint64_t>::max())
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot authoring revision identity space is exhausted");
    }
    std::string authoringBytes;
    SnapshotExecutionTopologyFingerprint authoringTopology;
    SnapshotPerformanceState candidate;
    try
    {
        authoringBytes = talas::project::serializeProjectDocument(
            editor_.document(), registry_);
        authoringTopology = snapshotExecutionTopologyFingerprint(
            editor_.document(), registry_);
        candidate = snapshotPerformanceState_;
        candidate.projectRevision = nextSnapshotProjectRevision_;
        candidate.activeSnapshotId.clear();
        candidate.morphActive = false;
        candidate.morphLeftSnapshotId.clear();
        candidate.morphRightSnapshotId.clear();
        candidate.morphGestureStartValues.clear();
        if (findPerformanceBank(
                editor_.document(),
                candidate.activeBankId) == nullptr)
        {
            candidate.activeBankId.clear();
        }
    }
    catch (const std::bad_alloc&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot authoring revision allocation failed");
    }
    catch (const std::length_error&)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::CapacityExceeded,
            "Snapshot authoring revision exceeded supported limits");
    }
    if (authoringTopology != *latestSnapshotAuthoringTopology_)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::TopologyMismatch,
            "Snapshot authoring adoption rejected a structural project edit");
    }
    stagedSnapshotAuthoringBytes_ = std::move(authoringBytes);
    snapshotPerformanceState_ = std::move(candidate);
    ++nextSnapshotProjectRevision_;
    SnapshotPerformanceActionResult result;
    result.outcome =
        SnapshotPerformanceActionOutcome::AdoptedAuthoringEdit;
    return result;
}

bool ProjectRuntimeBridge::snapshotTransitionActive() const noexcept
{
    return graphHost_.sameTopologyTransitionActive();
}

bool ProjectRuntimeBridge::snapshotTransitionCoalesced() const noexcept
{
    return coalescedSnapshotOverlay_ != nullptr;
}

void ProjectRuntimeBridge::resetResourceIdentityHistory() noexcept
{
    acceptedResourceIdentities_.clear();
    resetVariableValueHistory();
    latestSnapshotAuthoringTopology_.reset();
    stagedSnapshotAuthoringBytes_.clear();
    snapshotPerformanceState_ = {};
    projectSnapshotBaselineValues_.clear();
    coalescedSnapshotOverlay_.reset();
}

void ProjectRuntimeBridge::resetVariableValueHistory() noexcept
{
    latestPreparedVariables_ = {};
    latestPreparedVariableStageGeneration_ = 0;
    latestPreparedSoundVariables_ = {};
    latestPreparedSoundVariableStageGeneration_ = 0;
    latestVariableStageIsEditorDocument_ = false;
}

std::optional<AcceptedResourceIdentity>
ProjectRuntimeBridge::acceptedResourceIdentity(
    std::string_view assetId) const
{
    const auto accepted =
        acceptedResourceIdentities_.find(
            std::string{assetId});
    return accepted
            == acceptedResourceIdentities_.end()
        ? std::optional<AcceptedResourceIdentity>{}
        : std::optional<AcceptedResourceIdentity>{
              accepted->second};
}

void ProjectRuntimeBridge::setPlaying(bool playing) noexcept
{
    if (midiClockAuthorityGeneration_.load(
            std::memory_order_acquire) != 0
        || mtcAuthorityGeneration_.load(
            std::memory_order_acquire) != 0)
    {
        return;
    }
    if (!playing)
    {
        stopControllerRecording(
            talas::project::ControllerRecordingTerminal::Completed);
    }
    publishInternalTransportRequest(playing);
    if (midiClockAuthorityGeneration_.load(
            std::memory_order_acquire) != 0
        || mtcAuthorityGeneration_.load(
            std::memory_order_acquire) != 0)
    {
        publishInternalTransportRequest(false);
        return;
    }
}

void ProjectRuntimeBridge::setCompiledLiveSetSounds(
    std::unordered_map<std::string, talas::project::ProjectDocument> programs)
{
    compiledLiveSetSounds_ = std::move(programs);
}

bool ProjectRuntimeBridge::requestTransportRelocation(std::uint64_t absoluteFrame) noexcept
{
    const auto controllerRecordingEndFrame = graphHost_.transportSnapshot().absoluteFrame;
    const auto generation = internalTransportRelocationGeneration_.load(std::memory_order_relaxed);
    if (generation == std::numeric_limits<std::uint64_t>::max()
        || midiClockAuthorityGeneration_.load(std::memory_order_acquire) != 0
        || mtcAuthorityGeneration_.load(std::memory_order_acquire) != 0) return false;
    const auto stageId = acceptedMidiStageId_.load(std::memory_order_acquire);
    if (!stageId) return false;
    const auto midiEpoch = midiClockStopRequestGeneration_.load(std::memory_order_acquire);
    const auto mtcEpoch = mtcStopRequestGeneration_.load(std::memory_order_acquire);
    const auto resetEpoch = internalTransportResetRequest_.load(std::memory_order_acquire);
    (void) collectRetiredTransportRelocations();
    try
    {
        std::string error;
        auto sources = graphHost_.prepareTransportRelocation(absoluteFrame, error);
        if (!sources) return false;
        auto request = std::make_unique<TransportRelocationRequest>();
        request->sources = std::move(sources);
        request->stageId = stageId;
        request->midiEpoch = midiEpoch; request->mtcEpoch = mtcEpoch; request->resetEpoch = resetEpoch;
        if (acceptedMidiStageId_.load(std::memory_order_acquire) != stageId
            || midiClockAuthorityGeneration_.load(std::memory_order_acquire) != 0
            || mtcAuthorityGeneration_.load(std::memory_order_acquire) != 0
            || midiClockStopRequestGeneration_.load(std::memory_order_acquire) != midiEpoch
            || mtcStopRequestGeneration_.load(std::memory_order_acquire) != mtcEpoch
            || internalTransportResetRequest_.load(std::memory_order_acquire) != resetEpoch) return false;
        delete pendingTransportRelocation_.exchange(request.release(), std::memory_order_acq_rel);
        internalTransportRelocationGeneration_.store(generation + 1, std::memory_order_release);
    }
    catch (...) { return false; }
    controllerRecordingSeekEndFrame_.store(controllerRecordingEndFrame, std::memory_order_relaxed);
    stopControllerRecording(talas::project::ControllerRecordingTerminal::Seeked);
    return true;
}

void ProjectRuntimeBridge::stopTransport() noexcept
{
    if (midiClockAuthorityGeneration_.load(
            std::memory_order_acquire) != 0
        || mtcAuthorityGeneration_.load(
            std::memory_order_acquire) != 0)
    {
        return;
    }
    stopControllerRecording(
        talas::project::ControllerRecordingTerminal::Completed);
    publishInternalTransportRequest(false);
    realtimeControlGeneration_.fetch_add(1, std::memory_order_acq_rel);
    internalTransportResetRequest_.fetch_add(
        1, std::memory_order_release);
}

void ProjectRuntimeBridge::publishInternalTransportRequest(
    bool playing) noexcept
{
    auto current = internalTransportRequest_.load(
        std::memory_order_relaxed);
    while (true)
    {
        const auto generation = current & ~std::uint64_t{1};
        const auto nextGeneration = generation
                <= std::numeric_limits<std::uint64_t>::max() - 2
            ? generation + 2
            : generation;
        const auto next = nextGeneration
            | static_cast<std::uint64_t>(playing);
        if (internalTransportRequest_.compare_exchange_weak(
                current,
                next,
                std::memory_order_release,
                std::memory_order_relaxed))
        {
            return;
        }
    }
}

std::uint64_t ProjectRuntimeBridge::transportFramePosition() const noexcept
{
    return graphHost_.transportFramePosition();
}

std::optional<std::uint64_t> ProjectRuntimeBridge::scorePlaybackFrame() const noexcept
{
    return graphHost_.scorePlaybackFrame();
}

std::shared_ptr<const PreparedScoreSchedule>
ProjectRuntimeBridge::latestPreparedScoreSchedule() const noexcept
{
    return latestPreparedScoreSchedule_;
}

std::shared_ptr<const PreparedLiveSet>
ProjectRuntimeBridge::latestPreparedLiveSet() const noexcept
{
    return std::atomic_load_explicit(
        &latestPreparedLiveSet_,
        std::memory_order_acquire);
}

std::shared_ptr<const PreparedLiveTools>
ProjectRuntimeBridge::latestPreparedLiveTools() const noexcept
{
    return std::atomic_load_explicit(
        &latestPreparedLiveTools_,
        std::memory_order_acquire);
}

bool ProjectRuntimeBridge::liveSetActivationReady() const noexcept
{
    const auto prepared = std::atomic_load_explicit(
        &latestPreparedLiveSet_, std::memory_order_acquire);
    if (prepared == nullptr)
        return false;
    const auto& binding = prepared->binding();
    const auto transport = graphHost_.transportSnapshot();
    return binding.editorDocumentAuthority
        && binding.documentAuthorityGeneration
            == editor_.documentAuthorityGeneration()
        && binding.stageGeneration
            == latestStageGeneration_.load(std::memory_order_acquire)
        && graphHost_.activeStageId() == binding.stageId
        && transport.epoch != 0 && std::isfinite(transport.sampleRate)
        && transport.sampleRate > 0.0;
}

LiveSetActivationResult
ProjectRuntimeBridge::requestLiveSetActivation(std::string_view liveSetId,
                                               std::string_view cellId,
                                               RealtimeLiveSetSourceKind sourceKind,
                                               std::uint64_t sourceGeneration,
                                               std::uint64_t routeGeneration) noexcept
{
    const auto sourceIndex = static_cast<std::size_t>(sourceKind);
    if (sourceIndex >= liveSetSourceAuthorities_.size())
    {
        return {
            LiveSetActivationOutcome::StaleAuthority,
            0,
            0,
            "Live Set activation source is invalid",
        };
    }
    const auto prepared = std::atomic_load_explicit(
        &latestPreparedLiveSet_,
        std::memory_order_acquire);
    if (prepared == nullptr)
    {
        return {
            LiveSetActivationOutcome::NotPrepared,
            0,
            0,
            "No accepted Live Set is prepared",
        };
    }
    const auto cellIndex = prepared->findCell(liveSetId, cellId);
    if (!cellIndex.has_value() || *cellIndex > std::numeric_limits<std::uint16_t>::max())
    {
        return {
            LiveSetActivationOutcome::UnknownCell,
            0,
            0,
            "Live Set cell does not exist",
        };
    }
    const auto& cell = prepared->cells()[*cellIndex];
    const auto effect = preparedLiveSetEffect(*prepared, *cellIndex);
    if (!cell.ready() || !effect.has_value())
    {
        return {
            LiveSetActivationOutcome::CellFailed,
            0,
            0,
            "Live Set cell preparation failed",
        };
    }
    if (sourceGeneration == 0 || routeGeneration == 0)
    {
        return {
            LiveSetActivationOutcome::StaleAuthority,
            0,
            0,
            "Live Set activation requires exact source and route authority",
        };
    }
    std::uint64_t publishedSourceGeneration = 0;
    std::uint64_t publishedRouteGeneration = 0;
    if (!readLiveSetSourceAuthority(
            sourceIndex, publishedSourceGeneration, publishedRouteGeneration) ||
        publishedSourceGeneration != sourceGeneration ||
        publishedRouteGeneration != routeGeneration)
    {
        return {
            LiveSetActivationOutcome::StaleAuthority,
            0,
            0,
            "Live Set source or route authority is stale",
        };
    }

    const auto transport = graphHost_.transportSnapshot();
    const auto& binding = prepared->binding();
    if ((binding.editorDocumentAuthority &&
         editor_.documentAuthorityGeneration() != binding.documentAuthorityGeneration) ||
        transport.epoch == 0 || !std::isfinite(transport.sampleRate) ||
        transport.sampleRate <= 0.0 || graphHost_.activeStageId() != binding.stageId ||
        latestStageGeneration_.load(std::memory_order_acquire) != binding.stageGeneration)
    {
        return {
            LiveSetActivationOutcome::StaleAuthority,
            0,
            0,
            "Live Set stage or transport authority is stale",
        };
    }
    const auto resolvedFrame = preparedLiveSetLaunchFrame(
        *prepared,
        *cellIndex,
        transport.absoluteFrame,
        transport.loopEnabled
            ? std::optional<PreparedLiveSetTransportLoop>{{
                  transport.loopStartFrame, transport.loopEndFrame}}
            : std::nullopt);
    if (!resolvedFrame.has_value())
    {
        return {
            LiveSetActivationOutcome::QuantizationFailed,
            0,
            0,
            "Live Set quantization could not resolve an exact future frame",
        };
    }

    auto generation = liveSetRequestGeneration_.load(std::memory_order_relaxed);
    auto published = false;
    for (std::size_t attempt = 0; attempt < RealtimeLiveSetIngress::maximumProducerAttempts;
         ++attempt)
    {
        if (generation == std::numeric_limits<std::uint64_t>::max())
        {
            return {
                LiveSetActivationOutcome::IdentityExhausted,
                0,
                0,
                "Live Set request identity space is exhausted",
            };
        }
        if (liveSetRequestGeneration_.compare_exchange_strong(
                generation, generation + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            ++generation;
            published = true;
            break;
        }
    }
    if (!published)
    {
        return {
            LiveSetActivationOutcome::QueueFull,
            0,
            0,
            "Live Set request publication is busy",
        };
    }

    const auto targetKind = effect->kind == PreparedLiveSetEffectKind::ActivateSound
        ? RealtimeLiveSetTargetKind::Sound
        : effect->kind == PreparedLiveSetEffectKind::ActivateOverlaySnapshot
                || effect->kind == PreparedLiveSetEffectKind::ApplyDirectSnapshot
            ? RealtimeLiveSetTargetKind::Snapshot
            : effect->kind == PreparedLiveSetEffectKind::SeekScoreRegion
                ? RealtimeLiveSetTargetKind::ScoreRegion
                : effect->kind == PreparedLiveSetEffectKind::SetTrackMute
                    ? RealtimeLiveSetTargetKind::TrackMute
                : RealtimeLiveSetTargetKind::Stop;
    const RealtimeLiveSetRequest request{
        .targetKind = targetKind,
        .sourceKind = sourceKind,
        .preparedCellIndex = static_cast<std::uint16_t>(*cellIndex),
        .resolvedFrame = *resolvedFrame,
        .documentAuthorityGeneration = binding.documentAuthorityGeneration,
        .stageGeneration = binding.stageGeneration,
        .stageId = binding.stageId,
        .transportEpoch = transport.epoch,
        .transportDiscontinuitySequence = transport.discontinuitySequence,
        .sourceGeneration = sourceGeneration,
        .routeGeneration = routeGeneration,
        .requestGeneration = generation,
    };
    if (!liveSetIngress_.tryPush(request))
    {
        liveSetQueueOverflowCount_.fetch_add(1, std::memory_order_relaxed);
        return {
            LiveSetActivationOutcome::QueueFull,
            generation,
            *resolvedFrame,
            "Live Set realtime ingress is full",
        };
    }
    liveSetAcceptedRequestCount_.fetch_add(1, std::memory_order_relaxed);
    return {
        LiveSetActivationOutcome::Accepted,
        generation,
        *resolvedFrame,
        "Live Set activation accepted",
    };
}

void ProjectRuntimeBridge::publishLiveSetSourceAuthority(RealtimeLiveSetSourceKind sourceKind,
                                                         std::uint64_t sourceGeneration,
                                                         std::uint64_t routeGeneration) noexcept
{
    const auto index = static_cast<std::size_t>(sourceKind);
    if (index >= liveSetSourceAuthorities_.size())
        return;
    liveSetSourceAuthorities_[index].publish(sourceGeneration, routeGeneration);
}

void ProjectRuntimeBridge::revokeLiveSetSourceAuthority(
    RealtimeLiveSetSourceKind sourceKind) noexcept
{
    const auto index = static_cast<std::size_t>(sourceKind);
    if (index < liveSetSourceAuthorities_.size())
        liveSetSourceAuthorities_[index].clear();
}

bool ProjectRuntimeBridge::readLiveSetSourceAuthority(std::size_t sourceIndex,
                                                      std::uint64_t& sourceGeneration,
                                                      std::uint64_t& routeGeneration) const noexcept
{
    if (sourceIndex >= liveSetSourceAuthorities_.size())
        return false;
    return liveSetSourceAuthorities_[sourceIndex].read(
        sourceGeneration, routeGeneration);
}

RealtimeLiveSetTelemetrySnapshot ProjectRuntimeBridge::realtimeLiveSetTelemetry() const noexcept
{
    RealtimeLiveSetActiveState active;
    static_cast<void>(liveSetActiveState_.read(active));
    RealtimeLiveSetTerminalState terminal;
    static_cast<void>(liveSetTerminalState_.read(terminal));
    return {
        liveSetAcceptedRequestCount_.load(std::memory_order_relaxed),
        liveSetCompletedActivationCount_.load(std::memory_order_relaxed),
        liveSetQueueOverflowCount_.load(std::memory_order_relaxed),
        liveSetStaleAuthorityCount_.load(std::memory_order_relaxed),
        liveSetFailedActivationCount_.load(std::memory_order_relaxed),
        terminal.requestGeneration,
        terminal.outcome,
        active.requestGeneration,
        active.cellIndexPlusOne,
        active.scoreRegionActive,
        active.soundCellIndexPlusOneByColumn,
        active.mutedSoundColumnsMask,
    };
}

void ProjectRuntimeBridge::publishLiveSetActiveState(
    std::uint64_t requestGeneration,
    std::uint32_t cellIndexPlusOne,
    bool scoreRegionActive) noexcept
{
    RealtimeLiveSetActiveState state{
        requestGeneration, cellIndexPlusOne, scoreRegionActive};
    for (std::uint32_t column = 0;
         column < state.soundCellIndexPlusOneByColumn.size(); ++column)
    {
        state.soundCellIndexPlusOneByColumn[column] =
            graphHost_.activePreparedLiveSetSoundCellIndexPlusOne(column);
        if (graphHost_.preparedLiveSetSoundColumnMuted(column))
            state.mutedSoundColumnsMask |= std::uint32_t{1} << column;
    }
    liveSetActiveState_.publish(state);
}

LiveToolTriggerResult ProjectRuntimeBridge::requestLiveToolTrigger(
    const LiveToolTriggerRequest& source) noexcept
{
    const auto sourceIndex = static_cast<std::size_t>(source.sourceKind);
    if (sourceIndex >= liveToolSourceAuthorities_.size())
    {
        return {LiveToolTriggerOutcome::InvalidPayload, 0,
                "Live Tool trigger source is invalid"};
    }
    const auto sourceMatchesTrigger =
        (source.triggerKind == talas::project::ProjectLiveToolTriggerKind::Manual
         && (source.sourceKind == RealtimeLiveToolSourceKind::Workbench
             || source.sourceKind == RealtimeLiveToolSourceKind::Offline))
        || (source.triggerKind == talas::project::ProjectLiveToolTriggerKind::EventValue
            && source.sourceKind == RealtimeLiveToolSourceKind::EventValue)
        || (source.triggerKind == talas::project::ProjectLiveToolTriggerKind::VariableChange
            && source.sourceKind == RealtimeLiveToolSourceKind::Variable)
        || (source.triggerKind == talas::project::ProjectLiveToolTriggerKind::MidiProgram
            && source.sourceKind == RealtimeLiveToolSourceKind::MidiProgram)
        || (source.triggerKind == talas::project::ProjectLiveToolTriggerKind::Osc
            && source.sourceKind == RealtimeLiveToolSourceKind::Osc)
        || (source.triggerKind == talas::project::ProjectLiveToolTriggerKind::PeriodicTicks
            && source.sourceKind == RealtimeLiveToolSourceKind::Periodic);
    if (!sourceMatchesTrigger
        || !std::isfinite(source.variableNumber)
        || source.identifier.size() > maximumRealtimeLiveToolIdentifierBytes)
    {
        return {LiveToolTriggerOutcome::InvalidPayload, 0,
                "Live Tool trigger payload is invalid"};
    }
    const auto prepared = std::atomic_load_explicit(
        &latestPreparedLiveTools_, std::memory_order_acquire);
    if (prepared == nullptr)
    {
        return {LiveToolTriggerOutcome::NotPrepared, 0,
                "No accepted Live Tool authority is prepared"};
    }
    const auto toolIndex = prepared->findTool(source.toolId);
    if (!toolIndex.has_value()
        || *toolIndex > std::numeric_limits<std::uint16_t>::max())
    {
        return {LiveToolTriggerOutcome::UnknownTool, 0,
                "Live Tool does not exist"};
    }
    std::uint64_t publishedSource = 0;
    std::uint64_t publishedRoute = 0;
    if (source.sourceGeneration == 0 || source.routeGeneration == 0
        || !liveToolSourceAuthorities_[sourceIndex].read(
            publishedSource, publishedRoute)
        || publishedSource != source.sourceGeneration
        || publishedRoute != source.routeGeneration)
    {
        return {LiveToolTriggerOutcome::StaleAuthority, 0,
                "Live Tool source or route authority is stale"};
    }
    const auto& binding = prepared->binding();
    if ((binding.editorDocumentAuthority
         && editor_.documentAuthorityGeneration()
            != binding.documentAuthorityGeneration)
        || latestStageGeneration_.load(std::memory_order_acquire)
            != binding.stageGeneration
        || graphHost_.activeStageId() != binding.stageId)
    {
        return {LiveToolTriggerOutcome::StaleAuthority, 0,
                "Live Tool document or stage authority is stale"};
    }
    auto generation = liveToolRequestGeneration_.load(
        std::memory_order_relaxed);
    auto published = false;
    for (std::size_t attempt = 0;
         attempt < RealtimeLiveToolIngress::maximumProducerAttempts;
         ++attempt)
    {
        if (generation == std::numeric_limits<std::uint64_t>::max())
        {
            return {LiveToolTriggerOutcome::IdentityExhausted, 0,
                    "Live Tool request identity space is exhausted"};
        }
        if (liveToolRequestGeneration_.compare_exchange_strong(
                generation,
                generation + 1U,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
        {
            ++generation;
            published = true;
            break;
        }
    }
    if (!published)
    {
        return {LiveToolTriggerOutcome::QueueFull, 0,
                "Live Tool request publication is busy"};
    }
    RealtimeLiveToolTriggerRequest request;
    request.triggerKind = source.triggerKind;
    request.sourceKind = source.sourceKind;
    request.preparedToolIndex = static_cast<std::uint16_t>(*toolIndex);
    request.domain = source.domain;
    request.variableNumber = source.variableNumber;
    request.variableBoolean = source.variableBoolean;
    request.hasVariableValue = source.hasVariableValue;
    request.midiChannel = source.midiChannel;
    request.midiProgram = source.midiProgram;
    request.musicalTick = source.musicalTick;
    request.documentAuthorityGeneration = binding.documentAuthorityGeneration;
    request.stageGeneration = binding.stageGeneration;
    request.stageId = binding.stageId;
    request.sourceGeneration = source.sourceGeneration;
    request.routeGeneration = source.routeGeneration;
    request.requestGeneration = generation;
    if (!setRealtimeLiveToolTriggerIdentifier(request, source.identifier)
        || !liveToolIngress_.tryPush(request))
    {
        liveToolQueueOverflowCount_.fetch_add(1, std::memory_order_relaxed);
        return {LiveToolTriggerOutcome::QueueFull, generation,
                "Live Tool realtime ingress is full"};
    }
    liveToolAcceptedTriggerCount_.fetch_add(1, std::memory_order_relaxed);
    return {LiveToolTriggerOutcome::Accepted, generation,
            "Live Tool trigger accepted"};
}

void ProjectRuntimeBridge::publishLiveToolSourceAuthority(
    RealtimeLiveToolSourceKind sourceKind,
    std::uint64_t sourceGeneration,
    std::uint64_t routeGeneration) noexcept
{
    const auto index = static_cast<std::size_t>(sourceKind);
    if (index < liveToolSourceAuthorities_.size())
        liveToolSourceAuthorities_[index].publish(
            sourceGeneration, routeGeneration);
}

void ProjectRuntimeBridge::revokeLiveToolSourceAuthority(
    RealtimeLiveToolSourceKind sourceKind) noexcept
{
    const auto index = static_cast<std::size_t>(sourceKind);
    if (index < liveToolSourceAuthorities_.size())
        liveToolSourceAuthorities_[index].clear();
}

RealtimeLiveToolTelemetrySnapshot
ProjectRuntimeBridge::realtimeLiveToolTelemetry() const noexcept
{
    return {
        liveToolAcceptedTriggerCount_.load(std::memory_order_relaxed),
        liveToolAppliedTransitionCount_.load(std::memory_order_relaxed),
        liveToolQueueOverflowCount_.load(std::memory_order_relaxed),
        liveToolStaleAuthorityCount_.load(std::memory_order_relaxed),
        liveToolRejectedTriggerCount_.load(std::memory_order_relaxed),
        liveToolNoMatchingTransitionCount_.load(std::memory_order_relaxed),
        liveToolStaleTriggerAuthorityCount_.load(std::memory_order_relaxed),
        liveToolInvalidTriggerCount_.load(std::memory_order_relaxed),
        liveToolGuardRejectedCount_.load(std::memory_order_relaxed),
        liveToolCommandRejectedCount_.load(std::memory_order_relaxed),
        liveToolStateInitializationRejectedCount_.load(
            std::memory_order_relaxed),
        liveToolActivateCellCommandCount_.load(std::memory_order_relaxed),
        liveToolStopCommandCount_.load(std::memory_order_relaxed),
        liveToolSetProjectVariableCommandCount_.load(
            std::memory_order_relaxed),
        liveToolSetWorkspaceVariableCommandCount_.load(
            std::memory_order_relaxed),
        liveToolSetEventValueCommandCount_.load(std::memory_order_relaxed),
    };
}

std::optional<PreparedLiveToolRuntimeState>
ProjectRuntimeBridge::realtimeLiveToolState() const noexcept
{
    PreparedLiveToolRuntimeState state;
    if (!liveToolStatePublication_.read(state))
        return std::nullopt;
    const auto prepared = std::atomic_load_explicit(
        &latestPreparedLiveTools_, std::memory_order_acquire);
    if (prepared == nullptr
        || state.documentAuthorityGeneration
            != prepared->binding().documentAuthorityGeneration
        || state.stageGeneration != prepared->binding().stageGeneration
        || state.stageId != prepared->binding().stageId)
    {
        return std::nullopt;
    }
    return state;
}

bool ProjectRuntimeBridge::requestLiveToolStateRestore(
    const PreparedLiveToolRuntimeState& source,
    std::uint64_t* acceptedRequestGeneration) noexcept
{
    if (acceptedRequestGeneration != nullptr)
        *acceptedRequestGeneration = 0;
    const auto prepared = std::atomic_load_explicit(
        &latestPreparedLiveTools_, std::memory_order_acquire);
    if (prepared == nullptr
        || source.documentAuthorityGeneration
            != prepared->binding().documentAuthorityGeneration
        || source.stageGeneration != prepared->binding().stageGeneration
        || source.stageId != prepared->binding().stageId)
    {
        return false;
    }
    PreparedLiveToolRuntimeState candidate;
    if (!initializePreparedLiveToolRuntimeState(*prepared, candidate)
        || source.toolCount != candidate.toolCount
        || source.variableCount != candidate.variableCount)
    {
        return false;
    }
    for (std::size_t toolIndex = 0;
         toolIndex < candidate.toolCount;
         ++toolIndex)
    {
        const auto& tool = prepared->tools()[toolIndex];
        if (source.variableOffsets[toolIndex]
                != candidate.variableOffsets[toolIndex]
            || source.variableCounts[toolIndex]
                != candidate.variableCounts[toolIndex]
            || source.stateIndices[toolIndex] >= tool.stateIds.size())
        {
            return false;
        }
        candidate.stateIndices[toolIndex] =
            source.stateIndices[toolIndex];
        const auto offset = candidate.variableOffsets[toolIndex];
        for (std::size_t variableIndex = 0;
             variableIndex < tool.variables.size();
             ++variableIndex)
        {
            const auto& declaration = tool.variables[variableIndex];
            const auto& value = source.variables[offset + variableIndex];
            if (!std::isfinite(value.number)
                || (declaration.kind
                        == talas::project::ProjectLiveToolVariableKind::Boolean
                    ? value.number != 0.0
                    : value.boolean))
            {
                return false;
            }
            candidate.variables[offset + variableIndex] = value;
        }
        candidate.lastTriggerSequences[toolIndex] = 0;
    }
    if (!liveToolRestorePublication_.publishAll(candidate))
        return false;
    auto generation = liveToolRestoreRequestGeneration_.load(
        std::memory_order_relaxed);
    for (std::size_t attempt = 0;
         attempt < RealtimeLiveToolIngress::maximumProducerAttempts;
         ++attempt)
    {
        if (generation == std::numeric_limits<std::uint64_t>::max())
            return false;
        if (liveToolRestoreRequestGeneration_.compare_exchange_strong(
                generation,
                generation + 1U,
                std::memory_order_release,
                std::memory_order_relaxed))
        {
            if (acceptedRequestGeneration != nullptr)
                *acceptedRequestGeneration = generation + 1U;
            return true;
        }
    }
    return false;
}

std::uint64_t
ProjectRuntimeBridge::appliedLiveToolStateRestoreGeneration() const noexcept
{
    return liveToolAppliedRestoreGeneration_.load(
        std::memory_order_acquire);
}

void ProjectRuntimeBridge::publishLiveSetTerminal(
    std::uint64_t requestGeneration,
    RealtimeLiveSetTerminalOutcome outcome) noexcept
{
    if (requestGeneration == 0)
        return;
    RealtimeLiveSetTerminalState current;
    if (!liveSetTerminalState_.read(current)
        || requestGeneration <= current.requestGeneration)
    {
        return;
    }
    liveSetTerminalState_.publish({requestGeneration, outcome});
}

double ProjectRuntimeBridge::transportTimeSeconds() const noexcept
{
    const auto snapshot = graphHost_.transportSnapshot();
    return snapshot.sampleRate > 0.0
        ? static_cast<double>(snapshot.playheadFrame) / snapshot.sampleRate
        : 0.0;
}

RealtimeMusicalTransportSnapshot
ProjectRuntimeBridge::musicalTransportSnapshot() const noexcept
{
    return graphHost_.musicalTransportSnapshot();
}

std::shared_ptr<RealtimeAnalysisCapture> ProjectRuntimeBridge::latestAnalysisCapture() const noexcept
{
    if (latestAnalysisCaptureStageId_ != 0
        && graphHost_.lastRejectedPreparedStageId() == latestAnalysisCaptureStageId_)
        return {};
    return latestAnalysisCapture_;
}

std::shared_ptr<const PreparedScoreSourceAnalysisSet>
ProjectRuntimeBridge::latestScoreSourceAnalysisCaptures() const noexcept
{
    if (latestAnalysisCaptureStageId_ != 0
        && graphHost_.lastRejectedPreparedStageId() == latestAnalysisCaptureStageId_)
        return {};
    return latestScoreSourceAnalysis_;
}

double ProjectRuntimeBridge::latestAnalysisCaptureSampleRate() const noexcept
{
    if (latestAnalysisCaptureStageId_ != 0
        && graphHost_.lastRejectedPreparedStageId() == latestAnalysisCaptureStageId_)
        return 0.0;
    return latestAnalysisCapture_ != nullptr || latestScoreSourceAnalysis_ != nullptr
        ? latestSnapshotSampleRate_
        : 0.0;
}

bool ProjectRuntimeBridge::updateEventValue(std::string_view name, double value, talas::expr::ValueDomain domain)
{
    const auto eventId = realtimeEventValueId(name);
    return eventId.has_value() && enqueueEventValue(*eventId, value, domain, 0);
}

bool ProjectRuntimeBridge::scheduleEventValue(
    std::string_view name,
    double value,
    talas::expr::ValueDomain domain,
    std::size_t frameOffset)
{
    const auto eventId = realtimeEventValueId(name);
    return eventId.has_value() && enqueueEventValue(*eventId, value, domain, frameOffset);
}

bool ProjectRuntimeBridge::scheduleVariableAssignment(
    const talas::project::PreparedProjectVariableIdentity& identity,
    double value,
    talas::expr::ValueDomain domain,
    std::size_t frameOffset,
    std::uint64_t expectedStageGeneration) noexcept
{
    if (identity.isVoiceLocal())
    {
        invalidRealtimeControlCount_.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    const auto stageGeneration = latestStageGeneration_.load(
        std::memory_order_acquire);
    if (stageGeneration == 0
        || latestPreparedVariableStageGeneration_ != stageGeneration
        || graphHost_.sameTopologyTransitionActive()
        || coalescedSnapshotOverlay_ != nullptr
        || (expectedStageGeneration != 0
            && expectedStageGeneration != stageGeneration)
        || !std::isfinite(value))
    {
        staleRealtimeControlCount_.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    const auto cell = talas::project::findPreparedProjectVariableCell(
        latestPreparedVariables_,
        identity);
    if (!cell.has_value()
        || *cell >= latestPreparedVariables_.cells.size()
        || latestPreparedVariables_.cells[*cell].domain != domain
        || *cell >= std::numeric_limits<std::uint32_t>::max())
    {
        invalidRealtimeControlCount_.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    RealtimeControlCommand command;
    command.kind = RealtimeControlCommandKind::VariableAssignment;
    command.eventId = static_cast<std::uint32_t>(*cell + 1U);
    command.value = value;
    command.domain = domain;
    command.frameOffset = frameOffset;
    command.stageGeneration = stageGeneration;
    command.generation = realtimeControlGeneration_.load(
        std::memory_order_acquire);
    if (controlIngress_.tryPush(command))
        return true;
    realtimeControlQueueOverflowCount_.fetch_add(
        1, std::memory_order_relaxed);
    return false;
}

bool ProjectRuntimeBridge::scheduleVoiceVariableAssignment(
    const talas::project::PreparedProjectVariableIdentity& identity,
    std::uint64_t voiceGeneration,
    double value,
    talas::expr::ValueDomain domain,
    std::size_t frameOffset,
    std::uint64_t expectedStageGeneration) noexcept
{
    const auto stageGeneration = latestStageGeneration_.load(
        std::memory_order_acquire);
    if (!identity.isVoiceLocal()
        || voiceGeneration == 0
        || stageGeneration == 0
        || latestPreparedVariableStageGeneration_ != stageGeneration
        || graphHost_.sameTopologyTransitionActive()
        || coalescedSnapshotOverlay_ != nullptr
        || (expectedStageGeneration != 0
            && expectedStageGeneration != stageGeneration)
        || !std::isfinite(value))
    {
        staleRealtimeControlCount_.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    const auto cell = talas::project::findPreparedProjectVariableCell(
        latestPreparedVariables_,
        identity);
    const auto publication = preparedVariableValues();
    if (!cell.has_value()
        || *cell >= latestPreparedVariables_.cells.size()
        || latestPreparedVariables_.cells[*cell].domain != domain
        || *cell >= std::numeric_limits<std::uint32_t>::max()
        || !publication.publicationValid
        || publication.stageId == 0
        || publication.stageId != graphHost_.activeStageId()
        || publication.stageId != latestSnapshotStageId_
        || *cell >= publication.values.size()
        || publication.values[*cell].identity != identity
        || publication.values[*cell].voiceGeneration
            != voiceGeneration)
    {
        invalidRealtimeControlCount_.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    RealtimeControlCommand command;
    command.kind = RealtimeControlCommandKind::VariableAssignment;
    command.eventId = static_cast<std::uint32_t>(*cell + 1U);
    command.value = value;
    command.domain = domain;
    command.frameOffset = frameOffset;
    command.stageGeneration = stageGeneration;
    command.voiceGeneration = voiceGeneration;
    command.generation = realtimeControlGeneration_.load(
        std::memory_order_acquire);
    if (controlIngress_.tryPush(command))
        return true;
    realtimeControlQueueOverflowCount_.fetch_add(
        1, std::memory_order_relaxed);
    return false;
}

SnapshotDirectTransactionResult
ProjectRuntimeBridge::scheduleVariableAssignments(
    std::span<const ProjectVariableAssignment> assignments,
    std::uint64_t expectedStageGeneration)
{
    const auto stageGeneration = latestStageGeneration_.load(
        std::memory_order_acquire);
    if (assignments.empty() || expectedStageGeneration == 0
        || stageGeneration != expectedStageGeneration
        || latestPreparedVariableStageGeneration_ != stageGeneration
        || graphHost_.sameTopologyTransitionActive()
        || coalescedSnapshotOverlay_ != nullptr
        || latestSnapshotStageId_ == 0)
    {
        return SnapshotDirectTransactionResult::StaleStageOrGeneration;
    }
    if (assignments.size()
        > talas::project::maximumPreparedProjectVariableCells)
    {
        return SnapshotDirectTransactionResult::CapacityExceeded;
    }

    std::vector<PreparedTypedEvent> events;
    std::vector<RealtimeTypedEventDispatchResult> results;
    try
    {
        events.reserve(assignments.size());
        for (const auto& assignment : assignments)
        {
            if (assignment.identity.isVoiceLocal()
                || !std::isfinite(assignment.value.value))
            {
                return SnapshotDirectTransactionResult::InvalidTargetOrPayload;
            }
            const auto cell = talas::project::findPreparedProjectVariableCell(
                latestPreparedVariables_,
                assignment.identity);
            if (!cell.has_value()
                || *cell >= latestPreparedVariables_.cells.size()
                || *cell >= std::numeric_limits<std::uint32_t>::max()
                || latestPreparedVariables_.cells[*cell].domain
                    != assignment.value.domain
                || std::ranges::any_of(
                    events,
                    [&](const PreparedTypedEvent& event) {
                        return event.target.index == *cell + 1U;
                    }))
            {
                return SnapshotDirectTransactionResult::InvalidTargetOrPayload;
            }
            PreparedTypedEvent event{};
            event.kind = TypedEventKind::VariableAssignment;
            event.target = {
                TypedEventTargetKind::Variable,
                static_cast<std::uint32_t>(*cell + 1U),
                latestSnapshotStageId_,
                expectedStageGeneration,
            };
            event.frameOffset = 0;
            event.sequence = events.size() + 1U;
            event.payload.variable = {
                assignment.value.value,
                assignment.value.domain,
            };
            events.push_back(event);
        }
        results.resize(events.size());
    }
    catch (const std::bad_alloc&)
    {
        return SnapshotDirectTransactionResult::CapacityExceeded;
    }
    catch (const std::length_error&)
    {
        return SnapshotDirectTransactionResult::CapacityExceeded;
    }

    if (latestStageGeneration_.load(std::memory_order_acquire)
            != expectedStageGeneration
        || latestPreparedVariableStageGeneration_
            != expectedStageGeneration
        || graphHost_.sameTopologyTransitionActive()
        || coalescedSnapshotOverlay_ != nullptr)
    {
        return SnapshotDirectTransactionResult::StaleStageOrGeneration;
    }
    const ProducerGate producerGate(sessionProducerGate_, ProducerGate::Mode::Wait);
    if (!graphHost_.scheduleTypedEvents(events, results))
        return SnapshotDirectTransactionResult::QueueFull;
    return std::ranges::all_of(
               results,
               [](RealtimeTypedEventDispatchResult result) {
                   return result
                       == RealtimeTypedEventDispatchResult::Handled;
               })
        ? SnapshotDirectTransactionResult::Accepted
        : SnapshotDirectTransactionResult::InvalidTargetOrPayload;
}

SnapshotDirectTransactionResult
ProjectRuntimeBridge::resetVariables(
    std::span<const talas::project::PreparedProjectVariableIdentity>
        identities,
    std::uint64_t expectedStageGeneration)
{
    const auto stageGeneration = latestStageGeneration_.load(
        std::memory_order_acquire);
    if (identities.empty() || expectedStageGeneration == 0
        || stageGeneration != expectedStageGeneration
        || latestPreparedVariableStageGeneration_ != stageGeneration
        || graphHost_.sameTopologyTransitionActive()
        || coalescedSnapshotOverlay_ != nullptr
        || latestSnapshotStageId_ == 0)
    {
        return SnapshotDirectTransactionResult::
            StaleStageOrGeneration;
    }
    std::vector<PreparedTypedEvent> events;
    std::vector<RealtimeTypedEventDispatchResult> results;
    try
    {
        events.reserve(identities.size());
        for (const auto& identity : identities)
        {
            if (identity.isVoiceLocal())
            {
                return SnapshotDirectTransactionResult::
                    InvalidTargetOrPayload;
            }
            const auto cell =
                talas::project::findPreparedProjectVariableCell(
                    latestPreparedVariables_,
                    identity);
            if (!cell.has_value()
                || *cell >= latestPreparedVariables_.cells.size()
                || *cell
                    >= std::numeric_limits<std::uint32_t>::max()
                || std::ranges::any_of(
                    events,
                    [&](const PreparedTypedEvent& event) {
                        return event.target.index == *cell + 1U;
                    }))
            {
                return SnapshotDirectTransactionResult::
                    InvalidTargetOrPayload;
            }
            const auto& prepared =
                latestPreparedVariables_.cells[*cell];
            PreparedTypedEvent event{};
            event.kind = TypedEventKind::VariableAssignment;
            event.target = {
                TypedEventTargetKind::Variable,
                static_cast<std::uint32_t>(*cell + 1U),
                latestSnapshotStageId_,
                expectedStageGeneration,
            };
            event.frameOffset = 0;
            event.sequence = events.size() + 1U;
            event.payload.variable = {
                prepared.initialValue,
                prepared.domain,
            };
            events.push_back(event);
        }
        results.resize(events.size());
    }
    catch (const std::exception&)
    {
        return SnapshotDirectTransactionResult::CapacityExceeded;
    }
    if (latestStageGeneration_.load(std::memory_order_acquire)
            != expectedStageGeneration
        || latestPreparedVariableStageGeneration_
            != expectedStageGeneration
        || graphHost_.sameTopologyTransitionActive()
        || coalescedSnapshotOverlay_ != nullptr)
    {
        return SnapshotDirectTransactionResult::
            StaleStageOrGeneration;
    }
    const ProducerGate producerGate(sessionProducerGate_, ProducerGate::Mode::Wait);
    if (!graphHost_.scheduleTypedEvents(events, results))
        return SnapshotDirectTransactionResult::QueueFull;
    return std::ranges::all_of(
               results,
               [](RealtimeTypedEventDispatchResult result) {
                   return result
                       == RealtimeTypedEventDispatchResult::Handled;
               })
        ? SnapshotDirectTransactionResult::Accepted
        : SnapshotDirectTransactionResult::InvalidTargetOrPayload;
}

SnapshotDirectTransactionResult
ProjectRuntimeBridge::resetAllVariables(
    std::uint64_t expectedStageGeneration)
{
    std::vector<talas::project::PreparedProjectVariableIdentity>
        identities;
    try
    {
        identities.reserve(latestPreparedVariables_.cells.size());
        for (const auto& cell : latestPreparedVariables_.cells)
        {
            if (!cell.identity.isVoiceLocal())
            {
                identities.push_back(cell.identity);
            }
        }
    }
    catch (const std::exception&)
    {
        return SnapshotDirectTransactionResult::CapacityExceeded;
    }
    return resetVariables(identities, expectedStageGeneration);
}

bool ProjectRuntimeBridge::scheduleSoundVariableAssignment(
    const talas::project::PreparedProjectSoundVariableIdentity& identity,
    std::string_view selectedDefinitionId,
    std::size_t frameOffset,
    std::uint64_t expectedStageGeneration
) noexcept
{
    const auto stageGeneration = latestStageGeneration_.load(std::memory_order_acquire);
    if (identity.isVoiceLocal() ||
        stageGeneration == 0 || latestPreparedSoundVariableStageGeneration_ != stageGeneration ||
        graphHost_.sameTopologyTransitionActive() || coalescedSnapshotOverlay_ != nullptr ||
        (expectedStageGeneration != 0 && expectedStageGeneration != stageGeneration))
    {
        staleRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto cell =
        talas::project::findPreparedProjectSoundVariableCell(latestPreparedSoundVariables_, identity);
    if (!cell.has_value() || *cell >= latestPreparedSoundVariables_.cells.size() ||
        *cell >= std::numeric_limits<std::uint32_t>::max())
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto candidate = std::ranges::find(
        latestPreparedSoundVariables_.cells[*cell].candidateDefinitionIds, selectedDefinitionId
    );
    if (candidate == latestPreparedSoundVariables_.cells[*cell].candidateDefinitionIds.end())
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    RealtimeControlCommand command;
    command.kind = RealtimeControlCommandKind::SoundVariableAssignment;
    command.eventId = static_cast<std::uint32_t>(*cell + 1U);
    command.selectedOrdinal = static_cast<std::uint16_t>(std::distance(
        latestPreparedSoundVariables_.cells[*cell].candidateDefinitionIds.begin(), candidate
    ));
    command.frameOffset = frameOffset;
    command.stageGeneration = stageGeneration;
    command.generation = realtimeControlGeneration_.load(std::memory_order_acquire);
    if (controlIngress_.tryPush(command))
        return true;
    realtimeControlQueueOverflowCount_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool ProjectRuntimeBridge::scheduleVoiceSoundVariableAssignment(
    const talas::project::PreparedProjectSoundVariableIdentity& identity,
    std::uint64_t voiceGeneration,
    std::string_view selectedDefinitionId,
    std::size_t frameOffset,
    std::uint64_t expectedStageGeneration
) noexcept
{
    const auto stageGeneration = latestStageGeneration_.load(std::memory_order_acquire);
    if (!identity.isVoiceLocal() ||
        voiceGeneration == 0 || stageGeneration == 0 ||
        latestPreparedSoundVariableStageGeneration_ != stageGeneration ||
        graphHost_.sameTopologyTransitionActive() || coalescedSnapshotOverlay_ != nullptr ||
        (expectedStageGeneration != 0 && expectedStageGeneration != stageGeneration))
    {
        staleRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto cell =
        talas::project::findPreparedProjectSoundVariableCell(latestPreparedSoundVariables_, identity);
    const auto publication = preparedSoundVariableValues();
    if (!cell.has_value() || *cell >= latestPreparedSoundVariables_.cells.size() ||
        *cell >= std::numeric_limits<std::uint32_t>::max() || !publication.publicationValid ||
        publication.stageId == 0 || publication.stageId != graphHost_.activeStageId() ||
        publication.stageId != latestSnapshotStageId_ || *cell >= publication.values.size() ||
        publication.values[*cell].identity != identity ||
        publication.values[*cell].voiceGeneration != voiceGeneration)
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto candidate = std::ranges::find(
        latestPreparedSoundVariables_.cells[*cell].candidateDefinitionIds, selectedDefinitionId
    );
    if (candidate == latestPreparedSoundVariables_.cells[*cell].candidateDefinitionIds.end())
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    RealtimeControlCommand command;
    command.kind = RealtimeControlCommandKind::SoundVariableAssignment;
    command.eventId = static_cast<std::uint32_t>(*cell + 1U);
    command.selectedOrdinal = static_cast<std::uint16_t>(std::distance(
        latestPreparedSoundVariables_.cells[*cell].candidateDefinitionIds.begin(), candidate
    ));
    command.frameOffset = frameOffset;
    command.stageGeneration = stageGeneration;
    command.voiceGeneration = voiceGeneration;
    command.generation = realtimeControlGeneration_.load(std::memory_order_acquire);
    if (controlIngress_.tryPush(command))
        return true;
    realtimeControlQueueOverflowCount_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

SnapshotDirectTransactionResult ProjectRuntimeBridge::scheduleSoundVariableAssignments(
    std::span<const ProjectSoundVariableAssignment> assignments,
    std::uint64_t expectedStageGeneration
)
{
    const auto stageGeneration = latestStageGeneration_.load(std::memory_order_acquire);
    if (assignments.empty() || expectedStageGeneration == 0 ||
        stageGeneration != expectedStageGeneration ||
        latestPreparedSoundVariableStageGeneration_ != stageGeneration ||
        graphHost_.sameTopologyTransitionActive() || coalescedSnapshotOverlay_ != nullptr ||
        latestSnapshotStageId_ == 0)
    {
        return SnapshotDirectTransactionResult::StaleStageOrGeneration;
    }
    if (assignments.size() > talas::project::maximumPreparedProjectSoundVariableCells)
    {
        return SnapshotDirectTransactionResult::CapacityExceeded;
    }
    std::vector<PreparedTypedEvent> events;
    std::vector<RealtimeTypedEventDispatchResult> results;
    try
    {
        events.reserve(assignments.size());
        for (const auto& assignment : assignments)
        {
            if (assignment.identity.isVoiceLocal())
            {
                return SnapshotDirectTransactionResult::InvalidTargetOrPayload;
            }
            const auto target = [&]() -> std::optional<SoundVariableTypedEventTarget>
            {
                const auto cell = talas::project::findPreparedProjectSoundVariableCell(
                    latestPreparedSoundVariables_, assignment.identity
                );
                if (!cell.has_value() || std::ranges::any_of(
                                             events,
                                             [&](const PreparedTypedEvent& event)
                                             { return event.target.index == *cell + 1U; }
                                         ))
                {
                    return std::nullopt;
                }
                const auto candidate = std::ranges::find(
                    latestPreparedSoundVariables_.cells[*cell].candidateDefinitionIds,
                    assignment.selectedDefinitionId
                );
                if (candidate ==
                    latestPreparedSoundVariables_.cells[*cell].candidateDefinitionIds.end())
                {
                    return std::nullopt;
                }
                return SoundVariableTypedEventTarget{
                    {
                        TypedEventTargetKind::SoundVariable,
                        static_cast<std::uint32_t>(*cell + 1U),
                        latestSnapshotStageId_,
                        expectedStageGeneration,
                    },
                    static_cast<std::uint16_t>(std::distance(
                        latestPreparedSoundVariables_.cells[*cell].candidateDefinitionIds.begin(),
                        candidate
                    )),
                };
            }();
            if (!target.has_value())
            {
                return SnapshotDirectTransactionResult::InvalidTargetOrPayload;
            }
            PreparedTypedEvent event{};
            event.kind = TypedEventKind::SoundVariableAssignment;
            event.target = target->target;
            event.frameOffset = 0;
            event.sequence = events.size() + 1U;
            event.payload.soundVariable = {target->selectedOrdinal};
            events.push_back(event);
        }
        results.resize(events.size());
    }
    catch (const std::exception&)
    {
        return SnapshotDirectTransactionResult::CapacityExceeded;
    }
    if (latestStageGeneration_.load(std::memory_order_acquire) != expectedStageGeneration ||
        latestPreparedSoundVariableStageGeneration_ != expectedStageGeneration ||
        graphHost_.sameTopologyTransitionActive() || coalescedSnapshotOverlay_ != nullptr)
    {
        return SnapshotDirectTransactionResult::StaleStageOrGeneration;
    }
    const ProducerGate producerGate(sessionProducerGate_, ProducerGate::Mode::Wait);
    if (!graphHost_.scheduleTypedEvents(events, results))
        return SnapshotDirectTransactionResult::QueueFull;
    return std::ranges::all_of(
               results,
               [](RealtimeTypedEventDispatchResult result)
               { return result == RealtimeTypedEventDispatchResult::Handled; }
           )
               ? SnapshotDirectTransactionResult::Accepted
               : SnapshotDirectTransactionResult::InvalidTargetOrPayload;
}

SnapshotDirectTransactionResult ProjectRuntimeBridge::resetSoundVariables(
    std::span<const talas::project::PreparedProjectSoundVariableIdentity> identities,
    std::uint64_t expectedStageGeneration
)
{
    std::vector<ProjectSoundVariableAssignment> assignments;
    try
    {
        assignments.reserve(identities.size());
        for (const auto& identity : identities)
        {
            const auto cell = talas::project::findPreparedProjectSoundVariableCell(
                latestPreparedSoundVariables_, identity
            );
            if (identity.isVoiceLocal() ||
                !cell.has_value() || *cell >= latestPreparedSoundVariables_.cells.size())
            {
                return SnapshotDirectTransactionResult::InvalidTargetOrPayload;
            }
            const auto& prepared = latestPreparedSoundVariables_.cells[*cell];
            if (prepared.candidateDefinitionIds.empty())
                continue;
            if (prepared.initialSelectedOrdinal
                >= prepared.candidateDefinitionIds.size())
            {
                return SnapshotDirectTransactionResult::InvalidTargetOrPayload;
            }
            assignments.push_back({
                identity,
                prepared.candidateDefinitionIds[prepared.initialSelectedOrdinal],
            });
        }
    }
    catch (const std::exception&)
    {
        return SnapshotDirectTransactionResult::CapacityExceeded;
    }
    if (assignments.empty())
    {
        return expectedStageGeneration != 0
                && latestStageGeneration_.load(std::memory_order_acquire)
                    == expectedStageGeneration
                && latestPreparedSoundVariableStageGeneration_
                    == expectedStageGeneration
                && !graphHost_.sameTopologyTransitionActive()
                && coalescedSnapshotOverlay_ == nullptr
            ? SnapshotDirectTransactionResult::Accepted
            : SnapshotDirectTransactionResult::StaleStageOrGeneration;
    }
    return scheduleSoundVariableAssignments(assignments, expectedStageGeneration);
}

SnapshotDirectTransactionResult
ProjectRuntimeBridge::resetAllSoundVariables(std::uint64_t expectedStageGeneration)
{
    std::vector<talas::project::PreparedProjectSoundVariableIdentity> identities;
    try
    {
        identities.reserve(latestPreparedSoundVariables_.cells.size());
        for (const auto& cell : latestPreparedSoundVariables_.cells)
        {
            if (!cell.identity.isVoiceLocal())
            {
                identities.push_back(cell.identity);
            }
        }
    }
    catch (const std::exception&)
    {
        return SnapshotDirectTransactionResult::CapacityExceeded;
    }
    return resetSoundVariables(identities, expectedStageGeneration);
}

talas::project::EditResult
ProjectRuntimeBridge::captureProjectVariableInitialValue(
    const talas::project::PreparedProjectVariableIdentity& identity,
    std::uint64_t expectedStageGeneration,
    std::uint64_t expectedDocumentAuthorityGeneration)
{
    if (identity.ownerKind
            != talas::project::ProjectVariableOwnerKind::Project
        || !identity.definitionId.empty()
        || !identity.instancePath.empty()
        || !identity.voiceAncestors.empty()
        || expectedStageGeneration == 0
        || latestStageGeneration_.load(std::memory_order_acquire)
            != expectedStageGeneration
        || latestPreparedVariableStageGeneration_
            != expectedStageGeneration
        || graphHost_.sameTopologyTransitionActive()
        || coalescedSnapshotOverlay_ != nullptr)
    {
        return {
            talas::project::EditError::StaleProjectVariableCapture,
            "Project variable capture targets a stale or non-project live cell",
        };
    }
    const auto values = preparedVariableValues();
    const auto value = std::ranges::find(
        values.values,
        identity,
        &PreparedVariableValue::identity);
    if (!values.publicationValid
        || values.stageId != activeStageId()
        || value == values.values.end())
    {
        return {
            talas::project::EditError::StaleProjectVariableCapture,
            "Project variable live value is unavailable",
        };
    }
    return editor_.captureProjectVariableInitialValue(
        identity.variableId,
        value->value.value,
        expectedDocumentAuthorityGeneration);
}

talas::project::EditResult
ProjectRuntimeBridge::captureProjectSoundVariableInitialSelection(
    const talas::project::PreparedProjectSoundVariableIdentity& identity,
    std::uint64_t expectedStageGeneration,
    std::uint64_t expectedDocumentAuthorityGeneration)
{
    if (identity.ownerKind
            != talas::project::ProjectVariableOwnerKind::Project
        || !identity.definitionId.empty()
        || !identity.instancePath.empty()
        || !identity.voiceAncestors.empty()
        || !identity.ownerNodeId.empty()
        || identity.voiceOrdinal.has_value()
        || expectedStageGeneration == 0
        || latestStageGeneration_.load(std::memory_order_acquire)
            != expectedStageGeneration
        || latestPreparedSoundVariableStageGeneration_
            != expectedStageGeneration
        || graphHost_.sameTopologyTransitionActive()
        || coalescedSnapshotOverlay_ != nullptr)
    {
        return {
            talas::project::EditError::StaleProjectVariableCapture,
            "Project Sound-variable capture targets a stale or non-project live cell",
        };
    }
    const auto values = preparedSoundVariableValues();
    const auto value = std::ranges::find(
        values.values,
        identity,
        &PreparedSoundVariableValue::identity);
    if (!values.publicationValid
        || values.stageId != activeStageId()
        || value == values.values.end()
        || value->selectedDefinitionId.empty())
    {
        return {
            talas::project::EditError::StaleProjectVariableCapture,
            "Project Sound-variable live selection is unavailable",
        };
    }
    return editor_.captureProjectSoundVariableInitialSelection(
        identity.variableId,
        value->selectedDefinitionId,
        expectedDocumentAuthorityGeneration);
}

SnapshotDirectTransactionResult
ProjectRuntimeBridge::scheduleSnapshotDirectValues(
    std::span<const talas::project::SnapshotValueEntry> values,
    double smoothingSeconds,
    std::uint64_t expectedStageGeneration)
{
    const auto record = [&](SnapshotDirectTransactionResult result) {
        if (result == SnapshotDirectTransactionResult::Accepted)
        {
            preparedSnapshotDirectTransactionCount_.fetch_add(
                1,
                std::memory_order_relaxed);
            return result;
        }
        rejectedRealtimeControls_.fetch_add(
            1,
            std::memory_order_relaxed);
        switch (result)
        {
        case SnapshotDirectTransactionResult::CapacityExceeded:
        case SnapshotDirectTransactionResult::QueueFull:
            realtimeControlQueueOverflowCount_.fetch_add(
                1,
                std::memory_order_relaxed);
            break;
        case SnapshotDirectTransactionResult::StaleStageOrGeneration:
            staleRealtimeControlCount_.fetch_add(
                1,
                std::memory_order_relaxed);
            break;
        case SnapshotDirectTransactionResult::NotReady:
        case SnapshotDirectTransactionResult::InvalidTargetOrPayload:
            invalidRealtimeControlCount_.fetch_add(
                1,
                std::memory_order_relaxed);
            break;
        case SnapshotDirectTransactionResult::Accepted:
            break;
        }
        return result;
    };
    if (latestSnapshotStageId_ == 0
        || latestSnapshotStageGeneration_ == 0
        || (expectedStageGeneration != 0
            && (latestSnapshotStageGeneration_
                    != expectedStageGeneration
                || latestStageGeneration_.load(
                       std::memory_order_acquire)
                    != expectedStageGeneration)))
    {
        return record(SnapshotDirectTransactionResult::
            StaleStageOrGeneration);
    }
    if (values.empty() || !std::isfinite(smoothingSeconds)
        || smoothingSeconds < 0.0
        || smoothingSeconds
            > talas::project::maximumSnapshotSmoothingSeconds)
    {
        return record(SnapshotDirectTransactionResult::
            InvalidTargetOrPayload);
    }
    if (values.size()
        > talas::project::maximumSnapshotValuesPerSnapshot)
    {
        return record(
            SnapshotDirectTransactionResult::CapacityExceeded);
    }

    snapshotDirectUpdateScratch_.clear();
    snapshotDirectVariableUpdateScratch_.clear();
    const auto clearScratch = [&] {
        snapshotDirectUpdateScratch_.clear();
        snapshotDirectVariableUpdateScratch_.clear();
    };
    try
    {
        for (std::size_t valueIndex = 0;
             valueIndex < values.size();
             ++valueIndex)
        {
            const auto& value = values[valueIndex];
            for (std::size_t earlier = 0;
                 earlier < valueIndex;
                 ++earlier)
            {
                if (values[earlier].target == value.target)
                {
                    clearScratch();
                    return record(SnapshotDirectTransactionResult::
                        InvalidTargetOrPayload);
                }
            }
            const auto prepared =
                latestSnapshotPreparedTargets_.find(
                    talas::project::snapshotTargetIdentityString(
                        value.target));
            if (prepared
                    == latestSnapshotPreparedTargets_.end()
                || prepared->second.target != value.target
                || prepared->second.valueKind
                    != talas::project::SnapshotValueKind::Number
                || value.value.kind
                    != talas::project::SnapshotValueKind::Number
                || value.value.domain != prepared->second.domain
                || !std::isfinite(value.value.number))
            {
                clearScratch();
                return record(SnapshotDirectTransactionResult::
                    InvalidTargetOrPayload);
            }
            if (prepared->second.projection
                == SnapshotPreparedProjection::DirectVariable)
            {
                if (!prepared->second.variableTargetIndex.has_value()
                    || snapshotDirectUpdateScratch_.size()
                            + snapshotDirectVariableUpdateScratch_.size()
                        >= maximumSnapshotDirectEventValueUpdates)
                {
                    clearScratch();
                    return record(SnapshotDirectTransactionResult::
                        CapacityExceeded);
                }
                const auto duplicate = std::ranges::find(
                    snapshotDirectVariableUpdateScratch_,
                    *prepared->second.variableTargetIndex,
                    &SnapshotDirectVariableUpdate::targetIndex);
                if (duplicate
                    != snapshotDirectVariableUpdateScratch_.end())
                {
                    clearScratch();
                    return record(SnapshotDirectTransactionResult::
                        InvalidTargetOrPayload);
                }
                snapshotDirectVariableUpdateScratch_.push_back({
                    *prepared->second.variableTargetIndex,
                    value.value.number,
                    prepared->second.domain,
                });
                continue;
            }
            if (prepared->second.projection
                    != SnapshotPreparedProjection::DirectEventValues
                || prepared->second.eventValueTargets.empty())
            {
                clearScratch();
                return record(SnapshotDirectTransactionResult::
                    InvalidTargetOrPayload);
            }
            for (const auto& target :
                 prepared->second.eventValueTargets)
            {
                if (snapshotDirectUpdateScratch_.size()
                        + snapshotDirectVariableUpdateScratch_.size()
                    >= maximumSnapshotDirectEventValueUpdates)
                {
                    clearScratch();
                    return record(SnapshotDirectTransactionResult::
                        CapacityExceeded);
                }
                const auto duplicate = std::ranges::find(
                    snapshotDirectUpdateScratch_,
                    target.targetIndex,
                    &SnapshotDirectEventValueUpdate::targetIndex);
                if (duplicate
                    != snapshotDirectUpdateScratch_.end())
                {
                    clearScratch();
                    return record(SnapshotDirectTransactionResult::
                        InvalidTargetOrPayload);
                }
                snapshotDirectUpdateScratch_.push_back({
                    target.targetIndex,
                    value.value.number,
                    target.domain,
                });
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        clearScratch();
        return record(
            SnapshotDirectTransactionResult::CapacityExceeded);
    }
    catch (const std::length_error&)
    {
        clearScratch();
        return record(
            SnapshotDirectTransactionResult::CapacityExceeded);
    }

    if (expectedStageGeneration != 0
        && (latestSnapshotStageGeneration_ != expectedStageGeneration
            || latestStageGeneration_.load(std::memory_order_acquire)
                != expectedStageGeneration))
    {
        clearScratch();
        return record(SnapshotDirectTransactionResult::
            StaleStageOrGeneration);
    }
    const SnapshotDirectTransaction transaction{
        latestSnapshotStageId_,
        latestSnapshotStageGeneration_,
        0,
        snapshotSmoothingFrames(
            smoothingSeconds,
            latestSnapshotSampleRate_),
        snapshotDirectUpdateScratch_,
        snapshotDirectVariableUpdateScratch_,
    };
    const auto result = [&] {
        const ProducerGate producerGate(sessionProducerGate_, ProducerGate::Mode::Wait);
        return graphHost_.scheduleSnapshotDirectTransaction(transaction);
    }();
    clearScratch();
    return record(result);
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::applyPerformanceDirectValues(
    std::span<const talas::project::SnapshotValueEntry> values,
    double smoothingSeconds,
    std::uint64_t expectedStageGeneration)
{
    return applyPerformanceDirectValuesImpl(
        values, smoothingSeconds, expectedStageGeneration, nullptr,
        false);
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::applyPerformanceDirectValues(
    std::span<const talas::project::SnapshotValueEntry> values,
    double smoothingSeconds,
    std::uint64_t expectedStageGeneration,
    talas::project::PreparedVirtualControlValueEdit authoringEdit)
{
    return applyPerformanceDirectValuesImpl(
        values,
        smoothingSeconds,
        expectedStageGeneration,
        &authoringEdit,
        false);
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::applyGlobalControllerEvent(
    std::string_view generatedEventName,
    double value,
    std::uint64_t expectedStageGeneration)
{
    return applyGlobalControllerEvent(
        generatedEventName, value, true,
        expectedStageGeneration);
}

bool ProjectRuntimeBridge::globalControllerBlocksLiveOverride(
    const talas::project::SnapshotTargetIdentity& target) const noexcept
{
    if (target.kind != talas::project::SnapshotTargetKind::EventValue)
        return false;
    const auto claim = exclusiveGlobalControllerStageByName_.find(
        target.eventName);
    return claim != exclusiveGlobalControllerStageByName_.end()
        && claim->second == latestSnapshotStageGeneration_;
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::applyGlobalControllerEvent(
    std::string_view generatedEventName,
    double value,
    bool allowLiveOverride,
    std::uint64_t expectedStageGeneration)
{
    if (generatedEventName.size() < 2
        || generatedEventName.front() != '!'
        || !std::isfinite(value))
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::StageFailed,
            "Global-controller event requires a !-prefixed name and finite value");
    }
    if (expectedStageGeneration == 0
        || expectedStageGeneration != latestStageGeneration())
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::StalePreparedGeneration,
            "Global-controller event targets a stale prepared stage");
    }
    const auto eventName = generatedEventName.substr(1);
    const auto& document = editor_.document();
    const auto control = std::ranges::find_if(
        document.virtualControls,
        [&](const talas::project::VirtualControl& candidate) {
            return candidate.eventName == eventName;
        });
    if (control == document.virtualControls.end())
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::MissingParameter,
            "Global-controller event has no matching Virtual Control");
    }
    if (value < control->minimum || value > control->maximum)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::StageFailed,
            "Global-controller value is outside the Virtual Control range");
    }
    talas::project::SnapshotValueEntry update;
    update.target.kind = talas::project::SnapshotTargetKind::EventValue;
    update.target.eventName = std::string{eventName};
    update.target.domain = control->domain;
    update.value.kind = talas::project::SnapshotValueKind::Number;
    update.value.domain = control->domain;
    update.value.number = value;
    if (!snapshotDirectTargetPrepared(update.target))
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::PreparedTargetUnavailable,
            "Global-controller Event Value is not direct in the prepared stage",
            &update.target);
    }
    auto claim = exclusiveGlobalControllerStageByName_.end();
    bool insertedClaim = false;
    if (!allowLiveOverride)
    {
        try
        {
            auto [found, inserted] =
                exclusiveGlobalControllerStageByName_.try_emplace(
                    std::string{eventName}, expectedStageGeneration);
            claim = found;
            insertedClaim = inserted;
        }
        catch (const std::exception&)
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::CapacityExceeded,
                "Global-controller source claim exceeded capacity");
        }
    }
    const auto result = applyPerformanceDirectValuesImpl(
        std::span<const talas::project::SnapshotValueEntry>{&update, 1},
        0.0,
        expectedStageGeneration,
        nullptr,
        true);
    if (!result.ok())
    {
        if (insertedClaim)
            exclusiveGlobalControllerStageByName_.erase(claim);
        return result;
    }
    if (allowLiveOverride)
        exclusiveGlobalControllerStageByName_.erase(
            update.target.eventName);
    else
        claim->second = expectedStageGeneration;
    return result;
}

SnapshotPerformanceActionResult
ProjectRuntimeBridge::applyPerformanceDirectValuesImpl(
    std::span<const talas::project::SnapshotValueEntry> values,
    double smoothingSeconds,
    std::uint64_t expectedStageGeneration,
    talas::project::PreparedVirtualControlValueEdit* authoringEdit,
    bool generatedControllerSource)
{
    SnapshotPerformanceActionResult failure;
    if (!snapshotPerformanceDocumentIsCurrent(failure))
        return failure;
    if (expectedStageGeneration == 0
        || latestSnapshotStageGeneration_ != expectedStageGeneration
        || latestStageGeneration_.load(std::memory_order_acquire)
            != expectedStageGeneration)
    {
        return snapshotPerformanceFailure(
            talas::project::SnapshotDiagnosticCode::StalePreparedGeneration,
            "Performance control targets a stale prepared stage");
    }

    if (!generatedControllerSource)
        for (const auto& value : values)
            if (globalControllerBlocksLiveOverride(value.target))
                return snapshotPerformanceFailure(
                    talas::project::SnapshotDiagnosticCode::StageFailed,
                    "Manual control cannot override a generated-only Event Value",
                    &value.target);

    SnapshotPerformanceState candidate;
    if (!prepareSnapshotPerformanceState(values, candidate, failure))
        return failure;
    candidate.activeSnapshotId.clear();
    candidate.morphActive = false;
    candidate.morphLeftSnapshotId.clear();
    candidate.morphRightSnapshotId.clear();
    candidate.morphGestureStartValues.clear();

    std::string authoringBytes;
    if (authoringEdit != nullptr
        && authoringEdit->changesDocument())
    {
        if (nextSnapshotProjectRevision_
            == std::numeric_limits<std::uint64_t>::max())
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::CapacityExceeded,
                "Performance authoring revision identity space is exhausted");
        }
        try
        {
            authoringBytes = talas::project::serializeProjectDocument(
                authoringEdit->document(), registry_);
            if (!latestSnapshotAuthoringTopology_.has_value()
                || snapshotExecutionTopologyFingerprint(
                       authoringEdit->document(), registry_)
                    != *latestSnapshotAuthoringTopology_)
            {
                return snapshotPerformanceFailure(
                    talas::project::SnapshotDiagnosticCode::TopologyMismatch,
                    "Performance authoring edit changes the prepared topology");
            }
            candidate.projectRevision = nextSnapshotProjectRevision_;
        }
        catch (const std::bad_alloc&)
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::CapacityExceeded,
                "Performance authoring preparation allocation failed");
        }
        catch (const std::length_error&)
        {
            return snapshotPerformanceFailure(
                talas::project::SnapshotDiagnosticCode::CapacityExceeded,
                "Performance authoring preparation exceeded supported limits");
        }
    }

    const auto scheduled = scheduleSnapshotDirectValues(
        values,
        smoothingSeconds,
        expectedStageGeneration);
    if (scheduled != SnapshotDirectTransactionResult::Accepted)
    {
        const auto code = scheduled
                == SnapshotDirectTransactionResult::StaleStageOrGeneration
            ? talas::project::SnapshotDiagnosticCode::StalePreparedGeneration
            : scheduled == SnapshotDirectTransactionResult::CapacityExceeded
                    || scheduled == SnapshotDirectTransactionResult::QueueFull
            ? talas::project::SnapshotDiagnosticCode::CapacityExceeded
            : talas::project::SnapshotDiagnosticCode::StageFailed;
        return snapshotPerformanceFailure(
            code,
            "Performance direct transaction was rejected by the prepared stage");
    }
    if (authoringEdit != nullptr
        && authoringEdit->changesDocument())
    {
        editor_.commitVirtualControlValue(std::move(*authoringEdit));
        stagedSnapshotAuthoringBytes_ = std::move(authoringBytes);
        ++nextSnapshotProjectRevision_;
    }
    snapshotPerformanceState_ = std::move(candidate);
    SnapshotPerformanceActionResult result;
    result.outcome = SnapshotPerformanceActionOutcome::AppliedDirect;
    return result;
}

bool ProjectRuntimeBridge::snapshotDirectTargetPrepared(
    const talas::project::SnapshotTargetIdentity& target) const
{
    const auto prepared = latestSnapshotPreparedTargets_.find(
        talas::project::snapshotTargetIdentityString(target));
    return prepared != latestSnapshotPreparedTargets_.end()
        && prepared->second.target == target
        && (prepared->second.projection
                == SnapshotPreparedProjection::DirectEventValues
            || prepared->second.projection
                == SnapshotPreparedProjection::DirectVariable)
        && prepared->second.valueKind
            == talas::project::SnapshotValueKind::Number;
}

std::optional<std::uint32_t> ProjectRuntimeBridge::realtimeEventValueId(std::string_view name) const noexcept
{
    for (std::size_t index = 0; index < registeredEventValueNames_.size(); ++index)
    {
        const auto* candidate = registeredEventValueNames_[index].load(std::memory_order_acquire);
        if (candidate != nullptr && *candidate == name)
            return static_cast<std::uint32_t>(index + 1);
    }
    return std::nullopt;
}

std::optional<std::uint32_t> ProjectRuntimeBridge::realtimeEventValueIdForLatestStage(
    std::string_view name,
    talas::expr::ValueDomain domain) const noexcept
{
    const auto stageGeneration = latestStageGeneration_.load(std::memory_order_acquire);
    const auto eventId = realtimeEventValueId(name);
    if (stageGeneration == 0 || !eventId.has_value()
        || registeredEventValueDomains_[*eventId - 1].load(std::memory_order_relaxed) != domain
        || registeredEventValueStageGenerations_[*eventId - 1].load(std::memory_order_relaxed) != stageGeneration)
    {
        return std::nullopt;
    }
    return eventId;
}

std::uint64_t ProjectRuntimeBridge::latestStageGeneration() const noexcept
{
    return latestStageGeneration_.load(std::memory_order_acquire);
}

std::optional<std::uint8_t>
ProjectRuntimeBridge::defaultMidiChannelForLatestStage() const noexcept
{
    const auto generation = latestStageGeneration_.load(
        std::memory_order_acquire);
    if (generation == 0
        || acceptedMidiControllerStageGeneration_.load(
               std::memory_order_acquire) != generation)
        return std::nullopt;
    return acceptedDefaultMidiChannel_.load(std::memory_order_relaxed);
}

std::uint64_t ProjectRuntimeBridge::latestPreparedStageId() const noexcept
{
    return latestSnapshotStageId_;
}

bool ProjectRuntimeBridge::enqueueEventValue(
    std::uint32_t eventId,
    double value,
    talas::expr::ValueDomain domain,
    std::size_t frameOffset) noexcept
{
    if (eventId == 0 || eventId > registeredEventValueNames_.size() || !std::isfinite(value)
        || registeredEventValueNames_[eventId - 1].load(std::memory_order_acquire) == nullptr
        || registeredEventValueDomains_[eventId - 1].load(std::memory_order_relaxed) != domain)
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    RealtimeControlCommand command;
    command.eventId = eventId;
    command.value = value;
    command.domain = domain;
    command.frameOffset = frameOffset;
    command.generation = realtimeControlGeneration_.load(std::memory_order_acquire);
    if (controlIngress_.tryPush(command))
        return true;
    realtimeControlQueueOverflowCount_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool ProjectRuntimeBridge::enqueueEventValueAtTime(
    std::uint32_t eventId,
    double value,
    talas::expr::ValueDomain domain,
    double arrivalTimeSeconds) noexcept
{
    if (!std::isfinite(arrivalTimeSeconds))
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (eventId == 0 || eventId > registeredEventValueNames_.size() || !std::isfinite(value)
        || registeredEventValueNames_[eventId - 1].load(std::memory_order_acquire) == nullptr
        || registeredEventValueDomains_[eventId - 1].load(std::memory_order_relaxed) != domain)
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    RealtimeControlCommand command;
    command.eventId = eventId;
    command.value = value;
    command.domain = domain;
    command.timing = RealtimeControlTiming::ArrivalTimestamp;
    command.arrivalTimeSeconds = arrivalTimeSeconds;
    command.generation = realtimeControlGeneration_.load(std::memory_order_acquire);
    if (controlIngress_.tryPush(command))
        return true;
    realtimeControlQueueOverflowCount_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool ProjectRuntimeBridge::enqueueEventValueAtTimeForStage(
    std::uint32_t eventId,
    double value,
    talas::expr::ValueDomain domain,
    double arrivalTimeSeconds,
    std::uint64_t expectedStageGeneration,
    ControllerRecordingIngressEvidence recording) noexcept
{
    if (expectedStageGeneration == 0
        || latestStageGeneration_.load(std::memory_order_acquire)
            != expectedStageGeneration
        || eventId == 0
        || eventId > registeredEventValueNames_.size()
        || registeredEventValueStageGenerations_[eventId - 1].load(
               std::memory_order_relaxed) != expectedStageGeneration)
    {
        staleRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!std::isfinite(arrivalTimeSeconds) || !std::isfinite(value)
        || registeredEventValueNames_[eventId - 1].load(
               std::memory_order_acquire) == nullptr
        || registeredEventValueDomains_[eventId - 1].load(
               std::memory_order_relaxed) != domain)
    {
        invalidRealtimeControlCount_.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }
    RealtimeControlCommand command;
    command.eventId = eventId;
    command.value = value;
    command.domain = domain;
    command.timing = RealtimeControlTiming::ArrivalTimestamp;
    command.arrivalTimeSeconds = arrivalTimeSeconds;
    command.stageGeneration = expectedStageGeneration;
    command.generation = realtimeControlGeneration_.load(
        std::memory_order_acquire);
    command.recordingOrigin = recording.origin;
    command.recordingSourceTimestampNanoseconds =
        recording.sourceTimestampNanoseconds;
    command.recordingRouteGeneration = recording.routeGeneration;
    command.recordingSourceGeneration = recording.sourceGeneration;
    command.recordingTakeGeneration =
        requestedControllerRecordingTakeGeneration();
    if (controlIngress_.tryPush(command))
        return true;
    realtimeControlQueueOverflowCount_.fetch_add(
        1, std::memory_order_relaxed);
    return false;
}

bool ProjectRuntimeBridge::enqueueEventValueAtSteadyTimeForStage(
    std::uint32_t eventId,
    double value,
    talas::expr::ValueDomain domain,
    std::int64_t targetTimeNanoseconds,
    std::uint64_t expectedStageGeneration,
    ControllerRecordingIngressEvidence recording) noexcept
{
    if (expectedStageGeneration == 0
        || latestStageGeneration_.load(std::memory_order_acquire)
            != expectedStageGeneration
        || eventId == 0
        || eventId > registeredEventValueNames_.size()
        || registeredEventValueStageGenerations_[eventId - 1].load(
               std::memory_order_relaxed) != expectedStageGeneration)
    {
        staleRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (targetTimeNanoseconds <= 0 || !std::isfinite(value)
        || registeredEventValueNames_[eventId - 1].load(
               std::memory_order_acquire) == nullptr
        || registeredEventValueDomains_[eventId - 1].load(
               std::memory_order_relaxed) != domain)
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    RealtimeControlCommand command;
    command.eventId = eventId;
    command.value = value;
    command.domain = domain;
    command.timing = RealtimeControlTiming::AbsoluteSteadyTimestamp;
    command.targetTimeNanoseconds = targetTimeNanoseconds;
    command.stageGeneration = expectedStageGeneration;
    command.generation = realtimeControlGeneration_.load(
        std::memory_order_acquire);
    command.recordingOrigin = recording.origin;
    command.recordingSourceTimestampNanoseconds =
        recording.sourceTimestampNanoseconds;
    command.recordingRouteGeneration = recording.routeGeneration;
    command.recordingSourceGeneration = recording.sourceGeneration;
    command.recordingTakeGeneration =
        requestedControllerRecordingTakeGeneration();
    if (controlIngress_.tryPush(command))
    {
        addSaturatedBounded(scheduledRealtimeControlAcceptedCount_);
        return true;
    }
    realtimeControlQueueOverflowCount_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool ProjectRuntimeBridge::enqueueControllerRecordingObservationAtTimeForStage(
    std::uint32_t eventId,
    double value,
    talas::expr::ValueDomain domain,
    std::int64_t sourceTimestampNanoseconds,
    std::uint64_t expectedStageGeneration,
    ControllerRecordingIngressEvidence recording) noexcept
{
    const auto sourceAuthorityValid = recording.origin
            == talas::project::ControllerRecordingOrigin::Ui
        ? recording.routeGeneration != 0
        : recording.routeGeneration != 0
            && recording.sourceGeneration != 0;
    const auto takeGeneration =
        requestedControllerRecordingTakeGeneration();
    if (takeGeneration == 0
        || sourceTimestampNanoseconds <= 0
        || !sourceAuthorityValid
        || expectedStageGeneration == 0
        || latestStageGeneration_.load(std::memory_order_acquire)
            != expectedStageGeneration
        || eventId == 0
        || eventId > registeredEventValueNames_.size()
        || !std::isfinite(value)
        || registeredEventValueNames_[eventId - 1].load(
               std::memory_order_acquire) == nullptr
        || registeredEventValueDomains_[eventId - 1].load(
               std::memory_order_relaxed) != domain)
    {
        return false;
    }
    RealtimeControlCommand command;
    command.kind = RealtimeControlCommandKind::ControllerRecordingObservation;
    command.eventId = eventId;
    command.value = value;
    command.domain = domain;
    command.timing = RealtimeControlTiming::ArrivalTimestamp;
    command.arrivalTimeSeconds = static_cast<double>(sourceTimestampNanoseconds)
        / 1'000'000'000.0;
    command.stageGeneration = expectedStageGeneration;
    command.generation = realtimeControlGeneration_.load(
        std::memory_order_acquire);
    command.recordingOrigin = recording.origin;
    command.recordingSourceTimestampNanoseconds = sourceTimestampNanoseconds;
    command.recordingRouteGeneration = recording.routeGeneration;
    command.recordingSourceGeneration = recording.sourceGeneration;
    command.recordingTakeGeneration = takeGeneration;
    return controlIngress_.tryPush(command);
}

ControllerRecordingArmResult ProjectRuntimeBridge::beginControllerRecording(
    std::span<const talas::project::ControllerRecordingTarget> targets,
    std::optional<std::uint64_t> punchStartFrame,
    std::optional<std::uint64_t> punchEndFrame)
{
    ControllerRecordingArmResult result;
    if (controllerRecordingPendingTake_.takeGeneration != 0)
    {
        result.message = "Collect the completed controller recording before arming another take";
        return result;
    }
    if (targets.empty()
        || targets.size() > controllerRecordingTargetEventIds_.size()
        || punchStartFrame.has_value() != punchEndFrame.has_value()
        || (punchStartFrame.has_value()
            && *punchEndFrame <= *punchStartFrame))
    {
        result.message = "Controller recording target or punch range is invalid";
        return result;
    }
    const auto stageGeneration = latestStageGeneration_.load(
        std::memory_order_acquire);
    const auto sampleRateValue = graphHost_.preparedRenderSampleRate();
    if (stageGeneration == 0 || !std::isfinite(sampleRateValue)
        || sampleRateValue < 1.0
        || sampleRateValue
            > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
    {
        result.message = "Controller recording requires one accepted render stage";
        return result;
    }
    try
    {
        const auto currentAuthoringBytes =
            talas::project::serializeProjectDocument(
                editor_.document(), registry_);
        if (stagedSnapshotAuthoringBytes_.empty()
            || latestSnapshotStageGeneration_ != stageGeneration
            || currentAuthoringBytes != stagedSnapshotAuthoringBytes_)
        {
            result.message =
                "Controller recording requires the exact accepted project document";
            return result;
        }
        std::unordered_set<std::uint32_t> eventIds;
        std::unordered_set<std::string_view> laneIds;
        eventIds.reserve(targets.size());
        laneIds.reserve(targets.size());
        std::array<std::uint32_t,
            talas::project::maximumControllerRecordingTargets> targetEventIds{};
        for (std::size_t index = 0; index < targets.size(); ++index)
        {
            const auto& target = targets[index];
            const auto* registeredName = target.eventId == 0
                    || target.eventId > registeredEventValueNames_.size()
                ? nullptr
                : registeredEventValueNames_[target.eventId - 1].load(
                    std::memory_order_acquire);
            if (registeredName == nullptr
                || *registeredName != target.eventName
                || registeredEventValueDomains_[target.eventId - 1].load(
                       std::memory_order_relaxed) != target.domain
                || registeredEventValueStageGenerations_[target.eventId - 1].load(
                       std::memory_order_relaxed) != stageGeneration
                || !talas::project::isValidScoreIdentifier(target.laneId)
                || !talas::project::isValidScoreIdentifier(target.ownerId)
                || !eventIds.insert(target.eventId).second
                || !laneIds.insert(target.laneId).second)
            {
                result.message = "Controller recording target is stale, invalid, or ambiguous";
                return result;
            }
            targetEventIds[index] = target.eventId;
        }
        if (nextControllerRecordingTakeGeneration_ == 0
            || nextControllerRecordingTakeGeneration_
                > maximumControllerRecordingTakeGeneration)
        {
            result.message = "Controller recording generation space is exhausted";
            return result;
        }
        const auto transport = graphHost_.transportSnapshot();
        const auto takeGeneration = nextControllerRecordingTakeGeneration_;
        talas::project::ControllerRecordingTake candidate;
        candidate.takeGeneration = takeGeneration;
        candidate.stageGeneration = stageGeneration;
        candidate.sampleRate =
            static_cast<std::uint32_t>(std::llround(sampleRateValue));
        candidate.musicalTime = editor_.document().timeline.musicalTime;
        candidate.startFrame = punchStartFrame.value_or(transport.absoluteFrame);
        candidate.endFrame = punchEndFrame.value_or(candidate.startFrame);
        candidate.terminal = talas::project::ControllerRecordingTerminal::Cancelled;
        candidate.targets.assign(targets.begin(), targets.end());

        controllerRecordingPendingTake_ = std::move(candidate);
        controllerRecordingTargetEventIds_ = targetEventIds;
        controllerRecordingTargetCount_ = targets.size();
        ++nextControllerRecordingTakeGeneration_;
        controllerRecordingConfiguredStageGeneration_.store(
            stageGeneration, std::memory_order_relaxed);
        controllerRecordingConfiguredPunchStartFrame_.store(
            punchStartFrame.value_or(0), std::memory_order_relaxed);
        controllerRecordingConfiguredPunchEndFrame_.store(
            punchEndFrame.value_or(std::numeric_limits<std::uint64_t>::max()),
            std::memory_order_relaxed);
        controllerRecordingRequestedActive_.store(
            true, std::memory_order_release);
        controllerRecordingTerminalState_.store(
            controllerRecordingTerminalState(
                controllerRecordingPendingTake_.takeGeneration,
                controllerRecordingTerminalBit(
                    talas::project::ControllerRecordingTerminal::Completed)),
            std::memory_order_release);
        // Publish the configured generation only after every field and the
        // active request are visible. Concurrent lifecycle stops therefore
        // either belong to the prior generation or observe a complete arm;
        // they cannot poison an unpublished take.
        controllerRecordingConfiguredTakeGeneration_.store(
            controllerRecordingPendingTake_.takeGeneration,
            std::memory_order_release);
        controllerRecordingRequestGeneration_.fetch_add(
            1, std::memory_order_release);
        result.accepted = true;
        result.takeGeneration = controllerRecordingPendingTake_.takeGeneration;
    }
    catch (const std::bad_alloc&)
    {
        result.message = "Controller recording allocation failed before publication";
    }
    catch (const std::length_error&)
    {
        result.message = "Controller recording target capacity was exceeded";
    }
    return result;
}

void ProjectRuntimeBridge::stopControllerRecording(
    talas::project::ControllerRecordingTerminal terminal) noexcept
{
    const auto configured = controllerRecordingConfiguredTakeGeneration_.load(
        std::memory_order_acquire);
    if (configured == 0
        || controllerRecordingCompletedTakeGeneration_.load(
               std::memory_order_acquire) == configured)
        return;
    auto current = controllerRecordingTerminalState_.load(
        std::memory_order_acquire);
#if defined(TALAS_CONTROLLER_RECORDING_TEST_SUPPORT)
    if (controllerRecordingStopAfterLoadHookForTests_)
        controllerRecordingStopAfterLoadHookForTests_();
#endif
    for (std::size_t attempt = 0;
         attempt < maximumControllerRecordingTerminalCasAttempts;
         ++attempt)
    {
        if (controllerRecordingTerminalGeneration(current) != configured
            || (current & controllerRecordingTerminalCompletedBit) != 0)
        {
            return;
        }
        const auto desired = current | controllerRecordingTerminalBit(terminal);
        if (controllerRecordingTerminalState_.compare_exchange_strong(
                current,
                desired,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            controllerRecordingRequestedActive_.store(
                false, std::memory_order_release);
            controllerRecordingRequestGeneration_.fetch_add(
                1, std::memory_order_release);
            return;
        }
    }
    // Each failed comparison for one generation can only add a previously
    // absent terminal bit. There are eight terminal priorities, so exhausting
    // all attempts means this request is already represented or the generation
    // changed; neither case may mutate a newer take.
    return;
}

#if defined(TALAS_CONTROLLER_RECORDING_TEST_SUPPORT)
void ProjectRuntimeBridge::setControllerRecordingStopAfterLoadHookForTests(
    std::function<void()> hook)
{
    controllerRecordingStopAfterLoadHookForTests_ = std::move(hook);
}
#endif

std::uint64_t
ProjectRuntimeBridge::requestedControllerRecordingTakeGeneration()
    const noexcept
{
    if (!controllerRecordingRequestedActive_.load(std::memory_order_acquire))
        return 0;
    const auto generation = controllerRecordingConfiguredTakeGeneration_.load(
        std::memory_order_relaxed);
    return controllerRecordingRequestedActive_.load(std::memory_order_acquire)
        ? generation
        : 0;
}

bool ProjectRuntimeBridge::controllerRecordingActive() const noexcept
{
    return controllerRecordingPendingTake_.takeGeneration != 0
        && controllerRecordingRequestedActive_.load(
            std::memory_order_acquire)
        && controllerRecordingCompletedTakeGeneration_.load(
               std::memory_order_acquire)
            != controllerRecordingPendingTake_.takeGeneration;
}

std::optional<talas::project::ControllerRecordingTake>
ProjectRuntimeBridge::collectControllerRecordingTake()
{
    const auto completed = controllerRecordingCompletedTakeGeneration_.load(
        std::memory_order_acquire);
    if (completed == 0
        || completed != controllerRecordingPendingTake_.takeGeneration)
    {
        return std::nullopt;
    }
    try
    {
        auto result = controllerRecordingPendingTake_;
        const auto count = controllerRecordingCompletedEventCount_.load(
            std::memory_order_relaxed);
        result.events.assign(
            controllerRecordingEvents_.get(),
            controllerRecordingEvents_.get() + count);
        result.endFrame = controllerRecordingCompletedEndFrame_.load(
            std::memory_order_relaxed);
        result.terminal = controllerRecordingCompletedTerminal_.load(
            std::memory_order_relaxed);
        controllerRecordingPendingTake_ = {};
        controllerRecordingTargetCount_ = 0;
        controllerRecordingConfiguredTakeGeneration_.store(
            0, std::memory_order_release);
        controllerRecordingCompletedTakeGeneration_.store(
            0, std::memory_order_release);
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return std::nullopt;
    }
    catch (const std::length_error&)
    {
        return std::nullopt;
    }
}

bool ProjectRuntimeBridge::enqueueMidiNoteOn(
    int channel,
    int note,
    int velocity,
    double arrivalTimeSeconds) noexcept
{
    if (channel < 1 || channel > 16 || note < 0 || note > 127
        || velocity < 0 || velocity > 127 || !std::isfinite(arrivalTimeSeconds))
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (velocity == 0)
        return enqueueMidiNoteOff(channel, note, arrivalTimeSeconds);

    RealtimeControlCommand command;
    command.kind = RealtimeControlCommandKind::MidiNoteOn;
    command.timing = RealtimeControlTiming::ArrivalTimestamp;
    command.arrivalTimeSeconds = arrivalTimeSeconds;
    command.stageId = acceptedMidiStageId_.load(std::memory_order_acquire);
    command.generation = realtimeControlGeneration_.load(std::memory_order_acquire);
    command.midiChannel = static_cast<std::uint8_t>(channel);
    command.midiNote = static_cast<std::uint8_t>(note);
    command.midiVelocity = static_cast<std::uint8_t>(velocity);
    if (controlIngress_.tryPush(command))
        return true;
    realtimeControlQueueOverflowCount_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool ProjectRuntimeBridge::enqueueMidiNoteOff(
    int channel,
    int note,
    double arrivalTimeSeconds) noexcept
{
    if (channel < 1 || channel > 16 || note < 0 || note > 127 || !std::isfinite(arrivalTimeSeconds))
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    RealtimeControlCommand command;
    command.kind = RealtimeControlCommandKind::MidiNoteOff;
    command.timing = RealtimeControlTiming::ArrivalTimestamp;
    command.arrivalTimeSeconds = arrivalTimeSeconds;
    command.stageId = acceptedMidiStageId_.load(std::memory_order_acquire);
    command.generation = realtimeControlGeneration_.load(std::memory_order_acquire);
    command.midiChannel = static_cast<std::uint8_t>(channel);
    command.midiNote = static_cast<std::uint8_t>(note);
    if (controlIngress_.tryPush(command))
        return true;
    realtimeControlQueueOverflowCount_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool ProjectRuntimeBridge::enqueueMidiControlChange(
    int channel,
    int controller,
    int value,
    double arrivalTimeSeconds,
    std::uint64_t sourceGeneration) noexcept
{
    if (channel < 1 || channel > 16 || controller < 0 || controller > 127
        || value < 0 || value > 127 || !std::isfinite(arrivalTimeSeconds))
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto stageGeneration = acceptedMidiControllerStageGeneration_.load(
        std::memory_order_acquire);
    if (stageGeneration == 0
        || latestStageGeneration_.load(std::memory_order_acquire)
            != stageGeneration) return false;

    char name[6]{'c', 'c', '0', '0', '0', '\0'};
    std::size_t length = 4;
    if (controller < 10)
    {
        name[3] = static_cast<char>('0' + controller);
    }
    else if (controller < 100)
    {
        name[2] = static_cast<char>('0' + controller / 10);
        name[3] = static_cast<char>('0' + controller % 10);
    }
    else
    {
        name[2] = '1';
        name[3] = static_cast<char>('0' + (controller - 100) / 10);
        name[4] = static_cast<char>('0' + controller % 10);
        length = 5;
    }
    const auto defaultChannel = channel == acceptedDefaultMidiChannel_.load(
        std::memory_order_relaxed);
    const auto defaultEventId = defaultChannel
        ? realtimeEventValueIdForLatestStage(
              std::string_view(name, length), talas::expr::ValueDomain::Scalar)
        : std::nullopt;
    name[0] = 'b';
    name[1] = 't';
    const auto buttonEventId = defaultChannel
        ? realtimeEventValueIdForLatestStage(
              std::string_view(name, length), talas::expr::ValueDomain::Scalar)
        : std::nullopt;
    name[0] = 's';
    name[1] = 'w';
    const auto switchEventId = defaultChannel
        ? realtimeEventValueIdForLatestStage(
              std::string_view(name, length), talas::expr::ValueDomain::Scalar)
        : std::nullopt;
    name[0] = 'c';
    name[1] = 'c';
    char taggedName[10]{};
    for (std::size_t index = 0; index < length; ++index)
        taggedName[index] = name[index];
    taggedName[length++] = '_';
    taggedName[length++] = 'c';
    taggedName[length++] = 'h';
    taggedName[length++] = static_cast<char>('0' + channel / 10);
    taggedName[length++] = static_cast<char>('0' + channel % 10);
    const auto taggedEventId = realtimeEventValueIdForLatestStage(
        std::string_view(taggedName, length), talas::expr::ValueDomain::Scalar);
    // ADR-0024: the global map's names for this controller, too.
    bool globalMapMatched = false;
    for (std::uint32_t index = 0,
             count = acceptedGlobalMapCount_.load(std::memory_order_relaxed);
         index < count && !globalMapMatched; ++index)
    {
        const auto& slot = acceptedGlobalMap_[index];
        const auto slotChannel = slot.channel.load(std::memory_order_relaxed);
        globalMapMatched = !slot.pitchBend.load(std::memory_order_relaxed)
            && slot.controller.load(std::memory_order_relaxed) == controller
            && (slotChannel == 0 ? defaultChannel : slotChannel == channel);
    }
    if (!defaultEventId && !buttonEventId
        && !switchEventId && !taggedEventId && !globalMapMatched) return false;

    ControllerRecordingIngressEvidence recording;
    recording.origin = talas::project::ControllerRecordingOrigin::Midi;
    if (arrivalTimeSeconds > 0.0
        && arrivalTimeSeconds
            < static_cast<double>(std::numeric_limits<std::int64_t>::max())
                / 1.0e9)
    {
        recording.sourceTimestampNanoseconds = static_cast<std::int64_t>(
            std::llround(arrivalTimeSeconds * 1.0e9));
    }
    recording.routeGeneration = stageGeneration;
    recording.sourceGeneration = sourceGeneration;
    const auto normalized = static_cast<double>(value) / 127.0;
    bool accepted = true;
    if (defaultEventId)
        accepted = enqueueEventValueAtTimeForStage(
            *defaultEventId, normalized, talas::expr::ValueDomain::Scalar,
            arrivalTimeSeconds, stageGeneration, recording);
    const auto binary = value >= 64 ? 1.0 : 0.0;
    if (buttonEventId)
        accepted = enqueueEventValueAtTimeForStage(
            *buttonEventId, binary, talas::expr::ValueDomain::Scalar,
            arrivalTimeSeconds, stageGeneration, recording) && accepted;
    if (switchEventId)
        accepted = enqueueEventValueAtTimeForStage(
            *switchEventId, binary, talas::expr::ValueDomain::Scalar,
            arrivalTimeSeconds, stageGeneration, recording) && accepted;
    if (taggedEventId)
        accepted = enqueueEventValueAtTimeForStage(
            *taggedEventId, normalized, talas::expr::ValueDomain::Scalar,
            arrivalTimeSeconds, stageGeneration, recording) && accepted;
    if (globalMapMatched)
    {
        bool matched = false;
        accepted = enqueueGlobalMapValues(false, controller, channel, normalized,
                       arrivalTimeSeconds, stageGeneration, recording, matched)
            && accepted;
    }
    return accepted;
}

bool ProjectRuntimeBridge::enqueueGlobalMapValues(
    bool pitchBend,
    int controller,
    int channel,
    double normalized,
    double arrivalTimeSeconds,
    std::uint64_t stageGeneration,
    const ControllerRecordingIngressEvidence& recording,
    bool& anyMatched) noexcept
{
    const auto defaultChannel = acceptedDefaultMidiChannel_.load(std::memory_order_relaxed);
    bool accepted = true;
    for (std::uint32_t index = 0,
             count = acceptedGlobalMapCount_.load(std::memory_order_relaxed);
         index < count; ++index)
    {
        const auto& slot = acceptedGlobalMap_[index];
        const auto slotChannel = slot.channel.load(std::memory_order_relaxed);
        if (slot.pitchBend.load(std::memory_order_relaxed) != pitchBend
            || (!pitchBend && slot.controller.load(std::memory_order_relaxed) != controller)
            || (slotChannel == 0 ? channel != defaultChannel : channel != slotChannel))
            continue;
        const auto eventId = slot.eventId.load(std::memory_order_relaxed);
        if (eventId == 0 || eventId > registeredEventValueDomains_.size()
            || registeredEventValueDomains_[eventId - 1].load(std::memory_order_relaxed)
                != talas::expr::ValueDomain::Scalar
            || registeredEventValueStageGenerations_[eventId - 1].load(std::memory_order_relaxed)
                != stageGeneration)
            continue;
        anyMatched = true;
        talas::project::GlobalMapEntry entry;
        entry.pitchBend = pitchBend;
        entry.minimum = slot.minimum.load(std::memory_order_relaxed);
        entry.maximum = slot.maximum.load(std::memory_order_relaxed);
        entry.grid = slot.grid.load(std::memory_order_relaxed);
        entry.taper = slot.logTaper.load(std::memory_order_relaxed)
            ? talas::project::MidiMapperMapTaper::Log
            : talas::project::MidiMapperMapTaper::Linear;
        const auto value = talas::project::globalMapValue(entry, normalized);
        if (!value.has_value())
            continue;
        accepted = enqueueEventValueAtTimeForStage(
                       eventId, *value, talas::expr::ValueDomain::Scalar,
                       arrivalTimeSeconds, stageGeneration, recording)
            && accepted;
    }
    return accepted;
}

bool ProjectRuntimeBridge::enqueueMidiProgramChange(
    int channel,
    int program,
    double arrivalTimeSeconds) noexcept
{
    if (channel < 1 || channel > 16 || program < 0 || program > 127
        || !std::isfinite(arrivalTimeSeconds))
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto stageGeneration = acceptedMidiControllerStageGeneration_.load(
        std::memory_order_acquire);
    if (stageGeneration == 0
        || latestStageGeneration_.load(std::memory_order_acquire)
            != stageGeneration
        || channel != acceptedDefaultMidiChannel_.load(
            std::memory_order_relaxed)) return false;
    const auto eventId = realtimeEventValueIdForLatestStage(
        "MIDIProgramNumber", talas::expr::ValueDomain::Scalar);
    return eventId && enqueueEventValueAtTimeForStage(
        *eventId, static_cast<double>(program + 1),
        talas::expr::ValueDomain::Scalar, arrivalTimeSeconds, stageGeneration);
}

bool ProjectRuntimeBridge::enqueueMidiChannelPressure(
    int channel,
    int pressure,
    double arrivalTimeSeconds) noexcept
{
    if (channel < 1 || channel > 16 || pressure < 0 || pressure > 127
        || !std::isfinite(arrivalTimeSeconds))
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto stageGeneration = acceptedMidiControllerStageGeneration_.load(
        std::memory_order_acquire);
    if (stageGeneration == 0
        || latestStageGeneration_.load(std::memory_order_acquire)
            != stageGeneration
        || channel != acceptedDefaultMidiChannel_.load(
            std::memory_order_relaxed)) return false;
    const auto eventId = realtimeEventValueIdForLatestStage(
        "MIDIChannelPressure", talas::expr::ValueDomain::Scalar);
    return eventId && enqueueEventValueAtTimeForStage(
        *eventId, static_cast<double>(pressure) / 127.0,
        talas::expr::ValueDomain::Scalar, arrivalTimeSeconds, stageGeneration);
}

bool ProjectRuntimeBridge::enqueueMidiPitchBend(
    int channel,
    int value14,
    double arrivalTimeSeconds) noexcept
{
    if (channel < 1 || channel > 16 || value14 < 0 || value14 > 16383
        || !std::isfinite(arrivalTimeSeconds))
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto stageGeneration = acceptedMidiControllerStageGeneration_.load(
        std::memory_order_acquire);
    if (stageGeneration == 0
        || latestStageGeneration_.load(std::memory_order_acquire)
            != stageGeneration
        || channel != acceptedDefaultMidiChannel_.load(
            std::memory_order_relaxed)) return false;
    const auto eventId = realtimeEventValueIdForLatestStage(
        "MIDIPitchBend", talas::expr::ValueDomain::Scalar);
    const auto value = normalizeMidiPitchBend14(value14);
    bool accepted = true;
    if (eventId)
        accepted = enqueueEventValueAtTimeForStage(
            *eventId, value, talas::expr::ValueDomain::Scalar,
            arrivalTimeSeconds, stageGeneration);
    // ADR-0024: and the global map's pitch wheel names (!PitchBend).
    bool matched = false;
    accepted = enqueueGlobalMapValues(true, 0, channel, value,
                   arrivalTimeSeconds, stageGeneration, {}, matched)
        && accepted;
    if (!eventId && !matched)
        return false;
    return accepted;
}

void ProjectRuntimeBridge::configureMidiClockAuthority(
    std::uint64_t authorityGeneration,
    std::uint8_t sourceSlot,
    std::uint64_t sourceGeneration) noexcept
{
    if (mtcAuthorityGeneration_.load(std::memory_order_acquire) != 0)
        configureMtcAuthority(0);
    const auto currentGeneration =
        midiClockAuthorityGeneration_.load(
            std::memory_order_acquire);
    const auto currentSlot = midiClockExpectedSourceSlot_.load(
        std::memory_order_relaxed);
    const auto currentSourceGeneration =
        midiClockExpectedSourceGeneration_.load(
            std::memory_order_relaxed);
    const auto validInternal = authorityGeneration == 0
        && sourceSlot == 0 && sourceGeneration == 0;
    const auto validExternal = authorityGeneration != 0
        && sourceSlot > 0 && sourceSlot <= 16
        && sourceGeneration != 0;
    if (!validInternal && !validExternal)
    {
        authorityGeneration = 0;
        sourceSlot = 0;
        sourceGeneration = 0;
        addSaturatedBounded(midiClockInvalidRecordCount_);
    }
    else if (validInternal && currentGeneration == 0)
    {
        return;
    }
    else if (validExternal
             && currentGeneration == authorityGeneration)
    {
        if (currentSlot == sourceSlot
            && currentSourceGeneration == sourceGeneration)
        {
            return;
        }
        authorityGeneration = 0;
        sourceSlot = 0;
        sourceGeneration = 0;
        addSaturatedBounded(midiClockInvalidRecordCount_);
    }
    midiClockExpectedSourceSlot_.store(
        sourceSlot,
        std::memory_order_relaxed);
    midiClockExpectedSourceGeneration_.store(
        sourceGeneration,
        std::memory_order_relaxed);
    midiClockAuthorityGeneration_.store(
        authorityGeneration,
        std::memory_order_release);
    publishInternalTransportRequest(false);
    midiClockStopRequestGeneration_.fetch_add(
        1,
        std::memory_order_acq_rel);
}

void ProjectRuntimeBridge::revokeMidiClockAuthorityForSourceLoss() noexcept
{
    if (midiClockAuthorityGeneration_.load(
            std::memory_order_acquire) == 0)
    {
        return;
    }
    addSaturatedBounded(midiClockSourceLossCount_);
    configureMidiClockAuthority(0);
}

bool ProjectRuntimeBridge::enqueueMidiClock(
    RealtimeControlCommandKind kind,
    std::uint8_t sourceSlot,
    std::uint64_t sourceGeneration,
    std::uint64_t authorityGeneration,
    double arrivalTimeSeconds,
    std::uint16_t songPosition) noexcept
{
    const auto validKind =
        kind == RealtimeControlCommandKind::MidiClockTick
        || kind == RealtimeControlCommandKind::MidiClockStart
        || kind == RealtimeControlCommandKind::MidiClockContinue
        || kind == RealtimeControlCommandKind::MidiClockStop
        || kind == RealtimeControlCommandKind::MidiSongPosition;
    if (!validKind || sourceSlot == 0 || sourceSlot > 16
        || sourceGeneration == 0 || authorityGeneration == 0
        || (kind == RealtimeControlCommandKind::MidiSongPosition
            && songPosition > 0x3fffU)
        || (kind != RealtimeControlCommandKind::MidiSongPosition
            && songPosition != 0)
        || !std::isfinite(arrivalTimeSeconds)
        || arrivalTimeSeconds <= 0.0)
    {
        addSaturatedBounded(midiClockInvalidRecordCount_);
        return false;
    }
    if (midiClockAuthorityGeneration_.load(
            std::memory_order_acquire)
            != authorityGeneration
        || midiClockAuthorityGeneration_.load(
               std::memory_order_acquire)
            != authorityGeneration)
    {
        addSaturatedBounded(midiClockStaleAuthorityCount_);
        return false;
    }
    if (midiClockExpectedSourceSlot_.load(
               std::memory_order_relaxed)
            != sourceSlot
        || midiClockExpectedSourceGeneration_.load(
               std::memory_order_relaxed)
            != sourceGeneration)
    {
        addSaturatedBounded(midiClockWrongSourceCount_);
        return false;
    }
    RealtimeControlCommand command;
    command.kind = kind;
    command.timing = RealtimeControlTiming::ArrivalTimestamp;
    command.arrivalTimeSeconds = arrivalTimeSeconds;
    command.generation = authorityGeneration;
    command.stageId = acceptedMidiStageId_.load(std::memory_order_acquire);
    if (command.stageId == 0)
    {
        addSaturatedBounded(
            midiClockSongPositionConversionFailureCount_);
        return false;
    }
    command.midiSourceSlot = sourceSlot;
    command.midiSongPosition = songPosition;
    command.midiSourceGeneration = sourceGeneration;
    if (midiClockIngress_.tryPush(command))
        return true;
    addSaturatedBounded(midiClockQueueOverflowCount_);
    return false;
}

bool ProjectRuntimeBridge::enqueueMidiTimingClock(
    RealtimeControlCommandKind kind,
    double arrivalTimeSeconds) noexcept
{
    if (!std::isfinite(arrivalTimeSeconds) || arrivalTimeSeconds <= 0.0)
    {
        invalidRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // A Stop holds the clock; Start and Continue run it again (the
    // messages that are not ticks count nothing themselves).
    if (kind == RealtimeControlCommandKind::MidiClockStop
        || kind == RealtimeControlCommandKind::MidiClockStart
        || kind == RealtimeControlCommandKind::MidiClockContinue)
    {
        midiTimingClockStopped_.store(
            kind == RealtimeControlCommandKind::MidiClockStop,
            std::memory_order_relaxed);
        return false;
    }
    if (kind != RealtimeControlCommandKind::MidiClockTick
        || midiTimingClockStopped_.load(std::memory_order_relaxed))
        return false;
    const auto stageGeneration = latestStageGeneration_.load(
        std::memory_order_acquire);
    const auto eventId = realtimeEventValueIdForLatestStage(
        talas::expr::midiTimingClockCountEventName,
        talas::expr::ValueDomain::Scalar);
    if (stageGeneration == 0 || !eventId)
        return false;
    // Each message counts one more, which
    // !TimingClock and `MIDITimingClock take as one trigger; a double
    // counts exactly to 2^53, after which the count starts again at 1.
    auto count = midiTimingClockMessageCount_.load(std::memory_order_relaxed) + 1;
    if (count > (std::uint64_t{1} << 53))
        count = 1;
    midiTimingClockMessageCount_.store(count, std::memory_order_relaxed);
    return enqueueEventValueAtTimeForStage(
        *eventId, static_cast<double>(count),
        talas::expr::ValueDomain::Scalar, arrivalTimeSeconds, stageGeneration);
}

MidiClockTelemetrySnapshot
ProjectRuntimeBridge::midiClockTelemetry() const noexcept
{
    MidiClockTelemetrySnapshot result;
    result.sourceLosses = midiClockSourceLossCount_.load(
        std::memory_order_relaxed);
    result.acceptedTicks = midiClockAcceptedTickCount_.load(
        std::memory_order_relaxed);
    result.acceptedStarts = midiClockAcceptedStartCount_.load(
        std::memory_order_relaxed);
    result.acceptedContinues =
        midiClockAcceptedContinueCount_.load(
            std::memory_order_relaxed);
    result.acceptedStops = midiClockAcceptedStopCount_.load(
        std::memory_order_relaxed);
    const auto relocation = graphHost_.stageBoundRelocationSnapshot();
    result.acceptedSongPositions = relocation.acceptedCount;
    result.songPositionConversionFailures = saturatedSum(
        midiClockSongPositionConversionFailureCount_.load(
            std::memory_order_relaxed),
        graphHost_.rejectedStageBoundRelocationCount());
    result.lastSongPosition = relocation.lastSongPosition;
    result.lastRelocationFrame = relocation.lastFrame;
    result.queueOverflow = midiClockQueueOverflowCount_.load(
        std::memory_order_relaxed);
    result.staleAuthority = midiClockStaleAuthorityCount_.load(
        std::memory_order_relaxed);
    result.wrongSource = midiClockWrongSourceCount_.load(
        std::memory_order_relaxed);
    result.invalidRecords = midiClockInvalidRecordCount_.load(
        std::memory_order_relaxed);
    result.unsupportedSongPosition =
        midiClockUnsupportedSongPositionCount_.load(
            std::memory_order_relaxed);
    result.signalTimeouts = midiClockSignalTimeoutCount_.load(
        std::memory_order_relaxed);
    result.tempoAcquisitions = midiClockTempoAcquisitionCount_.load(
        std::memory_order_relaxed);
    result.tempoReacquisitions = midiClockTempoReacquisitionCount_.load(
        std::memory_order_relaxed);
    result.tempoRangeRejections = midiClockTempoRangeRejectionCount_.load(
        std::memory_order_relaxed);
    result.cadenceDiscontinuities =
        midiClockCadenceDiscontinuityCount_.load(
            std::memory_order_relaxed);
    result.timestampRejections = midiClockTimestampRejectionCount_.load(
        std::memory_order_relaxed);
    result.estimatorOverflows = midiClockEstimatorOverflowCount_.load(
        std::memory_order_relaxed);
    result.holdoverEntries = midiClockHoldoverEntryCount_.load(
        std::memory_order_relaxed);
    result.publicationValidationFailures =
        midiClockPublicationValidationFailureCount_.load(
            std::memory_order_relaxed);
    RealtimeMusicalTransportSnapshot musicalSnapshot;
    auto authorityStable = false;
    for (std::uint32_t attempt = 0; attempt < 3; ++attempt)
    {
        const auto before = midiClockAuthorityGeneration_.load(
            std::memory_order_acquire);
        musicalSnapshot = graphHost_.musicalTransportSnapshot();
        const auto after = midiClockAuthorityGeneration_.load(
            std::memory_order_acquire);
        if (before == after)
        {
            result.authorityGeneration = after;
            authorityStable = true;
            break;
        }
    }
    if (!authorityStable)
    {
        addSaturatedBounded(
            midiClockPublicationValidationFailureCount_);
        result.authorityGeneration = 0;
    }
    if (!musicalSnapshot.publicationValid
        || musicalSnapshot.usedStableFallback)
    {
        addSaturatedBounded(
            midiClockPublicationValidationFailureCount_);
    }
    result.externalAuthority = result.authorityGeneration != 0;
    const auto coherentExternal = result.externalAuthority
        && musicalSnapshot.publicationValid
        && musicalSnapshot.externalAuthority
        && musicalSnapshot.externalAuthorityGeneration
            == result.authorityGeneration;
    if (coherentExternal)
    {
        result.pulseSequence = musicalSnapshot.externalPulseSequence;
        result.intervalNanoseconds =
            musicalSnapshot.externalIntervalNanoseconds;
        result.estimatedMicrosecondsPerQuarter =
            musicalSnapshot.externalMicrosecondsPerQuarter;
        result.phaseErrorNanoseconds =
            musicalSnapshot.externalPhaseErrorNanoseconds;
        result.jitterNanoseconds =
            musicalSnapshot.externalJitterNanoseconds;
        result.acquisitionIntervals =
            musicalSnapshot.externalAcquisitionIntervals;
        result.lockState = musicalSnapshot.externalLockState;
        result.lastAnchor = musicalSnapshot.externalLastAnchor;
        result.lastTickTimeSeconds =
            musicalSnapshot.externalLastPulseTimeSeconds;
        result.running = musicalSnapshot.transport.playing;
        result.signalStale =
            musicalSnapshot.externalLockState == MidiClockLockState::Stale;
    }
    return result;
}

void ProjectRuntimeBridge::configureMtcAuthority(
    std::uint64_t authorityGeneration,
    std::uint8_t sourceSlot,
    std::uint64_t sourceGeneration) noexcept
{
    if (authorityGeneration != 0)
        configureMidiClockAuthority(0);
    const auto currentGeneration = mtcAuthorityGeneration_.load(
        std::memory_order_acquire);
    const auto currentSlot = mtcExpectedSourceSlot_.load(
        std::memory_order_relaxed);
    const auto currentSourceGeneration =
        mtcExpectedSourceGeneration_.load(std::memory_order_relaxed);
    const auto validInternal = authorityGeneration == 0
        && sourceSlot == 0 && sourceGeneration == 0;
    const auto validExternal = authorityGeneration != 0
        && sourceSlot > 0 && sourceSlot <= 16
        && sourceGeneration != 0;
    if (!validInternal && !validExternal)
    {
        authorityGeneration = 0;
        sourceSlot = 0;
        sourceGeneration = 0;
        addSaturatedBounded(mtcInvalidRecordCount_);
    }
    else if (validInternal && currentGeneration == 0)
    {
        return;
    }
    else if (validExternal && currentGeneration == authorityGeneration)
    {
        if (currentSlot == sourceSlot
            && currentSourceGeneration == sourceGeneration)
        {
            return;
        }
        authorityGeneration = 0;
        sourceSlot = 0;
        sourceGeneration = 0;
        addSaturatedBounded(mtcInvalidRecordCount_);
    }
    mtcExpectedSourceSlot_.store(sourceSlot, std::memory_order_relaxed);
    mtcExpectedSourceGeneration_.store(
        sourceGeneration, std::memory_order_relaxed);
    mtcAuthorityGeneration_.store(
        authorityGeneration, std::memory_order_release);
    publishInternalTransportRequest(false);
    mtcStopRequestGeneration_.fetch_add(1, std::memory_order_acq_rel);
}

void ProjectRuntimeBridge::revokeMtcAuthorityForSourceLoss() noexcept
{
    if (mtcAuthorityGeneration_.load(std::memory_order_acquire) == 0)
        return;
    addSaturatedBounded(mtcSourceLossCount_);
    configureMtcAuthority(0);
}

bool ProjectRuntimeBridge::enqueueMtc(
    std::span<const std::uint8_t> bytes,
    std::uint8_t sourceSlot,
    std::uint64_t sourceGeneration,
    std::uint64_t authorityGeneration,
    double arrivalTimeSeconds) noexcept
{
    const auto rejectLiveEvidence = [this] {
        if (mtcAuthorityGeneration_.load(std::memory_order_acquire) != 0)
        {
            mtcEvidenceRejectionGeneration_.fetch_add(
                1, std::memory_order_acq_rel);
        }
    };
    if ((bytes.size() != 2 && bytes.size() != 10)
        || sourceSlot == 0 || sourceSlot > 16
        || sourceGeneration == 0 || authorityGeneration == 0
        || !std::isfinite(arrivalTimeSeconds)
        || arrivalTimeSeconds <= 0.0)
    {
        addSaturatedBounded(mtcInvalidRecordCount_);
        rejectLiveEvidence();
        return false;
    }
    if (mtcAuthorityGeneration_.load(std::memory_order_acquire)
            != authorityGeneration
        || mtcAuthorityGeneration_.load(std::memory_order_acquire)
            != authorityGeneration)
    {
        addSaturatedBounded(mtcStaleAuthorityCount_);
        rejectLiveEvidence();
        return false;
    }
    if (mtcExpectedSourceSlot_.load(std::memory_order_relaxed)
            != sourceSlot
        || mtcExpectedSourceGeneration_.load(std::memory_order_relaxed)
            != sourceGeneration)
    {
        addSaturatedBounded(mtcWrongSourceCount_);
        rejectLiveEvidence();
        return false;
    }
    const auto stageId = acceptedMidiStageId_.load(std::memory_order_acquire);
    if (stageId == 0)
    {
        addSaturatedBounded(mtcMappingFailureCount_);
        rejectLiveEvidence();
        return false;
    }
    MtcTransportRecord record;
    std::copy_n(bytes.begin(), bytes.size(), record.bytes.begin());
    record.byteCount = static_cast<std::uint8_t>(bytes.size());
    record.arrivalTimeSeconds = arrivalTimeSeconds;
    record.evidenceGeneration = mtcEvidenceRejectionGeneration_.load(
        std::memory_order_acquire);
    record.identity = {
        .authorityGeneration = authorityGeneration,
        .sourceGeneration = sourceGeneration,
        .stageId = stageId,
        .sourceSlot = sourceSlot,
    };
    if (mtcIngress_.tryPush(record))
        return true;
    addSaturatedBounded(mtcQueueOverflowCount_);
    rejectLiveEvidence();
    return false;
}

MtcTelemetrySnapshot ProjectRuntimeBridge::mtcTelemetry() const noexcept
{
    MtcTelemetrySnapshot result;
    result.sourceLosses = mtcSourceLossCount_.load(std::memory_order_relaxed);
    result.queueOverflow = mtcQueueOverflowCount_.load(std::memory_order_relaxed);
    result.staleAuthority = mtcStaleAuthorityCount_.load(std::memory_order_relaxed);
    result.wrongSource = mtcWrongSourceCount_.load(std::memory_order_relaxed);
    result.invalidRecords = mtcInvalidRecordCount_.load(std::memory_order_relaxed);
    result.mappingFailures = mtcMappingFailureCount_.load(std::memory_order_relaxed);
    result.publicationValidationFailures =
        mtcPublicationValidationFailureCount_.load(std::memory_order_relaxed);
    auto authorityStable = false;
    RealtimeMusicalTransportSnapshot musical;
    for (std::uint32_t attempt = 0; attempt < 3; ++attempt)
    {
        const auto before = mtcAuthorityGeneration_.load(std::memory_order_acquire);
        musical = graphHost_.musicalTransportSnapshot();
        const auto after = mtcAuthorityGeneration_.load(std::memory_order_acquire);
        if (before == after)
        {
            result.authorityGeneration = after;
            authorityStable = true;
            break;
        }
    }
    if (!authorityStable || !musical.publicationValid
        || musical.usedStableFallback)
    {
        addSaturatedBounded(mtcPublicationValidationFailureCount_);
    }
    result.externalAuthority = authorityStable
        && result.authorityGeneration != 0;
    const auto coherent = result.externalAuthority
        && musical.publicationValid && musical.mtcAuthority
        && musical.mtcAuthorityGeneration == result.authorityGeneration;
    if (coherent)
    {
        result.follower = musical.mtc;
        result.mappedProjectFrame = musical.mtcMappedProjectFrame;
        result.running = musical.transport.playing
            && musical.mtc.runningEvidence;
    }
    result.follower.acceptedQuarterFrames =
        mtcAcceptedQuarterFrameCount_.load(std::memory_order_relaxed);
    result.follower.acceptedCompleteSets =
        mtcAcceptedCompleteSetCount_.load(std::memory_order_relaxed);
    result.follower.acceptedFullFrameLocates =
        mtcAcceptedFullFrameLocateCount_.load(std::memory_order_relaxed);
    result.follower.acquisitions = mtcAcquisitionCount_.load(std::memory_order_relaxed);
    result.follower.reacquisitions = mtcReacquisitionCount_.load(std::memory_order_relaxed);
    result.follower.staleTimeouts = mtcStaleTimeoutCount_.load(std::memory_order_relaxed);
    result.follower.reorderedOrSkippedPieces = mtcReorderedPieceCount_.load(std::memory_order_relaxed);
    result.follower.identityRejections = mtcFollowerIdentityRejectionCount_.load(std::memory_order_relaxed);
    result.follower.timestampRejections = mtcTimestampRejectionCount_.load(std::memory_order_relaxed);
    result.follower.invalidMessages = mtcFollowerInvalidMessageCount_.load(std::memory_order_relaxed);
    result.follower.invalidLabelsOrRates = mtcInvalidLabelOrRateCount_.load(std::memory_order_relaxed);
    result.follower.unsupportedDirections = mtcUnsupportedDirectionCount_.load(std::memory_order_relaxed);
    result.follower.arithmeticOverflows = mtcArithmeticOverflowCount_.load(std::memory_order_relaxed);
    result.follower.discontinuities = mtcDiscontinuityCount_.load(std::memory_order_relaxed);
    return result;
}

void ProjectRuntimeBridge::resetRealtimeControlTiming() noexcept
{
    stopControllerRecording(
        talas::project::ControllerRecordingTerminal::SourceLost);
    realtimeControlGeneration_.fetch_add(1, std::memory_order_acq_rel);
}

std::uint64_t ProjectRuntimeBridge::rejectedRealtimeControlCount() const noexcept
{
    return rejectedRealtimeControls_.load(std::memory_order_acquire);
}

RealtimeControlTelemetrySnapshot
ProjectRuntimeBridge::realtimeControlTelemetry() const noexcept
{
    return {
        preparedTypedEventCount_.load(std::memory_order_acquire),
        realtimeControlQueueOverflowCount_.load(std::memory_order_acquire),
        staleRealtimeControlCount_.load(std::memory_order_acquire),
        clampedRealtimeControlTimingCount_.load(std::memory_order_acquire),
        invalidRealtimeControlCount_.load(std::memory_order_acquire),
        typedEventBatchOverflowCount_.load(std::memory_order_acquire),
        rejectedRealtimeControls_.load(std::memory_order_acquire),
        dspRealtimeControlRejectionCount_.load(std::memory_order_acquire),
        scheduledRealtimeControlAcceptedCount_.load(std::memory_order_acquire),
        scheduledRealtimeControlAppliedCount_.load(std::memory_order_acquire),
        scheduledRealtimeControlLateCount_.load(std::memory_order_acquire),
        scheduledRealtimeControlPendingFullCount_.load(std::memory_order_acquire),
        scheduledRealtimeControlStaleCount_.load(std::memory_order_acquire),
        scheduledRealtimeControlPendingCount_.load(std::memory_order_acquire),
        scheduledRealtimeControlPendingHighWatermark_.load(
            std::memory_order_acquire),
        preparedSnapshotDirectTransactionCount_.load(
            std::memory_order_acquire),
        graphHost_.discardedOutputChannelCount(),
    };
}

RealtimeMetricsSnapshotV1
ProjectRuntimeBridge::rawRealtimeMetrics() const noexcept
{
    RealtimeMetricsSnapshotV1 result;
    const auto graph = graphHost_.realtimeMetricsRaw();
    result.activeStageId = graphHost_.activeStageId();
    result.renderCallbacks = graph.renderCallbacks;
    result.renderFailures = graph.renderFailures;
    result.stageRejections = graph.stageRejections;
    result.discardedOutputChannels =
        graphHost_.discardedOutputChannelCount();

    const auto control = realtimeControlTelemetry();
    result.preparedTypedEvents = control.preparedTypedEvents;
    result.controlQueueOverflow = control.queueOverflow;
    result.staleGenerationOrStage = control.staleGenerationOrStage;
    result.lateOrClampedTiming = control.lateOrClampedTiming;
    result.invalidTargetOrPayload = control.invalidTargetOrPayload;
    result.typedEventBatchOverflow = control.batchOverflow;
    result.aggregateControlRejected = control.aggregateRejected;
    result.dspRejected = control.dspRejection;

    if (latestAnalysisCapture_ != nullptr
        && latestAnalysisCaptureStageId_ == result.activeStageId)
    {
        result.analysisProbeCount = latestAnalysisCapture_->probeCount();
        for (std::size_t probe = 0;
             probe < latestAnalysisCapture_->probeCount();
             ++probe)
        {
            result.analysisDroppedBlocks = realtimeMetricAddSaturated(
                result.analysisDroppedBlocks,
                latestAnalysisCapture_->droppedBlockCount(probe));
        }
    }

    try
    {
        const auto health = graphHost_.streamHealth();
        if (health.stageId == result.activeStageId)
        {
            result.streamHealthAvailable = 1;
            result.streamConsumerCount = health.consumerCount;
            result.streamDecodedBlocks = health.totals.decodedBlocks;
            result.streamDecodeFailures = health.totals.decodeFailures;
            result.streamUnderrunEvents = health.totals.underrunEvents;
            result.streamMissingFrames = health.totals.missingFrames;
            result.streamRequestRejections =
                health.totals.requestRejections;
            result.streamResponseQueueFull =
                health.totals.responseQueueFull;
            result.streamStaleResponseBlocks =
                health.totals.staleResponseBlocks;
        }
    }
    catch (...)
    {
        // Metrics are observational. Allocation failure while copying the
        // control-thread health projection must not affect the active graph.
    }
    const auto confirmedStageId = graphHost_.activeStageId();
    if (confirmedStageId != result.activeStageId)
    {
        result.activeStageId = confirmedStageId;
        result.analysisProbeCount = 0;
        result.analysisDroppedBlocks = 0;
        result.streamConsumerCount = 0;
        result.streamDecodedBlocks = 0;
        result.streamDecodeFailures = 0;
        result.streamUnderrunEvents = 0;
        result.streamMissingFrames = 0;
        result.streamRequestRejections = 0;
        result.streamResponseQueueFull = 0;
        result.streamStaleResponseBlocks = 0;
        result.streamHealthAvailable = 0;
    }
    return result;
}

RealtimeMetricsSnapshotV1
ProjectRuntimeBridge::realtimeMetrics() noexcept
{
    const auto observed = rawRealtimeMetrics();
    const auto stageChanged = observed.activeStageId
        != realtimeMetricsBaseline_.activeStageId;
    auto current = observed;
    if (!stageChanged)
    {
        carryForwardLastAvailableStreamMetrics(
            current, realtimeMetricsLastRaw_);
    }
    if (stageChanged)
    {
        realtimeMetricsBaseline_ = current;
        if (realtimeMetricsEpoch_
            != std::numeric_limits<std::uint64_t>::max())
        {
            ++realtimeMetricsEpoch_;
        }
    }
    else if (realtimeMetricsCountersRegressed(
                 current, realtimeMetricsLastRaw_))
    {
        realtimeMetricsBaseline_ = realtimeMetricsRegressionBaseline(
            current, realtimeMetricsLastRaw_);
        if (realtimeMetricsEpoch_
            != std::numeric_limits<std::uint64_t>::max())
        {
            ++realtimeMetricsEpoch_;
        }
    }
    realtimeMetricsLastRaw_ = current;
    current.streamHealthAvailable = observed.streamHealthAvailable;
    return realtimeMetricsSince(
        current,
        realtimeMetricsBaseline_,
        realtimeMetricsEpoch_);
}

void ProjectRuntimeBridge::resetRealtimeMetrics() noexcept
{
    auto current = rawRealtimeMetrics();
    if (current.activeStageId == realtimeMetricsLastRaw_.activeStageId)
    {
        carryForwardLastAvailableStreamMetrics(
            current, realtimeMetricsLastRaw_);
    }
    realtimeMetricsBaseline_ = current;
    realtimeMetricsLastRaw_ = realtimeMetricsBaseline_;
    if (realtimeMetricsEpoch_
        != std::numeric_limits<std::uint64_t>::max())
    {
        ++realtimeMetricsEpoch_;
    }
}

std::size_t ProjectRuntimeBridge::activeOutputChannelCount() const noexcept
{
    return graphHost_.activeOutputChannelCount();
}

std::uint64_t ProjectRuntimeBridge::activeStageId() const noexcept
{
    return graphHost_.activeStageId();
}

RealtimeStreamHealthSnapshot
ProjectRuntimeBridge::streamHealth() const
{
    return graphHost_.streamHealth();
}

PreparedNodeTimingSnapshot
ProjectRuntimeBridge::preparedNodeTiming() const
{
    return graphHost_.preparedNodeTiming();
}

PreparedVariableValueSnapshot
ProjectRuntimeBridge::preparedVariableValues() const
{
    return graphHost_.preparedVariableValues();
}

PreparedSoundVariableValueSnapshot ProjectRuntimeBridge::preparedSoundVariableValues() const
{
    return graphHost_.preparedSoundVariableValues();
}

PreparedGraphStatusSnapshot
ProjectRuntimeBridge::preparedGraphStatus(
    std::span<
        const PreparedGraphStatusSnapshot::OutputDeviceRoute>
        outputDeviceRoutes) const
{
    PreparedGraphStatusSnapshot result;
    result.prepared = latestPreparedGraphSummary_;
    result.deviceMappingAttached =
        !outputDeviceRoutes.empty();
    if (result.prepared != nullptr
        && result.deviceMappingAttached)
    {
        result.outputDeviceRoutes.assign(
            result.prepared->outputs.size(),
            PreparedGraphStatusSnapshot::OutputDeviceRoute{});
        std::copy_n(
            outputDeviceRoutes.begin(),
            std::min(
                outputDeviceRoutes.size(),
                result.outputDeviceRoutes.size()),
            result.outputDeviceRoutes.begin());
    }
    result.activeStageId = graphHost_.activeStageId();
    result.activationRejected = result.prepared != nullptr
        && result.prepared->stageId != 0
        && result.prepared->stageId == graphHost_.lastRejectedPreparedStageId();
    result.awaitingActivation = result.prepared != nullptr
        && !result.activationRejected
        && result.prepared->stageId != result.activeStageId;
    if (result.prepared != nullptr && !result.awaitingActivation && !result.activationRejected)
    {
        auto health = graphHost_.streamHealth();
        if (health.stageId == result.prepared->stageId)
            result.streamHealth = std::move(health);
    }
    return result;
}

bool ProjectRuntimeBridge::collectRetired() noexcept
{
    const auto sourcesCollected = collectRetiredTransportRelocations();
    const auto collected = graphHost_.collectRetired() != nullptr;
    const auto staged = serviceCoalescedSnapshotOverlay();
    return sourcesCollected || collected || staged;
}

bool ProjectRuntimeBridge::render(float* const* outputChannels,
                                  std::size_t channelCount,
                                  std::size_t frameCount) noexcept
{
    return render({}, outputChannels, channelCount, frameCount);
}

bool ProjectRuntimeBridge::render(RealtimeAudioInputBlock input,
                                  float* const* outputChannels,
                                  std::size_t channelCount,
                                  std::size_t frameCount) noexcept
{
    return render(input, 0.0, outputChannels, channelCount, frameCount);
}

bool ProjectRuntimeBridge::render(RealtimeAudioInputBlock input,
                                  double callbackTimeSeconds,
                                  float* const* outputChannels,
                                  std::size_t channelCount,
                                  std::size_t frameCount) noexcept
{
    return renderWithOutputMapping(
        input,
        callbackTimeSeconds,
        outputChannels,
        channelCount,
        frameCount,
        false);
}

bool ProjectRuntimeBridge::renderOrdered(
    RealtimeAudioInputBlock input,
    double callbackTimeSeconds,
    float* const* outputChannels,
    std::size_t channelCount,
    std::size_t frameCount) noexcept
{
    return renderWithOutputMapping(
        input,
        callbackTimeSeconds,
        outputChannels,
        channelCount,
        frameCount,
        true);
}

RealtimeTerminalRenderResult ProjectRuntimeBridge::renderTerminalEvents(
    std::size_t frameCount,
    RealtimeTerminalOutputBlock& output) noexcept
{
    return renderTerminalEvents(0.0, frameCount, output);
}

RealtimeTerminalRenderResult ProjectRuntimeBridge::renderTerminalEvents(
    double callbackTimeSeconds,
    std::size_t frameCount,
    RealtimeTerminalOutputBlock& output) noexcept
{
    graphHost_.prepareNextRenderBoundary();
    adoptInternalTransportRelocation();
    drainMidiClockTransport(callbackTimeSeconds, frameCount);
    drainMtcTransport(callbackTimeSeconds, frameCount);
    struct PreparationContext
    {
        ProjectRuntimeBridge* bridge = nullptr;
        std::size_t frameCount = 0;
        double callbackTimeSeconds = 0.0;
    } context{this, frameCount, callbackTimeSeconds};
    const auto prepare = [](
        void* opaque,
        RealtimeAudioEngine& renderTarget) noexcept {
        const auto& preparation =
            *static_cast<const PreparationContext*>(opaque);
        if (preparation.frameCount == 0)
            return;
        preparation.bridge->drainRealtimeControls(
            renderTarget,
            preparation.frameCount,
            preparation.callbackTimeSeconds);
        const auto* liveSet = renderTarget.preparedLiveSet();
        const auto* liveTools = renderTarget.preparedLiveTools();
        if (liveSet != nullptr && liveTools != nullptr)
        {
            preparation.bridge->drainRealtimeLiveToolTriggers(
                renderTarget, *liveSet, *liveTools);
        }
    };
    const auto clearAdoption = [&output]() noexcept {
        output.previousStageId = 0;
        output.adoptionFrameOffset = 0;
        output.previousGlobalControllerEventCount = 0;
        output.previousGlobalControllerNameLength = 0;
    };

    // ADR-0005: a staged replacement becomes active exactly at its prepared
    // frame, so a callback that frame falls inside renders up to it with the
    // incumbent and on from it with the replacement, as audio callbacks do.
    const auto transport = graphHost_.prepareNextRenderTransportBoundary();
    const auto adoptionFrame = graphHost_.pendingAdoptionFrame();
    if (!transport.playing || !adoptionFrame.has_value()
        || *adoptionFrame <= transport.absoluteFrame
        || *adoptionFrame - transport.absoluteFrame >= frameCount
        || !terminalSegmentScratchReady_.load(std::memory_order_acquire))
    {
        const auto result = graphHost_.renderPreparedTerminalEvents(
            frameCount, output, &context, prepare);
        publishPendingLiveSetActivationOutcome();
        if (result == RealtimeTerminalRenderResult::Rendered)
            clearAdoption();
        return result;
    }

    // Each side renders into its own storage, sized so that together they
    // fit the caller's, and the caller's block changes only when both have
    // rendered. A side that fails leaves the caller's block as it was; the
    // incumbent's side, once rendered, has advanced the transport to the
    // prepared frame, where the next callback starts.
    const auto firstFrames = static_cast<std::size_t>(
        *adoptionFrame - transport.absoluteFrame);
    auto& firstScratch = terminalSegmentScratch_[0];
    auto& secondScratch = terminalSegmentScratch_[1];
    const auto nameCapacity = std::min(
        output.globalControllerName.size(), maximumTerminalSegmentNameBytes);
    RealtimeTerminalOutputBlock first{
        std::span{firstScratch.midiEvents}.first(
            std::min(output.midiEvents.size(), firstScratch.midiEvents.size())),
        0,
        0,
        std::span{firstScratch.globalControllerEvents}.first(std::min(
            output.globalControllerEvents.size(), firstScratch.globalControllerEvents.size())),
        0,
        std::span{firstScratch.globalControllerName}.first(nameCapacity),
        0,
    };
    const auto firstResult = graphHost_.renderPreparedTerminalEvents(
        firstFrames, first, &context, prepare);
    publishPendingLiveSetActivationOutcome();
    if (firstResult != RealtimeTerminalRenderResult::Rendered)
        return firstResult;
    (void) graphHost_.prepareContinuationRenderTransportBoundary();
    RealtimeTerminalOutputBlock second{
        std::span{secondScratch.midiEvents}.first(std::min(
            output.midiEvents.size() - first.midiEventCount, secondScratch.midiEvents.size())),
        0,
        0,
        std::span{secondScratch.globalControllerEvents}.first(std::min(
            output.globalControllerEvents.size() - first.globalControllerEventCount,
            secondScratch.globalControllerEvents.size())),
        0,
        std::span{secondScratch.globalControllerName}.first(nameCapacity),
        0,
    };
    PreparationContext continuation{this, 0, callbackTimeSeconds};
    const auto secondResult = graphHost_.renderPreparedTerminalEvents(
        frameCount - firstFrames, second, &continuation, prepare);
    publishPendingLiveSetActivationOutcome();
    if (secondResult != RealtimeTerminalRenderResult::Rendered)
        return secondResult;

    const auto firstName = std::span{first.globalControllerName}.first(
        first.globalControllerNameLength);
    const auto secondName = std::span{second.globalControllerName}.first(
        second.globalControllerNameLength);
    const auto namesDiffer = first.globalControllerEventCount != 0
        && second.globalControllerEventCount != 0
        && !std::ranges::equal(firstName, secondName);
    if (namesDiffer && firstName.size() > output.previousGlobalControllerName.size())
        return RealtimeTerminalRenderResult::CapacityExceeded;

    std::copy_n(first.midiEvents.begin(), first.midiEventCount, output.midiEvents.begin());
    for (std::size_t index = 0; index < second.midiEventCount; ++index)
    {
        auto& event = output.midiEvents[first.midiEventCount + index];
        event = second.midiEvents[index];
        event.frameOffset += firstFrames;
    }
    output.midiEventCount = first.midiEventCount + second.midiEventCount;
    std::copy_n(first.globalControllerEvents.begin(), first.globalControllerEventCount,
        output.globalControllerEvents.begin());
    for (std::size_t index = 0; index < second.globalControllerEventCount; ++index)
    {
        auto& event = output.globalControllerEvents[first.globalControllerEventCount + index];
        event = second.globalControllerEvents[index];
        event.frameOffset += firstFrames;
    }
    output.globalControllerEventCount =
        first.globalControllerEventCount + second.globalControllerEventCount;
    const auto name = second.globalControllerEventCount != 0 ? secondName : firstName;
    std::ranges::copy(name, output.globalControllerName.begin());
    output.globalControllerNameLength = name.size();
    output.stageId = second.stageId;
    clearAdoption();
    if (first.stageId != second.stageId)
    {
        output.previousStageId = first.stageId;
        output.adoptionFrameOffset = firstFrames;
    }
    if (namesDiffer)
    {
        std::ranges::copy(firstName, output.previousGlobalControllerName.begin());
        output.previousGlobalControllerNameLength = firstName.size();
        output.previousGlobalControllerEventCount = first.globalControllerEventCount;
    }
    return RealtimeTerminalRenderResult::Rendered;
}

void ProjectRuntimeBridge::drainMidiClockTransport(
    double callbackTimeSeconds,
    std::size_t frameCount) noexcept
{
    const auto authorityGeneration =
        midiClockAuthorityGeneration_.load(
            std::memory_order_acquire);
    const auto stopRequest =
        midiClockStopRequestGeneration_.load(
            std::memory_order_acquire);
    const auto acceptedStage = graphHost_.activeStageId();
    if (acceptedStage != audioMidiClockStageId_)
    {
        if (authorityGeneration != 0)
        {
            graphHost_.setPlaying(false);
            audioMidiClockRunning_ = false;
            audioMidiClockLastSignalTimeSeconds_ = 0.0;
            audioMidiClockAwaitingFirstPulseSince_ = 0.0;
            audioMidiClockPhaseFollower_.resetIdentity();
            midiClockLastTickTimeSeconds_.store(
                0.0, std::memory_order_relaxed);
            audioMidiClockMusicalTickQ24_ = 0;
            audioMidiClockPulseTargetQ24_ = 0;
            audioMidiClockMusicalPhaseAnchored_ = false;
            audioMidiClockLastAnchor_ = MidiClockAnchorKind::None;
        }
        audioMidiClockStageId_ = acceptedStage;
    }
    if (stopRequest != adoptedMidiClockStopRequestGeneration_
        || authorityGeneration
            != audioMidiClockAuthorityGeneration_)
    {
        graphHost_.setPlaying(false);
        audioMidiClockRunning_ = false;
        audioMidiClockLastSignalTimeSeconds_ = 0.0;
        audioMidiClockAwaitingFirstPulseSince_ = 0.0;
        midiClockLastTickTimeSeconds_.store(
            0.0, std::memory_order_relaxed);
        audioMidiClockPhaseFollower_.resetIdentity();
        audioMidiClockMusicalTickQ24_ = 0;
        audioMidiClockPulseTargetQ24_ = 0;
        audioMidiClockMusicalPhaseAnchored_ = false;
        audioMidiClockLastAnchor_ = MidiClockAnchorKind::None;
        midiClockRunning_.store(false, std::memory_order_release);
        midiClockSignalStale_.store(false, std::memory_order_release);
        adoptedMidiClockStopRequestGeneration_ = stopRequest;
        audioMidiClockAuthorityGeneration_ = authorityGeneration;
        adoptedInternalTransportResetRequest_ =
            internalTransportResetRequest_.load(
                std::memory_order_acquire);
        midiClockAdoptedAuthorityGeneration_.store(
            authorityGeneration,
            std::memory_order_release);
    }

    std::optional<std::uint64_t> relocationFrame;
    std::uint64_t relocationExpectedStageId = 0;
    std::uint16_t relocationSongPosition = 0;
    std::uint64_t relocationSongPositionCount = 0;
    bool nextPlaying = audioMidiClockRunning_;
    double lastSignalTimeSeconds =
        audioMidiClockLastSignalTimeSeconds_;
    double lastTickTimeSeconds =
        midiClockLastTickTimeSeconds_.load(
            std::memory_order_relaxed);
    std::uint64_t acceptedTicks = 0;
    std::uint64_t acceptedStarts = 0;
    std::uint64_t acceptedContinues = 0;
    std::uint64_t acceptedStops = 0;
    std::uint64_t acceptedSongPositions = 0;
    std::uint64_t songPositionConversionFailures = 0;
    bool reacquiredLock = false;
    bool estimatorStateOverflow = false;

    RealtimeControlCommand command;
    const auto popBudget = midiClockIngress_.capacity();
    for (std::size_t popped = 0;
         popped < popBudget
         && midiClockIngress_.tryPop(command);
         ++popped)
    {
        const auto currentAuthority =
            midiClockAuthorityGeneration_.load(
                std::memory_order_acquire);
        if (currentAuthority == 0
            || command.generation != currentAuthority)
        {
            addSaturatedBounded(midiClockStaleAuthorityCount_);
            continue;
        }
        if (command.midiSourceSlot
                != midiClockExpectedSourceSlot_.load(
                    std::memory_order_relaxed)
            || command.midiSourceGeneration
                != midiClockExpectedSourceGeneration_.load(
                    std::memory_order_relaxed))
        {
            addSaturatedBounded(midiClockWrongSourceCount_);
            continue;
        }
        if (!std::isfinite(command.arrivalTimeSeconds)
            || command.arrivalTimeSeconds <= 0.0)
        {
            addSaturatedBounded(midiClockInvalidRecordCount_);
            continue;
        }
        // Clock commands stamped for a stage that awaits adoption keep driving
        // the incumbent transport until that stage becomes audible.
        const auto commandStage = command.stageId != 0
                && (command.stageId == acceptedStage
                    || command.stageId == graphHost_.pendingStageId())
            ? acceptedStage
            : std::uint64_t{0};
        if (commandStage == 0)
        {
            if (command.kind
                == RealtimeControlCommandKind::MidiSongPosition)
            {
                ++songPositionConversionFailures;
            }
            else
            {
                addSaturatedBounded(midiClockStaleAuthorityCount_);
            }
            continue;
        }
        if ((command.kind == RealtimeControlCommandKind::MidiSongPosition
                && command.midiSongPosition > 0x3fffU)
            || (command.kind
                    != RealtimeControlCommandKind::MidiSongPosition
                && command.midiSongPosition != 0))
        {
            addSaturatedBounded(midiClockInvalidRecordCount_);
            continue;
        }

        switch (command.kind)
        {
        case RealtimeControlCommandKind::MidiClockTick:
        {
            const auto pulse = audioMidiClockPhaseFollower_.observe(
                command.arrivalTimeSeconds);
            switch (pulse)
            {
            case MidiClockPulseResult::Accepted:
                ++acceptedTicks;
                break;
            case MidiClockPulseResult::Acquired:
                ++acceptedTicks;
                addSaturatedBounded(midiClockTempoAcquisitionCount_);
                break;
            case MidiClockPulseResult::Reacquired:
                ++acceptedTicks;
                reacquiredLock = true;
                addSaturatedBounded(midiClockTempoReacquisitionCount_);
                break;
            case MidiClockPulseResult::TimestampRejected:
                addSaturatedBounded(midiClockTimestampRejectionCount_);
                break;
            case MidiClockPulseResult::RangeRejected:
                addSaturatedBounded(midiClockTempoRangeRejectionCount_);
                break;
            case MidiClockPulseResult::Discontinuity:
                addSaturatedBounded(midiClockCadenceDiscontinuityCount_);
                break;
            case MidiClockPulseResult::OverflowRejected:
                estimatorStateOverflow = true;
                break;
            }
            if (pulse == MidiClockPulseResult::Accepted
                || pulse == MidiClockPulseResult::Acquired
                || pulse == MidiClockPulseResult::Reacquired)
            {
                lastTickTimeSeconds = command.arrivalTimeSeconds;
                lastSignalTimeSeconds = command.arrivalTimeSeconds;
                audioMidiClockAwaitingFirstPulseSince_ = 0.0;
                if (nextPlaying && audioMidiClockMusicalPhaseAnchored_)
                {
                    constexpr auto pulseAdvance =
                        static_cast<std::uint64_t>(
                            midiClockProjectTicksPerPulse)
                        * midiClockPhaseOne;
                    if (pulseAdvance
                        > std::numeric_limits<std::uint64_t>::max()
                            - audioMidiClockPulseTargetQ24_)
                    {
                        estimatorStateOverflow = true;
                        break;
                    }
                    audioMidiClockPulseTargetQ24_ += pulseAdvance;
                    auto targetAtCallback =
                        audioMidiClockPulseTargetQ24_;
                    const auto follower =
                        audioMidiClockPhaseFollower_.snapshot();
                    const auto sampleRate =
                        graphHost_.preparedRenderSampleRate();
                    if (const auto rate = midiClockTickRateQ24(
                            follower.intervalNanoseconds,
                            sampleRate);
                        rate.has_value()
                        && std::isfinite(callbackTimeSeconds)
                        && callbackTimeSeconds
                            >= command.arrivalTimeSeconds)
                    {
                        const auto elapsed = std::clamp(
                            (callbackTimeSeconds
                                - command.arrivalTimeSeconds)
                                * sampleRate,
                            0.0,
                            static_cast<double>(frameCount));
                        const auto elapsedFrames =
                            static_cast<std::uint64_t>(
                                std::llround(elapsed));
                        if (elapsedFrames
                            <= std::numeric_limits<std::uint64_t>::max()
                                / *rate)
                        {
                            const auto advance = elapsedFrames * *rate;
                            if (advance
                                > std::numeric_limits<std::uint64_t>::max()
                                    - targetAtCallback)
                            {
                                estimatorStateOverflow = true;
                            }
                            else
                            {
                                targetAtCallback += advance;
                            }
                        }
                    }
                    if (!estimatorStateOverflow
                        && !correctMidiClockPhase(
                            targetAtCallback,
                            audioMidiClockMusicalTickQ24_))
                    {
                        estimatorStateOverflow = true;
                    }
                }
            }
            break;
        }
        case RealtimeControlCommandKind::MidiClockStart:
            if (midiClockSignalStale_.load(std::memory_order_acquire)
                && audioMidiClockPhaseFollower_.snapshot().state
                    != MidiClockLockState::Locked)
            {
                addSaturatedBounded(midiClockStaleAuthorityCount_);
                continue;
            }
            if (!nextPlaying && lastTickTimeSeconds <= 0.0
                && audioMidiClockAwaitingFirstPulseSince_ <= 0.0)
            {
                audioMidiClockAwaitingFirstPulseSince_ =
                    command.arrivalTimeSeconds;
            }
            ++acceptedStarts;
            relocationFrame = 0;
            relocationExpectedStageId = 0;
            relocationSongPosition = 0;
            relocationSongPositionCount = 0;
            nextPlaying = true;
            audioMidiClockMusicalTickQ24_ = 0;
            audioMidiClockPulseTargetQ24_ = 0;
            audioMidiClockMusicalPhaseAnchored_ = true;
            audioMidiClockLastAnchor_ = MidiClockAnchorKind::Start;
            lastSignalTimeSeconds = command.arrivalTimeSeconds;
            break;
        case RealtimeControlCommandKind::MidiClockContinue:
            if (midiClockSignalStale_.load(std::memory_order_acquire)
                && audioMidiClockPhaseFollower_.snapshot().state
                    != MidiClockLockState::Locked)
            {
                addSaturatedBounded(midiClockStaleAuthorityCount_);
                continue;
            }
            if (!nextPlaying && lastTickTimeSeconds <= 0.0
                && audioMidiClockAwaitingFirstPulseSince_ <= 0.0)
            {
                audioMidiClockAwaitingFirstPulseSince_ =
                    command.arrivalTimeSeconds;
            }
            ++acceptedContinues;
            nextPlaying = true;
            lastSignalTimeSeconds = command.arrivalTimeSeconds;
            break;
        case RealtimeControlCommandKind::MidiClockStop:
            ++acceptedStops;
            nextPlaying = false;
            lastSignalTimeSeconds = command.arrivalTimeSeconds;
            break;
        case RealtimeControlCommandKind::MidiSongPosition:
            if (const auto frame = graphHost_.midiSongPositionFrame(
                    command.midiSongPosition,
                    commandStage))
            {
                relocationFrame = *frame;
                relocationExpectedStageId = commandStage;
                relocationSongPosition = command.midiSongPosition;
                ++relocationSongPositionCount;
                ++acceptedSongPositions;
                const auto anchor =
                    static_cast<std::uint64_t>(
                        command.midiSongPosition)
                    * (talas::project::projectTicksPerQuarter / 4)
                    * midiClockPhaseOne;
                audioMidiClockMusicalTickQ24_ = anchor;
                audioMidiClockPulseTargetQ24_ = anchor;
                audioMidiClockMusicalPhaseAnchored_ = true;
                audioMidiClockLastAnchor_ =
                    MidiClockAnchorKind::SongPosition;
                lastSignalTimeSeconds = command.arrivalTimeSeconds;
            }
            else
            {
                ++songPositionConversionFailures;
            }
            break;
        default:
            addSaturatedBounded(midiClockInvalidRecordCount_);
            break;
        }
        if (estimatorStateOverflow)
            break;
    }

    const auto confirmedAuthority =
        midiClockAuthorityGeneration_.load(
            std::memory_order_acquire);
    if (confirmedAuthority != authorityGeneration
        || midiClockStopRequestGeneration_.load(
               std::memory_order_acquire)
            != adoptedMidiClockStopRequestGeneration_)
    {
        addSaturatedBounded(
            midiClockStaleAuthorityCount_,
            acceptedTicks + acceptedStarts
                + acceptedContinues + acceptedStops
                + acceptedSongPositions);
        graphHost_.setPlaying(false);
        audioMidiClockRunning_ = false;
        audioMidiClockLastSignalTimeSeconds_ = 0.0;
        audioMidiClockAwaitingFirstPulseSince_ = 0.0;
        midiClockRunning_.store(false, std::memory_order_release);
        midiClockSignalStale_.store(false, std::memory_order_release);
        midiClockAdoptedAuthorityGeneration_.store(
            0,
            std::memory_order_release);
        midiClockLastTickTimeSeconds_.store(
            0.0, std::memory_order_relaxed);
        audioMidiClockPhaseFollower_.resetIdentity();
        audioMidiClockMusicalTickQ24_ = 0;
        audioMidiClockPulseTargetQ24_ = 0;
        audioMidiClockMusicalPhaseAnchored_ = false;
        audioMidiClockLastAnchor_ = MidiClockAnchorKind::None;
        graphHost_.clearExternalMusicalTick();
        return;
    }

    const auto failEstimatorState = [&] {
        if (!estimatorStateOverflow)
            return;
        graphHost_.setPlaying(false);
        nextPlaying = false;
        audioMidiClockRunning_ = false;
        audioMidiClockAwaitingFirstPulseSince_ = 0.0;
        midiClockSignalStale_.store(true, std::memory_order_release);
        audioMidiClockPhaseFollower_.markStale();
        audioMidiClockMusicalTickQ24_ = 0;
        audioMidiClockPulseTargetQ24_ = 0;
        audioMidiClockMusicalPhaseAnchored_ = false;
        audioMidiClockLastAnchor_ = MidiClockAnchorKind::None;
        relocationFrame.reset();
        addSaturatedBounded(midiClockEstimatorOverflowCount_);
        estimatorStateOverflow = false;
    };
    failEstimatorState();

    if (relocationFrame.has_value())
    {
        resetRealtimeControlTiming();
        graphHost_.relocateTransportAtNextRender(
            *relocationFrame,
            relocationExpectedStageId,
            relocationSongPosition,
            relocationSongPositionCount);
    }
    if (confirmedAuthority != 0)
    {
        adoptedInternalTransportResetRequest_ =
            internalTransportResetRequest_.load(
                std::memory_order_acquire);
        graphHost_.setPlaying(nextPlaying);
    }
    else
    {
        const auto resetRequest =
            internalTransportResetRequest_.load(
                std::memory_order_acquire);
        if (resetRequest != adoptedInternalTransportResetRequest_)
        {
            graphHost_.relocateTransportAtNextRender(0);
            adoptedInternalTransportResetRequest_ = resetRequest;
        }
        const auto request = internalTransportRequest_.load(
            std::memory_order_acquire);
        if (request != adoptedInternalTransportRequest_)
        {
            graphHost_.setPlaying((request & 1U) != 0);
            adoptedInternalTransportRequest_ = request;
        }
    }
    audioMidiClockRunning_ = nextPlaying;
    audioMidiClockLastSignalTimeSeconds_ =
        lastSignalTimeSeconds;

    if (reacquiredLock)
    {
        midiClockSignalStale_.store(false, std::memory_order_release);
    }

    const auto timeoutReference = lastTickTimeSeconds > 0.0
        ? lastTickTimeSeconds
        : audioMidiClockAwaitingFirstPulseSince_;
    const auto timeout = confirmedAuthority != 0
        && !midiClockSignalStale_.load(std::memory_order_acquire)
        && std::isfinite(callbackTimeSeconds)
        && callbackTimeSeconds > 0.0
        && timeoutReference > 0.0
        && callbackTimeSeconds
            - timeoutReference
            > static_cast<double>(midiClockSignalTimeoutNanoseconds)
                / static_cast<double>(midiClockNanosecondsPerSecond);
    if (timeout)
    {
        graphHost_.setPlaying(false);
        audioMidiClockRunning_ = false;
        audioMidiClockAwaitingFirstPulseSince_ = 0.0;
        addSaturatedBounded(midiClockSignalTimeoutCount_);
        midiClockSignalStale_.store(true, std::memory_order_release);
        audioMidiClockPhaseFollower_.markStale();
    }
    else if (confirmedAuthority != 0
        && lastTickTimeSeconds > 0.0
        && std::isfinite(callbackTimeSeconds)
        && callbackTimeSeconds > lastTickTimeSeconds)
    {
        const auto phase = audioMidiClockPhaseFollower_.snapshot();
        if (phase.state == MidiClockLockState::Locked
            && phase.intervalNanoseconds != 0
            && callbackTimeSeconds - lastTickTimeSeconds
                > static_cast<double>(
                      phase.intervalNanoseconds
                      * midiClockHoldoverIntervalMultiplier)
                    / static_cast<double>(midiClockNanosecondsPerSecond))
        {
            audioMidiClockPhaseFollower_.markHoldover();
            addSaturatedBounded(midiClockHoldoverEntryCount_);
        }
    }

    addSaturatedBounded(midiClockAcceptedTickCount_, acceptedTicks);
    addSaturatedBounded(midiClockAcceptedStartCount_, acceptedStarts);
    addSaturatedBounded(
        midiClockAcceptedContinueCount_, acceptedContinues);
    addSaturatedBounded(midiClockAcceptedStopCount_, acceptedStops);
    addSaturatedBounded(
        midiClockSongPositionConversionFailureCount_,
        songPositionConversionFailures);
    if (lastTickTimeSeconds > 0.0)
    {
        midiClockLastTickTimeSeconds_.store(
            lastTickTimeSeconds,
            std::memory_order_relaxed);
    }
    midiClockRunning_.store(
        audioMidiClockRunning_,
        std::memory_order_release);
    if (confirmedAuthority == 0)
    {
        graphHost_.clearExternalMusicalTick();
    }
    else if (audioMidiClockMusicalPhaseAnchored_)
    {
        const auto phase = audioMidiClockPhaseFollower_.snapshot();
        if (audioMidiClockRunning_
            && (phase.state == MidiClockLockState::Locked
                || phase.state == MidiClockLockState::Holdover))
        {
            if (const auto rate = midiClockTickRateQ24(
                    phase.intervalNanoseconds,
                    graphHost_.preparedRenderSampleRate());
                rate.has_value())
            {
                if (frameCount != 0
                    && *rate
                        > std::numeric_limits<std::uint64_t>::max()
                            / frameCount)
                {
                    estimatorStateOverflow = true;
                }
                else
                {
                    const auto advance = *rate * frameCount;
                    if (advance
                        > std::numeric_limits<std::uint64_t>::max()
                            - audioMidiClockMusicalTickQ24_)
                    {
                        estimatorStateOverflow = true;
                    }
                    else
                    {
                        audioMidiClockMusicalTickQ24_ += advance;
                    }
                }
            }
        }
        failEstimatorState();
        if (audioMidiClockMusicalPhaseAnchored_)
        {
            graphHost_.setExternalMusicalTick(
                audioMidiClockMusicalTickQ24_
                    >> midiClockPhaseFractionBits,
                acceptedStage,
                confirmedAuthority,
                audioMidiClockPhaseFollower_.snapshot(),
                audioMidiClockLastAnchor_,
                lastTickTimeSeconds);
        }
        else
        {
            graphHost_.setExternalMusicalTick(
                0,
                0,
                confirmedAuthority,
                audioMidiClockPhaseFollower_.snapshot(),
                audioMidiClockLastAnchor_,
                lastTickTimeSeconds);
        }
    }
    else
    {
        graphHost_.setExternalMusicalTick(
            0,
            0,
            confirmedAuthority,
            audioMidiClockPhaseFollower_.snapshot(),
            audioMidiClockLastAnchor_,
            lastTickTimeSeconds);
    }
}

void ProjectRuntimeBridge::drainMtcTransport(
    double callbackTimeSeconds,
    std::size_t frameCount) noexcept
{
    const auto authorityGeneration = mtcAuthorityGeneration_.load(
        std::memory_order_acquire);
    const auto stopRequest = mtcStopRequestGeneration_.load(
        std::memory_order_acquire);
    const auto evidenceRejection = mtcEvidenceRejectionGeneration_.load(
        std::memory_order_acquire);
    const auto acceptedStage = graphHost_.activeStageId();
    const MtcTransportIdentity identity = authorityGeneration != 0
        ? MtcTransportIdentity{
              .authorityGeneration = authorityGeneration,
              .sourceGeneration = mtcExpectedSourceGeneration_.load(
                  std::memory_order_relaxed),
              .stageId = acceptedStage,
              .sourceSlot = mtcExpectedSourceSlot_.load(
                  std::memory_order_relaxed),
          }
        : MtcTransportIdentity{};
    const auto priorIdentity = audioMtcFollower_.snapshot().identity;
    const auto identityChanged = identity != priorIdentity;
    const auto stopRequested =
        stopRequest != adoptedMtcStopRequestGeneration_;
    auto evidenceRejected = evidenceRejection
        != adoptedMtcEvidenceRejectionGeneration_;
    if (stopRequested || evidenceRejected || identityChanged)
    {
        graphHost_.setPlaying(false);
        if (identityChanged)
        {
            audioMtcFollower_.configure(identity);
        }
        if (evidenceRejected && identity.valid())
        {
            audioMtcFollower_.rejectEvidence();
        }
        audioMtcAuthorityGeneration_ = authorityGeneration;
        audioMtcStageId_ = acceptedStage;
        if (identityChanged || stopRequested)
        {
            audioMtcMappedProjectFrame_ = 0;
            audioMtcSoundStartTimeSeconds_ = 0.0;
        }
        adoptedMtcStopRequestGeneration_ = stopRequest;
        adoptedMtcEvidenceRejectionGeneration_ = evidenceRejection;
    }

    auto positionResult = MtcTransportResult::PieceAccepted;
    auto hasPositionAction = false;
    auto relocationRequired = false;
    MtcTransportRecord record;
    const auto popBudget = mtcIngress_.capacity();
    for (std::size_t popped = 0;
         popped < popBudget && mtcIngress_.tryPop(record);
         ++popped)
    {
        if (evidenceRejected
            || record.evidenceGeneration != evidenceRejection)
            continue;
        const auto result = audioMtcFollower_.observe(record);
        switch (result)
        {
        case MtcTransportResult::Acquired:
        case MtcTransportResult::Reacquired:
        case MtcTransportResult::Located:
        case MtcTransportResult::Discontinuity:
            positionResult = result;
            hasPositionAction = true;
            relocationRequired = true;
            break;
        case MtcTransportResult::Advanced:
            positionResult = result;
            hasPositionAction = true;
            break;
        case MtcTransportResult::IdentityRejected:
        case MtcTransportResult::TimestampRejected:
        case MtcTransportResult::Reordered:
        case MtcTransportResult::InvalidMessage:
        case MtcTransportResult::InvalidLabel:
        case MtcTransportResult::UnsupportedDirection:
        case MtcTransportResult::ArithmeticOverflow:
            hasPositionAction = false;
            relocationRequired = false;
            break;
        case MtcTransportResult::PieceAccepted:
        case MtcTransportResult::TimedOut:
            break;
        }
    }

    const auto confirmedEvidenceRejection =
        mtcEvidenceRejectionGeneration_.load(std::memory_order_acquire);
    if (confirmedEvidenceRejection != evidenceRejection)
    {
        evidenceRejected = true;
        hasPositionAction = false;
        relocationRequired = false;
        graphHost_.setPlaying(false);
        if (identity.valid())
        {
            audioMtcFollower_.rejectEvidence();
        }
        adoptedMtcEvidenceRejectionGeneration_ =
            confirmedEvidenceRejection;
    }

    auto snapshot = audioMtcFollower_.snapshot();
    const auto sampleRateValue = graphHost_.preparedRenderSampleRate();
    const auto sampleRateValid = std::isfinite(sampleRateValue)
        && sampleRateValue >= 1.0
        && sampleRateValue
            <= static_cast<double>(std::numeric_limits<std::uint32_t>::max())
        && std::floor(sampleRateValue) == sampleRateValue;
    std::uint64_t mappedFrame = audioMtcMappedProjectFrame_;
    const auto resultHasPosition = snapshot.hasTimeCode
        && hasPositionAction;
    auto mappingValid = true;
    if (resultHasPosition)
    {
        mappingValid = sampleRateValid
            && smpteTimeCodeToSampleFrame(
                snapshot.timeCode,
                static_cast<std::uint32_t>(sampleRateValue),
                mappedFrame);
        // Time code at the offset is the Timeline's
        // start; earlier time code holds it there.
        if (mappingValid)
        {
            const auto offset = static_cast<std::uint64_t>(std::llround(
                acceptedTimeCodeOffsetSeconds_.load(std::memory_order_relaxed) * sampleRateValue));
            mappedFrame = mappedFrame > offset ? mappedFrame - offset : 0;
        }
        if (mappingValid
            && positionResult != MtcTransportResult::Located
            && std::isfinite(callbackTimeSeconds)
            && callbackTimeSeconds >= snapshot.lastCompleteTimeSeconds)
        {
            const auto elapsed = std::clamp(
                (callbackTimeSeconds - snapshot.lastCompleteTimeSeconds)
                    * sampleRateValue,
                0.0,
                static_cast<double>(frameCount));
            const auto elapsedFrames = static_cast<std::uint64_t>(
                std::llround(elapsed));
            if (elapsedFrames
                > std::numeric_limits<std::uint64_t>::max() - mappedFrame)
            {
                mappingValid = false;
            }
            else
            {
                mappedFrame += elapsedFrames;
            }
        }
    }
    if (resultHasPosition && !mappingValid)
    {
        addSaturatedBounded(mtcMappingFailureCount_);
        audioMtcFollower_.markDiscontinuity(
            MtcDiscontinuityReason::ArithmeticOverflow);
        snapshot = audioMtcFollower_.snapshot();
        positionResult = MtcTransportResult::ArithmeticOverflow;
        hasPositionAction = false;
        relocationRequired = false;
        graphHost_.setPlaying(false);
    }

    if (mappingValid && hasPositionAction
        && positionResult == MtcTransportResult::Advanced
        && !relocationRequired)
    {
        const auto profile = smpteRateRational(snapshot.timeCode.rate);
        const auto numerator = static_cast<std::uint64_t>(sampleRateValue)
            * profile.denominator;
        const auto tolerance =
            (numerator + profile.numerator - 1) / profile.numerator;
        const auto currentFrame =
            graphHost_.transportSnapshot().absoluteFrame;
        const auto distance = currentFrame > mappedFrame
            ? currentFrame - mappedFrame
            : mappedFrame - currentFrame;
        if (distance > tolerance)
        {
            audioMtcFollower_.markDiscontinuity(
                currentFrame > mappedFrame
                    ? MtcDiscontinuityReason::BackwardMovement
                    : MtcDiscontinuityReason::ForwardJump);
            snapshot = audioMtcFollower_.snapshot();
            positionResult = MtcTransportResult::Discontinuity;
            relocationRequired = true;
        }
    }

    const auto relocate = mappingValid && hasPositionAction
        && relocationRequired;
    if (relocate)
    {
        resetRealtimeControlTiming();
        graphHost_.setPlaying(false);
        graphHost_.relocateTransportAtNextRender(
            mappedFrame, acceptedStage);
        audioMtcMappedProjectFrame_ = mappedFrame;
        graphHost_.setPlaying(snapshot.runningEvidence);
    }
    else if (mappingValid && hasPositionAction
        && positionResult == MtcTransportResult::Advanced)
    {
        audioMtcMappedProjectFrame_ = mappedFrame;
        graphHost_.setPlaying(snapshot.runningEvidence);
    }
    if (authorityGeneration != 0 && !snapshot.runningEvidence)
        graphHost_.setPlaying(false);

    if (audioMtcFollower_.advanceTime(callbackTimeSeconds)
        == MtcTransportResult::TimedOut)
    {
        graphHost_.setPlaying(false);
        snapshot = audioMtcFollower_.snapshot();
    }
    if (!snapshot.runningEvidence)
        audioMtcSoundStartTimeSeconds_ = 0.0;

    const auto confirmedAuthority = mtcAuthorityGeneration_.load(
        std::memory_order_acquire);
    if (confirmedAuthority != authorityGeneration
        || mtcStopRequestGeneration_.load(std::memory_order_acquire)
            != adoptedMtcStopRequestGeneration_)
    {
        addSaturatedBounded(mtcStaleAuthorityCount_);
        graphHost_.setPlaying(false);
        graphHost_.clearExternalMtc();
        audioMtcFollower_.configure({});
        audioMtcAuthorityGeneration_ = 0;
        audioMtcStageId_ = 0;
        audioMtcMappedProjectFrame_ = 0;
        audioMtcSoundStartTimeSeconds_ = 0.0;
        return;
    }

    snapshot = audioMtcFollower_.snapshot();
    if (confirmedAuthority == 0)
    {
        graphHost_.clearExternalMtc();
    }
    else
    {
        graphHost_.setExternalMtc(snapshot, audioMtcMappedProjectFrame_);
    }
    if (confirmedAuthority != 0 && mappingValid && hasPositionAction
        && snapshot.hasTimeCode)
    {
        const auto rate = smpteRateRational(snapshot.timeCode.rate);
        const auto positionSeconds =
            static_cast<double>(snapshot.timeCode.absoluteFrame)
            * static_cast<double>(rate.denominator)
            / static_cast<double>(rate.numerator);
        const auto stageGeneration = latestStageGeneration_.load(
            std::memory_order_acquire);
        if (stageGeneration != 0 && std::isfinite(positionSeconds))
        {
            const auto publishTimeSource = [&](
                                               std::string_view name,
                                               double value) noexcept {
                for (const auto domain : {
                         talas::expr::ValueDomain::Scalar,
                         talas::expr::ValueDomain::Seconds})
                {
                    if (const auto id = realtimeEventValueIdForLatestStage(
                            name, domain))
                    {
                        (void)enqueueEventValueAtTimeForStage(
                            *id, value, domain,
                            snapshot.lastCompleteTimeSeconds,
                            stageGeneration);
                        break;
                    }
                }
            };
            publishTimeSource("MIDITimeCode", positionSeconds);
            if (snapshot.runningEvidence)
            {
                if (audioMtcSoundStartTimeSeconds_ == 0.0)
                    audioMtcSoundStartTimeSeconds_ =
                        snapshot.lastCompleteTimeSeconds;
                const auto elapsed = std::max(0.0,
                    snapshot.lastCompleteTimeSeconds
                        - audioMtcSoundStartTimeSeconds_);
                publishTimeSource("RealTime", elapsed);
            }
        }
    }
    mtcAcceptedQuarterFrameCount_.store(
        snapshot.acceptedQuarterFrames, std::memory_order_relaxed);
    mtcAcceptedCompleteSetCount_.store(
        snapshot.acceptedCompleteSets, std::memory_order_relaxed);
    mtcAcceptedFullFrameLocateCount_.store(
        snapshot.acceptedFullFrameLocates, std::memory_order_relaxed);
    mtcAcquisitionCount_.store(snapshot.acquisitions, std::memory_order_relaxed);
    mtcReacquisitionCount_.store(snapshot.reacquisitions, std::memory_order_relaxed);
    mtcStaleTimeoutCount_.store(snapshot.staleTimeouts, std::memory_order_relaxed);
    mtcReorderedPieceCount_.store(
        snapshot.reorderedOrSkippedPieces, std::memory_order_relaxed);
    mtcFollowerIdentityRejectionCount_.store(
        snapshot.identityRejections, std::memory_order_relaxed);
    mtcTimestampRejectionCount_.store(
        snapshot.timestampRejections, std::memory_order_relaxed);
    mtcFollowerInvalidMessageCount_.store(
        snapshot.invalidMessages, std::memory_order_relaxed);
    mtcInvalidLabelOrRateCount_.store(
        snapshot.invalidLabelsOrRates, std::memory_order_relaxed);
    mtcUnsupportedDirectionCount_.store(
        snapshot.unsupportedDirections, std::memory_order_relaxed);
    mtcArithmeticOverflowCount_.store(
        snapshot.arithmeticOverflows, std::memory_order_relaxed);
    mtcDiscontinuityCount_.store(
        snapshot.discontinuities, std::memory_order_relaxed);
}

void ProjectRuntimeBridge::drainRealtimeLiveSetRequests(
    const PreparedLiveSet& prepared, RealtimeTransportSnapshot transport) noexcept
{
    const auto& binding = prepared.binding();
    RealtimeLiveSetAuthority authority{
        .documentAuthorityGeneration =
            binding.editorDocumentAuthority
                ? editor_.documentAuthorityGeneration()
                : binding.documentAuthorityGeneration,
        .stageGeneration = binding.stageGeneration,
        .stageId = binding.stageId,
        .transportEpoch = transport.epoch,
        .transportDiscontinuitySequence = transport.discontinuitySequence,
    };
    for (std::size_t index = 0; index < authority.sourceGenerations.size(); ++index)
    {
        if (!readLiveSetSourceAuthority(
                index, authority.sourceGenerations[index], authority.routeGenerations[index]))
        {
            authority.sourceGenerations[index] = std::numeric_limits<std::uint64_t>::max();
            authority.routeGenerations[index] = std::numeric_limits<std::uint64_t>::max();
        }
    }
    std::uint64_t latestDiscarded = 0;
    const auto discarded = liveSetScheduler_.discardStale(
        authority, &latestDiscarded);
    liveSetStaleAuthorityCount_.fetch_add(
        discarded, std::memory_order_relaxed);
    publishLiveSetTerminal(
        latestDiscarded, RealtimeLiveSetTerminalOutcome::StaleAuthority);

    RealtimeLiveSetRequest request;
    const auto popBudget = liveSetIngress_.capacity();
    for (std::size_t popped = 0; popped < popBudget && liveSetIngress_.tryPop(request); ++popped)
    {
        if (!realtimeLiveSetRequestMatchesAuthority(request, authority) ||
            request.preparedCellIndex >= prepared.cells().size())
        {
            liveSetStaleAuthorityCount_.fetch_add(1, std::memory_order_relaxed);
            publishLiveSetTerminal(
                request.requestGeneration,
                RealtimeLiveSetTerminalOutcome::StaleAuthority);
            continue;
        }
        // Immediate means "at the next callback". A click published while a
        // callback was rendering resolved to that callback's start frame.
        if (prepared.cells()[request.preparedCellIndex].quantization.kind
                == talas::project::ProjectLiveSetQuantizationKind::Immediate
            && request.resolvedFrame < transport.absoluteFrame)
        {
            request.resolvedFrame = transport.absoluteFrame;
        }
        if (!liveSetScheduler_.schedule(request))
        {
            liveSetQueueOverflowCount_.fetch_add(1, std::memory_order_relaxed);
            publishLiveSetTerminal(
                request.requestGeneration,
                RealtimeLiveSetTerminalOutcome::Failed);
        }
    }
}

bool ProjectRuntimeBridge::applyRealtimeLiveSetRequest(
    const PreparedLiveSet& prepared, const RealtimeLiveSetRequest& request) noexcept
{
    const ProducerGate producerGate(sessionProducerGate_, ProducerGate::Mode::TryOnce);
    if (!producerGate || request.preparedCellIndex >= prepared.cells().size())
        return false;
    const auto& cell = prepared.cells()[request.preparedCellIndex];
    const auto effect = preparedLiveSetEffect(
        prepared, request.preparedCellIndex);
    if (!cell.ready() || !effect.has_value())
        return false;
    const auto& binding = prepared.binding();
    if (request.documentAuthorityGeneration != binding.documentAuthorityGeneration ||
        request.stageGeneration != binding.stageGeneration || request.stageId != binding.stageId ||
        (binding.editorDocumentAuthority &&
         editor_.documentAuthorityGeneration() != binding.documentAuthorityGeneration) ||
        graphHost_.activeStageId() != binding.stageId)
    {
        return false;
    }
    const auto targetMatches =
        (effect->kind == PreparedLiveSetEffectKind::ActivateSound
         && request.targetKind == RealtimeLiveSetTargetKind::Sound)
        || ((effect->kind
                 == PreparedLiveSetEffectKind::ActivateOverlaySnapshot
             || effect->kind
                 == PreparedLiveSetEffectKind::ApplyDirectSnapshot)
            && request.targetKind == RealtimeLiveSetTargetKind::Snapshot)
        || (effect->kind == PreparedLiveSetEffectKind::SeekScoreRegion
            && request.targetKind == RealtimeLiveSetTargetKind::ScoreRegion)
        || (effect->kind == PreparedLiveSetEffectKind::SetTrackMute
            && request.targetKind == RealtimeLiveSetTargetKind::TrackMute)
        || (effect->kind == PreparedLiveSetEffectKind::Stop
            && request.targetKind == RealtimeLiveSetTargetKind::Stop);
    if (!targetMatches)
        return false;

    struct ExecutionContext
    {
        ProjectRuntimeBridge* bridge = nullptr;
        const PreparedLiveSetBinding* binding = nullptr;
        const PreparedLiveSet* prepared = nullptr;
    } executionContext{this, &binding, &prepared};
    const PreparedLiveSetExecutionCallbacks callbacks{
        .context = &executionContext,
        .activateSound = [](void* context,
                            const PreparedLiveSetEffect& selected) noexcept {
            auto& bridge = *static_cast<ExecutionContext*>(context)->bridge;
            if (!bridge.graphHost_.activatePreparedLiveSetProgram(
                    selected.cellIndex))
            {
                return PreparedLiveSetApplicationResult::Rejected;
            }
            bridge.graphHost_.setPlaying(true);
            bridge.audioLiveSetScoreEndFrame_ = 0;
            bridge.audioLiveSetScoreStageId_ = 0;
            bridge.audioLiveSetScoreDiscontinuitySequence_ = 0;
            bridge.liveSetScoreRegionActive_.store(
                false, std::memory_order_release);
            return PreparedLiveSetApplicationResult::
                PendingRenderConfirmation;
        },
        .activateOverlaySnapshot = [](
            void* context, const PreparedLiveSetEffect& selected) noexcept {
            auto& bridge = *static_cast<ExecutionContext*>(context)->bridge;
            if (!bridge.graphHost_.activatePreparedLiveSetProgram(
                    selected.cellIndex, selected.rampFrames))
            {
                return PreparedLiveSetApplicationResult::Rejected;
            }
            bridge.audioLiveSetScoreEndFrame_ = 0;
            bridge.audioLiveSetScoreStageId_ = 0;
            bridge.audioLiveSetScoreDiscontinuitySequence_ = 0;
            bridge.liveSetScoreRegionActive_.store(
                false, std::memory_order_release);
            return PreparedLiveSetApplicationResult::
                PendingRenderConfirmation;
        },
        .applyDirectSnapshot = [](
            void* context, const PreparedLiveSetEffect& selected) noexcept {
            const auto& execution = *static_cast<ExecutionContext*>(context);
            auto& bridge = *execution.bridge;
            const SnapshotDirectTransaction transaction{
                .stageId = execution.binding->stageId,
                .generation = execution.binding->stageGeneration,
                .frameOffset = 0,
                .rampFrames = selected.rampFrames,
                .updates = selected.eventValueUpdates,
                .variableUpdates = selected.variableUpdates,
            };
            if (bridge.graphHost_.scheduleSnapshotDirectTransaction(
                    transaction)
                != SnapshotDirectTransactionResult::Accepted)
            {
                return PreparedLiveSetApplicationResult::Rejected;
            }
            bridge.graphHost_.deactivatePreparedLiveSetProgram();
            bridge.audioLiveSetScoreEndFrame_ = 0;
            bridge.audioLiveSetScoreStageId_ = 0;
            bridge.audioLiveSetScoreDiscontinuitySequence_ = 0;
            bridge.liveSetScoreRegionActive_.store(
                false, std::memory_order_release);
            return PreparedLiveSetApplicationResult::Applied;
        },
        .seekScoreRegion = [](
            void* context, const PreparedLiveSetEffect& selected) noexcept {
            const auto& execution = *static_cast<ExecutionContext*>(context);
            auto& bridge = *execution.bridge;
            bridge.graphHost_.deactivatePreparedLiveSetProgram();
            bridge.audioLiveSetScoreEndFrame_ = selected.scoreEndFrame;
            bridge.audioLiveSetScoreStageId_ = execution.binding->stageId;
            bridge.audioLiveSetScoreDiscontinuitySequence_ = 0;
            bridge.liveSetScoreRegionActive_.store(
                true, std::memory_order_release);
            bridge.resetRealtimeControlTiming();
            bridge.graphHost_.relocateTransportAtNextRender(
                selected.scoreStartFrame, execution.binding->stageId);
            bridge.graphHost_.setPlaying(true);
            return PreparedLiveSetApplicationResult::Applied;
        },
        .setTrackMute = [](
            void* context, const PreparedLiveSetEffect& selected) noexcept {
            const auto& execution = *static_cast<ExecutionContext*>(context);
            auto& bridge = *execution.bridge;
            const auto activeCellIndexPlusOne =
                bridge.graphHost_.activePreparedLiveSetCellIndexPlusOne();
            if (activeCellIndexPlusOne == 0
                || activeCellIndexPlusOne > execution.prepared->cells().size()
                || selected.cellIndex >= execution.prepared->cells().size()
                || execution.prepared->cells()[activeCellIndexPlusOne - 1U]
                       .liveSetId
                    != execution.prepared->cells()[selected.cellIndex]
                           .liveSetId)
                return PreparedLiveSetApplicationResult::Rejected;
            return bridge.graphHost_.setPreparedLiveSetSoundColumnMuted(
                       selected.column, selected.muted)
                ? PreparedLiveSetApplicationResult::Applied
                : PreparedLiveSetApplicationResult::Rejected;
        },
        .stop = [](void* context,
                   const PreparedLiveSetEffect&) noexcept {
            auto& bridge = *static_cast<ExecutionContext*>(context)->bridge;
            bridge.graphHost_.deactivatePreparedLiveSetProgram();
            bridge.graphHost_.setPlaying(false);
            bridge.audioLiveSetScoreEndFrame_ = 0;
            bridge.audioLiveSetScoreStageId_ = 0;
            bridge.audioLiveSetScoreDiscontinuitySequence_ = 0;
            bridge.liveSetScoreRegionActive_.store(
                false, std::memory_order_release);
            return PreparedLiveSetApplicationResult::Applied;
        },
    };
    const auto outcome = executePreparedLiveSetEffect(
        *effect,
        request.requestGeneration,
        {
            .transportFrame = graphHost_.transportFramePosition(),
            .playing = graphHost_.transportSnapshot().playing,
        },
        callbacks);
    if (!outcome.has_value())
        return false;
    if (outcome->completionRequiresRenderConfirmation)
    {
        audioLiveSetPendingRequestGeneration_ = request.requestGeneration;
        audioLiveSetPendingCellIndexPlusOne_ =
            static_cast<std::uint32_t>(request.preparedCellIndex) + 1U;
        return true;
    }
    audioLiveSetPendingRequestGeneration_ = 0;
    audioLiveSetPendingCellIndexPlusOne_ = 0;
    if (effect->kind == PreparedLiveSetEffectKind::SetTrackMute)
    {
        RealtimeLiveSetActiveState active;
        if (liveSetActiveState_.read(active))
            publishLiveSetActiveState(
                active.requestGeneration,
                active.cellIndexPlusOne,
                active.scoreRegionActive);
    }
    else
        publishLiveSetActiveState(
            outcome->state.activeRequestGeneration,
            outcome->state.activeCellIndexPlusOne,
            outcome->state.scoreRegionActive);
    publishLiveSetTerminal(
        request.requestGeneration,
        RealtimeLiveSetTerminalOutcome::Completed);
    liveSetCompletedActivationCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ProjectRuntimeBridge::publishPendingLiveSetActivationOutcome() noexcept
{
    if (audioLiveSetPendingRequestGeneration_ == 0 || audioLiveSetPendingCellIndexPlusOne_ == 0 ||
        graphHost_.preparedLiveSetActivationPending())
    {
        return;
    }
    if (graphHost_.activePreparedLiveSetCellIndexPlusOne() == audioLiveSetPendingCellIndexPlusOne_)
    {
        publishLiveSetActiveState(
            audioLiveSetPendingRequestGeneration_,
            audioLiveSetPendingCellIndexPlusOne_,
            false);
        publishLiveSetTerminal(
            audioLiveSetPendingRequestGeneration_,
            RealtimeLiveSetTerminalOutcome::Completed);
        liveSetCompletedActivationCount_.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        publishLiveSetTerminal(
            audioLiveSetPendingRequestGeneration_,
            RealtimeLiveSetTerminalOutcome::Failed);
        liveSetFailedActivationCount_.fetch_add(1, std::memory_order_relaxed);
    }
    audioLiveSetPendingRequestGeneration_ = 0;
    audioLiveSetPendingCellIndexPlusOne_ = 0;
}

void ProjectRuntimeBridge::enqueueLiveToolEventTriggersFromAudio(
    RealtimeAudioEngine& renderTarget,
    std::string_view eventName,
    talas::expr::ValueDomain domain) noexcept
{
    const auto* tools = renderTarget.preparedLiveTools();
    if (tools == nullptr || tools->binding().stageId != renderTarget.stageId())
        return;
    const auto& binding = tools->binding();
    for (std::size_t toolIndex = 0;
         toolIndex < tools->tools().size();
         ++toolIndex)
    {
        const auto& tool = tools->tools()[toolIndex];
        const auto matches = std::ranges::any_of(
            tool.transitions,
            [&](const PreparedLiveToolTransition& transition) {
                return transition.trigger.kind
                        == talas::project::ProjectLiveToolTriggerKind::EventValue
                    && transition.trigger.eventName == eventName
                    && transition.trigger.eventDomain == domain;
            });
        if (!matches)
            continue;
        auto generation = liveToolRequestGeneration_.load(
            std::memory_order_relaxed);
        auto published = false;
        for (std::size_t attempt = 0;
             attempt < RealtimeLiveToolIngress::maximumProducerAttempts;
             ++attempt)
        {
            if (generation == std::numeric_limits<std::uint64_t>::max())
                break;
            if (liveToolRequestGeneration_.compare_exchange_strong(
                    generation,
                    generation + 1U,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                ++generation;
                published = true;
                break;
            }
        }
        RealtimeLiveToolTriggerRequest request;
        request.triggerKind =
            talas::project::ProjectLiveToolTriggerKind::EventValue;
        request.sourceKind = RealtimeLiveToolSourceKind::EventValue;
        request.preparedToolIndex = static_cast<std::uint16_t>(toolIndex);
        request.domain = domain;
        request.documentAuthorityGeneration =
            binding.documentAuthorityGeneration;
        request.stageGeneration = binding.stageGeneration;
        request.stageId = binding.stageId;
        request.sourceGeneration = binding.stageGeneration;
        request.routeGeneration = binding.stageGeneration;
        request.requestGeneration = generation;
        if (!published
            || !setRealtimeLiveToolTriggerIdentifier(request, eventName)
            || !liveToolIngress_.tryPush(request))
        {
            liveToolQueueOverflowCount_.fetch_add(
                1, std::memory_order_relaxed);
            continue;
        }
        liveToolAcceptedTriggerCount_.fetch_add(
            1, std::memory_order_relaxed);
    }
}

void ProjectRuntimeBridge::drainRealtimeLiveToolTriggers(
    RealtimeAudioEngine& renderTarget,
    const PreparedLiveSet& liveSet,
    const PreparedLiveTools& tools) noexcept
{
    if (tools.binding() != liveSet.binding()
        || renderTarget.stageId() != tools.binding().stageId)
    {
        RealtimeLiveToolTriggerRequest stale;
        const auto budget = liveToolIngress_.capacity();
        for (std::size_t count = 0;
             count < budget && liveToolIngress_.tryPop(stale);
             ++count)
        {
            liveToolStaleAuthorityCount_.fetch_add(
                1, std::memory_order_relaxed);
        }
        return;
    }
    if (audioLiveToolStageId_ != tools.binding().stageId)
    {
        PreparedLiveToolRuntimeState candidate;
        if (!initializePreparedLiveToolRuntimeState(tools, candidate))
        {
            liveToolRejectedTriggerCount_.fetch_add(
                1, std::memory_order_relaxed);
            liveToolStateInitializationRejectedCount_.fetch_add(
                1, std::memory_order_relaxed);
            return;
        }
        audioLiveToolState_ = candidate;
        audioLiveToolStageId_ = tools.binding().stageId;
        audioLiveToolLastPeriodicTick_ = 0;
        audioLiveToolHasPeriodicTick_ = false;
        static_cast<void>(liveToolStatePublication_.publishAll(
            audioLiveToolState_));
    }
    const auto restoreGeneration = liveToolRestoreRequestGeneration_.load(
        std::memory_order_acquire);
    if (restoreGeneration != audioLiveToolRestoreRequestGeneration_)
    {
        PreparedLiveToolRuntimeState restored;
        if (liveToolRestorePublication_.read(restored)
            && restored.documentAuthorityGeneration
                == tools.binding().documentAuthorityGeneration
            && restored.stageGeneration == tools.binding().stageGeneration
            && restored.stageId == tools.binding().stageId
            && restored.toolCount == audioLiveToolState_.toolCount
            && restored.variableCount == audioLiveToolState_.variableCount)
        {
            audioLiveToolState_ = restored;
            static_cast<void>(liveToolStatePublication_.publishAll(
                audioLiveToolState_));
        }
        else
        {
            liveToolStaleAuthorityCount_.fetch_add(
                1, std::memory_order_relaxed);
        }
        audioLiveToolRestoreRequestGeneration_ = restoreGeneration;
        liveToolAppliedRestoreGeneration_.store(
            restoreGeneration, std::memory_order_release);
    }
    RealtimeLiveToolAuthority authority{
        .documentAuthorityGeneration =
            tools.binding().editorDocumentAuthority
                ? editor_.documentAuthorityGeneration()
                : tools.binding().documentAuthorityGeneration,
        .stageGeneration = tools.binding().stageGeneration,
        .stageId = tools.binding().stageId,
    };
    for (std::size_t index = 0;
         index < liveToolSourceAuthorities_.size();
         ++index)
    {
        if (!liveToolSourceAuthorities_[index].read(
                authority.sourceGenerations[index],
                authority.routeGenerations[index]))
        {
            authority.sourceGenerations[index] =
                std::numeric_limits<std::uint64_t>::max();
            authority.routeGenerations[index] =
                std::numeric_limits<std::uint64_t>::max();
        }
    }
    const auto transport = graphHost_.preparedTransportSnapshot();
    const auto sampleRateHz = static_cast<std::uint32_t>(
        std::llround(renderTarget.sampleRate()));
    const auto tickAt = [&](std::uint64_t frame) {
        return talas::project::sampleFrameToMusicalTick(
            liveSet.musicalTime(), frame, sampleRateHz);
    };
    const auto currentTick = transport.playing
        ? tickAt(transport.playheadFrame)
        : std::optional<std::uint64_t>{};
    if (currentTick.has_value()
        && (!audioLiveToolHasPeriodicTick_
            || *currentTick != audioLiveToolLastPeriodicTick_
            || transport.discontinuitySequence
                != audioLiveToolPeriodicDiscontinuity_))
    {
        // The ticks reached since the previous callback. A relocation
        // restarts at the current tick; a Timeline loop wrap also covers
        // the rest of the previous pass.
        struct TickRange
        {
            std::uint64_t first = 0;
            std::uint64_t last = 0;
        };
        std::array<TickRange, 2> ranges{};
        std::size_t rangeCount = 0;
        const auto continuous = audioLiveToolHasPeriodicTick_
            && transport.discontinuitySequence
                == audioLiveToolPeriodicDiscontinuity_;
        if (continuous && *currentTick > audioLiveToolLastPeriodicTick_)
        {
            ranges[rangeCount++] = {
                audioLiveToolLastPeriodicTick_ + 1U, *currentTick};
        }
        else if (continuous && transport.loopEnabled
                 && transport.loopEndFrame > transport.loopStartFrame)
        {
            const auto passEnd = tickAt(transport.loopEndFrame - 1U);
            if (passEnd.has_value()
                && *passEnd > audioLiveToolLastPeriodicTick_)
            {
                ranges[rangeCount++] = {
                    audioLiveToolLastPeriodicTick_ + 1U, *passEnd};
            }
            const auto passStart = tickAt(transport.loopStartFrame);
            ranges[rangeCount++] = {
                std::min(passStart.value_or(*currentTick), *currentTick),
                *currentTick};
        }
        else
        {
            ranges[rangeCount++] = {*currentTick, *currentTick};
        }

        // A tool fires once per tick that is a multiple of one of its
        // periods. Enumerate those multiples, keeping the earliest ones the
        // bounded trigger queue can hold and counting the rest.
        struct PeriodicTrigger
        {
            std::uint64_t tick = 0;
            std::uint16_t toolIndex = 0;

            [[nodiscard]] bool operator<(const PeriodicTrigger& other) const noexcept
            {
                return tick != other.tick ? tick < other.tick
                                          : toolIndex < other.toolIndex;
            }
        };
        constexpr auto capacity =
            talas::project::maximumRealtimeLiveToolTriggerQueue;
        std::array<PeriodicTrigger, capacity> triggers{};
        std::size_t triggerCount = 0;
        std::uint64_t droppedTriggers = 0;
        const auto keep = [&](PeriodicTrigger candidate) noexcept {
            const auto end = triggers.begin()
                + static_cast<std::ptrdiff_t>(triggerCount);
            const auto position = std::lower_bound(
                triggers.begin(), end, candidate);
            if (position != end && position->tick == candidate.tick
                && position->toolIndex == candidate.toolIndex)
            {
                return;
            }
            if (triggerCount == capacity)
            {
                ++droppedTriggers;
                if (position == end)
                    return;
                --triggerCount;
            }
            std::move_backward(
                position,
                triggers.begin() + static_cast<std::ptrdiff_t>(triggerCount),
                triggers.begin() + static_cast<std::ptrdiff_t>(triggerCount) + 1);
            *position = candidate;
            ++triggerCount;
        };
        for (std::size_t rangeIndex = 0; rangeIndex < rangeCount; ++rangeIndex)
        {
            const auto range = ranges[rangeIndex];
            for (std::size_t toolIndex = 0;
                 toolIndex < tools.tools().size();
                 ++toolIndex)
            {
                for (const auto& transition : tools.tools()[toolIndex].transitions)
                {
                    const auto period = transition.trigger.periodTicks;
                    if (transition.trigger.kind
                            != talas::project::ProjectLiveToolTriggerKind::PeriodicTicks
                        || period == 0)
                    {
                        continue;
                    }
                    const auto remainder = range.first % period;
                    if (remainder != 0
                        && period - remainder
                            > std::numeric_limits<std::uint64_t>::max() - range.first)
                    {
                        continue;
                    }
                    auto multiple = remainder == 0
                        ? range.first
                        : range.first + (period - remainder);
                    auto exhausted = multiple > range.last;
                    for (std::size_t generated = 0;
                         !exhausted && generated < capacity;
                         ++generated)
                    {
                        keep({multiple, static_cast<std::uint16_t>(toolIndex)});
                        if (range.last - multiple < period)
                            exhausted = true;
                        else
                            multiple += period;
                    }
                    // Later multiples of this period can never displace the
                    // earlier ones already kept.
                    if (!exhausted)
                        droppedTriggers += (range.last - multiple) / period + 1U;
                }
            }
        }
        if (droppedTriggers != 0)
        {
            liveToolQueueOverflowCount_.fetch_add(
                droppedTriggers, std::memory_order_relaxed);
        }
        for (std::size_t index = 0; index < triggerCount; ++index)
        {
            const auto& trigger = triggers[index];
            auto generation = liveToolRequestGeneration_.load(
                std::memory_order_relaxed);
            auto published = false;
            for (std::size_t attempt = 0;
                 attempt < RealtimeLiveToolIngress::maximumProducerAttempts;
                 ++attempt)
            {
                if (generation
                    == std::numeric_limits<std::uint64_t>::max())
                {
                    break;
                }
                if (liveToolRequestGeneration_.compare_exchange_strong(
                        generation,
                        generation + 1U,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed))
                {
                    ++generation;
                    published = true;
                    break;
                }
            }
            RealtimeLiveToolTriggerRequest periodic;
            periodic.triggerKind =
                talas::project::ProjectLiveToolTriggerKind::PeriodicTicks;
            periodic.sourceKind =
                RealtimeLiveToolSourceKind::Periodic;
            periodic.preparedToolIndex =
                trigger.toolIndex;
            periodic.musicalTick = trigger.tick;
            periodic.documentAuthorityGeneration =
                tools.binding().documentAuthorityGeneration;
            periodic.stageGeneration =
                tools.binding().stageGeneration;
            periodic.stageId = tools.binding().stageId;
            periodic.sourceGeneration =
                tools.binding().stageGeneration;
            periodic.routeGeneration =
                tools.binding().stageGeneration;
            periodic.requestGeneration = generation;
            if (!published || !liveToolIngress_.tryPush(periodic))
            {
                liveToolQueueOverflowCount_.fetch_add(
                    1, std::memory_order_relaxed);
            }
            else
            {
                liveToolAcceptedTriggerCount_.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        audioLiveToolLastPeriodicTick_ = *currentTick;
        audioLiveToolPeriodicDiscontinuity_ = transport.discontinuitySequence;
        audioLiveToolHasPeriodicTick_ = true;
    }

    struct CommandContext
    {
        ProjectRuntimeBridge* bridge = nullptr;
        RealtimeAudioEngine* renderTarget = nullptr;
        const PreparedLiveSet* liveSet = nullptr;
        const PreparedLiveTools* tools = nullptr;
    } commandContext{this, &renderTarget, &liveSet, &tools};
    const PreparedLiveToolCommandSink sink{
        .context = &commandContext,
        .apply = [](void* opaque,
                    std::span<const PreparedLiveToolCommand> commands) noexcept {
            auto& context = *static_cast<CommandContext*>(opaque);
            const auto recordAppliedCommands = [&]() noexcept {
                for (const auto& command : commands)
                {
                    switch (command.kind)
                    {
                    case talas::project::ProjectLiveToolCommandKind::ActivateCell:
                        context.bridge->liveToolActivateCellCommandCount_
                            .fetch_add(1, std::memory_order_relaxed);
                        break;
                    case talas::project::ProjectLiveToolCommandKind::Stop:
                        context.bridge->liveToolStopCommandCount_.fetch_add(
                            1, std::memory_order_relaxed);
                        break;
                    case talas::project::ProjectLiveToolCommandKind::SetProjectVariable:
                        context.bridge->liveToolSetProjectVariableCommandCount_
                            .fetch_add(1, std::memory_order_relaxed);
                        break;
                    case talas::project::ProjectLiveToolCommandKind::SetWorkspaceVariable:
                        context.bridge->liveToolSetWorkspaceVariableCommandCount_
                            .fetch_add(1, std::memory_order_relaxed);
                        break;
                    case talas::project::ProjectLiveToolCommandKind::SetEventValue:
                        context.bridge->liveToolSetEventValueCommandCount_
                            .fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            };
            std::array<PreparedTypedEvent,
                       talas::project::maximumProjectLiveToolCommandsPerTransition>
                events{};
            std::array<RealtimeTypedEventDispatchResult,
                       talas::project::maximumProjectLiveToolCommandsPerTransition>
                results{};
            std::size_t eventCount = 0;
            const PreparedLiveToolCommand* programCommand = nullptr;
            for (const auto& command : commands)
            {
                if (command.kind
                        == talas::project::ProjectLiveToolCommandKind::ActivateCell
                    || command.kind
                        == talas::project::ProjectLiveToolCommandKind::Stop)
                {
                    if (programCommand != nullptr)
                        return false;
                    programCommand = std::addressof(command);
                    continue;
                }
                if (!command.typedTarget.has_value()
                    || eventCount >= events.size())
                {
                    return false;
                }
                auto& event = events[eventCount];
                event.target = *command.typedTarget;
                event.frameOffset = 0;
                event.sequence = eventCount + 1U;
                if (command.kind
                        == talas::project::ProjectLiveToolCommandKind::SetEventValue)
                {
                    event.kind = TypedEventKind::EventValueAssignment;
                    event.payload.eventValue = {
                        command.value,
                        command.eventDomain,
                    };
                }
                else
                {
                    event.kind = TypedEventKind::VariableAssignment;
                    event.payload.variable = {
                        command.value,
                        talas::expr::ValueDomain::Scalar,
                    };
                }
                ++eventCount;
            }
            if (programCommand != nullptr
                && programCommand->kind
                    == talas::project::ProjectLiveToolCommandKind::ActivateCell
                && eventCount != 0)
            {
                return false;
            }
            if (eventCount != 0)
            {
                const auto eventSpan = std::span<const PreparedTypedEvent>(
                    events.data(), eventCount);
                const auto resultSpan =
                    std::span<RealtimeTypedEventDispatchResult>(
                        results.data(), eventCount);
                if (!context.renderTarget->schedulePreparedLiveToolEvents(
                        eventSpan, resultSpan)
                    || !std::ranges::all_of(
                        resultSpan,
                        [](RealtimeTypedEventDispatchResult result) {
                            return result
                                == RealtimeTypedEventDispatchResult::Handled;
                        }))
                {
                    return false;
                }
            }
            if (programCommand == nullptr)
            {
                recordAppliedCommands();
                return true;
            }
            if (programCommand->kind
                == talas::project::ProjectLiveToolCommandKind::Stop)
            {
                context.bridge->graphHost_.deactivatePreparedLiveSetProgram();
                context.bridge->graphHost_.setPlaying(false);
                context.bridge->audioLiveSetScoreEndFrame_ = 0;
                context.bridge->audioLiveSetScoreStageId_ = 0;
                context.bridge->audioLiveSetScoreDiscontinuitySequence_ = 0;
                context.bridge->liveSetScoreRegionActive_.store(
                    false, std::memory_order_release);
                context.bridge->liveSetActiveState_.clear();
                recordAppliedCommands();
                return true;
            }
            if (programCommand->preparedCellIndex
                >= context.liveSet->cells().size())
            {
                return false;
            }
            const auto effect = preparedLiveSetEffect(
                *context.liveSet, programCommand->preparedCellIndex);
            if (!effect.has_value())
                return false;
            auto requestGeneration =
                context.bridge->liveSetRequestGeneration_.load(
                    std::memory_order_relaxed);
            auto published = false;
            for (std::size_t attempt = 0;
                 attempt < RealtimeLiveSetIngress::maximumProducerAttempts;
                 ++attempt)
            {
                if (requestGeneration
                    == std::numeric_limits<std::uint64_t>::max())
                {
                    return false;
                }
                if (context.bridge->liveSetRequestGeneration_
                        .compare_exchange_strong(
                            requestGeneration,
                            requestGeneration + 1U,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed))
                {
                    ++requestGeneration;
                    published = true;
                    break;
                }
            }
            if (!published)
                return false;
            const auto targetKind =
                effect->kind == PreparedLiveSetEffectKind::ActivateSound
                ? RealtimeLiveSetTargetKind::Sound
                : (effect->kind
                        == PreparedLiveSetEffectKind::ActivateOverlaySnapshot
                    || effect->kind
                        == PreparedLiveSetEffectKind::ApplyDirectSnapshot)
                ? RealtimeLiveSetTargetKind::Snapshot
                : effect->kind
                        == PreparedLiveSetEffectKind::SeekScoreRegion
                ? RealtimeLiveSetTargetKind::ScoreRegion
                : effect->kind
                        == PreparedLiveSetEffectKind::SetTrackMute
                ? RealtimeLiveSetTargetKind::TrackMute
                : RealtimeLiveSetTargetKind::Stop;
            const auto applied = context.bridge->applyRealtimeLiveSetRequest(
                *context.liveSet,
                {
                    .targetKind = targetKind,
                    .sourceKind = RealtimeLiveSetSourceKind::Tool,
                    .preparedCellIndex = programCommand->preparedCellIndex,
                    .resolvedFrame = context.bridge->graphHost_
                        .transportFramePosition(),
                    .documentAuthorityGeneration =
                        context.tools->binding().documentAuthorityGeneration,
                    .stageGeneration =
                        context.tools->binding().stageGeneration,
                    .stageId = context.tools->binding().stageId,
                    .requestGeneration = requestGeneration,
                });
            if (applied)
                recordAppliedCommands();
            return applied;
        },
    };
    const PreparedLiveToolEvaluationInputs inputs{
        .context = &renderTarget,
        .resolveEvent = [](void* opaque,
                           std::string_view name,
                           talas::expr::ExpressionValue& value) noexcept {
            return static_cast<RealtimeAudioEngine*>(opaque)
                ->resolvePreparedLiveToolEvent(name, value);
        },
        .sampleRate = renderTarget.sampleRate(),
        .tempoBpm = 120.0,
    };
    RealtimeLiveToolTriggerRequest request;
    const auto budget = liveToolIngress_.capacity();
    for (std::size_t count = 0;
         count < budget && liveToolIngress_.tryPop(request);
         ++count)
    {
        if (!realtimeLiveToolTriggerMatchesAuthority(request, authority)
            || request.preparedToolIndex >= tools.tools().size())
        {
            liveToolStaleAuthorityCount_.fetch_add(
                1, std::memory_order_relaxed);
            continue;
        }
        PreparedLiveToolTriggerEvent trigger;
        trigger.kind = request.triggerKind;
        trigger.eventDomain = request.domain;
        trigger.variableNumber = request.variableNumber;
        trigger.variableBoolean = request.variableBoolean;
        trigger.hasVariableValue = request.hasVariableValue;
        trigger.midiChannel = request.midiChannel;
        trigger.midiProgram = request.midiProgram;
        trigger.musicalTick = request.musicalTick;
        trigger.sequence = request.requestGeneration;
        trigger.documentAuthorityGeneration =
            request.documentAuthorityGeneration;
        trigger.stageGeneration = request.stageGeneration;
        trigger.stageId = request.stageId;
        switch (request.triggerKind)
        {
        case talas::project::ProjectLiveToolTriggerKind::EventValue:
            trigger.eventName = request.identifierView();
            break;
        case talas::project::ProjectLiveToolTriggerKind::VariableChange:
            trigger.variableId = request.identifierView();
            break;
        case talas::project::ProjectLiveToolTriggerKind::MidiProgram:
            trigger.midiSourceIdentifier = request.identifierView();
            break;
        case talas::project::ProjectLiveToolTriggerKind::Osc:
            trigger.oscAddress = request.identifierView();
            break;
        case talas::project::ProjectLiveToolTriggerKind::Manual:
        case talas::project::ProjectLiveToolTriggerKind::PeriodicTicks:
            break;
        }
        const auto result = stepPreparedLiveTool(
            tools,
            request.preparedToolIndex,
            trigger,
            inputs,
            sink,
            audioLiveToolState_);
        static_cast<void>(liveToolStatePublication_.publishTool(
            audioLiveToolState_, request.preparedToolIndex));
        if (result.ok())
        {
            liveToolAppliedTransitionCount_.fetch_add(
                1, std::memory_order_relaxed);
        }
        else
        {
            liveToolRejectedTriggerCount_.fetch_add(
                1, std::memory_order_relaxed);
            switch (result.outcome)
            {
            case PreparedLiveToolStepOutcome::NoMatchingTransition:
                liveToolNoMatchingTransitionCount_.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case PreparedLiveToolStepOutcome::StaleAuthority:
                liveToolStaleTriggerAuthorityCount_.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case PreparedLiveToolStepOutcome::InvalidTrigger:
                liveToolInvalidTriggerCount_.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case PreparedLiveToolStepOutcome::GuardRejected:
                liveToolGuardRejectedCount_.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case PreparedLiveToolStepOutcome::CommandRejected:
                liveToolCommandRejectedCount_.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            case PreparedLiveToolStepOutcome::Applied:
                break;
            }
        }
    }
}

void ProjectRuntimeBridge::setDiskRecorderTapSink(void* context, DiskRecorderTapSink sink) noexcept
{
    diskRecorderTapSink_.store(nullptr, std::memory_order_release);
    diskRecorderTapSinkContext_.store(context, std::memory_order_release);
    diskRecorderTapSink_.store(sink, std::memory_order_release);
}

void ProjectRuntimeBridge::publishDiskRecorderTaps(const RealtimeAudioEngine& engine) noexcept
{
    const auto sink = diskRecorderTapSink_.load(std::memory_order_acquire);
    const auto frames = engine.diskRecorderTapFrames();
    if (sink == nullptr || frames == 0)
        return;
    auto* context = diskRecorderTapSinkContext_.load(std::memory_order_acquire);
    for (std::size_t tap = 0; tap < engine.diskRecorderTapCount(); ++tap)
    {
        const auto channels = engine.diskRecorderTapChannels(tap);
        sink(context, tap, channels.data(), channels.size(), frames);
    }
}

bool ProjectRuntimeBridge::renderWithOutputMapping(
    RealtimeAudioInputBlock input,
    double callbackTimeSeconds,
    float* const* outputChannels,
    std::size_t channelCount,
    std::size_t frameCount,
    bool orderedOutputs) noexcept
{
    graphHost_.prepareNextRenderBoundary();
    adoptInternalTransportRelocation();
    drainMidiClockTransport(callbackTimeSeconds, frameCount);
    drainMtcTransport(callbackTimeSeconds, frameCount);
    struct PreparationContext
    {
        ProjectRuntimeBridge* bridge;
        std::size_t frameCount;
        double callbackTimeSeconds;
        // The engine rendering the part, whose DiskRecorders within the
        // Sound record it.
        RealtimeAudioEngine* target = nullptr;
    };
    const auto prepare = [](void* opaque,
                            RealtimeAudioEngine& renderTarget,
                            RealtimeAudioEngine* fadingOutTarget) noexcept {
            auto& preparation = *static_cast<PreparationContext*>(opaque);
            preparation.target = &renderTarget;
            if (preparation.frameCount > 0)
            {
                preparation.bridge->drainRealtimeControls(
                    renderTarget,
                    preparation.frameCount,
                    preparation.callbackTimeSeconds);
                if (fadingOutTarget != nullptr)
                {
                    preparation.bridge->mirrorDrainedRealtimeControls(
                        *fadingOutTarget);
                }
                const auto* liveSet = renderTarget.preparedLiveSet();
                const auto* liveTools = renderTarget.preparedLiveTools();
                if (liveSet != nullptr && liveTools != nullptr)
                {
                    preparation.bridge->drainRealtimeLiveToolTriggers(
                        renderTarget, *liveSet, *liveTools);
                }
            }
        };
    if (frameCount == 0)
    {
        PreparationContext context{this, 0, callbackTimeSeconds};
        return orderedOutputs
                   ? graphHost_.renderPreparedOrderedMirrored(
                         input, outputChannels, channelCount, frameCount, &context, prepare)
                   : graphHost_.renderPreparedMirrored(
                         input, outputChannels, channelCount, frameCount, &context, prepare);
    }

    constexpr std::size_t maximumSegmentChannels = 8;
    std::array<const float*, maximumSegmentChannels> segmentInputs{};
    std::array<float*, maximumSegmentChannels> segmentOutputs{};
    auto controlsDrained = false;
    auto renderedAll = true;
    std::size_t deviceFrameOffset = 0;
    auto renderSegment = [&](std::size_t segmentFrames)
    {
        if (segmentFrames == 0)
            return true;
        if (channelCount > segmentOutputs.size() || input.channelCount > segmentInputs.size())
        {
            return false;
        }
        for (std::size_t channel = 0; channel < input.channelCount; ++channel)
        {
            segmentInputs[channel] = input.channels != nullptr && input.channels[channel] != nullptr
                                         ? input.channels[channel] + deviceFrameOffset
                                         : nullptr;
        }
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            segmentOutputs[channel] =
                outputChannels != nullptr && outputChannels[channel] != nullptr
                    ? outputChannels[channel] + deviceFrameOffset
                    : nullptr;
        }
        PreparationContext context{
            this,
            controlsDrained ? 0 : frameCount,
            callbackTimeSeconds,
        };
        const RealtimeAudioInputBlock segmentInput{
            input.channelCount == 0 ? nullptr : segmentInputs.data(),
            input.channelCount,
        };
        const auto rendered = orderedOutputs
                                  ? graphHost_.renderPreparedOrderedMirrored(segmentInput,
                                                                             segmentOutputs.data(),
                                                                             channelCount,
                                                                             segmentFrames,
                                                                             &context,
                                                                             prepare)
                                  : graphHost_.renderPreparedMirrored(segmentInput,
                                                                      segmentOutputs.data(),
                                                                      channelCount,
                                                                      segmentFrames,
                                                                      &context,
                                                                      prepare);
        if (rendered && context.target != nullptr)
            publishDiskRecorderTaps(*context.target);
        publishPendingLiveSetActivationOutcome();
        controlsDrained = true;
        deviceFrameOffset += segmentFrames;
        return rendered;
    };
    const auto clearActiveScoreRegionIdentity = [this]() noexcept
    {
        audioLiveSetScoreEndFrame_ = 0;
        audioLiveSetScoreStageId_ = 0;
        audioLiveSetScoreDiscontinuitySequence_ = 0;
        liveSetScoreRegionActive_.store(false, std::memory_order_release);
        liveSetActiveState_.clear();
    };

    auto transport = graphHost_.prepareNextRenderTransportBoundary();
    for (std::size_t iteration = 0;
         deviceFrameOffset < frameCount && iteration < liveSetScheduler_.capacity() + 4U;
         ++iteration)
    {
        const auto* prepared = graphHost_.preparedLiveSet();
        const auto activeStageId = graphHost_.activeStageId();
        if (activeStageId != audioLiveSetStageId_)
        {
            audioLiveSetStageId_ = activeStageId;
            audioLiveSetScoreEndFrame_ = 0;
            audioLiveSetScoreStageId_ = 0;
            audioLiveSetScoreDiscontinuitySequence_ = 0;
            publishLiveSetTerminal(
                audioLiveSetPendingRequestGeneration_,
                RealtimeLiveSetTerminalOutcome::StaleAuthority);
            audioLiveSetPendingRequestGeneration_ = 0;
            audioLiveSetPendingCellIndexPlusOne_ = 0;
            liveSetActiveState_.clear();
            liveSetScoreRegionActive_.store(false, std::memory_order_release);
        }
        if (prepared == nullptr || prepared->binding().stageId != graphHost_.activeStageId())
        {
            std::uint64_t latestDiscarded = 0;
            const auto scheduledCount = liveSetScheduler_.size();
            liveSetScheduler_.clear(&latestDiscarded);
            liveSetStaleAuthorityCount_.fetch_add(
                scheduledCount, std::memory_order_relaxed);
            publishLiveSetTerminal(
                latestDiscarded,
                RealtimeLiveSetTerminalOutcome::StaleAuthority);
            RealtimeLiveSetRequest stale;
            const auto popBudget = liveSetIngress_.capacity();
            for (std::size_t popped = 0; popped < popBudget && liveSetIngress_.tryPop(stale);
                 ++popped)
            {
                liveSetStaleAuthorityCount_.fetch_add(1, std::memory_order_relaxed);
                publishLiveSetTerminal(
                    stale.requestGeneration,
                    RealtimeLiveSetTerminalOutcome::StaleAuthority);
            }
        }
        else
        {
            drainRealtimeLiveSetRequests(*prepared, transport);
        }
        std::uint64_t latestLateRequest = 0;
        const auto lateCount = liveSetScheduler_.discardBefore(
            transport.absoluteFrame, &latestLateRequest);
        liveSetStaleAuthorityCount_.fetch_add(
            lateCount, std::memory_order_relaxed);
        publishLiveSetTerminal(
            latestLateRequest,
            RealtimeLiveSetTerminalOutcome::StaleAuthority);

        const auto remaining = frameCount - deviceFrameOffset;
        const auto transportEnd =
            transport.playing ? saturatingTransportFrameAdd(transport.absoluteFrame, remaining)
                              : saturatingTransportFrameAdd(transport.absoluteFrame, 1);
        const auto scoreEndsNow = liveSetScoreRegionActive_.load(std::memory_order_acquire) &&
                                  audioLiveSetScoreStageId_ == graphHost_.activeStageId() &&
                                  transport.absoluteFrame >= audioLiveSetScoreEndFrame_;
        const auto scoreLostTransportAuthority =
            liveSetScoreRegionActive_.load(std::memory_order_acquire) &&
            audioLiveSetScoreDiscontinuitySequence_ != 0 &&
            transport.discontinuitySequence != audioLiveSetScoreDiscontinuitySequence_;
        if (scoreLostTransportAuthority)
            clearActiveScoreRegionIdentity();
        if (scoreEndsNow && !scoreLostTransportAuthority)
        {
            graphHost_.setPlaying(false);
            clearActiveScoreRegionIdentity();
            transport.playing = false;
        }

        auto requestFrame = liveSetScheduler_.nextFrame();
        const auto requestDueNow =
            requestFrame.has_value() && *requestFrame == transport.absoluteFrame;
        if (requestDueNow && prepared != nullptr)
        {
            const auto request = liveSetScheduler_.takeNextInRange(
                transport.absoluteFrame, saturatingTransportFrameAdd(transport.absoluteFrame, 1));
            if (!request.has_value() || !applyRealtimeLiveSetRequest(*prepared, *request))
            {
                if (request.has_value())
                {
                    publishLiveSetTerminal(
                        request->requestGeneration,
                        RealtimeLiveSetTerminalOutcome::Failed);
                }
                liveSetFailedActivationCount_.fetch_add(1, std::memory_order_relaxed);
            }
            transport = graphHost_.prepareContinuationRenderTransportBoundary();
            if (liveSetScoreRegionActive_.load(std::memory_order_acquire) &&
                audioLiveSetScoreDiscontinuitySequence_ == 0)
            {
                audioLiveSetScoreDiscontinuitySequence_ = transport.discontinuitySequence;
            }
            continue;
        }

        auto boundary = transportEnd;
        if (requestFrame.has_value() && *requestFrame > transport.absoluteFrame &&
            *requestFrame < boundary)
        {
            boundary = *requestFrame;
        }
        // A staged replacement becomes audible exactly at its prepared frame;
        // the continuation boundary there adopts it.
        if (const auto adoptionFrame = graphHost_.pendingAdoptionFrame();
            adoptionFrame.has_value() && *adoptionFrame > transport.absoluteFrame
            && *adoptionFrame < boundary)
        {
            boundary = *adoptionFrame;
        }
        if (liveSetScoreRegionActive_.load(std::memory_order_acquire) &&
            audioLiveSetScoreStageId_ == graphHost_.activeStageId() &&
            audioLiveSetScoreEndFrame_ > transport.absoluteFrame &&
            audioLiveSetScoreEndFrame_ < boundary)
        {
            boundary = audioLiveSetScoreEndFrame_;
        }
        auto segmentFrames = remaining;
        if (transport.playing && boundary < transportEnd)
        {
            segmentFrames = static_cast<std::size_t>(boundary - transport.absoluteFrame);
        }
        if (segmentFrames == 0)
        {
            liveSetFailedActivationCount_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        const auto scoreEndsAfterSegment =
            transport.playing &&
            liveSetScoreRegionActive_.load(std::memory_order_acquire) &&
            audioLiveSetScoreStageId_ == graphHost_.activeStageId() &&
            saturatingTransportFrameAdd(transport.absoluteFrame, segmentFrames) >=
                audioLiveSetScoreEndFrame_;
        const auto segmentRendered = renderSegment(segmentFrames);
        renderedAll = segmentRendered && renderedAll;
        if (segmentRendered && scoreEndsAfterSegment)
        {
            graphHost_.setPlaying(false);
            clearActiveScoreRegionIdentity();
        }
        if (deviceFrameOffset < frameCount)
        {
            transport = graphHost_.prepareContinuationRenderTransportBoundary();
        }
    }
    if (deviceFrameOffset < frameCount)
    {
        liveSetFailedActivationCount_.fetch_add(1, std::memory_order_relaxed);
        renderedAll = renderSegment(frameCount - deviceFrameOffset) && renderedAll;
    }
    return renderedAll;
}

void ProjectRuntimeBridge::clearAfterAudioStopped() noexcept
{
    acceptedMidiControllerStageGeneration_.store(
        0, std::memory_order_release);
    delete pendingTransportRelocation_.exchange(nullptr, std::memory_order_acq_rel);
    (void) collectRetiredTransportRelocations();
    const auto controllerRecordingEndFrame =
        graphHost_.transportSnapshot().absoluteFrame;
    graphHost_.clearAfterAudioStopped();
    latestPreparedGraphSummary_.reset();
    latestPreparedScoreSchedule_.reset();
    std::atomic_store_explicit(
        &latestPreparedLiveSet_,
        std::shared_ptr<const PreparedLiveSet>{},
        std::memory_order_release);
    std::atomic_store_explicit(
        &latestPreparedLiveTools_,
        std::shared_ptr<const PreparedLiveTools>{},
        std::memory_order_release);
    std::uint64_t latestDiscardedLiveSetRequest = 0;
    liveSetScheduler_.clear(&latestDiscardedLiveSetRequest);
    publishLiveSetTerminal(
        latestDiscardedLiveSetRequest,
        RealtimeLiveSetTerminalOutcome::StaleAuthority);
    RealtimeLiveSetRequest discardedLiveSetRequest;
    while (liveSetIngress_.tryPop(discardedLiveSetRequest))
    {
        liveSetStaleAuthorityCount_.fetch_add(1, std::memory_order_relaxed);
        publishLiveSetTerminal(
            discardedLiveSetRequest.requestGeneration,
            RealtimeLiveSetTerminalOutcome::StaleAuthority);
    }
    for (auto& authority : liveSetSourceAuthorities_)
        authority.clear();
    RealtimeLiveToolTriggerRequest discardedLiveToolTrigger;
    while (liveToolIngress_.tryPop(discardedLiveToolTrigger))
    {
        liveToolStaleAuthorityCount_.fetch_add(
            1, std::memory_order_relaxed);
    }
    for (auto& authority : liveToolSourceAuthorities_)
        authority.clear();
    audioLiveToolState_ = {};
    liveToolStatePublication_.clear();
    liveToolRestorePublication_.clear();
    audioLiveToolStageId_ = 0;
    audioLiveToolLastPeriodicTick_ = 0;
    audioLiveToolHasPeriodicTick_ = false;
    audioLiveToolRestoreRequestGeneration_ =
        liveToolRestoreRequestGeneration_.load(
            std::memory_order_acquire);
    liveToolAppliedRestoreGeneration_.store(
        audioLiveToolRestoreRequestGeneration_,
        std::memory_order_release);
    liveSetActiveState_.clear();
    liveSetScoreRegionActive_.store(false, std::memory_order_release);
    audioLiveSetScoreEndFrame_ = 0;
    audioLiveSetScoreStageId_ = 0;
    audioLiveSetScoreDiscontinuitySequence_ = 0;
    audioLiveSetStageId_ = 0;
    audioLiveSetPendingRequestGeneration_ = 0;
    audioLiveSetPendingCellIndexPlusOne_ = 0;
    latestSnapshotPreparedTargets_.clear();
    snapshotDirectUpdateScratch_.clear();
    latestSnapshotStageId_ = 0;
    latestSnapshotStageGeneration_ = 0;
    latestSnapshotSampleRate_ = 0.0;
    projectSnapshotRenderSettings_.reset();
    latestSnapshotExecutionTopology_.reset();
    latestSnapshotAuthoringTopology_.reset();
    coalescedSnapshotOverlay_.reset();
    observedSnapshotTransitionCompletionToken_ = 0;
    snapshotPerformanceState_ = {};
    projectSnapshotBaselineValues_.clear();
    stagedSnapshotAuthoringBytes_.clear();
    const auto configuredControllerRecording =
        controllerRecordingConfiguredTakeGeneration_.load(
            std::memory_order_acquire);
    if (audioControllerRecordingActive_
        || controllerRecordingRequestedActive_.load(
            std::memory_order_acquire)
        || (configuredControllerRecording != 0
            && controllerRecordingCompletedTakeGeneration_.load(
                   std::memory_order_acquire)
                != configuredControllerRecording))
    {
        completeControllerRecordingFromAudio(
            talas::project::ControllerRecordingTerminal::DeviceChanged,
            controllerRecordingEndFrame);
    }
}

void ProjectRuntimeBridge::completeControllerRecordingFromAudio(
    talas::project::ControllerRecordingTerminal terminal,
    std::uint64_t endFrame) noexcept
{
    const auto configuredTakeGeneration =
        controllerRecordingConfiguredTakeGeneration_.load(
            std::memory_order_relaxed);
    const auto adopted = configuredTakeGeneration != 0
        && audioControllerRecordingTakeGeneration_ == configuredTakeGeneration;
    const auto takeGeneration = adopted
        ? audioControllerRecordingTakeGeneration_
        : configuredTakeGeneration;
    audioControllerRecordingActive_ = false;
    if (takeGeneration == 0)
        return;
    auto terminalState = controllerRecordingTerminalState_.load(
        std::memory_order_acquire);
    auto completedState = std::uint64_t{};
    auto completionPublished = false;
    for (std::size_t attempt = 0;
         attempt < maximumControllerRecordingTerminalCasAttempts;
         ++attempt)
    {
        if (controllerRecordingTerminalGeneration(terminalState)
                != takeGeneration
            || (terminalState & controllerRecordingTerminalCompletedBit) != 0)
        {
            return;
        }
        completedState = terminalState
            | controllerRecordingTerminalCompletedBit
            | controllerRecordingTerminalBit(terminal);
        if (controllerRecordingTerminalState_.compare_exchange_strong(
                terminalState,
                completedState,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            completionPublished = true;
            break;
        }
    }
    if (!completionPublished)
        return;
    terminal = controllerRecordingTerminalFromState(completedState);
    if (terminal == talas::project::ControllerRecordingTerminal::Seeked)
    {
        endFrame = controllerRecordingSeekEndFrame_.load(std::memory_order_relaxed);
    }
    controllerRecordingCompletedEventCount_.store(
        adopted ? audioControllerRecordingEventCount_ : 0,
        std::memory_order_relaxed);
    controllerRecordingCompletedEndFrame_.store(
        endFrame, std::memory_order_relaxed);
    controllerRecordingCompletedTerminal_.store(
        terminal, std::memory_order_relaxed);
    controllerRecordingRequestedActive_.store(
        false, std::memory_order_relaxed);
    controllerRecordingCompletedTakeGeneration_.store(
        takeGeneration, std::memory_order_release);
}

void ProjectRuntimeBridge::adoptControllerRecordingRequest(
    RealtimeAudioEngine& renderTarget) noexcept
{
    const auto request = controllerRecordingRequestGeneration_.load(
        std::memory_order_acquire);
    if (request != audioControllerRecordingRequestGeneration_)
    {
        audioControllerRecordingRequestGeneration_ = request;
        if (!controllerRecordingRequestedActive_.load(
                std::memory_order_relaxed))
        {
            if (!audioControllerRecordingActive_)
            {
                completeControllerRecordingFromAudio(
                    controllerRecordingTerminalFromState(
                        controllerRecordingTerminalState_.load(std::memory_order_acquire)),
                    graphHost_.preparedTransportSnapshot().absoluteFrame);
                return;
            }
        }
        else
        {
            audioControllerRecordingTakeGeneration_ =
                controllerRecordingConfiguredTakeGeneration_.load(
                    std::memory_order_relaxed);
            audioControllerRecordingStageGeneration_ =
                controllerRecordingConfiguredStageGeneration_.load(
                    std::memory_order_relaxed);
            audioControllerRecordingPunchStartFrame_ =
                controllerRecordingConfiguredPunchStartFrame_.load(
                    std::memory_order_relaxed);
            audioControllerRecordingPunchEndFrame_ =
                controllerRecordingConfiguredPunchEndFrame_.load(
                    std::memory_order_relaxed);
            audioControllerRecordingEventCount_ = 0;
            audioControllerRecordingActive_ = true;
        }
    }
    if (audioControllerRecordingActive_
        && renderTarget.snapshotGeneration()
        != audioControllerRecordingStageGeneration_)
    {
        completeControllerRecordingFromAudio(talas::project::ControllerRecordingTerminal::StageChanged,
                                             graphHost_.preparedTransportSnapshot().absoluteFrame);
    }
}

void ProjectRuntimeBridge::captureControllerRecordingEvent(
    const RealtimeControlCommand& command,
    std::uint64_t absoluteFrame,
    std::uint64_t stageGeneration) noexcept
{
    if (!audioControllerRecordingActive_)
        return;
    if (command.recordingTakeGeneration
            != audioControllerRecordingTakeGeneration_)
        return;
    if (stageGeneration != audioControllerRecordingStageGeneration_
        || command.stageGeneration
            != audioControllerRecordingStageGeneration_)
    {
        completeControllerRecordingFromAudio(
            talas::project::ControllerRecordingTerminal::StageChanged,
            absoluteFrame);
        return;
    }
    if (absoluteFrame < audioControllerRecordingPunchStartFrame_)
        return;
    if (absoluteFrame > audioControllerRecordingPunchEndFrame_)
    {
        completeControllerRecordingFromAudio(
            talas::project::ControllerRecordingTerminal::PunchComplete,
            audioControllerRecordingPunchEndFrame_);
        return;
    }
    if (command.recordingSourceTimestampNanoseconds <= 0
        || command.recordingRouteGeneration == 0
        || (command.recordingOrigin
                != talas::project::ControllerRecordingOrigin::Ui
            && command.recordingSourceGeneration == 0)
        || std::ranges::find(
               controllerRecordingTargetEventIds_.begin(),
               controllerRecordingTargetEventIds_.begin()
                   + static_cast<std::ptrdiff_t>(
                       controllerRecordingTargetCount_),
               command.eventId)
            == controllerRecordingTargetEventIds_.begin()
                + static_cast<std::ptrdiff_t>(
                    controllerRecordingTargetCount_))
    {
        return;
    }
    if (audioControllerRecordingEventCount_
        >= talas::project::maximumControllerRecordingEvents)
    {
        completeControllerRecordingFromAudio(
            talas::project::ControllerRecordingTerminal::CapacityExceeded,
            absoluteFrame);
        return;
    }
    auto& record = controllerRecordingEvents_[
        audioControllerRecordingEventCount_];
    record = {
        static_cast<std::uint64_t>(audioControllerRecordingEventCount_ + 1),
        command.eventId,
        command.value,
        absoluteFrame,
        command.recordingSourceTimestampNanoseconds,
        stageGeneration,
        command.recordingRouteGeneration,
        command.recordingSourceGeneration,
        command.recordingOrigin,
    };
    ++audioControllerRecordingEventCount_;
}

void ProjectRuntimeBridge::drainRealtimeControls(
    RealtimeAudioEngine& renderTarget,
    std::size_t frameCount,
    double callbackTimeSeconds) noexcept
{
    const ProducerGate producerGate(sessionProducerGate_, ProducerGate::Mode::TryOnce);
    if (!producerGate)
    {
        // Commands stay in the ingress; the next callback drains them.
        controlDrainScratch_.clear();
        deferredRealtimeControlDrainCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto targetStageId = renderTarget.stageId();
    adoptControllerRecordingRequest(renderTarget);
    const auto callbackAbsoluteFrame = graphHost_.preparedTransportSnapshot().absoluteFrame;
    const auto callbackEndFrame = frameCount
            > std::numeric_limits<std::uint64_t>::max()
                - callbackAbsoluteFrame
        ? std::numeric_limits<std::uint64_t>::max()
        : callbackAbsoluteFrame + frameCount;
    const auto finishRequestedRecording = [&] {
        if (audioControllerRecordingActive_
            && !controllerRecordingRequestedActive_.load(
                std::memory_order_acquire))
        {
            completeControllerRecordingFromAudio(
                controllerRecordingTerminalFromState(
                    controllerRecordingTerminalState_.load(
                        std::memory_order_acquire)),
                callbackEndFrame);
        }
    };
    if (audioControllerRecordingActive_
        && callbackAbsoluteFrame > audioControllerRecordingPunchEndFrame_)
    {
        completeControllerRecordingFromAudio(
            talas::project::ControllerRecordingTerminal::PunchComplete,
            audioControllerRecordingPunchEndFrame_);
    }
    if (targetStageId != audioMidiStageId_)
    {
        audioMidiStageId_ = targetStageId;
        activeMidiChannel_ = 0;
        activeMidiNote_ = -1;
    }
    controlDrainScratch_.clear();
    const auto adoptGeneration = [&](std::uint64_t generation) {
        if (generation <= audioControlGeneration_)
            return;
        staleRealtimeControlCount_.fetch_add(
            controlDrainScratch_.size() + scheduledControlPending_.size(),
            std::memory_order_relaxed);
        addSaturatedBounded(
            scheduledRealtimeControlStaleCount_,
            scheduledControlPending_.size());
        audioControlGeneration_ = generation;
        lastRealtimeCallbackTimeSeconds_ = 0.0;
        controlDrainScratch_.clear();
        scheduledControlPending_.clear();
        scheduledRealtimeControlPendingCount_.store(
            0, std::memory_order_relaxed);
    };
    adoptGeneration(realtimeControlGeneration_.load(std::memory_order_acquire));
    scheduledControlSequenceScratch_.clear();

    const auto sampleRate = renderTarget.sampleRate();
    constexpr auto nanosecondsPerSecond = 1'000'000'000.0;
    const auto callbackDuration =
        static_cast<double>(frameCount) * nanosecondsPerSecond
        / sampleRate;
    const auto validScheduledClock =
        std::isfinite(callbackTimeSeconds) && callbackTimeSeconds > 0.0
        && callbackTimeSeconds < 9'000'000'000.0
        && std::isfinite(sampleRate) && sampleRate > 0.0
        && std::isfinite(callbackDuration)
        && callbackDuration >= 1.0
        && callbackDuration < 9'000'000'000'000'000'000.0;
    const auto callbackStartNanoseconds = validScheduledClock
        ? static_cast<std::int64_t>(std::llround(
            callbackTimeSeconds * nanosecondsPerSecond))
        : std::int64_t{0};
    const auto callbackDurationNanoseconds = validScheduledClock
        ? static_cast<std::int64_t>(std::max(
            1.0,
            std::round(callbackDuration)))
        : std::int64_t{0};
    const auto callbackEndNanoseconds = validScheduledClock
            && callbackStartNanoseconds
                <= std::numeric_limits<std::int64_t>::max()
                    - callbackDurationNanoseconds
        ? callbackStartNanoseconds + callbackDurationNanoseconds
        : std::int64_t{0};

    const auto appendScheduledIfDue = [&](RealtimeControlCommand scheduled) {
        if (callbackEndNanoseconds == 0
            || scheduled.targetTimeNanoseconds <= 0)
        {
            invalidRealtimeControlCount_.fetch_add(
                1, std::memory_order_relaxed);
            rejectedRealtimeControls_.fetch_add(
                1, std::memory_order_relaxed);
            return true;
        }
        if (scheduled.targetTimeNanoseconds >= callbackEndNanoseconds)
            return false;
        if (scheduled.targetTimeNanoseconds < callbackStartNanoseconds)
        {
            scheduled.frameOffset = 0;
            addSaturatedBounded(scheduledRealtimeControlLateCount_);
        }
        else
        {
            const auto deltaNanoseconds = static_cast<double>(
                scheduled.targetTimeNanoseconds - callbackStartNanoseconds);
            const auto offset = std::llround(
                deltaNanoseconds * sampleRate / nanosecondsPerSecond);
            scheduled.frameOffset = static_cast<std::size_t>(std::clamp(
                offset,
                std::int64_t{0},
                static_cast<std::int64_t>(frameCount - 1)));
        }
        controlDrainScratch_.push_back(scheduled);
        return true;
    };

    std::size_t retainedScheduled = 0;
    for (const auto& scheduled : scheduledControlPending_)
    {
        if (scheduled.generation != audioControlGeneration_)
        {
            staleRealtimeControlCount_.fetch_add(
                1, std::memory_order_relaxed);
            addSaturatedBounded(scheduledRealtimeControlStaleCount_);
            continue;
        }
        if (scheduled.stageGeneration != 0
            && scheduled.stageGeneration
                != renderTarget.snapshotGeneration())
        {
            staleRealtimeControlCount_.fetch_add(
                1, std::memory_order_relaxed);
            addSaturatedBounded(scheduledRealtimeControlStaleCount_);
            continue;
        }
        if (!appendScheduledIfDue(scheduled))
            scheduledControlPending_[retainedScheduled++] = scheduled;
    }
    scheduledControlPending_.resize(retainedScheduled);

    RealtimeControlCommand command;
    const auto popBudget = controlIngress_.capacity();
    for (std::size_t popped = 0; popped < popBudget && controlIngress_.tryPop(command); ++popped)
    {
        adoptGeneration(command.generation);
        if (command.generation != audioControlGeneration_)
        {
            staleRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
            if (command.timing
                == RealtimeControlTiming::AbsoluteSteadyTimestamp)
            {
                addSaturatedBounded(
                    scheduledRealtimeControlStaleCount_);
            }
        }
        else if (command.timing
                 != RealtimeControlTiming::AbsoluteSteadyTimestamp)
        {
            controlDrainScratch_.push_back(command);
        }
        else if (!appendScheduledIfDue(command))
        {
            if (scheduledControlPending_.size() >= controlIngress_.capacity())
            {
                addSaturatedBounded(
                    scheduledRealtimeControlPendingFullCount_);
                rejectedRealtimeControls_.fetch_add(
                    1, std::memory_order_relaxed);
            }
            else
            {
                const auto position = std::ranges::lower_bound(
                    scheduledControlPending_,
                    command,
                    [](const auto& left, const auto& right) {
                        if (left.targetTimeNanoseconds
                            != right.targetTimeNanoseconds)
                        {
                            return left.targetTimeNanoseconds
                                < right.targetTimeNanoseconds;
                        }
                        return left.sequence < right.sequence;
                    });
                scheduledControlPending_.insert(position, command);
            }
        }
    }

    adoptGeneration(realtimeControlGeneration_.load(std::memory_order_acquire));
    const auto pendingHighWatermark =
        scheduledRealtimeControlPendingHighWatermark_.load(
            std::memory_order_relaxed);
    if (scheduledControlPending_.size() > pendingHighWatermark)
    {
        scheduledRealtimeControlPendingHighWatermark_.store(
            scheduledControlPending_.size(), std::memory_order_relaxed);
    }
    scheduledRealtimeControlPendingCount_.store(
        scheduledControlPending_.size(), std::memory_order_release);
    const auto hasTiming = std::isfinite(callbackTimeSeconds) && callbackTimeSeconds > 0.0
        && std::isfinite(lastRealtimeCallbackTimeSeconds_) && lastRealtimeCallbackTimeSeconds_ > 0.0
        && callbackTimeSeconds > lastRealtimeCallbackTimeSeconds_
        && std::isfinite(sampleRate) && sampleRate > 0.0;
    if (hasTiming)
    {
        const auto elapsedSamples = std::max(
            1.0,
            (callbackTimeSeconds - lastRealtimeCallbackTimeSeconds_) * sampleRate);
        for (auto& pending : controlDrainScratch_)
        {
            if (pending.timing != RealtimeControlTiming::ArrivalTimestamp)
                continue;
            auto sourceSample = (pending.arrivalTimeSeconds - lastRealtimeCallbackTimeSeconds_) * sampleRate;
            if (!std::isfinite(sourceSample))
                sourceSample = 0.0;
            double mappedSample = 0.0;
            if (elapsedSamples > static_cast<double>(frameCount))
                mappedSample = sourceSample * static_cast<double>(frameCount) / elapsedSamples;
            else
                mappedSample = sourceSample + static_cast<double>(frameCount) - elapsedSamples;
            const auto maximumFrame = static_cast<double>(frameCount - 1);
            if (mappedSample < 0.0 || mappedSample > maximumFrame)
            {
                clampedRealtimeControlTimingCount_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
            pending.frameOffset = static_cast<std::size_t>(std::llround(
                std::clamp(mappedSample, 0.0, maximumFrame)));
        }
    }
    else
    {
        for (auto& pending : controlDrainScratch_)
            if (pending.timing == RealtimeControlTiming::ArrivalTimestamp)
                pending.frameOffset = 0;
    }
    if (std::isfinite(callbackTimeSeconds) && callbackTimeSeconds > 0.0)
        lastRealtimeCallbackTimeSeconds_ = callbackTimeSeconds;

    const auto rejectDsp = [&] {
        rejectedRealtimeControls_.fetch_add(1, std::memory_order_relaxed);
        dspRealtimeControlRejectionCount_.fetch_add(
            1,
            std::memory_order_relaxed);
    };

    std::size_t typedEventCount = 0;
    for (const auto& pending : controlDrainScratch_)
    {
        if (pending.frameOffset > renderTarget.maxBlockFrames())
        {
            rejectDsp();
            continue;
        }

        if (pending.kind
            == RealtimeControlCommandKind::ControllerRecordingObservation)
        {
            const auto absoluteFrame = pending.frameOffset
                    > std::numeric_limits<std::uint64_t>::max()
                        - callbackAbsoluteFrame
                ? std::numeric_limits<std::uint64_t>::max()
                : callbackAbsoluteFrame + pending.frameOffset;
            captureControllerRecordingEvent(
                pending,
                absoluteFrame,
                renderTarget.snapshotGeneration());
            continue;
        }

        TypedEventPayload payload{};
        PreparedTypedEvent event{};
        event.frameOffset = pending.frameOffset;
        event.sequence = static_cast<std::uint64_t>(typedEventCount + 1);
        if (pending.kind == RealtimeControlCommandKind::EventValue)
        {
            if (pending.stageGeneration != 0
                && pending.stageGeneration
                    != renderTarget.snapshotGeneration())
            {
                staleRealtimeControlCount_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                if (pending.timing
                    == RealtimeControlTiming::AbsoluteSteadyTimestamp)
                {
                    addSaturatedBounded(
                        scheduledRealtimeControlStaleCount_);
                }
                rejectedRealtimeControls_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                continue;
            }
            const auto* name = pending.eventId == 0
                    || pending.eventId
                        > registeredEventValueNames_.size()
                ? nullptr
                : registeredEventValueNames_[pending.eventId - 1].load(
                      std::memory_order_acquire);
            if (renderTarget.activePreparedLiveSetSoundCount() > 1)
            {
                if (realtimeControlGeneration_.load(
                        std::memory_order_acquire)
                    != audioControlGeneration_)
                {
                    staleRealtimeControlCount_.fetch_add(
                        1, std::memory_order_relaxed);
                    continue;
                }
                if (name == nullptr
                    || !renderTarget.scheduleEventValue(
                        *name, pending.value, pending.domain,
                        pending.frameOffset))
                {
                    rejectDsp();
                    continue;
                }
                if (pending.timing
                    == RealtimeControlTiming::AbsoluteSteadyTimestamp)
                    addSaturatedBounded(
                        scheduledRealtimeControlAppliedCount_);
                const auto absoluteFrame = pending.frameOffset
                        > std::numeric_limits<std::uint64_t>::max()
                            - callbackAbsoluteFrame
                    ? std::numeric_limits<std::uint64_t>::max()
                    : callbackAbsoluteFrame + pending.frameOffset;
                captureControllerRecordingEvent(
                    pending, absoluteFrame,
                    renderTarget.snapshotGeneration());
                if (pending.frameOffset == 0)
                    enqueueLiveToolEventTriggersFromAudio(
                        renderTarget, *name, pending.domain);
                else
                    liveToolRejectedTriggerCount_.fetch_add(
                        1, std::memory_order_relaxed);
                continue;
            }
            const auto resolvedTarget = name == nullptr
                ? std::optional<TypedEventTarget>{}
                : renderTarget.eventValueTarget(
                      *name,
                      pending.domain,
                      pending.generation);
            if (!resolvedTarget.has_value())
            {
                rejectDsp();
                continue;
            }
            event.kind = TypedEventKind::EventValueAssignment;
            event.target = *resolvedTarget;
            payload.eventValue = {pending.value, pending.domain};
        }
        else if (pending.kind
                 == RealtimeControlCommandKind::VariableAssignment)
        {
            if (pending.stageGeneration == 0
                || pending.stageGeneration
                    != renderTarget.snapshotGeneration()
                || pending.eventId == 0)
            {
                staleRealtimeControlCount_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                rejectedRealtimeControls_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                continue;
            }
            event.kind = TypedEventKind::VariableAssignment;
            event.target = {
                TypedEventTargetKind::Variable,
                pending.eventId,
                targetStageId,
                pending.generation,
                pending.voiceGeneration,
            };
            payload.variable = {pending.value, pending.domain};
        }
        else if (pending.kind == RealtimeControlCommandKind::SoundVariableAssignment)
        {
            if (pending.stageGeneration == 0 ||
                pending.stageGeneration != renderTarget.snapshotGeneration() ||
                pending.eventId == 0)
            {
                staleRealtimeControlCount_.fetch_add(1, std::memory_order_relaxed);
                rejectedRealtimeControls_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            event.kind = TypedEventKind::SoundVariableAssignment;
            event.target = {
                TypedEventTargetKind::SoundVariable,
                pending.eventId,
                targetStageId,
                pending.generation,
                pending.voiceGeneration,
            };
            payload.soundVariable = {pending.selectedOrdinal};
        }
        else if (pending.kind == RealtimeControlCommandKind::MidiNoteOn
                 || pending.kind == RealtimeControlCommandKind::MidiNoteOff)
        {
            // Notes played while a newly staged graph awaits adoption keep
            // playing the incumbent until the new graph becomes audible.
            if (pending.stageId != targetStageId
                && (pending.stageId == 0
                    || pending.stageId != graphHost_.pendingStageId()))
            {
                staleRealtimeControlCount_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                rejectedRealtimeControls_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                continue;
            }
            event.kind = pending.kind == RealtimeControlCommandKind::MidiNoteOff
                ? TypedEventKind::MidiNoteOff
                : TypedEventKind::MidiNoteOn;
            event.target = {
                TypedEventTargetKind::MidiVoice,
                1,
                targetStageId,
                pending.generation,
            };
            payload.midiNote = {
                pending.midiChannel,
                pending.midiNote,
                pending.midiVelocity,
            };
        }
        else
        {
            invalidRealtimeControlCount_.fetch_add(
                1,
                std::memory_order_relaxed);
            rejectedRealtimeControls_.fetch_add(
                1,
                std::memory_order_relaxed);
            continue;
        }
        event.payload = payload;
        typedEventCommandScratch_[typedEventCount] = pending;
        typedEventScratch_[typedEventCount++] = event;
        if (pending.timing
            == RealtimeControlTiming::AbsoluteSteadyTimestamp)
        {
            scheduledControlSequenceScratch_.push_back(
                event.sequence);
        }
    }

    PreparedTypedEventBatch typedBatch(typedEventScratch_);
    const auto batchStatus = typedBatch.assign(
        std::span<const PreparedTypedEvent>(
            typedEventScratch_.data(),
            typedEventCount),
        renderTarget.maxBlockFrames());
    if (batchStatus != TypedEventBatchStatus::Ok)
    {
        rejectedRealtimeControls_.fetch_add(
            typedEventCount,
            std::memory_order_relaxed);
        if (batchStatus == TypedEventBatchStatus::Overflow)
        {
            typedEventBatchOverflowCount_.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        else
        {
            invalidRealtimeControlCount_.fetch_add(
                typedEventCount,
                std::memory_order_relaxed);
        }
        finishRequestedRecording();
        return;
    }
    preparedTypedEventCount_.fetch_add(
        typedBatch.events().size(),
        std::memory_order_relaxed);

    const auto events = typedBatch.events();
    if (events.empty())
    {
        finishRequestedRecording();
        return;
    }
    std::ranges::sort(scheduledControlSequenceScratch_);
    auto dispatchResults = std::span<
        RealtimeTypedEventDispatchResult>(
        typedEventDispatchScratch_.data(),
        events.size());
    if (!renderTarget.scheduleTypedEvents(events, dispatchResults))
    {
        rejectDsp();
        finishRequestedRecording();
        return;
    }
    for (std::size_t eventIndex = 0;
         eventIndex < events.size();
         ++eventIndex)
    {
        const auto& event = events[eventIndex];
        if (realtimeControlGeneration_.load(std::memory_order_acquire)
            != audioControlGeneration_)
        {
            staleRealtimeControlCount_.fetch_add(
                events.size() - eventIndex,
                std::memory_order_relaxed);
            lastRealtimeCallbackTimeSeconds_ = 0.0;
            break;
        }
        if (dispatchResults[eventIndex]
            == RealtimeTypedEventDispatchResult::Handled)
        {
            const auto scheduled = std::ranges::binary_search(
                scheduledControlSequenceScratch_, event.sequence);
            if (scheduled)
            {
                addSaturatedBounded(
                    scheduledRealtimeControlAppliedCount_);
            }
            if (event.kind == TypedEventKind::EventValueAssignment
                && event.sequence != 0
                && event.sequence <= typedEventCommandScratch_.size())
            {
                const auto& command = typedEventCommandScratch_[
                    static_cast<std::size_t>(event.sequence - 1)];
                const auto absoluteFrame = command.frameOffset
                        > std::numeric_limits<std::uint64_t>::max()
                            - callbackAbsoluteFrame
                    ? std::numeric_limits<std::uint64_t>::max()
                    : callbackAbsoluteFrame + command.frameOffset;
                captureControllerRecordingEvent(
                    command,
                    absoluteFrame,
                    renderTarget.snapshotGeneration());
                const auto* eventName = command.eventId == 0
                        || command.eventId
                            > registeredEventValueNames_.size()
                    ? nullptr
                    : registeredEventValueNames_[command.eventId - 1].load(
                          std::memory_order_acquire);
                if (eventName != nullptr)
                {
                    if (command.frameOffset == 0)
                    {
                        enqueueLiveToolEventTriggersFromAudio(
                            renderTarget, *eventName, command.domain);
                    }
                    else
                    {
                        liveToolRejectedTriggerCount_.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
            }
            continue;
        }
        if (dispatchResults[eventIndex]
            == RealtimeTypedEventDispatchResult::Rejected)
        {
            rejectDsp();
            continue;
        }
        if (event.kind != TypedEventKind::MidiNoteOn
            && event.kind != TypedEventKind::MidiNoteOff)
        {
            rejectDsp();
            continue;
        }

        const auto channel = static_cast<int>(
            event.payload.midiNote.channel);
        const auto note = static_cast<int>(event.payload.midiNote.note);
        if (event.kind == TypedEventKind::MidiNoteOff)
        {
            if (channel != activeMidiChannel_ || note != activeMidiNote_)
                continue;
            if (!renderTarget.hasEventValueBinding(
                    "KeyDown",
                    talas::expr::ValueDomain::Scalar))
            {
                activeMidiChannel_ = 0;
                activeMidiNote_ = -1;
                continue;
            }
            const std::array<RealtimeEventValueUpdate, 1> updates{{
                {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
            }};
            if (renderTarget.scheduleEventValues(
                    updates,
                    event.frameOffset))
            {
                activeMidiChannel_ = 0;
                activeMidiNote_ = -1;
            }
            else
            {
                rejectDsp();
            }
            continue;
        }

        std::array<
            RealtimeEventValueUpdate,
            maxRealtimeEventValueBatchSize> updates{};
        std::size_t updateCount = 0;
        if (renderTarget.hasEventValueBinding(
                "Pitch",
                talas::expr::ValueDomain::NoteNumber))
        {
            updates[updateCount++] = {
                "Pitch",
                static_cast<double>(note),
                talas::expr::ValueDomain::NoteNumber,
            };
        }
        if (renderTarget.hasEventValueBinding(
                "KeyVelocity",
                talas::expr::ValueDomain::Scalar))
        {
            updates[updateCount++] = {
                "KeyVelocity",
                static_cast<double>(event.payload.midiNote.velocity) / 127.0,
                talas::expr::ValueDomain::Scalar,
            };
        }
        if (renderTarget.hasEventValueBinding(
                "KeyDown",
                talas::expr::ValueDomain::Scalar))
        {
            updates[updateCount++] = {
                "KeyDown",
                1.0,
                talas::expr::ValueDomain::Scalar,
            };
        }
        if (updateCount == 0)
            continue;
        if (renderTarget.scheduleEventValues(
                std::span<const RealtimeEventValueUpdate>(
                    updates.data(),
                    updateCount),
                event.frameOffset))
        {
            activeMidiChannel_ = channel;
            activeMidiNote_ = note;
        }
        else
        {
            rejectDsp();
        }
    }
    finishRequestedRecording();
}

void ProjectRuntimeBridge::mirrorDrainedRealtimeControls(
    RealtimeAudioEngine& renderTarget) noexcept
{
    const ProducerGate producerGate(sessionProducerGate_, ProducerGate::Mode::TryOnce);
    if (!producerGate)
        return;
    const auto rejectMirror = [&] {
        rejectedRealtimeControls_.fetch_add(
            1,
            std::memory_order_relaxed);
        dspRealtimeControlRejectionCount_.fetch_add(
            1,
            std::memory_order_relaxed);
    };
    for (const auto& command : controlDrainScratch_)
    {
        if (command.frameOffset > renderTarget.maxBlockFrames())
        {
            rejectMirror();
            continue;
        }
        if (command.kind == RealtimeControlCommandKind::EventValue)
        {
            if (command.stageGeneration != 0)
            {
                continue;
            }
            const auto* name = command.eventId == 0
                    || command.eventId
                        > registeredEventValueNames_.size()
                ? nullptr
                : registeredEventValueNames_[command.eventId - 1].load(
                      std::memory_order_acquire);
            if (name == nullptr
                || !renderTarget.scheduleEventValue(
                    *name,
                    command.value,
                    command.domain,
                    command.frameOffset))
            {
                rejectMirror();
            }
            continue;
        }
        if (command.kind != RealtimeControlCommandKind::MidiNoteOn
            && command.kind != RealtimeControlCommandKind::MidiNoteOff)
        {
            continue;
        }
        const RealtimeMidiNoteEvent event{
            command.kind == RealtimeControlCommandKind::MidiNoteOff
                ? RealtimeMidiNoteKind::NoteOff
                : RealtimeMidiNoteKind::NoteOn,
            command.midiChannel,
            command.midiNote,
            command.midiVelocity,
        };
        const auto result = renderTarget.scheduleMidiNote(
            event,
            command.frameOffset);
        if (result == RealtimeMidiDispatchResult::Rejected)
        {
            rejectMirror();
            continue;
        }
        if (result == RealtimeMidiDispatchResult::Handled)
            continue;

        std::array<RealtimeEventValueUpdate,
                   maxRealtimeEventValueBatchSize> updates{};
        std::size_t updateCount = 0;
        if (event.kind == RealtimeMidiNoteKind::NoteOff)
        {
            if (renderTarget.hasEventValueBinding(
                    "KeyDown",
                    talas::expr::ValueDomain::Scalar))
            {
                updates[updateCount++] = {
                    "KeyDown",
                    0.0,
                    talas::expr::ValueDomain::Scalar,
                };
            }
        }
        else
        {
            if (renderTarget.hasEventValueBinding(
                    "Pitch",
                    talas::expr::ValueDomain::NoteNumber))
            {
                updates[updateCount++] = {
                    "Pitch",
                    static_cast<double>(event.note),
                    talas::expr::ValueDomain::NoteNumber,
                };
            }
            if (renderTarget.hasEventValueBinding(
                    "KeyVelocity",
                    talas::expr::ValueDomain::Scalar))
            {
                updates[updateCount++] = {
                    "KeyVelocity",
                    static_cast<double>(event.velocity) / 127.0,
                    talas::expr::ValueDomain::Scalar,
                };
            }
            if (renderTarget.hasEventValueBinding(
                    "KeyDown",
                    talas::expr::ValueDomain::Scalar))
            {
                updates[updateCount++] = {
                    "KeyDown",
                    1.0,
                    talas::expr::ValueDomain::Scalar,
                };
            }
        }
        if (updateCount != 0
            && !renderTarget.scheduleEventValues(
                std::span<const RealtimeEventValueUpdate>(
                    updates.data(),
                    updateCount),
                command.frameOffset))
        {
            rejectMirror();
        }
    }
}
}
