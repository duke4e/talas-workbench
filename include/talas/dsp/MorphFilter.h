#pragma once

#include <cmath>
#include <numbers>

namespace talas::dsp
{
class MorphFilter
{
public:
    [[nodiscard]] bool prepare(double sampleRate) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
            return false;
        prepared_ = true;
        sampleRate_ = sampleRate;
        reset();
        return true;
    }

    void reset() noexcept
    {
        integrator1_ = 0.0;
        integrator2_ = 0.0;
    }

    [[nodiscard]] bool copyProcessingStateFrom(
        const MorphFilter& source) noexcept
    {
        if (sampleRate_ != source.sampleRate_
            || prepared_ != source.prepared_)
        {
            return false;
        }
        integrator1_ = source.integrator1_;
        integrator2_ = source.integrator2_;
        return true;
    }

    [[nodiscard]] bool processSample(
        double input,
        double cutoff,
        double resonance,
        double morph,
        double drive,
        double scale,
        double& output) noexcept
    {
        if (!prepared_ || !std::isfinite(input) || !std::isfinite(cutoff)
            || cutoff <= 0.0 || cutoff >= sampleRate_ * 0.45
            || !inUnitInterval(resonance) || !inUnitInterval(morph)
            || !std::isfinite(drive) || drive < 0.0 || drive > 4.0
            || !std::isfinite(scale) || scale < 0.0 || scale > 8.0)
        {
            return false;
        }

        auto driven = input;
        if (drive > 0.0)
        {
            const auto gain = 1.0 + 3.0 * drive;
            const auto normalization = std::tanh(gain);
            const auto drivenInput = input * gain;
            if (!std::isfinite(gain) || !std::isfinite(normalization)
                || normalization == 0.0 || !std::isfinite(drivenInput))
            {
                return false;
            }
            driven = std::tanh(drivenInput) / normalization;
        }

        const auto g = std::tan(std::numbers::pi * cutoff / sampleRate_);
        const auto damping = 2.0 - 1.9 * resonance;
        const auto denominator = 1.0 + g * (g + damping);
        const auto a1 = 1.0 / denominator;
        const auto a2 = g * a1;
        const auto a3 = g * a2;
        if (!std::isfinite(driven) || !std::isfinite(g)
            || !std::isfinite(damping) || !std::isfinite(denominator)
            || denominator <= 0.0 || !std::isfinite(a1)
            || !std::isfinite(a2) || !std::isfinite(a3))
        {
            return false;
        }

        const auto v3 = driven - integrator2_;
        const auto band = a1 * integrator1_ + a2 * v3;
        const auto low = integrator2_ + a2 * integrator1_ + a3 * v3;
        const auto high = driven - damping * band - low;
        const auto nextIntegrator1 = 2.0 * band - integrator1_;
        const auto nextIntegrator2 = 2.0 * low - integrator2_;

        const auto morphed = morph <= 0.5
            ? low + (band - low) * (2.0 * morph)
            : band + (high - band) * (2.0 * morph - 1.0);
        const auto scaledOutput = morphed * scale;
        if (!std::isfinite(v3) || !std::isfinite(low)
            || !std::isfinite(band) || !std::isfinite(high)
            || !std::isfinite(nextIntegrator1)
            || !std::isfinite(nextIntegrator2)
            || !std::isfinite(morphed) || !std::isfinite(scaledOutput))
        {
            return false;
        }

        integrator1_ = nextIntegrator1;
        integrator2_ = nextIntegrator2;
        output = scaledOutput;
        return true;
    }

    [[nodiscard]] double integrator1() const noexcept { return integrator1_; }
    [[nodiscard]] double integrator2() const noexcept { return integrator2_; }

private:
    [[nodiscard]] static bool inUnitInterval(double value) noexcept
    {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    }

    double sampleRate_ = 0.0;
    double integrator1_ = 0.0;
    double integrator2_ = 0.0;
    bool prepared_ = false;
};
}
