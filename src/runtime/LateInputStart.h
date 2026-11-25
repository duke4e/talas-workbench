#pragma once

#include "CheckedSetDurationState.h"
#include "talas/dsp/TimeOffset.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime::detail
{
namespace late_input_start
{
// A field that holds the same value throughout the Sound.
[[nodiscard]] inline bool isFixed(const ParameterValue& parameter) noexcept
{
    return !parameter.expression.has_value()
        || (parameter.expression->eventReferences().empty()
            && parameter.expression->variableReferences().empty()
            && !parameter.expression->usesTime()
            && !parameter.expression->isStateful());
}

// Whether any MIDIVoice in the plan is nested in another's Input.
[[nodiscard]] inline bool hasNestedMidiVoices(const RuntimePlan& plan) noexcept
{
    return std::ranges::any_of(plan.midiVoices, [](const MidiVoicePlan& owner) {
        return owner.parentVoiceScopeId.has_value();
    });
}

// Projects from format 64 to 86 start an Input late only outside MIDI
// voices, in Sounds without nested ones.
[[nodiscard]] inline bool outsideMidiVoices(const RuntimePlan& plan, std::size_t stepIndex) noexcept
{
    if (hasNestedMidiVoices(plan))
        return false;
    for (const auto& owner : plan.midiVoices)
    {
        for (const auto& voice : owner.voices)
        {
            if (stepIndex >= voice.firstStepIndex
                && stepIndex < voice.firstStepIndex + voice.stepCount)
            {
                return false;
            }
        }
    }
    return true;
}

// How a TimeOffset's offsetMeans or a SetDuration's startTimeMeans starts
// its Input late: from format 89 anywhere
// (Classic); in formats 87 and 88 so only in Sounds without nested MIDIVoices,
// and with a Sound-fed field not paced by a fixed Rate (FlatVoices); from
// 64 or 66 only with a fixed field outside MIDI voices (FixedOnly).
enum class Mode
{
    None,
    FixedOnly,
    FlatVoices,
    Classic,
};

[[nodiscard]] inline Mode modeOf(const ParameterValue* means) noexcept
{
    if (means == nullptr || !means->choice.has_value())
        return Mode::None;
    // Format 96 paces a Sound-fed Duration too; older projects of this
    // rule keep it unpaced (legacyDurationUnpaced) and start alike.
    if (*means->choice == "startsInputLate" || *means->choice == "legacyDurationUnpaced")
        return Mode::Classic;
    if (*means->choice == "legacyFlatVoicesOnly")
        return Mode::FlatVoices;
    if (*means->choice == "legacyFixedStartsLate")
        return Mode::FixedOnly;
    return Mode::None;
}

// Where a step may start its Input late: anywhere from format 89, a
// MIDIVoice's Input counting from the MIDIVoice's start, and a nested
// MIDIVoice's from where its own MIDIVoice starts; formats 87 and 88 only
// in Sounds without nested MIDIVoices.
[[nodiscard]] inline bool placeable(const RuntimePlan& plan, std::size_t stepIndex, Mode mode) noexcept
{
    if (mode == Mode::Classic)
        return true;
    return mode == Mode::FlatVoices ? !hasNestedMidiVoices(plan) : outsideMidiVoices(plan, stepIndex);
}

// Whether a step reads its timing field as a constant field is read,
// once as it starts (format 87 on).
[[nodiscard]] inline bool readsAsTalas(Mode mode) noexcept
{
    return mode == Mode::Classic || mode == Mode::FlatVoices;
}
}

// ADR-0023 (2026-10-10): a TimeOffset's SilentTime or a SetDuration's
// StartTime fed by a Sound or following Event Values, variables or time is
// read once, when the activation schedule first reaches the step, and the
// step then starts its Input that much later. The delay
// lateInputDelayFrames gives such a step is a placeholder.
[[nodiscard]] inline bool lateInputReadsDelayAtStart(const RuntimePlan& plan, std::size_t stepIndex) noexcept
{
    if (stepIndex >= plan.steps.size())
        return false;
    const auto& step = plan.steps[stepIndex];
    const auto startTime = step.typeId == "talas.setDuration";
    if (!startTime && step.typeId != "talas.timeOffset")
        return false;
    const auto port = startTime ? std::string_view{"startTime"} : std::string_view{"offset"};
    const auto field = std::ranges::find(step.parameters, port, &ParameterValue::id);
    const auto means = std::ranges::find(step.parameters,
        startTime ? std::string_view{"startTimeMeans"} : std::string_view{"offsetMeans"}, &ParameterValue::id);
    if (field == step.parameters.end() || means == step.parameters.end()
        || !late_input_start::readsAsTalas(late_input_start::modeOf(&*means)))
    {
        return false;
    }
    return std::ranges::any_of(step.inputs, [&](const InputBinding& input) { return input.inputPortId == port; })
        || !late_input_start::isFixed(*field);
}

// ADR-0023: the frames by which a TimeOffset or a SetDuration starts its
// Input late, as a WaitUntil does, in projects from format 64 for a
// TimeOffset and 66 for a SetDuration. From format 87 that holds in flat
// MIDIVoices too, from 89 in nested ones, and a field fed by a Sound or
// that moves is read when the step starts (lateInputReadsDelayAtStart, 0
// here); before 87 only a fixed field outside MIDI voices starts the Input
// late. A TimeOffset must put its silence before the Input; a fixed
// StartTime of 0 has nothing to start late. Any other TimeOffset or
// SetDuration delays its Input's samples, as do those of formats 87 and 88
// in Sounds with nested MIDIVoices.
[[nodiscard]] inline std::optional<std::uint64_t> lateInputDelayFrames(
    const RuntimePlan& plan,
    std::size_t stepIndex,
    double sampleRate) noexcept
{
    if (stepIndex >= plan.steps.size())
        return std::nullopt;
    const auto& step = plan.steps[stepIndex];
    const auto find = [&](std::string_view id) -> const ParameterValue* {
        const auto found = std::ranges::find(step.parameters, id, &ParameterValue::id);
        return found == step.parameters.end() ? nullptr : &*found;
    };
    const auto connected = [&](std::string_view port) {
        return std::ranges::any_of(step.inputs, [&](const InputBinding& input) {
            return input.inputPortId == port;
        });
    };
    if (step.typeId == "talas.setDuration")
    {
        const auto mode = late_input_start::modeOf(find("startTimeMeans"));
        const auto* startTime = find("startTime");
        if (mode == late_input_start::Mode::None || startTime == nullptr
            || !late_input_start::placeable(plan, stepIndex, mode))
        {
            return std::nullopt;
        }
        if (lateInputReadsDelayAtStart(plan, stepIndex))
            return std::uint64_t{0};
        if (connected("startTime") || !late_input_start::isFixed(*startTime))
            return std::nullopt;
        std::size_t frames = 0;
        if (checked_set_duration::startFramesFor(startTime->value, sampleRate, frames)
                != checked_set_duration::Error::None
            || frames == 0)
        {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(frames);
    }
    if (step.typeId != "talas.timeOffset")
        return std::nullopt;
    const auto mode = late_input_start::modeOf(find("offsetMeans"));
    const auto* offset = find("offset");
    const auto readsAtStart = lateInputReadsDelayAtStart(plan, stepIndex);
    if (mode == late_input_start::Mode::None || offset == nullptr
        || (!readsAtStart && (connected("offset") || !late_input_start::isFixed(*offset))))
    {
        return std::nullopt;
    }
    const auto boolean = [&](std::string_view id) {
        const auto* parameter = find(id);
        return parameter != nullptr && parameter->boolean.value_or(false);
    };
    const auto* reverseMeans = find("reverseMeans");
    const auto flags = talas::dsp::effectiveTimeOffsetFlags(
        boolean("reverse"),
        boolean("retrograde"),
        reverseMeans != nullptr
            && reverseMeans->choice == std::optional<std::string>{"legacyReverseWindows"});
    if (flags.reverse || flags.retrograde || !late_input_start::placeable(plan, stepIndex, mode))
        return std::nullopt;
    if (readsAtStart)
        return std::uint64_t{0};
    std::size_t frames = 0;
    if (!talas::dsp::checkedTimeOffsetFrames(sampleRate, offset->value, frames))
        return std::nullopt;
    return static_cast<std::uint64_t>(frames);
}

namespace late_input_start
{
// The innermost TimeOffset, SetDuration or WaitUntil whose Input each step
// belongs to, topLevel for none; a step in a flat MIDIVoice's Input belongs
// where the MIDIVoice does.
inline constexpr auto topLevel = static_cast<std::size_t>(-1);

[[nodiscard]] inline std::vector<std::size_t> regionsOf(
    const RuntimePlan& plan,
    std::span<const std::optional<std::uint64_t>> delayFrames)
{
    const auto count = plan.steps.size();
    const auto isGate = [&](std::size_t index) {
        return delayFrames[index].has_value() || plan.steps[index].typeId == "talas.waitUntil";
    };
    // The steps reading each step's outputs, and through which input.
    struct Consumer
    {
        std::size_t step;
        bool throughInput;
    };
    std::vector<std::vector<Consumer>> consumers(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        for (const auto& input : plan.steps[index].inputs)
        {
            for (std::size_t producer = 0; producer < count; ++producer)
            {
                if (plan.steps[producer].nodeId == input.sourceNodeId
                    && plan.steps[producer].scopeId == input.sourceScopeId)
                {
                    consumers[producer].push_back({index, input.inputPortId == "audioIn"});
                }
            }
        }
    }
    // A voice's output reaches its MIDIVoice outside the plan's inputs.
    for (const auto& owner : plan.midiVoices)
    {
        for (const auto& voice : owner.voices)
        {
            for (const auto bufferIndex :
                {std::optional<std::size_t>{voice.audioOutputBufferIndex}, voice.rightAudioOutputBufferIndex})
            {
                if (!bufferIndex.has_value() || *bufferIndex >= plan.buffers.size()
                    || owner.ownerStepIndex >= count)
                    continue;
                const auto& buffer = plan.buffers[*bufferIndex];
                for (std::size_t producer = 0; producer < count; ++producer)
                {
                    if (plan.steps[producer].nodeId == buffer.nodeId
                        && plan.steps[producer].scopeId == buffer.scopeId)
                    {
                        consumers[producer].push_back({owner.ownerStepIndex, false});
                    }
                }
            }
        }
    }
    // The innermost gate whose Input a step belongs to; the plan lists each
    // step before the steps that read it, so the gates around a step are
    // known before the step.
    std::vector<std::size_t> region(count, topLevel);
    const auto enclosing = [&](std::size_t gate) { return region[gate]; };
    const auto commonRegion = [&](std::size_t first, std::size_t second) {
        for (auto outer = first; outer != topLevel; outer = enclosing(outer))
        {
            for (auto inner = second; inner != topLevel; inner = enclosing(inner))
            {
                if (outer == inner)
                    return outer;
            }
        }
        return topLevel;
    };
    for (auto index = count; index-- > 0;)
    {
        std::optional<std::size_t> shared;
        for (const auto& consumer : consumers[index])
        {
            if (consumer.step <= index)
            {
                shared = topLevel;
                break;
            }
            const auto through = consumer.throughInput && isGate(consumer.step)
                ? consumer.step
                : region[consumer.step];
            shared = shared.has_value() ? commonRegion(*shared, through) : through;
        }
        region[index] = shared.value_or(topLevel);
    }
    return region;
}
}

// The frame at which each TimeOffset or SetDuration that starts its Input
// late starts it: its delay after the start of the innermost such step whose
// Input it belongs to, or after the start of the whole Sound; a step in a
// flat MIDIVoice's Input belongs where the MIDIVoice does.
// One in a WaitUntil's Input, paced by a clock (pacedAtRuntime, ADR-0019) or
// reading its delay when it starts has no fixed frame; it starts its Input
// when the activation schedule finds its delay has passed.
[[nodiscard]] inline std::vector<std::optional<std::uint64_t>> lateInputStartFrames(
    const RuntimePlan& plan,
    std::span<const std::optional<std::uint64_t>> delayFrames,
    std::span<const char> pacedAtRuntime = {})
{
    const auto count = plan.steps.size();
    std::vector<std::optional<std::uint64_t>> starts(count);
    if (delayFrames.size() != count
        || std::ranges::none_of(delayFrames, [](const auto& frames) { return frames.has_value(); }))
    {
        return starts;
    }
    const auto region = late_input_start::regionsOf(plan, delayFrames);
    constexpr auto topLevel = late_input_start::topLevel;
    for (auto index = count; index-- > 0;)
    {
        if (!delayFrames[index].has_value()
            || (index < pacedAtRuntime.size() && pacedAtRuntime[index] != 0)
            || lateInputReadsDelayAtStart(plan, index))
            continue;
        const auto outer = region[index];
        if (outer == topLevel)
            starts[index] = delayFrames[index];
        else if (delayFrames[outer].has_value() && starts[outer].has_value())
            starts[index] = *starts[outer] + *delayFrames[index];
    }
    return starts;
}

// The frame at which each step's own Sound starts, from which its functions
// of time count: 0 for a step at the top of the
// whole Sound, or the start of the innermost TimeOffset or SetDuration whose
// Input it belongs to. One whose Sound starts at no fixed frame counts from
// the start of the whole Sound.
[[nodiscard]] inline std::vector<std::uint64_t> soundStartFrames(
    const RuntimePlan& plan,
    std::span<const std::optional<std::uint64_t>> delayFrames,
    std::span<const char> pacedAtRuntime = {})
{
    std::vector<std::uint64_t> starts(plan.steps.size(), 0);
    const auto gates = lateInputStartFrames(plan, delayFrames, pacedAtRuntime);
    if (std::ranges::none_of(gates, [](const auto& frame) { return frame.has_value(); }))
        return starts;
    const auto region = late_input_start::regionsOf(plan, delayFrames);
    for (std::size_t index = 0; index < starts.size(); ++index)
    {
        if (region[index] != late_input_start::topLevel && gates[region[index]].has_value())
            starts[index] = *gates[region[index]];
    }
    return starts;
}
}
