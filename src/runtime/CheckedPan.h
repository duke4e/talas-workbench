#pragma once

#include "talas/runtime/SpatialPanning.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_pan
{
inline constexpr std::size_t maximumOutputCount = 8;

enum class Law
{
    Power,
    Linear,
};

enum class Layout
{
    Stereo,
    Discrete4,
    Discrete8,
};

struct Configuration
{
    Law law = Law::Power;
    Layout layout = Layout::Stereo;
};

enum class Error
{
    None,
    SizeMismatch,
    InvalidConfiguration,
    InputNonFinite,
    PanInvalid,
    ScaleInvalid,
    ScaledInputNonFinite,
    GainNonFinite,
    FirstProductNonFinite,
    SecondProductNonFinite,
};

[[nodiscard]] inline std::size_t outputCount(
    Layout layout) noexcept
{
    switch (layout)
    {
    case Layout::Stereo:
        return 2;
    case Layout::Discrete4:
        return 4;
    case Layout::Discrete8:
        return 8;
    }
    return 0;
}

[[nodiscard]] inline Error advance(
    double input,
    double pan,
    double scale,
    Configuration configuration,
    std::array<double, maximumOutputCount>& output) noexcept
{
    if (configuration.law == Law::Linear
        && configuration.layout != Layout::Stereo)
    {
        return Error::InvalidConfiguration;
    }
    if (!std::isfinite(input))
        return Error::InputNonFinite;
    if (!std::isfinite(pan) || pan < 0.0 || pan > 1.0)
        return Error::PanInvalid;
    if (!std::isfinite(scale) || scale < 0.0)
        return Error::ScaleInvalid;

    const auto scaledInput = input * scale;
    if (!std::isfinite(scaledInput))
        return Error::ScaledInputNonFinite;

    auto candidate = std::array<double, maximumOutputCount>{};
    if (configuration.layout == Layout::Stereo)
    {
        constexpr double halfPi = 1.57079632679489661923;
        const auto firstGain = configuration.law == Law::Linear
            ? 1.0 - pan
            : std::cos(pan * halfPi);
        const auto secondGain = configuration.law == Law::Linear
            ? pan
            : std::sin(pan * halfPi);
        if (!std::isfinite(firstGain) || !std::isfinite(secondGain))
            return Error::GainNonFinite;
        const auto first = scaledInput * firstGain;
        if (!std::isfinite(first))
            return Error::FirstProductNonFinite;
        const auto second = scaledInput * secondGain;
        if (!std::isfinite(second))
            return Error::SecondProductNonFinite;
        candidate[0] = first;
        candidate[1] = second;
        output = candidate;
        return Error::None;
    }

    const auto count = outputCount(configuration.layout);
    const auto ring = equalPowerRingPan(pan, count);
    if (!ring.has_value()
        || !std::isfinite(ring->firstGain)
        || !std::isfinite(ring->secondGain))
    {
        return Error::GainNonFinite;
    }
    const auto first = scaledInput * ring->firstGain;
    if (!std::isfinite(first))
        return Error::FirstProductNonFinite;
    const auto second = scaledInput * ring->secondGain;
    if (!std::isfinite(second))
        return Error::SecondProductNonFinite;
    candidate[ring->firstChannel] += first;
    candidate[ring->secondChannel] += second;
    output = candidate;
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> input,
    std::optional<std::span<const double>> connectedPan,
    double staticPan,
    std::optional<std::span<const double>> connectedScale,
    double staticScale,
    Configuration configuration,
    std::span<std::span<double>> candidates,
    std::span<std::span<double>> outputs) noexcept
{
    const auto count = outputCount(configuration.layout);
    if (count == 0
        || candidates.size() != count
        || outputs.size() != count
        || (connectedPan.has_value()
            && connectedPan->size() != input.size())
        || (connectedScale.has_value()
            && connectedScale->size() != input.size()))
    {
        return Error::SizeMismatch;
    }
    for (std::size_t channel = 0; channel < count; ++channel)
    {
        if (candidates[channel].size() != input.size()
            || outputs[channel].size() != input.size())
        {
            return Error::SizeMismatch;
        }
    }

    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        auto frameOutputs =
            std::array<double, maximumOutputCount>{};
        const auto error = advance(
            input[frame],
            connectedPan.has_value()
                ? (*connectedPan)[frame]
                : staticPan,
            connectedScale.has_value()
                ? (*connectedScale)[frame]
                : staticScale,
            configuration,
            frameOutputs);
        if (error != Error::None)
            return error;
        for (std::size_t channel = 0; channel < count; ++channel)
            candidates[channel][frame] = frameOutputs[channel];
    }

    for (std::size_t channel = 0; channel < count; ++channel)
    {
        for (std::size_t frame = 0; frame < input.size(); ++frame)
            outputs[channel][frame] = candidates[channel][frame];
    }
    return Error::None;
}
}
