#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_multislope_function_generator
{
struct EnvelopeView
{
    std::span<const double> timePoints;
    std::span<const double> slopes;
    std::span<const double> valuesAtTimePoints;
};

struct State
{
    double elapsedTime = 0.0;
    bool triggered = false;
    bool gateWasPositive = false;

    bool operator==(const State&) const = default;
};

struct Controls
{
    double level = 0.0;
    double gate = 0.0;
    double rate = 0.0;
    bool gateConnected = false;
};

struct Config
{
    double sampleRate = 0.0;
};

enum class Error
{
    None,
    EnvelopeSizeInvalid,
    TimePointNonFinite,
    SlopeNonFinite,
    FirstTimePointInvalid,
    TimePointsNotAscending,
    SegmentDurationNonFinite,
    DerivedDeltaNonFinite,
    DerivedValueNonFinite,
    StateInvalid,
    ControlsInvalid,
    ConfigInvalid,
    SegmentOffsetNonFinite,
    SegmentProductNonFinite,
    RawValueNonFinite,
    OutputNonFinite,
    SampleDurationNonFinite,
    ScaledStepNonFinite,
    StateStepNonFinite,
};

[[nodiscard]] inline Error validateEnvelopeDefinition(
    std::span<const double> timePoints,
    std::span<const double> slopes,
    double& finalDerivedValue) noexcept
{
    if (timePoints.size() < 2
        || timePoints.size() != slopes.size() + 1)
    {
        return Error::EnvelopeSizeInvalid;
    }
    for (const auto timePoint : timePoints)
    {
        if (!std::isfinite(timePoint))
            return Error::TimePointNonFinite;
    }
    for (const auto slope : slopes)
    {
        if (!std::isfinite(slope))
            return Error::SlopeNonFinite;
    }
    if (std::abs(timePoints.front()) > 1.0e-12)
        return Error::FirstTimePointInvalid;

    auto value = 1.0;
    for (std::size_t index = 0; index < slopes.size(); ++index)
    {
        if (timePoints[index + 1] <= timePoints[index])
            return Error::TimePointsNotAscending;
        const auto duration = timePoints[index + 1] - timePoints[index];
        if (!std::isfinite(duration) || duration <= 0.0)
            return Error::SegmentDurationNonFinite;
        const auto delta = slopes[index] * duration;
        if (!std::isfinite(delta))
            return Error::DerivedDeltaNonFinite;
        value += delta;
        if (!std::isfinite(value))
            return Error::DerivedValueNonFinite;
    }
    finalDerivedValue = value;
    return Error::None;
}

[[nodiscard]] inline Error prepareEnvelope(
    std::span<const double> timePoints,
    std::span<const double> slopes,
    std::span<double> valuesAtTimePoints) noexcept
{
    if (valuesAtTimePoints.size() != timePoints.size())
        return Error::EnvelopeSizeInvalid;

    auto ignoredFinalValue = 0.0;
    const auto validation = validateEnvelopeDefinition(
        timePoints, slopes, ignoredFinalValue);
    if (validation != Error::None)
        return validation;

    valuesAtTimePoints.front() = 1.0;
    for (std::size_t index = 0; index < slopes.size(); ++index)
    {
        const auto duration = timePoints[index + 1] - timePoints[index];
        const auto delta = slopes[index] * duration;
        valuesAtTimePoints[index + 1] =
            valuesAtTimePoints[index] + delta;
    }
    return Error::None;
}

[[nodiscard]] inline Error evaluateRaw(
    EnvelopeView envelope,
    double elapsedTime,
    double& raw) noexcept
{
    if (envelope.timePoints.size() < 2
        || envelope.timePoints.size() != envelope.slopes.size() + 1
        || envelope.valuesAtTimePoints.size() != envelope.timePoints.size())
    {
        return Error::EnvelopeSizeInvalid;
    }
    if (elapsedTime <= envelope.timePoints.front())
    {
        if (!std::isfinite(envelope.valuesAtTimePoints.front()))
            return Error::RawValueNonFinite;
        raw = envelope.valuesAtTimePoints.front();
        return Error::None;
    }
    if (elapsedTime >= envelope.timePoints.back())
    {
        raw = 1.0;
        return Error::None;
    }

    const auto upper = std::ranges::upper_bound(
        envelope.timePoints, elapsedTime);
    if (upper == envelope.timePoints.begin()
        || upper == envelope.timePoints.end())
    {
        return Error::StateInvalid;
    }
    const auto rightIndex = static_cast<std::size_t>(
        std::distance(envelope.timePoints.begin(), upper));
    const auto segmentIndex = rightIndex - 1;
    const auto offset =
        elapsedTime - envelope.timePoints[segmentIndex];
    if (!std::isfinite(offset))
        return Error::SegmentOffsetNonFinite;
    const auto product = envelope.slopes[segmentIndex] * offset;
    if (!std::isfinite(product))
        return Error::SegmentProductNonFinite;
    const auto candidateRaw =
        envelope.valuesAtTimePoints[segmentIndex] + product;
    if (!std::isfinite(candidateRaw))
        return Error::RawValueNonFinite;
    raw = candidateRaw;
    return Error::None;
}

[[nodiscard]] inline Error advanceFrame(
    EnvelopeView envelope,
    State state,
    Controls controls,
    Config config,
    State& nextState,
    double& output) noexcept
{
    if (envelope.timePoints.size() < 2
        || envelope.timePoints.size() != envelope.slopes.size() + 1
        || envelope.valuesAtTimePoints.size() != envelope.timePoints.size()
        || !std::isfinite(envelope.timePoints.front())
        || !std::isfinite(envelope.timePoints.back()))
    {
        return Error::EnvelopeSizeInvalid;
    }
    const auto finalElapsedTime = envelope.timePoints.back();
    const auto maximumRetainedElapsedTime =
        std::max(0.0, finalElapsedTime);
    if (!std::isfinite(state.elapsedTime) || state.elapsedTime < 0.0
        || state.elapsedTime > maximumRetainedElapsedTime
        || (!state.triggered
            && (state.elapsedTime != 0.0 || state.gateWasPositive)))
    {
        return Error::StateInvalid;
    }
    if (!std::isfinite(controls.level)
        || !std::isfinite(controls.gate)
        || !std::isfinite(controls.rate)
        || controls.rate <= 0.0)
    {
        return Error::ControlsInvalid;
    }
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0)
        return Error::ConfigInvalid;

    auto candidate = state;
    auto candidateOutput = 0.0;
    if (!controls.gateConnected && controls.gate <= 0.0)
    {
        candidate = {};
        candidateOutput = controls.level;
        nextState = candidate;
        output = candidateOutput;
        return Error::None;
    }

    if (!controls.gateConnected && !candidate.triggered)
    {
        candidate.elapsedTime = 0.0;
        candidate.triggered = true;
        candidate.gateWasPositive = true;
    }
    else if (controls.gateConnected)
    {
        const auto gateIsPositive = controls.gate > 0.0;
        if (gateIsPositive && !candidate.gateWasPositive)
        {
            candidate.elapsedTime = 0.0;
            candidate.triggered = true;
        }
        candidate.gateWasPositive = gateIsPositive;
        if (!candidate.triggered)
        {
            candidateOutput = controls.level;
            nextState = candidate;
            output = candidateOutput;
            return Error::None;
        }
    }

    auto raw = 0.0;
    const auto evaluated = evaluateRaw(
        envelope, candidate.elapsedTime, raw);
    if (evaluated != Error::None)
        return evaluated;
    candidateOutput = controls.level * raw;
    if (!std::isfinite(candidateOutput))
        return Error::OutputNonFinite;

    if (candidate.elapsedTime < finalElapsedTime)
    {
        const auto sampleDuration = 1.0 / config.sampleRate;
        if (!std::isfinite(sampleDuration) || sampleDuration <= 0.0)
            return Error::SampleDurationNonFinite;
        const auto step = sampleDuration * controls.rate;
        if (!std::isfinite(step) || step <= 0.0)
            return Error::ScaledStepNonFinite;
        auto candidateElapsedTime = candidate.elapsedTime + step;
        if (!std::isfinite(candidateElapsedTime)
            || candidateElapsedTime < 0.0)
        {
            return Error::StateStepNonFinite;
        }
        if (candidateElapsedTime > finalElapsedTime)
            candidateElapsedTime = finalElapsedTime;
        candidate.elapsedTime = candidateElapsedTime;
    }

    nextState = candidate;
    output = candidateOutput;
    return Error::None;
}
}
