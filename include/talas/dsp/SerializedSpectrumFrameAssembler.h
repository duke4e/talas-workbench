#pragma once

#include "talas/dsp/SpectralFrame.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace talas::dsp
{
enum class SerializedSpectrumAssemblyStatus
{
    Ok,
    InvalidBlock,
    CapacityExceeded,
    DiscontinuousBlock,
};

class SerializedSpectrumAssemblySnapshot
{
public:
    explicit SerializedSpectrumAssemblySnapshot(std::size_t frameLength);
    [[nodiscard]] std::size_t frameLength() const noexcept
    {
        return frameLength_;
    }

private:
    friend class PreparedSerializedSpectrumFrameAssembler;
    std::size_t frameLength_ = 0;
    std::vector<PartialSpectrumPoint> staging_;
    SpectralFrameTimestamp nextSample_;
    std::uint64_t currentFrameStart_ = 0;
    std::uint64_t sourceSequence_ = 0;
    std::size_t nextComponent_ = 0;
    bool hasNextSample_ = false;
    bool building_ = false;
};

// Bridges a paired, sample-rate spectral stream to complete native frames.
// A component index equal to frameLength marks silence. Partial frames are
// discarded when the stream restarts at zero or loses component order.
// Output views remain valid until the next process() or reset() call.
class PreparedSerializedSpectrumFrameAssembler
{
public:
    PreparedSerializedSpectrumFrameAssembler(
        std::size_t frameLength, std::size_t maximumBlockFrames);

    [[nodiscard]] std::size_t frameLength() const noexcept
    {
        return frameLength_;
    }

    [[nodiscard]] SerializedSpectrumAssemblyStatus process(
        SpectralFrameTimestamp firstSample,
        std::span<const double> amplitudes,
        std::span<const double> frequenciesHz,
        std::span<const std::size_t> componentIndices) noexcept;

    [[nodiscard]] std::span<const PartialSpectrumFrame> frames() const noexcept;
    void reset() noexcept;
    [[nodiscard]] SerializedSpectrumAssemblySnapshot prepareSnapshot() const;
    [[nodiscard]] bool capture(
        SerializedSpectrumAssemblySnapshot& snapshot) const noexcept;
    [[nodiscard]] bool restore(
        const SerializedSpectrumAssemblySnapshot& snapshot) noexcept;

private:
    std::size_t frameLength_ = 0;
    std::size_t maximumBlockFrames_ = 0;
    PreparedPartialSpectrumSequence completedFrames_;
    std::vector<PartialSpectrumPoint> staging_;
    SpectralFrameTimestamp nextSample_;
    std::uint64_t currentFrameStart_ = 0;
    std::uint64_t sourceSequence_ = 0;
    std::size_t nextComponent_ = 0;
    bool hasNextSample_ = false;
    bool building_ = false;
};
}
