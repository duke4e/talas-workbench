#pragma once

#include "CheckedBiquadMath.h"

#include <cmath>

namespace talas::runtime::graphic_eq
{
// A GraphicEQ's lowest band reaches from DC up to about 4 f
// (350 hz) and its highest from about 9 f (11,175 hz) up to half the
// sample rate; the five between are an octave wide.
inline constexpr double lowestBandEdge = 349.22823143300388;
inline constexpr double highestBandEdge = 11175.303405856124;

// The lowest band's one-pole low pass or the highest band's one-pole high
// pass, as biquad coefficients: moved to a level, either shelves the
// spectrum beyond its edge to that level, half way there at the edge.
[[nodiscard]] inline bool makeOuterBandCoefficients(
    double sampleRate,
    bool highest,
    checked_biquad::Coefficients& coefficients) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    const auto edge = highest ? highestBandEdge : lowestBandEdge;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || edge >= 0.5 * sampleRate)
        return false;
    const auto k = std::tan(pi * edge / sampleRate);
    if (!std::isfinite(k) || k <= 0.0)
        return false;
    checked_biquad::Coefficients candidate;
    candidate.b0 = (highest ? 1.0 : k) / (1.0 + k);
    candidate.b1 = highest ? -candidate.b0 : candidate.b0;
    candidate.a1 = (k - 1.0) / (k + 1.0);
    coefficients = candidate;
    return true;
}
}
