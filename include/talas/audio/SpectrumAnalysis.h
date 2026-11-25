#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace talas::audio
{
// The window applied before the FFT, one of LinearEnvelope (a triangle),
// Hamming, Hann and Long Kaiser, whose resolution rises in that order: the
// leakage of a tone four bins and more from it falls from one to the next.
// Long Kaiser is taken as a Kaiser window of beta 12 (our choice), its main
// lobe within four bins of the tone and its leakage past them below the
// display's -90 dB.
enum class SpectrumWindow
{
    Rectangular,
    Hann,
    Blackman,
    LinearEnvelope,
    Hamming,
    LongKaiser,
};

// The window's weight at a frame of a window `frameCount` frames long.
[[nodiscard]] double spectrumWindowValue(SpectrumWindow window, std::size_t frame, std::size_t frameCount) noexcept;

struct SpectrumAnalysisResult
{
    std::size_t frameCount = 0;
    std::vector<double> magnitudes;
};

[[nodiscard]] SpectrumAnalysisResult analyzeNormalizedSpectrum(
    std::span<const double> samples,
    std::size_t requestedFrameCount,
    SpectrumWindow window);

void updateSpectrumAverage(
    std::span<const double> current,
    std::size_t depth,
    std::vector<double>& state);

void updateSpectrumPeakHold(
    std::span<const double> current,
    std::vector<double>& peaks);

void updateSpectrumDisplay(
    std::span<const double> current,
    std::size_t averagingDepth,
    bool peakHoldEnabled,
    std::vector<double>& average,
    std::vector<double>& peaks);
}
