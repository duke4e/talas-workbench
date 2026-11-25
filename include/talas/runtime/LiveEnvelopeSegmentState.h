#pragma once

#include <cstddef>

namespace talas::runtime
{
// Where a MultisegmentEnvelope or MultislopeFunctionGenerator with hot list
// elements is: the segment it plays, with
// the values that segment took as it started.
struct LiveEnvelopeSegmentState
{
    bool triggered = false;
    bool gateWasPositive = false;
    // Gate fell during a MultisegmentEnvelope's loop, which plays out once
    // more before the segments after it follow.
    bool released = false;
    bool finished = false;
    std::size_t segment = 0;
    double elapsed = 0.0;
    double duration = 0.0;
    double from = 0.0;
    // A MultisegmentEnvelope segment's end, a MultislopeFunctionGenerator
    // segment's slope.
    double to = 0.0;
    double slope = 0.0;

    bool operator==(const LiveEnvelopeSegmentState&) const = default;
};

// A MultisegmentEnvelope or GraphicalEnvelope falling rapidly to zero
// because its Gate went negative, and the value it
// fell from.
struct NegativeGateDecayState
{
    bool decaying = false;
    double from = 0.0;
    double elapsed = 0.0;
    // The envelope's latest output, where a decay starts.
    double lastOutput = 0.0;

    bool operator==(const NegativeGateDecayState&) const = default;
};
}
