#pragma once

#include "talas/runtime/LiveEnvelopeSegmentState.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

// A MultisegmentEnvelope or MultislopeFunctionGenerator whose lists hold
// elements that follow Event Values, variables or time: hot BreakPoints,
// Durations, TimePoints or Slopes. It
// plays segment by segment, each segment taking its values as it starts and
// going on from where the last one left the envelope, since a segment has a
// slope rather than absolute values at its ends. Static lists keep
// their own time-indexed envelopes, which this matches.
namespace talas::runtime::live_envelope_segments
{
enum class Error
{
    None,
    ListInvalid,
    ControlsInvalid,
    ConfigInvalid,
    DurationInvalid,
    ValueNonFinite,
};

struct MultisegmentLists
{
    std::span<const double> durations;
    std::span<const double> breakPoints;
};

struct MultisegmentControls
{
    double level = 0.0;
    double gate = 0.0;
    bool gateConnected = false;
};

struct MultisegmentConfig
{
    double sampleRate = 0.0;
    // The loop's first and last segments, counted from 1; 0 for none.
    std::size_t startLoop = 0;
    std::size_t endLoop = 0;
    // The loop repeats while Gate is on; projects before format 73
    // repeat it until the next trigger.
    bool loopWhileGate = true;
};

// Segment `segment` takes its Duration and the change between its
// BreakPoints now, starting from where the envelope is.
[[nodiscard]] inline Error captureSegment(
    MultisegmentLists lists,
    std::size_t segment,
    LiveEnvelopeSegmentState& state) noexcept
{
    if (segment >= lists.durations.size()
        || segment + 1 >= lists.breakPoints.size())
    {
        return Error::ListInvalid;
    }
    const auto duration = lists.durations[segment];
    const auto start = lists.breakPoints[segment];
    const auto end = lists.breakPoints[segment + 1];
    if (!std::isfinite(duration) || duration <= 0.0)
        return Error::DurationInvalid;
    if (!std::isfinite(start) || !std::isfinite(end))
        return Error::ValueNonFinite;
    const auto to = state.from == start ? end : state.from + (end - start);
    if (!std::isfinite(to))
        return Error::ValueNonFinite;
    state.segment = segment;
    state.duration = duration;
    state.to = to;
    return Error::None;
}

[[nodiscard]] inline Error advanceMultisegment(
    MultisegmentLists lists,
    LiveEnvelopeSegmentState state,
    MultisegmentControls controls,
    MultisegmentConfig config,
    LiveEnvelopeSegmentState& nextState,
    double& output) noexcept
{
    const auto segments = lists.durations.size();
    if (segments == 0 || lists.breakPoints.size() != segments + 1
        || (config.startLoop != 0
            && (config.startLoop < 2 || config.endLoop < config.startLoop
                || config.endLoop > segments)))
    {
        return Error::ListInvalid;
    }
    if (!std::isfinite(controls.level) || controls.level < 0.0
        || controls.level > 1.0 || !std::isfinite(controls.gate))
    {
        return Error::ControlsInvalid;
    }
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0)
        return Error::ConfigInvalid;

    const auto looping = config.startLoop != 0;
    const auto gateOn = controls.gate > 0.0;
    auto candidate = state;
    if (!controls.gateConnected && !gateOn)
    {
        nextState = {};
        output = 0.0;
        return Error::None;
    }
    auto trigger = false;
    if (!controls.gateConnected)
    {
        trigger = !candidate.triggered;
        candidate.gateWasPositive = true;
    }
    else
    {
        if (gateOn && !candidate.gateWasPositive)
            trigger = true;
        else if (!gateOn && candidate.gateWasPositive && candidate.triggered
            && config.loopWhileGate && looping)
        {
            candidate.released = true;
        }
        candidate.gateWasPositive = gateOn;
        if (!trigger && !candidate.triggered)
        {
            nextState = candidate;
            output = 0.0;
            return Error::None;
        }
    }
    if (trigger)
    {
        candidate.triggered = true;
        candidate.released = false;
        candidate.finished = false;
        candidate.elapsed = 0.0;
        candidate.from = lists.breakPoints.front();
        if (!std::isfinite(candidate.from))
            return Error::ValueNonFinite;
        if (const auto captured = captureSegment(lists, 0, candidate);
            captured != Error::None)
        {
            return captured;
        }
    }

    const auto raw = candidate.finished
        ? candidate.to
        : candidate.from
            + (candidate.to - candidate.from)
                * (candidate.elapsed / candidate.duration);
    const auto value = controls.level * std::clamp(raw, 0.0, 1.0);
    if (!std::isfinite(raw) || !std::isfinite(value))
        return Error::ValueNonFinite;

    if (!candidate.finished)
    {
        candidate.elapsed += 1.0 / config.sampleRate;
        // A frame crosses at most every segment once.
        for (std::size_t pass = 0;
             pass <= segments && candidate.elapsed >= candidate.duration;
             ++pass)
        {
            candidate.elapsed -= candidate.duration;
            candidate.from = candidate.to;
            auto next = candidate.segment + 1;
            if (looping && !candidate.released && next == config.endLoop)
                next = config.startLoop - 1;
            if (next >= segments)
            {
                candidate.finished = true;
                candidate.elapsed = 0.0;
                break;
            }
            if (const auto captured = captureSegment(lists, next, candidate);
                captured != Error::None)
            {
                return captured;
            }
        }
        if (!std::isfinite(candidate.elapsed))
            return Error::ValueNonFinite;
    }

    nextState = candidate;
    output = value;
    return Error::None;
}

struct MultislopeLists
{
    std::span<const double> timePoints;
    std::span<const double> slopes;
};

struct MultislopeControls
{
    double level = 0.0;
    double gate = 0.0;
    double rate = 0.0;
    bool gateConnected = false;
};

// Segment `segment` takes the time between its TimePoints and its Slope
// now.
[[nodiscard]] inline Error captureSlope(
    MultislopeLists lists,
    std::size_t segment,
    LiveEnvelopeSegmentState& state) noexcept
{
    if (segment >= lists.slopes.size()
        || segment + 1 >= lists.timePoints.size())
    {
        return Error::ListInvalid;
    }
    const auto duration =
        lists.timePoints[segment + 1] - lists.timePoints[segment];
    const auto slope = lists.slopes[segment];
    if (!std::isfinite(duration) || duration <= 0.0)
        return Error::DurationInvalid;
    if (!std::isfinite(slope))
        return Error::ValueNonFinite;
    state.segment = segment;
    state.duration = duration;
    state.slope = slope;
    return Error::None;
}

// The envelope rests at 1 and, triggered, plays once at Rate.
[[nodiscard]] inline Error advanceMultislope(
    MultislopeLists lists,
    LiveEnvelopeSegmentState state,
    MultislopeControls controls,
    double sampleRate,
    LiveEnvelopeSegmentState& nextState,
    double& output) noexcept
{
    const auto segments = lists.slopes.size();
    if (segments == 0 || lists.timePoints.size() != segments + 1)
        return Error::ListInvalid;
    if (!std::isfinite(controls.level) || !std::isfinite(controls.gate)
        || !std::isfinite(controls.rate) || controls.rate <= 0.0)
    {
        return Error::ControlsInvalid;
    }
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return Error::ConfigInvalid;

    const auto gateOn = controls.gate > 0.0;
    auto candidate = state;
    if (!controls.gateConnected && !gateOn)
    {
        nextState = {};
        output = controls.level;
        return Error::None;
    }
    auto trigger = false;
    if (!controls.gateConnected)
    {
        trigger = !candidate.triggered;
        candidate.gateWasPositive = true;
    }
    else
    {
        trigger = gateOn && !candidate.gateWasPositive;
        candidate.gateWasPositive = gateOn;
        if (!trigger && !candidate.triggered)
        {
            nextState = candidate;
            output = controls.level;
            return Error::None;
        }
    }
    if (trigger)
    {
        candidate.triggered = true;
        candidate.finished = false;
        candidate.elapsed = 0.0;
        candidate.from = 1.0;
        if (const auto captured = captureSlope(lists, 0, candidate);
            captured != Error::None)
        {
            return captured;
        }
    }

    const auto raw = candidate.finished
        ? 1.0
        : candidate.from + candidate.slope * candidate.elapsed;
    const auto value = controls.level * raw;
    if (!std::isfinite(raw) || !std::isfinite(value))
        return Error::ValueNonFinite;

    if (!candidate.finished)
    {
        candidate.elapsed += controls.rate / sampleRate;
        for (std::size_t pass = 0;
             pass <= segments && candidate.elapsed >= candidate.duration;
             ++pass)
        {
            candidate.elapsed -= candidate.duration;
            candidate.from += candidate.slope * candidate.duration;
            const auto next = candidate.segment + 1;
            if (next >= segments)
            {
                candidate.finished = true;
                candidate.elapsed = 0.0;
                break;
            }
            if (const auto captured = captureSlope(lists, next, candidate);
                captured != Error::None)
            {
                return captured;
            }
        }
        if (!std::isfinite(candidate.elapsed) || !std::isfinite(candidate.from))
            return Error::ValueNonFinite;
    }

    nextState = candidate;
    output = value;
    return Error::None;
}
}
