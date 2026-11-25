#pragma once

#include "talas/project/ControllerRecording.h"
#include "talas/project/ProjectEditor.h"
#include "talas/runtime/MidiClockPhaseFollower.h"
#include "talas/runtime/MtcRealtimeIngress.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/runtime/PreparedLiveSet.h"
#include "talas/runtime/PreparedLiveTools.h"
#include "talas/runtime/RealtimeControlIngress.h"
#include "talas/runtime/RealtimeGraphHost.h"
#include "talas/runtime/RealtimeLiveSetIngress.h"
#include "talas/runtime/RealtimeLiveToolIngress.h"
#include "talas/runtime/Runtime.h"
#include "talas/runtime/SnapshotPerformance.h"
#include "talas/runtime/TypedEvent.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace talas::runtime
{
namespace component_bundle
{
class ComponentBundleReplacementReceipt;
}

enum class ProjectStageOutcome
{
    Staged,
    CompileFailed,
    ResourceIdentityConflict,
    EngineCreationFailed,
    EngineRejected,
    ReplacementRejected,
    StageRejected,
    TopologyMismatch,
    OverlayCoalesced
};

struct ControllerRecordingIngressEvidence
{
    talas::project::ControllerRecordingOrigin origin =
        talas::project::ControllerRecordingOrigin::Ui;
    std::int64_t sourceTimestampNanoseconds = 0;
    std::uint64_t routeGeneration = 0;
    std::uint64_t sourceGeneration = 0;
};

struct ControllerRecordingArmResult
{
    bool accepted = false;
    std::uint64_t takeGeneration = 0;
    std::string message;
};

struct ProjectStageResult
{
    ProjectStageOutcome outcome = ProjectStageOutcome::CompileFailed;
    std::optional<RealtimeGraphStageResult> stageResult;
    std::vector<Diagnostic> diagnostics;
    std::string message;
    std::size_t outputChannelCount = 0;

    [[nodiscard]] bool ok() const
    {
        return outcome == ProjectStageOutcome::Staged
            || outcome == ProjectStageOutcome::OverlayCoalesced;
    }
};

enum class LiveSetActivationOutcome : std::uint8_t
{
    Accepted,
    NotPrepared,
    UnknownCell,
    CellFailed,
    StaleAuthority,
    QuantizationFailed,
    QueueFull,
    IdentityExhausted,
};

struct LiveSetActivationResult
{
    LiveSetActivationOutcome outcome = LiveSetActivationOutcome::NotPrepared;
    std::uint64_t requestGeneration = 0;
    std::uint64_t resolvedFrame = 0;
    std::string_view message;

    [[nodiscard]] bool ok() const noexcept { return outcome == LiveSetActivationOutcome::Accepted; }
};

struct RealtimeLiveSetTelemetrySnapshot
{
    std::uint64_t acceptedRequests = 0;
    std::uint64_t completedActivations = 0;
    std::uint64_t queueOverflow = 0;
    std::uint64_t staleAuthority = 0;
    std::uint64_t failedActivations = 0;
    std::uint64_t terminalRequestGeneration = 0;
    RealtimeLiveSetTerminalOutcome terminalOutcome =
        RealtimeLiveSetTerminalOutcome::None;
    std::uint64_t activeRequestGeneration = 0;
    std::uint32_t activeCellIndexPlusOne = 0;
    bool scoreRegionActive = false;
    std::array<std::uint32_t, talas::project::projectLiveSetColumns>
        soundCellIndexPlusOneByColumn{};
    std::uint32_t mutedSoundColumnsMask = 0;
};

enum class LiveToolTriggerOutcome : std::uint8_t
{
    Accepted,
    NotPrepared,
    UnknownTool,
    InvalidPayload,
    StaleAuthority,
    QueueFull,
    IdentityExhausted,
};

struct LiveToolTriggerResult
{
    LiveToolTriggerOutcome outcome = LiveToolTriggerOutcome::NotPrepared;
    std::uint64_t requestGeneration = 0;
    std::string_view message;

    [[nodiscard]] bool ok() const noexcept
    {
        return outcome == LiveToolTriggerOutcome::Accepted;
    }
};

struct LiveToolTriggerRequest
{
    std::string_view toolId;
    talas::project::ProjectLiveToolTriggerKind triggerKind =
        talas::project::ProjectLiveToolTriggerKind::Manual;
    RealtimeLiveToolSourceKind sourceKind =
        RealtimeLiveToolSourceKind::Workbench;
    std::string_view identifier;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double variableNumber = 0.0;
    bool variableBoolean = false;
    bool hasVariableValue = false;
    std::int32_t midiChannel = 1;
    std::int32_t midiProgram = 0;
    std::uint64_t musicalTick = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t routeGeneration = 0;
};

struct RealtimeLiveToolTelemetrySnapshot
{
    std::uint64_t acceptedTriggers = 0;
    std::uint64_t appliedTransitions = 0;
    std::uint64_t queueOverflow = 0;
    std::uint64_t staleAuthority = 0;
    std::uint64_t rejectedTriggers = 0;
    std::uint64_t noMatchingTransition = 0;
    std::uint64_t staleTriggerAuthority = 0;
    std::uint64_t invalidTrigger = 0;
    std::uint64_t guardRejected = 0;
    std::uint64_t commandRejected = 0;
    std::uint64_t stateInitializationRejected = 0;
    std::uint64_t activateCellCommands = 0;
    std::uint64_t stopCommands = 0;
    std::uint64_t setProjectVariableCommands = 0;
    std::uint64_t setWorkspaceVariableCommands = 0;
    std::uint64_t setEventValueCommands = 0;
};

struct RealtimeControlTelemetrySnapshot
{
    std::uint64_t preparedTypedEvents = 0;
    std::uint64_t queueOverflow = 0;
    std::uint64_t staleGenerationOrStage = 0;
    std::uint64_t lateOrClampedTiming = 0;
    std::uint64_t invalidTargetOrPayload = 0;
    std::uint64_t batchOverflow = 0;
    std::uint64_t aggregateRejected = 0;
    std::uint64_t dspRejection = 0;
    std::uint64_t scheduledAccepted = 0;
    std::uint64_t scheduledApplied = 0;
    std::uint64_t scheduledLate = 0;
    std::uint64_t scheduledPendingFull = 0;
    std::uint64_t scheduledStale = 0;
    std::uint64_t scheduledPending = 0;
    std::uint64_t scheduledPendingHighWatermark = 0;
    std::uint64_t preparedSnapshotDirectTransactions = 0;
    std::uint64_t discardedOutputChannels = 0;
};

struct MidiClockTelemetrySnapshot
{
    std::uint64_t authorityGeneration = 0;
    std::uint64_t sourceLosses = 0;
    std::uint64_t acceptedTicks = 0;
    std::uint64_t acceptedStarts = 0;
    std::uint64_t acceptedContinues = 0;
    std::uint64_t acceptedStops = 0;
    std::uint64_t acceptedSongPositions = 0;
    std::uint64_t songPositionConversionFailures = 0;
    std::uint16_t lastSongPosition = 0;
    std::uint64_t lastRelocationFrame = 0;
    std::uint64_t queueOverflow = 0;
    std::uint64_t staleAuthority = 0;
    std::uint64_t wrongSource = 0;
    std::uint64_t invalidRecords = 0;
    std::uint64_t unsupportedSongPosition = 0;
    std::uint64_t signalTimeouts = 0;
    std::uint64_t tempoAcquisitions = 0;
    std::uint64_t tempoReacquisitions = 0;
    std::uint64_t tempoRangeRejections = 0;
    std::uint64_t cadenceDiscontinuities = 0;
    std::uint64_t timestampRejections = 0;
    std::uint64_t estimatorOverflows = 0;
    std::uint64_t holdoverEntries = 0;
    std::uint64_t publicationValidationFailures = 0;
    std::uint64_t pulseSequence = 0;
    std::uint64_t intervalNanoseconds = 0;
    std::uint32_t estimatedMicrosecondsPerQuarter = 0;
    std::int64_t phaseErrorNanoseconds = 0;
    std::uint64_t jitterNanoseconds = 0;
    std::uint8_t acquisitionIntervals = 0;
    MidiClockLockState lockState = MidiClockLockState::Waiting;
    MidiClockAnchorKind lastAnchor = MidiClockAnchorKind::None;
    double lastTickTimeSeconds = 0.0;
    bool externalAuthority = false;
    bool running = false;
    bool signalStale = false;
};

struct MtcTelemetrySnapshot
{
    MtcTransportFollowerSnapshot follower;
    std::uint64_t authorityGeneration = 0;
    std::uint64_t mappedProjectFrame = 0;
    std::uint64_t sourceLosses = 0;
    std::uint64_t queueOverflow = 0;
    std::uint64_t staleAuthority = 0;
    std::uint64_t wrongSource = 0;
    std::uint64_t invalidRecords = 0;
    std::uint64_t mappingFailures = 0;
    std::uint64_t publicationValidationFailures = 0;
    bool externalAuthority = false;
    bool running = false;
};

struct AcceptedResourceIdentity
{
    talas::core::ResourceSnapshotKind kind =
        talas::core::ResourceSnapshotKind::Table;
    std::uint64_t generation = 0;
    std::string contentHash;
};

struct PreparedGraphStatusSnapshot
{
    struct OutputDeviceRoute
    {
        std::int32_t oneBasedPhysicalChannel = -1;
        bool available = false;
    };

    std::shared_ptr<const PreparedGraphSummary> prepared;
    RealtimeStreamHealthSnapshot streamHealth;
    std::vector<OutputDeviceRoute> outputDeviceRoutes;
    std::uint64_t activeStageId = 0;
    bool deviceMappingAttached = false;
    bool awaitingActivation = false;
    // The source history missed its prepared audio boundary. The previous
    // graph remains active; collect retirement and compile a fresh candidate.
    bool activationRejected = false;
};

struct ProjectVariableAssignment
{
    talas::project::PreparedProjectVariableIdentity identity;
    talas::expr::ExpressionValue value;

    bool operator==(const ProjectVariableAssignment&) const = default;
};

struct ProjectSoundVariableAssignment
{
    talas::project::PreparedProjectSoundVariableIdentity identity;
    std::string selectedDefinitionId;

    bool operator==(const ProjectSoundVariableAssignment&) const = default;
};

class ProjectRuntimeBridge
{
public:
    ProjectRuntimeBridge(
        talas::project::ProjectDocument document,
        const talas::core::NodeRegistry& registry,
        std::size_t controlIngressCapacity = 1024);

    ~ProjectRuntimeBridge();

    [[nodiscard]] talas::project::ProjectEditor& editor();
    [[nodiscard]] const talas::project::ProjectEditor& editor() const;

    // Control-thread methods.
    // Every stage registers its Event Value names for realtime control. A
    // stage that gives a name another domain than the open project staged it
    // with fails; after editor().replaceDocument() the next accepted stage
    // may give a name the replaced project used another domain.
    [[nodiscard]] ProjectStageResult compileAndStage(RenderSettings settings);
    [[nodiscard]] ProjectStageResult compileAndStageDocument(
        const talas::project::ProjectDocument& document,
        RenderSettings settings);
    [[nodiscard]] ProjectStageResult compileAndStageIsolatedComponent(
        const talas::project::ProjectDocument& document,
        RenderSettings settings,
        std::shared_ptr<PreparedIsolatedComponentBinding> binding);
    [[nodiscard]] ProjectStageResult
    compileAndStageIsolatedComponentReplacement(
        const talas::project::ProjectDocument& document,
        RenderSettings settings,
        std::shared_ptr<PreparedIsolatedComponentBinding> binding,
        std::uint64_t documentGeneration,
        std::string_view nodeId,
        const ComponentApprovalFingerprint& candidate,
        component_bundle::ComponentBundleReplacementReceipt&& receipt);
    [[nodiscard]] ProjectStageResult
    compileAndStagePerformanceOverlay(
        std::span<const talas::project::SnapshotValueEntry> values,
        double smoothingSeconds);
    [[nodiscard]] const SnapshotPerformanceState&
    performanceState() const noexcept;
    [[nodiscard]] std::span<const talas::project::SnapshotValueEntry>
    performanceBaselineValues() const noexcept;
    [[nodiscard]] bool
    snapshotPerformanceDocumentCurrent() const;
    [[nodiscard]] talas::project::SnapshotOperationResult
    currentSnapshotScopeValues(
        const talas::project::SnapshotScopeIdentity& scope) const;
    [[nodiscard]] SnapshotPerformanceActionResult recallSnapshot(
        std::string_view bankId,
        std::string_view snapshotId);
    [[nodiscard]] SnapshotPerformanceActionResult beginSnapshotMorph(
        std::string_view bankId,
        std::string_view leftSnapshotId,
        std::string_view rightSnapshotId);
    [[nodiscard]] SnapshotPerformanceActionResult updateSnapshotMorph(
        double amount);
    void endSnapshotMorph() noexcept;
    [[nodiscard]] SnapshotPerformanceActionResult
    randomizeSnapshotBank(
        std::string_view bankId,
        std::uint64_t seed);
    [[nodiscard]] SnapshotPerformanceActionResult
    randomMergeSnapshotBank(
        std::string_view bankId,
        std::uint64_t seed,
        double amount);
    [[nodiscard]] SnapshotPerformanceActionResult
    resetSnapshotPerformance();
    [[nodiscard]] SnapshotPerformanceActionResult
    captureSnapshotPerformance(
        std::string_view bankId,
        std::string_view snapshotId);
    [[nodiscard]] SnapshotPerformanceActionResult
    adoptSnapshotAuthoringEdit();
    [[nodiscard]] bool snapshotTransitionActive() const noexcept;
    [[nodiscard]] bool snapshotTransitionCoalesced() const noexcept;
    // Call when this bridge adopts a different project document identity.
    void resetResourceIdentityHistory() noexcept;
    void resetVariableValueHistory() noexcept;
    [[nodiscard]] std::optional<AcceptedResourceIdentity>
    acceptedResourceIdentity(
        std::string_view assetId) const;
    void setPlaying(bool playing) noexcept;
    // A grid playing its compiled Sounds: each
    // Sound named here plays, as a Live Grid box, the program kept for it,
    // not the project's own, from the next stage until it is cleared.
    void setCompiledLiveSetSounds(
        std::unordered_map<std::string, talas::project::ProjectDocument> programs);
    void stopTransport() noexcept;
    // Single control-producer request. The audio consumer adopts the newest
    // exact frame at the next render boundary without blocking the callback.
    // Source reconstruction occurs here on control; failed preparation leaves
    // the active graph and any previously accepted request untouched.
    [[nodiscard]] bool requestTransportRelocation(
        std::uint64_t absoluteFrame) noexcept;
    [[nodiscard]] std::uint64_t transportFramePosition() const noexcept;
    // The Timeline's composition cursor after the
    // last render, which a WaitUntil within it holds until it resumes, so a
    // live recording knows when the Timeline has reached its end.
    [[nodiscard]] std::optional<std::uint64_t> scorePlaybackFrame() const noexcept;
    [[nodiscard]] std::shared_ptr<const PreparedScoreSchedule>
    latestPreparedScoreSchedule() const noexcept;
    [[nodiscard]] std::shared_ptr<const PreparedLiveSet>
    latestPreparedLiveSet() const noexcept;
    [[nodiscard]] std::shared_ptr<const PreparedLiveTools>
    latestPreparedLiveTools() const noexcept;
    [[nodiscard]] bool liveSetActivationReady() const noexcept;
    [[nodiscard]] LiveSetActivationResult requestLiveSetActivation(
        std::string_view liveSetId,
        std::string_view cellId,
        RealtimeLiveSetSourceKind sourceKind,
        std::uint64_t sourceGeneration,
        std::uint64_t routeGeneration) noexcept;
    void publishLiveSetSourceAuthority(RealtimeLiveSetSourceKind sourceKind,
                                       std::uint64_t sourceGeneration,
                                       std::uint64_t routeGeneration) noexcept;
    void revokeLiveSetSourceAuthority(
        RealtimeLiveSetSourceKind sourceKind) noexcept;
    [[nodiscard]] RealtimeLiveSetTelemetrySnapshot realtimeLiveSetTelemetry() const noexcept;
    [[nodiscard]] LiveToolTriggerResult requestLiveToolTrigger(
        const LiveToolTriggerRequest& request) noexcept;
    void publishLiveToolSourceAuthority(
        RealtimeLiveToolSourceKind sourceKind,
        std::uint64_t sourceGeneration,
        std::uint64_t routeGeneration) noexcept;
    void revokeLiveToolSourceAuthority(
        RealtimeLiveToolSourceKind sourceKind) noexcept;
    [[nodiscard]] RealtimeLiveToolTelemetrySnapshot
    realtimeLiveToolTelemetry() const noexcept;
    [[nodiscard]] std::optional<PreparedLiveToolRuntimeState>
    realtimeLiveToolState() const noexcept;
    [[nodiscard]] bool requestLiveToolStateRestore(
        const PreparedLiveToolRuntimeState& state,
        std::uint64_t* acceptedRequestGeneration = nullptr) noexcept;
    [[nodiscard]] std::uint64_t
    appliedLiveToolStateRestoreGeneration() const noexcept;
    [[nodiscard]] double transportTimeSeconds() const noexcept;
    [[nodiscard]] RealtimeMusicalTransportSnapshot
    musicalTransportSnapshot() const noexcept;
    // These producer APIs report bounded queue admission. DSP validation happens on the audio thread;
    // observe rejectedRealtimeControlCount() for commands rejected by the pinned render graph.
    [[nodiscard]] bool updateEventValue(std::string_view name, double value, talas::expr::ValueDomain domain);
    [[nodiscard]] bool scheduleEventValue(
        std::string_view name,
        double value,
        talas::expr::ValueDomain domain,
        std::size_t frameOffset);
    // Control-thread only. The stable identity is resolved against the exact
    // latest prepared stage before a numeric cell handle enters the callback.
    [[nodiscard]] bool scheduleVariableAssignment(
        const talas::project::PreparedProjectVariableIdentity& identity,
        double value,
        talas::expr::ValueDomain domain,
        std::size_t frameOffset = 0,
        std::uint64_t expectedStageGeneration = 0) noexcept;
    [[nodiscard]] bool scheduleVoiceVariableAssignment(
        const talas::project::PreparedProjectVariableIdentity& identity,
        std::uint64_t voiceGeneration,
        double value,
        talas::expr::ValueDomain domain,
        std::size_t frameOffset = 0,
        std::uint64_t expectedStageGeneration = 0) noexcept;
    [[nodiscard]] SnapshotDirectTransactionResult
    scheduleVariableAssignments(
        std::span<const ProjectVariableAssignment> assignments,
        std::uint64_t expectedStageGeneration);
    [[nodiscard]] SnapshotDirectTransactionResult resetVariables(
        std::span<const talas::project::PreparedProjectVariableIdentity>
            identities,
        std::uint64_t expectedStageGeneration);
    [[nodiscard]] SnapshotDirectTransactionResult resetAllVariables(
        std::uint64_t expectedStageGeneration);
    [[nodiscard]] bool scheduleSoundVariableAssignment(
        const talas::project::PreparedProjectSoundVariableIdentity& identity,
        std::string_view selectedDefinitionId,
        std::size_t frameOffset = 0,
        std::uint64_t expectedStageGeneration = 0
    ) noexcept;
    [[nodiscard]] bool scheduleVoiceSoundVariableAssignment(
        const talas::project::PreparedProjectSoundVariableIdentity& identity,
        std::uint64_t voiceGeneration,
        std::string_view selectedDefinitionId,
        std::size_t frameOffset = 0,
        std::uint64_t expectedStageGeneration = 0
    ) noexcept;
    [[nodiscard]] SnapshotDirectTransactionResult scheduleSoundVariableAssignments(
        std::span<const ProjectSoundVariableAssignment> assignments,
        std::uint64_t expectedStageGeneration
    );
    [[nodiscard]] SnapshotDirectTransactionResult resetSoundVariables(
        std::span<const talas::project::PreparedProjectSoundVariableIdentity> identities,
        std::uint64_t expectedStageGeneration
    );
    [[nodiscard]] SnapshotDirectTransactionResult
    resetAllSoundVariables(std::uint64_t expectedStageGeneration);
    [[nodiscard]] talas::project::EditResult
    captureProjectVariableInitialValue(
        const talas::project::PreparedProjectVariableIdentity& identity,
        std::uint64_t expectedStageGeneration,
        std::uint64_t expectedDocumentAuthorityGeneration);
    [[nodiscard]] talas::project::EditResult
    captureProjectSoundVariableInitialSelection(
        const talas::project::PreparedProjectSoundVariableIdentity& identity,
        std::uint64_t expectedStageGeneration,
        std::uint64_t expectedDocumentAuthorityGeneration);
    [[nodiscard]] SnapshotDirectTransactionResult
    scheduleSnapshotDirectValues(
        std::span<const talas::project::SnapshotValueEntry> values,
        double smoothingSeconds,
        std::uint64_t expectedStageGeneration = 0);
    [[nodiscard]] SnapshotPerformanceActionResult
    applyPerformanceDirectValues(
        std::span<const talas::project::SnapshotValueEntry> values,
        double smoothingSeconds,
        std::uint64_t expectedStageGeneration);
    [[nodiscard]] SnapshotPerformanceActionResult
    applyPerformanceDirectValues(
        std::span<const talas::project::SnapshotValueEntry> values,
        double smoothingSeconds,
        std::uint64_t expectedStageGeneration,
        talas::project::PreparedVirtualControlValueEdit authoringEdit);
    // Control-thread host route for a terminal GeneratedEvent. The leading
    // '!' names one prepared numeric Event Value; the live update participates
    // in snapshot capture without changing authored Virtual Controls.
    [[nodiscard]] SnapshotPerformanceActionResult
    applyGlobalControllerEvent(
        std::string_view generatedEventName,
        double value,
        std::uint64_t expectedStageGeneration);
    [[nodiscard]] SnapshotPerformanceActionResult
    applyGlobalControllerEvent(
        std::string_view generatedEventName,
        double value,
        bool allowLiveOverride,
        std::uint64_t expectedStageGeneration);
    [[nodiscard]] bool snapshotDirectTargetPrepared(
        const talas::project::SnapshotTargetIdentity& target) const;
    [[nodiscard]] std::shared_ptr<RealtimeAnalysisCapture> latestAnalysisCapture() const noexcept;
    [[nodiscard]] std::shared_ptr<const PreparedScoreSourceAnalysisSet>
    latestScoreSourceAnalysisCaptures() const noexcept;
    [[nodiscard]] double latestAnalysisCaptureSampleRate() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> realtimeEventValueId(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> realtimeEventValueIdForLatestStage(
        std::string_view name,
        talas::expr::ValueDomain domain) const noexcept;
    [[nodiscard]] std::uint64_t latestStageGeneration() const noexcept;
    [[nodiscard]] std::optional<std::uint8_t>
    defaultMidiChannelForLatestStage() const noexcept;
    [[nodiscard]] std::uint64_t latestPreparedStageId() const noexcept;
    [[nodiscard]] bool enqueueEventValue(
        std::uint32_t eventId,
        double value,
        talas::expr::ValueDomain domain,
        std::size_t frameOffset = 0) noexcept;
    [[nodiscard]] bool enqueueEventValueAtTime(
        std::uint32_t eventId,
        double value,
        talas::expr::ValueDomain domain,
        double arrivalTimeSeconds) noexcept;
    [[nodiscard]] bool enqueueEventValueAtTimeForStage(
        std::uint32_t eventId,
        double value,
        talas::expr::ValueDomain domain,
        double arrivalTimeSeconds,
        std::uint64_t expectedStageGeneration,
        ControllerRecordingIngressEvidence recording = {}) noexcept;
    [[nodiscard]] bool enqueueEventValueAtSteadyTimeForStage(
        std::uint32_t eventId,
        double value,
        talas::expr::ValueDomain domain,
        std::int64_t targetTimeNanoseconds,
        std::uint64_t expectedStageGeneration,
        ControllerRecordingIngressEvidence recording = {}) noexcept;
    [[nodiscard]] bool enqueueControllerRecordingObservationAtTimeForStage(
        std::uint32_t eventId,
        double value,
        talas::expr::ValueDomain domain,
        std::int64_t sourceTimestampNanoseconds,
        std::uint64_t expectedStageGeneration,
        ControllerRecordingIngressEvidence recording) noexcept;
    [[nodiscard]] ControllerRecordingArmResult beginControllerRecording(
        std::span<const talas::project::ControllerRecordingTarget> targets,
        std::optional<std::uint64_t> punchStartFrame = {},
        std::optional<std::uint64_t> punchEndFrame = {});
    void stopControllerRecording(
        talas::project::ControllerRecordingTerminal terminal =
            talas::project::ControllerRecordingTerminal::Completed) noexcept;
#if defined(TALAS_CONTROLLER_RECORDING_TEST_SUPPORT)
    void setControllerRecordingStopAfterLoadHookForTests(
        std::function<void()> hook);
#endif
    [[nodiscard]] bool controllerRecordingActive() const noexcept;
    [[nodiscard]] std::optional<talas::project::ControllerRecordingTake>
    collectControllerRecordingTake();
    [[nodiscard]] bool enqueueMidiNoteOn(
        int channel,
        int note,
        int velocity,
        double arrivalTimeSeconds) noexcept;
    [[nodiscard]] bool enqueueMidiNoteOff(
        int channel,
        int note,
        double arrivalTimeSeconds) noexcept;
    [[nodiscard]] bool enqueueMidiControlChange(
        int channel,
        int controller,
        int value,
        double arrivalTimeSeconds,
        std::uint64_t sourceGeneration = 0) noexcept;
    [[nodiscard]] bool enqueueMidiProgramChange(
        int channel,
        int program,
        double arrivalTimeSeconds) noexcept;
    [[nodiscard]] bool enqueueMidiChannelPressure(
        int channel,
        int pressure,
        double arrivalTimeSeconds) noexcept;
    [[nodiscard]] bool enqueueMidiPitchBend(
        int channel,
        int value14,
        double arrivalTimeSeconds) noexcept;
    // Control/message-thread authority publication. Generation zero restores
    // Internal transport. Every change stops and drains the prior authority at
    // the next callback boundary.
    void configureMidiClockAuthority(
        std::uint64_t authorityGeneration,
        std::uint8_t sourceSlot = 0,
        std::uint64_t sourceGeneration = 0) noexcept;
    // Registry/device loss is distinct from an explicit owner switch back to
    // Internal transport and remains observable as monotonic telemetry.
    void revokeMidiClockAuthorityForSourceLoss() noexcept;
    [[nodiscard]] bool enqueueMidiClock(
        RealtimeControlCommandKind kind,
        std::uint8_t sourceSlot,
        std::uint64_t sourceGeneration,
        std::uint64_t authorityGeneration,
        double arrivalTimeSeconds,
        std::uint16_t songPosition = 0) noexcept;
    [[nodiscard]] MidiClockTelemetrySnapshot
    midiClockTelemetry() const noexcept;
    // A MIDI timing clock message (Tick, Start,
    // Continue or Stop) from any MIDI input, with or without a clock
    // authority, for the Sound's `MIDITimingClock and !TimingClock: each
    // Tick while the clock runs counts one more in
    // talas::expr::midiTimingClockCountEventName at its arrival. Stop holds the
    // clock until Start or Continue. MIDI-thread producer; false when the
    // staged Sound reads no MIDI timing clock or the message is not counted.
    [[nodiscard]] bool enqueueMidiTimingClock(
        RealtimeControlCommandKind kind,
        double arrivalTimeSeconds) noexcept;
    void configureMtcAuthority(
        std::uint64_t authorityGeneration,
        std::uint8_t sourceSlot = 0,
        std::uint64_t sourceGeneration = 0) noexcept;
    void revokeMtcAuthorityForSourceLoss() noexcept;
    [[nodiscard]] bool enqueueMtc(
        std::span<const std::uint8_t> bytes,
        std::uint8_t sourceSlot,
        std::uint64_t sourceGeneration,
        std::uint64_t authorityGeneration,
        double arrivalTimeSeconds) noexcept;
    [[nodiscard]] MtcTelemetrySnapshot mtcTelemetry() const noexcept;
    // Asynchronous reset request. The audio thread adopts it at callback entry; an already claimed
    // command may complete concurrently, while queued commands from the old generation are discarded.
    void resetRealtimeControlTiming() noexcept;
    [[nodiscard]] std::uint64_t rejectedRealtimeControlCount() const noexcept;
    [[nodiscard]] RealtimeControlTelemetrySnapshot
    realtimeControlTelemetry() const noexcept;
    // Control-thread projection. A newly adopted graph begins a new metrics
    // epoch and returns zero deltas on its first observation.
    [[nodiscard]] RealtimeMetricsSnapshotV1 realtimeMetrics() noexcept;
    void resetRealtimeMetrics() noexcept;
    [[nodiscard]] std::uint64_t activeStageId() const noexcept;
    [[nodiscard]] std::size_t activeOutputChannelCount() const noexcept;
    [[nodiscard]] RealtimeStreamHealthSnapshot streamHealth() const;
    [[nodiscard]] PreparedNodeTimingSnapshot preparedNodeTiming() const;
    [[nodiscard]] PreparedVariableValueSnapshot preparedVariableValues() const;
    [[nodiscard]] PreparedSoundVariableValueSnapshot preparedSoundVariableValues() const;
    [[nodiscard]] PreparedGraphStatusSnapshot
    preparedGraphStatus(
        std::span<const PreparedGraphStatusSnapshot::OutputDeviceRoute>
            outputDeviceRoutes = {}) const;
    [[nodiscard]] bool collectRetired() noexcept;

    // Audio-thread method.
    [[nodiscard]] bool render(float* const* outputChannels, std::size_t channelCount, std::size_t frameCount) noexcept;
    [[nodiscard]] bool render(
        RealtimeAudioInputBlock input,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount) noexcept;
    [[nodiscard]] bool render(
        RealtimeAudioInputBlock input,
        double callbackTimeSeconds,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount) noexcept;
    [[nodiscard]] bool renderOrdered(
        RealtimeAudioInputBlock input,
        double callbackTimeSeconds,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount) noexcept;
    [[nodiscard]] RealtimeTerminalRenderResult renderTerminalEvents(
        std::size_t frameCount,
        RealtimeTerminalOutputBlock& output) noexcept;
    [[nodiscard]] RealtimeTerminalRenderResult renderTerminalEvents(
        double callbackTimeSeconds,
        std::size_t frameCount,
        RealtimeTerminalOutputBlock& output) noexcept;

    // On the audio thread, after each part of a
    // render, what each DiskRecorder within the playing Sound recorded.
    // Set it before audio starts, or while no render runs.
    using DiskRecorderTapSink = void (*)(void* context, std::size_t tap, float* const* channels,
        std::size_t channelCount, std::size_t frameCount) noexcept;
    void setDiskRecorderTapSink(void* context, DiskRecorderTapSink sink) noexcept;

    // Call only after the audio device has stopped invoking render.
    void clearAfterAudioStopped() noexcept;

private:
    friend class ProjectRuntimeBridgeTestAccess;
    [[nodiscard]] ProjectStageResult compileAndStageDocumentImpl(
        const talas::project::ProjectDocument& document,
        RenderSettings settings,
        std::shared_ptr<PreparedIsolatedComponentBinding> binding,
        std::uint64_t replacementDocumentGeneration = 0,
        std::string_view replacementNodeId = {},
        const ComponentApprovalFingerprint* replacementCandidate = nullptr,
        component_bundle::ComponentBundleReplacementReceipt*
            replacementReceipt = nullptr,
        const std::array<std::uint8_t, componentTrustDigestBytes>*
            replacementPreservedDigest = nullptr,
        bool preserveVariableValueHistory = false);
    static constexpr std::size_t maxRegisteredEventValues = 1024;

    struct PreparedSnapshotOverlayStage
    {
        std::unique_ptr<RealtimeAudioEngine> engine;
        std::unordered_map<std::string, SnapshotPreparedTarget>
            preparedTargets;
        std::shared_ptr<RealtimeAnalysisCapture> analysisCapture;
        std::shared_ptr<const PreparedScoreSourceAnalysisSet> scoreSourceAnalysis;
        std::shared_ptr<const PreparedGraphSummary> preparedSummary;
        talas::project::PreparedProjectVariableTable preparedVariables;
        talas::project::PreparedProjectSoundVariableTable preparedSoundVariables;
        std::uint64_t stageId = 0;
        std::uint64_t stageGeneration = 0;
        std::uint64_t transitionFrames = 0;
        double sampleRate = 0.0;
        std::size_t outputChannelCount = 0;
    };

    void drainRealtimeControls(
        RealtimeAudioEngine& renderTarget,
        std::size_t frameCount,
        double callbackTimeSeconds) noexcept;
    void mirrorDrainedRealtimeControls(
        RealtimeAudioEngine& renderTarget) noexcept;
    void adoptControllerRecordingRequest(
        RealtimeAudioEngine& renderTarget) noexcept;
    void completeControllerRecordingFromAudio(
        talas::project::ControllerRecordingTerminal terminal,
        std::uint64_t endFrame) noexcept;
    [[nodiscard]] std::uint64_t requestedControllerRecordingTakeGeneration()
        const noexcept;
    void captureControllerRecordingEvent(
        const RealtimeControlCommand& command,
        std::uint64_t absoluteFrame,
        std::uint64_t stageGeneration) noexcept;
    void publishInternalTransportRequest(bool playing) noexcept;
    void drainMidiClockTransport(
        double callbackTimeSeconds,
        std::size_t frameCount) noexcept;
    void drainMtcTransport(
        double callbackTimeSeconds,
        std::size_t frameCount) noexcept;
    void drainRealtimeLiveSetRequests(const PreparedLiveSet& prepared,
                                      RealtimeTransportSnapshot transport) noexcept;
    void drainRealtimeLiveToolTriggers(
        RealtimeAudioEngine& renderTarget,
        const PreparedLiveSet& liveSet,
        const PreparedLiveTools& tools) noexcept;
    void enqueueLiveToolEventTriggersFromAudio(
        RealtimeAudioEngine& renderTarget,
        std::string_view eventName,
        talas::expr::ValueDomain domain) noexcept;
    void publishLiveSetTerminal(
        std::uint64_t requestGeneration,
        RealtimeLiveSetTerminalOutcome outcome) noexcept;
    void publishLiveSetActiveState(
        std::uint64_t requestGeneration,
        std::uint32_t cellIndexPlusOne,
        bool scoreRegionActive) noexcept;
    [[nodiscard]] bool applyRealtimeLiveSetRequest(const PreparedLiveSet& prepared,
                                                   const RealtimeLiveSetRequest& request) noexcept;
    void publishPendingLiveSetActivationOutcome() noexcept;
    [[nodiscard]] bool readLiveSetSourceAuthority(std::size_t sourceIndex,
                                                  std::uint64_t& sourceGeneration,
                                                  std::uint64_t& routeGeneration) const noexcept;
    [[nodiscard]] bool renderWithOutputMapping(
        RealtimeAudioInputBlock input,
        double callbackTimeSeconds,
        float* const* outputChannels,
        std::size_t channelCount,
        std::size_t frameCount,
        bool orderedOutputs) noexcept;
    void publishAcceptedSnapshotOverlay(
        PreparedSnapshotOverlayStage stage);
    [[nodiscard]] bool serviceCoalescedSnapshotOverlay() noexcept;
    [[nodiscard]] SnapshotPerformanceActionResult
    publishSnapshotPerformanceValues(
        const talas::project::ProjectSnapshotBank& bank,
        std::vector<talas::project::SnapshotValueEntry> values,
        const SnapshotPerformanceState& candidateState);
    [[nodiscard]] bool resolveSnapshotPerformanceValues(
        const talas::project::ProjectSnapshotBank& bank,
        const talas::project::ProjectSnapshot& snapshot,
        std::span<const talas::project::SnapshotValueEntry> currentValues,
        std::vector<talas::project::SnapshotValueEntry>& values,
        SnapshotPerformanceActionResult& failure) const;
    [[nodiscard]] bool snapshotPerformanceDocumentIsCurrent(
        SnapshotPerformanceActionResult& failure) const;
    [[nodiscard]] bool authoritativeSnapshotPerformanceValues(
        std::vector<talas::project::SnapshotValueEntry>& values,
        SnapshotPerformanceActionResult& failure) const;
    [[nodiscard]] RealtimeMetricsSnapshotV1
    rawRealtimeMetrics() const noexcept;
    [[nodiscard]] bool prepareSnapshotPerformanceState(
        std::span<const talas::project::SnapshotValueEntry> values,
        SnapshotPerformanceState& candidate,
        SnapshotPerformanceActionResult& failure) const;
    [[nodiscard]] SnapshotPerformanceActionResult
    applyPerformanceDirectValuesImpl(
        std::span<const talas::project::SnapshotValueEntry> values,
        double smoothingSeconds,
        std::uint64_t expectedStageGeneration,
        talas::project::PreparedVirtualControlValueEdit* authoringEdit,
        bool generatedControllerSource);
    [[nodiscard]] bool globalControllerBlocksLiveOverride(
        const talas::project::SnapshotTargetIdentity& target) const noexcept;

    talas::project::ProjectEditor editor_;
    void publishDiskRecorderTaps(const RealtimeAudioEngine& engine) noexcept;
    std::atomic<void*> diskRecorderTapSinkContext_ = nullptr;
    std::atomic<DiskRecorderTapSink> diskRecorderTapSink_ = nullptr;
    RealtimeGraphHost graphHost_;
    const talas::core::NodeRegistry& registry_;
    RealtimeControlIngress controlIngress_;
    // Render-target event queues accept one producer at a time. Control
    // threads wait for this gate; the audio thread only tries it and defers
    // its producer work to the next callback when a control thread holds it.
    std::atomic_flag sessionProducerGate_;
    // Frames ahead of the next callback that a replacement staged during
    // playback is first prepared for. Tests use it to hold one pending.
    std::uint64_t initialStageLeadFrames_ = 0;
    // ADR-0005: a terminal callback split at a pending stage's prepared
    // frame renders each side here before publishing either. Allocated on
    // the control thread before the first terminal-only stage, at the
    // largest per-callback event count a session accepts.
    struct TerminalSegmentScratch
    {
        std::vector<RealtimeMidiOutputEvent> midiEvents;
        std::vector<RealtimeGlobalControllerOutputEvent> globalControllerEvents;
        std::vector<char> globalControllerName;
    };
    std::array<TerminalSegmentScratch, 2> terminalSegmentScratch_;
    std::atomic<bool> terminalSegmentScratchReady_{false};
    std::atomic<std::uint64_t> deferredRealtimeControlDrainCount_{0};
    RealtimeControlIngress midiClockIngress_;
    MtcRealtimeIngress mtcIngress_;
    RealtimeLiveSetIngress liveSetIngress_;
    RealtimeLiveSetScheduler liveSetScheduler_;
    RealtimeLiveToolIngress liveToolIngress_;
    std::array<std::atomic<const std::string*>, maxRegisteredEventValues> registeredEventValueNames_{};
    std::array<std::atomic<talas::expr::ValueDomain>, maxRegisteredEventValues> registeredEventValueDomains_{};
    std::array<std::atomic<std::uint64_t>, maxRegisteredEventValues> registeredEventValueStageGenerations_{};
    // The editor's documentReplacements() when each registered name was last
    // staged. A name keeps its domain while that project stays open; once a
    // project is put in its place (New, Open, a demo, recovery), its next
    // accepted stage may give the name another domain. Control thread only.
    std::array<std::uint64_t, maxRegisteredEventValues> registeredEventValueDocumentReplacements_{};
    std::vector<std::unique_ptr<std::string>> ownedEventValueNames_;
    talas::project::PreparedProjectVariableTable latestPreparedVariables_;
    std::uint64_t latestPreparedVariableStageGeneration_ = 0;
    talas::project::PreparedProjectSoundVariableTable latestPreparedSoundVariables_;
    std::uint64_t latestPreparedSoundVariableStageGeneration_ = 0;
    bool latestVariableStageIsEditorDocument_ = false;
    std::vector<RealtimeControlCommand> controlDrainScratch_;
    std::vector<RealtimeControlCommand> scheduledControlPending_;
    std::vector<std::uint64_t> scheduledControlSequenceScratch_;
    std::vector<PreparedTypedEvent> typedEventScratch_;
    std::vector<RealtimeTypedEventDispatchResult>
        typedEventDispatchScratch_;
    std::vector<RealtimeControlCommand> typedEventCommandScratch_;
    std::unique_ptr<talas::project::ControllerRecordingEvent[]>
        controllerRecordingEvents_;
    std::array<std::uint32_t,
        talas::project::maximumControllerRecordingTargets>
        controllerRecordingTargetEventIds_{};
    std::size_t controllerRecordingTargetCount_ = 0;
    talas::project::ControllerRecordingTake controllerRecordingPendingTake_;
    std::uint64_t nextControllerRecordingTakeGeneration_ = 1;
    std::uint64_t audioControllerRecordingRequestGeneration_ = 0;
    std::uint64_t audioControllerRecordingTakeGeneration_ = 0;
    std::uint64_t audioControllerRecordingStageGeneration_ = 0;
    std::uint64_t audioControllerRecordingPunchStartFrame_ = 0;
    std::uint64_t audioControllerRecordingPunchEndFrame_ =
        std::numeric_limits<std::uint64_t>::max();
    std::size_t audioControllerRecordingEventCount_ = 0;
    bool audioControllerRecordingActive_ = false;
    std::atomic<std::uint64_t> controllerRecordingRequestGeneration_ = 0;
    std::atomic<bool> controllerRecordingRequestedActive_ = false;
    std::atomic<std::uint64_t> controllerRecordingTerminalState_ = 1U << 1U;
    std::atomic<std::uint64_t> controllerRecordingConfiguredTakeGeneration_ = 0;
    std::atomic<std::uint64_t> controllerRecordingConfiguredStageGeneration_ = 0;
    std::atomic<std::uint64_t> controllerRecordingConfiguredPunchStartFrame_ = 0;
    std::atomic<std::uint64_t> controllerRecordingConfiguredPunchEndFrame_ =
        std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> controllerRecordingSeekEndFrame_ = 0;
    std::atomic<std::uint64_t> controllerRecordingCompletedTakeGeneration_ = 0;
    std::atomic<std::size_t> controllerRecordingCompletedEventCount_ = 0;
    std::atomic<std::uint64_t> controllerRecordingCompletedEndFrame_ = 0;
    std::atomic<talas::project::ControllerRecordingTerminal>
        controllerRecordingCompletedTerminal_ =
            talas::project::ControllerRecordingTerminal::Cancelled;
#if defined(TALAS_CONTROLLER_RECORDING_TEST_SUPPORT)
    std::function<void()> controllerRecordingStopAfterLoadHookForTests_;
#endif
    std::atomic<std::uint64_t> rejectedRealtimeControls_ = 0;
    std::atomic<std::uint64_t> dspRealtimeControlRejectionCount_ = 0;
    std::atomic<std::uint64_t> preparedTypedEventCount_ = 0;
    std::atomic<std::uint64_t> realtimeControlQueueOverflowCount_ = 0;
    std::atomic<std::uint64_t> staleRealtimeControlCount_ = 0;
    std::atomic<std::uint64_t> clampedRealtimeControlTimingCount_ = 0;
    std::atomic<std::uint64_t> invalidRealtimeControlCount_ = 0;
    std::atomic<std::uint64_t> typedEventBatchOverflowCount_ = 0;
    std::atomic<std::uint64_t> scheduledRealtimeControlAcceptedCount_ = 0;
    std::atomic<std::uint64_t> scheduledRealtimeControlAppliedCount_ = 0;
    std::atomic<std::uint64_t> scheduledRealtimeControlLateCount_ = 0;
    std::atomic<std::uint64_t> scheduledRealtimeControlPendingFullCount_ = 0;
    std::atomic<std::uint64_t> scheduledRealtimeControlStaleCount_ = 0;
    std::atomic<std::uint64_t> scheduledRealtimeControlPendingCount_ = 0;
    std::atomic<std::uint64_t> scheduledRealtimeControlPendingHighWatermark_ = 0;
    std::atomic<std::uint64_t>
        preparedSnapshotDirectTransactionCount_ = 0;
    std::atomic<std::uint64_t> realtimeControlGeneration_ = 1;
    std::atomic<std::uint64_t> liveSetRequestGeneration_ = 0;
    // Single control-writer, double-slot publication. Audio and request
    // readers use fixed-pass stable snapshots so source and route cannot mix.
    std::array<RealtimeLiveSetSourceAuthorityPublication, 5>
        liveSetSourceAuthorities_{};
    std::array<RealtimeLiveToolSourceAuthorityPublication,
               realtimeLiveToolSourceKindCount>
        liveToolSourceAuthorities_{};
    std::atomic<std::uint64_t> liveSetAcceptedRequestCount_ = 0;
    std::atomic<std::uint64_t> liveSetCompletedActivationCount_ = 0;
    std::atomic<std::uint64_t> liveSetQueueOverflowCount_ = 0;
    std::atomic<std::uint64_t> liveSetStaleAuthorityCount_ = 0;
    std::atomic<std::uint64_t> liveSetFailedActivationCount_ = 0;
    RealtimeLiveSetActiveStatePublication liveSetActiveState_;
    RealtimeLiveSetTerminalStatePublication liveSetTerminalState_;
    std::atomic<bool> liveSetScoreRegionActive_ = false;
    std::uint64_t audioLiveSetScoreEndFrame_ = 0;
    std::uint64_t audioLiveSetScoreStageId_ = 0;
    std::uint64_t audioLiveSetScoreDiscontinuitySequence_ = 0;
    std::uint64_t audioLiveSetStageId_ = 0;
    std::uint64_t audioLiveSetPendingRequestGeneration_ = 0;
    std::uint32_t audioLiveSetPendingCellIndexPlusOne_ = 0;
    PreparedLiveToolRuntimeState audioLiveToolState_;
    PreparedLiveToolRuntimeStatePublication liveToolStatePublication_;
    PreparedLiveToolRuntimeStatePublication liveToolRestorePublication_;
    std::uint64_t audioLiveToolStageId_ = 0;
    std::uint64_t audioLiveToolLastPeriodicTick_ = 0;
    std::uint64_t audioLiveToolPeriodicDiscontinuity_ = 0;
    bool audioLiveToolHasPeriodicTick_ = false;
    std::atomic<std::uint64_t> liveToolRequestGeneration_ = 0;
    std::atomic<std::uint64_t> liveToolRestoreRequestGeneration_ = 0;
    std::atomic<std::uint64_t> liveToolAppliedRestoreGeneration_ = 0;
    std::uint64_t audioLiveToolRestoreRequestGeneration_ = 0;
    std::atomic<std::uint64_t> liveToolAcceptedTriggerCount_ = 0;
    std::atomic<std::uint64_t> liveToolAppliedTransitionCount_ = 0;
    std::atomic<std::uint64_t> liveToolQueueOverflowCount_ = 0;
    std::atomic<std::uint64_t> liveToolStaleAuthorityCount_ = 0;
    std::atomic<std::uint64_t> liveToolRejectedTriggerCount_ = 0;
    std::atomic<std::uint64_t> liveToolNoMatchingTransitionCount_ = 0;
    std::atomic<std::uint64_t> liveToolStaleTriggerAuthorityCount_ = 0;
    std::atomic<std::uint64_t> liveToolInvalidTriggerCount_ = 0;
    std::atomic<std::uint64_t> liveToolGuardRejectedCount_ = 0;
    std::atomic<std::uint64_t> liveToolCommandRejectedCount_ = 0;
    std::atomic<std::uint64_t> liveToolStateInitializationRejectedCount_ = 0;
    std::atomic<std::uint64_t> liveToolActivateCellCommandCount_ = 0;
    std::atomic<std::uint64_t> liveToolStopCommandCount_ = 0;
    std::atomic<std::uint64_t> liveToolSetProjectVariableCommandCount_ = 0;
    std::atomic<std::uint64_t> liveToolSetWorkspaceVariableCommandCount_ = 0;
    std::atomic<std::uint64_t> liveToolSetEventValueCommandCount_ = 0;
    std::atomic<std::uint64_t> midiClockAuthorityGeneration_ = 0;
    std::atomic<std::uint8_t> midiClockExpectedSourceSlot_ = 0;
    std::atomic<std::uint64_t> midiClockExpectedSourceGeneration_ = 0;
    std::atomic<std::uint64_t> midiClockStopRequestGeneration_ = 0;
    // Packed control-to-audio request: bit zero is the desired Internal
    // playing state and the remaining bits form a monotonic publication token.
    std::atomic<std::uint64_t> internalTransportRequest_ = 0;
    std::atomic<std::uint64_t> internalTransportResetRequest_ = 0;
    struct TransportRelocationRequest;
    std::atomic<TransportRelocationRequest*> pendingTransportRelocation_ = nullptr;
    std::array<std::atomic<TransportRelocationRequest*>, 2> retiredTransportRelocations_{};
    [[nodiscard]] bool collectRetiredTransportRelocations() noexcept;
    void adoptInternalTransportRelocation() noexcept;
    std::atomic<std::uint64_t> internalTransportRelocationGeneration_ = 0;
    std::atomic<std::uint64_t> midiClockAcceptedTickCount_ = 0;
    // The MIDI timing clock's messages counted for `MIDITimingClock and
    // !TimingClock, and whether a Stop holds it.
    std::atomic<std::uint64_t> midiTimingClockMessageCount_ = 0;
    std::atomic<bool> midiTimingClockStopped_ = false;
    std::atomic<std::uint64_t> midiClockAcceptedStartCount_ = 0;
    std::atomic<std::uint64_t> midiClockAcceptedContinueCount_ = 0;
    std::atomic<std::uint64_t> midiClockAcceptedStopCount_ = 0;
    std::atomic<std::uint64_t>
        midiClockSongPositionConversionFailureCount_ = 0;
    std::atomic<std::uint64_t> midiClockQueueOverflowCount_ = 0;
    std::atomic<std::uint64_t> midiClockStaleAuthorityCount_ = 0;
    std::atomic<std::uint64_t> midiClockWrongSourceCount_ = 0;
    std::atomic<std::uint64_t> midiClockInvalidRecordCount_ = 0;
    std::atomic<std::uint64_t> midiClockUnsupportedSongPositionCount_ = 0;
    std::atomic<std::uint64_t> midiClockSignalTimeoutCount_ = 0;
    std::atomic<std::uint64_t> midiClockSourceLossCount_ = 0;
    std::atomic<std::uint64_t> midiClockTempoAcquisitionCount_ = 0;
    std::atomic<std::uint64_t> midiClockTempoReacquisitionCount_ = 0;
    std::atomic<std::uint64_t> midiClockTempoRangeRejectionCount_ = 0;
    std::atomic<std::uint64_t> midiClockCadenceDiscontinuityCount_ = 0;
    std::atomic<std::uint64_t> midiClockTimestampRejectionCount_ = 0;
    std::atomic<std::uint64_t> midiClockEstimatorOverflowCount_ = 0;
    std::atomic<std::uint64_t> midiClockHoldoverEntryCount_ = 0;
    mutable std::atomic<std::uint64_t>
        midiClockPublicationValidationFailureCount_ = 0;
    std::atomic<double> midiClockLastTickTimeSeconds_ = 0.0;
    std::atomic<std::uint64_t>
        midiClockAdoptedAuthorityGeneration_ = 0;
    std::atomic<bool> midiClockRunning_ = false;
    std::atomic<bool> midiClockSignalStale_ = false;
    std::atomic<std::uint64_t> mtcAuthorityGeneration_ = 0;
    std::atomic<std::uint8_t> mtcExpectedSourceSlot_ = 0;
    std::atomic<std::uint64_t> mtcExpectedSourceGeneration_ = 0;
    std::atomic<std::uint64_t> mtcStopRequestGeneration_ = 0;
    std::atomic<std::uint64_t> mtcEvidenceRejectionGeneration_ = 0;
    std::atomic<std::uint64_t> mtcSourceLossCount_ = 0;
    std::atomic<std::uint64_t> mtcQueueOverflowCount_ = 0;
    std::atomic<std::uint64_t> mtcStaleAuthorityCount_ = 0;
    std::atomic<std::uint64_t> mtcWrongSourceCount_ = 0;
    std::atomic<std::uint64_t> mtcInvalidRecordCount_ = 0;
    std::atomic<std::uint64_t> mtcMappingFailureCount_ = 0;
    std::atomic<std::uint64_t> mtcAcceptedQuarterFrameCount_ = 0;
    std::atomic<std::uint64_t> mtcAcceptedCompleteSetCount_ = 0;
    std::atomic<std::uint64_t> mtcAcceptedFullFrameLocateCount_ = 0;
    std::atomic<std::uint64_t> mtcAcquisitionCount_ = 0;
    std::atomic<std::uint64_t> mtcReacquisitionCount_ = 0;
    std::atomic<std::uint64_t> mtcStaleTimeoutCount_ = 0;
    std::atomic<std::uint64_t> mtcReorderedPieceCount_ = 0;
    std::atomic<std::uint64_t> mtcFollowerIdentityRejectionCount_ = 0;
    std::atomic<std::uint64_t> mtcTimestampRejectionCount_ = 0;
    std::atomic<std::uint64_t> mtcFollowerInvalidMessageCount_ = 0;
    std::atomic<std::uint64_t> mtcInvalidLabelOrRateCount_ = 0;
    std::atomic<std::uint64_t> mtcUnsupportedDirectionCount_ = 0;
    std::atomic<std::uint64_t> mtcArithmeticOverflowCount_ = 0;
    std::atomic<std::uint64_t> mtcDiscontinuityCount_ = 0;
    mutable std::atomic<std::uint64_t>
        mtcPublicationValidationFailureCount_ = 0;
    std::atomic<std::uint64_t> latestStageGeneration_ = 0;
    RealtimeMetricsSnapshotV1 realtimeMetricsBaseline_;
    RealtimeMetricsSnapshotV1 realtimeMetricsLastRaw_;
    std::uint64_t realtimeMetricsEpoch_ = 1;
    std::atomic<std::uint64_t> acceptedMidiStageId_ = 0;
    std::atomic<std::uint8_t> acceptedDefaultMidiChannel_ = 1;
    // The staged Timeline's time code offset, the
    // time code its start follows.
    std::atomic<double> acceptedTimeCodeOffsetSeconds_ = 0.0;
    // ADR-0024 and its 2026-10-19 amendment: the global map's associations
    // the accepted stage's MIDI also sets, each written before the stage
    // generation that a controller callback checks is published.
    struct AcceptedGlobalMapEntry
    {
        std::atomic<std::uint32_t> eventId = 0;
        std::atomic<std::uint8_t> controller = 0;
        // 0: the default MIDI channel.
        std::atomic<std::uint8_t> channel = 0;
        std::atomic<bool> pitchBend = false;
        std::atomic<bool> logTaper = false;
        std::atomic<double> minimum = 0.0;
        std::atomic<double> maximum = 1.0;
        std::atomic<double> grid = 0.0;
    };
    static constexpr std::size_t maximumAcceptedGlobalMapEntries = 128;
    std::array<AcceptedGlobalMapEntry, maximumAcceptedGlobalMapEntries>
        acceptedGlobalMap_{};
    std::atomic<std::uint32_t> acceptedGlobalMapCount_ = 0;
    [[nodiscard]] bool enqueueGlobalMapValues(
        bool pitchBend,
        int controller,
        int channel,
        double normalized,
        double arrivalTimeSeconds,
        std::uint64_t stageGeneration,
        const ControllerRecordingIngressEvidence& recording,
        bool& anyMatched) noexcept;
    std::atomic<std::uint64_t> acceptedMidiControllerStageGeneration_ = 0;
    std::uint64_t nextStageGeneration_ = 1;
    std::uint64_t nextEngineStageId_ = 1;
    std::shared_ptr<RealtimeAnalysisCapture> latestAnalysisCapture_;
    std::shared_ptr<const PreparedScoreSourceAnalysisSet> latestScoreSourceAnalysis_;
    std::uint64_t latestAnalysisCaptureStageId_ = 0;
    std::shared_ptr<const PreparedGraphSummary>
        latestPreparedGraphSummary_;
    std::shared_ptr<const PreparedScoreSchedule>
        latestPreparedScoreSchedule_;
    // Control/source-thread publication. The audio callback never touches this
    // shared owner; its engine retains the same immutable PreparedLiveSet.
    std::shared_ptr<const PreparedLiveSet> latestPreparedLiveSet_;
    std::unordered_map<std::string, talas::project::ProjectDocument>
        compiledLiveSetSounds_;
    std::shared_ptr<const PreparedLiveTools> latestPreparedLiveTools_;
    std::uint64_t audioControlGeneration_ = 1;
    std::uint64_t audioMidiClockAuthorityGeneration_ = 0;
    std::uint64_t audioMidiClockStageId_ = 0;
    std::uint64_t adoptedMidiClockStopRequestGeneration_ = 0;
    std::uint64_t adoptedInternalTransportRequest_ = 0;
    std::uint64_t adoptedInternalTransportResetRequest_ = 0;
    double audioMidiClockLastSignalTimeSeconds_ = 0.0;
    double audioMidiClockAwaitingFirstPulseSince_ = 0.0;
    bool audioMidiClockRunning_ = false;
    MidiClockPhaseFollower audioMidiClockPhaseFollower_;
    std::uint64_t audioMidiClockMusicalTickQ24_ = 0;
    std::uint64_t audioMidiClockPulseTargetQ24_ = 0;
    bool audioMidiClockMusicalPhaseAnchored_ = false;
    MidiClockAnchorKind audioMidiClockLastAnchor_ =
        MidiClockAnchorKind::None;
    MtcTransportFollower audioMtcFollower_;
    std::uint64_t audioMtcAuthorityGeneration_ = 0;
    std::uint64_t audioMtcStageId_ = 0;
    std::uint64_t adoptedMtcStopRequestGeneration_ = 0;
    std::uint64_t adoptedMtcEvidenceRejectionGeneration_ = 0;
    std::uint64_t audioMtcMappedProjectFrame_ = 0;
    double audioMtcSoundStartTimeSeconds_ = 0.0;
    std::uint64_t audioMidiStageId_ = 0;
    int activeMidiChannel_ = 0;
    int activeMidiNote_ = -1;
    double lastRealtimeCallbackTimeSeconds_ = 0.0;
    std::unordered_map<std::string, AcceptedResourceIdentity>
        acceptedResourceIdentities_;
    std::unordered_map<std::string, SnapshotPreparedTarget>
        latestSnapshotPreparedTargets_;
    std::unordered_map<std::string, std::uint64_t>
        exclusiveGlobalControllerStageByName_;
    std::vector<SnapshotDirectEventValueUpdate>
        snapshotDirectUpdateScratch_;
    std::vector<SnapshotDirectVariableUpdate>
        snapshotDirectVariableUpdateScratch_;
    std::uint64_t latestSnapshotStageId_ = 0;
    std::uint64_t latestSnapshotStageGeneration_ = 0;
    double latestSnapshotSampleRate_ = 0.0;
    std::optional<RenderSettings> projectSnapshotRenderSettings_;
    std::optional<SnapshotExecutionTopologyFingerprint>
        latestSnapshotExecutionTopology_;
    std::optional<SnapshotExecutionTopologyFingerprint>
        latestSnapshotAuthoringTopology_;
    std::unique_ptr<PreparedSnapshotOverlayStage>
        coalescedSnapshotOverlay_;
    std::uint64_t observedSnapshotTransitionCompletionToken_ = 0;
    SnapshotPerformanceState snapshotPerformanceState_;
    std::vector<talas::project::SnapshotValueEntry>
        projectSnapshotBaselineValues_;
    std::string stagedSnapshotAuthoringBytes_;
    std::uint64_t nextSnapshotProjectRevision_ = 1;
};
}
