#include "talas/dsp/SpectralWindow.h"

#include "talas/dsp/Radix2Fft.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace talas::dsp
{
std::vector<double> makeSpectralWindow(
    SpectralWindowKind kind,
    std::size_t frameSize)
{
    if (!isSupportedRadix2FftSize(frameSize))
        throw std::invalid_argument("Spectral window size must use the supported FFT matrix");

    std::vector<double> result(frameSize, 1.0);
    if (kind == SpectralWindowKind::Rectangular)
        return result;
    if (kind != SpectralWindowKind::Hann)
        throw std::invalid_argument("Unknown spectral window kind");

    const auto twoPi = 2.0 * std::acos(-1.0);
    for (std::size_t index = 0; index < frameSize; ++index)
    {
        result[index] = 0.5
            - (0.5 * std::cos(
                twoPi * static_cast<double>(index)
                / static_cast<double>(frameSize)));
    }
    return result;
}

ConstantOverlapAddValidation validateConstantOverlapAdd(
    std::span<const double> window,
    std::size_t hopSize,
    double tolerance) noexcept
{
    if (window.empty() || hopSize == 0
        || window.size() % hopSize != 0
        || !std::isfinite(tolerance) || tolerance < 0.0
        || !std::ranges::all_of(window, [](double value) {
               return std::isfinite(value);
           }))
    {
        return {};
    }

    double total = 0.0;
    for (std::size_t phase = 0; phase < hopSize; ++phase)
    {
        double sum = 0.0;
        for (std::size_t index = phase; index < window.size(); index += hopSize)
            sum += window[index];
        total += sum;
    }
    const auto constantSum = total / static_cast<double>(hopSize);
    if (!std::isfinite(constantSum) || std::abs(constantSum) <= tolerance)
        return {};

    double maximumDeviation = 0.0;
    for (std::size_t phase = 0; phase < hopSize; ++phase)
    {
        double sum = 0.0;
        for (std::size_t index = phase; index < window.size(); index += hopSize)
            sum += window[index];
        maximumDeviation = std::max(
            maximumDeviation,
            std::abs(sum - constantSum));
    }
    return {
        maximumDeviation <= tolerance,
        constantSum,
        maximumDeviation,
    };
}

bool isSupportedNativeFrameConfiguration(
    std::size_t frameSize,
    std::size_t hopSize,
    SpectralWindowKind window) noexcept
{
    if (!isSupportedRadix2FftSize(frameSize))
        return false;
    if (window == SpectralWindowKind::Rectangular)
        return hopSize == frameSize;
    if (window == SpectralWindowKind::Hann)
        return hopSize == frameSize / 2 || hopSize == frameSize / 4;
    return false;
}
}
