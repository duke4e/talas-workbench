#pragma once

#include "talas/runtime/AudioStream.h"
#include "talas/runtime/RealtimeTransport.h"
#include "talas/runtime/Runtime.h"
#include "talas/runtime/SnapshotPerformance.h"
#include "talas/runtime/TypedEvent.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime
{
struct RealtimeAudioInputBlock
{
    const float* const* channels = nullptr;
    std::size_t channelCount = 0;
    // Offline snapshots may retain double precision without narrowing into
    // device float buffers. A present double channel takes precedence.
    const double* const* doubleChannels = nullptr;
    [[nodiscard]] bool hasChannel(std::size_t channel) const noexcept
    {
        return channel < channelCount && ((doubleChannels && doubleChannels[channel])
            || (channels && channels[channel]));
    }
    [[nodiscard]] double sample(std::size_t channel, std::size_t frame) const noexcept
    {
        return doubleChannels && doubleChannels[channel] ? doubleChannels[channel][frame] : channels[channel][frame];
    }
};

inline constexpr std::size_t maximumRealtimeMidiOutputBytes = 256;

struct RealtimeMidiOutputEvent
{
    std::size_t frameOffset = 0;
    std::array<std::uint8_t, maximumRealtimeMidiOutputBytes> bytes{};
    std::size_t byteCount = 0;
};

struct RealtimeGlobalControllerOutputEvent
{
    std::size_t frameOffset = 0;
    double value = 0.0;
    bool allowLiveOverride = false;
};

struct RealtimeTerminalOutputBlock
{
    std::span<RealtimeMidiOutputEvent> midiEvents;
    std::size_t midiEventCount = 0;
    std::uint64_t stageId = 0;
    std::span<RealtimeGlobalControllerOutputEvent> globalControllerEvents;
    std::size_t globalControllerEventCount = 0;
    std::span<char> globalControllerName;
    std::size_t globalControllerNameLength = 0;
    // Set by a ProjectRuntimeBridge whose callback adopted a staged
    // replacement at its prepared frame (ADR-0005): the events before
    // adoptionFrameOffset came from previousStageId and those after from
    // stageId. When both stages sent global controller events under
    // different names, the first previousGlobalControllerEventCount events
    // carry the name written to previousGlobalControllerName, and the rest
    // the name above. Zero otherwise.
    std::uint64_t previousStageId = 0;
    std::size_t adoptionFrameOffset = 0;
    std::size_t previousGlobalControllerEventCount = 0;
    std::span<char> previousGlobalControllerName;
    std::size_t previousGlobalControllerNameLength = 0;
};

// Rendered replaces the publication metadata above and fills only the
// published prefixes. Every failure result leaves caller storage and
// publication metadata unchanged.
enum class RealtimeTerminalRenderResult : std::uint8_t
{
    Rendered,
    WrongRenderKind,
    InvalidArguments,
    EvaluationFailure,
    CapacityExceeded,
};

enum class RealtimeStreamRole : std::uint8_t
{
    Playback,
    CachePrewarm,
};

struct RealtimeStreamConsumerHealthSnapshot
{
    std::string nodeId;
    std::string structuralSourcePath;
    std::string resourceId;
    RuntimeScopeId scopeId = globalRuntimeScope;
    RealtimeStreamRole role = RealtimeStreamRole::Playback;
    std::uint64_t seekGeneration = 0;
    AudioStreamHealthSnapshot health;
};

struct RealtimeStreamHealthSnapshot
{
    std::uint64_t stageId = 0;
    std::size_t consumerCount = 0;
    std::vector<RealtimeStreamConsumerHealthSnapshot> consumers;
    AudioStreamHealthSnapshot totals;
};

inline constexpr std::size_t maximumPreparedNodeTimingRecords = 256;

struct PreparedNodeTimingRecord
{
    std::string nodeId;
    std::string typeId;
    std::string structuralSourcePath;
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::uint64_t latestDurationNanoseconds = 0;
    std::uint64_t maximumDurationNanoseconds = 0;
    std::uint64_t sampledDurationNanoseconds = 0;
    std::uint64_t sampleCount = 0;
    std::uint64_t sampleSequence = 0;
    std::size_t callbackFrames = 0;
    bool publicationValid = false;
};

struct PreparedNodeTimingSnapshot
{
    std::uint64_t stageId = 0;
    double sampleRate = 0.0;
    std::vector<PreparedNodeTimingRecord> records;
    bool capacityExceeded = false;
};

struct PreparedVariableValue
{
    talas::project::PreparedProjectVariableIdentity identity;
    talas::expr::ExpressionValue value;
    std::uint64_t voiceGeneration = 0;
    std::uint8_t midiChannel = 0;
    std::uint8_t midiNote = 0;
    std::uint8_t midiVelocity = 0;
    bool voiceActive = false;
    bool voiceReleasing = false;
};

struct PreparedVariableValueSnapshot
{
    std::uint64_t stageId = 0;
    std::vector<PreparedVariableValue> values;
    bool publicationValid = false;
};

struct PreparedSoundVariableValue
{
    talas::project::PreparedProjectSoundVariableIdentity identity;
    std::string selectedDefinitionId;
    std::uint16_t selectedOrdinal = 0;
    std::uint64_t voiceGeneration = 0;
    std::uint8_t midiChannel = 0;
    std::uint8_t midiNote = 0;
    std::uint8_t midiVelocity = 0;
    bool voiceActive = false;
    bool voiceReleasing = false;
};

struct PreparedSoundVariableValueSnapshot
{
    std::uint64_t stageId = 0;
    std::vector<PreparedSoundVariableValue> values;
    bool publicationValid = false;
};

struct PreparedSoundVariableTargetIndex
{
    std::uint32_t index = 0;
    std::uint16_t selectedOrdinal = 0;

    bool operator==(const PreparedSoundVariableTargetIndex&) const = default;
};

struct RealtimeEventValueUpdate
{
    std::string_view name;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
};

enum class RealtimeMidiNoteKind : std::uint8_t
{
    NoteOn,
    NoteOff,
};

struct RealtimeMidiNoteEvent
{
    RealtimeMidiNoteKind kind = RealtimeMidiNoteKind::NoteOff;
    std::uint8_t channel = 1;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
};

enum class RealtimeMidiDispatchResult : std::uint8_t
{
    NotHandled,
    Handled,
    Rejected,
};

enum class RealtimeTypedEventDispatchResult : std::uint8_t
{
    Rejected,
    Handled,
    NotHandled,
};

inline constexpr std::size_t maxRealtimeEventValueBatchSize = 3;

class RealtimeRenderSession
{
public:
    class PreparedTransportRelocation
    {
    public:
        ~PreparedTransportRelocation();
    private:
        friend class RealtimeRenderSession;
        class State;
        explicit PreparedTransportRelocation(std::unique_ptr<State>);
        std::unique_ptr<State> state_;
    };
    // Single control producer; may run while the audio consumer renders. Reads
    // immutable compiler authority only. The budget includes reconstructed offsets.
    [[nodiscard]] std::unique_ptr<PreparedTransportRelocation> prepareTransportRelocation(
        std::uint64_t frame, std::uint64_t& remainingPreparationFrames, std::string& error) const;
    // Audio thread, before a callback. Installation preserves parent state;
    // the host must adopt the matching transport frame at this same boundary.
    [[nodiscard]] bool canInstallTransportRelocation(const PreparedTransportRelocation&) const noexcept;
    [[nodiscard]] bool installTransportRelocation(PreparedTransportRelocation&) noexcept;
    // A consumed candidate retains old source owners. Destroy it on the control thread.

    // The graph is read during construction; the session retains prepared state only.
    explicit RealtimeRenderSession(const PreparedRuntimeGraph& graph);
    ~RealtimeRenderSession();

    RealtimeRenderSession(RealtimeRenderSession&&) noexcept;
    RealtimeRenderSession& operator=(RealtimeRenderSession&&) noexcept;
    RealtimeRenderSession(const RealtimeRenderSession&) = delete;
    RealtimeRenderSession& operator=(const RealtimeRenderSession&) = delete;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::string_view error() const noexcept;
    [[nodiscard]] std::size_t maxBlockFrames() const noexcept;
    [[nodiscard]] std::size_t outputChannelCount() const noexcept;
    // The DiskRecorders within the Sound, in the
    // plan's order, and what one recorded on a channel during the last
    // render (silent where it was not recording; empty after a failed one).
    // What a MIDI output within the Sound,
    // beside its audio Sounds, sent during the last render, frame offsets
    // within it.
    [[nodiscard]] std::span<const RealtimeMidiOutputEvent> midiOutputTapEvents() const noexcept;
    [[nodiscard]] std::size_t diskRecorderTapCount() const noexcept;
    [[nodiscard]] std::span<const double> diskRecorderTapChannel(
        std::size_t tap, std::size_t channel) const noexcept;
    [[nodiscard]] const PreparedDiskRecorderTapPlan* diskRecorderTap(std::size_t tap) const noexcept;
    [[nodiscard]] double sampleRate() const noexcept;
    [[nodiscard]] std::shared_ptr<const PreparedScoreSchedule>
    preparedScoreSchedule() const noexcept;
    // Control-thread diagnostic for the exact consumed Score boundary.
    [[nodiscard]] std::optional<std::uint64_t>
    preparedScorePlaybackFrame() const noexcept;
    [[nodiscard]] std::optional<TransportLoopRegion> transportLoop() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> midiSongPositionFrame(
        std::uint16_t songPosition) const noexcept;
    [[nodiscard]] std::optional<talas::project::MusicalPosition>
    musicalPositionAtFrame(std::uint64_t frame) const noexcept;
    [[nodiscard]] std::optional<talas::project::MusicalPosition>
    musicalPositionAtTick(std::uint64_t tick) const noexcept;
    // ADR-0030: the prepared musical map's song position and tempo at a
    // frame, and the frame of a musical tick (960 a quarter).
    [[nodiscard]] std::optional<RealtimeMusicalClockReading>
    musicalClockAtFrame(std::uint64_t frame) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> musicalTickFrame(
        std::uint64_t tick) const noexcept;
    // Control-thread snapshot. It copies immutable stream identity and reads
    // atomic counters; never call it from the audio callback.
    [[nodiscard]] RealtimeStreamHealthSnapshot streamHealth() const;
    // Control-thread snapshot. One exact prepared node is sampled per callback
    // in round-robin order; callback publication is fixed and preallocated.
    [[nodiscard]] PreparedNodeTimingSnapshot preparedNodeTiming() const;
    [[nodiscard]] PreparedVariableValueSnapshot preparedVariableValues() const;
    // Control-thread only, before graph-host publication.
    [[nodiscard]] PreparedSoundVariableValueSnapshot preparedSoundVariableValues() const;
    // Control-thread preparation check for a precompiled same-topology
    // performance overlay. No state is copied here.
    [[nodiscard]] bool performanceStateCompatibleWith(
        const RealtimeRenderSession& source) const noexcept;
    // Audio-thread only at an activation boundary. Copies into already-sized
    // storage and never allocates, locks, or advances expression state.
    [[nodiscard]] bool transferPerformanceStateFrom(
        const RealtimeRenderSession& source) noexcept;
    // Control-thread only, before graph-host publication.
    [[nodiscard]] bool bindAnalysisStage(std::uint64_t stageId) noexcept;
    // Audio-thread only. The host owns and advances this immutable callback-entry value.
    void setTransportSnapshotBeforeRender(RealtimeTransportSnapshot snapshot) noexcept;
    // Audio-thread only, before the session's first render.
    void setTransportFrameBeforeFirstRender(std::uint64_t frame) noexcept;
    // Exclusive control-thread preparation before publication/first render.
    // Prepares live Score source DSP at one target; the realtime setter adopts it.
    // Failure preserves the initial reset state and any earlier prepared target.
    [[nodiscard]] bool prepareTransportFrameBeforeFirstRender(std::uint64_t frame, std::string& error);
    // Audio-thread only. Forward synchronization repairs a prior failed callback.
    void synchronizeTransportFrame(std::uint64_t frame) noexcept;
    // Audio-thread only at a stopped callback boundary.
    void resetTransport() noexcept;
    // Control-thread only. Live Event Value expressions are re-evaluated before atomically publishing new scalar values.
    [[nodiscard]] bool updateEventValue(std::string_view name, double value, talas::expr::ValueDomain domain);
    // Single-producer only. Scalar and batch scheduling share projected state and must never run concurrently.
    // The offset binds to the first callback whose entry snapshot observes publication.
    [[nodiscard]] bool scheduleEventValue(
        std::string_view name,
        double value,
        talas::expr::ValueDomain domain,
        std::size_t frameOffset) noexcept;
    // Audio-thread only at Live Set activation, after reset and before render.
    // The caller resets this candidate if any inherited assignment is rejected.
    [[nodiscard]] bool scheduleMatchingGlobalEventValuesFrom(
        const RealtimeRenderSession& source) noexcept;
    // Audio-thread snapshot of the latest rendered global values. Names refer
    // to this session's prepared bindings and remain valid while it is alive.
    [[nodiscard]] std::optional<std::size_t> captureCurrentGlobalEventValues(
        std::span<RealtimeEventValueUpdate> values) const noexcept;
    [[nodiscard]] std::optional<double> currentGlobalEventValue(
        std::string_view name,
        talas::expr::ValueDomain domain) const noexcept;
    [[nodiscard]] std::optional<double> currentGlobalEventValueAt(
        std::uint32_t targetIndex) const noexcept;
    // Admission check for a same-Sound reset: initial global values are
    // restored, so each differing retained value needs a queue slot.
    [[nodiscard]] bool canScheduleGlobalEventValuesAfterReset(
        std::span<const RealtimeEventValueUpdate> values) const noexcept;
    [[nodiscard]] bool scheduleEventValues(
        std::span<const RealtimeEventValueUpdate> values,
        std::size_t frameOffset) noexcept;
    [[nodiscard]] SnapshotDirectTransactionResult
    scheduleSnapshotDirectTransaction(
        std::span<const SnapshotDirectEventValueUpdate> updates,
        std::span<const SnapshotDirectVariableUpdate> variableUpdates,
        std::size_t frameOffset,
        std::uint64_t rampFrames) noexcept;
    [[nodiscard]] SnapshotDirectTransactionResult
    scheduleSnapshotDirectTransaction(
        std::span<const SnapshotDirectEventValueUpdate> updates,
        std::size_t frameOffset,
        std::uint64_t rampFrames) noexcept
    {
        return scheduleSnapshotDirectTransaction(
            updates,
            {},
            frameOffset,
            rampFrames);
    }
    [[nodiscard]] RealtimeMidiDispatchResult scheduleMidiNote(
        RealtimeMidiNoteEvent event,
        std::size_t frameOffset) noexcept;
    [[nodiscard]] std::optional<std::uint32_t> eventValueTargetIndex(
        std::string_view name,
        talas::expr::ValueDomain domain) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> eventValueTargetIndex(
        RuntimeScopeId scopeId,
        std::string_view name,
        talas::expr::ValueDomain domain) const noexcept;
    [[nodiscard]] bool resolveEventValue(
        std::string_view name,
        talas::expr::ExpressionValue& value) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> variableTargetIndex(
        const talas::project::PreparedProjectVariableIdentity& identity,
        talas::expr::ValueDomain domain) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> voiceVariableTargetIndex(
        const talas::project::PreparedProjectVariableIdentity& identity,
        talas::expr::ValueDomain domain,
        std::uint64_t voiceGeneration) const noexcept;
    [[nodiscard]] std::optional<PreparedSoundVariableTargetIndex> soundVariableTargetIndex(
        const talas::project::PreparedProjectSoundVariableIdentity& identity,
        std::string_view selectedDefinitionId
    ) const noexcept;
    [[nodiscard]] std::optional<PreparedSoundVariableTargetIndex> voiceSoundVariableTargetIndex(
        const talas::project::PreparedProjectSoundVariableIdentity& identity,
        std::string_view selectedDefinitionId,
        std::uint64_t voiceGeneration
    ) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> gateTargetIndex(
        std::string_view nodeId) const noexcept;
    [[nodiscard]] bool scheduleTypedEvents(
        std::span<const PreparedTypedEvent> events,
        std::span<RealtimeTypedEventDispatchResult> results) noexcept;
    // Control-thread preflight used to coordinate atomic graph-host forwarding.
    [[nodiscard]] bool hasEventValueBinding(std::string_view name, talas::expr::ValueDomain domain) const noexcept;
    [[nodiscard]] bool eventValueRequiresReprepare(
        std::string_view name,
        talas::expr::ValueDomain domain) const noexcept;
    [[nodiscard]] bool canScheduleEventValue(
        std::string_view name,
        double value,
        talas::expr::ValueDomain domain,
        std::size_t frameOffset) noexcept;
    // Read-only audio-thread inspection of the most recently rendered process
    // interval. The span and its nested partial spans remain valid until the
    // next render or reset.
    [[nodiscard]] std::span<const talas::dsp::PartialSpectrumFrame>
    inspectPartialSpectrum(
        std::string_view nodeId,
        std::string_view portId,
        RuntimeScopeId scopeId = globalRuntimeScope) const noexcept;
    [[nodiscard]] bool render(std::span<double> monoOutput) noexcept;
    [[nodiscard]] bool render(std::span<double> leftOutput, std::span<double> rightOutput) noexcept;
    [[nodiscard]] bool render(
        RealtimeAudioInputBlock input,
        std::span<double> leftOutput,
        std::span<double> rightOutput) noexcept;
    [[nodiscard]] bool render(
        RealtimeAudioInputBlock input,
        double* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount) noexcept;
    [[nodiscard]] RealtimeTerminalRenderResult renderTerminalEvents(
        std::size_t frameCount,
        RealtimeTerminalOutputBlock& output) noexcept;

private:
    friend class PreparedSourceCallback;
    friend class PreparedLiveScorePlayback;
    RealtimeRenderSession(const PreparedRuntimeGraph& graph, bool parentOwnedCallback);
    friend struct RealtimeRenderSessionTestAccess;
    friend class OfflineRenderSession;
    // Exclusive offline/control-thread transaction. Unavailable participants
    // retain their existing execution path until their checkpoint contract lands.
    class OfflineTransaction
    {
    public:
        class State;
        explicit OfflineTransaction(std::unique_ptr<State> state) noexcept;
        ~OfflineTransaction();
        // Offline thread, after one successful callback and before its scratch
        // is reused. Allocating capture; live realtime callbacks never call this.
        void collectAnalysis(std::size_t frameCount);
        [[nodiscard]] bool commit() noexcept;
    private:
        friend class PreparedLiveScorePlayback;
        void collectAnalysisFrom(OfflineTransaction& source, std::size_t frameCount);
        std::unique_ptr<State> state_;
    };
    [[nodiscard]] std::unique_ptr<OfflineTransaction> beginOfflineTransaction();


    // Offline adapter only, immediately after construction and before input
    // publication. Restore retained non-voice selections without rendering or
    // consuming event capacity; voice cells must retain their prepared initial value.
    [[nodiscard]] bool initializeOfflineSoundSelections(
        std::span<const std::uint16_t> selections) noexcept;

    [[nodiscard]] std::optional<std::size_t>
    multifileDiskPlayerInitialSelectedIndexForTesting() const noexcept;
    [[nodiscard]] std::optional<std::size_t>
    multifileDiskPlayerSelectedIndexForTesting() const noexcept;
    [[nodiscard]] std::optional<std::size_t>
    multisampleSelectedIndexForTesting() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    grainCloudRenderedFramesForTesting() const noexcept;
    [[nodiscard]] bool setGrainCloudRenderedFramesForTesting(
        std::uint64_t renderedFrames) noexcept;
    [[nodiscard]] std::optional<double>
    chopperElapsedTimeForTesting() const noexcept;
    [[nodiscard]] std::optional<double>
    functionGeneratorElapsedTimeForTesting() const noexcept;
    [[nodiscard]] std::optional<double>
    multislopeElapsedTimeForTesting() const noexcept;
    [[nodiscard]] std::optional<double>
    multisegmentElapsedTimeForTesting() const noexcept;
    [[nodiscard]] std::optional<double>
    graphicalEnvelopeElapsedTimeForTesting() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    noiseRandomStateForTesting() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    sampleCloudRenderedFramesForTesting() const noexcept;
    [[nodiscard]] bool setSampleCloudRenderedFramesForTesting(
        std::uint64_t renderedFrames) noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    cloudBankRenderedFramesForTesting() const noexcept;
    [[nodiscard]] bool setCloudBankRenderedFramesForTesting(
        std::uint64_t renderedFrames) noexcept;
    [[nodiscard]] std::array<std::uint64_t, 3>
    callbackTimelineStateForTesting() const noexcept;
    void setRecoveryScheduleSnapshotPauseForTesting(
        std::uint8_t state) noexcept;
    [[nodiscard]] std::uint8_t
    recoveryScheduleSnapshotPauseForTesting() const noexcept;

    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
