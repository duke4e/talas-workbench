#include "talas/dsp/SystemWavetables.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace talas::dsp
{
namespace
{
constexpr std::array<std::string_view, 21> names{
    "ATAN20",
    "ATAN40",
    "Buzz",
    "Cosine",
    "ExponRev",
    "Exponential",
    "FourExpons",
    "FullRamp",
    "Gaussian",
    "GlottalPulse",
    "Hamming",
    "Hann",
    "LinearEnvelope",
    "Ramp",
    "Random",
    "Rectangular",
    "Saw21",
    "Sine",
    "Square21",
    "Triangle",
    "Zero",
};

constexpr std::array<std::string_view, 4> tableNames{
    "HarmonicAmplitudes",
    "HarmonicSpectrum",
    "Resonance",
    "Terrain",
};

template <typename Shape>
std::vector<double> tabulate(Shape shape)
{
    std::vector<double> table(systemWavetableLength);
    for (std::size_t index = 0; index < table.size(); ++index)
        table[index] = shape(static_cast<double>(index));
    return table;
}

// One cycle summing the first 21 harmonics, each harmonic's sine (or
// cosine) times its amplitude, scaled so that its peak is 1.
template <typename Amplitude>
std::vector<double> harmonics(bool cosine, Amplitude amplitude)
{
    constexpr auto twoPi = 2.0 * std::numbers::pi;
    constexpr auto length = static_cast<double>(systemWavetableLength);
    auto table = tabulate([&](double index) {
        double sum = 0.0;
        for (int number = 1; number <= 21; ++number)
        {
            const auto phase = twoPi * number * index / length;
            sum += amplitude(number) * (cosine ? std::cos(phase) : std::sin(phase));
        }
        return sum;
    });
    double peak = 0.0;
    for (const auto value : table)
        peak = std::max(peak, std::abs(value));
    for (auto& value : table)
        value /= peak;
    return table;
}
}

std::span<const std::string_view> systemWavetableNames() noexcept
{
    return names;
}

std::span<const std::string_view> systemTableNames() noexcept
{
    return tableNames;
}

std::optional<std::vector<double>> systemTable(std::string_view name)
{
    constexpr auto twoPi = 2.0 * std::numbers::pi;
    if (name == "HarmonicAmplitudes")
    {
        constexpr std::size_t partials = 8;
        double harmonicSum = 0.0;
        for (std::size_t partial = 1; partial <= partials; ++partial)
            harmonicSum += 1.0 / static_cast<double>(partial);
        std::vector<double> amplitudes;
        for (std::size_t partial = 1; partial <= partials; ++partial)
            amplitudes.push_back(1.0 / static_cast<double>(partial) / harmonicSum);
        return amplitudes;
    }
    if (name == "HarmonicSpectrum")
    {
        constexpr std::size_t partials = 8;
        double harmonicSum = 0.0;
        for (std::size_t partial = 1; partial <= partials; ++partial)
            harmonicSum += 1.0 / static_cast<double>(partial);
        std::vector<double> triples;
        for (std::size_t partial = 1; partial <= partials; ++partial)
        {
            const auto number = static_cast<double>(partial);
            triples.insert(triples.end(), {1.0 / number / harmonicSum, number, 0.0});
        }
        return triples;
    }
    if (name == "Resonance")
        return std::vector<double>{2.0, -0.9, 0.5};
    if (name == "Terrain")
    {
        constexpr std::size_t side = 8;
        std::vector<double> terrain;
        for (std::size_t row = 0; row < side; ++row)
        {
            for (std::size_t column = 0; column < side; ++column)
            {
                terrain.push_back(std::sin(twoPi * static_cast<double>(column) / side)
                    * std::cos(twoPi * static_cast<double>(row) / side));
            }
        }
        return terrain;
    }
    return systemWavetable(name);
}

std::optional<std::vector<double>> systemWavetable(std::string_view name)
{
    constexpr auto length = static_cast<double>(systemWavetableLength);
    // Periodic tables divide by the length, since the next cycle starts
    // where this one ends; one-shot tables divide by the last index, so
    // they reach their end value.
    constexpr auto last = length - 1.0;
    constexpr auto twoPi = 2.0 * std::numbers::pi;
    if (name == "Sine")
        return tabulate([&](double index) { return std::sin(twoPi * index / length); });
    if (name == "Cosine")
        return tabulate([&](double index) { return std::cos(twoPi * index / length); });
    if (name == "Triangle")
    {
        return tabulate([&](double index) {
            const auto phase = index / length;
            if (phase < 0.25)
                return 4.0 * phase;
            if (phase < 0.75)
                return 2.0 - 4.0 * phase;
            return 4.0 * phase - 4.0;
        });
    }
    if (name == "Saw21")
        return harmonics(false, [](int number) { return -1.0 / number; });
    if (name == "Square21")
        return harmonics(false, [](int number) { return number % 2 == 1 ? 1.0 / number : 0.0; });
    if (name == "Buzz")
        return harmonics(true, [](int) { return 1.0; });
    if (name == "GlottalPulse")
    {
        // A raised cosine opening over 40% of the cycle, a quarter cosine
        // closing over the next 16%, then closed, the TwoFormantElement's
        // pulse.
        return tabulate([&](double index) {
            const auto phase = index / length;
            if (phase < 0.40)
                return 0.5 * (1.0 - std::cos(std::numbers::pi * phase / 0.40));
            if (phase < 0.56)
                return std::cos(0.5 * std::numbers::pi * (phase - 0.40) / 0.16);
            return 0.0;
        });
    }
    // Exponential rises from 0 to 1 over 60 dB, (1000^x - 1) / 999, and
    // ExponRev falls back, the same curve reversed; FourExpons is four
    // ExponRevs in a row; Random is uniform from -1 to 1, the same each time.
    const auto rising = [](double phase) { return (std::pow(1000.0, phase) - 1.0) / 999.0; };
    if (name == "Exponential")
        return tabulate([&](double index) { return rising(index / last); });
    if (name == "ExponRev")
        return tabulate([&](double index) { return rising((last - index) / last); });
    if (name == "FourExpons")
    {
        constexpr auto quarter = length / 4.0;
        return tabulate([&](double index) {
            return rising(1.0 - std::fmod(index, quarter) / (quarter - 1.0));
        });
    }
    if (name == "Random")
    {
        std::uint32_t seed = 0x2545F491U;
        return tabulate([&](double) {
            seed ^= seed << 13U;
            seed ^= seed >> 17U;
            seed ^= seed << 5U;
            return 2.0 * static_cast<double>(seed) / 4294967295.0 - 1.0;
        });
    }
    if (name == "FullRamp")
        return tabulate([&](double index) { return -1.0 + 2.0 * index / last; });
    // The Waveshapers ATAN20 and ATAN40 are arctangents over an
    // input from -1 to 1, atan(20 x) and atan(40 x) scaled to reach 1: they
    // raise small samples about 13 and 26 times and round off loud ones.
    if (name == "ATAN20" || name == "ATAN40")
    {
        const auto gain = name == "ATAN20" ? 20.0 : 40.0;
        return tabulate([&](double index) {
            return std::atan(gain * (-1.0 + 2.0 * index / last)) / std::atan(gain);
        });
    }
    if (name == "Ramp")
        return tabulate([&](double index) { return index / last; });
    if (name == "Gaussian")
    {
        return tabulate([&](double index) {
            const auto distance = (index - last / 2.0) / (length / 8.0);
            return std::exp(-0.5 * distance * distance);
        });
    }
    if (name == "LinearEnvelope")
        return tabulate([&](double index) { return 1.0 - std::abs(2.0 * index / last - 1.0); });
    if (name == "Hann")
        return tabulate([&](double index) { return 0.5 - 0.5 * std::cos(twoPi * index / last); });
    if (name == "Hamming")
        return tabulate([&](double index) { return 0.54 - 0.46 * std::cos(twoPi * index / last); });
    if (name == "Rectangular")
        return std::vector<double>(systemWavetableLength, 1.0);
    if (name == "Zero")
        return std::vector<double>(systemWavetableLength, 0.0);
    return std::nullopt;
}
}
