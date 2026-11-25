#pragma once

#include "CheckedPhaseShiftBy90Math.h"
#include "CheckedQuadOscillatorMath.h"

#include <cmath>

namespace talas::runtime::checked_single_side_band_rm
{

struct State
{
    checked_biquad::State left{};
    checked_biquad::State right{};
    double phase = 0.0;
};

[[nodiscard]] inline bool transition(
    double sampleRate,
    double frequency,
    double freqScale,
    double leftInput,
    double rightInput,
    const State& prior,
    State& next,
    double& output) noexcept
{
    if (!std::isfinite(freqScale)
        || freqScale <= 0.0
        || !std::isfinite(leftInput) || !std::isfinite(rightInput)
        || !std::isfinite(prior.left.x1) || !std::isfinite(prior.left.x2)
        || !std::isfinite(prior.left.y1) || !std::isfinite(prior.left.y2)
        || !std::isfinite(prior.right.x1) || !std::isfinite(prior.right.x2)
        || !std::isfinite(prior.right.y1) || !std::isfinite(prior.right.y2)
        || !std::isfinite(prior.phase))
    {
        return false;
    }

    checked_phase_shift_by_90::Coefficients coefficients;
    double scaleDelta = 0.0;
    double shiftFrequency = 0.0;
    if (!checked_phase_shift_by_90::makeCoefficients(
            sampleRate, frequency, coefficients)
        || !checked_biquad::checkedSubtract(
            freqScale, 1.0, scaleDelta)
        || !checked_biquad::checkedMultiply(
            frequency, scaleDelta, shiftFrequency))
    {
        return false;
    }

    auto candidate = prior;
    double leftEnvelope = 0.0;
    double rightEnvelope = 0.0;
    if (!checked_phase_shift_by_90::processSample(coefficients,
            candidate.left, candidate.right, leftInput, rightInput,
            leftEnvelope, rightEnvelope))
    {
        return false;
    }

    // Prototype construction: stereo PhaseShiftBy90 followed by
    // QuadOscillator. This shares both stages and their candidate conventions.
    checked_quad_oscillator::Frame frame;
    if (!checked_quad_oscillator::processFiniteCarrierSample(sampleRate,
            prior.phase, leftEnvelope, rightEnvelope, shiftFrequency, frame))
    {
        return false;
    }

    candidate.phase = frame.nextPhase;
    next = candidate;
    output = frame.left;
    return true;
}

} // namespace talas::runtime::checked_single_side_band_rm
