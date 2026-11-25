#pragma once

#include "talas/dsp/NativeStft.h"

#include <complex>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace talas::dsp
{
inline constexpr std::size_t phaseVocoderPitchFrameSize = 1024;
inline constexpr std::size_t phaseVocoderPitchHopSize = 256;
inline constexpr std::size_t phaseVocoderPitchLatencyFrames =
    phaseVocoderPitchFrameSize - 1;
inline constexpr std::size_t phaseVocoderPitchRatioRampHops = 4;
inline constexpr std::size_t dynamicPhaseVocoderPitchTransitionFrames =
    phaseVocoderPitchFrameSize;
inline constexpr double phaseVocoderTransientFluxThreshold = 0.35;
inline constexpr double phaseVocoderTransientEnergyFloor = 1.0e-12;
inline constexpr std::size_t phaseVocoderTransientRefractoryHops = 3;
inline constexpr double phaseVocoderIdentityPeakEnergyFloor =
    phaseVocoderTransientEnergyFloor;
inline constexpr double phaseVocoderIdentityPeakRelativeFloor = 0.05;
inline constexpr std::size_t
    phaseVocoderPeakIdentityMaximumDisplacementBins = 4;
inline constexpr std::size_t phaseVocoderSpectralEnvelopeRadiusBins = 8;
inline constexpr double minimumPhaseVocoderPitchInterval = -12.0;
inline constexpr double maximumPhaseVocoderPitchInterval = 12.0;
inline constexpr double minimumPhaseVocoderFrequencyScale = 0.5;
inline constexpr double maximumPhaseVocoderFrequencyScale = 2.0;

// Extended profile for resampled-input correction. Original-rate source-window
// TimeFrequencyScale validates its pitch and time controls independently.
// Other consumers keep the default one-octave range.
enum class PhaseVocoderPitchProfile
{
    OneOctave,
    TimeFrequencyCorrection,
};
inline constexpr double maximumPhaseVocoderTimeFrequencyCorrection = 4.0;

[[nodiscard]] bool isSupportedPhaseVocoderPitchInterval(
    double intervalSemitones,
    PhaseVocoderPitchProfile profile = PhaseVocoderPitchProfile::OneOctave) noexcept;
[[nodiscard]] double phaseVocoderPitchRatio(
    double intervalSemitones,
    PhaseVocoderPitchProfile profile = PhaseVocoderPitchProfile::OneOctave) noexcept;
[[nodiscard]] bool isSupportedPhaseVocoderFrequencyScale(
    double frequencyScale) noexcept;
[[nodiscard]] double phaseVocoderFrequencyScaleInterval(
    double frequencyScale) noexcept;
[[nodiscard]] double phaseVocoderKnownPitchRatio(
    double targetFrequency,
    double minimumInputPitch,
    double maximumInputPitch) noexcept;
[[nodiscard]] double phaseVocoderTimeFrequencyCorrectionRatio(
    double frequencyScale,
    double playbackRate) noexcept;
[[nodiscard]] double phaseVocoderTimeFrequencyCorrectionInterval(
    double correctionRatio) noexcept;

enum class MonotonizerControlStatus
{
    Ok,
    InvalidSampleRate,
    InvalidBufferShape,
    NonPositiveFrequency,
    NonPositiveMinimumInputPitch,
    NonPositiveMaximumInputPitch,
    NonSingletonInputPitch,
    UnsupportedRatio,
    NyquistExceeded,
    FrequencyNotBlockConstant,
    MinimumInputPitchNotBlockConstant,
    MaximumInputPitchNotBlockConstant,
    ProfileMismatch,
    InvalidInterval,
};

struct MonotonizerControlInput
{
    double fallback = 0.0;
    std::span<const double> connectedValues;
    bool connected = false;
};

struct MonotonizerBlockDecision
{
    bool activeProfile = false;
    double frequency = 0.0;
    double sourcePitch = 0.0;
    double ratio = 0.0;
    double intervalSemitones = 0.0;
};

[[nodiscard]] MonotonizerControlStatus checkedMonotonizerBlockDecision(
    double sampleRate,
    std::size_t frameCount,
    bool activeProfile,
    MonotonizerControlInput frequency,
    MonotonizerControlInput minimumInputPitch,
    MonotonizerControlInput maximumInputPitch,
    MonotonizerBlockDecision& decision) noexcept;

enum class PhaseVocoderPitchStatus
{
    Ok,
    InvalidInterval,
    InvalidSampleRate,
    InvalidBufferShape,
    BlockCapacityExceeded,
    TimestampOverflow,
    DiscontinuousBlock,
    NonFiniteInput,
    NumericOverflow,
    InternalSpectralFailure,
};

enum class PhaseVocoderSpectralEnvelopePolicy
{
    ScaleWithSpectrum,
    PreserveAbsoluteFrequency,
};

class PreparedPhaseVocoderPitchShifter
{
public:
    PreparedPhaseVocoderPitchShifter(
        double sampleRate,
        std::size_t maximumBlockFrames,
        double intervalSemitones,
        PhaseVocoderSpectralEnvelopePolicy envelopePolicy =
            PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum,
        PhaseVocoderPitchProfile pitchProfile = PhaseVocoderPitchProfile::OneOctave);

    [[nodiscard]] PhaseVocoderPitchStatus process(
        std::span<const double> input,
        SpectralFrameTimestamp blockStart,
        std::size_t compiledInputLatencyFrames,
        std::span<double> output) noexcept;
    [[nodiscard]] PhaseVocoderPitchStatus processWithInterval(
        std::span<const double> input,
        double intervalSemitones,
        SpectralFrameTimestamp blockStart,
        std::size_t compiledInputLatencyFrames,
        std::span<double> output) noexcept;
    [[nodiscard]] bool copyProcessingStateFrom(
        const PreparedPhaseVocoderPitchShifter& source) noexcept;
    // Allocating control-thread snapshot; retains immutable preparation settings
    // even when the current interval differs from the initial interval.
    [[nodiscard]] std::unique_ptr<PreparedPhaseVocoderPitchShifter> cloneProcessingState() const;
    [[nodiscard]] bool retimeNextBlockAbsoluteFrame(
        std::uint64_t absoluteFrame) noexcept;
    void reset() noexcept;

    [[nodiscard]] double sampleRate() const noexcept;
    [[nodiscard]] std::size_t maximumBlockFrames() const noexcept;
    [[nodiscard]] double intervalSemitones() const noexcept;
    [[nodiscard]] double ratio() const noexcept;
    [[nodiscard]] PhaseVocoderPitchProfile pitchProfile() const noexcept;
    [[nodiscard]] PhaseVocoderSpectralEnvelopePolicy
        spectralEnvelopePolicy() const noexcept;
    [[nodiscard]] std::size_t transientPhaseAnchorCount() const noexcept;
    [[nodiscard]] std::size_t identityLockedFrameCount() const noexcept;
    [[nodiscard]] std::size_t peakIdentityMatchCount() const noexcept;
    [[nodiscard]] std::size_t activePeakIdentityCount() const noexcept;
    [[nodiscard]] static constexpr std::size_t latencyFrames() noexcept
    {
        return phaseVocoderPitchLatencyFrames;
    }

private:
    [[nodiscard]] PhaseVocoderPitchStatus transformFrames(
        double targetRatio) noexcept;

    double sampleRate_ = 0.0;
    std::size_t maximumBlockFrames_ = 0;
    double initialIntervalSemitones_ = 0.0;
    double targetIntervalSemitones_ = 0.0;
    double currentRatio_ = 1.0;
    double targetRatio_ = 1.0;
    PhaseVocoderSpectralEnvelopePolicy envelopePolicy_ =
        PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum;
    PhaseVocoderPitchProfile pitchProfile_ = PhaseVocoderPitchProfile::OneOctave;
    std::size_t ratioRampFramesRemaining_ = 0;
    PreparedForwardStft forward_;
    PreparedInverseStft inverse_;
    PreparedComplexSpectrumSequence analysisFrames_;
    PreparedComplexSpectrumSequence synthesisFrames_;
    std::vector<double> previousAnalysisPhases_;
    std::vector<double> synthesisPhases_;
    std::vector<double> analysisMagnitudes_;
    std::vector<double> spectralLogMagnitudes_;
    std::vector<double> spectralLogEnvelope_;
    std::vector<double> spectralResidualMagnitudes_;
    std::vector<double> previousAnalysisMagnitudes_;
    std::vector<double> analysisAdvances_;
    std::vector<double> mappedMagnitudes_;
    std::vector<double> mappedAnalysisPhases_;
    std::vector<double> mappedAdvances_;
    std::vector<std::size_t> identityPeakBins_;
    std::vector<std::size_t> identityPeakOwners_;
    std::vector<std::size_t> identityPeakMatches_;
    std::vector<std::size_t> previousIdentityPeakBins_;
    std::vector<double> previousIdentityPeakPhases_;
    std::vector<std::complex<double>> synthesisBins_;
    bool phaseInitialized_ = false;
    bool magnitudeHistoryInitialized_ = false;
    std::size_t transientRefractoryHopsRemaining_ = 0;
    std::size_t transientPhaseAnchorCount_ = 0;
    std::size_t identityLockedFrameCount_ = 0;
    std::size_t peakIdentityMatchCount_ = 0;
    std::size_t previousIdentityPeakCount_ = 0;
};

class PreparedDynamicPhaseVocoderPitchShifter
{
public:
    PreparedDynamicPhaseVocoderPitchShifter(
        double sampleRate,
        std::size_t maximumBlockFrames,
        double initialIntervalSemitones,
        PhaseVocoderSpectralEnvelopePolicy envelopePolicy =
            PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum,
        PhaseVocoderPitchProfile pitchProfile = PhaseVocoderPitchProfile::OneOctave);

    [[nodiscard]] PhaseVocoderPitchStatus process(
        std::span<const double> input,
        double intervalSemitones,
        SpectralFrameTimestamp blockStart,
        std::size_t compiledInputLatencyFrames,
        std::span<double> output) noexcept;
    [[nodiscard]] bool copyProcessingStateFrom(
        const PreparedDynamicPhaseVocoderPitchShifter& source) noexcept;
    // Allocating control-thread snapshot; retains immutable preparation settings
    // even when the current interval differs from the initial interval.
    [[nodiscard]] std::unique_ptr<PreparedDynamicPhaseVocoderPitchShifter> cloneProcessingState() const;
    [[nodiscard]] bool retimeNextBlockAbsoluteFrame(
        std::uint64_t absoluteFrame) noexcept;
    void reset() noexcept;

    [[nodiscard]] double initialIntervalSemitones() const noexcept;
    [[nodiscard]] std::size_t maximumBlockFrames() const noexcept;
    [[nodiscard]] static constexpr std::size_t latencyFrames() noexcept
    {
        return phaseVocoderPitchLatencyFrames;
    }

private:
    double initialIntervalSemitones_ = 0.0;
    std::size_t maximumBlockFrames_ = 0;
    PreparedPhaseVocoderPitchShifter spectral_;
    std::vector<double> spectralScratch_;
    std::vector<double> delayScratch_;
    std::vector<double> delayLine_;
    std::size_t delayWriteIndex_ = 0;
    double spectralMix_ = 0.0;
    double targetSpectralMix_ = 0.0;
    double spectralMixStep_ = 0.0;
    std::size_t transitionFramesRemaining_ = 0;
};
}
