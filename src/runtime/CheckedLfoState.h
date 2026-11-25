#pragma once

#include <cmath>

namespace talas::runtime::checked_lfo
{
inline constexpr double twoPi =
    6.283185307179586476925286766559;

struct State
{
    double phase = 0.0;
};

struct Controls
{
    double frequency = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double sampleRate = 0.0;
};

enum class Error
{
    None,
    SampleRateNonFiniteOrNonPositive,
    FrequencyNonFiniteOrNegative,
    MinimumNonFinite,
    MaximumNonFinite,
    PhaseNonFiniteOrOutOfRange,
    CenterSumNonFinite,
    CenterNonFinite,
    RangeNonFinite,
    AmplitudeNonFinite,
    SineNonFinite,
    ScaledSineNonFinite,
    OutputNonFinite,
    PhaseProductNonFinite,
    PhaseIncrementNonFinite,
    AdvancedPhaseNonFinite,
    WrappedPhaseNonFiniteOrOutOfRange,
};

[[nodiscard]] inline Error advance(
    const State& state,
    const Controls& controls,
    State& candidate,
    double& output) noexcept
{
    if (!std::isfinite(controls.sampleRate)
        || controls.sampleRate <= 0.0)
        return Error::SampleRateNonFiniteOrNonPositive;
    if (!std::isfinite(controls.frequency)
        || controls.frequency < 0.0)
        return Error::FrequencyNonFiniteOrNegative;
    if (!std::isfinite(controls.minimum))
        return Error::MinimumNonFinite;
    if (!std::isfinite(controls.maximum))
        return Error::MaximumNonFinite;
    if (!std::isfinite(state.phase)
        || state.phase < 0.0 || state.phase >= twoPi)
        return Error::PhaseNonFiniteOrOutOfRange;

    const auto centerSum = controls.minimum + controls.maximum;
    if (!std::isfinite(centerSum))
        return Error::CenterSumNonFinite;
    const auto center = centerSum * 0.5;
    if (!std::isfinite(center))
        return Error::CenterNonFinite;
    const auto range = controls.maximum - controls.minimum;
    if (!std::isfinite(range))
        return Error::RangeNonFinite;
    const auto amplitude = range * 0.5;
    if (!std::isfinite(amplitude))
        return Error::AmplitudeNonFinite;
    const auto sine = std::sin(state.phase);
    if (!std::isfinite(sine))
        return Error::SineNonFinite;
    const auto scaledSine = amplitude * sine;
    if (!std::isfinite(scaledSine))
        return Error::ScaledSineNonFinite;
    const auto candidateOutput = center + scaledSine;
    if (!std::isfinite(candidateOutput))
        return Error::OutputNonFinite;

    const auto phaseProduct = twoPi * controls.frequency;
    if (!std::isfinite(phaseProduct))
        return Error::PhaseProductNonFinite;
    const auto increment = phaseProduct / controls.sampleRate;
    if (!std::isfinite(increment))
        return Error::PhaseIncrementNonFinite;
    const auto advancedPhase = state.phase + increment;
    if (!std::isfinite(advancedPhase))
        return Error::AdvancedPhaseNonFinite;
    auto nextPhase = advancedPhase;
    if (nextPhase >= twoPi || nextPhase <= -twoPi)
        nextPhase = std::fmod(nextPhase, twoPi);
    if (!std::isfinite(nextPhase)
        || nextPhase < 0.0 || nextPhase >= twoPi)
        return Error::WrappedPhaseNonFiniteOrOutOfRange;

    candidate = {nextPhase};
    output = candidateOutput;
    return Error::None;
}
}
