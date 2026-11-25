#pragma once

#include "talas/dsp/Radix2Fft.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace talas::dsp
{
inline constexpr std::size_t diffuseSpectralStretchFrameSize = 4096;
inline constexpr std::size_t diffuseSpectralStretchHopSize = 1024;
inline constexpr std::size_t diffuseSpectralStretchLatencyFrames =
    diffuseSpectralStretchFrameSize - 1;
inline constexpr std::size_t diffuseSpectralStretchMaximumSourceSamples =
    8U * 1024U * 1024U;
inline constexpr double diffuseSpectralStretchMinimumFactor = 2.0;
inline constexpr double diffuseSpectralStretchMaximumFactor = 32.0;

struct DiffuseSpectralStretchConfiguration
{
    double sampleRate = 0.0;
    double stretch = 8.0;
    double smear = 0.75;
    double diffusion = 1.0;
    std::uint32_t seed = 1;
};

struct DiffuseSpectralStretchProcessingState
{
    explicit DiffuseSpectralStretchProcessingState(std::size_t frameSize);

    std::vector<std::complex<double>> analysisSpectrum;
    std::vector<std::complex<double>> synthesisSpectrum;
    std::vector<double> previousAnalysisPhases;
    std::vector<double> synthesisPhases;
    std::vector<double> magnitudeMemory;
    std::vector<double> candidateAnalysisPhases;
    std::vector<double> candidateSynthesisPhases;
    std::vector<double> candidateMagnitudes;
    std::vector<double> overlapSamples;
    std::vector<double> overlapWeights;
    double analysisCursor = 0.0;
    std::size_t overlapReadIndex = 0;
    std::size_t hopFramesRemaining = 0;
    std::size_t startupFramesRemaining = diffuseSpectralStretchLatencyFrames;
    std::size_t tailFramesRemaining = 0;
    std::uint64_t synthesisFrameCount = 0;
    bool phaseInitialized = false;
    bool lastSynthesisSpectrumWasExactlyHermitian = true;
    bool sourceExhausted = false;
    bool previousResetHigh = false;
    bool gateActive = false;

    bool operator==(
        const DiffuseSpectralStretchProcessingState&) const = default;
};

[[nodiscard]] bool isValidDiffuseSpectralStretchConfiguration(
    const DiffuseSpectralStretchConfiguration& configuration,
    std::span<const double> source) noexcept;

class PreparedDiffuseSpectralStretch
{
public:
    PreparedDiffuseSpectralStretch(
        DiffuseSpectralStretchConfiguration configuration,
        std::span<const double> source);

    [[nodiscard]] bool processSample(
        double reset,
        double gate,
        double level,
        double& output) noexcept;
    void reset() noexcept;
    // Takes a new Stretch, Smear, Diffusion and Seed for the same source and
    // starts afresh, without allocating; false, unchanged, when they leave
    // the accepted profile.
    [[nodiscard]] bool reconfigure(
        double stretch, double smear, double diffusion, double seed) noexcept;

    [[nodiscard]] const DiffuseSpectralStretchConfiguration&
    configuration() const noexcept;
    [[nodiscard]] std::size_t sourceSampleCount() const noexcept;
    [[nodiscard]] std::uint64_t synthesisFrameCount() const noexcept;
    [[nodiscard]] double analysisCursor() const noexcept;
    [[nodiscard]] bool exhausted() const noexcept;
    [[nodiscard]] bool synthesisSpectrumIsExactlyHermitian() const noexcept;
    [[nodiscard]] DiffuseSpectralStretchProcessingState
    makeProcessingStateCheckpoint() const;
    [[nodiscard]] bool captureProcessingState(
        DiffuseSpectralStretchProcessingState& state) const noexcept;
    [[nodiscard]] bool restoreProcessingState(
        const DiffuseSpectralStretchProcessingState& state) noexcept;
    [[nodiscard]] static constexpr std::size_t latencyFrames() noexcept
    {
        return diffuseSpectralStretchLatencyFrames;
    }

private:
  friend struct DiffuseSpectralStretchTestAccess;
  [[nodiscard]] bool generateFrame(double level) noexcept;
  [[nodiscard]] double sampleSource(double position) const noexcept;
  void clearProcessingState() noexcept;

  DiffuseSpectralStretchConfiguration configuration_;
  std::vector<double> source_;
  Radix2FftPlan plan_;
  std::vector<double> window_;
  std::vector<std::complex<double>> analysisSpectrum_;
  std::vector<std::complex<double>> synthesisSpectrum_;
  std::vector<double> previousAnalysisPhases_;
  std::vector<double> synthesisPhases_;
  std::vector<double> magnitudeMemory_;
  std::vector<double> candidateAnalysisPhases_;
  std::vector<double> candidateSynthesisPhases_;
  std::vector<double> candidateMagnitudes_;
  std::vector<double> overlapSamples_;
  std::vector<double> overlapWeights_;
  double analysisCursor_ = 0.0;
  std::size_t overlapReadIndex_ = 0;
  std::size_t hopFramesRemaining_ = 0;
  std::size_t startupFramesRemaining_ = diffuseSpectralStretchLatencyFrames;
  std::size_t tailFramesRemaining_ = 0;
  std::uint64_t synthesisFrameCount_ = 0;
  bool phaseInitialized_ = false;
  bool lastSynthesisSpectrumWasExactlyHermitian_ = true;
  bool sourceExhausted_ = false;
  bool previousResetHigh_ = false;
  bool gateActive_ = false;
};
}
