#pragma once

#include "TimeFrequencyScaleLowOutputReference.h"

namespace talas::test
{
// Independent full-rate measurement. No production FFT/estimator is used.
struct TimeFrequencyScaleDensePartialsReference
{
    using ToneReference = TimeFrequencyScaleLowOutputReference;
    static constexpr double sampleRate = ToneReference::sampleRate, tau = ToneReference::tau;
    static constexpr std::size_t window = 8192, latency = 10239, measurementFrames = 384000;
    std::size_t partials;
    double scale, rate;
    bool descending;
    static constexpr double dc = .1;
    [[nodiscard]] static constexpr std::size_t outputFrames() { return latency + 528000; }
    [[nodiscard]] static double frequency(std::size_t i) { return 20 + 8 * i; }
    [[nodiscard]] double amplitude(std::size_t i) const { return descending ? .2 / (i+1) : .04; }
    [[nodiscard]] static double phase(std::size_t i) { return .37 - .63 * i; }
    [[nodiscard]] std::vector<double> recording() const
    {
        std::vector<double> samples(432000, dc);
        for (std::size_t i = 0; i < samples.size(); ++i)
            for (std::size_t k = 0; k < partials; ++k)
                samples[i] += amplitude(k) * std::sin(tau * frequency(k) * i / sampleRate + phase(k));
        return samples;
    }
    // General finite-length Goertzel with the final phase rotation restored.
    [[nodiscard]] static std::complex<double> projection(std::span<const double> samples, double hz)
    {
        const auto omega = tau * hz / sampleRate, cosine = std::cos(omega), sine = std::sin(omega);
        double first = 0, second = 0;
        for (const auto value : samples)
        {
            const auto next = value + 2 * cosine * first - second;
            second = first; first = next;
        }
        return std::complex{first - cosine * second, sine * second}
            * std::polar(1.0, -omega * (samples.size()-1)) / static_cast<double>(samples.size());
    }
    [[nodiscard]] std::vector<double> ideal(int defect = 0) const
    {
        std::vector<double> output(outputFrames());
        for (std::size_t i = latency; i < output.size(); ++i)
        {
            const auto time = static_cast<double>(i - latency) / sampleRate;
            output[i] = dc + (defect == 4 ? .02 : 0);
            for (std::size_t k = 0; k < partials; ++k)
            {
                const auto gain = k+1 != partials ? 1.0 : defect == 1 ? 1.26 : defect == 2 ? 0.0 : 1.0;
                output[i] += gain * amplitude(k) * std::sin(tau * frequency(k)
                    * (defect == 3 ? rate : scale) * time + phase(k));
            }
        }
        return output;
    }
    [[nodiscard]] bool verify(std::span<const double> output, bool print = true) const
    {
        if (output.size() != outputFrames()) throw std::runtime_error("dense reference shape");
        const auto measured = output.last(measurementFrames);
        std::vector<double> weighted(measurementFrames);
        double centeredEnergy = 0, mean = 0;
        for (std::size_t i = 0; i < measured.size(); ++i)
        {
            weighted[i] = measured[i] * (.5 - .5 * std::cos(tau*i/measured.size()));
            centeredEnergy += (measured[i]-dc)*(measured[i]-dc); mean += measured[i];
        }
        centeredEnergy /= measured.size(); mean /= measured.size();
        double wrong = 0;
        for (std::size_t k = 0; k < partials; ++k)
            for (const auto rival : {frequency(k), frequency(k)*rate})
            {
                bool wanted = false;
                for (std::size_t j = 0; j < partials; ++j) wanted |= rival == frequency(j)*scale;
                if (!wanted) wrong = std::max(wrong, std::abs(projection(weighted, rival)));
            }
        double minGain = 1e300, maxGain = 0, residualEnergy = centeredEnergy, maxPeakOffset = 0;
        bool valid = true;
        for (std::size_t k = 0; k < partials; ++k)
        {
            const auto target = frequency(k)*scale, wanted = std::abs(projection(weighted, target));
            const auto gain = 4*wanted/amplitude(k);
            minGain = std::min(minGain, gain); maxGain = std::max(maxGain, gain);
            const auto coefficient = 2.0*projection(measured, target);
            residualEnergy -= .5*std::norm(coefficient);
            double best = 0, peakOffset = 0;
            const auto extent = std::min(.5, 4*scale);
            for (double offset = -extent; offset <= extent; offset += .125)
            {
                const auto value = std::abs(projection(weighted, target+offset));
                if (value > best) { best = value; peakOffset = offset; }
            }
            maxPeakOffset = std::max(maxPeakOffset, std::abs(peakOffset));
            valid &= gain > .72 && gain < 1.28 && wanted > 10*wrong && std::abs(peakOffset) <= .25;
        }
        const auto residual = std::sqrt(std::max(0.0, residualEnergy));
        valid &= minGain/maxGain > .8 && residualEnergy >= -1e-7 && residual < .035
            && std::abs(mean-dc) < .006
            && std::ranges::all_of(output, [](double value) { return std::isfinite(value); })
            && std::ranges::all_of(output.first(latency), [](double value) { return value == 0; });
        if (print) std::cout << "dense_partials count=" << partials << " scale=" << scale << " rate=" << rate
            << " descending=" << descending << " min_gain=" << minGain << " max_gain=" << maxGain
            << " ratio_gain=" << minGain/maxGain << " max_peak_offset=" << maxPeakOffset
            << " wrong=" << wrong << " mean=" << mean << " residual_rms=" << residual
            << " residual_energy=" << residualEnergy << " pass=" << valid << std::endl;
        return valid;
    }
};
}
