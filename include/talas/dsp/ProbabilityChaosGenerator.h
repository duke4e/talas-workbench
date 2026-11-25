#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace talas::dsp
{
struct ProbabilityChaosFrame
{
    double value = 0.0;
    double gate = 0.0;
};

class ProbabilityChaosGenerator
{
public:
    [[nodiscard]] bool prepare(double sampleRate, double seed) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate <= 0.0
            || !validSeed(seed))
        {
            return false;
        }
        sampleRate_ = sampleRate;
        seed_ = static_cast<std::uint64_t>(seed);
        prepared_ = true;
        reset();
        return true;
    }

    void reset() noexcept
    {
        phase_ = 0.0;
        randomState_ = seed_;
        const auto initial = nextUnit(randomState_);
        logisticState_ = 0.05 + 0.9 * initial;
        target_ = logisticState_ * 2.0 - 1.0;
        current_ = target_;
    }

    [[nodiscard]] bool processSample(
        double frequency,
        double probability,
        double chaos,
        double slew,
        double scale,
        ProbabilityChaosFrame& output) noexcept
    {
        if (!prepared_ || !std::isfinite(frequency) || frequency < 0.0
            || frequency > sampleRate_ * 0.45
            || !inUnitInterval(probability) || !inUnitInterval(chaos)
            || !std::isfinite(slew) || slew < 0.0 || slew > 10.0
            || !std::isfinite(scale) || scale < 0.0 || scale > 8.0)
        {
            return false;
        }

        auto candidatePhase = phase_;
        auto candidateRandomState = randomState_;
        auto candidateLogisticState = logisticState_;
        auto candidateTarget = target_;
        auto candidateCurrent = current_;
        auto candidateGate = 0.0;
        candidatePhase += frequency / sampleRate_;
        if (candidatePhase >= 1.0)
        {
            candidatePhase -= 1.0;
            const auto rate = 3.57 + 0.42 * chaos;
            candidateLogisticState = rate * candidateLogisticState
                * (1.0 - candidateLogisticState);
            if (!std::isfinite(candidateLogisticState)
                || candidateLogisticState <= 0.0
                || candidateLogisticState >= 1.0)
            {
                return false;
            }
            if (nextUnit(candidateRandomState) < probability)
            {
                candidateTarget = candidateLogisticState * 2.0 - 1.0;
                candidateGate = 1.0;
            }
        }

        if (slew == 0.0)
        {
            candidateCurrent = candidateTarget;
        }
        else
        {
            const auto alpha = -std::expm1(-1.0 / (slew * sampleRate_));
            candidateCurrent +=
                (candidateTarget - candidateCurrent) * alpha;
        }
        const auto candidateValue = candidateCurrent * scale;
        if (!std::isfinite(candidatePhase) || candidatePhase < 0.0
            || candidatePhase >= 1.0
            || !std::isfinite(candidateTarget)
            || !std::isfinite(candidateCurrent)
            || !std::isfinite(candidateValue))
        {
            return false;
        }

        phase_ = candidatePhase;
        randomState_ = candidateRandomState;
        logisticState_ = candidateLogisticState;
        target_ = candidateTarget;
        current_ = candidateCurrent;
        output.value = candidateValue;
        output.gate = candidateGate;
        return true;
    }

    [[nodiscard]] double phase() const noexcept { return phase_; }

private:
    [[nodiscard]] static bool validSeed(double seed) noexcept
    {
        return std::isfinite(seed) && seed >= 1.0
            && seed <= 4294967295.0 && std::floor(seed) == seed;
    }

    [[nodiscard]] static bool inUnitInterval(double value) noexcept
    {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    }

    [[nodiscard]] static double nextUnit(std::uint64_t& state) noexcept
    {
        state += 0x9e3779b97f4a7c15ULL;
        auto value = state;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return static_cast<double>(value >> 11U)
            * (1.0 / 9007199254740992.0);
    }

    double sampleRate_ = 0.0;
    double phase_ = 0.0;
    double logisticState_ = 0.5;
    double target_ = 0.0;
    double current_ = 0.0;
    std::uint64_t seed_ = 1;
    std::uint64_t randomState_ = 1;
    bool prepared_ = false;
};
}
