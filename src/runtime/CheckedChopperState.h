#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>

namespace talas::runtime::checked_chopper
{
struct State
{
    double elapsedTime = 0.0;

    bool operator==(const State&) const = default;
};

struct Controls
{
    double inputSample = 0.0;
    double grainDuration = 0.0;
    double interGrainDelay = 0.0;
};

struct Config
{
    double sampleRate = 0.0;
};

// A Chopper's timing: "grainDurationAndDelay", a GrainDuration and an
// InterGrainDelay and what projects without the field play, or
// "rateAndDutyCycle", a Chopper with a DutyCycle control.
[[nodiscard]] inline std::optional<bool> timedByRateAndDutyCycle(
    const std::optional<std::string>& choice) noexcept
{
    if (!choice.has_value() || *choice == "grainDurationAndDelay")
        return false;
    if (*choice == "rateAndDutyCycle")
        return true;
    return std::nullopt;
}

// A DutyCycle is the part of each beat during which
// the gate is on. Timed by a Rate of beats a second, each beat holds a
// grain lasting DutyCycle / Rate and a silence for the rest of it. A
// DutyCycle of 0 would never open the gate and one of 1 never close it, so
// it lies between them, as a GrainDuration and an InterGrainDelay lie above
// 0. False when the Rate or DutyCycle is out
// of range.
[[nodiscard]] inline bool beatTiming(
    double rate,
    double dutyCycle,
    double& grainDuration,
    double& interGrainDelay) noexcept
{
    if (!std::isfinite(rate) || rate <= 0.0 || !std::isfinite(dutyCycle)
        || dutyCycle <= 0.0 || dutyCycle >= 1.0)
    {
        return false;
    }
    const auto beat = 1.0 / rate;
    const auto on = dutyCycle * beat;
    const auto off = beat - on;
    if (!std::isfinite(beat) || !std::isfinite(on) || on <= 0.0
        || !std::isfinite(off) || off <= 0.0)
    {
        return false;
    }
    grainDuration = on;
    interGrainDelay = off;
    return true;
}

enum class Error
{
    None,
    TableInvalid,
    StateInvalid,
    ControlsInvalid,
    ConfigInvalid,
    SampleDurationNonFinite,
    CycleDurationNonFinite,
    WrapNonFinite,
    PhaseNonFinite,
    InterpolationNonFinite,
    OutputNonFinite,
    StateStepNonFinite,
};

[[nodiscard]] inline Error sampleLinear(
    std::span<const double> table,
    double normalizedPosition,
    double& output) noexcept
{
    if (table.empty())
        return Error::TableInvalid;
    if (!std::isfinite(normalizedPosition))
        return Error::PhaseNonFinite;
    if (table.size() == 1)
    {
        if (!std::isfinite(table.front()))
            return Error::InterpolationNonFinite;
        output = table.front();
        return Error::None;
    }

    const auto clamped = std::clamp(normalizedPosition, 0.0, 1.0);
    const auto scaled =
        clamped * static_cast<double>(table.size() - 1);
    if (!std::isfinite(clamped) || !std::isfinite(scaled))
        return Error::InterpolationNonFinite;
    const auto leftIndex =
        static_cast<std::size_t>(std::floor(scaled));
    const auto rightIndex =
        std::min(leftIndex + 1, table.size() - 1);
    const auto amount = scaled - static_cast<double>(leftIndex);
    const auto left = table[leftIndex];
    const auto right = table[rightIndex];
    const auto difference = right - left;
    const auto scaledDifference = difference * amount;
    const auto interpolated = left + scaledDifference;
    if (!std::isfinite(left) || !std::isfinite(right)
        || !std::isfinite(amount) || !std::isfinite(difference)
        || !std::isfinite(scaledDifference)
        || !std::isfinite(interpolated))
    {
        return Error::InterpolationNonFinite;
    }
    output = interpolated;
    return Error::None;
}

[[nodiscard]] inline Error advanceFrame(
    std::span<const double> table,
    State state,
    Controls controls,
    Config config,
    State& nextState,
    double& output) noexcept
{
    if (table.empty())
        return Error::TableInvalid;
    if (!std::isfinite(state.elapsedTime)
        || state.elapsedTime < 0.0)
    {
        return Error::StateInvalid;
    }
    if (!std::isfinite(controls.grainDuration)
        || controls.grainDuration <= 0.0
        || !std::isfinite(controls.interGrainDelay)
        || controls.interGrainDelay <= 0.0)
    {
        return Error::ControlsInvalid;
    }
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0)
        return Error::ConfigInvalid;

    const auto sampleDuration = 1.0 / config.sampleRate;
    if (!std::isfinite(sampleDuration) || sampleDuration <= 0.0)
        return Error::SampleDurationNonFinite;
    const auto cycleDuration =
        controls.grainDuration + controls.interGrainDelay;
    if (!std::isfinite(cycleDuration) || cycleDuration <= 0.0)
        return Error::CycleDurationNonFinite;

    // Elapsed time grows one sample at a time, so grain and cycle edges are
    // decided on whole samples: rounding drift must not make a grain one
    // sample longer or its silence one sample shorter.
    const auto samples = [&] (double seconds) noexcept {
        return std::round(seconds * config.sampleRate);
    };
    const auto wrap = [&] (double elapsed, double& wrapped) noexcept {
        if (samples(elapsed) < samples(cycleDuration))
        {
            wrapped = elapsed;
            return true;
        }
        wrapped = std::fmod(elapsed, cycleDuration);
        if (samples(wrapped) >= samples(cycleDuration))
            wrapped = 0.0;
        return std::isfinite(wrapped) && wrapped >= 0.0;
    };
    auto candidateElapsedTime = state.elapsedTime;
    if (!wrap(candidateElapsedTime, candidateElapsedTime))
        return Error::WrapNonFinite;

    auto envelope = 0.0;
    if (samples(candidateElapsedTime) < samples(controls.grainDuration))
    {
        const auto phase =
            candidateElapsedTime / controls.grainDuration;
        if (!std::isfinite(phase))
            return Error::PhaseNonFinite;
        const auto interpolation = sampleLinear(table, phase, envelope);
        if (interpolation != Error::None)
            return interpolation;
    }
    if (!std::isfinite(controls.inputSample))
        return Error::ControlsInvalid;
    const auto candidateOutput = controls.inputSample * envelope;
    if (!std::isfinite(candidateOutput))
        return Error::OutputNonFinite;

    candidateElapsedTime += sampleDuration;
    if (!std::isfinite(candidateElapsedTime)
        || candidateElapsedTime < 0.0)
    {
        return Error::StateStepNonFinite;
    }
    if (!wrap(candidateElapsedTime, candidateElapsedTime))
        return Error::WrapNonFinite;

    nextState.elapsedTime = candidateElapsedTime;
    output = candidateOutput;
    return Error::None;
}
}
