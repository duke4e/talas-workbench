#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_function_generator
{
struct State
{
    double elapsedTime = 0.0;
    bool triggered = false;
    bool triggerWasPositive = false;

    bool operator==(const State&) const = default;
};

struct Controls
{
    double trigger = 0.0;
    double onDuration = 0.0;
    bool triggerConnected = false;
};

struct Config
{
    double sampleRate = 0.0;
};

enum class Error
{
    None,
    TableInvalid,
    StateInvalid,
    ControlsInvalid,
    ConfigInvalid,
    SampleDurationNonFinite,
    PhaseNonFinite,
    InterpolationNonFinite,
    StateStepNonFinite,
};

[[nodiscard]] inline Error sampleLinear(
    std::span<const double> table,
    double normalizedPosition,
    double& output) noexcept
{
    if (table.empty())
        return Error::TableInvalid;
    if (!std::isfinite(normalizedPosition))
        return Error::PhaseNonFinite;
    if (table.size() == 1)
    {
        if (!std::isfinite(table.front()))
            return Error::InterpolationNonFinite;
        output = table.front();
        return Error::None;
    }

    const auto clamped = std::clamp(normalizedPosition, 0.0, 1.0);
    const auto scaled = clamped * static_cast<double>(table.size() - 1);
    if (!std::isfinite(clamped) || !std::isfinite(scaled))
        return Error::InterpolationNonFinite;
    const auto leftIndex = static_cast<std::size_t>(std::floor(scaled));
    const auto rightIndex = std::min(leftIndex + 1, table.size() - 1);
    const auto amount = scaled - static_cast<double>(leftIndex);
    const auto left = table[leftIndex];
    const auto right = table[rightIndex];
    const auto difference = right - left;
    const auto scaledDifference = difference * amount;
    const auto interpolated = left + scaledDifference;
    if (!std::isfinite(left) || !std::isfinite(right)
        || !std::isfinite(amount) || !std::isfinite(difference)
        || !std::isfinite(scaledDifference)
        || !std::isfinite(interpolated))
    {
        return Error::InterpolationNonFinite;
    }
    output = interpolated;
    return Error::None;
}

[[nodiscard]] inline Error advanceFrame(
    std::span<const double> table,
    State state,
    Controls controls,
    Config config,
    State& nextState,
    double& output) noexcept
{
    if (table.empty())
        return Error::TableInvalid;
    if (!std::isfinite(state.elapsedTime) || state.elapsedTime < 0.0
        || (!state.triggered
            && (state.elapsedTime != 0.0 || state.triggerWasPositive)))
    {
        return Error::StateInvalid;
    }
    if (!std::isfinite(controls.trigger)
        || !std::isfinite(controls.onDuration)
        || controls.onDuration <= 0.0)
    {
        return Error::ControlsInvalid;
    }
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0)
        return Error::ConfigInvalid;

    auto candidate = state;
    auto candidateOutput = 0.0;
    if (!controls.triggerConnected && controls.trigger <= 0.0)
    {
        candidate = {};
        nextState = candidate;
        output = candidateOutput;
        return Error::None;
    }

    if (!controls.triggerConnected && !candidate.triggered)
    {
        candidate.elapsedTime = 0.0;
        candidate.triggered = true;
        candidate.triggerWasPositive = true;
    }
    else if (controls.triggerConnected)
    {
        const auto triggerIsPositive = controls.trigger > 0.0;
        if (triggerIsPositive && !candidate.triggerWasPositive)
        {
            candidate.elapsedTime = 0.0;
            candidate.triggered = true;
        }
        candidate.triggerWasPositive = triggerIsPositive;
    }

    if (candidate.triggered
        && candidate.elapsedTime < controls.onDuration)
    {
        const auto phase = candidate.elapsedTime / controls.onDuration;
        if (!std::isfinite(phase))
            return Error::PhaseNonFinite;
        const auto interpolation = sampleLinear(
            table, phase, candidateOutput);
        if (interpolation != Error::None)
            return interpolation;

        const auto sampleDuration = 1.0 / config.sampleRate;
        if (!std::isfinite(sampleDuration) || sampleDuration <= 0.0)
            return Error::SampleDurationNonFinite;

        auto candidateElapsedTime =
            candidate.elapsedTime + sampleDuration;
        if (!std::isfinite(candidateElapsedTime)
            || candidateElapsedTime < 0.0)
        {
            return Error::StateStepNonFinite;
        }
        if (candidateElapsedTime > controls.onDuration)
            candidateElapsedTime = controls.onDuration;
        candidate.elapsedTime = candidateElapsedTime;
    }

    nextState = candidate;
    output = candidateOutput;
    return Error::None;
}
}
