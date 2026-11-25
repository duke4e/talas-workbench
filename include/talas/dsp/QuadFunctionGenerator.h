#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace talas::dsp
{
struct QuadFunctionFrame
{
    std::array<double, 4> values{};
};

class QuadFunctionGenerator
{
public:
    [[nodiscard]] bool prepare(double sampleRate) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
            return false;
        sampleRate_ = sampleRate;
        prepared_ = true;
        reset();
        return true;
    }

    void reset() noexcept { phase_ = 0.0; }

    [[nodiscard]] bool processSample(
        double frequency,
        double phaseOffset,
        double skew,
        double curve,
        double smooth,
        double scale,
        QuadFunctionFrame& output) noexcept
    {
        if (!prepared_ || !std::isfinite(frequency) || frequency < 0.0
            || frequency > sampleRate_ * 0.45
            || !std::isfinite(phaseOffset) || phaseOffset < 0.0
            || phaseOffset > 1.0 || !std::isfinite(skew) || skew < 0.05
            || skew > 0.95 || !std::isfinite(curve) || curve < 0.25
            || curve > 4.0 || !inUnitInterval(smooth)
            || !std::isfinite(scale) || scale < 0.0 || scale > 8.0)
        {
            return false;
        }

        QuadFunctionFrame candidate;
        for (std::size_t channel = 0; channel < candidate.values.size(); ++channel)
        {
            const auto position = wrapPhase(
                phase_ + phaseOffset + 0.25 * static_cast<double>(channel));
            if (!std::isfinite(position))
                return false;
            const auto triangle = position < skew
                ? position / skew
                : (1.0 - position) / (1.0 - skew);
            if (!std::isfinite(triangle))
                return false;
            const auto curved = std::pow(std::clamp(triangle, 0.0, 1.0), curve);
            if (!std::isfinite(curved))
                return false;
            const auto cosine = 0.5 - 0.5 * std::cos(std::numbers::pi * curved);
            if (!std::isfinite(cosine))
                return false;
            candidate.values[channel] =
                (curved + (cosine - curved) * smooth) * scale;
            if (!std::isfinite(candidate.values[channel]))
                return false;
        }

        const auto nextPhase = wrapPhase(phase_ + frequency / sampleRate_);
        if (!std::isfinite(nextPhase))
            return false;
        output = candidate;
        phase_ = nextPhase;
        return true;
    }

    [[nodiscard]] double phase() const noexcept { return phase_; }

private:
    [[nodiscard]] static bool inUnitInterval(double value) noexcept
    {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    }

    [[nodiscard]] static double wrapPhase(double phase) noexcept
    {
        return phase - std::floor(phase);
    }

    double sampleRate_ = 0.0;
    double phase_ = 0.0;
    bool prepared_ = false;
};
}
