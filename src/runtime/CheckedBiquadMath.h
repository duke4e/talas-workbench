#pragma once

#include <cmath>

namespace talas::runtime::checked_biquad
{

struct Coefficients
{
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

struct State
{
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
};

[[nodiscard]] inline bool checkedAdd(
    double left, double right, double& result) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return false;
    const auto candidate = left + right;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool checkedSubtract(
    double left, double right, double& result) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return false;
    const auto candidate = left - right;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool checkedMultiply(
    double left, double right, double& result) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return false;
    const auto candidate = left * right;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool checkedDivide(
    double numerator, double denominator, double& result) noexcept
{
    if (!std::isfinite(numerator)
        || !std::isfinite(denominator)
        || denominator == 0.0)
    {
        return false;
    }
    const auto candidate = numerator / denominator;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool makeBandPassCoefficients(
    double sampleRate,
    double centerFrequency,
    double bandwidth,
    Coefficients& coefficients) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    if (!std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || !std::isfinite(centerFrequency)
        || centerFrequency <= 0.0
        || !std::isfinite(bandwidth)
        || bandwidth <= 0.0)
    {
        return false;
    }

    double nyquist = 0.0;
    double q = 0.0;
    double omegaNumerator = 0.0;
    double omega = 0.0;
    if (!checkedMultiply(sampleRate, 0.5, nyquist)
        || centerFrequency >= nyquist
        || !checkedDivide(centerFrequency, bandwidth, q)
        || q <= 0.0
        || !checkedMultiply(2.0, pi, omegaNumerator)
        || !checkedMultiply(
            omegaNumerator, centerFrequency, omegaNumerator)
        || !checkedDivide(omegaNumerator, sampleRate, omega))
    {
        return false;
    }

    const auto sinOmega = std::sin(omega);
    const auto cosOmega = std::cos(omega);
    if (!std::isfinite(sinOmega) || !std::isfinite(cosOmega))
        return false;

    double twoQ = 0.0;
    double alpha = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a0 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    if (!checkedMultiply(2.0, q, twoQ)
        || !checkedDivide(sinOmega, twoQ, alpha)
        || !checkedMultiply(-1.0, alpha, b2)
        || !checkedAdd(1.0, alpha, a0)
        || !checkedMultiply(-2.0, cosOmega, a1)
        || !checkedSubtract(1.0, alpha, a2))
    {
        return false;
    }

    Coefficients candidate;
    if (!checkedDivide(alpha, a0, candidate.b0)
        || !checkedDivide(b1, a0, candidate.b1)
        || !checkedDivide(b2, a0, candidate.b2)
        || !checkedDivide(a1, a0, candidate.a1)
        || !checkedDivide(a2, a0, candidate.a2))
    {
        return false;
    }
    coefficients = candidate;
    return true;
}

[[nodiscard]] inline bool makeFixedPoleCoefficients(
    double real,
    double imaginary,
    Coefficients& coefficients) noexcept
{
    double realSquared = 0.0;
    double imaginarySquared = 0.0;
    double radiusSquared = 0.0;
    double doubledReal = 0.0;
    if (!checkedMultiply(real, real, realSquared)
        || !checkedMultiply(imaginary, imaginary, imaginarySquared)
        || !checkedAdd(realSquared, imaginarySquared, radiusSquared)
        || radiusSquared > 1.0
        || !checkedMultiply(-2.0, real, doubledReal))
    {
        return false;
    }
    coefficients = {
        1.0,
        -2.0,
        1.0,
        doubledReal,
        radiusSquared,
    };
    return true;
}

[[nodiscard]] inline bool makeFormantPoleCoefficients(
    double sampleRate,
    double centerFrequency,
    double bandwidth,
    Coefficients& coefficients) noexcept
{
    // TwoFormantElement is documented as a DualParallelTwoPoleFilter, so it
    // uses that section's double zero at z=1. The frequency/bandwidth-to-pole
    // map below is an explicit candidate; the manuals do not give its formula.
    constexpr double pi = 3.1415926535897932384626433832795;
    if (!std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || !std::isfinite(centerFrequency)
        || centerFrequency <= 0.0
        || !std::isfinite(bandwidth)
        || bandwidth <= 0.0)
    {
        return false;
    }

    double nyquist = 0.0;
    double angle = 0.0;
    double decay = 0.0;
    if (!checkedMultiply(sampleRate, 0.5, nyquist)
        || centerFrequency >= nyquist
        || !checkedMultiply(2.0 * pi, centerFrequency, angle)
        || !checkedDivide(angle, sampleRate, angle)
        || !checkedMultiply(-pi, bandwidth, decay)
        || !checkedDivide(decay, sampleRate, decay))
    {
        return false;
    }

    const auto radius = std::exp(decay);
    double real = 0.0;
    double imaginary = 0.0;
    Coefficients candidate;
    if (!std::isfinite(radius)
        || radius >= 1.0
        || !checkedMultiply(radius, std::cos(angle), real)
        || !checkedMultiply(radius, std::sin(angle), imaginary)
        || !makeFixedPoleCoefficients(real, imaginary, candidate)
        || candidate.a2 >= 1.0)
    {
        return false;
    }
    coefficients = candidate;
    return true;
}

// A formant section scaled to unity gain at its centre frequency: the
// double zero at z=1 and the pole pair
// r*exp(+-j*theta) give a gain of 2(1 - cos theta) / ((1 - r) |1 - r
// exp(-2j*theta)|) there, so the numerator is multiplied by its inverse. As
// the bandwidth narrows, r nears 1 and the numerator, the share of new
// Input let in, nears 0: a narrower band passes less noise, and at a
// bandwidth near 0 no new energy enters while the poles ring on.
[[nodiscard]] inline bool makeUnityPeakFormantCoefficients(
    double sampleRate,
    double centerFrequency,
    double bandwidth,
    Coefficients& coefficients) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    Coefficients candidate;
    if (!makeFormantPoleCoefficients(
            sampleRate, centerFrequency, bandwidth, candidate))
    {
        return false;
    }
    const auto angle = 2.0 * pi * centerFrequency / sampleRate;
    const auto radius = std::exp(-pi * bandwidth / sampleRate);
    const auto zeroGain = 2.0 * (1.0 - std::cos(angle));
    const auto poleGain = (1.0 - radius)
        * std::sqrt(1.0 - 2.0 * radius * std::cos(2.0 * angle) + radius * radius);
    double scale = 0.0;
    if (!std::isfinite(angle)
        || !std::isfinite(radius)
        || !std::isfinite(poleGain)
        || poleGain < 0.0
        || !checkedDivide(poleGain, zeroGain, scale)
        || !checkedMultiply(candidate.b0, scale, candidate.b0)
        || !checkedMultiply(candidate.b1, scale, candidate.b1)
        || !checkedMultiply(candidate.b2, scale, candidate.b2))
    {
        return false;
    }
    coefficients = candidate;
    return true;
}

// The section a TwoFormantElement plays: unscaled, as projects have
// played it, or at unity gain at its formant when its normalization is
// unityPeak.
[[nodiscard]] inline bool makeTwoFormantSectionCoefficients(
    bool unityPeak,
    double sampleRate,
    double centerFrequency,
    double bandwidth,
    Coefficients& coefficients) noexcept
{
    return unityPeak
        ? makeUnityPeakFormantCoefficients(
            sampleRate, centerFrequency, bandwidth, coefficients)
        : makeFormantPoleCoefficients(
            sampleRate, centerFrequency, bandwidth, coefficients);
}

[[nodiscard]] inline bool makeFirstOrderAllpassCoefficient(
    double sampleRate,
    double frequency,
    double& coefficient) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    double nyquist = 0.0;
    double angle = 0.0;
    if (!std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || !std::isfinite(frequency)
        || frequency <= 0.0
        || !checkedMultiply(sampleRate, 0.5, nyquist)
        || frequency >= nyquist
        || !checkedMultiply(pi, frequency, angle)
        || !checkedDivide(angle, sampleRate, angle))
    {
        return false;
    }
    const auto tangent = std::tan(angle);
    if (!std::isfinite(tangent) || tangent <= 0.0)
        return false;
    double numerator = 0.0;
    double denominator = 0.0;
    double candidate = 0.0;
    if (!checkedSubtract(tangent, 1.0, numerator)
        || !checkedAdd(tangent, 1.0, denominator)
        || !checkedDivide(numerator, denominator, candidate)
        || std::abs(candidate) >= 1.0)
    {
        return false;
    }
    coefficient = candidate;
    return true;
}

template <typename StateType>
[[nodiscard]] inline bool processFirstOrderAllpassSample(
    double coefficient,
    StateType& state,
    double input,
    double& output) noexcept
{
    if (!std::isfinite(coefficient)
        || !std::isfinite(input)
        || !std::isfinite(state.x1)
        || !std::isfinite(state.y1))
    {
        return false;
    }
    double feed = 0.0;
    double feedback = 0.0;
    double candidateOutput = 0.0;
    if (!checkedMultiply(coefficient, input, feed)
        || !checkedMultiply(coefficient, state.y1, feedback)
        || !checkedAdd(feed, state.x1, candidateOutput)
        || !checkedSubtract(candidateOutput, feedback, candidateOutput))
    {
        return false;
    }
    StateType candidateState = state;
    candidateState.x1 = input;
    candidateState.y1 = candidateOutput;
    state = candidateState;
    output = candidateOutput;
    return true;
}

template <typename CoefficientsType, typename StateType>
[[nodiscard]] inline bool processSample(
    const CoefficientsType& coefficients,
    StateType& state,
    double input,
    double& output) noexcept
{
    if (!std::isfinite(input)
        || !std::isfinite(state.x1)
        || !std::isfinite(state.x2)
        || !std::isfinite(state.y1)
        || !std::isfinite(state.y2))
    {
        return false;
    }

    double feed0 = 0.0;
    double feed1 = 0.0;
    double feed2 = 0.0;
    double feedback1 = 0.0;
    double feedback2 = 0.0;
    if (!checkedMultiply(coefficients.b0, input, feed0)
        || !checkedMultiply(coefficients.b1, state.x1, feed1)
        || !checkedMultiply(coefficients.b2, state.x2, feed2)
        || !checkedMultiply(coefficients.a1, state.y1, feedback1)
        || !checkedMultiply(coefficients.a2, state.y2, feedback2))
    {
        return false;
    }

    double candidateOutput = 0.0;
    if (!checkedAdd(feed0, feed1, candidateOutput)
        || !checkedAdd(candidateOutput, feed2, candidateOutput)
        || !checkedSubtract(candidateOutput, feedback1, candidateOutput)
        || !checkedSubtract(candidateOutput, feedback2, candidateOutput))
    {
        return false;
    }

    StateType candidateState = state;
    candidateState.x2 = state.x1;
    candidateState.x1 = input;
    candidateState.y2 = state.y1;
    candidateState.y1 = candidateOutput;
    state = candidateState;
    output = candidateOutput;
    return true;
}

template <
    typename FirstCoefficientsType,
    typename FirstStateType,
    typename SecondCoefficientsType,
    typename SecondStateType>
[[nodiscard]] inline bool processParallelSample(
    const FirstCoefficientsType& firstCoefficients,
    FirstStateType& firstState,
    double firstScale,
    const SecondCoefficientsType& secondCoefficients,
    SecondStateType& secondState,
    double secondScale,
    double input,
    double& output) noexcept
{
    if (!std::isfinite(firstScale) || !std::isfinite(secondScale))
        return false;

    auto firstCandidate = firstState;
    auto secondCandidate = secondState;
    double firstOutput = 0.0;
    double secondOutput = 0.0;
    double firstScaled = 0.0;
    double secondScaled = 0.0;
    double candidateOutput = 0.0;
    if (!processSample(
            firstCoefficients, firstCandidate, input, firstOutput)
        || !processSample(
            secondCoefficients, secondCandidate, input, secondOutput)
        || !checkedMultiply(firstOutput, firstScale, firstScaled)
        || !checkedMultiply(secondOutput, secondScale, secondScaled)
        || !checkedAdd(firstScaled, secondScaled, candidateOutput))
    {
        return false;
    }

    firstState = firstCandidate;
    secondState = secondCandidate;
    output = candidateOutput;
    return true;
}

[[nodiscard]] inline bool advanceWrappedPhase(
    double phase,
    double frequency,
    double sampleRate,
    double& nextPhase) noexcept
{
    constexpr double twoPi = 6.283185307179586476925286766559;
    double nyquist = 0.0;
    double numerator = 0.0;
    double increment = 0.0;
    double candidate = 0.0;
    if (!std::isfinite(phase)
        || !std::isfinite(frequency)
        || frequency <= 0.0
        || !std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || !checkedMultiply(sampleRate, 0.5, nyquist)
        || frequency >= nyquist
        || !checkedMultiply(twoPi, frequency, numerator)
        || !checkedDivide(numerator, sampleRate, increment)
        || !checkedAdd(phase, increment, candidate))
    {
        return false;
    }
    if (candidate >= twoPi || candidate <= -twoPi)
        candidate = std::fmod(candidate, twoPi);
    if (!std::isfinite(candidate))
        return false;
    nextPhase = candidate;
    return true;
}

} // namespace talas::runtime::checked_biquad
