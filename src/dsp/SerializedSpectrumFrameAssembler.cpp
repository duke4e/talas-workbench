#include "talas/dsp/SerializedSpectrumFrameAssembler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace talas::dsp
{
namespace
{
[[nodiscard]] std::size_t positiveLength(
    std::size_t length, const char* message)
{
    if (length == 0) throw std::invalid_argument(message);
    return length;
}

[[nodiscard]] std::size_t frameCapacity(
    std::size_t frameLength, std::size_t maximumBlockFrames)
{
    const auto capacity = maximumBlockFrames / frameLength;
    if (capacity == std::numeric_limits<std::size_t>::max())
        throw std::length_error("Serialized spectrum frame capacity overflow");
    return capacity + 1;
}
}

SerializedSpectrumAssemblySnapshot::SerializedSpectrumAssemblySnapshot(
    std::size_t frameLength)
    : frameLength_(positiveLength(frameLength,
          "Serialized spectrum assembly snapshot requires a positive frame length")),
      staging_(frameLength_)
{
}

PreparedSerializedSpectrumFrameAssembler::
    PreparedSerializedSpectrumFrameAssembler(
        std::size_t frameLength, std::size_t maximumBlockFrames)
    : frameLength_(positiveLength(
          frameLength, "Serialized spectrum frame length must be positive")),
      maximumBlockFrames_(positiveLength(
          maximumBlockFrames,
          "Serialized spectrum maximum block length must be positive")),
      completedFrames_(frameCapacity(frameLength_, maximumBlockFrames_),
          frameLength_),
      staging_(frameLength_)
{
}

SerializedSpectrumAssemblyStatus
PreparedSerializedSpectrumFrameAssembler::process(
    SpectralFrameTimestamp firstSample,
    std::span<const double> amplitudes,
    std::span<const double> frequenciesHz,
    std::span<const std::size_t> componentIndices) noexcept
{
    const auto frames = amplitudes.size();
    if (frequenciesHz.size() != frames || componentIndices.size() != frames)
        return SerializedSpectrumAssemblyStatus::InvalidBlock;
    if (frames > maximumBlockFrames_)
        return SerializedSpectrumAssemblyStatus::CapacityExceeded;
    if (frames > std::numeric_limits<std::uint64_t>::max()
            - firstSample.absoluteFrame)
    {
        return SerializedSpectrumAssemblyStatus::InvalidBlock;
    }
    if (hasNextSample_ && firstSample != nextSample_)
        return SerializedSpectrumAssemblyStatus::DiscontinuousBlock;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        if (!std::isfinite(amplitudes[frame])
            || !std::isfinite(frequenciesHz[frame])
            || frequenciesHz[frame] < 0.0
            || componentIndices[frame] > frameLength_)
        {
            return SerializedSpectrumAssemblyStatus::InvalidBlock;
        }
    }
    const auto maximumCompletions = frames / frameLength_ + 1;
    if (sourceSequence_
        > std::numeric_limits<std::uint64_t>::max()
            - maximumCompletions)
    {
        return SerializedSpectrumAssemblyStatus::CapacityExceeded;
    }

    completedFrames_.reset();
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto index = componentIndices[frame];
        if (index == frameLength_)
        {
            building_ = false;
            nextComponent_ = 0;
            continue;
        }
        if (index == 0)
        {
            building_ = true;
            nextComponent_ = 0;
            currentFrameStart_ = firstSample.absoluteFrame + frame;
        }
        if (!building_ || index != nextComponent_)
        {
            building_ = false;
            nextComponent_ = 0;
            continue;
        }
        staging_[index] = {
            true,
            static_cast<std::uint64_t>(index) + 1,
            amplitudes[frame],
            frequenciesHz[frame],
            0.0,
        };
        ++nextComponent_;
        if (nextComponent_ == frameLength_)
        {
            const auto status = completedFrames_.append(
                {firstSample.transportEpoch,
                 firstSample.discontinuitySequence,
                 currentFrameStart_},
                sourceSequence_, staging_);
            if (status != SpectralFrameStatus::Ok)
                return SerializedSpectrumAssemblyStatus::CapacityExceeded;
            ++sourceSequence_;
            building_ = false;
            nextComponent_ = 0;
        }
    }
    nextSample_ = firstSample;
    nextSample_.absoluteFrame += frames;
    hasNextSample_ = true;
    return SerializedSpectrumAssemblyStatus::Ok;
}

std::span<const PartialSpectrumFrame>
PreparedSerializedSpectrumFrameAssembler::frames() const noexcept
{
    return completedFrames_.frames();
}

void PreparedSerializedSpectrumFrameAssembler::reset() noexcept
{
    completedFrames_.reset();
    nextSample_ = {};
    currentFrameStart_ = 0;
    sourceSequence_ = 0;
    nextComponent_ = 0;
    hasNextSample_ = false;
    building_ = false;
}

SerializedSpectrumAssemblySnapshot
PreparedSerializedSpectrumFrameAssembler::prepareSnapshot() const
{
    return SerializedSpectrumAssemblySnapshot(frameLength_);
}

bool PreparedSerializedSpectrumFrameAssembler::capture(
    SerializedSpectrumAssemblySnapshot& snapshot) const noexcept
{
    if (snapshot.frameLength_ != frameLength_
        || snapshot.staging_.size() != staging_.size())
        return false;
    std::ranges::copy(staging_, snapshot.staging_.begin());
    snapshot.nextSample_ = nextSample_;
    snapshot.currentFrameStart_ = currentFrameStart_;
    snapshot.sourceSequence_ = sourceSequence_;
    snapshot.nextComponent_ = nextComponent_;
    snapshot.hasNextSample_ = hasNextSample_;
    snapshot.building_ = building_;
    return true;
}

bool PreparedSerializedSpectrumFrameAssembler::restore(
    const SerializedSpectrumAssemblySnapshot& snapshot) noexcept
{
    if (snapshot.frameLength_ != frameLength_
        || snapshot.staging_.size() != staging_.size()
        || snapshot.nextComponent_ >= frameLength_)
        return false;
    completedFrames_.reset();
    std::ranges::copy(snapshot.staging_, staging_.begin());
    nextSample_ = snapshot.nextSample_;
    currentFrameStart_ = snapshot.currentFrameStart_;
    sourceSequence_ = snapshot.sourceSequence_;
    nextComponent_ = snapshot.nextComponent_;
    hasNextSample_ = snapshot.hasNextSample_;
    building_ = snapshot.building_;
    return true;
}
}
