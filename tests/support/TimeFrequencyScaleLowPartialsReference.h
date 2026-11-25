#pragma once

#include "TimeFrequencyScaleLowOutputReference.h"

#include <array>

namespace talas::test
{
struct TimeFrequencyScaleLowPartialsReference
{
    using ToneReference = TimeFrequencyScaleLowOutputReference;
    static constexpr double sampleRate = ToneReference::sampleRate, tau = ToneReference::tau;
    static constexpr std::size_t measurementFrames = 384000;
    double upperFrequency, scale, rate, dc;
    bool quietFundamental, shiftedPhases;
    std::size_t window = ToneReference::window;

    [[nodiscard]] std::size_t latencyFrames() const { return window + window / 4 - 1; }
    [[nodiscard]] std::size_t outputFrames() const { return latencyFrames() + 528000; }

    [[nodiscard]] std::array<double, 2> frequencies() const { return {20, upperFrequency}; }
    [[nodiscard]] std::array<double, 2> amplitudes() const
    { return quietFundamental ? std::array{0.08, 0.25} : std::array{0.25, 0.08}; }
    [[nodiscard]] std::vector<double> recording() const
    {
        std::vector<double> samples(432000, dc);
        const auto frequency = frequencies(), amplitude = amplitudes();
        const std::array phases = shiftedPhases ? std::array{0.73, -1.1} : std::array{0.0, 0.0};
        for (std::size_t i = 0; i < samples.size(); ++i)
            for (std::size_t partial = 0; partial < frequency.size(); ++partial)
                samples[i] += amplitude[partial]
                    * std::sin(tau * frequency[partial] * i / sampleRate + phases[partial]);
        return samples;
    }
    [[nodiscard]] bool verify(std::span<const double> output, std::string_view label, bool print = true) const
    {
        if (output.size() != outputFrames()) throw std::runtime_error("low-partial reference shape");
        const auto measured = output.last(measurementFrames);
        const auto frequency = frequencies(), amplitude = amplitudes();
        const std::array targets{frequency[0] * scale, frequency[1] * scale};
        std::array<double, 2> wanted{}, gains{}, peakOffsets{};
        std::array<std::complex<double>, 2> coefficients{};
        double wrong = 0;
        for (const auto rival : {frequency[0], frequency[1], frequency[0] * rate, frequency[1] * rate})
            // A resampled rival may coincide with the other wanted partial.
            if (rival != targets[0] && rival != targets[1])
                wrong = std::max(wrong, ToneReference::magnitude(measured, rival));
        bool valid = true;
        for (std::size_t partial = 0; partial < frequency.size(); ++partial)
        {
            wanted[partial] = ToneReference::magnitude(measured, targets[partial]);
            gains[partial] = 4 * wanted[partial] / amplitude[partial];
            double best = 0;
            const auto scanExtent = std::min(0.5, (targets[1] - targets[0]) / 2);
            for (double offset = -0.5; offset <= 0.5; offset += 0.125)
            {
                if (std::abs(offset) > scanExtent) continue;
                const auto value = ToneReference::magnitude(measured, targets[partial] + offset);
                if (value > best) { best = value; peakOffsets[partial] = offset; }
            }
            for (std::size_t i = 0; i < measured.size(); ++i)
                coefficients[partial] += measured[i]
                    * std::polar(1.0, -tau * targets[partial] * i / sampleRate);
            coefficients[partial] *= 2.0 / measured.size();
            valid = valid && gains[partial] > 0.72 && gains[partial] < 1.28
                && wanted[partial] > 10 * wrong && std::abs(peakOffsets[partial]) <= 0.25;
        }
        double residualEnergy = 0;
        for (std::size_t i = 0; i < measured.size(); ++i)
        {
            auto residual = measured[i] - dc;
            for (std::size_t partial = 0; partial < frequency.size(); ++partial)
                residual -= (coefficients[partial]
                    * std::polar(1.0, tau * targets[partial] * i / sampleRate)).real();
            residualEnergy += residual * residual;
        }
        const auto residualRms = std::sqrt(residualEnergy / measured.size());
        const auto mean = std::accumulate(measured.begin(), measured.end(), 0.0) / measured.size();
        const auto ratioGain = gains[1] / gains[0];
        valid = valid && ratioGain > 0.8 && ratioGain < 1.25 && residualRms < 0.035
            && std::abs(mean - dc) < 0.006
            && std::ranges::all_of(output, [](double value) { return std::isfinite(value); })
            && std::ranges::all_of(output.first(latencyFrames()), [](double value) { return value == 0; });
        if (print) std::cout << "low_partials " << label << " window=" << window
            << " upper=" << upperFrequency << " scale=" << scale
            << " rate=" << rate << " dc=" << dc << " quiet_fundamental=" << quietFundamental
            << " shifted_phases=" << shiftedPhases << " gain0=" << gains[0] << " gain1=" << gains[1]
            << " ratio_gain=" << ratioGain << " peak0=" << peakOffsets[0] << " peak1=" << peakOffsets[1]
            << " wrong=" << wrong << " mean=" << mean << " residual_rms=" << residualRms
            << " pass=" << valid << std::endl;
        return valid;
    }
};
}
