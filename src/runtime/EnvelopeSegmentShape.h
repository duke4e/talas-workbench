#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "talas/runtime/LiveEnvelopeSegmentState.h"

// The shape of an ADSR's and an AR's segments.
// A linear segment covers its distance at a constant rate. An
// exponential one approaches its level as a charging capacitor does and
// comes within 60 dB of it at the end of its time, where it lands on it;
// no curve is specified, so the 60 dB is ours.
namespace talas::runtime::envelope_segment
{
// ln 1000: e to the minus this is the 60 dB left at a segment's end.
inline constexpr double exponentialRate = 6.907755278982137;

// A Gate of -1 tells an envelope to decay rapidly
// to zero. Ours then runs its release in at most this long, well inside
// the 5 ms a note-on holds !KeyDown at -1 (the time is ours).
inline constexpr double rapidDecaySeconds = 0.002;

// How many times faster than ReleaseTime a release runs at this Gate.
[[nodiscard]] inline double releaseSpeed(
    double releaseTime, double gate, bool negativeGateDecays) noexcept
{
    if (!negativeGateDecays || !(gate < 0.0) || !(releaseTime > rapidDecaySeconds))
        return 1.0;
    return releaseTime / rapidDecaySeconds;
}

// A MultisegmentEnvelope or GraphicalEnvelope has no release, so a Gate
// that goes negative while it plays makes it fall from where it is to zero,
// in a straight line over rapidDecaySeconds, and wait there for the Gate to
// become positive and trigger it again. Once begun, the fall runs to zero unless the Gate
// becomes positive first. Whether the Gate going negative now starts a
// fall: the caller then resets its envelope to wait for a trigger.
[[nodiscard]] inline bool beginsNegativeGateDecay(
    NegativeGateDecayState& decay, bool enabled, double gate, bool triggered) noexcept
{
    if (!enabled || !(gate < 0.0) || decay.decaying || !triggered)
        return false;
    decay.decaying = true;
    decay.from = decay.lastOutput;
    decay.elapsed = 0.0;
    return true;
}

// Whether the fall owns this frame, and if so its output.
[[nodiscard]] inline bool advanceNegativeGateDecay(
    NegativeGateDecayState& decay, double gate, double sampleRate, double& output) noexcept
{
    // A Gate that is not finite is the envelope's to refuse.
    if (!decay.decaying || !std::isfinite(gate))
        return false;
    if (gate > 0.0)
    {
        decay.decaying = false;
        return false;
    }
    const auto remaining = 1.0 - decay.elapsed / rapidDecaySeconds;
    if (!(remaining > 0.0))
    {
        decay = {};
        output = 0.0;
        return true;
    }
    output = decay.from * remaining;
    decay.elapsed += 1.0 / sampleRate;
    decay.lastOutput = output;
    return true;
}

// The fraction of its distance a segment has covered after `ratio` of its
// time.
[[nodiscard]] inline double covered(double ratio, bool exponential) noexcept
{
    const auto clamped = std::clamp(ratio, 0.0, 1.0);
    if (!exponential)
        return clamped;
    return clamped >= 1.0 ? 1.0 : 1.0 - std::exp(-exponentialRate * clamped);
}

// The ratio of its time a segment takes to cover `fraction` of its
// distance: where a retriggered attack resumes from the level it has.
[[nodiscard]] inline double ratioFor(double fraction, bool exponential) noexcept
{
    const auto clamped = std::clamp(fraction, 0.0, 1.0);
    if (!exponential)
        return clamped;
    const auto remaining = 1.0 - clamped;
    if (remaining <= std::exp(-exponentialRate))
        return 1.0;
    return -std::log(remaining) / exponentialRate;
}
}

namespace talas::runtime
{
// An ADSR's or an AR's Type.
[[nodiscard]] inline bool isEnvelopeSegmentShape(
    const std::optional<std::string>& choice) noexcept
{
    return choice.has_value()
        && (*choice == "linear" || *choice == "exponential");
}
}
