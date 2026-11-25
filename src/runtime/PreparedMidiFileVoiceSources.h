#pragma once

#include "talas/runtime/Runtime.h"
#include "talas/core/MidiInputSource.h"

#include <memory>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime
{
using PreparedMidiInputSource = talas::core::MidiInputSource;

// Canonical labels, ASCII case-insensitive; blank retains live MIDI.
[[nodiscard]] std::optional<PreparedMidiInputSource>
parsePreparedMidiInputSource(std::string_view text) noexcept;

[[nodiscard]] bool hasPreparedMidiFileVoiceSource(const CompileStep& step) noexcept;
[[nodiscard]] bool hasPreparedMidiFileVoiceSource(const RuntimePlan& plan) noexcept;
[[nodiscard]] bool hasPreparedMidiScriptVoiceSource(const CompileStep& step) noexcept;
[[nodiscard]] bool hasPreparedMidiScheduledVoiceSource(const RuntimePlan& plan) noexcept;
[[nodiscard]] bool hasPreparedAnalogSequencerKeyOwner(const CompileStep& step) noexcept;
[[nodiscard]] bool hasPreparedAnalogSequencerGeneratedIndex(const RuntimePlan& plan) noexcept;

struct PreparedMidiScriptNoteEvent
{
    std::size_t frame = 0;
    bool noteOn = false;
    std::uint8_t note = 0;
    std::uint8_t allocationVelocity = 0;
    double pitch = 0.0;
    double velocity = 0.0;
    double timbre = 0.0;
    // A note-on tied to this held note moves its voice to the new note
    // without retriggering it.
    std::optional<std::uint8_t> legatoFrom;

    bool operator==(const PreparedMidiScriptNoteEvent&) const = default;
};

struct PreparedAnalogSequencerKeyStage
{
    std::size_t frame = 0;
    std::size_t sourceIndex = 0;
    std::size_t durationFrames = 0;
    std::size_t gateFrames = 0;
    // Unscaled values let live Rate integration rebuild frame timing per sample.
    double durationSeconds = 0.0;
    double dutyCycle = 0.0;
    std::optional<PreparedMidiScriptNoteEvent> attack;

    bool operator==(const PreparedAnalogSequencerKeyStage&) const = default;
};

struct PreparedAnalogSequencerSequence
{
    std::string name;
    std::vector<double> values;
    std::vector<talas::expr::Expression> expressions;
    std::vector<std::vector<std::uint32_t>> variableCellIndices;
    RuntimeScopeId expressionScope = globalRuntimeScope;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    bool operator==(const PreparedAnalogSequencerSequence& other) const noexcept;
};

[[nodiscard]] std::optional<std::vector<double>>
prepareAnalogSequencerConstantArray(std::string_view text,
    const RenderSettings& settings, talas::expr::ValueDomain domain);

[[nodiscard]] std::optional<std::vector<PreparedAnalogSequencerSequence>>
prepareAnalogSequencerExtraValues(std::string_view text,
    const RenderSettings& settings, std::string& error,
    RuntimeScopeId expressionScope = globalRuntimeScope);

// The keyboard Event Values of the voice a sequence plays within, as an
// AnalogSequencer playing another reads the outer one's notes as its base
// pitch: true, with the value, when the scope lies
// within a voice.
struct AnalogSequencerVoiceValues
{
    const void* user = nullptr;
    bool (*resolve)(const void* user, RuntimeScopeId scope, std::string_view name,
        talas::expr::ExpressionValue& value) noexcept = nullptr;
};

[[nodiscard]] bool evaluateAnalogSequencerValue(
    const PreparedAnalogSequencerSequence& sequence, std::size_t index,
    const std::vector<EventValueBinding>& bindings,
    const talas::project::PreparedProjectVariableTable& scopes,
    double sampleRate, double tempoBpm, double& value,
    std::span<const talas::expr::ExpressionValue> variableValues,
    talas::expr::ExpressionState* state = nullptr,
    std::uint64_t elapsedSampleFrame = 0,
    AnalogSequencerVoiceValues voiceValues = {}) noexcept;

[[nodiscard]] std::optional<PreparedAnalogSequencerKeyStage>
makeAnalogSequencerKeyStage(std::span<const double, 6> values,
    double sampleRate, double rate) noexcept;

struct PreparedAnalogSequencerKeySchedule
{
    // One finite cycle. The owning voice scheduler will repeat it when Loop
    // is enabled; note-off precedes note-on at a shared stage boundary.
    std::vector<PreparedMidiScriptNoteEvent> events;
    // Traversal order includes silent stages. Step pulses use these durations
    // as a minimum onset interval rather than advancing the fixed event clock.
    std::vector<PreparedAnalogSequencerKeyStage> stages;
    std::size_t cycleFrames = 0;
};

[[nodiscard]] std::optional<PreparedAnalogSequencerKeySchedule>
prepareAnalogSequencerKeySchedule(
    std::span<const double> durationsSeconds,
    std::span<const double> dutyCycles,
    std::span<const double> keyDowns,
    std::span<const double> keyPitches,
    std::span<const double> keyVelocities,
    std::span<const double> keyTimbres,
    std::size_t startIndex,
    std::size_t endIndex,
    double sampleRate,
    double rate,
    std::size_t maximumEvents,
    std::string& error);

// Ties each stage with a DutyCycle of 1 to the next stage's note, which
// takes over its voice without a new attack; a rest or the end of a
// sequence that does not loop leaves it down.
void tieFullDutyCycles(PreparedAnalogSequencerKeySchedule& schedule, bool loop);

struct PreparedMidiScriptControllerEvent
{
    std::size_t frame = 0;
    std::string name;
    double value = 0.0;

    bool operator==(const PreparedMidiScriptControllerEvent&) const = default;
};

struct PreparedMidiFileVoiceSourceLimits
{
    std::size_t maximumTotalEvents = 65536;
    std::size_t maximumSimultaneousEvents = 256;
};

// Preparation-only data, not execution authority. The runtime must independently
// admit the compiler-owned plan and immutable resource authority before using it.
// No cursor is stored here: consumption belongs to the render checkpoint.
class PreparedMidiFileVoiceSources
{
public:
    struct Owner
    {
        struct ControllerChange
        {
            std::size_t frame = 0;
            std::size_t bindingIndex = 0;
            double value = 0;
            bool operator==(const ControllerChange&) const = default;
        };
        std::size_t stepIndex = 0;
        std::string nodeId;
        std::string definitionId;
        std::vector<std::string> instancePath;
        std::optional<RuntimeScopeId> parentVoiceScopeId;
        std::vector<talas::project::ProjectMidiVoiceAncestorIdentity> voiceAncestors;
        std::uint8_t channel = 1;
        PreparedMidiInputSource source = PreparedMidiInputSource::Live;
        // Delayed Inputs start these clocks at their first active consumer;
        // their controllers must not enter the global composition timeline.
        bool activationControlled = false;
        std::vector<ControllerChange> activationControllers;
        std::string resourceId;
        std::uint64_t resourceGeneration = 0;
        std::string resourceContentHash;
        std::vector<MidiEvent> events;
        // The frames of the file's MIDI timing
        // clock messages while its clock runs, for `MIDITimingClock and
        // !TimingClock.
        std::vector<std::size_t> timingClockFrames;
        std::vector<PreparedMidiScriptNoteEvent> scriptEvents;
        std::vector<PreparedMidiScriptControllerEvent> scriptControllers;
        std::vector<PreparedAnalogSequencerSequence> analogExtraSequences;
        std::string analogGeneratedIndex;
        std::vector<PreparedAnalogSequencerKeyStage> analogKeyStages;
        std::array<PreparedAnalogSequencerSequence, 6> analogKeySequences;
        bool analogLiveArrays = false;
        bool analogLiveRate = false;
        bool analogLiveRange = false;
        bool analogLegacyRange = false;
        double analogKeyRate = 1.0;
        bool analogKeyStepTriggered = false;
        // A DutyCycle of 1 keeps the key down into the next stage;
        // projects before format 76 released and retriggered it.
        bool analogKeyLegato = false;

        [[nodiscard]] bool hasAnalogDynamicClock() const noexcept
        {
            return analogLiveRate || analogLiveRange;
        }

        [[nodiscard]] std::optional<std::uint64_t> analogExtraFrameAt(std::size_t cursor) const noexcept;
        [[nodiscard]] std::optional<std::size_t> analogExtraCursorAtOrAfter(std::uint64_t frame) const noexcept;
        bool loop = false;
        std::size_t cycleFrames = 0;

        [[nodiscard]] std::optional<std::uint64_t> scriptFrameAt(
            std::size_t cursor) const noexcept;
        [[nodiscard]] std::optional<std::size_t> scriptCursorAtOrAfter(
            std::uint64_t frame) const noexcept;
    };

    [[nodiscard]] static std::unique_ptr<const PreparedMidiFileVoiceSources> prepare(
        const RuntimePlan& plan, const RenderSettings& settings,
        PreparedMidiFileVoiceSourceLimits limits, std::string& error);

    [[nodiscard]] std::span<const Owner> owners() const noexcept { return owners_; }
    [[nodiscard]] bool hasFileSource() const noexcept { return hasFileSource_; }
    [[nodiscard]] bool hasScheduledSource() const noexcept { return hasScheduledSource_; }
    [[nodiscard]] std::size_t eventCount() const noexcept { return eventCount_; }
    [[nodiscard]] bool validateNotePlayback(
        const RuntimePlan& plan, const RenderSettings& settings, std::string& error) const;
    [[nodiscard]] bool appendScheduledControllerChanges(
        const RuntimePlan& plan, RenderSettings& settings, std::string& error) const;
    [[nodiscard]] bool equivalent(const PreparedMidiFileVoiceSources& other) const noexcept;

private:
    PreparedMidiFileVoiceSources() = default;
    std::vector<Owner> owners_;
    bool hasFileSource_ = false;
    bool hasScheduledSource_ = false;
    std::size_t eventCount_ = 0;
};
}
