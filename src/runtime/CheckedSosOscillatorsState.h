#pragma once

#include "CheckedOscillatorBankMath.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace talas::runtime::checked_sos_oscillators
{

using Partial = checked_oscillator_bank::Partial;

struct State
{
    std::size_t framePosition = 0;
};

[[nodiscard]] inline bool decodePositiveIntegralSelection(
    double value,
    std::size_t maximum,
    std::size_t& decoded) noexcept
{
    if (!std::isfinite(value)
        || value < 1.0
        || value > static_cast<double>(maximum)
        || std::floor(value) != value)
    {
        return false;
    }

    decoded = static_cast<std::size_t>(value);
    return true;
}

template <typename PartialAt>
[[nodiscard]] inline bool transition(
    double sampleRate,
    double cascadeInput,
    std::size_t oscillatorCount,
    std::span<const double> wavetable,
    PartialAt&& partialAt,
    const State& prior,
    State& next,
    double& output) noexcept
{
    if (!std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || !std::isfinite(cascadeInput)
        || prior.framePosition
            == std::numeric_limits<std::size_t>::max())
    {
        return false;
    }

    const auto nyquist = sampleRate * 0.5;
    const auto time =
        static_cast<double>(prior.framePosition) / sampleRate;
    if (!std::isfinite(nyquist) || !std::isfinite(time))
        return false;

    auto sample = cascadeInput;
    for (std::size_t partialIndex = 0;
         partialIndex < oscillatorCount;
         ++partialIndex)
    {
        const auto partial = partialAt(partialIndex);
        if (!std::isfinite(partial.amplitude)
            || !std::isfinite(partial.frequency)
            || !std::isfinite(partial.bandwidth)
            || partial.frequency < 0.0
            || partial.frequency >= nyquist)
        {
            return false;
        }

        const auto phase = partial.frequency * time;
        if (!std::isfinite(phase))
            return false;
        double waveformSample = 0.0;
        if (wavetable.empty())
        {
            const auto angle =
                checked_oscillator_bank::twoPi * phase;
            if (!std::isfinite(angle))
                return false;
            waveformSample = std::sin(angle);
            if (!std::isfinite(waveformSample))
                return false;
        }
        else if (!checked_oscillator_bank::sampleCyclicWavetable(
                     wavetable, phase, waveformSample))
        {
            return false;
        }

        const auto product = partial.amplitude * waveformSample;
        if (!std::isfinite(product))
            return false;
        // Preserve the historical compound-expression rounding while the
        // materialized product keeps overflow fail-closed.
        sample += partial.amplitude * waveformSample;
        if (!std::isfinite(sample))
            return false;
    }

    next = {prior.framePosition + 1};
    output = sample;
    return true;
}

} // namespace talas::runtime::checked_sos_oscillators
