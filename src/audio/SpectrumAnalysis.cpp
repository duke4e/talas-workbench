#include "talas/audio/SpectrumAnalysis.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>

namespace talas::audio
{
namespace
{
// The modified Bessel function of the first kind and order zero, for the
// Kaiser window: its power series, summed until the terms vanish.
[[nodiscard]] double besselI0(double x) noexcept
{
    auto sum = 1.0;
    auto term = 1.0;
    for (int k = 1; k < 200; ++k)
    {
        const auto half = x / (2.0 * static_cast<double>(k));
        term *= half * half;
        sum += term;
        if (term < 1.0e-17 * sum)
            break;
    }
    return sum;
}

[[nodiscard]] double windowValue(
    SpectrumWindow window,
    std::size_t frame,
    std::size_t frameCount)
{
    if (window == SpectrumWindow::Rectangular || frameCount < 2)
        return 1.0;
    const auto position =
        static_cast<double>(frame) / static_cast<double>(frameCount - 1);
    const auto phase = 2.0 * std::numbers::pi * position;
    switch (window)
    {
    case SpectrumWindow::Rectangular:
        return 1.0;
    case SpectrumWindow::Hann:
        return 0.5 - 0.5 * std::cos(phase);
    case SpectrumWindow::Blackman:
        return 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
    case SpectrumWindow::LinearEnvelope:
        return 1.0 - std::abs(2.0 * position - 1.0);
    case SpectrumWindow::Hamming:
        return 0.54 - 0.46 * std::cos(phase);
    case SpectrumWindow::LongKaiser:
    {
        constexpr double beta = 12.0;
        const auto centred = 2.0 * position - 1.0;
        return besselI0(beta * std::sqrt(std::max(0.0, 1.0 - centred * centred))) / besselI0(beta);
    }
    }
    return 1.0;
}

[[nodiscard]] double normalizedDisplayMagnitude(double amplitude)
{
    constexpr double floorDecibels = -90.0;
    if (!std::isfinite(amplitude) || amplitude <= 0.0)
        return 0.0;
    const auto decibels =
        20.0 * std::log10(std::max(amplitude, 1.0e-12));
    return std::clamp(
        (decibels - floorDecibels) / -floorDecibels,
        0.0,
        1.0);
}

[[nodiscard]] double sanitizedMagnitude(double value)
{
    return std::isfinite(value)
        ? std::clamp(value, 0.0, 1.0)
        : 0.0;
}
}

double spectrumWindowValue(SpectrumWindow window, std::size_t frame, std::size_t frameCount) noexcept
{
    return windowValue(window, frame, frameCount);
}

SpectrumAnalysisResult analyzeNormalizedSpectrum(
    std::span<const double> samples,
    std::size_t requestedFrameCount,
    SpectrumWindow window)
{
    SpectrumAnalysisResult result;
    result.frameCount =
        std::min(samples.size(), requestedFrameCount);
    if (result.frameCount < 2)
        return result;

    const auto selected =
        samples.last(result.frameCount);
    std::vector<double> weights(result.frameCount, 1.0);
    auto weightSum = 0.0;
    for (std::size_t frame = 0; frame < result.frameCount; ++frame)
    {
        weights[frame] =
            windowValue(window, frame, result.frameCount);
        weightSum += weights[frame];
    }
    if (!std::isfinite(weightSum) || weightSum <= 1.0e-12)
    {
        std::ranges::fill(weights, 1.0);
        weightSum = static_cast<double>(result.frameCount);
    }

    const auto binCount = result.frameCount / 2 + 1;
    result.magnitudes.assign(binCount, 0.0);
    for (std::size_t bin = 0; bin < binCount; ++bin)
    {
        auto real = 0.0;
        auto imaginary = 0.0;
        for (std::size_t frame = 0; frame < result.frameCount; ++frame)
        {
            const auto input = std::isfinite(selected[frame])
                ? selected[frame]
                : 0.0;
            const auto phase = -2.0 * std::numbers::pi
                * static_cast<double>(bin)
                * static_cast<double>(frame)
                / static_cast<double>(result.frameCount);
            const auto weighted = input * weights[frame];
            real += weighted * std::cos(phase);
            imaginary += weighted * std::sin(phase);
        }

        auto amplitude =
            std::hypot(real, imaginary) / weightSum;
        const auto nyquist =
            result.frameCount % 2 == 0
            && bin == result.frameCount / 2;
        if (bin != 0 && !nyquist)
            amplitude *= 2.0;
        result.magnitudes[bin] =
            normalizedDisplayMagnitude(amplitude);
    }
    return result;
}

void updateSpectrumAverage(
    std::span<const double> current,
    std::size_t depth,
    std::vector<double>& state)
{
    if (current.empty())
    {
        state.clear();
        return;
    }
    if (state.size() != current.size() || depth <= 1)
    {
        state.resize(current.size());
        std::ranges::transform(
            current,
            state.begin(),
            sanitizedMagnitude);
        return;
    }

    const auto alpha =
        1.0 / static_cast<double>(depth);
    for (std::size_t index = 0; index < current.size(); ++index)
    {
        const auto value = sanitizedMagnitude(current[index]);
        state[index] += (value - state[index]) * alpha;
    }
}

void updateSpectrumPeakHold(
    std::span<const double> current,
    std::vector<double>& peaks)
{
    if (current.empty())
    {
        peaks.clear();
        return;
    }
    if (peaks.size() != current.size())
    {
        peaks.resize(current.size());
        std::ranges::transform(
            current,
            peaks.begin(),
            sanitizedMagnitude);
        return;
    }
    for (std::size_t index = 0; index < current.size(); ++index)
    {
        peaks[index] = std::max(
            peaks[index],
            sanitizedMagnitude(current[index]));
    }
}

void updateSpectrumDisplay(
    std::span<const double> current,
    std::size_t averagingDepth,
    bool peakHoldEnabled,
    std::vector<double>& average,
    std::vector<double>& peaks)
{
    if (peakHoldEnabled)
        updateSpectrumPeakHold(current, peaks);
    else
        peaks.clear();
    updateSpectrumAverage(current, averagingDepth, average);
}
}
