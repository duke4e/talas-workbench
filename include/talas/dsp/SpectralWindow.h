#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace talas::dsp
{
enum class SpectralWindowKind
{
    Rectangular,
    Hann,
};

struct ConstantOverlapAddValidation
{
    bool valid = false;
    double constantSum = 0.0;
    double maximumDeviation = 0.0;
};

[[nodiscard]] std::vector<double> makeSpectralWindow(
    SpectralWindowKind kind,
    std::size_t frameSize);

[[nodiscard]] ConstantOverlapAddValidation validateConstantOverlapAdd(
    std::span<const double> window,
    std::size_t hopSize,
    double tolerance = 1.0e-12) noexcept;

[[nodiscard]] bool isSupportedNativeFrameConfiguration(
    std::size_t frameSize,
    std::size_t hopSize,
    SpectralWindowKind window) noexcept;
}
