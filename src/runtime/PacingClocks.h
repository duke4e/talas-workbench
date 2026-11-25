#pragma once

#include "LateInputStart.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::runtime::detail
{
// ADR-0019: a TimeControl whose Rate moves, and a TimeStopper, run a clock
// for the timing inside their Input. The clock advances by Rate each sample
// (a TimeStopper's by nothing until Resume, then by one); a TimeOffset or
// SetDuration that starts its Input late starts it when its clock reaches
// its delay, and a SetDuration's Duration counts on it. From format 87 that
// holds in flat MIDIVoices and for timing read when it starts, from 89 in
// nested ones; timing that delays its Input's samples keeps real time; a
// fixed Rate is paced when compiling, and from 89 a value read from a Sound
// by soundDelayRateScale.
namespace pacing_clocks
{
[[nodiscard]] inline const ParameterValue* findParameter(
    const CompileStep& step, std::string_view id) noexcept
{
    const auto found = std::ranges::find(step.parameters, id, &ParameterValue::id);
    return found == step.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] inline bool connected(const CompileStep& step, std::string_view port) noexcept
{
    return std::ranges::any_of(step.inputs, [&](const InputBinding& input) {
        return input.inputPortId == port;
    });
}

// A TimeControl whose Rate is fed by a Sound or follows Event Values,
// variables or time, and a TimeStopper whose Resume can hold time.
[[nodiscard]] inline bool ownsClock(const RuntimePlan& plan, std::size_t stepIndex) noexcept
{
    const auto& step = plan.steps[stepIndex];
    if (step.typeId == "talas.timeControl")
    {
        const auto* changes = findParameter(step, "rateChanges");
        const auto* rate = findParameter(step, "rate");
        return (changes == nullptr
                   || changes->choice != std::optional<std::string>{"legacyPlaybackSpeed"})
            && rate != nullptr
            && (connected(step, "rate") || !late_input_start::isFixed(*rate));
    }
    if (step.typeId == "talas.timeStopper")
    {
        const auto* stopped = findParameter(step, "whileStopped");
        const auto* resume = findParameter(step, "resume");
        const auto* onZero = findParameter(step, "resumeOnZero");
        if ((stopped != nullptr
                && stopped->choice == std::optional<std::string>{"legacyHold"})
            || resume == nullptr)
        {
            return false;
        }
        if (connected(step, "resume"))
            return true;
        // A fixed Resume that already lets time run holds nothing.
        const auto resumesAtOnce = (onZero != nullptr && onZero->boolean.value_or(false))
            ? resume->value == 0.0
            : resume->value != 0.0;
        return !late_input_start::isFixed(*resume) || !resumesAtOnce;
    }
    return false;
}

// The steps a TimeControl or TimeStopper paces: those upstream of its
// Input whose every path to an output runs through that Input, as
// compiling finds them for a fixed Rate (paceTimeControls). A voice's
// output reaches its MIDIVoice outside the plan's inputs; throughVoices
// follows it there, as compiling follows the MIDIVoice's Input.
[[nodiscard]] inline std::vector<char> pacedSteps(
    const RuntimePlan& plan, std::size_t owner, bool throughVoices = false)
{
    const auto count = plan.steps.size();
    std::vector<char> paced(count, 0);
    // Each input's producer, and each producer's consumers.
    const auto producerOf = [&](const InputBinding& input) -> std::optional<std::size_t> {
        for (std::size_t candidate = 0; candidate < count; ++candidate)
        {
            if (plan.steps[candidate].nodeId == input.sourceNodeId
                && plan.steps[candidate].scopeId == input.sourceScopeId)
            {
                return candidate;
            }
        }
        return std::nullopt;
    };
    struct Use
    {
        std::size_t consumer = 0;
        std::string_view port;
    };
    std::vector<std::vector<Use>> uses(count);
    // The producers of each step's inputs, voices' outputs included.
    std::vector<std::vector<std::size_t>> producers(count);
    for (std::size_t consumer = 0; consumer < count; ++consumer)
    {
        for (const auto& input : plan.steps[consumer].inputs)
        {
            if (const auto producer = producerOf(input))
            {
                uses[*producer].push_back({consumer, input.inputPortId});
                producers[consumer].push_back(*producer);
            }
        }
    }
    if (throughVoices)
    {
        for (const auto& voiceOwner : plan.midiVoices)
        {
            if (voiceOwner.ownerStepIndex >= count)
                continue;
            for (const auto& voice : voiceOwner.voices)
            {
                for (const auto bufferIndex : {std::optional<std::size_t>{voice.audioOutputBufferIndex},
                         voice.rightAudioOutputBufferIndex})
                {
                    if (!bufferIndex.has_value() || *bufferIndex >= plan.buffers.size())
                        continue;
                    const auto& buffer = plan.buffers[*bufferIndex];
                    for (std::size_t producer = 0; producer < count; ++producer)
                    {
                        if (plan.steps[producer].nodeId == buffer.nodeId
                            && plan.steps[producer].scopeId == buffer.scopeId)
                        {
                            uses[producer].push_back({voiceOwner.ownerStepIndex, "audioIn"});
                            producers[voiceOwner.ownerStepIndex].push_back(producer);
                        }
                    }
                }
            }
        }
    }
    // Everything upstream of the owner's Input.
    std::vector<char> inside(count, 0);
    std::vector<std::size_t> pending;
    for (const auto& input : plan.steps[owner].inputs)
    {
        if (input.inputPortId == "audioIn")
            if (const auto producer = producerOf(input))
                pending.push_back(*producer);
    }
    while (!pending.empty())
    {
        const auto index = pending.back();
        pending.pop_back();
        if (index == owner || inside[index] != 0)
            continue;
        inside[index] = 1;
        for (const auto producer : producers[index])
            pending.push_back(producer);
    }
    // A step is paced when all it feeds stays inside, reaching the owner
    // only through its Input.
    const auto exclusive = [&](std::size_t index) {
        return std::ranges::all_of(uses[index], [&](const Use& use) {
            return use.consumer == owner ? use.port == "audioIn"
                                         : inside[use.consumer] != 0;
        });
    };
    for (std::size_t index = 0; index < count; ++index)
    {
        if (inside[index] == 0)
            continue;
        std::vector<std::size_t> frontier{index};
        std::vector<char> seen(count, 0);
        bool allInside = true;
        while (allInside && !frontier.empty())
        {
            const auto current = frontier.back();
            frontier.pop_back();
            if (seen[current] != 0)
                continue;
            seen[current] = 1;
            if (!exclusive(current))
                allInside = false;
            for (const auto& use : uses[current])
                if (use.consumer != owner)
                    frontier.push_back(use.consumer);
        }
        paced[index] = allInside ? 1 : 0;
    }
    return paced;
}

// Format 153: the Rate of
// a TimeControl whose fixed Rate slows or speeds !Time and !LocalTime in its
// Input, and 0 for a TimeStopper whose fixed Resume holds time there; none
// for one whose Rate or Resume moves (not followed) or that lets time run.
[[nodiscard]] inline std::optional<double> fixedTimeRate(const CompileStep& step) noexcept
{
    if (step.typeId == "talas.timeControl")
    {
        const auto* changes = findParameter(step, "rateChanges");
        const auto* rate = findParameter(step, "rate");
        if ((changes != nullptr && changes->choice == std::optional<std::string>{"legacyPlaybackSpeed"})
            || rate == nullptr || connected(step, "rate") || !late_input_start::isFixed(*rate)
            || !std::isfinite(rate->value) || rate->value < 0.0 || rate->value == 1.0)
        {
            return std::nullopt;
        }
        return rate->value;
    }
    if (step.typeId == "talas.timeStopper")
    {
        const auto* stopped = findParameter(step, "whileStopped");
        const auto* resume = findParameter(step, "resume");
        const auto* onZero = findParameter(step, "resumeOnZero");
        if ((stopped != nullptr && stopped->choice == std::optional<std::string>{"legacyHold"})
            || resume == nullptr || connected(step, "resume") || !late_input_start::isFixed(*resume))
        {
            return std::nullopt;
        }
        const auto resumesAtOnce = (onZero != nullptr && onZero->boolean.value_or(false))
            ? resume->value == 0.0
            : resume->value != 0.0;
        return resumesAtOnce ? std::nullopt : std::optional<double>{0.0};
    }
    return std::nullopt;
}

// For each step outside MIDI voices, the clock its !Time and !LocalTime
// read: those of the TimeControls and TimeStoppers whose Input it reaches
// an output only through, from the start of each (soundStarts) on.
[[nodiscard]] inline std::vector<ControlledTimeClock> controlledTimeClocksFor(
    const RuntimePlan& plan, std::span<const std::uint64_t> soundStarts, double sampleRate)
{
    const auto count = plan.steps.size();
    std::vector<ControlledTimeClock> clocks(count);
    if (soundStarts.size() != count || !(sampleRate > 0.0))
        return clocks;
    struct Owner
    {
        std::uint64_t start = 0;
        double rate = 1.0;
        std::vector<char> paced;
    };
    std::vector<Owner> owners;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (plan.steps[index].scopeId != globalRuntimeScope)
            continue;
        if (const auto rate = fixedTimeRate(plan.steps[index]))
            owners.push_back({soundStarts[index], *rate, pacedSteps(plan, index)});
    }
    if (owners.empty())
        return clocks;
    std::ranges::stable_sort(owners, {}, &Owner::start);
    for (std::size_t index = 0; index < count; ++index)
    {
        if (plan.steps[index].scopeId != globalRuntimeScope)
            continue;
        auto& clock = clocks[index];
        for (const auto& owner : owners)
        {
            if (owner.paced[index] == 0)
                continue;
            // By the owner's start the clock has reached this !Time.
            clock.originSeconds += clock.rate
                * static_cast<double>(owner.start - std::min(owner.start, clock.originFrame)) / sampleRate;
            clock.originFrame = std::max(clock.originFrame, owner.start);
            clock.rate *= owner.rate;
            clock.controlled = true;
        }
    }
    return clocks;
}

// The !Time and !LocalTime a controlled clock reads at a frame, its step's
// own Sound starting at soundStart.
inline void controlledTimes(const ControlledTimeClock& clock, std::uint64_t frame, std::uint64_t soundStart,
    double sampleRate, double& time, double& localTime) noexcept
{
    const auto from = std::max(soundStart, clock.originFrame);
    time = clock.originSeconds
        + clock.rate * static_cast<double>(frame > clock.originFrame ? frame - clock.originFrame : 0) / sampleRate;
    localTime = clock.rate * static_cast<double>(frame > from ? frame - from : 0) / sampleRate;
}

// For each step, the clocks that pace it: those of the TimeControls and
// TimeStoppers whose Input it reaches an output only through, when it is a
// TimeOffset or SetDuration that starts its Input late (delayFrames) or a
// SetDuration that starts at once. Others get none.
[[nodiscard]] inline std::vector<std::vector<std::size_t>> clocksFor(
    const RuntimePlan& plan,
    std::span<const std::optional<std::uint64_t>> delayFrames)
{
    const auto count = plan.steps.size();
    std::vector<std::vector<std::size_t>> clocks(count);
    if (delayFrames.size() != count)
        return clocks;
    std::vector<std::size_t> owners;
    for (std::size_t index = 0; index < count; ++index)
        if (ownsClock(plan, index))
            owners.push_back(index);
    if (owners.empty())
        return clocks;

    const auto paceable = [&](std::size_t index) {
        const auto& step = plan.steps[index];
        if (step.typeId == "talas.timeOffset")
            return delayFrames[index].has_value();
        if (step.typeId != "talas.setDuration")
            return false;
        if (delayFrames[index].has_value())
            return true;
        if (!late_input_start::placeable(plan, index,
                late_input_start::modeOf(findParameter(step, "startTimeMeans"))))
            return false;
        const auto* startTime = findParameter(step, "startTime");
        return startTime != nullptr && !connected(step, "startTime")
            && late_input_start::isFixed(*startTime) && startTime->value == 0.0;
    };

    // From format 89 a clock around a MIDIVoice paces the timing in its
    // voices too.
    const auto classic = [&](std::size_t index) {
        const auto& step = plan.steps[index];
        return late_input_start::modeOf(findParameter(step,
                   step.typeId == "talas.setDuration" ? "startTimeMeans" : "offsetMeans"))
            == late_input_start::Mode::Classic;
    };
    for (const auto owner : owners)
    {
        const auto paced = pacedSteps(plan, owner);
        const auto pacedThroughVoices = pacedSteps(plan, owner, true);
        for (std::size_t index = 0; index < count; ++index)
        {
            if ((paced[index] != 0 || (pacedThroughVoices[index] != 0 && classic(index))) && paceable(index))
                clocks[index].push_back(owner);
        }
    }
    return clocks;
}

// The fixed Rates of the TimeControls a step reaches an output only through,
// as one factor (1 / their product).
[[nodiscard]] inline double fixedRatesScale(const RuntimePlan& plan, std::size_t stepIndex)
{
    auto scale = 1.0;
    for (std::size_t owner = 0; owner < plan.steps.size(); ++owner)
    {
        const auto& control = plan.steps[owner];
        if (control.typeId != "talas.timeControl")
            continue;
        const auto* changes = findParameter(control, "rateChanges");
        const auto* rate = findParameter(control, "rate");
        // A legacy playback speed paces nothing; a Rate fed by a Sound or
        // that moves runs a clock instead.
        if ((changes != nullptr
                && changes->choice == std::optional<std::string>{"legacyPlaybackSpeed"})
            || rate == nullptr || connected(control, "rate") || !late_input_start::isFixed(*rate)
            || !std::isfinite(rate->value) || rate->value <= 0.0 || rate->value == 1.0)
        {
            continue;
        }
        if (pacedSteps(plan, owner, true)[stepIndex] != 0)
            scale /= rate->value;
    }
    return scale;
}

// ADR-0019 (2026-10-12): compiling paces a TimeOffset's SilentTime or a
// SetDuration's StartTime under a TimeControl with a fixed Rate by dividing
// the field's expression by Rate, which leaves a field fed by a Sound. From
// format 89 the value such a step reads from its Sound as it starts is
// divided by the same Rates; this is the factor. 1 for older projects.
[[nodiscard]] inline double soundDelayRateScale(const RuntimePlan& plan, std::size_t stepIndex)
{
    if (stepIndex >= plan.steps.size())
        return 1.0;
    const auto& step = plan.steps[stepIndex];
    const auto startTime = step.typeId == "talas.setDuration";
    if (!startTime && step.typeId != "talas.timeOffset")
        return 1.0;
    if (late_input_start::modeOf(findParameter(step, startTime ? "startTimeMeans" : "offsetMeans"))
        != late_input_start::Mode::Classic)
    {
        return 1.0;
    }
    return fixedRatesScale(plan, stepIndex);
}

// ADR-0019 (2026-10-19): from format 96 a SetDuration's Duration fed by a
// Sound is paced by the same fixed Rates, each frame's value divided by
// them, as compiling divides a Duration expression; 1 for older projects
// (startTimeMeans legacyDurationUnpaced and the earlier choices).
[[nodiscard]] inline double soundDurationRateScale(const RuntimePlan& plan, std::size_t stepIndex)
{
    if (stepIndex >= plan.steps.size() || plan.steps[stepIndex].typeId != "talas.setDuration")
        return 1.0;
    const auto* means = findParameter(plan.steps[stepIndex], "startTimeMeans");
    if (means == nullptr || means->choice != std::optional<std::string>{"startsInputLate"})
        return 1.0;
    return fixedRatesScale(plan, stepIndex);
}
}
}
