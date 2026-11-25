#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace talas::dsp
{
inline constexpr std::size_t minimumRadix2FftSize = 16;
inline constexpr std::size_t maximumRadix2FftSize = 4096;
inline constexpr std::size_t maximumLongAnalysisFftSize = 65536;

enum class Radix2FftSizeProfile
{
    Standard,
    LongAnalysis,
};

[[nodiscard]] constexpr bool isSupportedRadix2FftSize(
    std::size_t size,
    Radix2FftSizeProfile profile = Radix2FftSizeProfile::Standard) noexcept
{
    const auto maximum = profile == Radix2FftSizeProfile::LongAnalysis
        ? maximumLongAnalysisFftSize : maximumRadix2FftSize;
    return (profile == Radix2FftSizeProfile::Standard
            || profile == Radix2FftSizeProfile::LongAnalysis)
        && size >= minimumRadix2FftSize
        && size <= maximum
        && (size & (size - 1)) == 0;
}

class Radix2FftPlan
{
public:
    // Owns only progress indices; the caller owns transform storage. Copy both
    // together to checkpoint an unfinished transform without retaining pointers.
    class Cursor
    {
    public:
        [[nodiscard]] bool complete() const noexcept { return stage_ == Stage::Complete; }
    private:
        friend class Radix2FftPlan;
        enum class Stage { Permutation, Butterflies, Scale, Complete };
        Stage stage_ = Stage::Complete;
        std::size_t size_ = 0, index_ = 0, length_ = 2;
        bool inverse_ = false, longAnalysis_ = false;
    };

    explicit Radix2FftPlan(std::size_t size,
        Radix2FftSizeProfile profile = Radix2FftSizeProfile::Standard);

    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] Cursor beginTransform(bool inverse) const noexcept;
    // Consumes at most workUnits permutation/butterfly/scale operations and
    // returns the remaining budget. Zero budget makes no progress.
    [[nodiscard]] bool advanceTransform(std::span<std::complex<double>> values,
        Cursor& cursor, std::size_t& workUnits) const noexcept;

    [[nodiscard]] bool forward(
        std::span<std::complex<double>> values) const noexcept;
    [[nodiscard]] bool inverse(
        std::span<std::complex<double>> values) const noexcept;
    [[nodiscard]] bool forward(
        std::span<const std::complex<double>> input,
        std::span<std::complex<double>> output) const noexcept;
    [[nodiscard]] bool inverse(
        std::span<const std::complex<double>> input,
        std::span<std::complex<double>> output) const noexcept;

    [[nodiscard]] bool forward(
        std::span<std::complex<float>> values) const noexcept;
    [[nodiscard]] bool inverse(
        std::span<std::complex<float>> values) const noexcept;
    [[nodiscard]] bool forward(
        std::span<const std::complex<float>> input,
        std::span<std::complex<float>> output) const noexcept;
    [[nodiscard]] bool inverse(
        std::span<const std::complex<float>> input,
        std::span<std::complex<float>> output) const noexcept;

private:
    template <typename Value>
    [[nodiscard]] bool transform(
        std::span<std::complex<Value>> values,
        bool inverse) const noexcept;

    template <typename Value>
    [[nodiscard]] bool transform(
        std::span<const std::complex<Value>> input,
        std::span<std::complex<Value>> output,
        bool inverse) const noexcept;

    std::size_t size_ = 0;
    bool longAnalysis_ = false;
    std::vector<std::size_t> bitReversal_;
    std::vector<std::complex<double>> forwardTwiddles_;
};
}
