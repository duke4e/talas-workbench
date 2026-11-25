#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <iostream>
#include <numbers>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace talas::test
{
// Pure source and measurement mathematics, independent of production DSP.
struct TimeFrequencyScaleLowOutputReference
{
    static constexpr double sampleRate = 48000, rate = 0.75;
    static constexpr double tau = 2 * std::numbers::pi;
    static constexpr std::size_t window = 8192, latency = window + window / 4 - 1;
    double frequency, scale, dc;
    bool cosine = false;

    [[nodiscard]] std::size_t measurementFrames() const { return scale < 0.25 ? 384000 : 96000; }
    [[nodiscard]] std::size_t outputFrames() const { return latency + (scale < 0.25 ? 528000 : 240000); }
    [[nodiscard]] std::vector<double> recording() const
    {
        std::vector<double> samples(scale < 0.25 ? 432000 : 192000);
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            const auto phase = tau * frequency * i / sampleRate;
            samples[i] = dc + 0.25 * (cosine ? std::cos(phase) : std::sin(phase));
        }
        return samples;
    }
    [[nodiscard]] static double magnitude(std::span<const double> samples, double frequency)
    {
        std::complex<double> projection{};
        for (std::size_t i = 0; i < samples.size(); ++i)
            projection += samples[i] * (0.5 - 0.5 * std::cos(tau * i / samples.size()))
                * std::polar(1.0, -tau * frequency * i / sampleRate);
        return std::abs(projection) / samples.size();
    }
    [[nodiscard]] bool verify(std::span<const double> output, std::string_view label) const
    {
        if (output.size() != outputFrames()) throw std::runtime_error("low-output reference shape");
        const auto measured = output.last(measurementFrames());
        const auto target = frequency * scale;
        // Whole cycles separate DC from every tone, including 1.25Hz. The
        // source and synthesis support are interior and Rate never equals Scale.
        const auto wanted = magnitude(measured, target);
        const auto wrong = std::max(magnitude(measured, frequency), magnitude(measured, frequency * rate));
        double best = 0, peakOffset = 0;
        for (double offset = -0.5; offset <= 0.5; offset += 0.125)
        {
            const auto value = magnitude(measured, target + offset);
            if (value > best) { best = value; peakOffset = offset; }
        }
        const auto mean = std::accumulate(measured.begin(), measured.end(), 0.0) / measured.size();
        std::complex<double> coefficient{};
        for (std::size_t i = 0; i < measured.size(); ++i)
            coefficient += measured[i] * std::polar(1.0, -tau * target * i / sampleRate);
        coefficient *= 2.0 / measured.size();
        double residualEnergy = 0;
        for (std::size_t i = 0; i < measured.size(); ++i)
        {
            const auto residual = measured[i] - dc
                - (coefficient * std::polar(1.0, tau * target * i / sampleRate)).real();
            residualEnergy += residual * residual;
        }
        const auto residualRms = std::sqrt(residualEnergy / measured.size());
        const auto valid = wanted > 0.045 && wanted < 0.08 && wanted > 10 * wrong
            && std::abs(peakOffset) <= 0.25 && std::abs(mean - dc) < 0.006
            && residualRms < 0.035
            && std::ranges::all_of(output, [](double value) { return std::isfinite(value); })
            && std::ranges::all_of(output.first(latency), [](double value) { return value == 0; });
        std::cout << "low_output " << label << " source=" << frequency << " scale=" << scale
            << " rate=" << rate << " dc=" << dc << " cosine=" << cosine << " wanted=" << wanted
            << " wrong=" << wrong << " peak_offset_hz=" << peakOffset
            << " amplitude=" << std::abs(coefficient) << " mean=" << mean
            << " residual_rms=" << residualRms << " pass=" << valid << std::endl;
        return valid;
    }
};
}
