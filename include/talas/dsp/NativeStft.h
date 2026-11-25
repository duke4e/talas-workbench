#pragma once

#include "talas/dsp/Radix2Fft.h"
#include "talas/dsp/SpectralFrame.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace talas::dsp
{
class PreparedLiveSpectralAnalysis;
class PreparedPhaseVocoderPitchShifter;

struct NativeStftConfiguration
{
    std::size_t frameSize = 0;
    std::size_t hopSize = 0;
    SpectralWindowKind window = SpectralWindowKind::Rectangular;

    bool operator==(const NativeStftConfiguration&) const = default;
};

[[nodiscard]] bool isValidNativeStftConfiguration(
    const NativeStftConfiguration& configuration) noexcept;

enum class NativeStftStatus
{
    Ok,
    DiscontinuousBlock,
    TimestampOverflow,
    NonFiniteInput,
    OutputSequenceNotEmpty,
    OutputCapacityExceeded,
    OutputSequenceRejected,
    InputCapacityExceeded,
    MissingFrame,
    UnexpectedFrame,
    InvalidFrame,
    NonRealEndpoint,
    NumericOverflow,
};

class PreparedForwardStft
{
public:
    explicit PreparedForwardStft(NativeStftConfiguration configuration);

    [[nodiscard]] NativeStftStatus process(
        std::span<const double> input,
        SpectralFrameTimestamp blockStart,
        PreparedComplexSpectrumSequence& output,
        std::size_t compiledInputLatencyFrames = 0) noexcept;
    [[nodiscard]] bool copyProcessingStateFrom(
        const PreparedForwardStft& source) noexcept;
    [[nodiscard]] bool retimeNextBlockAbsoluteFrame(
        std::uint64_t absoluteFrame) noexcept;
    void reset() noexcept;

    [[nodiscard]] const NativeStftConfiguration& configuration() const noexcept;

private:
    friend class PreparedLiveSpectralAnalysis;
    friend class PreparedPhaseVocoderPitchShifter;

    NativeStftConfiguration configuration_;
    Radix2FftPlan plan_;
    std::vector<double> window_;
    std::vector<double> history_;
    std::vector<std::complex<double>> transformScratch_;
    std::size_t historyWriteIndex_ = 0;
    std::uint64_t samplesSinceReset_ = 0;
    std::uint64_t sourceSequence_ = 0;
    SpectralFrameTimestamp segmentStart_;
    std::uint64_t nextBlockAbsoluteFrame_ = 0;
    std::size_t preparedInputLatencyFrames_ = 0;
    bool initialized_ = false;
};

enum class InverseStftSynthesisWindowPolicy
{
    AnalysisWindowOnly,
    ApplyAnalysisWindow,
};

class PreparedInverseStft
{
public:
    PreparedInverseStft(
        NativeStftConfiguration configuration,
        std::size_t maximumFramesPerProcess,
        InverseStftSynthesisWindowPolicy synthesisWindowPolicy =
            InverseStftSynthesisWindowPolicy::AnalysisWindowOnly);

    [[nodiscard]] NativeStftStatus process(
        std::span<const ComplexSpectrumFrame> input,
        SpectralFrameTimestamp blockStart,
        std::size_t compiledInputLatencyFrames,
        std::span<double> output) noexcept;
    [[nodiscard]] bool copyProcessingStateFrom(
        const PreparedInverseStft& source) noexcept;
    [[nodiscard]] bool retimeNextBlockAbsoluteFrame(
        std::uint64_t absoluteFrame) noexcept;
    void reset() noexcept;

    [[nodiscard]] const NativeStftConfiguration& configuration() const noexcept;
    [[nodiscard]] std::size_t maximumFramesPerProcess() const noexcept;

private:
    friend class PreparedPhaseVocoderPitchShifter;

    NativeStftConfiguration configuration_;
    Radix2FftPlan plan_;
    std::vector<double> window_;
    std::vector<std::complex<double>> transformedFrames_;
    std::vector<double> overlapSamples_;
    std::vector<double> overlapWeights_;
    std::vector<double> preflightOverlapSamples_;
    std::vector<double> preflightOverlapWeights_;
    std::size_t maximumFramesPerProcess_ = 0;
    InverseStftSynthesisWindowPolicy synthesisWindowPolicy_ =
        InverseStftSynthesisWindowPolicy::AnalysisWindowOnly;
    double steadyStateNormalizationWeight_ = 0.0;
    std::size_t overlapReadIndex_ = 0;
    std::uint64_t nextExpectedFrameStart_ = 0;
    std::uint64_t nextExpectedSourceSequence_ = 0;
    std::uint64_t nextBlockAbsoluteFrame_ = 0;
    std::size_t preparedInputLatencyFrames_ = 0;
    SpectralFrameTimestamp segmentStart_;
    bool initialized_ = false;
};
}
