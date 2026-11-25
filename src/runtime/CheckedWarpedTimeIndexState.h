#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_warped_time_index
{

inline constexpr std::size_t maximumTimePoints = 4096;

enum class Error
{
    None,
    MapShapeInvalid,
    MapPointNonFinite,
    MapNotAscending,
    ScratchShapeInvalid,
    SampleRateInvalid,
    TriggerInvalid,
    RateInvalid,
    MorphInvalid,
    StateInvalid,
    PlaybackOverflow,
    OutputNonFinite,
};

struct MapView
{
    std::span<const double> idealTimePoints0;
    std::span<const double> idealTimePoints1;
    std::span<const double> currentTimePoints;
};

struct State
{
    double playbackTime = 0.0;
    bool active = false;
    bool triggerWasPositive = false;
};

struct Transition
{
    State nextState;
    double output = -1.0;
};

[[nodiscard]] inline bool strictlyAscendingFinite(
    std::span<const double> values) noexcept
{
    if (values.empty())
        return false;
    if (!std::isfinite(values.front()))
        return false;
    for (std::size_t index = 1; index < values.size(); ++index)
    {
        if (!std::isfinite(values[index])
            || !(values[index] > values[index - 1]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline Error prepareMap(
    MapView source,
    double morph,
    std::span<double> morphedIdealTimePoints) noexcept
{
    const auto pointCount = source.idealTimePoints0.size();
    if (pointCount < 2 || pointCount > maximumTimePoints
        || source.idealTimePoints1.size() != pointCount
        || source.currentTimePoints.size() != pointCount)
    {
        return Error::MapShapeInvalid;
    }
    if (morphedIdealTimePoints.size() != pointCount)
        return Error::ScratchShapeInvalid;
    if (!std::isfinite(morph) || morph < 0.0 || morph > 1.0)
        return Error::MorphInvalid;
    if (!strictlyAscendingFinite(source.idealTimePoints0)
        || !strictlyAscendingFinite(source.idealTimePoints1)
        || !strictlyAscendingFinite(source.currentTimePoints))
    {
        return Error::MapNotAscending;
    }

    double previous = 0.0;
    for (std::size_t index = 0; index < pointCount; ++index)
    {
        const auto value = std::lerp(
            source.idealTimePoints0[index],
            source.idealTimePoints1[index],
            morph);
        if (!std::isfinite(value))
            return Error::MapPointNonFinite;
        if (index != 0 && !(value > previous))
        {
            return Error::MapNotAscending;
        }
        previous = value;
    }
    for (std::size_t index = 0; index < pointCount; ++index)
    {
        const auto value = std::lerp(
            source.idealTimePoints0[index],
            source.idealTimePoints1[index],
            morph);
        morphedIdealTimePoints[index] = value;
    }
    return Error::None;
}

[[nodiscard]] inline Error stablePosition(
    double value,
    double lower,
    double upper,
    double& position) noexcept
{
    if (!std::isfinite(value) || !std::isfinite(lower)
        || !std::isfinite(upper) || !(upper > lower))
    {
        return Error::MapPointNonFinite;
    }
    if (value <= lower)
    {
        position = 0.0;
        return Error::None;
    }
    if (value >= upper)
    {
        position = 1.0;
        return Error::None;
    }
    const auto scale = std::max(std::abs(lower), std::abs(upper));
    if (!std::isfinite(scale) || scale <= 0.0)
        return Error::MapPointNonFinite;
    const auto scaledLower = lower / scale;
    const auto scaledUpper = upper / scale;
    const auto scaledValue = value / scale;
    const auto denominator = scaledUpper - scaledLower;
    if (!std::isfinite(scaledLower) || !std::isfinite(scaledUpper)
        || !std::isfinite(scaledValue) || !std::isfinite(denominator)
        || denominator <= 0.0)
    {
        return Error::MapPointNonFinite;
    }
    const auto candidate = (scaledValue - scaledLower) / denominator;
    if (!std::isfinite(candidate))
        return Error::MapPointNonFinite;
    position = std::clamp(candidate, 0.0, 1.0);
    return Error::None;
}

[[nodiscard]] inline Error evaluate(
    std::span<const double> idealTimePoints,
    std::span<const double> currentTimePoints,
    double playbackTime,
    double& output) noexcept
{
    if (idealTimePoints.size() < 2
        || currentTimePoints.size() != idealTimePoints.size())
    {
        return Error::MapShapeInvalid;
    }
    if (!std::isfinite(playbackTime) || playbackTime < 0.0)
        return Error::StateInvalid;
    if (playbackTime <= idealTimePoints.front())
    {
        output = -1.0;
        return Error::None;
    }
    if (playbackTime >= idealTimePoints.back())
    {
        output = 1.0;
        return Error::None;
    }

    const auto upper = std::upper_bound(
        idealTimePoints.begin(), idealTimePoints.end(), playbackTime);
    if (upper == idealTimePoints.begin() || upper == idealTimePoints.end())
        return Error::MapNotAscending;
    const auto rightIndex = static_cast<std::size_t>(
        std::distance(idealTimePoints.begin(), upper));
    const auto leftIndex = rightIndex - 1;
    double amount = 0.0;
    if (const auto error = stablePosition(
            playbackTime, idealTimePoints[leftIndex],
            idealTimePoints[rightIndex], amount);
        error != Error::None)
    {
        return error;
    }
    double currentLeft = 0.0;
    double currentRight = 0.0;
    if (const auto error = stablePosition(
            currentTimePoints[leftIndex], currentTimePoints.front(),
            currentTimePoints.back(), currentLeft);
        error != Error::None)
    {
        return error;
    }
    if (const auto error = stablePosition(
            currentTimePoints[rightIndex], currentTimePoints.front(),
            currentTimePoints.back(), currentRight);
        error != Error::None)
    {
        return error;
    }
    const auto normalizedLeft = -1.0 + 2.0 * currentLeft;
    const auto normalizedRight = -1.0 + 2.0 * currentRight;
    const auto candidate = std::lerp(
        normalizedLeft, normalizedRight, amount);
    if (!std::isfinite(candidate))
        return Error::OutputNonFinite;
    output = std::clamp(candidate, -1.0, 1.0);
    return Error::None;
}

[[nodiscard]] inline Error advance(
    MapView source,
    std::span<double> morphedIdealTimePoints,
    State state,
    double trigger,
    double rate,
    double morph,
    double sampleRate,
    bool resetWhenTriggerClosed,
    Transition& transition) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return Error::SampleRateInvalid;
    if (!std::isfinite(trigger))
        return Error::TriggerInvalid;
    if (!std::isfinite(rate) || rate < 0.0)
        return Error::RateInvalid;
    if (!std::isfinite(state.playbackTime) || state.playbackTime < 0.0)
        return Error::StateInvalid;
    const auto triggerIsPositive = trigger > 0.0;
    if (resetWhenTriggerClosed && !triggerIsPositive)
    {
        if (const auto error = prepareMap(
                source, morph, morphedIdealTimePoints);
            error != Error::None)
        {
            return error;
        }
        transition = {State{}, -1.0};
        return Error::None;
    }

    auto nextState = state;
    const auto triggerRisingEdge = triggerIsPositive
        && !state.triggerWasPositive;
    nextState.triggerWasPositive = triggerIsPositive;
    if (triggerRisingEdge || (!state.active && triggerIsPositive))
    {
        nextState.playbackTime = 0.0;
        nextState.active = true;
    }
    if (!nextState.active)
    {
        if (const auto error = prepareMap(
                source, morph, morphedIdealTimePoints);
            error != Error::None)
        {
            return error;
        }
        transition = {nextState, -1.0};
        return Error::None;
    }

    const auto increment = rate / sampleRate;
    const auto nextPlaybackTime = nextState.playbackTime + increment;
    if (!std::isfinite(increment) || increment < 0.0
        || !std::isfinite(nextPlaybackTime)
        || nextPlaybackTime < nextState.playbackTime)
    {
        return Error::PlaybackOverflow;
    }
    if (const auto error = prepareMap(
            source, morph, morphedIdealTimePoints);
        error != Error::None)
    {
        return error;
    }
    double output = 0.0;
    if (const auto error = evaluate(
            morphedIdealTimePoints, source.currentTimePoints,
            nextState.playbackTime, output);
        error != Error::None)
    {
        return error;
    }
    nextState.playbackTime = nextPlaybackTime;
    transition = {nextState, output};
    return Error::None;
}

} // namespace talas::runtime::checked_warped_time_index
