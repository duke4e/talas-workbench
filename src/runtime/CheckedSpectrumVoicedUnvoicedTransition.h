#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_spectrum_voiced_unvoiced
{
struct State
{
    bool initialized = false;
    bool stableClassification = false;
};

struct Transition
{
    double value = 0.0;
    State candidate;
};

enum class ClassificationError
{
    None,
    InvalidCount,
    InvalidFrame,
    InvalidPartial,
};

[[nodiscard]] inline ClassificationError classify(
    std::span<const double> spectrum,
    std::size_t maximumPartials,
    bool& voiced) noexcept
{
    voiced = false;
    if (spectrum.empty() || !std::isfinite(spectrum.front())
        || spectrum.front() < 1.0
        || spectrum.front() > static_cast<double>(maximumPartials)
        || std::abs(spectrum.front() - std::round(spectrum.front()))
            > 1.0e-9)
    {
        return ClassificationError::InvalidCount;
    }
    const auto partialCount =
        static_cast<std::size_t>(std::round(spectrum.front()));
    if (partialCount > (spectrum.size() - 1U) / 3U)
        return ClassificationError::InvalidFrame;

    const auto fundamentalFrequency = spectrum[2];
    bool hasPositiveAmplitude = false;
    bool isHarmonic = fundamentalFrequency > 0.0;
    for (std::size_t partial = 0; partial < partialCount; ++partial)
    {
        const auto base = 1U + partial * 3U;
        const auto amplitude = spectrum[base];
        const auto frequency = spectrum[base + 1U];
        const auto bandwidth = spectrum[base + 2U];
        if (!std::isfinite(amplitude) || !std::isfinite(frequency)
            || !std::isfinite(bandwidth))
        {
            return ClassificationError::InvalidPartial;
        }
        if (amplitude > 0.0)
            hasPositiveAmplitude = true;
        const auto expectedFrequency =
            fundamentalFrequency * static_cast<double>(partial + 1U);
        const auto tolerance = std::max(
            1.0e-9, std::abs(expectedFrequency) * 1.0e-6);
        if (frequency < 0.0
            || std::abs(frequency - expectedFrequency) > tolerance)
        {
            isHarmonic = false;
        }
    }
    voiced = isHarmonic && hasPositiveAmplitude;
    return ClassificationError::None;
}

[[nodiscard]] inline Transition transition(
    const State& committed,
    bool classification) noexcept
{
    Transition output;
    output.value = committed.initialized
            && classification != committed.stableClassification
        ? 0.5
        : (classification ? 1.0 : 0.0);
    output.candidate.initialized = true;
    output.candidate.stableClassification = classification;
    return output;
}
}
