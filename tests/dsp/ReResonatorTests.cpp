#include "talas/dsp/ReAnalysis.h"
#include "talas/dsp/ReResonator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace re = talas::dsp::re_resonator;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// A sound whose two resonances move, as speech's do: a pulse train
// through two two-pole sections gliding apart.
std::vector<double> gliding(double sampleRate, std::size_t frames)
{
    std::vector<double> sound(frames);
    double y1 = 0.0, y2 = 0.0, z1 = 0.0, z2 = 0.0;
    for (std::size_t n = 0; n < frames; ++n)
    {
        const auto time = static_cast<double>(n) / sampleRate;
        const auto pulse = n % 400 == 0 ? 1.0 : 0.0;
        const auto radius = 0.995;
        const auto first = -2.0 * radius * std::cos(2.0 * std::numbers::pi * (500.0 + 300.0 * time) / sampleRate);
        const auto second = -2.0 * radius * std::cos(2.0 * std::numbers::pi * (1500.0 - 400.0 * time) / sampleRate);
        const auto low = pulse - first * y1 - radius * radius * y2;
        y2 = y1;
        y1 = low;
        const auto high = 0.05 * low - second * z1 - radius * radius * z2;
        z2 = z1;
        z1 = high;
        sound[n] = high;
    }
    const auto peak = std::ranges::max(sound, {}, [](double sample) { return std::abs(sample); });
    for (auto& sample : sound)
        sample *= 0.8 / std::abs(peak);
    return sound;
}

void testTablesAreValidated()
{
    const std::vector<double> valid{2.0, 0.5, -0.25, 0.75, 0.0};
    const auto table = re::parseTable(valid);
    require(table.has_value() && table->order == 2 && table->frameCount == 2,
        "an order and two sets of two coefficients should make a table");
    for (const auto& invalid : std::vector<std::vector<double>>{
             {}, {2.0}, {0.0, 0.5}, {2.5, 0.5, 0.5}, {129.0, 0.5}, {2.0, 0.5, 0.5, 0.5},
             {1.0, 1.0}, {1.0, -1.0}, {1.0, std::nan("")}})
    {
        require(!re::parseTable(invalid).has_value(),
            "a table without a whole order, whole coefficient sets, or coefficients inside -1 to 1 should be refused");
    }
}

// TimeIndex -1 reads the first coefficient set and 1 the last.
void testTimeIndexReadsFromFirstToLastSet()
{
    require(re::framePosition(-1.0, 5) == 0.0 && re::framePosition(1.0, 5) == 4.0
            && re::framePosition(0.0, 5) == 2.0 && re::framePosition(-3.0, 5) == 0.0
            && re::framePosition(2.0, 5) == 4.0 && re::framePosition(0.5, 1) == 0.0,
        "TimeIndex should read from the first set at -1 to the last at 1");
    const std::vector<double> samples{1.0, -0.5, 0.5};
    const auto table = re::parseTable(samples);
    std::vector<double> coefficient(1);
    re::interpolate(*table, re::framePosition(0.0, table->frameCount), coefficient);
    require(coefficient[0] == 0.0, "between two sets the coefficients should be interpolated");
}

// A first-order lattice with reflection coefficient k is 1 / (1 + k z^-1).
void testLatticeIsTheAllPoleFilter()
{
    const std::vector<double> coefficient{-0.9};
    std::vector<double> state(1, 0.0);
    double output = 0.0;
    for (std::size_t n = 0; n < 8; ++n)
    {
        require(re::synthesize(coefficient, state, n == 0 ? 1.0 : 0.0, output),
            "the lattice should stay finite");
        require(std::abs(output - std::pow(0.9, static_cast<double>(n))) < 1.0e-15,
            "a one-pole lattice's impulse response should decay by its coefficient");
    }
}

// The analysis splits a sound into a time-varying resonator
// and the excitation that recreates the sound exactly.
void testAnalysisExcitationRecreatesTheSound()
{
    constexpr double sampleRate = 48000.0;
    const auto sound = gliding(sampleRate, 24000);
    const auto analysis = talas::dsp::analyzeRe(sound, sampleRate, {}, true);
    require(analysis.ok(), "the analysis should succeed: " + analysis.error);
    const auto table = re::parseTable(analysis.table);
    require(table.has_value() && table->order == 32 && table->frameCount == 51,
        "half a second at 100 updates per second should give 51 sets of 32 coefficients");
    std::vector<double> coefficients(table->order);
    std::vector<double> state(table->order, 0.0);
    double excitationEnergy = 0.0;
    double soundEnergy = 0.0;
    for (std::size_t n = 0; n < sound.size(); ++n)
    {
        const auto timeIndex = -1.0 + 2.0 * static_cast<double>(n) / static_cast<double>(sound.size() - 1);
        re::interpolate(*table, re::framePosition(timeIndex, table->frameCount), coefficients);
        double output = 0.0;
        require(re::synthesize(coefficients, state, analysis.excitation[n], output),
            "resynthesis should stay finite");
        require(std::abs(output - sound[n]) < 1.0e-12,
            "the excitation through the resonator should recreate the sound at frame " + std::to_string(n));
        excitationEnergy += analysis.excitation[n] * analysis.excitation[n];
        soundEnergy += sound[n] * sound[n];
    }
    require(excitationEnergy < 1.0e-3 * soundEnergy,
        "the resonator should carry the sound's resonances, leaving a faint excitation");
}

// Noise from a fixed seed, between -0.5 and 0.5.
std::vector<double> noise(std::size_t frames, std::uint32_t seed)
{
    std::vector<double> samples(frames);
    for (auto& sample : samples)
    {
        seed = seed * 1664525U + 1013904223U;
        sample = static_cast<double>(seed) / 4294967296.0 - 0.5;
    }
    return samples;
}

// Noise through two-pole resonances in series, which stay put: an all-pole
// sound of twice as many poles as resonances.
std::vector<double> resonant(double sampleRate, std::size_t frames, const std::vector<double>& centres)
{
    auto sound = noise(frames, 12345);
    constexpr auto radius = 0.98;
    for (const auto centre : centres)
    {
        const auto feedback = -2.0 * radius * std::cos(2.0 * std::numbers::pi * centre / sampleRate);
        double y1 = 0.0, y2 = 0.0;
        for (auto& sample : sound)
        {
            const auto out = sample - feedback * y1 - radius * radius * y2;
            y2 = y1;
            y1 = out;
            sample = out;
        }
    }
    const auto peak = std::ranges::max(sound, {}, [](double sample) { return std::abs(sample); });
    for (auto& sample : sound)
        sample *= 0.8 / std::abs(peak);
    return sound;
}

// The magnitude at a frequency of a signal's first frames.
double magnitude(const std::vector<double>& signal, double frequency, double sampleRate)
{
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        const auto phase = 2.0 * std::numbers::pi * frequency * static_cast<double>(n) / sampleRate;
        re += signal[n] * std::cos(phase);
        im -= signal[n] * std::sin(phase);
    }
    return std::hypot(re, im);
}

// The middle coefficient set's resonator driven by `input`.
std::vector<double> throughMiddleSet(const talas::dsp::ReAnalysisResult& analysis, const std::vector<double>& input)
{
    const auto table = re::parseTable(analysis.table);
    require(table.has_value(), "the analysis should give a valid RE table");
    std::vector<double> coefficients(table->order);
    re::interpolate(*table, re::framePosition(0.0, table->frameCount), coefficients);
    std::vector<double> state(table->order, 0.0);
    std::vector<double> output(input.size());
    for (std::size_t n = 0; n < input.size(); ++n)
        require(re::synthesize(coefficients, state, input[n], output[n]), "the resonator should stay finite");
    return output;
}

// The filter order is about twice the resonant peaks it can
// track, so order 4 finds a sound's two resonances and order 2 one; the
// updates per second set how many filters there are; and an RE table
// imposes the analysed sound's formants on another input, here other noise
// (cross synthesis).
void testAnalysisSettingsAndCrossSynthesis()
{
    constexpr double sampleRate = 48000.0;
    const auto sound = resonant(sampleRate, 24000, {1000.0, 3000.0});
    std::vector<double> impulse(4096, 0.0);
    impulse.front() = 1.0;
    // The frequencies between 100 and 8000 hz where the resonator peaks.
    const auto peaks = [&](std::size_t order) {
        const auto analysis = talas::dsp::analyzeRe(sound, sampleRate, {order, 100.0, 0.02}, false);
        require(analysis.ok(), "the analysis should succeed: " + analysis.error);
        const auto response = throughMiddleSet(analysis, impulse);
        std::vector<double> levels;
        for (double frequency = 100.0; frequency <= 8000.0; frequency += 25.0)
            levels.push_back(magnitude(response, frequency, sampleRate));
        std::vector<double> found;
        for (std::size_t index = 1; index + 1 < levels.size(); ++index)
            if (levels[index] > levels[index - 1] && levels[index] > levels[index + 1])
                found.push_back(100.0 + 25.0 * static_cast<double>(index));
        return found;
    };
    const auto four = peaks(4);
    require(four.size() == 2 && std::abs(four[0] - 1000.0) <= 150.0 && std::abs(four[1] - 3000.0) <= 150.0,
        "order 4 should find the two resonances, not " + std::to_string(four.size()) + " peaks");
    require(peaks(2).size() == 1, "order 2 should track one resonant peak");
    const auto sets = [&](double updates) {
        const auto analysis = talas::dsp::analyzeRe(sound, sampleRate, {4, updates, 0.02}, false);
        return analysis.ok() ? re::parseTable(analysis.table)->frameCount : 0;
    };
    require(sets(100.0) == 51 && sets(200.0) == 101,
        "twice the updates per second should make twice the filters over half a second");

    const auto crossed = throughMiddleSet(talas::dsp::analyzeRe(sound, sampleRate, {4, 100.0, 0.02}, false),
        noise(24000, 777));
    // The mean magnitude over 200 hz around a frequency.
    const auto band = [&](double centre) {
        double sum = 0.0;
        for (double frequency = centre - 100.0; frequency <= centre + 100.0; frequency += 10.0)
            sum += magnitude(crossed, frequency, sampleRate);
        return sum / 21.0;
    };
    require(band(1000.0) > 4.0 * band(2000.0) && band(3000.0) > band(2000.0) && band(3000.0) > band(4000.0),
        "other noise through the RE table should take the analysed sound's formants");
}

// The averaging time is how much of the sound each
// snapshot analyses, not the inverse of the updates per second: windows of
// 40 ms at 100 updates a second overlap, and give other filters than
// windows of 5 ms, as many of them.
void testAveragingTimeIsNotTheUpdateInterval()
{
    constexpr double sampleRate = 48000.0;
    const auto sound = resonant(sampleRate, 24000, {1000.0, 3000.0});
    const auto overlapping = talas::dsp::analyzeRe(sound, sampleRate, {4, 100.0, 0.04}, false);
    const auto short_ = talas::dsp::analyzeRe(sound, sampleRate, {4, 100.0, 0.005}, false);
    require(overlapping.ok() && short_.ok(), "windows longer than the update interval should overlap");
    const auto longer = re::parseTable(overlapping.table);
    const auto shorter = re::parseTable(short_.table);
    require(longer.has_value() && shorter.has_value() && longer->frameCount == shorter->frameCount
            && overlapping.table != short_.table,
        "the averaging time should change the filters, not how many there are");
}

void testAnalysisRefusesInvalidSettings()
{
    const std::vector<double> sound(1000, 0.25);
    require(!talas::dsp::analyzeRe({}, 48000.0, {}, false).ok()
            && !talas::dsp::analyzeRe(sound, 48000.0, {0, 100.0, 0.02}, false).ok()
            && !talas::dsp::analyzeRe(sound, 48000.0, {129, 100.0, 0.02}, false).ok()
            && !talas::dsp::analyzeRe(sound, 48000.0, {32, 0.0, 0.02}, false).ok()
            && !talas::dsp::analyzeRe(sound, 48000.0, {32, 100.0, -1.0}, false).ok()
            && !talas::dsp::analyzeRe(sound, 0.0, {}, false).ok(),
        "the analysis should refuse an empty sound and invalid settings");
    const auto silence = talas::dsp::analyzeRe(std::vector<double>(4800, 0.0), 48000.0, {4, 100.0, 0.02}, true);
    require(silence.ok() && std::all_of(silence.table.begin() + 1, silence.table.end(), [](double k) { return k == 0.0; }),
        "silence should analyze to a resonator that passes its input unchanged");
}

void testTableTextLoadsBack()
{
    const std::vector<double> table{2.0, 0.5, -0.25, 0.1, 0.2};
    const auto text = talas::dsp::formatReTable(table);
    require(text.starts_with("# RE table") && text.find("\n2\n0.5 -0.25\n0.1 0.2\n") != std::string::npos,
        "an RE table file should hold the order and one coefficient set per line");
}
}

int main()
{
    testTablesAreValidated();
    testTimeIndexReadsFromFirstToLastSet();
    testLatticeIsTheAllPoleFilter();
    testAnalysisExcitationRecreatesTheSound();
    testAnalysisSettingsAndCrossSynthesis();
    testAveragingTimeIsNotTheUpdateInterval();
    testAnalysisRefusesInvalidSettings();
    testTableTextLoadsBack();
    return 0;
}
