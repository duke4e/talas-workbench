#pragma once

#include "CheckedBiquadMath.h"

namespace talas::runtime::checked_analysis_filter
{

// Candidate for the documented harmonic-zero bin. This is the continuous
// center-frequency limit of the existing RBJ bandpass at fixed bandwidth:
// alpha = pi * bandwidth / Fs; H = alpha*(1+z^-1) /
// ((1+alpha) - (1-alpha)*z^-1).
[[nodiscard]] inline bool makeZeroHarmonicCoefficients(
    double sampleRate,
    double fundamental,
    checked_biquad::Coefficients& coefficients) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    double ratio = 0.0;
    double alpha = 0.0;
    double denominator = 0.0;
    double numerator = 0.0;
    double gain = 0.0;
    double pole = 0.0;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(fundamental) || fundamental <= 0.0
        || !checked_biquad::checkedDivide(fundamental, sampleRate, ratio)
        || !checked_biquad::checkedMultiply(pi, ratio, alpha)
        || !checked_biquad::checkedAdd(1.0, alpha, denominator)
        || !checked_biquad::checkedSubtract(alpha, 1.0, numerator)
        || !checked_biquad::checkedDivide(alpha, denominator, gain)
        || !checked_biquad::checkedDivide(numerator, denominator, pole)
        || gain <= 0.0 || std::abs(pole) >= 1.0)
    {
        return false;
    }
    coefficients = {gain, gain, 0.0, pole, 0.0};
    return true;
}

template <typename BandState, typename PhaseState>
[[nodiscard]] inline bool processZeroHarmonicSample(
    const checked_biquad::Coefficients& coefficients,
    BandState& bandState,
    PhaseState& phaseState,
    double input,
    double& left,
    double& right) noexcept
{
    if (!std::isfinite(phaseState.x1) || !std::isfinite(phaseState.x2)
        || !std::isfinite(phaseState.y1) || !std::isfinite(phaseState.y2))
    {
        return false;
    }
    auto candidate = bandState;
    double output = 0.0;
    if (!checked_biquad::processSample(coefficients, candidate, input, output))
        return false;

    bandState = candidate;
    // The DC component is real. Clearing its absent sine history also defines
    // the candidate's transition back to a positive harmonic.
    phaseState = {};
    left = output;
    right = 0.0;
    return true;
}

} // namespace talas::runtime::checked_analysis_filter
