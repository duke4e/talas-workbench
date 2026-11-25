#pragma once

#include "talas/dsp/PhaseVocoderPitchShifter.h"
#include "talas/dsp/Radix2Fft.h"
#include "talas/dsp/IncrementalStationaryModel.h"

namespace talas::dsp
{
inline constexpr double timeFrequencyScaleConnectedMinimumInputFrequency = 20.0;

// Original-rate analysis treats pitch scale and source-clock speed independently.
// Runtime frequency hints and prepared capacity impose their own separate bounds.
[[nodiscard]] bool isSupportedTimeFrequencyScaleControls(double frequencyScale, double rate) noexcept;

// Zero denotes an unsupported preparation. A connected hint uses a fixed,
// published bound so an unused parameter fallback cannot determine latency.
[[nodiscard]] std::size_t timeFrequencyScaleAnalysisFrameSize(
    double sampleRate, double minimumInputFrequency) noexcept;

[[nodiscard]] constexpr std::size_t timeFrequencyScaleLatencyFrames(std::size_t frameSize) noexcept
{
    return frameSize == 0 ? 0 : frameSize + frameSize / 4 - 1;
}

// Windows are sampled at the source recording's original rate. Their centers
// follow the integrated playback clock; synthesis advances at a fixed hop.
class PreparedTimeFrequencyScale
{
public:
    PreparedTimeFrequencyScale(double sampleRate, std::size_t maximumBlockFrames,
        std::size_t analysisFrameSize, double initialFrequencyScale, double initialRate);

    [[nodiscard]] PhaseVocoderPitchStatus processSource(
        std::span<const double> source, double frequencyScale, double rate,
        SpectralFrameTimestamp blockStart, std::size_t compiledInputLatencyFrames,
        std::span<double> output) noexcept;
    [[nodiscard]] bool copyProcessingStateFrom(const PreparedTimeFrequencyScale& source) noexcept;
    [[nodiscard]] std::unique_ptr<PreparedTimeFrequencyScale> cloneProcessingState() const;
    [[nodiscard]] bool retimeNextBlockAbsoluteFrame(std::uint64_t absoluteFrame) noexcept;
    void reset() noexcept;

    [[nodiscard]] std::size_t analysisFrameSize() const noexcept { return frameSize_; }
    [[nodiscard]] std::size_t latencyFrames() const noexcept
    { return timeFrequencyScaleLatencyFrames(frameSize_); }
    // Read-only diagnostics for integration/rollback verification.
    [[nodiscard]] double componentModelMix() const noexcept { return components_.mix; }
    [[nodiscard]] bool componentAnalysisPending() const noexcept
    { return componentAnalysis_->status() == IncrementalStationaryModel::Status::Running; }

private:
    friend struct PreparedTimeFrequencyScaleTestAccess;
    [[nodiscard]] PhaseVocoderPitchStatus captureFrame(
        std::span<const double> source, double center, double frequencyScale) noexcept;
    [[nodiscard]] PhaseVocoderPitchStatus advanceFrame(std::size_t& work) noexcept;
    void prepareComponentFrame(double start) noexcept;
    void finishComponentValidation() noexcept;
    void alignComponentPhase(std::size_t component) noexcept;
    void advanceComponentAnalysis(std::size_t work) noexcept;
    [[nodiscard]] double nextComponentSample() noexcept;
    [[nodiscard]] static double sourceSample(std::span<const double> source, double position) noexcept;

    double sampleRate_;
    std::size_t maximumBlockFrames_;
    std::size_t frameSize_;
    std::size_t hopSize_;
    std::size_t historySize_;
    double initialFrequencyScale_;
    double initialRate_;
    Radix2FftPlan fft_;
    Radix2FftPlan componentFft_;
    std::unique_ptr<IncrementalStationaryModel> componentAnalysis_;
    std::vector<double> componentRaw_;
    struct ComponentModel
    {
        std::array<double, IncrementalStationaryModel::maximumPartials> frequency{}, amplitude{}, phase{};
        std::size_t count = 0;
        double sourceStart = 0, dc = 0;
        bool present = false;
    };
    struct ComponentOscillator
    {
        double sine = 0, cosine = 1, stepSine = 0, stepCosine = 1, amplitude = 0;
    };
    struct ComponentState
    {
        ComponentModel active, candidate, frame;
        std::array<double, IncrementalStationaryModel::maximumPartials> outputPhase{};
        std::array<ComponentOscillator, IncrementalStationaryModel::maximumPartials> oscillators{};
        std::array<bool, IncrementalStationaryModel::maximumPartials> seedPhase{};
        double jobStart = 0, rawScale = 1, oscillatorDc = 0, mean = 0, variance = 0,
            errorEnergy = 0, maxError = 0, mix = 0;
        bool candidateReady = false, currentValidated = false, frameIsCandidate = false, badFit = false;
    };
    ComponentState components_;
    std::vector<double> window_;
    std::vector<double> clockHistory_;
    std::vector<double> frequencyHistory_;
    std::vector<std::uint8_t> neutralHistory_;
    std::vector<double> rawHistory_;
    std::vector<double> overlap_;
    std::vector<double> previousPhase_;
    std::vector<double> synthesisPhase_;
    // A pending frame owns its captured source and partial spectral work. These
    // buffers are checkpoint state until its overlap contribution is complete.
    std::vector<std::complex<double>> analysis_;
    std::vector<std::complex<double>> synthesis_;
    std::vector<double> magnitude_;
    std::vector<double> phase_;
    std::vector<double> frequencyBin_;
    std::vector<std::size_t> peaks_;
    enum class FrameStage { Idle, ValidateComponents, Analysis, Reference, Phase, Peaks,
        AlignComponents, Clear, Map, Mirror, Inverse, Overlap };
    FrameStage frameStage_ = FrameStage::Idle;
    Radix2FftPlan::Cursor fftCursor_;
    std::size_t frameIndex_ = 0, peakCount_ = 0, peakIndex_ = 0;
    std::size_t frameOutputIndex_ = 0, frameSamplesRemaining_ = 0;
    double frameCenter_ = 0.0, framePhaseHop_ = 0.0;
    bool frameUsesReference_ = false, frameIsFirst_ = true;
    std::uint64_t processedFrames_ = 0;
    std::size_t overlapReadIndex_ = 0;
    double sourcePosition_ = 0.0;
    double previousCenter_ = 0.0;
    bool hasAnalysis_ = false;
    bool hasTimeline_ = false;
    SpectralFrameTimestamp nextBlock_{};
    std::size_t inputLatency_ = 0;
    double currentFrequencyScale_;
    double targetFrequencyScale_;
    std::size_t scaleRampHopsRemaining_ = 0;
    double spectralMix_;
    double targetSpectralMix_;
    double spectralMixStep_ = 0.0;
    std::size_t mixFramesRemaining_ = 0;
};
}
