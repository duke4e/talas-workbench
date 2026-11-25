#include "talas/dsp/PackedRealFftAdapter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace talas::dsp
{
namespace
{
[[nodiscard]] std::size_t validatedFrameSize(
    const PackedRealFftConfiguration& configuration)
{
    if (!isValidPackedRealFftConfiguration(configuration))
        throw std::invalid_argument("Unsupported packed real FFT configuration");
    return configuration.frameSize;
}

[[nodiscard]] bool addWithoutOverflow(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    result = left + right;
    return true;
}
}

bool isValidPackedRealFftConfiguration(
    const PackedRealFftConfiguration& configuration) noexcept
{
    return configuration.frameSize == 1
        || isSupportedRadix2FftSize(configuration.frameSize);
}

PreparedPackedRealFftAdapter::PreparedPackedRealFftAdapter(
    PackedRealFftConfiguration configuration)
    : configuration_(configuration),
      collection_(validatedFrameSize(configuration), 0.0),
      serialization_(configuration.frameSize, 0.0),
      collectionRight_(configuration.stereo ? configuration.frameSize : 0, 0.0),
      serializationRight_(configuration.stereo ? configuration.frameSize : 0, 0.0),
      transformScratch_(configuration.frameSize),
      serializationIndex_(configuration.frameSize)
{
    if (configuration_.frameSize > 1)
        plan_.emplace(configuration_.frameSize);
}

PackedRealFftStatus PreparedPackedRealFftAdapter::process(
    std::span<const double> input,
    SpectralFrameTimestamp blockStart,
    std::size_t compiledInputLatencyFrames,
    std::span<double> output) noexcept
{
    if (configuration_.stereo)
    {
        std::ranges::fill(output, 0.0);
        return PackedRealFftStatus::InvalidBufferSize;
    }
    return processChannels(input, {}, blockStart, compiledInputLatencyFrames, output, {});
}

PackedRealFftStatus PreparedPackedRealFftAdapter::processStereo(
    std::span<const double> left,
    std::span<const double> right,
    SpectralFrameTimestamp blockStart,
    std::size_t compiledInputLatencyFrames,
    std::span<double> leftOutput,
    std::span<double> rightOutput) noexcept
{
    if (!configuration_.stereo)
    {
        std::ranges::fill(leftOutput, 0.0);
        std::ranges::fill(rightOutput, 0.0);
        return PackedRealFftStatus::InvalidBufferSize;
    }
    return processChannels(left, right, blockStart, compiledInputLatencyFrames, leftOutput, rightOutput);
}

void PreparedPackedRealFftAdapter::clearFrames() noexcept
{
    std::ranges::fill(collection_, 0.0);
    std::ranges::fill(serialization_, 0.0);
    std::ranges::fill(collectionRight_, 0.0);
    std::ranges::fill(serializationRight_, 0.0);
    std::ranges::fill(
        transformScratch_,
        std::complex<double>{0.0, 0.0});
    collectionSize_ = 0;
    serializationIndex_ = configuration_.frameSize;
}

PackedRealFftStatus PreparedPackedRealFftAdapter::processChannels(
    std::span<const double> left,
    std::span<const double> right,
    SpectralFrameTimestamp blockStart,
    std::size_t compiledInputLatencyFrames,
    std::span<double> leftOutput,
    std::span<double> rightOutput) noexcept
{
    const auto stereo = configuration_.stereo;
    std::ranges::fill(leftOutput, 0.0);
    std::ranges::fill(rightOutput, 0.0);
    if (left.size() != leftOutput.size()
        || (stereo
            && (right.size() != left.size()
                || rightOutput.size() != left.size())))
    {
        return PackedRealFftStatus::InvalidBufferSize;
    }

    if (configuration_.frameSize == 1)
    {
        std::ranges::copy(left, leftOutput.begin());
        if (stereo)
            std::ranges::copy(right, rightOutput.begin());
        return PackedRealFftStatus::Ok;
    }

    if (initialized_)
    {
        if (blockStart.transportEpoch != segmentStart_.transportEpoch
            || blockStart.discontinuitySequence
                != segmentStart_.discontinuitySequence
            || blockStart.absoluteFrame != nextBlockAbsoluteFrame_
            || compiledInputLatencyFrames != preparedInputLatencyFrames_)
        {
            return PackedRealFftStatus::DiscontinuousBlock;
        }
    }

    std::uint64_t blockEnd = 0;
    std::uint64_t phaseStartAbsoluteFrame = 0;
    if (!addWithoutOverflow(
            blockStart.absoluteFrame,
            static_cast<std::uint64_t>(left.size()),
            blockEnd)
        || !addWithoutOverflow(
            initialized_
                ? segmentStart_.absoluteFrame
                : blockStart.absoluteFrame,
            static_cast<std::uint64_t>(compiledInputLatencyFrames),
            phaseStartAbsoluteFrame))
    {
        return PackedRealFftStatus::TimestampOverflow;
    }

    const auto relevantInputOffset = phaseStartAbsoluteFrame
            > blockStart.absoluteFrame
        ? static_cast<std::size_t>(std::min<std::uint64_t>(
              phaseStartAbsoluteFrame - blockStart.absoluteFrame,
              static_cast<std::uint64_t>(left.size())))
        : std::size_t{0};
    const auto safeInputMagnitude = std::numeric_limits<double>::max()
        / (2.0 * static_cast<double>(configuration_.frameSize));
    for (const auto channel : {left, right})
    {
        if (channel.empty())
            continue;
        const auto relevantInput = channel.subspan(relevantInputOffset);
        if (!std::ranges::all_of(relevantInput, [&](double value) {
                return std::isfinite(value)
                    && std::abs(value) <= safeInputMagnitude;
            }))
        {
            return std::ranges::all_of(relevantInput, [](double value) {
                return std::isfinite(value);
            })
                ? PackedRealFftStatus::NumericOverflow
                : PackedRealFftStatus::NonFiniteInput;
        }
    }

    if (!initialized_)
    {
        segmentStart_ = blockStart;
        nextBlockAbsoluteFrame_ = blockStart.absoluteFrame;
        preparedInputLatencyFrames_ = compiledInputLatencyFrames;
        initialized_ = true;
    }

    for (std::size_t inputIndex = 0; inputIndex < left.size(); ++inputIndex)
    {
        const auto absoluteFrame = blockStart.absoluteFrame
            + static_cast<std::uint64_t>(inputIndex);
        if (absoluteFrame < phaseStartAbsoluteFrame)
            continue;

        if (serializationIndex_ < configuration_.frameSize)
        {
            leftOutput[inputIndex] = serialization_[serializationIndex_];
            if (stereo)
                rightOutput[inputIndex] = serializationRight_[serializationIndex_];
            ++serializationIndex_;
        }

        collection_[collectionSize_] = left[inputIndex];
        if (stereo)
            collectionRight_[collectionSize_] = right[inputIndex];
        ++collectionSize_;
        if (collectionSize_ != configuration_.frameSize)
            continue;

        const auto status = serializationIndex_ != configuration_.frameSize
            ? PackedRealFftStatus::TransformFailed
            : stereo ? transformCollectedStereoFrame()
                     : transformCollectedFrame();
        if (status != PackedRealFftStatus::Ok)
        {
            std::ranges::fill(leftOutput, 0.0);
            std::ranges::fill(rightOutput, 0.0);
            clearFrames();
            return status;
        }
        collectionSize_ = 0;
        serializationIndex_ = 1;
        leftOutput[inputIndex] = serialization_.front();
        if (stereo)
            rightOutput[inputIndex] = serializationRight_.front();
    }

    nextBlockAbsoluteFrame_ = blockEnd;
    return PackedRealFftStatus::Ok;
}

bool PreparedPackedRealFftAdapter::copyProcessingStateFrom(
    const PreparedPackedRealFftAdapter& source) noexcept
{
    if (configuration_ != source.configuration_
        || collection_.size() != source.collection_.size()
        || serialization_.size() != source.serialization_.size()
        || transformScratch_.size() != source.transformScratch_.size())
    {
        return false;
    }
    std::ranges::copy(source.collection_, collection_.begin());
    std::ranges::copy(source.serialization_, serialization_.begin());
    std::ranges::copy(source.collectionRight_, collectionRight_.begin());
    std::ranges::copy(source.serializationRight_, serializationRight_.begin());
    std::ranges::copy(source.transformScratch_, transformScratch_.begin());
    collectionSize_ = source.collectionSize_;
    serializationIndex_ = source.serializationIndex_;
    segmentStart_ = source.segmentStart_;
    nextBlockAbsoluteFrame_ = source.nextBlockAbsoluteFrame_;
    preparedInputLatencyFrames_ = source.preparedInputLatencyFrames_;
    initialized_ = source.initialized_;
    return true;
}

bool PreparedPackedRealFftAdapter::retimeNextBlockAbsoluteFrame(
    std::uint64_t absoluteFrame) noexcept
{
    if (!initialized_)
        return true;
    if (absoluteFrame < nextBlockAbsoluteFrame_)
        return false;
    const auto delta = absoluteFrame - nextBlockAbsoluteFrame_;
    if (segmentStart_.absoluteFrame
        > std::numeric_limits<std::uint64_t>::max() - delta)
    {
        return false;
    }
    segmentStart_.absoluteFrame += delta;
    nextBlockAbsoluteFrame_ = absoluteFrame;
    return true;
}

void PreparedPackedRealFftAdapter::reset() noexcept
{
    clearFrames();
    segmentStart_ = {};
    nextBlockAbsoluteFrame_ = 0;
    preparedInputLatencyFrames_ = 0;
    initialized_ = false;
}

const PackedRealFftConfiguration&
PreparedPackedRealFftAdapter::configuration() const noexcept
{
    return configuration_;
}

PackedRealFftStatus
PreparedPackedRealFftAdapter::transformCollectedFrame() noexcept
{
    if (!plan_.has_value())
        return PackedRealFftStatus::TransformFailed;

    const auto frameSize = configuration_.frameSize;
    const auto halfSize = frameSize / 2;
    if (!configuration_.inverse)
    {
        for (std::size_t index = 0; index < frameSize; ++index)
            transformScratch_[index] = {collection_[index], 0.0};
        if (!plan_->forward(transformScratch_))
            return PackedRealFftStatus::TransformFailed;
        if (!std::ranges::all_of(transformScratch_, [](const auto& value) {
                return std::isfinite(value.real())
                    && std::isfinite(value.imag());
            }))
        {
            return PackedRealFftStatus::NumericOverflow;
        }

        serialization_[0] = transformScratch_[0].real();
        serialization_[1] = transformScratch_[halfSize].real();
        for (std::size_t bin = 1; bin < halfSize; ++bin)
        {
            const auto packedIndex = 2 + 2 * (bin - 1);
            serialization_[packedIndex] = transformScratch_[bin].real();
            serialization_[packedIndex + 1] = transformScratch_[bin].imag();
        }
        return PackedRealFftStatus::Ok;
    }

    transformScratch_[0] = {collection_[0], 0.0};
    transformScratch_[halfSize] = {collection_[1], 0.0};
    for (std::size_t bin = 1; bin < halfSize; ++bin)
    {
        const auto packedIndex = 2 + 2 * (bin - 1);
        transformScratch_[bin] = {
            collection_[packedIndex],
            collection_[packedIndex + 1],
        };
        transformScratch_[frameSize - bin]
            = std::conj(transformScratch_[bin]);
    }
    if (!plan_->inverse(transformScratch_))
        return PackedRealFftStatus::TransformFailed;
    if (!std::ranges::all_of(transformScratch_, [](const auto& value) {
            return std::isfinite(value.real())
                && std::isfinite(value.imag());
        }))
    {
        return PackedRealFftStatus::NumericOverflow;
    }
    for (std::size_t index = 0; index < frameSize; ++index)
        serialization_[index] = transformScratch_[index].real();
    return PackedRealFftStatus::Ok;
}

// The stereo FFT transforms the left and the right
// signal at once as the real and imaginary parts of one complex signal and
// separates their spectra. Each frame gives the left spectrum's bins 0 to
// Length / 2 - 1 and then the right's, real parts on the left channel and
// imaginary parts on the right; each Nyquist bin, real, takes the place of
// its DC bin's imaginary part, which is zero.
PackedRealFftStatus
PreparedPackedRealFftAdapter::transformCollectedStereoFrame() noexcept
{
    if (!plan_.has_value())
        return PackedRealFftStatus::TransformFailed;
    const auto frameSize = configuration_.frameSize;
    const auto halfSize = frameSize / 2;
    const auto size = static_cast<double>(frameSize);
    const auto finite = [&] {
        return std::ranges::all_of(transformScratch_, [](const auto& value) {
            return std::isfinite(value.real()) && std::isfinite(value.imag());
        });
    };
    if (!configuration_.inverse)
    {
        for (std::size_t index = 0; index < frameSize; ++index)
            transformScratch_[index] = {collection_[index], collectionRight_[index]};
        if (!plan_->forward(transformScratch_))
            return PackedRealFftStatus::TransformFailed;
        if (!finite())
            return PackedRealFftStatus::NumericOverflow;
        const auto separate = [&](std::size_t bin) {
            const auto value = transformScratch_[bin];
            const auto mirror = std::conj(transformScratch_[(frameSize - bin) % frameSize]);
            return std::pair{(value + mirror) * (0.5 / size),
                (value - mirror) * std::complex<double>{0.0, -0.5 / size}};
        };
        for (std::size_t bin = 0; bin < halfSize; ++bin)
        {
            const auto [leftBin, rightBin] = separate(bin);
            serialization_[bin] = leftBin.real();
            serializationRight_[bin] = leftBin.imag();
            serialization_[halfSize + bin] = rightBin.real();
            serializationRight_[halfSize + bin] = rightBin.imag();
        }
        const auto [leftNyquist, rightNyquist] = separate(halfSize);
        serializationRight_[0] = leftNyquist.real();
        serializationRight_[halfSize] = rightNyquist.real();
        return PackedRealFftStatus::Ok;
    }

    const auto spectrum = [&](std::size_t offset, std::size_t bin) {
        return std::complex<double>{collection_[offset + bin], collectionRight_[offset + bin]};
    };
    constexpr std::complex<double> imaginaryUnit{0.0, 1.0};
    transformScratch_[0] = std::complex<double>{collection_[0], 0.0}
        + imaginaryUnit * collection_[halfSize];
    transformScratch_[halfSize] = std::complex<double>{collectionRight_[0], 0.0}
        + imaginaryUnit * collectionRight_[halfSize];
    for (std::size_t bin = 1; bin < halfSize; ++bin)
    {
        const auto leftBin = spectrum(0, bin);
        const auto rightBin = spectrum(halfSize, bin);
        transformScratch_[bin] = leftBin + imaginaryUnit * rightBin;
        transformScratch_[frameSize - bin] = std::conj(leftBin) + imaginaryUnit * std::conj(rightBin);
    }
    if (!plan_->inverse(transformScratch_))
        return PackedRealFftStatus::TransformFailed;
    for (std::size_t index = 0; index < frameSize; ++index)
    {
        serialization_[index] = transformScratch_[index].real() * size;
        serializationRight_[index] = transformScratch_[index].imag() * size;
        if (!std::isfinite(serialization_[index]) || !std::isfinite(serializationRight_[index]))
            return PackedRealFftStatus::NumericOverflow;
    }
    return PackedRealFftStatus::Ok;
}
}
