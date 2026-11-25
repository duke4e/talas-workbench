#pragma once

#include "CheckedDelayWithFeedbackState.h"
#include "CheckedHarmonicResonatorMath.h"
#include "talas/dsp/ReverbSection.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace talas::runtime::detail
{
namespace delay_with_feedback_lines
{
[[nodiscard]] inline const ParameterValue* find(const CompileStep& step, std::string_view id) noexcept
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
}

// A field that a connected Sound, an Event Value, a variable or time
// drives, so that it changes while the Sound plays.
[[nodiscard]] inline bool fieldChangesWhilePlaying(const CompileStep& step, std::string_view id) noexcept
{
    const auto* parameter = delay_with_feedback_lines::find(step, id);
    return delay_with_feedback_lines::connected(step, id)
        || (parameter != nullptr
            && (!parameter->variableCellIndices.empty()
                || (parameter->expression.has_value()
                    && (!parameter->expression->eventReferences().empty()
                        || !parameter->expression->variableReferences().empty()
                        || parameter->expression->usesTime()))));
}

// ADR-0025: the Wavetable a DelayWithFeedback, ReverbSection or
// HarmonicResonator (2026-10-10) shares its delay line by, or nothing for
// Private.
[[nodiscard]] inline std::optional<std::string_view> sharedDelayWavetable(
    const CompileStep& step) noexcept
{
    if (step.typeId != "talas.delayWithFeedback" && step.typeId != "talas.reverbSection"
        && step.typeId != "talas.harmonicResonator")
        return std::nullopt;
    const auto* wavetable = delay_with_feedback_lines::find(step, "wavetable");
    if (wavetable == nullptr || !wavetable->resource.has_value())
        return std::nullopt;
    std::string_view name = *wavetable->resource;
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())))
        name.remove_prefix(1);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
        name.remove_suffix(1);
    if (name.empty() || name == "Private")
        return std::nullopt;
    return name;
}

[[nodiscard]] inline bool delayWithFeedbackBoolean(
    const CompileStep& step, std::string_view id, bool fallback) noexcept
{
    const auto* parameter = delay_with_feedback_lines::find(step, id);
    return parameter != nullptr ? parameter->boolean.value_or(fallback) : fallback;
}

// The frames a DelayWithFeedback's line holds: those of its fixed delay, of
// its whole Delay when DelayScale changes, or the bounded history when the
// Delay itself changes. A value driven by a connected Sound, an Event Value,
// a variable or time changes.
[[nodiscard]] inline std::optional<std::size_t> delayWithFeedbackHistoryFrames(
    const CompileStep& step,
    const RenderSettings& settings) noexcept
{
    const auto changes = [&](std::string_view id) {
        return fieldChangesWhilePlaying(step, id);
    };
    // A HarmonicResonator holds its period, 1/Frequency, or the bounded
    // history when its Frequency changes.
    if (step.typeId == "talas.harmonicResonator")
    {
        if (changes("frequency"))
            return settings.maxRealtimeHistoryFrames;
        const auto* frequency = delay_with_feedback_lines::find(step, "frequency");
        auto period = checked_harmonic_resonator::Period{};
        if (frequency == nullptr
            || !checked_harmonic_resonator::preparePeriod(settings.sampleRate, frequency->value,
                settings.maxRealtimeHistoryFrames, period))
        {
            return std::nullopt;
        }
        return period.delaySamples;
    }
    const auto* interpolation = delay_with_feedback_lines::find(step, "interpolation");
    const auto linear = interpolation != nullptr
        && interpolation->choice == std::optional<std::string>{"linear"};
    // A ReverbSection holds the bounded history whenever its Delay or
    // DelayScale changes.
    if (step.typeId == "talas.reverbSection")
    {
        if (changes("delay") || changes("delayScale"))
            return settings.maxRealtimeHistoryFrames;
        const auto* reverbDelay = delay_with_feedback_lines::find(step, "delay");
        const auto* reverbDelayScale = delay_with_feedback_lines::find(step, "delayScale");
        talas::dsp::ReverbSectionDelayGeometry geometry;
        if (reverbDelay == nullptr || reverbDelayScale == nullptr
            || !talas::dsp::checkedReverbSectionDelayGeometry(
                settings.sampleRate, reverbDelay->value, reverbDelayScale->value,
                linear ? talas::dsp::ReverbSectionInterpolation::Linear
                       : talas::dsp::ReverbSectionInterpolation::Nearest,
                geometry))
        {
            return std::nullopt;
        }
        return geometry.capacity;
    }
    if (changes("delay"))
        return settings.maxRealtimeHistoryFrames;
    const auto* delay = delay_with_feedback_lines::find(step, "delay");
    const auto* delayScale = delay_with_feedback_lines::find(step, "delayScale");
    if (delay == nullptr || delayScale == nullptr)
        return std::nullopt;
    const checked_delay_with_feedback::Config config{
        settings.sampleRate,
        checked_delay_with_feedback::Type::Comb,
        linear ? checked_delay_with_feedback::Interpolation::Linear
               : checked_delay_with_feedback::Interpolation::None,
    };
    auto delayFrames = 0.0;
    std::size_t historyFrames = 0;
    if (checked_delay_with_feedback::delayGeometry(
            config, delay->value, changes("delayScale") ? 1.0 : delayScale->value,
            delayFrames, historyFrames)
        != checked_delay_with_feedback::Error::None)
    {
        return std::nullopt;
    }
    return historyFrames;
}

// The length of the line the DelayWithFeedbacks naming Wavetable share: one
// frame longer than the longest of their lines, or nothing when one of them
// cannot be measured. A line kept over a restart is one whose delays all
// have Prezero off.
struct SharedDelayLineShape
{
    std::size_t length = 0;
    bool keptOverRestart = true;
};

[[nodiscard]] inline std::optional<SharedDelayLineShape> sharedDelayLineShape(
    const RuntimePlan& plan,
    const RenderSettings& settings,
    std::string_view wavetable) noexcept
{
    SharedDelayLineShape shape;
    for (const auto& step : plan.steps)
    {
        if (sharedDelayWavetable(step) != std::optional<std::string_view>{wavetable})
            continue;
        const auto frames = delayWithFeedbackHistoryFrames(step, settings);
        if (!frames.has_value() || *frames == std::numeric_limits<std::size_t>::max())
            return std::nullopt;
        shape.length = std::max(shape.length, *frames + 1);
        shape.keptOverRestart = shape.keptOverRestart
            && !delayWithFeedbackBoolean(step, "prezero", true);
    }
    if (shape.length == 0)
        return std::nullopt;
    return shape;
}
}
