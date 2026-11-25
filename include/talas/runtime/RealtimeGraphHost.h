#pragma once

#include "talas/runtime/MidiClockPhaseFollower.h"
#include "talas/runtime/MtcTransportFollower.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/RealtimeMetrics.h"
#include "talas/runtime/RealtimeTransport.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

namespace talas::runtime
{
class PreparedLiveSet;
class PreparedLiveTools;
static_assert(
    std::atomic<RealtimeAudioEngine*>::is_always_lock_free,
    "realtime graph health publication must remain lock-free");
static_assert(
    std::atomic<std::uint64_t>::is_always_lock_free,
    "realtime graph metric counters must remain lock-free");

struct RealtimeGraphTransition;

using RealtimeGraphPreparationFn = void (*)(void* context, RealtimeAudioEngine& renderTarget) noexcept;
using RealtimeGraphMirroredPreparationFn = void (*)(
    void* context,
    RealtimeAudioEngine& renderTarget,
    RealtimeAudioEngine* fadingOutTarget) noexcept;

enum class RealtimeGraphStageResult
{
    Accepted,
    NullEngine,
    UnreadyEngine,
    PendingEngineAwaitingActivation,
    RetiredEngineAwaitingCollection
};

struct StageBoundRelocationSnapshot
{
    std::uint64_t acceptedCount = 0;
    std::uint16_t lastSongPosition = 0;
    std::uint64_t lastFrame = 0;
};

struct RealtimeMusicalTransportSnapshot
{
    RealtimeTransportSnapshot transport;
    std::uint64_t stageId = 0;
    std::optional<talas::project::MusicalPosition> musicalPosition;
    std::uint64_t externalAuthorityGeneration = 0;
    std::uint64_t externalPulseSequence = 0;
    std::uint64_t externalIntervalNanoseconds = 0;
    std::uint32_t externalMicrosecondsPerQuarter = 0;
    std::int64_t externalPhaseErrorNanoseconds = 0;
    std::uint64_t externalJitterNanoseconds = 0;
    std::uint8_t externalAcquisitionIntervals = 0;
    double externalLastPulseTimeSeconds = 0.0;
    MidiClockLockState externalLockState = MidiClockLockState::Waiting;
    MidiClockAnchorKind externalLastAnchor = MidiClockAnchorKind::None;
    MtcTransportFollowerSnapshot mtc;
    std::uint64_t mtcAuthorityGeneration = 0;
    std::uint64_t mtcStageId = 0;
    std::uint64_t mtcMappedProjectFrame = 0;
    bool externalAuthority = false;
    bool mtcAuthority = false;
    bool publicationValid = false;
    bool usedStableFallback = false;
};

class RealtimeGraphHost
{
public:
    static constexpr std::uint64_t noScorePlaybackFrame = ~std::uint64_t{0};
    RealtimeGraphHost();
    ~RealtimeGraphHost();

    RealtimeGraphHost(const RealtimeGraphHost&) = delete;
    RealtimeGraphHost& operator=(const RealtimeGraphHost&) = delete;

    // Control-thread methods. The host has one control producer.
    class PreparedTransportRelocation
    {
        friend class RealtimeGraphHost;
        std::vector<std::unique_ptr<RealtimeAudioEngine::PreparedTransportRelocation>> engines;
        std::uint64_t frame = 0;
    public:
        PreparedTransportRelocation() = default;
    };
    [[nodiscard]] std::unique_ptr<PreparedTransportRelocation> prepareTransportRelocation(
        std::uint64_t frame, std::string& error) const;
    // Audio thread, after pending-stage adoption, before transport adoption.
    // On failure neither sources nor transport change. Retire the handle on control.
    [[nodiscard]] bool installTransportRelocation(PreparedTransportRelocation&, std::uint64_t expectedStageId) noexcept;
    [[nodiscard]] RealtimeGraphStageResult tryStage(std::unique_ptr<RealtimeAudioEngine> engine) noexcept;
    [[nodiscard]] RealtimeGraphStageResult
    tryStageSameTopologyTransition(
        std::unique_ptr<RealtimeAudioEngine> engine,
        std::uint64_t transitionFrames) noexcept;
    void setPlaying(bool playing) noexcept;
    [[nodiscard]] bool updateEventValue(std::string_view name, double value, talas::expr::ValueDomain domain);
    [[nodiscard]] bool scheduleEventValue(
        std::string_view name,
        double value,
        talas::expr::ValueDomain domain,
        std::size_t frameOffset) noexcept;
    [[nodiscard]] SnapshotDirectTransactionResult
    scheduleSnapshotDirectTransaction(
        const SnapshotDirectTransaction& transaction) noexcept;
    [[nodiscard]] bool scheduleTypedEvents(
        std::span<const PreparedTypedEvent> events,
        std::span<RealtimeTypedEventDispatchResult> results) noexcept;
    [[nodiscard]] std::uint64_t transportFramePosition() const noexcept;
    // The active Timeline's composition cursor after the last render, held
    // while a WaitUntil within it waits; none
    // without a prepared Score.
    [[nodiscard]] std::optional<std::uint64_t> scorePlaybackFrame() const noexcept;
    [[nodiscard]] double transportSampleRate() const noexcept;
    [[nodiscard]] RealtimeTransportSnapshot transportSnapshot() const noexcept;
    [[nodiscard]] RealtimeMusicalTransportSnapshot
    musicalTransportSnapshot() const noexcept;
    [[nodiscard]] std::uint64_t discardedOutputChannelCount() const noexcept;
    [[nodiscard]] RealtimeGraphMetricsSnapshot
    realtimeMetricsRaw() const noexcept;
    [[nodiscard]] std::size_t activeOutputChannelCount() const noexcept;
    [[nodiscard]] std::uint64_t activeStageId() const noexcept;
    // A candidate whose prepared frame was passed, or cannot be reached because
    // the transport stopped short of it, is retired and the incumbent stays
    // audible. The control thread can collect and retry.
    [[nodiscard]] std::uint64_t lastRejectedPreparedStageId() const noexcept;
    [[nodiscard]] bool sameTopologyTransitionActive() const noexcept;
    [[nodiscard]] std::uint64_t
    sameTopologyTransitionCompletionToken() const noexcept;
    [[nodiscard]] RealtimeStreamHealthSnapshot streamHealth() const;
    [[nodiscard]] PreparedNodeTimingSnapshot preparedNodeTiming() const;
    [[nodiscard]] PreparedVariableValueSnapshot preparedVariableValues() const;
    [[nodiscard]] PreparedSoundVariableValueSnapshot preparedSoundVariableValues() const;
    // The audio thread adopts the reset at the next render callback boundary.
    void requestTransportReset() noexcept;
    // Audio-consumer thread only, immediately before render. The next render
    // adopts the exact absolute frame as one transport discontinuity.
    void relocateTransportAtNextRender(
        std::uint64_t absoluteFrame,
        std::uint64_t expectedStageId = 0,
        std::uint16_t songPosition = 0,
        std::uint64_t songPositionCount = 0) noexcept;
    // Audio-consumer only. Overrides the map-derived musical presentation for
    // one exact external-authority stage without relocating the DSP cursor.
    void setExternalMusicalTick(
        std::uint64_t tick,
        std::uint64_t expectedStageId,
        std::uint64_t authorityGeneration,
        MidiClockPhaseFollowerSnapshot follower,
        MidiClockAnchorKind anchor,
        double lastPulseTimeSeconds) noexcept;
    void clearExternalMusicalTick() noexcept;
    void setExternalMtc(
        MtcTransportFollowerSnapshot follower,
        std::uint64_t mappedProjectFrame) noexcept;
    void clearExternalMtc() noexcept;
    [[nodiscard]] bool relocateMidiSongPositionAtNextRender(
        std::uint16_t songPosition) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> midiSongPositionFrame(
        std::uint16_t songPosition,
        std::uint64_t expectedStageId = 0) const noexcept;
    [[nodiscard]] std::uint64_t
    rejectedStageBoundRelocationCount() const noexcept;
    [[nodiscard]] std::uint64_t
    lastStageBoundRelocationFrame() const noexcept;
    [[nodiscard]] StageBoundRelocationSnapshot
    stageBoundRelocationSnapshot() const noexcept;
    [[nodiscard]] std::unique_ptr<RealtimeAudioEngine> collectRetired() noexcept;

    // Audio-thread method. The host has one audio consumer.
    [[nodiscard]] bool render(float* const* outputChannels, std::size_t channelCount, std::size_t frameCount) noexcept;
    [[nodiscard]] bool render(
        RealtimeAudioInputBlock input,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount) noexcept;
    [[nodiscard]] RealtimeTerminalRenderResult renderTerminalEvents(
        std::size_t frameCount,
        RealtimeTerminalOutputBlock& output) noexcept;
    [[nodiscard]] RealtimeTerminalRenderResult renderPreparedTerminalEvents(
        std::size_t frameCount,
        RealtimeTerminalOutputBlock& output,
        void* context,
        RealtimeGraphPreparationFn prepare) noexcept;
    [[nodiscard]] bool renderPrepared(
        RealtimeAudioInputBlock input,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount,
        void* context,
        RealtimeGraphPreparationFn prepare) noexcept;
    [[nodiscard]] bool renderPreparedOrdered(
        RealtimeAudioInputBlock input,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount,
        void* context,
        RealtimeGraphPreparationFn prepare) noexcept;
    [[nodiscard]] bool renderPreparedMirrored(
        RealtimeAudioInputBlock input,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount,
        void* context,
        RealtimeGraphMirroredPreparationFn prepare) noexcept;
    [[nodiscard]] bool renderPreparedOrderedMirrored(
        RealtimeAudioInputBlock input,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount,
        void* context,
        RealtimeGraphMirroredPreparationFn prepare) noexcept;
    // Audio-consumer only. Freezes one exact pending/active render target for
    // the immediately following render call so pre-render control drains bind
    // to the same stage and profile that callback will execute.
    void prepareNextRenderBoundary() noexcept;
    [[nodiscard]] RealtimeTransportSnapshot prepareNextRenderTransportBoundary() noexcept;
    // Also adopts a pending stage prepared for exactly this split point.
    [[nodiscard]] RealtimeTransportSnapshot prepareContinuationRenderTransportBoundary() noexcept;
    // Audio-consumer only. The frame a pending stage was prepared for, so a
    // callback that contains it can split there.
    [[nodiscard]] std::optional<std::uint64_t> pendingAdoptionFrame() const noexcept;
    // Audio-consumer only. Zero when no stage is pending.
    [[nodiscard]] std::uint64_t pendingStageId() const noexcept;
    [[nodiscard]] double preparedRenderSampleRate() const noexcept;

    // Audio-consumer only, after preparing the current render boundary.
    [[nodiscard]] RealtimeTransportSnapshot preparedTransportSnapshot() const noexcept;
    // Audio-consumer-only Live Set operations. Their pointees and mutable
    // transition state remain owned by the adopted prepared engine.
    [[nodiscard]] const PreparedLiveSet* preparedLiveSet() const noexcept;
    [[nodiscard]] const PreparedLiveTools* preparedLiveTools() const noexcept;
    [[nodiscard]] bool activatePreparedLiveSetProgram(std::size_t cellIndex,
                                                      std::uint64_t transitionFrames = 0) noexcept;
    [[nodiscard]] std::uint32_t activePreparedLiveSetCellIndexPlusOne() const noexcept;
    [[nodiscard]] std::uint32_t activePreparedLiveSetSoundCellIndexPlusOne(
        std::uint32_t column) const noexcept;
    [[nodiscard]] bool setPreparedLiveSetSoundColumnMuted(
        std::uint32_t column, bool muted) noexcept;
    [[nodiscard]] bool preparedLiveSetSoundColumnMuted(
        std::uint32_t column) const noexcept;
    [[nodiscard]] bool preparedLiveSetActivationPending() const noexcept;
    void deactivatePreparedLiveSetProgram() noexcept;

    // Call only after the audio device has stopped invoking render.
    void clearAfterAudioStopped() noexcept;

private:
    friend class RealtimeGraphHostTestAccess;
    [[nodiscard]] bool renderPreparedInternal(
        RealtimeAudioInputBlock input,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount,
        void* context,
        RealtimeGraphPreparationFn prepare,
        RealtimeGraphMirroredPreparationFn mirroredPrepare,
        bool orderedOutputs) noexcept;
    void adoptPendingAtAudioBoundary() noexcept;
    void adoptTransportAtAudioBoundary(bool allowControlReset) noexcept;
    void publishTransportSnapshot() noexcept;

    std::atomic<RealtimeAudioEngine*> active_ = nullptr;
    std::atomic<RealtimeAudioEngine*> pending_ = nullptr;
    std::atomic<RealtimeAudioEngine*> retired_ = nullptr;
    std::atomic<RealtimeAudioEngine*> transitioning_ = nullptr;
    std::atomic<RealtimeGraphTransition*> pendingTransition_ = nullptr;
    std::atomic<RealtimeGraphTransition*> activeTransition_ = nullptr;
    std::atomic<RealtimeGraphTransition*> retiredTransition_ = nullptr;
    // Always names one live pending, active, or transitioning engine. The
    // audio thread publishes the replacement before retiring the prior source,
    // so control-thread health reads need exactly one non-waiting atomic load.
    std::atomic<RealtimeAudioEngine*> healthSource_ = nullptr;
    std::atomic<bool> playing_ = false;
    std::atomic<std::uint64_t> publishedTransportEpoch_ = 0;
    std::atomic<std::uint64_t> publishedTransportAbsoluteFrame_ = 0;
    std::atomic<std::uint64_t> transportFramePosition_ = 0;
    std::atomic<std::uint64_t> scorePlaybackFrame_ = noScorePlaybackFrame;
    std::atomic<double> transportSampleRate_ = 0.0;
    std::atomic<std::uint64_t> publishedTransportLoopStartFrame_ = 0;
    std::atomic<std::uint64_t> publishedTransportLoopEndFrame_ = 0;
    std::atomic<std::uint64_t> publishedTransportDiscontinuitySequence_ = 0;
    std::atomic<bool> publishedTransportPlaying_ = false;
    std::atomic<bool> publishedTransportLoopEnabled_ = false;
    std::atomic<std::uint64_t> publishedTransportStageId_ = 0;
    std::atomic<bool> publishedMusicalPositionAvailable_ = false;
    std::atomic<std::uint64_t> publishedMusicalBar_ = 0;
    std::atomic<std::uint32_t> publishedMusicalBeat_ = 0;
    std::atomic<std::uint32_t> publishedMusicalTick_ = 0;
    std::atomic<std::uint64_t> publishedExternalAuthorityGeneration_ = 0;
    std::atomic<std::uint64_t> publishedExternalPulseSequence_ = 0;
    std::atomic<std::uint64_t> publishedExternalIntervalNanoseconds_ = 0;
    std::atomic<std::uint32_t> publishedExternalMicrosecondsPerQuarter_ = 0;
    std::atomic<std::int64_t> publishedExternalPhaseErrorNanoseconds_ = 0;
    std::atomic<std::uint64_t> publishedExternalJitterNanoseconds_ = 0;
    std::atomic<std::uint8_t> publishedExternalAcquisitionIntervals_ = 0;
    std::atomic<double> publishedExternalLastPulseTimeSeconds_ = 0.0;
    std::atomic<MidiClockLockState> publishedExternalLockState_ =
        MidiClockLockState::Waiting;
    std::atomic<MidiClockAnchorKind> publishedExternalAnchor_ =
        MidiClockAnchorKind::None;
    std::atomic<bool> publishedExternalAuthority_ = false;
    std::atomic<std::uint64_t> publishedMtcAuthorityGeneration_ = 0;
    std::atomic<std::uint64_t> publishedMtcStageId_ = 0;
    std::atomic<std::uint64_t> publishedMtcSourceGeneration_ = 0;
    std::atomic<std::uint8_t> publishedMtcSourceSlot_ = 0;
    std::atomic<std::uint64_t> publishedMtcMappedProjectFrame_ = 0;
    std::atomic<std::uint64_t> publishedMtcAbsoluteFrame_ = 0;
    std::atomic<std::uint64_t> publishedMtcCompleteSetSequence_ = 0;
    std::atomic<std::uint8_t> publishedMtcNextExpectedPiece_ = 0;
    std::atomic<double> publishedMtcLastCompleteTimeSeconds_ = 0.0;
    std::atomic<MtcTransportLockState> publishedMtcLockState_ =
        MtcTransportLockState::Waiting;
    std::atomic<MtcDiscontinuityReason> publishedMtcDiscontinuityReason_ =
        MtcDiscontinuityReason::None;
    std::atomic<SmpteRate> publishedMtcRate_ = SmpteRate::Fps24;
    std::atomic<SmpteDirection> publishedMtcDirection_ =
        SmpteDirection::Forward;
    std::atomic<std::uint8_t> publishedMtcHour_ = 0;
    std::atomic<std::uint8_t> publishedMtcMinute_ = 0;
    std::atomic<std::uint8_t> publishedMtcSecond_ = 0;
    std::atomic<std::uint8_t> publishedMtcFrame_ = 0;
    std::atomic<bool> publishedMtcDropFrame_ = false;
    std::atomic<bool> publishedMtcHasTimeCode_ = false;
    std::atomic<bool> publishedMtcRunningEvidence_ = false;
    std::atomic<bool> publishedMtcAuthority_ = false;
    std::atomic<std::uint64_t> transportSnapshotGeneration_ = 0;
    const std::uint64_t snapshotInstanceIdentity_;
    std::uint64_t absoluteTransportFramePosition_ = 0;
    std::uint64_t transportEpoch_ = 0;
    std::uint64_t transportDiscontinuitySequence_ = 0;
    double activeTransportSampleRate_ = 0.0;
    std::optional<TransportLoopRegion> activeTransportLoop_;
    std::atomic<std::uint64_t> transportResetGeneration_ = 0;
    std::uint64_t audioThreadRelocationFrame_ = 0;
    std::uint64_t audioThreadRelocationExpectedStageId_ = 0;
    std::uint16_t audioThreadRelocationSongPosition_ = 0;
    std::uint64_t audioThreadRelocationSongPositionCount_ = 0;
    bool audioThreadRelocationPending_ = false;
    std::uint64_t audioThreadExternalMusicalTick_ = 0;
    std::uint64_t audioThreadExternalMusicalStageId_ = 0;
    std::uint64_t audioThreadExternalAuthorityGeneration_ = 0;
    MidiClockPhaseFollowerSnapshot audioThreadExternalFollower_;
    MidiClockAnchorKind audioThreadExternalAnchor_ =
        MidiClockAnchorKind::None;
    double audioThreadExternalLastPulseTimeSeconds_ = 0.0;
    bool audioThreadExternalMusicalTickAvailable_ = false;
    MtcTransportFollowerSnapshot audioThreadExternalMtc_;
    std::uint64_t audioThreadExternalMtcMappedProjectFrame_ = 0;
    bool audioThreadExternalMtcAvailable_ = false;
    std::atomic<std::uint64_t> rejectedStageBoundRelocationCount_ = 0;
    std::atomic<std::uint64_t> stageBoundRelocationGeneration_ = 0;
    std::atomic<std::uint64_t> acceptedStageBoundRelocationCount_ = 0;
    std::atomic<std::uint16_t> lastStageBoundRelocationSongPosition_ = 0;
    std::atomic<std::uint64_t> lastStageBoundRelocationFrame_ = 0;
    std::atomic<std::uint64_t> discardedOutputChannelCount_ = 0;
    std::atomic<std::uint64_t> renderNonFailureCount_ = 0;
    std::atomic<std::uint64_t> renderFailureCount_ = 0;
    std::atomic<std::uint64_t> stageRejectionCount_ = 0;
    std::atomic<std::uint64_t> preparedStageRejectionCount_ = 0;
    std::atomic<std::uint64_t> lastRejectedPreparedStageId_ = 0;
    std::atomic<std::size_t> activeOutputChannelCount_ = 0;
    std::atomic<std::uint64_t> activeStageId_ = 0;
    std::atomic<std::uint64_t> transitionCompletionToken_ = 0;
    std::uint64_t adoptedTransportResetGeneration_ = 0;
    bool audioBoundaryPrepared_ = false;
    bool transportBoundaryPrepared_ = false;
};
}
