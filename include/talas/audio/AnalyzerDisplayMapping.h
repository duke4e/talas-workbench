#pragma once

#include "talas/audio/OscilloscopeAnalysis.h"

#include <cstddef>
#include <optional>
#include <span>

namespace talas::audio
{
struct ScopeCursorReadout
{
    std::size_t sampleOffset = 0;
    double timeSeconds = 0.0;
    double amplitude = 0.0;
};

struct SpectrumCursorReadout
{
    std::size_t bin = 0;
    double frequencyHz = 0.0;
    double normalizedMagnitude = 0.0;
    double decibels = -90.0;
};

enum class SpectrumFrequencyScale
{
    Linear,
    Logarithmic,
};

struct SpectrumView
{
    std::size_t firstBin = 1;
    std::size_t lastBin = 0;
    SpectrumFrequencyScale scale =
        SpectrumFrequencyScale::Linear;
};

enum class AnalyzerCursorMove
{
    Previous,
    Next,
    First,
    Last
};

[[nodiscard]] double moveNormalizedAnalyzerCursor(
    double current,
    AnalyzerCursorMove move,
    bool fineStep = false);

[[nodiscard]] double zoomedOscilloscopeSample(
    double sample,
    double zoom);
// The amplitude at the cursor's height in the oscilloscope: its distance above
// the centre line over the trace's full-scale reach, divided by the zoom;
// clipped to -1..1.
[[nodiscard]] double oscilloscopeAmplitudeAtHeight(
    double pixelsAboveCentre,
    double fullScalePixels,
    double zoom);

[[nodiscard]] std::optional<ScopeCursorReadout>
scopeCursorReadout(
    std::span<const double> samples,
    const OscilloscopeWindow& window,
    double sampleRate,
    double normalizedX);

[[nodiscard]] std::size_t visibleSpectrumBinCount(
    std::size_t totalBinCount,
    double zoom);

[[nodiscard]] SpectrumView selectSpectrumView(
    std::size_t totalBinCount,
    double zoom,
    double pan,
    SpectrumFrequencyScale scale);

[[nodiscard]] double spectrumNormalizedX(
    const SpectrumView& view,
    std::size_t bin);

[[nodiscard]] std::size_t spectrumBinAt(
    const SpectrumView& view,
    double normalizedX);

[[nodiscard]] std::optional<SpectrumCursorReadout>
spectrumCursorReadout(
    std::span<const double> magnitudes,
    std::size_t frameCount,
    double sampleRate,
    double zoom,
    double normalizedX);

[[nodiscard]] std::optional<SpectrumCursorReadout>
spectrumCursorReadout(
    std::span<const double> magnitudes,
    std::size_t frameCount,
    double sampleRate,
    const SpectrumView& view,
    double normalizedX);
}
