#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_multisegment_envelope
{
struct EnvelopeView
{
    std::span<const double> timePoints;
    std::span<const double> breakPoints;
};

struct LoopWindow
{
    double startTime = 0.0;
    double endTime = 0.0;
    double startValue = 0.0;
    double endValue = 0.0;

    bool operator==(const LoopWindow&) const = default;
};

struct State
{
    double elapsedTime = 0.0;
    bool triggered = false;
    bool gateWasPositive = false;
    // Once Gate falls the loop is left where it was, the segments after it
    // following on by the drift its repeats had built up.
    bool released = false;
    double drift = 0.0;

    bool operator==(const State&) const = default;
};

struct Controls
{
    double level = 0.0;
    double gate = 0.0;
    bool gateConnected = false;
};

struct Config
{
    double sampleRate = 0.0;
    // The loop repeats only while Gate stays on, and the envelope
    // then plays on through its last segments; projects before format 73
    // repeat the loop until the next trigger. Gate is on while positive,
    // so a !KeyDown that dips to -1 retriggers it.
    bool loopWhileGate = false;
};

enum class Error
{
    None,
    EnvelopeSizeInvalid,
    DurationNonFinite,
    DurationNotPositive,
    BreakPointNonFinite,
    BreakPointNegative,
    CumulativeTimeNonFinite,
    TimePointNonFinite,
    TimePointsNotAscending,
    LoopControlNonFinite,
    LoopPairIncomplete,
    LoopStartInvalid,
    LoopEndInvalid,
    LoopEndBeforeStart,
    LoopEndOutOfRange,
    LoopDurationNonFinite,
    StateInvalid,
    ControlsInvalid,
    ConfigInvalid,
    SegmentWidthNonFinite,
    SegmentOffsetNonFinite,
    InterpolationAmountNonFinite,
    BreakPointDeltaNonFinite,
    InterpolationProductNonFinite,
    RawValueNonFinite,
    LoopElapsedNonFinite,
    CompletedLoopQuotientNonFinite,
    CompletedLoopCountNonFinite,
    LoopOffsetNonFinite,
    LoopDriftNonFinite,
    LoopLookupTimeNonFinite,
    OutputNonFinite,
    SampleDurationNonFinite,
    StateStepNonFinite,
};

[[nodiscard]] inline Error validateEnvelopeView(
    EnvelopeView envelope) noexcept
{
    if (envelope.timePoints.size() < 2
        || envelope.breakPoints.size() != envelope.timePoints.size())
    {
        return Error::EnvelopeSizeInvalid;
    }
    for (std::size_t index = 0; index < envelope.timePoints.size(); ++index)
    {
        if (!std::isfinite(envelope.timePoints[index]))
            return Error::TimePointNonFinite;
        if (index != 0
            && envelope.timePoints[index]
                <= envelope.timePoints[index - 1])
        {
            return Error::TimePointsNotAscending;
        }
        if (!std::isfinite(envelope.breakPoints[index]))
            return Error::BreakPointNonFinite;
        if (envelope.breakPoints[index] < 0.0)
            return Error::BreakPointNegative;
    }
    return Error::None;
}

[[nodiscard]] inline Error prepareEnvelope(
    std::span<const double> durations,
    std::span<const double> breakPoints,
    std::span<double> timePoints) noexcept
{
    if (durations.empty()
        || breakPoints.size() != durations.size() + 1
        || timePoints.size() != breakPoints.size())
    {
        return Error::EnvelopeSizeInvalid;
    }

    for (const auto duration : durations)
    {
        if (!std::isfinite(duration))
            return Error::DurationNonFinite;
        if (duration <= 0.0)
            return Error::DurationNotPositive;
    }
    for (const auto breakPoint : breakPoints)
    {
        if (!std::isfinite(breakPoint))
            return Error::BreakPointNonFinite;
        if (breakPoint < 0.0)
            return Error::BreakPointNegative;
    }

    auto cumulativeTime = 0.0;
    for (const auto duration : durations)
    {
        const auto nextTime = cumulativeTime + duration;
        if (!std::isfinite(nextTime) || nextTime <= cumulativeTime)
            return Error::CumulativeTimeNonFinite;
        cumulativeTime = nextTime;
    }

    timePoints.front() = 0.0;
    cumulativeTime = 0.0;
    for (std::size_t index = 0; index < durations.size(); ++index)
    {
        cumulativeTime += durations[index];
        timePoints[index + 1] = cumulativeTime;
    }
    return Error::None;
}

[[nodiscard]] inline Error prepareLoop(
    EnvelopeView envelope,
    double startLoop,
    double endLoop,
    std::optional<LoopWindow>& loop) noexcept
{
    const auto envelopeValidation = validateEnvelopeView(envelope);
    if (envelopeValidation != Error::None)
        return envelopeValidation;
    if (!std::isfinite(startLoop) || !std::isfinite(endLoop))
        return Error::LoopControlNonFinite;

    const auto hasStartLoop = std::abs(startLoop) > 1.0e-12;
    const auto hasEndLoop = std::abs(endLoop) > 1.0e-12;
    if (!hasStartLoop && !hasEndLoop)
    {
        loop.reset();
        return Error::None;
    }
    if (hasStartLoop != hasEndLoop)
        return Error::LoopPairIncomplete;
    if (startLoop < 2.0 || std::trunc(startLoop) != startLoop)
        return Error::LoopStartInvalid;
    if (endLoop <= 0.0 || std::trunc(endLoop) != endLoop)
        return Error::LoopEndInvalid;
    if (endLoop < startLoop)
        return Error::LoopEndBeforeStart;

    const auto segmentCount = envelope.timePoints.size() - 1;
    if (endLoop > static_cast<double>(segmentCount))
        return Error::LoopEndOutOfRange;
    const auto startSegment = static_cast<std::size_t>(startLoop);
    const auto endSegment = static_cast<std::size_t>(endLoop);
    const auto startTime = envelope.timePoints[startSegment - 1];
    const auto endTime = envelope.timePoints[endSegment];
    const auto duration = endTime - startTime;
    if (!std::isfinite(duration) || duration <= 0.0)
        return Error::LoopDurationNonFinite;

    loop = LoopWindow{
        startTime,
        endTime,
        envelope.breakPoints[startSegment - 1],
        envelope.breakPoints[endSegment],
    };
    return Error::None;
}

[[nodiscard]] inline Error evaluateRaw(
    EnvelopeView envelope,
    double playbackTime,
    double& raw) noexcept
{
    if (envelope.timePoints.size() < 2
        || envelope.breakPoints.size() != envelope.timePoints.size())
    {
        return Error::EnvelopeSizeInvalid;
    }
    if (playbackTime <= envelope.timePoints.front())
    {
        const auto candidate = envelope.breakPoints.front();
        if (!std::isfinite(candidate))
            return Error::RawValueNonFinite;
        raw = candidate;
        return Error::None;
    }
    if (playbackTime >= envelope.timePoints.back())
    {
        const auto candidate = envelope.breakPoints.back();
        if (!std::isfinite(candidate))
            return Error::RawValueNonFinite;
        raw = candidate;
        return Error::None;
    }

    const auto upper = std::ranges::upper_bound(
        envelope.timePoints, playbackTime);
    if (upper == envelope.timePoints.begin()
        || upper == envelope.timePoints.end())
    {
        return Error::StateInvalid;
    }
    const auto rightIndex = static_cast<std::size_t>(
        std::distance(envelope.timePoints.begin(), upper));
    const auto leftIndex = rightIndex - 1;
    const auto width = envelope.timePoints[rightIndex]
        - envelope.timePoints[leftIndex];
    if (!std::isfinite(width) || width <= 0.0)
        return Error::SegmentWidthNonFinite;
    const auto offset = playbackTime - envelope.timePoints[leftIndex];
    if (!std::isfinite(offset))
        return Error::SegmentOffsetNonFinite;
    const auto amount = offset / width;
    if (!std::isfinite(amount))
        return Error::InterpolationAmountNonFinite;
    const auto delta = envelope.breakPoints[rightIndex]
        - envelope.breakPoints[leftIndex];
    if (!std::isfinite(delta))
        return Error::BreakPointDeltaNonFinite;
    const auto product = delta * amount;
    if (!std::isfinite(product))
        return Error::InterpolationProductNonFinite;
    const auto candidate = envelope.breakPoints[leftIndex] + product;
    if (!std::isfinite(candidate))
        return Error::RawValueNonFinite;
    raw = candidate;
    return Error::None;
}

[[nodiscard]] inline Error evaluateEnvelope(
    EnvelopeView envelope,
    double playbackTime,
    double& value) noexcept
{
    auto raw = 0.0;
    const auto evaluated = evaluateRaw(envelope, playbackTime, raw);
    if (evaluated != Error::None)
        return evaluated;
    value = std::clamp(raw, 0.0, 1.0);
    return Error::None;
}

[[nodiscard]] inline Error evaluateLoopedEnvelope(
    EnvelopeView envelope,
    LoopWindow loop,
    double playbackTime,
    double& value) noexcept
{
    if (playbackTime < loop.startTime)
        return evaluateEnvelope(envelope, playbackTime, value);

    const auto loopDuration = loop.endTime - loop.startTime;
    if (!std::isfinite(loopDuration) || loopDuration <= 0.0)
        return Error::LoopDurationNonFinite;
    const auto loopElapsed = playbackTime - loop.startTime;
    if (!std::isfinite(loopElapsed) || loopElapsed < 0.0)
        return Error::LoopElapsedNonFinite;
    const auto completedLoopQuotient = loopElapsed / loopDuration;
    if (!std::isfinite(completedLoopQuotient))
        return Error::CompletedLoopQuotientNonFinite;
    const auto completedLoops = std::floor(completedLoopQuotient);
    if (!std::isfinite(completedLoops))
        return Error::CompletedLoopCountNonFinite;
    auto loopOffset = std::fmod(loopElapsed, loopDuration);
    if (!std::isfinite(loopOffset))
        return Error::LoopOffsetNonFinite;
    if (loopOffset < 0.0)
    {
        loopOffset += loopDuration;
        if (!std::isfinite(loopOffset))
            return Error::LoopOffsetNonFinite;
    }
    const auto endpointDelta = loop.endValue - loop.startValue;
    if (!std::isfinite(endpointDelta))
        return Error::BreakPointDeltaNonFinite;
    const auto drift = completedLoops * endpointDelta;
    if (!std::isfinite(drift))
        return Error::LoopDriftNonFinite;
    const auto lookupTime = loop.startTime + loopOffset;
    if (!std::isfinite(lookupTime))
        return Error::LoopLookupTimeNonFinite;
    auto raw = 0.0;
    const auto evaluated = evaluateRaw(envelope, lookupTime, raw);
    if (evaluated != Error::None)
        return evaluated;
    const auto drifted = raw + drift;
    if (!std::isfinite(drifted))
        return Error::LoopDriftNonFinite;
    value = std::clamp(drifted, 0.0, 1.0);
    return Error::None;
}

[[nodiscard]] inline Error advanceFrame(
    EnvelopeView envelope,
    std::optional<LoopWindow> loop,
    State state,
    Controls controls,
    Config config,
    State& nextState,
    double& output) noexcept
{
    if (envelope.timePoints.size() < 2
        || envelope.breakPoints.size() != envelope.timePoints.size()
        || !std::isfinite(envelope.timePoints.front())
        || !std::isfinite(envelope.timePoints.back()))
    {
        return Error::EnvelopeSizeInvalid;
    }
    const auto finalElapsedTime = envelope.timePoints.back();
    const auto looping = loop.has_value() && !state.released;
    if (!std::isfinite(state.elapsedTime) || state.elapsedTime < 0.0
        || !std::isfinite(state.drift)
        || (!looping && state.elapsedTime > finalElapsedTime)
        || (!state.triggered
            && (state.elapsedTime != 0.0 || state.gateWasPositive
                || state.released || state.drift != 0.0)))
    {
        return Error::StateInvalid;
    }
    if (!std::isfinite(controls.level)
        || controls.level < 0.0 || controls.level > 1.0
        || !std::isfinite(controls.gate))
    {
        return Error::ControlsInvalid;
    }
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0)
        return Error::ConfigInvalid;

    auto candidate = state;
    auto candidateOutput = 0.0;
    const auto gateOn = controls.gate > 0.0;
    if (!controls.gateConnected && !gateOn)
    {
        candidate = {};
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
        const auto gateIsPositive = gateOn;
        if (gateIsPositive && !candidate.gateWasPositive)
        {
            candidate.elapsedTime = 0.0;
            candidate.triggered = true;
            candidate.released = false;
            candidate.drift = 0.0;
        }
        else if (!gateIsPositive && candidate.gateWasPositive
            && candidate.triggered && config.loopWhileGate
            && loop.has_value() && !candidate.released)
        {
            // Leave the loop where it is: fold the time into its window
            // and keep the drift of the repeats completed.
            if (candidate.elapsedTime >= loop->startTime)
            {
                const auto loopDuration = loop->endTime - loop->startTime;
                if (!std::isfinite(loopDuration) || loopDuration <= 0.0)
                    return Error::LoopDurationNonFinite;
                const auto loopElapsed = candidate.elapsedTime - loop->startTime;
                const auto completedLoops = std::floor(loopElapsed / loopDuration);
                const auto loopOffset = loopElapsed - completedLoops * loopDuration;
                const auto drift = completedLoops * (loop->endValue - loop->startValue);
                if (!std::isfinite(completedLoops) || !std::isfinite(loopOffset)
                    || !std::isfinite(drift))
                {
                    return Error::LoopDriftNonFinite;
                }
                candidate.elapsedTime = std::clamp(
                    loop->startTime + loopOffset, loop->startTime, loop->endTime);
                candidate.drift = drift;
            }
            candidate.released = true;
        }
        candidate.gateWasPositive = gateIsPositive;
        if (!candidate.triggered)
        {
            nextState = candidate;
            output = candidateOutput;
            return Error::None;
        }
    }

    auto envelopeValue = 0.0;
    const auto candidateLooping = loop.has_value() && !candidate.released;
    auto evaluated = Error::None;
    if (candidateLooping)
    {
        evaluated = evaluateLoopedEnvelope(
            envelope, *loop, candidate.elapsedTime, envelopeValue);
    }
    else if (candidate.drift != 0.0)
    {
        auto raw = 0.0;
        evaluated = evaluateRaw(envelope, candidate.elapsedTime, raw);
        if (evaluated == Error::None)
        {
            if (!std::isfinite(raw + candidate.drift))
                return Error::LoopDriftNonFinite;
            envelopeValue = std::clamp(raw + candidate.drift, 0.0, 1.0);
        }
    }
    else
    {
        evaluated = evaluateEnvelope(
            envelope, candidate.elapsedTime, envelopeValue);
    }
    if (evaluated != Error::None)
        return evaluated;
    candidateOutput = controls.level * envelopeValue;
    if (!std::isfinite(candidateOutput))
        return Error::OutputNonFinite;

    if (candidateLooping || candidate.elapsedTime < finalElapsedTime)
    {
        const auto sampleDuration = 1.0 / config.sampleRate;
        if (!std::isfinite(sampleDuration) || sampleDuration <= 0.0)
            return Error::SampleDurationNonFinite;
        auto candidateElapsedTime = candidate.elapsedTime + sampleDuration;
        if (!std::isfinite(candidateElapsedTime)
            || candidateElapsedTime < 0.0)
        {
            return Error::StateStepNonFinite;
        }
        if (!candidateLooping
            && candidateElapsedTime > finalElapsedTime)
        {
            candidateElapsedTime = finalElapsedTime;
        }
        candidate.elapsedTime = candidateElapsedTime;
    }

    nextState = candidate;
    output = candidateOutput;
    return Error::None;
}
}
