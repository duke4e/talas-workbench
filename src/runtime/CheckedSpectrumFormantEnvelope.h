#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_spectrum_formant_envelope {

// The local bounded profile treats formants as peaks in an ordered amplitude
// envelope. It resamples that envelope around the first partial while keeping
// every partial frequency and bandwidth fixed.
[[nodiscard]] inline bool resample(
    std::span<const double> input,
    std::span<double> output,
    std::size_t partialCount,
    double scale) noexcept
{
    if (input.empty() || output.empty()
        || !std::isfinite(scale) || scale <= 0.0
        || partialCount > (input.size() - 1) / 3
        || partialCount > (output.size() - 1) / 3)
    {
        return false;
    }

    const auto encodedSize = 1 + partialCount * 3;
    if (partialCount == 0)
    {
        output[0] = input[0];
        return true;
    }

    double previousFrequency = 0.0;
    for (std::size_t partial = 0; partial < partialCount; ++partial)
    {
        const auto base = 1 + partial * 3;
        const auto frequency = input[base + 1];
        if (!std::isfinite(input[base])
            || !std::isfinite(frequency)
            || !std::isfinite(input[base + 2])
            || frequency <= previousFrequency)
        {
            return false;
        }
        previousFrequency = frequency;
    }

    std::copy_n(input.begin(), encodedSize, output.begin());
    const auto fundamental = input[2];
    const auto highestFrequency = input[1 + (partialCount - 1) * 3 + 1];
    std::size_t lower = 0;
    for (std::size_t partial = 0; partial < partialCount; ++partial)
    {
        const auto base = 1 + partial * 3;
        const auto sourceFrequency =
            fundamental + (input[base + 1] - fundamental) / scale;
        if (!std::isfinite(sourceFrequency))
            return false;

        double amplitude = input[1];
        if (sourceFrequency >= highestFrequency)
        {
            amplitude = input[1 + (partialCount - 1) * 3];
        }
        else if (sourceFrequency > fundamental)
        {
            while (lower + 1 < partialCount
                && input[1 + (lower + 1) * 3 + 1] < sourceFrequency)
            {
                ++lower;
            }
            const auto leftBase = 1 + lower * 3;
            const auto rightBase = leftBase + 3;
            const auto fraction = (sourceFrequency - input[leftBase + 1])
                / (input[rightBase + 1] - input[leftBase + 1]);
            amplitude = std::lerp(
                input[leftBase], input[rightBase], fraction);
        }
        if (!std::isfinite(amplitude))
            return false;
        output[base] = amplitude;
    }
    return true;
}

// SpectrumFrequencyScale (format 92): every
// partial's frequency and bandwidth multiplied by Scale, its amplitude read
// from the input's amplitude envelope at the new frequency, so the pitch
// moves and the formants stay where they were. The envelope joins the
// partials of positive frequency by straight lines, holds the lowest one's
// amplitude below it and falls silent above the highest. A frame whose
// positive frequencies do not rise, which a harmonic spectrum's do, keeps
// its amplitudes. False when a value is not finite.
[[nodiscard]] inline bool shiftKeepingFormants(
    std::span<const double> input,
    std::span<double> output,
    std::size_t partialCount,
    double scale) noexcept
{
    if (input.empty() || output.empty() || !std::isfinite(scale) || scale < 0.0
        || partialCount > (input.size() - 1) / 3 || partialCount > (output.size() - 1) / 3)
    {
        return false;
    }
    output[0] = input[0];
    auto rising = true;
    std::size_t lowest = partialCount;
    std::size_t highest = partialCount;
    auto previous = 0.0;
    for (std::size_t partial = 0; partial < partialCount; ++partial)
    {
        const auto base = 1 + partial * 3;
        if (!std::isfinite(input[base]) || !std::isfinite(input[base + 1]) || !std::isfinite(input[base + 2]))
            return false;
        const auto frequency = input[base + 1];
        if (frequency <= 0.0)
            continue;
        if (frequency <= previous)
            rising = false;
        previous = frequency;
        if (lowest == partialCount)
            lowest = partial;
        highest = partial;
    }
    // The envelope's next point at or above a frequency, walked once as the
    // scaled frequencies rise.
    std::size_t upper = lowest;
    for (std::size_t partial = 0; partial < partialCount; ++partial)
    {
        const auto base = 1 + partial * 3;
        const auto frequency = input[base + 1] * scale;
        const auto bandwidth = input[base + 2] * scale;
        if (!std::isfinite(frequency) || !std::isfinite(bandwidth))
            return false;
        auto amplitude = input[base];
        if (rising && lowest < partialCount && input[base + 1] > 0.0)
        {
            if (frequency <= input[1 + lowest * 3 + 1])
                amplitude = input[1 + lowest * 3];
            else if (frequency > input[1 + highest * 3 + 1])
                amplitude = 0.0;
            else
            {
                while (input[1 + upper * 3 + 1] < frequency)
                {
                    do
                        ++upper;
                    while (input[1 + upper * 3 + 1] <= 0.0);
                }
                auto below = upper;
                if (input[1 + upper * 3 + 1] > frequency)
                {
                    do
                        --below;
                    while (input[1 + below * 3 + 1] <= 0.0);
                }
                const auto low = input[1 + below * 3 + 1];
                const auto high = input[1 + upper * 3 + 1];
                amplitude = high == low
                    ? input[1 + upper * 3]
                    : std::lerp(input[1 + below * 3], input[1 + upper * 3], (frequency - low) / (high - low));
            }
        }
        if (!std::isfinite(amplitude))
            return false;
        output[base] = amplitude;
        output[base + 1] = frequency;
        output[base + 2] = bandwidth;
    }
    return true;
}

} // namespace talas::runtime::checked_spectrum_formant_envelope
