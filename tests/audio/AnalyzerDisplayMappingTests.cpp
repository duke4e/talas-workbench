#include "talas/audio/AnalyzerDisplayMapping.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    require(
        std::abs(actual - expected) <= tolerance,
        message + ": expected " + std::to_string(expected)
            + ", got " + std::to_string(actual));
}

void testScopeZoomClipsOnlyDisplayCoordinate()
{
    requireNear(
        talas::audio::zoomedOscilloscopeSample(0.6, 2.0),
        1.0,
        1.0e-12,
        "positive zoom should clip at the plot boundary");
    requireNear(
        talas::audio::zoomedOscilloscopeSample(-0.4, 2.0),
        -0.8,
        1.0e-12,
        "in-range zoom should retain its scaled coordinate");
    requireNear(
        talas::audio::zoomedOscilloscopeSample(
            std::numeric_limits<double>::quiet_NaN(),
            4.0),
        0.0,
        1.0e-12,
        "non-finite samples should remain drawing-safe");
    // The cursor's height reads as the amplitude there.
    requireNear(talas::audio::oscilloscopeAmplitudeAtHeight(23.0, 46.0, 1.0), 0.5, 1.0e-12,
        "halfway up the trace's reach should read half full scale");
    requireNear(talas::audio::oscilloscopeAmplitudeAtHeight(-23.0, 46.0, 2.0), -0.25, 1.0e-12,
        "zoomed in twice, the same height should read half the amplitude");
    requireNear(talas::audio::oscilloscopeAmplitudeAtHeight(100.0, 46.0, 1.0), 1.0, 1.0e-12,
        "a height above the reach should read full scale");
}

void testScopeCursorUsesNearestVisibleSample()
{
    const std::vector<double> samples{
        -1.0, -0.5, 0.2, 0.3, 0.4, 0.5};
    const talas::audio::OscilloscopeWindow window{2, 4, true};

    const auto left = talas::audio::scopeCursorReadout(
        samples,
        window,
        1000.0,
        0.0);
    require(
        left.has_value()
            && left->sampleOffset == 0,
        "left endpoint should select the first visible sample");
    requireNear(left->amplitude, 0.2, 1.0e-12,
                "scope cursor should report the raw visible amplitude");
    requireNear(left->timeSeconds, 0.0, 1.0e-12,
                "scope time should be relative to the visible window");

    const auto middle = talas::audio::scopeCursorReadout(
        samples,
        window,
        1000.0,
        0.5);
    require(
        middle.has_value()
            && middle->sampleOffset == 2,
        "halfway should round to the nearest visible sample");
    requireNear(middle->amplitude, 0.4, 1.0e-12,
                "nearest scope sample should drive the readout");
    requireNear(middle->timeSeconds, 0.002, 1.0e-12,
                "scope cursor should report exact sample-relative time");

    const auto right = talas::audio::scopeCursorReadout(
        samples,
        window,
        1000.0,
        1.0);
    require(
        right.has_value()
            && right->sampleOffset == 3,
        "right endpoint should stay inside the visible window");
}

void testSpectrumZoomKeepsDcAndSelectedUpperEndpoint()
{
    require(
        talas::audio::visibleSpectrumBinCount(129, 1.0) == 129,
        "1x spectrum zoom should retain every bin");
    require(
        talas::audio::visibleSpectrumBinCount(129, 2.0) == 65,
        "2x spectrum zoom should end at half Nyquist");
    require(
        talas::audio::visibleSpectrumBinCount(129, 4.0) == 33,
        "4x spectrum zoom should end at quarter Nyquist");
    require(
        talas::audio::visibleSpectrumBinCount(1, 4.0) == 1,
        "a DC-only spectrum should remain addressable");
}

void testLinearSpectrumViewPansAcrossExactBoundedBins()
{
    const auto low = talas::audio::selectSpectrumView(
        129,
        4.0,
        0.0,
        talas::audio::SpectrumFrequencyScale::Linear);
    const auto middle = talas::audio::selectSpectrumView(
        129,
        4.0,
        0.5,
        talas::audio::SpectrumFrequencyScale::Linear);
    const auto high = talas::audio::selectSpectrumView(
        129,
        4.0,
        1.0,
        talas::audio::SpectrumFrequencyScale::Linear);
    require(
        low.firstBin == 0 && low.lastBin == 32,
        "low linear viewport should preserve the legacy quarter-Nyquist range");
    require(
        middle.firstBin == 48 && middle.lastBin == 80,
        "middle linear viewport should use the normalized pan position");
    require(
        high.firstBin == 96 && high.lastBin == 128,
        "high linear viewport should end exactly at Nyquist");
}

void testLogSpectrumViewUsesPositiveBinsAndRoundTripsCoordinates()
{
    const auto view = talas::audio::selectSpectrumView(
        129,
        1.0,
        0.75,
        talas::audio::SpectrumFrequencyScale::Logarithmic);
    require(
        view.firstBin == 1 && view.lastBin == 128,
        "full log view should exclude only the non-logarithmic DC bin");
    requireNear(
        talas::audio::spectrumNormalizedX(view, 1),
        0.0,
        1.0e-12,
        "first positive bin should map to the left endpoint");
    requireNear(
        talas::audio::spectrumNormalizedX(view, 128),
        1.0,
        1.0e-12,
        "last bin should map to the right endpoint");
    require(
        talas::audio::spectrumBinAt(view, 0.0) == 1
            && talas::audio::spectrumBinAt(view, 1.0) == 128,
        "log viewport endpoints should round-trip to exact bins");

    const auto lowHalf = talas::audio::selectSpectrumView(
        129,
        2.0,
        0.0,
        talas::audio::SpectrumFrequencyScale::Logarithmic);
    const auto highHalf = talas::audio::selectSpectrumView(
        129,
        2.0,
        1.0,
        talas::audio::SpectrumFrequencyScale::Logarithmic);
    require(
        lowHalf.firstBin == 1
            && lowHalf.lastBin < highHalf.lastBin
            && highHalf.firstBin > lowHalf.firstBin
            && highHalf.lastBin == 128,
        "log pan should move a bounded logarithmic viewport");
}

void testSpectrumCursorReportsExactBinFrequencyAndDecibels()
{
    std::vector<double> magnitudes(129, 0.0);
    magnitudes[32] = 0.5;
    const auto result = talas::audio::spectrumCursorReadout(
        magnitudes,
        256,
        48000.0,
        4.0,
        1.0);
    require(
        result.has_value() && result->bin == 32,
        "right endpoint should select the last visible zoomed bin");
    requireNear(result->frequencyHz, 6000.0, 1.0e-12,
                "cursor frequency should use the exact DFT bin");
    requireNear(result->decibels, -45.0, 1.0e-12,
                "cursor dB should invert the normalized display range");
    requireNear(result->normalizedMagnitude, 0.5, 1.0e-12,
                "cursor should preserve the primary trace magnitude");
}

void testSpectrumCursorUsesSharedPannedLogViewport()
{
    std::vector<double> magnitudes(129, 0.0);
    magnitudes[128] = 0.75;
    const auto view = talas::audio::selectSpectrumView(
        magnitudes.size(),
        2.0,
        1.0,
        talas::audio::SpectrumFrequencyScale::Logarithmic);
    const auto result = talas::audio::spectrumCursorReadout(
        magnitudes,
        256,
        48000.0,
        view,
        1.0);
    require(
        result.has_value() && result->bin == 128,
        "cursor should use the same panned log endpoint as drawing");
    requireNear(result->frequencyHz, 24000.0, 1.0e-12,
                "shared viewport cursor should report exact bin frequency");
    requireNear(result->decibels, -22.5, 1.0e-12,
                "shared viewport cursor should report the selected magnitude");
}

void testCursorMappingsRejectEmptyRangesAndSanitizeCoordinates()
{
    const std::vector<double> empty;
    const auto noScope = talas::audio::scopeCursorReadout(
        empty,
        {},
        48000.0,
        0.5);
    const auto noSpectrum = talas::audio::spectrumCursorReadout(
        empty,
        256,
        48000.0,
        1.0,
        0.5);
    require(
        !noScope.has_value() && !noSpectrum.has_value(),
        "empty display ranges should not fabricate cursor data");

    const auto invalidScope = talas::audio::scopeCursorReadout(
        std::vector<double>{0.0},
        talas::audio::OscilloscopeWindow{1, 1, false},
        48000.0,
        0.0);
    require(
        !invalidScope.has_value(),
        "out-of-range scope windows should be rejected");

    const std::vector<double> spectrum{
        std::numeric_limits<double>::quiet_NaN(),
        0.5};
    const auto safe = talas::audio::spectrumCursorReadout(
        spectrum,
        2,
        48000.0,
        1.0,
        std::numeric_limits<double>::quiet_NaN());
    require(
        safe.has_value() && safe->bin == 0,
        "non-finite cursor coordinates should sanitize to the left edge");
    require(
        safe->normalizedMagnitude == 0.0
            && safe->decibels == -90.0,
        "non-finite spectrum magnitudes should map to the display floor");

    const auto noTimeAxis = talas::audio::scopeCursorReadout(
        std::vector<double>{0.0, 0.5},
        talas::audio::OscilloscopeWindow{0, 2, false},
        0.0,
        1.0);
    const auto noFrequencyAxis = talas::audio::spectrumCursorReadout(
        std::vector<double>{0.0, 0.5},
        0,
        48000.0,
        1.0,
        1.0);
    require(
        !noTimeAxis.has_value()
            && !noFrequencyAxis.has_value(),
        "cursor readouts should not fabricate unavailable axes");

    const auto invalidLogView =
        talas::audio::spectrumCursorReadout(
            std::vector<double>{0.0, 0.5},
            2,
            48000.0,
            talas::audio::SpectrumView{
                0,
                1,
                talas::audio::SpectrumFrequencyScale::Logarithmic},
            0.5);
    require(
        !invalidLogView.has_value(),
        "logarithmic views that include DC should be rejected");
}

void testKeyboardCursorMovementIsBoundedAndSupportsFineSteps()
{
    requireNear(
        talas::audio::moveNormalizedAnalyzerCursor(
            0.5,
            talas::audio::AnalyzerCursorMove::Previous),
        0.49,
        1.0e-12,
        "left arrow should move the analyzer cursor by one percent");
    requireNear(
        talas::audio::moveNormalizedAnalyzerCursor(
            0.5,
            talas::audio::AnalyzerCursorMove::Next,
            true),
        0.501,
        1.0e-12,
        "Shift plus arrow should provide a fine analyzer cursor step");
    requireNear(
        talas::audio::moveNormalizedAnalyzerCursor(
            0.0,
            talas::audio::AnalyzerCursorMove::Previous),
        0.0,
        1.0e-12,
        "cursor movement should remain bounded at the left edge");
    requireNear(
        talas::audio::moveNormalizedAnalyzerCursor(
            0.4,
            talas::audio::AnalyzerCursorMove::First),
        0.0,
        1.0e-12,
        "Home should select the first visible measurement");
    requireNear(
        talas::audio::moveNormalizedAnalyzerCursor(
            0.4,
            talas::audio::AnalyzerCursorMove::Last),
        1.0,
        1.0e-12,
        "End should select the last visible measurement");
}
}

int main()
{
    testScopeZoomClipsOnlyDisplayCoordinate();
    testScopeCursorUsesNearestVisibleSample();
    testSpectrumZoomKeepsDcAndSelectedUpperEndpoint();
    testLinearSpectrumViewPansAcrossExactBoundedBins();
    testLogSpectrumViewUsesPositiveBinsAndRoundTripsCoordinates();
    testSpectrumCursorReportsExactBinFrequencyAndDecibels();
    testSpectrumCursorUsesSharedPannedLogViewport();
    testCursorMappingsRejectEmptyRangesAndSanitizeCoordinates();
    testKeyboardCursorMovementIsBoundedAndSupportsFineSteps();
    return 0;
}
