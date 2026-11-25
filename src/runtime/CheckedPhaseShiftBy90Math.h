#pragma once

#include "CheckedBiquadMath.h"

#include <algorithm>

namespace talas::runtime::checked_phase_shift_by_90
{
struct Coefficients
{
    double advance = 0.0;
    double delay = 0.0;
};

// Contract: two IIR branches, +45/-45 degrees at the selected frequency.
// This unit-gain allpass realization is a candidate.
// H(a,z) = (a + z^-1)/(1 + a*z^-1). The advance branch is -H, tuned to
// -135 degrees before inversion; the delay branch is H tuned to -45 degrees.
[[nodiscard]] inline bool makeCoefficients(
    double sampleRate, double frequency, Coefficients& coefficients) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    constexpr double tanPiOverEight = 0.4142135623730950488016887242097;
    constexpr double tanThreePiOverEight = 2.4142135623730950488016887242097;
    double nyquist = 0.0;
    double angle = 0.0;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(frequency) || frequency <= 0.0
        || !checked_biquad::checkedMultiply(sampleRate, 0.5, nyquist)
        || frequency >= nyquist
        || !checked_biquad::checkedDivide(frequency, sampleRate, angle)
        || !checked_biquad::checkedMultiply(pi, angle, angle))
    {
        return false;
    }
    const auto tangent = std::tan(angle);
    if (!std::isfinite(tangent) || tangent <= 0.0)
        return false;
    const auto makeBranch = [tangent](double ratio, double& coefficient) {
        double tuned = 0.0;
        double numerator = 0.0;
        double denominator = 0.0;
        return checked_biquad::checkedMultiply(tangent, ratio, tuned)
            && checked_biquad::checkedSubtract(tuned, 1.0, numerator)
            && checked_biquad::checkedAdd(tuned, 1.0, denominator)
            && checked_biquad::checkedDivide(numerator, denominator, coefficient)
            && std::abs(coefficient) < 1.0;
    };
    Coefficients candidate;
    if (!makeBranch(tanPiOverEight, candidate.advance)
        || !makeBranch(tanThreePiOverEight, candidate.delay))
    {
        return false;
    }
    coefficients = candidate;
    return true;
}

// The filters are very sensitive to Input
// amplitude. As fixed-point filters do, a saturating branch runs in direct
// form II, w = x - a w1 and y = a w + w1, its state w and its output held to
// -1..1; the state's gain at the tuned frequency is about rate / (2 pi f),
// so a loud Input overflows it and distorts (starting from 0.05 is
// suggested). The state w1 is kept in x1.
template <typename StateType>
[[nodiscard]] inline bool processSaturatingBranch(
    double coefficient, StateType& state, double input, double& output) noexcept
{
    if (!std::isfinite(coefficient) || !std::isfinite(input) || !std::isfinite(state.x1))
        return false;
    const auto held = std::clamp(input - coefficient * state.x1, -1.0, 1.0);
    output = std::clamp(coefficient * held + state.x1, -1.0, 1.0);
    state.x1 = held;
    return true;
}

template <typename StateType>
[[nodiscard]] inline bool processSample(
    const Coefficients& coefficients,
    StateType& leftState, StateType& rightState,
    double leftInput, double rightInput,
    double& leftOutput, double& rightOutput,
    bool saturating = false) noexcept
{
    auto candidateLeft = leftState;
    auto candidateRight = rightState;
    double left = 0.0;
    double right = 0.0;
    if (saturating
            ? (!processSaturatingBranch(coefficients.advance, candidateLeft, leftInput, left)
                || !processSaturatingBranch(coefficients.delay, candidateRight, rightInput, right))
            : (!checked_biquad::processFirstOrderAllpassSample(
                   coefficients.advance, candidateLeft, leftInput, left)
                || !checked_biquad::processFirstOrderAllpassSample(
                    coefficients.delay, candidateRight, rightInput, right)))
    {
        return false;
    }
    leftState = candidateLeft;
    rightState = candidateRight;
    leftOutput = -left;
    rightOutput = right;
    return true;
}
}
