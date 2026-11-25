#include "talas/audio/AnalyzerDisplayMapping.h"

#include <algorithm>
#include <cmath>

namespace talas::audio
{
namespace
{
[[nodiscard]] double safeZoom(double zoom)
{
    return std::isfinite(zoom) && zoom >= 1.0
        ? zoom
        : 1.0;
}

[[nodiscard]] double safeCoordinate(double coordinate)
{
    return std::isfinite(coordinate)
        ? std::clamp(coordinate, 0.0, 1.0)
        : 0.0;
}

[[nodiscard]] std::size_t nearestIndex(
    std::size_t count,
    double normalizedX)
{
    if (count < 2)
        return 0;
    const auto continuous =
        safeCoordinate(normalizedX)
        * static_cast<double>(count - 1);
    return static_cast<std::size_t>(
        std::floor(continuous + 0.5));
}

[[nodiscard]] double safeMagnitude(double magnitude)
{
    return std::isfinite(magnitude)
        ? std::clamp(magnitude, 0.0, 1.0)
        : 0.0;
}
}

double moveNormalizedAnalyzerCursor(
    double current,
    AnalyzerCursorMove move,
    bool fineStep)
{
    const auto position = std::isfinite(current)
        ? std::clamp(current, 0.0, 1.0)
        : 0.5;
    switch (move)
    {
    case AnalyzerCursorMove::Previous:
        return std::max(0.0, position - (fineStep ? 0.001 : 0.01));
    case AnalyzerCursorMove::Next:
        return std::min(1.0, position + (fineStep ? 0.001 : 0.01));
    case AnalyzerCursorMove::First:
        return 0.0;
    case AnalyzerCursorMove::Last:
        return 1.0;
    }
    return position;
}

double zoomedOscilloscopeSample(double sample, double zoom)
{
    return std::clamp(
        normalizedOscilloscopeSample(sample) * safeZoom(zoom),
        -1.0,
        1.0);
}

double oscilloscopeAmplitudeAtHeight(double pixelsAboveCentre, double fullScalePixels, double zoom)
{
    if (!std::isfinite(pixelsAboveCentre) || !std::isfinite(fullScalePixels) || fullScalePixels <= 0.0)
        return 0.0;
    return std::clamp(pixelsAboveCentre / fullScalePixels / safeZoom(zoom), -1.0, 1.0);
}

std::optional<ScopeCursorReadout> scopeCursorReadout(
    std::span<const double> samples,
    const OscilloscopeWindow& window,
    double sampleRate,
    double normalizedX)
{
    if (window.frameCount == 0
        || window.firstFrame > samples.size()
        || window.frameCount
            > samples.size() - window.firstFrame
        || !std::isfinite(sampleRate)
        || sampleRate <= 0.0)
    {
        return std::nullopt;
    }
    const auto offset =
        nearestIndex(window.frameCount, normalizedX);
    ScopeCursorReadout result;
    result.sampleOffset = offset;
    result.amplitude = normalizedOscilloscopeSample(
        samples[window.firstFrame + offset]);
    result.timeSeconds =
        static_cast<double>(offset) / sampleRate;
    return result;
}

std::size_t visibleSpectrumBinCount(
    std::size_t totalBinCount,
    double zoom)
{
    const auto view = selectSpectrumView(
        totalBinCount,
        zoom,
        0.0,
        SpectrumFrequencyScale::Linear);
    return view.firstBin <= view.lastBin
        ? view.lastBin - view.firstBin + 1
        : 0;
}

SpectrumView selectSpectrumView(
    std::size_t totalBinCount,
    double zoom,
    double pan,
    SpectrumFrequencyScale scale)
{
    SpectrumView result;
    result.scale = scale;
    if (totalBinCount == 0)
        return result;

    const auto lastAvailable = totalBinCount - 1;
    const auto normalizedPan = safeCoordinate(pan);
    if (scale == SpectrumFrequencyScale::Linear)
    {
        const auto visibleIntervals =
            static_cast<std::size_t>(std::floor(
                static_cast<double>(lastAvailable)
                / safeZoom(zoom)));
        const auto maxFirst =
            lastAvailable - visibleIntervals;
        result.firstBin = static_cast<std::size_t>(
            std::floor(
                normalizedPan * static_cast<double>(maxFirst)
                + 0.5));
        result.lastBin =
            result.firstBin + visibleIntervals;
        return result;
    }

    if (lastAvailable == 0)
        return result;
    if (lastAvailable == 1)
    {
        result.firstBin = 1;
        result.lastBin = 1;
        return result;
    }

    const auto logLast =
        std::log(static_cast<double>(lastAvailable));
    const auto visibleLogWidth =
        logLast / safeZoom(zoom);
    const auto logFirst =
        normalizedPan * (logLast - visibleLogWidth);
    const auto logEnd = logFirst + visibleLogWidth;
    result.firstBin = std::clamp<std::size_t>(
        static_cast<std::size_t>(
            std::llround(std::exp(logFirst))),
        1,
        lastAvailable);
    result.lastBin = std::clamp<std::size_t>(
        static_cast<std::size_t>(
            std::llround(std::exp(logEnd))),
        result.firstBin,
        lastAvailable);
    return result;
}

double spectrumNormalizedX(
    const SpectrumView& view,
    std::size_t bin)
{
    if (view.firstBin > view.lastBin
        || (view.scale == SpectrumFrequencyScale::Logarithmic
            && view.firstBin == 0)
        || view.firstBin == view.lastBin)
    {
        return 0.0;
    }
    const auto boundedBin =
        std::clamp(bin, view.firstBin, view.lastBin);
    if (view.scale == SpectrumFrequencyScale::Linear)
    {
        return static_cast<double>(
            boundedBin - view.firstBin)
            / static_cast<double>(
                view.lastBin - view.firstBin);
    }
    const auto first =
        std::log(static_cast<double>(view.firstBin));
    const auto last =
        std::log(static_cast<double>(view.lastBin));
    if (!(last > first))
        return 0.0;
    return (
        std::log(static_cast<double>(boundedBin)) - first)
        / (last - first);
}

std::size_t spectrumBinAt(
    const SpectrumView& view,
    double normalizedX)
{
    if (view.firstBin > view.lastBin
        || (view.scale == SpectrumFrequencyScale::Logarithmic
            && view.firstBin == 0))
        return 0;
    if (view.firstBin == view.lastBin)
        return view.firstBin;
    const auto x = safeCoordinate(normalizedX);
    if (view.scale == SpectrumFrequencyScale::Linear)
    {
        return view.firstBin
            + nearestIndex(
                view.lastBin - view.firstBin + 1,
                x);
    }
    const auto first =
        std::log(static_cast<double>(view.firstBin));
    const auto last =
        std::log(static_cast<double>(view.lastBin));
    return std::clamp<std::size_t>(
        static_cast<std::size_t>(
            std::llround(std::exp(first + x * (last - first)))),
        view.firstBin,
        view.lastBin);
}

std::optional<SpectrumCursorReadout> spectrumCursorReadout(
    std::span<const double> magnitudes,
    std::size_t frameCount,
    double sampleRate,
    double zoom,
    double normalizedX)
{
    return spectrumCursorReadout(
        magnitudes,
        frameCount,
        sampleRate,
        selectSpectrumView(
            magnitudes.size(),
            zoom,
            0.0,
            SpectrumFrequencyScale::Linear),
        normalizedX);
}

std::optional<SpectrumCursorReadout> spectrumCursorReadout(
    std::span<const double> magnitudes,
    std::size_t frameCount,
    double sampleRate,
    const SpectrumView& view,
    double normalizedX)
{
    if (view.firstBin > view.lastBin
        || (view.scale == SpectrumFrequencyScale::Logarithmic
            && view.firstBin == 0)
        || view.lastBin >= magnitudes.size()
        || frameCount == 0
        || !std::isfinite(sampleRate)
        || sampleRate <= 0.0)
    {
        return std::nullopt;
    }
    SpectrumCursorReadout result;
    result.bin = spectrumBinAt(view, normalizedX);
    result.normalizedMagnitude =
        safeMagnitude(magnitudes[result.bin]);
    result.decibels =
        -90.0 + 90.0 * result.normalizedMagnitude;
    result.frequencyHz =
        static_cast<double>(result.bin)
        * sampleRate
        / static_cast<double>(frameCount);
    return result;
}
}
