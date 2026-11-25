#pragma once

#include <cmath>

namespace talas::runtime::checked_amplitude_follower
{
struct State
{
    double envelope = 0.0;
};

enum class Error
{
    None,
    SampleRateNonFiniteOrNonPositive,
    TimeConstantNonFiniteOrNonPositive,
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
    double timeConstant,
    double sampleRate,
    double& alpha) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return Error::SampleRateNonFiniteOrNonPositive;
    if (!std::isfinite(timeConstant) || timeConstant <= 0.0)
        return Error::TimeConstantNonFiniteOrNonPositive;

    const auto framesToSixtyPercent = timeConstant * sampleRate;
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

[[nodiscard]] inline Error advance(
    State& state,
    double audioIn,
    double scale,
    double alpha,
    double& output) noexcept
{
    if (!std::isfinite(audioIn))
        return Error::AudioInNonFinite;
    if (!std::isfinite(scale) || scale < 0.0)
        return Error::ScaleNonFiniteOrNegative;
    if (!std::isfinite(alpha))
        return Error::AlphaNonFinite;
    if (!std::isfinite(state.envelope))
        return Error::PreviousEnvelopeNonFinite;

    const auto absoluteAmplitude = std::abs(audioIn);
    if (!std::isfinite(absoluteAmplitude))
        return Error::AbsoluteAmplitudeNonFinite;
    const auto scaledAmplitude = absoluteAmplitude * scale;
    if (!std::isfinite(scaledAmplitude))
        return Error::ScaledAmplitudeNonFinite;
    const auto newWeight = scaledAmplitude * (1.0 - alpha);
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
