#include "talas/dsp/SpectralFrame.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace talas::dsp
{
namespace
{
[[nodiscard]] bool checkedProduct(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept
{
    if (left != 0
        && right > std::numeric_limits<std::size_t>::max() / left)
    {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool addWithoutOverflow(
    std::size_t value,
    std::size_t addition,
    std::size_t& result) noexcept
{
    if (addition > std::numeric_limits<std::size_t>::max() - value)
        return false;
    result = value + addition;
    return true;
}

[[nodiscard]] std::size_t validatedFrameCapacity(
    std::size_t frameCapacity,
    std::size_t payloadCapacity,
    const char* message)
{
    std::size_t ignored = 0;
    if (!checkedProduct(frameCapacity, payloadCapacity, ignored))
        throw std::length_error(message);
    return frameCapacity;
}

[[nodiscard]] std::size_t validatedStorageCapacity(
    std::size_t frameCapacity,
    std::size_t payloadCapacity,
    const char* message)
{
    std::size_t result = 0;
    if (!checkedProduct(frameCapacity, payloadCapacity, result))
        throw std::length_error(message);
    return result;
}

[[nodiscard]] std::size_t validatedComplexFrameCapacity(
    std::size_t frameCapacity,
    std::size_t binCapacity,
    std::size_t channelCount)
{
    if (channelCount == 0)
        throw std::invalid_argument("Prepared complex spectrum channel count must be positive");
    return validatedFrameCapacity(
        frameCapacity,
        binCapacity,
        "Complex spectrum storage capacity overflow");
}

[[nodiscard]] std::size_t ceilingDivision(
    std::size_t value,
    std::size_t divisor) noexcept
{
    return value / divisor + (value % divisor != 0 ? 1U : 0U);
}

[[nodiscard]] bool validPartial(
    const PartialSpectrumPoint& partial) noexcept
{
    return std::isfinite(partial.amplitude)
        && std::isfinite(partial.frequencyHz)
        && std::isfinite(partial.bandwidthHz) && partial.bandwidthHz >= 0.0;
}

template <typename Frame>
[[nodiscard]] bool orderedAfter(
    SpectralFrameTimestamp timestamp,
    std::uint64_t sourceSequence,
    const Frame& previous) noexcept
{
    const auto& previousTimestamp = [](
        const Frame& frame) -> const SpectralFrameTimestamp& {
        if constexpr (std::is_same_v<Frame, PartialSpectrumFrame>)
            return frame.sourceTimestamp;
        else
            return frame.analysisTimestamp;
    }(previous);
    return timestamp > previousTimestamp
        || (timestamp == previousTimestamp
            && sourceSequence > previous.sourceSequence);
}
}

std::optional<std::size_t> preparedSpectralFrameCapacity(
    std::size_t maximumBlockFrames,
    std::size_t minimumHopFrames,
    std::size_t boundaryFrameAllowance,
    std::size_t synthesisTailFrames) noexcept
{
    if (minimumHopFrames == 0)
        return std::nullopt;

    auto result = ceilingDivision(maximumBlockFrames, minimumHopFrames);
    if (!addWithoutOverflow(result, boundaryFrameAllowance, result)
        || !addWithoutOverflow(
            result,
            ceilingDivision(synthesisTailFrames, minimumHopFrames),
            result))
    {
        return std::nullopt;
    }
    return result;
}

PreparedPartialSpectrumSequence::PreparedPartialSpectrumSequence(
    std::size_t frameCapacity,
    std::size_t maximumPartialsPerFrame)
    : frames_(validatedFrameCapacity(
          frameCapacity,
          maximumPartialsPerFrame,
          "Partial spectrum storage capacity overflow")),
      partialStorage_(validatedStorageCapacity(
          frameCapacity,
          maximumPartialsPerFrame,
          "Partial spectrum storage capacity overflow")),
      maximumPartialsPerFrame_(maximumPartialsPerFrame)
{
}

SpectralFrameStatus PreparedPartialSpectrumSequence::append(
    SpectralFrameTimestamp timestamp,
    std::uint64_t sourceSequence,
    std::span<const PartialSpectrumPoint> partials) noexcept
{
    if (size_ >= frames_.size())
        return SpectralFrameStatus::CapacityExceeded;
    if (partials.size() > maximumPartialsPerFrame_)
        return SpectralFrameStatus::PayloadTooLarge;
    if (!std::ranges::all_of(partials, validPartial))
        return SpectralFrameStatus::InvalidFrame;
    if (size_ != 0
        && !orderedAfter(timestamp, sourceSequence, frames_[size_ - 1]))
    {
        return SpectralFrameStatus::OutOfOrder;
    }

    const auto offset = size_ * maximumPartialsPerFrame_;
    PartialSpectrumPoint* destination = partials.empty()
        ? nullptr
        : partialStorage_.data() + offset;
    if (!partials.empty())
        std::ranges::copy(partials, destination);
    frames_[size_] = {
        partialSpectrumFormatVersion,
        timestamp,
        sourceSequence,
        {destination, partials.size()},
    };
    ++size_;
    return SpectralFrameStatus::Ok;
}

void PreparedPartialSpectrumSequence::reset() noexcept
{
    size_ = 0;
}

std::span<const PartialSpectrumFrame>
PreparedPartialSpectrumSequence::frames() const noexcept
{
    return {frames_.data(), size_};
}

std::size_t PreparedPartialSpectrumSequence::capacity() const noexcept
{
    return frames_.size();
}

std::size_t PreparedPartialSpectrumSequence::maximumPartialsPerFrame() const noexcept
{
    return maximumPartialsPerFrame_;
}

PreparedComplexSpectrumSequence::PreparedComplexSpectrumSequence(
    std::size_t frameCapacity,
    std::size_t maximumBinsPerFrame,
    std::size_t channelCount)
    : frames_(validatedComplexFrameCapacity(
          frameCapacity,
          maximumBinsPerFrame,
          channelCount)),
      binStorage_(validatedStorageCapacity(
          frameCapacity,
          maximumBinsPerFrame,
          "Complex spectrum storage capacity overflow")),
      maximumBinsPerFrame_(maximumBinsPerFrame),
      channelCount_(channelCount)
{
}

SpectralFrameStatus PreparedComplexSpectrumSequence::append(
    SpectralFrameTimestamp timestamp,
    std::uint64_t sourceSequence,
    std::size_t frameSize,
    std::size_t hopSize,
    SpectralWindowKind window,
    std::size_t channelIndex,
    std::span<const std::complex<double>> bins) noexcept
{
    if (size_ >= frames_.size())
        return SpectralFrameStatus::CapacityExceeded;
    if (bins.size() > maximumBinsPerFrame_)
        return SpectralFrameStatus::PayloadTooLarge;
    if (channelIndex >= channelCount_
        || !isSupportedNativeFrameConfiguration(frameSize, hopSize, window)
        || bins.size() != frameSize / 2 + 1
        || !std::ranges::all_of(bins, [](const auto& value) {
               return std::isfinite(value.real())
                   && std::isfinite(value.imag());
           }))
    {
        return SpectralFrameStatus::InvalidFrame;
    }
    if (size_ != 0
        && !orderedAfter(timestamp, sourceSequence, frames_[size_ - 1]))
    {
        return SpectralFrameStatus::OutOfOrder;
    }

    const auto offset = size_ * maximumBinsPerFrame_;
    auto* destination = binStorage_.data() + offset;
    std::ranges::copy(bins, destination);
    frames_[size_] = {
        complexSpectrumFormatVersion,
        timestamp,
        sourceSequence,
        frameSize,
        hopSize,
        window,
        channelIndex,
        {destination, bins.size()},
    };
    ++size_;
    return SpectralFrameStatus::Ok;
}

void PreparedComplexSpectrumSequence::reset() noexcept
{
    size_ = 0;
}

std::span<const ComplexSpectrumFrame>
PreparedComplexSpectrumSequence::frames() const noexcept
{
    return {frames_.data(), size_};
}

std::size_t PreparedComplexSpectrumSequence::capacity() const noexcept
{
    return frames_.size();
}

std::size_t PreparedComplexSpectrumSequence::maximumBinsPerFrame() const noexcept
{
    return maximumBinsPerFrame_;
}

std::size_t PreparedComplexSpectrumSequence::channelCount() const noexcept
{
    return channelCount_;
}

SpectralFrameStatus encodeLegacyPartialSpectrum(
    const PartialSpectrumFrame& frame,
    std::span<double> output,
    std::size_t& writtenValues) noexcept
{
    writtenValues = 0;
    if (frame.formatVersion != partialSpectrumFormatVersion
        || !std::ranges::all_of(frame.partials, validPartial)
        || frame.partials.size()
            > (std::numeric_limits<std::size_t>::max() - 1) / 3)
    {
        return SpectralFrameStatus::InvalidFrame;
    }
    const auto required = 1 + frame.partials.size() * 3;
    if (output.size() < required)
        return SpectralFrameStatus::PayloadTooLarge;

    output[0] = static_cast<double>(frame.partials.size());
    for (std::size_t index = 0; index < frame.partials.size(); ++index)
    {
        const auto base = 1 + index * 3;
        output[base] = frame.partials[index].amplitude;
        output[base + 1] = frame.partials[index].frequencyHz;
        output[base + 2] = frame.partials[index].bandwidthHz;
    }
    writtenValues = required;
    return SpectralFrameStatus::Ok;
}

SpectralFrameStatus decodeLegacyPartialSpectrum(
    std::span<const double> encoded,
    std::span<PartialSpectrumPoint> output,
    std::size_t& decodedPartials) noexcept
{
    decodedPartials = 0;
    if (encoded.empty() || !std::isfinite(encoded.front())
        || encoded.front() < 0.0
        || std::trunc(encoded.front()) != encoded.front()
        || encoded.front()
            > static_cast<double>((encoded.size() - 1) / 3))
    {
        return SpectralFrameStatus::InvalidFrame;
    }
    const auto count = static_cast<std::size_t>(encoded.front());
    if (count > (std::numeric_limits<std::size_t>::max() - 1) / 3
        || encoded.size() != 1 + count * 3)
    {
        return SpectralFrameStatus::InvalidFrame;
    }
    if (output.size() < count)
        return SpectralFrameStatus::PayloadTooLarge;

    const auto decodedAt = [&](std::size_t index) {
        const auto base = 1 + index * 3;
        return PartialSpectrumPoint{
            false,
            0,
            encoded[base],
            encoded[base + 1],
            encoded[base + 2],
        };
    };
    for (std::size_t index = 0; index < count; ++index)
    {
        if (!validPartial(decodedAt(index)))
            return SpectralFrameStatus::InvalidFrame;
    }
    for (std::size_t index = 0; index < count; ++index)
    {
        output[index] = decodedAt(index);
    }
    decodedPartials = count;
    return SpectralFrameStatus::Ok;
}
}
