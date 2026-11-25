#include "talas/audio/GaAnalysis.h"
#include "talas/audio/ResourceLoader.h"
#include "talas/dsp/GaTable.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
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

constexpr double sampleRate = 48000.0;

// Two groups of harmonics of 220 Hz: 1 to 3 dying away, 5 to 7 rising.
std::vector<double> twoGroups()
{
    std::vector<double> samples(48000);
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
    {
        const auto t = static_cast<double>(frame) / sampleRate;
        const auto falling = std::exp(-4.0 * t);
        const auto rising = t;
        const auto partial = [&](int k) { return std::sin(2.0 * std::numbers::pi * 220.0 * k * t); };
        samples[frame] = falling * (0.4 * partial(1) + 0.2 * partial(2) + 0.12 * partial(3))
            + rising * (0.16 * partial(5) + 0.12 * partial(6) + 0.08 * partial(7));
    }
    return samples;
}

// A waveform's amplitude at harmonic k (of its 4096 samples' cycle).
double harmonicOf(const talas::dsp::ga::View& view, std::size_t waveform, int k)
{
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t n = 0; n < talas::dsp::ga::waveformLength; ++n)
    {
        const auto phase = 2.0 * std::numbers::pi * k * static_cast<double>(n)
            / static_cast<double>(talas::dsp::ga::waveformLength);
        real += view.waveform(waveform, n) * std::cos(phase);
        imaginary += view.waveform(waveform, n) * std::sin(phase);
    }
    return 2.0 * std::hypot(real, imaginary) / static_cast<double>(talas::dsp::ga::waveformLength);
}

// ADR-0027: a GA
// analysis made from a sample holds complex waveforms, 4096 samples each,
// then an amplitude envelope for each, then one frequency deviation
// envelope, all envelopes of one length; harmonics whose amplitude
// envelopes are alike share a waveform and its envelope.
void testGaAnalysisGroupsHarmonicsWithAlikeEnvelopes()
{
    talas::audio::GaAnalysisSettings settings;
    settings.waveforms = 2;
    settings.harmonics = 8;
    const talas::audio::GaAnalysisSettings defaults;
    require(defaults.harmonics == 124 && defaults.waveforms == 3,
        "the analysis should reduce 124 harmonics to 3 oscillators by default");
    const auto analysis = talas::audio::analyzeGa(twoGroups(), sampleRate, settings);
    require(analysis.ok(), "the GA analysis should succeed: " + analysis.error);
    const auto view = talas::dsp::ga::read(analysis.table);
    require(view.has_value() && view->waveforms == 2 && view->envelopeLength == 100
            && analysis.table.size() == talas::dsp::ga::headerSize + 2 * 4096 + 2 * 100 + 100,
        "the analysis should hold 2 waveforms of 4096 samples, 2 amplitude envelopes and one deviation envelope of "
        "100 frames, in that order");
    require(std::abs(analysis.basePitch - 220.0) < 2.0, "the base pitch should be the sound's, 220 Hz");
    // Which waveform holds the falling group.
    const auto falling = harmonicOf(*view, 0, 1) > harmonicOf(*view, 1, 1) ? std::size_t{0} : std::size_t{1};
    const auto rising = 1 - falling;
    for (const int k : {1, 2, 3})
        require(harmonicOf(*view, falling, k) > 0.1 && harmonicOf(*view, rising, k) < 0.01,
            "harmonic " + std::to_string(k) + " should be in the falling group's waveform alone");
    for (const int k : {5, 6, 7})
        require(harmonicOf(*view, rising, k) > 0.1 && harmonicOf(*view, falling, k) < 0.01,
            "harmonic " + std::to_string(k) + " should be in the rising group's waveform alone");
    require(view->amplitude(falling, 10) > 2.0 * view->amplitude(falling, 90)
            && view->amplitude(rising, 90) > 2.0 * view->amplitude(rising, 10),
        "each waveform should keep its group's own amplitude envelope");
    // Harmonic k's amplitude comes back as its waveform's share times the
    // envelope.
    for (const int frame : {20, 50, 80})
    {
        const auto t = frame / 100.0;
        const auto expected = 0.4 * std::exp(-4.0 * t);
        const auto got = harmonicOf(*view, falling, 1) * view->amplitude(falling, static_cast<std::size_t>(frame));
        require(std::abs(got - expected) < 0.1 * expected,
            "harmonic 1 should be its share of its group's envelope at frame " + std::to_string(frame));
    }
    for (std::size_t frame = 0; frame < view->envelopeLength; ++frame)
        require(std::abs(view->deviation(frame) - 1.0) < 0.02, "a steady pitch should deviate by nothing");

    // The file holds the same table.
    const auto path = std::filesystem::temp_directory_path() / "talas-ga-analysis-test GA.wav";
    const std::vector<std::vector<double>> channels{twoGroups()};
    const auto written = talas::audio::writeGaAnalysisFile(channels, sampleRate, settings, path);
    require(written.ok(), "the GA file should write: " + written.error);
    const auto loaded = talas::audio::loadTableResource(path);
    std::filesystem::remove(path);
    require(loaded.ok() && loaded.samples.size() == written.table.size() && talas::dsp::ga::read(loaded.samples).has_value()
            && loaded.samples[2] == 2.0 && loaded.samples[3] == 100.0,
        "the GA file should load back as a whole GA table");

    require(!talas::audio::analyzeGa(std::vector<double>(4800, 0.0), sampleRate, settings).ok(),
        "a silent sample should be refused, as it has no pitch");
    settings.waveforms = 0;
    require(!talas::audio::analyzeGa(twoGroups(), sampleRate, settings).ok(), "no waveforms should be refused");
}

// The GA analysis of a
// quasi-harmonic spectrum file, the same two groups as its harmonics'
// tracks, the fundamental rising a tone at the end.
void testGaAnalysisOfAHarmonicSpectrum()
{
    std::vector<talas::project::PartialTrackFrame> frames;
    for (std::uint64_t index = 0; index < 100; ++index)
    {
        const auto t = static_cast<double>(index) / 100.0;
        const auto fundamental = t < 0.5 ? 220.0 : 220.0 * std::pow(2.0, (t - 0.5) / 3.0);
        const auto falling = std::exp(-4.0 * t);
        const std::vector<std::pair<int, double>> levels{{1, 0.4 * falling}, {2, 0.2 * falling}, {3, 0.12 * falling},
            {5, 0.16 * t}, {6, 0.12 * t}, {7, 0.08 * t}};
        talas::project::PartialTrackFrame frame{1, index * 480, {}};
        for (int k = 1; k <= 7; ++k)
        {
            const auto found = std::ranges::find(levels, k, &std::pair<int, double>::first);
            frame.points.push_back({static_cast<std::uint64_t>(k), found == levels.end() ? 0.0 : found->second,
                fundamental * k, 0.0});
        }
        frames.push_back(std::move(frame));
    }
    const talas::project::PartialTrackAsset spectrum{sampleRate, 48000, std::move(frames)};
    talas::audio::GaAnalysisSettings settings;
    settings.waveforms = 2;
    settings.harmonics = 124;
    std::vector<talas::audio::GaAnalysisStage> stages;
    const auto analysis = talas::audio::analyzeGaFromSpectrum(spectrum, settings, [&](talas::audio::GaAnalysisStage stage) { stages.push_back(stage); });
    require(analysis.ok(), "the spectrum's GA analysis should succeed: " + analysis.error);
    // First the envelopes are read from the SOS analysis,
    // then the waveforms and their envelopes determined.
    require(stages == std::vector{talas::audio::GaAnalysisStage::ReadingEnvelopes, talas::audio::GaAnalysisStage::DeterminingWaveforms}
            && talas::audio::gaAnalysisStageText(stages.front()) == "Reading envelopes from the SOS analysis"
            && talas::audio::gaAnalysisStageText(stages.back()).starts_with("Determining waveforms and corresponding envelopes"),
        "the analysis should tell its two stages in turn");
    const auto view = talas::dsp::ga::read(analysis.table);
    require(view.has_value() && view->waveforms == 2 && view->envelopeLength == 100 && analysis.harmonics == 7,
        "the spectrum's harmonics, at most its tracks, reduce to 2 waveforms of 100 frames");
    require(std::abs(analysis.basePitch - 220.0) < 1.0, "the base pitch is the spectrum's median fundamental");
    const auto falling = harmonicOf(*view, 0, 1) > harmonicOf(*view, 1, 1) ? std::size_t{0} : std::size_t{1};
    const auto rising = 1 - falling;
    for (const int k : {1, 2, 3})
        require(harmonicOf(*view, falling, k) > 0.1 && harmonicOf(*view, rising, k) < 0.01,
            "harmonic " + std::to_string(k) + " of the spectrum should be in the falling group");
    for (const int k : {5, 6, 7})
        require(harmonicOf(*view, rising, k) > 0.1 && harmonicOf(*view, falling, k) < 0.01,
            "harmonic " + std::to_string(k) + " of the spectrum should be in the rising group");
    require(std::abs(view->deviation(20) - 1.0) < 1.0e-6 && std::abs(view->deviation(99) - std::pow(2.0, 0.49 / 3.0)) < 1.0e-3,
        "the deviation follows the spectrum's fundamental over the base pitch");

    const auto path = std::filesystem::temp_directory_path() / "talas-ga-from-spectrum GA.wav";
    const auto written = talas::audio::writeGaAnalysisFileFromSpectrum(spectrum, settings, path);
    const auto loaded = talas::audio::loadTableResource(path);
    std::filesystem::remove(path);
    require(written.ok() && loaded.ok() && loaded.samples.size() == written.table.size(),
        "the spectrum's GA file should load back as its GA table: " + written.error);
    require(!talas::audio::analyzeGaFromSpectrum({sampleRate, 0, {}}, settings).ok(), "an empty spectrum is refused");
}
}

int main()
{
    try
    {
        testGaAnalysisGroupsHarmonicsWithAlikeEnvelopes();
        testGaAnalysisOfAHarmonicSpectrum();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "GA analysis tests passed\n";
    return 0;
}
