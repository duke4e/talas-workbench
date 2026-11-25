#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace talas::dsp
{
// The RE Analysis Tool's parameters: the filter order is about twice the
// number of resonant peaks, the updates are coefficient sets per second, and
// the averaging time, about twice the sound's period, is how much of it each
// set is taken from. The defaults are our choice.
struct ReAnalysisSettings
{
    std::size_t order = 32;
    double updatesPerSecond = 100.0;
    double averagingTimeSeconds = 0.02;
};

struct ReAnalysisResult
{
    // An RE table (ADR-0022): the order, then the coefficient sets.
    std::vector<double> table;
    // The excitation that recreates the sound through the REResonator when
    // its TimeIndex ramps from -1 to 1 over the sound's duration.
    std::vector<double> excitation;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

// Splits a sound into the time-varying resonator and, when asked, the
// excitation that recreates it. Coefficient sets are spaced evenly over the
// sound, as close to the requested rate as a whole number of them allows;
// each comes from the autocorrelation of a Hann-windowed stretch of the
// averaging time centred on it (Levinson-Durbin).
[[nodiscard]] ReAnalysisResult analyzeRe(
    std::span<const double> samples,
    double sampleRate,
    const ReAnalysisSettings& settings,
    bool withExcitation);

// The RE table as a numeric table file, one coefficient set per line.
[[nodiscard]] std::string formatReTable(std::span<const double> table);
}
