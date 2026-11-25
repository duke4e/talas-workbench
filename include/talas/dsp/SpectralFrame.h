#pragma once

#include "talas/dsp/SpectralWindow.h"

#include <compare>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace talas::dsp
{
inline constexpr std::uint32_t partialSpectrumFormatVersion = 1;
inline constexpr std::uint32_t complexSpectrumFormatVersion = 1;

struct SpectralFrameTimestamp
{
    std::uint64_t transportEpoch = 0;
    std::uint64_t discontinuitySequence = 0;
    std::uint64_t absoluteFrame = 0;

    auto operator<=>(const SpectralFrameTimestamp&) const = default;
};

struct PartialSpectrumPoint
{
    bool hasTrackId = false;
    std::uint64_t trackId = 0;
    double amplitude = 0.0;
    double frequencyHz = 0.0;
    double bandwidthHz = 0.0;
};

struct PartialSpectrumFrame
{
    std::uint32_t formatVersion = partialSpectrumFormatVersion;
    SpectralFrameTimestamp sourceTimestamp;
    std::uint64_t sourceSequence = 0;
    std::span<const PartialSpectrumPoint> partials;
};

struct ComplexSpectrumFrame
{
    std::uint32_t formatVersion = complexSpectrumFormatVersion;
    SpectralFrameTimestamp analysisTimestamp;
    std::uint64_t sourceSequence = 0;
    std::size_t frameSize = 0;
    std::size_t hopSize = 0;
    SpectralWindowKind window = SpectralWindowKind::Rectangular;
    std::size_t channelIndex = 0;
    std::span<const std::complex<double>> bins;
};

enum class SpectralFrameStatus
{
    Ok,
    CapacityExceeded,
    PayloadTooLarge,
    InvalidFrame,
    OutOfOrder,
};

[[nodiscard]] std::optional<std::size_t> preparedSpectralFrameCapacity(
    std::size_t maximumBlockFrames,
    std::size_t minimumHopFrames,
    std::size_t boundaryFrameAllowance,
    std::size_t synthesisTailFrames) noexcept;

class PreparedPartialSpectrumSequence
{
public:
    PreparedPartialSpectrumSequence(
        std::size_t frameCapacity,
        std::size_t maximumPartialsPerFrame);
    PreparedPartialSpectrumSequence(
        const PreparedPartialSpectrumSequence&) = delete;
    PreparedPartialSpectrumSequence& operator=(
        const PreparedPartialSpectrumSequence&) = delete;
    PreparedPartialSpectrumSequence(
        PreparedPartialSpectrumSequence&&) = delete;
    PreparedPartialSpectrumSequence& operator=(
        PreparedPartialSpectrumSequence&&) = delete;

    [[nodiscard]] SpectralFrameStatus append(
        SpectralFrameTimestamp timestamp,
        std::uint64_t sourceSequence,
        std::span<const PartialSpectrumPoint> partials) noexcept;
    void reset() noexcept;

    [[nodiscard]] std::span<const PartialSpectrumFrame> frames() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t maximumPartialsPerFrame() const noexcept;

private:
    std::vector<PartialSpectrumFrame> frames_;
    std::vector<PartialSpectrumPoint> partialStorage_;
    std::size_t maximumPartialsPerFrame_ = 0;
    std::size_t size_ = 0;
};

class PreparedComplexSpectrumSequence
{
public:
    PreparedComplexSpectrumSequence(
        std::size_t frameCapacity,
        std::size_t maximumBinsPerFrame,
        std::size_t channelCount = 1);
    PreparedComplexSpectrumSequence(
        const PreparedComplexSpectrumSequence&) = delete;
    PreparedComplexSpectrumSequence& operator=(
        const PreparedComplexSpectrumSequence&) = delete;
    PreparedComplexSpectrumSequence(
        PreparedComplexSpectrumSequence&&) = delete;
    PreparedComplexSpectrumSequence& operator=(
        PreparedComplexSpectrumSequence&&) = delete;

    [[nodiscard]] SpectralFrameStatus append(
        SpectralFrameTimestamp timestamp,
        std::uint64_t sourceSequence,
        std::size_t frameSize,
        std::size_t hopSize,
        SpectralWindowKind window,
        std::size_t channelIndex,
        std::span<const std::complex<double>> bins) noexcept;
    void reset() noexcept;

    [[nodiscard]] std::span<const ComplexSpectrumFrame> frames() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] std::size_t maximumBinsPerFrame() const noexcept;
    [[nodiscard]] std::size_t channelCount() const noexcept;

private:
    std::vector<ComplexSpectrumFrame> frames_;
    std::vector<std::complex<double>> binStorage_;
    std::size_t maximumBinsPerFrame_ = 0;
    std::size_t channelCount_ = 0;
    std::size_t size_ = 0;
};

[[nodiscard]] SpectralFrameStatus encodeLegacyPartialSpectrum(
    const PartialSpectrumFrame& frame,
    std::span<double> output,
    std::size_t& writtenValues) noexcept;

[[nodiscard]] SpectralFrameStatus decodeLegacyPartialSpectrum(
    std::span<const double> encoded,
    std::span<PartialSpectrumPoint> output,
    std::size_t& decodedPartials) noexcept;
}
