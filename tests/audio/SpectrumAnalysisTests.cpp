#include "talas/audio/SpectrumAnalysis.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <ranges>
#include <span>
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

std::vector<double> onBinSine(std::size_t frameCount, std::size_t bin)
{
    std::vector<double> samples(frameCount, 0.0);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        samples[frame] = std::sin(
            2.0 * std::numbers::pi
            * static_cast<double>(bin)
            * static_cast<double>(frame)
            / static_cast<double>(frameCount));
    }
    return samples;
}

void testNormalizesOnBinSineAcrossWindows()
{
    const auto samples = onBinSine(64, 4);
    for (const auto window : {
             talas::audio::SpectrumWindow::Rectangular,
             talas::audio::SpectrumWindow::Hann,
             talas::audio::SpectrumWindow::Blackman,
         })
    {
        const auto result =
            talas::audio::analyzeNormalizedSpectrum(samples, 64, window);
        require(result.frameCount == 64,
                "spectrum should report the analyzed frame count");
        require(result.magnitudes.size() == 33,
                "64-frame real spectrum should contain 33 bins");
        const auto peak = std::max_element(
            result.magnitudes.begin(),
            result.magnitudes.end());
        require(
            peak == result.magnitudes.begin() + 4,
            "on-bin sine should peak at its source bin");
        require(
            *peak > 0.995,
            "coherent-gain normalization should retain a full-scale peak");
    }
}

// Selecting a SpectrumAnalyzerDisplay shows a
// real-time spectrum analyzer whose Window is the weighting applied to the
// FFT's analysis window and whose Length is the FFT's length, ideally the
// number of samples in the period of the lowest frequency.
void testAnalyzerWindowAndLengthAsDocumented()
{
    std::vector<double> offBin(64);
    for (std::size_t frame = 0; frame < offBin.size(); ++frame)
        offBin[frame] = std::sin(2.0 * std::numbers::pi * 4.5 * static_cast<double>(frame) / 64.0);
    const auto far = [&](talas::audio::SpectrumWindow window) {
        return talas::audio::analyzeNormalizedSpectrum(offBin, 64, window).magnitudes[20];
    };
    require(far(talas::audio::SpectrumWindow::Rectangular) > far(talas::audio::SpectrumWindow::Hann) + 0.2
            && far(talas::audio::SpectrumWindow::Rectangular) > far(talas::audio::SpectrumWindow::Blackman) + 0.2,
        "the Window should weight the analysis window, a Hann or Blackman leaking less than none");

    const auto eightCycles = onBinSine(128, 8);
    const auto longer = talas::audio::analyzeNormalizedSpectrum(eightCycles, 128, talas::audio::SpectrumWindow::Rectangular);
    const auto shorter = talas::audio::analyzeNormalizedSpectrum(eightCycles, 64, talas::audio::SpectrumWindow::Rectangular);
    require(longer.magnitudes.size() == 65 && shorter.magnitudes.size() == 33
            && std::ranges::max_element(longer.magnitudes) - longer.magnitudes.begin() == 8
            && std::ranges::max_element(shorter.magnitudes) - shorter.magnitudes.begin() == 4,
        "the Length should be the FFT's length, a longer one resolving finer bins");

    // A tone of period 40 samples with its second and third harmonics.
    std::vector<double> tone(256);
    for (std::size_t frame = 0; frame < tone.size(); ++frame)
    {
        const auto phase = 2.0 * std::numbers::pi * static_cast<double>(frame) / 40.0;
        tone[frame] = 0.5 * std::sin(phase) + 0.25 * std::sin(2.0 * phase) + 0.125 * std::sin(3.0 * phase);
    }
    const auto period = talas::audio::analyzeNormalizedSpectrum(tone, 40, talas::audio::SpectrumWindow::Rectangular).magnitudes;
    const auto other = talas::audio::analyzeNormalizedSpectrum(tone, 64, talas::audio::SpectrumWindow::Rectangular).magnitudes;
    // Amplitudes of 0.5, 0.25 and 0.125 read -6, -12 and -18 dB, on a
    // display from -90 dB.
    require(std::abs(period[1] - 84.0 / 90.0) < 0.01 && std::abs(period[2] - 78.0 / 90.0) < 0.01
            && std::abs(period[3] - 72.0 / 90.0) < 0.01 && period[4] < 0.05 && period[10] < 0.05
            && *std::max_element(other.begin() + 8, other.end()) > 0.3,
        "a Length of the lowest frequency's period should resolve its harmonics on their own bins");
}

// Each window function has properties of its own,
// and resolution generally rises from LinearEnvelope through Hamming and
// Hann to Long Kaiser. Here resolution is how far below a full-scale tone
// its leakage lies four bins and more from it, past the Hann's and
// Hamming's main lobe, which is how quiet a neighbouring partial the display
// still shows apart from it. Each of the four keeps a full-scale on-bin tone
// at full scale on its own bin.
void testTheManualsWindowsResolveInItsOrder()
{
    const std::vector<talas::audio::SpectrumWindow> order{talas::audio::SpectrumWindow::LinearEnvelope,
        talas::audio::SpectrumWindow::Hamming, talas::audio::SpectrumWindow::Hann, talas::audio::SpectrumWindow::LongKaiser};
    for (const auto window : order)
    {
        const auto onBin = talas::audio::analyzeNormalizedSpectrum(onBinSine(256, 40), 256, window);
        const auto peak = std::ranges::max_element(onBin.magnitudes);
        require(peak - onBin.magnitudes.begin() == 40 && *peak > 0.995,
            "each window should keep an on-bin tone at full scale on its bin");
        require(talas::audio::spectrumWindowValue(window, 128, 256) > 0.99
                && talas::audio::spectrumWindowValue(window, 0, 256) < 0.1,
            "each window should weight the middle of the analysis fully and its edges little");
    }
    for (const auto tone : {38.4, 38.65, 38.9})
    {
        std::vector<double> samples(256);
        for (std::size_t frame = 0; frame < samples.size(); ++frame)
            samples[frame] = std::sin(2.0 * std::numbers::pi * tone * static_cast<double>(frame) / 256.0);
        // The loudest bin four or more bins from the tone, on the display's
        // scale (0 at -90 dB, 1 at full scale).
        std::vector<double> leakage;
        for (const auto window : order)
        {
            const auto spectrum = talas::audio::analyzeNormalizedSpectrum(samples, 256, window).magnitudes;
            double loudest = 0.0;
            for (std::size_t bin = 0; bin < spectrum.size(); ++bin)
            {
                if (std::abs(static_cast<double>(bin) - tone) >= 4.0)
                    loudest = std::max(loudest, spectrum[bin]);
            }
            leakage.push_back(loudest);
        }
        // Two dB at least between each window and the next.
        for (std::size_t index = 0; index + 1 < leakage.size(); ++index)
        {
            require(leakage[index] > leakage[index + 1] + 2.0 / 90.0,
                "resolution should rise from LinearEnvelope through Hamming and Hann to Long Kaiser, at a tone of "
                    + std::to_string(tone) + " bins: " + std::to_string(leakage[index]) + " then "
                    + std::to_string(leakage[index + 1]));
        }
    }
}

void testUsesLastRequestedSamplesAndReportsShortInput()
{
    std::vector<double> samples(128, 0.0);
    const auto tail = onBinSine(64, 4);
    std::copy(tail.begin(), tail.end(), samples.begin() + 64);

    const auto last64 = talas::audio::analyzeNormalizedSpectrum(
        samples,
        64,
        talas::audio::SpectrumWindow::Rectangular);
    require(last64.frameCount == 64,
            "requested length should select exactly the newest samples");
    require(
        std::max_element(
            last64.magnitudes.begin(),
            last64.magnitudes.end())
            == last64.magnitudes.begin() + 4,
        "last-N analysis should ignore older samples");

    const auto shortInput = talas::audio::analyzeNormalizedSpectrum(
        std::span<const double>{samples}.first(32),
        256,
        talas::audio::SpectrumWindow::Hann);
    require(shortInput.frameCount == 32
                && shortInput.magnitudes.size() == 17,
            "short input should be analyzed honestly without padding");
}

void testSilenceAndNonFiniteSamplesStayFinite()
{
    std::vector<double> samples(64, 0.0);
    samples[7] = std::numeric_limits<double>::quiet_NaN();
    samples[11] = std::numeric_limits<double>::infinity();
    const auto result = talas::audio::analyzeNormalizedSpectrum(
        samples,
        64,
        talas::audio::SpectrumWindow::Blackman);
    require(
        std::ranges::all_of(
            result.magnitudes,
            [](double value) {
                return std::isfinite(value)
                    && value >= 0.0
                    && value <= 1.0;
            }),
        "display magnitudes should remain finite and normalized");
    require(
        std::ranges::all_of(
            result.magnitudes,
            [](double value) { return value == 0.0; }),
        "non-finite samples should be treated as silence");
}

void testExponentialAveragingSeedsAndBlends()
{
    std::vector<double> state;
    const std::vector<double> first{1.0, 0.0};
    talas::audio::updateSpectrumAverage(first, 4, state);
    require(state == first,
            "first averaged spectrum should seed state exactly");

    const std::vector<double> second{0.0, 1.0};
    talas::audio::updateSpectrumAverage(second, 4, state);
    requireNear(state[0], 0.75, 1.0e-12,
                "4x average should retain three quarters of prior state");
    requireNear(state[1], 0.25, 1.0e-12,
                "4x average should add one quarter of current state");

    talas::audio::updateSpectrumAverage(second, 1, state);
    require(state == second,
            "1x averaging should expose the current spectrum directly");
}

void testPeakHoldRetainsPerBinMaximumAndCanReset()
{
    std::vector<double> peaks;
    talas::audio::updateSpectrumPeakHold(
        std::vector<double>{0.25, 0.75},
        peaks);
    talas::audio::updateSpectrumPeakHold(
        std::vector<double>{0.5, 0.25},
        peaks);
    require(
        peaks == std::vector<double>({0.5, 0.75}),
        "peak hold should retain each bin's maximum");

    peaks.clear();
    talas::audio::updateSpectrumPeakHold(
        std::vector<double>{0.1, 0.2},
        peaks);
    require(
        peaks == std::vector<double>({0.1, 0.2}),
        "cleared peak hold should seed from the next frame");
}

void testDisplayUpdateHoldsRawTransientWhileAveragingTrace()
{
    std::vector<double> average;
    std::vector<double> peaks;
    talas::audio::updateSpectrumDisplay(
        std::vector<double>{0.0},
        8,
        true,
        average,
        peaks);
    talas::audio::updateSpectrumDisplay(
        std::vector<double>{1.0},
        8,
        true,
        average,
        peaks);

    requireNear(
        average[0],
        0.125,
        1.0e-12,
        "8x average should smooth a one-frame transient");
    requireNear(
        peaks[0],
        1.0,
        1.0e-12,
        "peak hold should retain the raw transient before averaging");
}
}

int main()
{
    testNormalizesOnBinSineAcrossWindows();
    testAnalyzerWindowAndLengthAsDocumented();
    testTheManualsWindowsResolveInItsOrder();
    testUsesLastRequestedSamplesAndReportsShortInput();
    testSilenceAndNonFiniteSamplesStayFinite();
    testExponentialAveragingSeedsAndBlends();
    testPeakHoldRetainsPerBinMaximumAndCanReset();
    testDisplayUpdateHoldsRawTransientWhileAveragingTrace();
    return 0;
}
