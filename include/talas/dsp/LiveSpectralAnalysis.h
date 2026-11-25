#pragma once

#include "talas/dsp/NativeStft.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace talas::dsp
{
// The analysis gives one track per band, up to 512 of them; the legacy peak
// picking kept the 64 strongest peaks.
inline constexpr std::size_t maximumLiveSpectralPartials = 512;
inline constexpr std::size_t legacyLiveSpectralPartials = 64;

struct LiveSpectralAnalysisConfigurationInput
{
    double sampleRate = 0.0;
    std::size_t maximumBlockFrames = 0;
    double lowestAnalyzedFrequencyHz = 0.0;
    double amplitudeScale = 0.0;
    double frequencyScale = 0.0;
    double lowFrequencyHz = 0.0;
    double highFrequencyHz = 0.0;
    double initialFrequencyHz = 0.0;
    bool fundamentalOnly = false;
    // One track per band up to half the sample rate, rather than
    // the strongest peaks between LowFreq and HighFreq.
    bool bandTracks = false;
    // Harmonic analysis: the tracks are the harmonics of the
    // fundamental under TrackedHarmonic, which is followed between LowFreq
    // and HighFreq from InitFreq.
    bool harmonic = false;
    std::size_t trackedHarmonic = 1;
    bool unpitchedOnly = false;
    double unpitchedThreshold = 0.0;
    // Band tracks before format 75 took the power of two at or above the
    // sample rate over LowestAnalyzedFreq and one track fewer than half of
    // it; from format 75 they are the nearest power of two and half of it.
    bool legacyNextPower = false;
    // Response: the bandwidth of the analysis filters, from BestFreq's
    // one bin to BestTime's four, each wider band answering sooner.
    std::size_t responseBins = 1;
};

struct LiveSpectralAnalysisConfiguration
{
    NativeStftConfiguration stft;
    std::size_t frameCapacity = 0;
    std::size_t maximumBlockFrames = 0;
    double sampleRate = 0.0;
    double lowestAnalyzedFrequencyHz = 0.0;
    double amplitudeScale = 0.0;
    double frequencyScale = 0.0;
    double analysisLowFrequencyHz = 0.0;
    double analysisHighFrequencyHz = 0.0;
    double initialFrequencyHz = 0.0;
    bool fundamentalOnly = false;
    bool bandTracks = false;
    std::size_t trackCount = 0;
    bool harmonic = false;
    std::size_t trackedHarmonic = 1;
    bool unpitchedOnly = false;
    double unpitchedThreshold = 0.0;
    double trackedLowFrequencyHz = 0.0;
    double trackedHighFrequencyHz = 0.0;
    std::size_t responseBins = 1;

    bool operator==(const LiveSpectralAnalysisConfiguration&) const = default;
};

enum class LiveSpectralAnalysisConfigurationStatus
{
    Ok,
    InvalidSampleRate,
    InvalidMaximumBlockFrames,
    InvalidLowestAnalyzedFrequency,
    InvalidAmplitudeScale,
    InvalidFrequencyScale,
    InvalidAnalysisBand,
    InvalidInitialFrequency,
    UnsupportedResolution,
    FrameCapacityOverflow,
};

struct LiveSpectralAnalysisConfigurationResult
{
    LiveSpectralAnalysisConfigurationStatus status =
        LiveSpectralAnalysisConfigurationStatus::InvalidSampleRate;
    LiveSpectralAnalysisConfiguration configuration;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return status == LiveSpectralAnalysisConfigurationStatus::Ok;
    }
};

[[nodiscard]] LiveSpectralAnalysisConfigurationResult
deriveLiveSpectralAnalysisConfiguration(
    const LiveSpectralAnalysisConfigurationInput& input) noexcept;

enum class LiveSpectralAnalysisStatus
{
    Ok,
    InvalidSpan,
    InputCapacityExceeded,
    DiscontinuousBlock,
    TimestampOverflow,
    NonFiniteInput,
    NumericOverflow,
    InvalidScale,
    OutputSequenceNotEmpty,
    OutputShapeMismatch,
    OutputCapacityExceeded,
    MetadataCapacityExceeded,
    InternalFrameMismatch,
    OutputSequenceRejected,
};

class PreparedLiveSpectralAnalysis
{
public:
    explicit PreparedLiveSpectralAnalysis(
        LiveSpectralAnalysisConfiguration configuration);
    PreparedLiveSpectralAnalysis(
        const PreparedLiveSpectralAnalysis&) = delete;
    PreparedLiveSpectralAnalysis& operator=(
        const PreparedLiveSpectralAnalysis&) = delete;
    PreparedLiveSpectralAnalysis(
        PreparedLiveSpectralAnalysis&&) = delete;
    PreparedLiveSpectralAnalysis& operator=(
        PreparedLiveSpectralAnalysis&&) = delete;

    [[nodiscard]] LiveSpectralAnalysisStatus process(
        std::span<const double> input,
        std::span<const double> amplitudeScale,
        std::span<const double> frequencyScale,
        SpectralFrameTimestamp blockStart,
        PreparedPartialSpectrumSequence& output,
        std::size_t compiledInputLatencyFrames = 0) noexcept;
    [[nodiscard]] bool copyProcessingStateFrom(
        const PreparedLiveSpectralAnalysis& source) noexcept;
    // Allocating control-thread snapshot with independently owned history and metadata.
    [[nodiscard]] std::unique_ptr<PreparedLiveSpectralAnalysis> cloneProcessingState() const;
    [[nodiscard]] bool retimeNextBlockAbsoluteFrame(
        std::uint64_t absoluteFrame) noexcept;
    void reset() noexcept;

    // The absolute frame of the sample that completes the next frame: the
    // first sample of a block whose processing emits it. Before the first
    // block, the next block starts at nextBlockStartFrame with the given
    // input latency. A renderer that splits its blocks there emits at most
    // one frame per block, at its first sample.
    [[nodiscard]] std::optional<std::uint64_t> nextFrameCompletionFrame(
        std::uint64_t nextBlockStartFrame,
        std::size_t compiledInputLatencyFrames) const noexcept;

    [[nodiscard]] const LiveSpectralAnalysisConfiguration&
    configuration() const noexcept;

private:
    struct ScaleMetadata
    {
        SpectralFrameTimestamp timestamp;
        std::uint64_t sourceSequence = 0;
        double amplitudeScale = 0.0;
        double frequencyScale = 0.0;
    };

    [[nodiscard]] LiveSpectralAnalysisStatus reduceFrame(
        const ComplexSpectrumFrame& frame,
        const ScaleMetadata& metadata) noexcept;
    [[nodiscard]] bool reduceHarmonics(
        const ScaleMetadata& metadata,
        double binWidth) noexcept;
    [[nodiscard]] const ScaleMetadata& metadataAt(
        std::size_t offset) const noexcept;
    void appendMetadata(const ScaleMetadata& metadata) noexcept;
    void popMetadata(std::size_t count) noexcept;

    LiveSpectralAnalysisConfiguration configuration_;
    PreparedForwardStft stft_;
    PreparedForwardStft stftCheckpoint_;
    PreparedComplexSpectrumSequence complexFrames_;
    PreparedPartialSpectrumSequence preflightFrames_;
    std::vector<double> window_;
    double coherentGain_ = 0.0;
    // Response wider than one bin: each bin becomes a weighted sum of its
    // neighbours, alternating in sign, which narrows the window about the
    // frame's centre.
    std::vector<double> responseKernel_;
    std::vector<std::complex<double>> responseScratch_;
    std::vector<double> magnitudeScratch_;
    std::vector<PartialSpectrumPoint> candidateScratch_;
    std::vector<PartialSpectrumPoint> magnitudeHarmonicScratch_;
    // Bin phases of the last analysed frame: a peak's phase advance over one
    // hop gives its frequency far more exactly than interpolating between
    // bins. The candidates follow a block's frames and replace them once
    // the block succeeds.
    std::vector<double> previousPhases_;
    std::vector<double> candidatePhases_;
    // The tracked harmonic's frequency, kept while no peak is found.
    double trackedFrequency_ = 0.0;
    double candidateTrackedFrequency_ = 0.0;
    SpectralFrameTimestamp previousFrameTimestamp_;
    SpectralFrameTimestamp candidateFrameTimestamp_;
    bool hasPreviousFrame_ = false;
    bool candidateHasFrame_ = false;
    std::vector<ScaleMetadata> metadata_;
    std::vector<ScaleMetadata> captureScratch_;
    std::size_t metadataHead_ = 0;
    std::size_t metadataSize_ = 0;
    std::uint64_t sourceSamplesSinceReset_ = 0;
    std::uint64_t nextFrameStartSourceOffset_ = 0;
    std::uint64_t nextSourceSequence_ = 0;
    SpectralFrameTimestamp segmentStart_;
    std::uint64_t nextBlockAbsoluteFrame_ = 0;
    std::size_t preparedInputLatencyFrames_ = 0;
    bool initialized_ = false;
};
}
