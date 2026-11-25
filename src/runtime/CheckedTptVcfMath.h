#pragma once

#include "CheckedBiquadMath.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace talas::runtime::checked_tpt_vcf
{

inline constexpr std::size_t stageCount = 4;
using Stages = std::array<FilterStageState, stageCount>;

[[nodiscard]] inline bool processSample(
    double sampleRate,
    double input,
    double amplitude,
    double cutoff,
    double cutoffModulator,
    double cutoffModRange,
    double resonance,
    Stages& stages,
    double& output) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    constexpr double minimumCutoff = 1.0e-6;

    double nyquist = 0.0;
    if (!checked_biquad::checkedMultiply(sampleRate, 0.5, nyquist)
        || nyquist <= 0.0
        || !std::isfinite(input)
        || !std::isfinite(amplitude)
        || !std::isfinite(cutoff)
        || cutoff <= 0.0
        || cutoff >= nyquist
        || !std::isfinite(cutoffModulator)
        || !std::isfinite(cutoffModRange)
        || cutoffModRange < 0.0
        || !std::isfinite(resonance)
        || resonance < 0.0
        || resonance > 1.0)
    {
        return false;
    }

    for (const auto& stage : stages)
    {
        if (!std::isfinite(stage.ic1eq)
            || !std::isfinite(stage.ic2eq))
        {
            return false;
        }
    }

    double cutoffHeadroom = 0.0;
    if (!checked_biquad::checkedSubtract(
            nyquist, minimumCutoff, cutoffHeadroom))
    {
        return false;
    }
    const auto maximumCutoff =
        std::max(minimumCutoff, cutoffHeadroom);

    double modulation = 0.0;
    double rawCutoff = 0.0;
    if (!checked_biquad::checkedMultiply(
            cutoffModulator, cutoffModRange, modulation)
        || !checked_biquad::checkedAdd(
            cutoff, modulation, rawCutoff))
    {
        return false;
    }
    const auto effectiveCutoff = std::clamp(
        rawCutoff, minimumCutoff, maximumCutoff);

    double coefficientAngle = 0.0;
    if (!checked_biquad::checkedMultiply(
            pi, effectiveCutoff, coefficientAngle)
        || !checked_biquad::checkedDivide(
            coefficientAngle, sampleRate, coefficientAngle))
    {
        return false;
    }
    const auto g = std::tan(coefficientAngle);
    if (!std::isfinite(g) || g <= 0.0)
        return false;

    double stageDenominator = 0.0;
    double stageGain = 0.0;
    if (!checked_biquad::checkedAdd(1.0, g, stageDenominator)
        || !checked_biquad::checkedDivide(
            g, stageDenominator, stageGain))
    {
        return false;
    }

    // Zero-delay feedback (Zavalishin, The Art of VA Filter Design): each
    // stage outputs G*x + (1 - G)*s, so the ladder outputs G^4*u + S and the
    // loop u = x - k*y solves exactly. A Resonance of 1 (k = 4) rings at the
    // cutoff; a feedback one sample late rang flat.
    double oneMinusGain = 0.0;
    double stateSum = stages.front().ic1eq;
    for (std::size_t index = 1; index < stages.size(); ++index)
    {
        double weighted = 0.0;
        if (!checked_biquad::checkedMultiply(stateSum, stageGain, weighted)
            || !checked_biquad::checkedAdd(
                weighted, stages[index].ic1eq, stateSum))
        {
            return false;
        }
    }
    double ladderState = 0.0;
    double gainSquared = 0.0;
    double ladderGain = 0.0;
    double scaledInput = 0.0;
    double feedbackGain = 0.0;
    double drivenInput = 0.0;
    double numerator = 0.0;
    double loopGain = 0.0;
    double denominator = 0.0;
    double ladderOutput = 0.0;
    double feedback = 0.0;
    double sample = 0.0;
    if (!checked_biquad::checkedSubtract(1.0, stageGain, oneMinusGain)
        || !checked_biquad::checkedMultiply(
            stateSum, oneMinusGain, ladderState)
        || !checked_biquad::checkedMultiply(
            stageGain, stageGain, gainSquared)
        || !checked_biquad::checkedMultiply(
            gainSquared, gainSquared, ladderGain)
        || !checked_biquad::checkedMultiply(
            input, amplitude, scaledInput)
        || !checked_biquad::checkedMultiply(
            resonance, 4.0, feedbackGain)
        || !checked_biquad::checkedMultiply(
            ladderGain, scaledInput, drivenInput)
        || !checked_biquad::checkedAdd(
            drivenInput, ladderState, numerator)
        || !checked_biquad::checkedMultiply(
            feedbackGain, ladderGain, loopGain)
        || !checked_biquad::checkedAdd(1.0, loopGain, denominator)
        || !checked_biquad::checkedDivide(
            numerator, denominator, ladderOutput)
        || !checked_biquad::checkedMultiply(
            feedbackGain, ladderOutput, feedback)
        || !checked_biquad::checkedSubtract(
            scaledInput, feedback, sample))
    {
        return false;
    }

    auto candidate = stages;
    for (auto& stage : candidate)
    {
        double difference = 0.0;
        double v = 0.0;
        double y = 0.0;
        double nextIc1eq = 0.0;
        if (!checked_biquad::checkedSubtract(
                sample, stage.ic1eq, difference)
            || !checked_biquad::checkedMultiply(
                difference, stageGain, v)
            || !checked_biquad::checkedAdd(v, stage.ic1eq, y)
            || !checked_biquad::checkedAdd(y, v, nextIc1eq))
        {
            return false;
        }
        stage.ic1eq = nextIc1eq;
        stage.ic2eq = y;
        sample = y;
    }

    stages = candidate;
    output = sample;
    return true;
}

}
