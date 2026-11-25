#pragma once

#include "talas/dsp/Radix2Fft.h"
#include "talas/dsp/SpectralFrame.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace talas::dsp
{
struct PackedRealFftConfiguration
{
    std::size_t frameSize = 0;
    bool inverse = false;
    // The stereo format: two signals, on the left and right channels; each
    // frame gives the left signal's spectrum and then the right's, Length / 2
    // bins each in frequency order, the real parts on the left channel and the
    // imaginary parts on the right. The forward transform divides by Length.
    bool stereo = false;

    bool operator==(const PackedRealFftConfiguration&) const = default;
};

[[nodiscard]] bool isValidPackedRealFftConfiguration(
    const PackedRealFftConfiguration& configuration) noexcept;

enum class PackedRealFftStatus
{
    Ok,
    InvalidBufferSize,
    DiscontinuousBlock,
    TimestampOverflow,
    NonFiniteInput,
    NumericOverflow,
    TransformFailed,
};

class PreparedPackedRealFftAdapter
{
public:
    explicit PreparedPackedRealFftAdapter(
        PackedRealFftConfiguration configuration);

    [[nodiscard]] PackedRealFftStatus process(
        std::span<const double> input,
        SpectralFrameTimestamp blockStart,
        std::size_t compiledInputLatencyFrames,
        std::span<double> output) noexcept;
    [[nodiscard]] PackedRealFftStatus processStereo(
        std::span<const double> left,
        std::span<const double> right,
        SpectralFrameTimestamp blockStart,
        std::size_t compiledInputLatencyFrames,
        std::span<double> leftOutput,
        std::span<double> rightOutput) noexcept;
    [[nodiscard]] bool copyProcessingStateFrom(
        const PreparedPackedRealFftAdapter& source) noexcept;
    [[nodiscard]] bool retimeNextBlockAbsoluteFrame(
        std::uint64_t absoluteFrame) noexcept;
    void reset() noexcept;

    [[nodiscard]] const PackedRealFftConfiguration& configuration() const noexcept;

private:
    [[nodiscard]] PackedRealFftStatus processChannels(
        std::span<const double> left,
        std::span<const double> right,
        SpectralFrameTimestamp blockStart,
        std::size_t compiledInputLatencyFrames,
        std::span<double> leftOutput,
        std::span<double> rightOutput) noexcept;
    [[nodiscard]] PackedRealFftStatus transformCollectedFrame() noexcept;
    [[nodiscard]] PackedRealFftStatus transformCollectedStereoFrame() noexcept;
    void clearFrames() noexcept;

    PackedRealFftConfiguration configuration_;
    std::optional<Radix2FftPlan> plan_;
    std::vector<double> collection_;
    std::vector<double> serialization_;
    std::vector<double> collectionRight_;
    std::vector<double> serializationRight_;
    std::vector<std::complex<double>> transformScratch_;
    std::size_t collectionSize_ = 0;
    std::size_t serializationIndex_ = 0;
    SpectralFrameTimestamp segmentStart_;
    std::uint64_t nextBlockAbsoluteFrame_ = 0;
    std::size_t preparedInputLatencyFrames_ = 0;
    bool initialized_ = false;
};
}
