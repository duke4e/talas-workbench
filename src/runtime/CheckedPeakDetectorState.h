#pragma once

#include <cmath>

namespace talas::runtime::checked_peak_detector
{
struct State
{
    double envelope = 0.0;
};

enum class Branch
{
    Attack,
    Release,
};

struct Selection
{
    Branch branch = Branch::Release;
    double amplitude = 0.0;
};

enum class Error
{
    None,
    SampleRateNonFiniteOrNonPositive,
    TimeNonFiniteOrNonPositive,
    FramesToSixtyPercentNonFinite,
    ExponentNonFinite,
    AlphaNonFinite,
    AudioInNonFinite,
    ScaleNonFiniteOrNegative,
    PreviousEnvelopeNonFinite,
    AbsoluteAmplitudeNonFinite,
    ScaledAmplitudeNonFinite,
    NewWeightNonFinite,
    RetainedWeightNonFinite,
    EnvelopeNonFinite,
};

[[nodiscard]] inline Error computeAlpha(
    double time,
    double sampleRate,
    double& alpha) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return Error::SampleRateNonFiniteOrNonPositive;
    if (!std::isfinite(time) || time <= 0.0)
        return Error::TimeNonFiniteOrNonPositive;

    const auto framesToSixtyPercent = time * sampleRate;
    if (!std::isfinite(framesToSixtyPercent))
        return Error::FramesToSixtyPercentNonFinite;
    const auto exponent = 1.0 / framesToSixtyPercent;
    if (!std::isfinite(exponent))
        return Error::ExponentNonFinite;
    const auto candidate = std::pow(0.4, exponent);
    if (!std::isfinite(candidate))
        return Error::AlphaNonFinite;

    alpha = candidate;
    return Error::None;
}

[[nodiscard]] inline Error select(
    const State& state,
    double audioIn,
    double scale,
    Selection& selection) noexcept
{
    if (!std::isfinite(audioIn))
        return Error::AudioInNonFinite;
    if (!std::isfinite(scale) || scale < 0.0)
        return Error::ScaleNonFiniteOrNegative;
    if (!std::isfinite(state.envelope))
        return Error::PreviousEnvelopeNonFinite;

    const auto absoluteAmplitude = std::abs(audioIn);
    if (!std::isfinite(absoluteAmplitude))
        return Error::AbsoluteAmplitudeNonFinite;
    const auto scaledAmplitude = absoluteAmplitude * scale;
    if (!std::isfinite(scaledAmplitude))
        return Error::ScaledAmplitudeNonFinite;

    selection = {
        scaledAmplitude > state.envelope ? Branch::Attack : Branch::Release,
        scaledAmplitude,
    };
    return Error::None;
}

[[nodiscard]] inline Error advance(
    State& state,
    const Selection& selection,
    double alpha,
    double& output) noexcept
{
    if (!std::isfinite(state.envelope))
        return Error::PreviousEnvelopeNonFinite;
    if (!std::isfinite(selection.amplitude))
        return Error::ScaledAmplitudeNonFinite;
    if (!std::isfinite(alpha))
        return Error::AlphaNonFinite;

    const auto newWeight = selection.amplitude * (1.0 - alpha);
    if (!std::isfinite(newWeight))
        return Error::NewWeightNonFinite;
    const auto retainedWeight = state.envelope * alpha;
    if (!std::isfinite(retainedWeight))
        return Error::RetainedWeightNonFinite;
    const auto envelope = newWeight + retainedWeight;
    if (!std::isfinite(envelope))
        return Error::EnvelopeNonFinite;

    state.envelope = envelope;
    output = envelope;
    return Error::None;
}
}
