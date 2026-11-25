#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_spectral_shape
{
// A SpectralShape reads its Wavetable as the shape of the spectrum from
// 0 hz up to half the sample rate: a partial is
// as loud as the table at its frequency, read between samples, and silent at
// or above half the sample rate.
[[nodiscard]] inline double envelopeAt(
    std::span<const double> table,
    double frequency,
    double sampleRate) noexcept
{
    const auto nyquist = sampleRate * 0.5;
    if (table.empty() || !std::isfinite(frequency) || frequency < 0.0
        || !std::isfinite(nyquist) || nyquist <= 0.0 || frequency >= nyquist)
    {
        return 0.0;
    }
    const auto position =
        frequency / nyquist * static_cast<double>(table.size());
    const auto index = static_cast<std::size_t>(position);
    if (index >= table.size())
        return 0.0;
    const auto fraction = position - static_cast<double>(index);
    const auto next = index + 1 < table.size() ? table[index + 1] : table[index];
    return table[index] + (next - table[index]) * fraction;
}
}
