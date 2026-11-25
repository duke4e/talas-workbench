#pragma once

#include "talas/runtime/Runtime.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace talas::runtime
{
struct ConformanceTolerance
{
    double absolute = 1.0e-12;
    double relative = 1.0e-12;
};

struct ConformanceMismatch
{
    std::size_t channel = 0;
    std::size_t frame = 0;
    double offline = 0.0;
    double realtime = 0.0;
    double allowedError = 0.0;
    // The legacy numeric fields hold expected/actual values for reference
    // comparisons, and offline/realtime values for the default comparison.
    std::string comparison = "offline/realtime";
};

enum class ConformanceMidiNoteKind { NoteOn, NoteOff };
enum class ConformanceZeroFramePolicy { RequireSuccess, RequireRejection };

struct ConformanceMidiNote
{
    std::size_t frame = 0;
    ConformanceMidiNoteKind kind = ConformanceMidiNoteKind::NoteOff;
    std::uint8_t channel = 1;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
};

struct ConformanceVariableAssignment
{
    std::size_t frame = 0;
    talas::project::PreparedProjectVariableIdentity target;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    // Only the explicit ordered input stream admits voice-owned assignments.
    // Their declared slot generation is checked when the transaction executes.
    std::uint64_t voiceGeneration = 0;
};

struct ConformanceSoundVariableAssignment
{
    std::size_t frame = 0;
    talas::project::PreparedProjectSoundVariableIdentity target;
    std::string selectedDefinitionId;
    // Zero for non-voice cells; voice cells use the transaction's final slot
    // generation, as required by the native typed-event execution contract.
    std::uint64_t voiceGeneration = 0;
};

struct ConformanceEventValueAssignment
{
    std::size_t frame = 0;
    std::string name;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    RuntimeScopeId scopeId = globalRuntimeScope;
};

using ConformanceInputEvent = std::variant<ConformanceMidiNote, ConformanceVariableAssignment,
    ConformanceSoundVariableAssignment, ConformanceEventValueAssignment>;

struct ConformanceTerminalCapacityProbe
{
    // Index in the realtime partition, including empty entries. Repeated
    // indices request successive failed attempts before the real callback.
    std::size_t blockIndex = 0;
    std::optional<std::size_t> midiEvents;
    std::optional<std::size_t> controllerEvents;
    std::optional<std::size_t> controllerNameBytes;
};

struct ConformanceSoundStateCheckpoint
{
    // Must be a public transaction boundary in BOTH partitions; zero observes
    // initial/reset state before any input is scheduled. No callback is split.
    std::size_t frame = 0;
    std::vector<OfflineSoundVariableValue> expected;
    // Retained state may differ immediately after reset even when the audio
    // stimulus restores its baseline at frame zero. Omitted repeats expected.
    std::optional<std::vector<OfflineSoundVariableValue>> expectedAfterReset;
};

struct ConformanceSoundStateObservation
{
    std::size_t frame = 0;
    bool afterReset = false;
    bool publicationValid = false;
    std::vector<OfflineSoundVariableValue> values;
};

struct ConformanceStateMismatch
{
    std::string comparison;
    std::size_t frame = 0;
    std::size_t index = 0;
    std::string field;
    bool afterReset = false;
};

struct ConformanceOfflineFailureSoundState
{
    // Coordinates of the failed public call, not a claim about how far its
    // hidden state advanced. These are actual before/after getter values.
    std::size_t startFrame = 0;
    std::size_t requestedFrames = 0;
    bool afterReset = false;
    std::vector<OfflineSoundVariableValue> before;
    std::vector<OfflineSoundVariableValue> after;
};

struct ConformanceRenderOptions
{
    // Empty retains the original one-transaction offline reference. Explicit
    // partitions preserve callback-constant profiles without assuming that
    // their output is invariant under a different transaction schedule.
    std::span<const std::size_t> offlineBlockPartition;
    // Ordered absolute-frame note events; equal-time array order is retained.
    std::span<const ConformanceMidiNote> midiNotes;
    // Optional independent oracle, checked against BOTH execution paths.
    std::span<const std::vector<double>> referenceChannels;
    // Existing profiles differ on empty public transactions. Exercise the
    // call and require its declared outcome; never silently skip the call.
    ConformanceZeroFramePolicy realtimeZeroFrames = ConformanceZeroFramePolicy::RequireSuccess;
    // Disengaged means no independent event oracle. An engaged empty span
    // requires silence. Event times are absolute; equal-time order is exact.
    std::optional<std::span<const MidiEvent>> referenceMidiEvents;
    std::optional<std::span<const GlobalControllerEvent>> referenceGlobalControllerEvents;
    // Render, invoke each session's real reset API, then replay the same
    // schedule. Assert equality with the first pass and independent oracles.
    // Opt-in: retained variable/resource state can intentionally differ.
    bool verifyResetReplay = false;
    // Non-voice project/workspace/instance cells, ordered by absolute frame.
    // Equal-frame assignments retain array order and form one transaction;
    // they precede equal-frame midiNotes in that same transaction.
    std::span<const ConformanceVariableAssignment> variableAssignments;
    // Ordered caller-capacity failures, after input publication. Every probe
    // must return CapacityExceeded and preserve all caller storage/metadata;
    // the normal callback retries without rescheduling input or resetting.
    std::span<const ConformanceTerminalCapacityProbe> terminalCapacityProbes;
    // Alternative to midiNotes/variableAssignments: exact array order within
    // each frame, including numeric/Sound assignments to a voice created in
    // that frame. Execution retains the native atomic transaction semantics.
    // Do not combine the explicit stream with the two convenience schedules.
    std::span<const ConformanceInputEvent> inputEvents;
    // Exact complete Sound-variable publications at ordered shared boundaries.
    // Also retain before/after Sound values if an offline render fails.
    std::span<const ConformanceSoundStateCheckpoint> soundStateCheckpoints;
};

struct ConformanceEventMismatch
{
    std::string comparison;
    std::string stream;
    std::size_t index = 0;
    std::string field;
};

struct RuntimeConformanceResult
{
    std::vector<std::vector<double>> offlineChannels;
    std::vector<std::vector<double>> realtimeChannels;
    std::optional<ConformanceMismatch> mismatch;
    std::string message;
    std::vector<MidiEvent> offlineMidiEvents;
    std::vector<MidiEvent> realtimeMidiEvents;
    std::vector<GlobalControllerEvent> offlineGlobalControllerEvents;
    std::vector<GlobalControllerEvent> realtimeGlobalControllerEvents;
    std::optional<ConformanceEventMismatch> eventMismatch;
    bool resetReplayVerified = false;
    // Includes both passes when reset/replay was requested and succeeded.
    std::size_t terminalFailureProbesVerified = 0;
    std::vector<ConformanceSoundStateObservation> offlineSoundStates;
    std::vector<ConformanceSoundStateObservation> realtimeSoundStates;
    std::optional<ConformanceStateMismatch> stateMismatch;
    std::optional<ConformanceOfflineFailureSoundState> offlineFailureSoundState;

    [[nodiscard]] bool ok() const noexcept
    {
        return message.empty() && !mismatch.has_value() && !eventMismatch.has_value()
            && !stateMismatch.has_value();
    }
};

[[nodiscard]] RuntimeConformanceResult
compareOfflineAndPreparedRealtime(
    const PreparedRuntimeGraph& graph,
    std::size_t frameCount,
    std::span<const std::size_t> realtimeBlockPartition,
    ConformanceTolerance tolerance = {});

// This control/test-thread utility allocates output storage. It is not an
// audio callback, a reference capture, or a general typed-event runner.
[[nodiscard]] RuntimeConformanceResult runRuntimeConformance(
    const PreparedRuntimeGraph& graph,
    std::size_t frameCount,
    std::span<const std::size_t> realtimeBlockPartition,
    const ConformanceRenderOptions& options,
    ConformanceTolerance tolerance = {});
}
