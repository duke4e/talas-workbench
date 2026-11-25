#pragma once

#include <cmath>

namespace talas::runtime::checked_quad_oscillator
{
inline constexpr double twoPi =
    6.283185307179586476925286766559;

struct Frame
{
    double left = 0.0;
    double right = 0.0;
    double nextPhase = 0.0;
};

[[nodiscard]] inline bool isAdmittedFrequency(
    double sampleRate,
    double frequency) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(frequency))
    {
        return false;
    }
    const auto nyquist = sampleRate * 0.5;
    return std::isfinite(nyquist)
        && std::abs(frequency) < nyquist;
}

// Internal carrier path shared with SingleSideBandRM, whose existing profile
// also admits finite aliased carriers. The public QuadOscillator entry point
// below retains its strict Nyquist admission rule.
[[nodiscard]] inline bool processFiniteCarrierSample(
    double sampleRate,
    double phase,
    double leftEnvelope,
    double rightEnvelope,
    double frequency,
    Frame& result) noexcept
{
    if (!std::isfinite(phase)
        || !std::isfinite(leftEnvelope)
        || !std::isfinite(rightEnvelope)
        || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(frequency))
    {
        return false;
    }

    const auto sine = std::sin(phase);
    const auto cosine = std::cos(phase);
    if (!std::isfinite(sine) || !std::isfinite(cosine))
        return false;

    const auto left = leftEnvelope * sine;
    const auto right = rightEnvelope * cosine;
    const auto output = left + right;
    const auto incrementProduct = twoPi * frequency;
    if (!std::isfinite(left) || !std::isfinite(right)
        || !std::isfinite(output)
        || !std::isfinite(incrementProduct))
    {
        return false;
    }
    const auto increment = incrementProduct / sampleRate;
    if (!std::isfinite(increment))
        return false;

    auto nextPhase = phase + increment;
    if (!std::isfinite(nextPhase))
        return false;
    if (nextPhase >= twoPi || nextPhase <= -twoPi)
    {
        nextPhase = std::fmod(nextPhase, twoPi);
        if (!std::isfinite(nextPhase))
            return false;
    }

    // The QuadOscillator sums the two ring-modulated Envelope channels.
    // Retain rightOut as a mirror of the mono result for existing graphs.
    result = {output, output, nextPhase};
    return true;
}

[[nodiscard]] inline bool processSample(
    double sampleRate,
    double phase,
    double leftEnvelope,
    double rightEnvelope,
    double frequency,
    Frame& result) noexcept
{
    return isAdmittedFrequency(sampleRate, frequency)
        && processFiniteCarrierSample(sampleRate, phase, leftEnvelope,
            rightEnvelope, frequency, result);
}
}
