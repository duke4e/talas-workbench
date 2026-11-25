#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace talas::runtime::checked_oscillator_bank
{
inline constexpr double twoPi =
    6.283185307179586476925286766559;

struct Partial
{
    double amplitude = 0.0;
    double frequency = 0.0;
    double bandwidth = 0.0;
};

struct Frame
{
    double sample = 0.0;
};

[[nodiscard]] inline bool sampleCyclicWavetable(
    std::span<const double> samples,
    double phase,
    double& result) noexcept
{
    if (samples.empty() || !std::isfinite(phase))
        return false;
    if (samples.size() == 1)
    {
        if (!std::isfinite(samples.front()))
            return false;
        result = samples.front();
        return true;
    }

    const auto phaseFloor = std::floor(phase);
    if (!std::isfinite(phaseFloor))
        return false;
    auto wrappedPhase = phase - phaseFloor;
    if (!std::isfinite(wrappedPhase))
        return false;
    if (wrappedPhase >= 1.0)
        wrappedPhase = 0.0;

    const auto scaledIndex =
        wrappedPhase * static_cast<double>(samples.size());
    if (!std::isfinite(scaledIndex))
        return false;
    const auto indexFloor = std::floor(scaledIndex);
    if (!std::isfinite(indexFloor)
        || indexFloor
            > static_cast<double>(
                std::numeric_limits<std::size_t>::max()))
    {
        return false;
    }
    const auto leftIndex =
        static_cast<std::size_t>(indexFloor) % samples.size();
    const auto rightIndex = (leftIndex + 1) % samples.size();
    const auto amount = scaledIndex - indexFloor;
    const auto difference = samples[rightIndex] - samples[leftIndex];
    if (!std::isfinite(samples[leftIndex])
        || !std::isfinite(samples[rightIndex])
        || !std::isfinite(amount)
        || !std::isfinite(difference))
    {
        return false;
    }
    const auto scaledDifference = difference * amount;
    if (!std::isfinite(scaledDifference))
        return false;
    const auto sample = samples[leftIndex] + scaledDifference;
    if (!std::isfinite(sample))
        return false;
    result = sample;
    return true;
}

// Each oscillator keeps its own phase, in cycles, and advances it by its
// partial's frequency every sample, so a partial whose frequency moves
// glides without a jump. `phases` holds one phase per oscillator; the frame
// advances the first `oscillatorCount` of them.
template <typename PartialAt>
[[nodiscard]] inline bool processFrame(
    double sampleRate,
    std::size_t renderedFrames,
    std::size_t frameOffset,
    std::size_t oscillatorCount,
    std::span<const double> wavetable,
    PartialAt&& partialAt,
    std::span<double> phases,
    Frame& result) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || frameOffset
            > std::numeric_limits<std::size_t>::max()
                - renderedFrames
        || phases.size() < oscillatorCount)
    {
        return false;
    }
    const auto nyquist = sampleRate * 0.5;
    if (!std::isfinite(nyquist))
        return false;

    double sample = 0.0;
    for (std::size_t partialIndex = 0;
         partialIndex < oscillatorCount;
         ++partialIndex)
    {
        const auto partial = partialAt(partialIndex);
        // A silent partial may sit at or above half the sample rate, as a
        // SpectralShape's partials beyond its spectrum do.
        if (!std::isfinite(partial.amplitude)
            || !std::isfinite(partial.frequency)
            || !std::isfinite(partial.bandwidth)
            || partial.frequency < 0.0
            || (partial.frequency >= nyquist && partial.amplitude != 0.0))
        {
            return false;
        }

        const auto phase = phases[partialIndex];
        if (!std::isfinite(phase))
            return false;
        auto nextPhase = phase + partial.frequency / sampleRate;
        nextPhase -= std::floor(nextPhase);
        if (!std::isfinite(nextPhase))
            return false;
        phases[partialIndex] = nextPhase;
        double waveformSample = 0.0;
        if (wavetable.empty())
        {
            const auto angle = twoPi * phase;
            if (!std::isfinite(angle))
                return false;
            waveformSample = std::sin(angle);
            if (!std::isfinite(waveformSample))
                return false;
        }
        else if (!sampleCyclicWavetable(
                     wavetable, phase, waveformSample))
        {
            return false;
        }

        const auto product = partial.amplitude * waveformSample;
        if (!std::isfinite(product))
            return false;
        sample += product;
        if (!std::isfinite(sample))
            return false;
    }

    result = {sample};
    return true;
}
}
