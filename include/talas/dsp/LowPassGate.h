#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace talas::dsp
{
struct LowPassGateConfiguration
{
    double sampleRate = 0.0;
    double closedFrequency = 40.0;
    double openFrequency = 18000.0;
    double attackTime = 0.005;
    double releaseTime = 0.08;
    double curve = 2.0;
    double scale = 1.0;
};

class LowPassGate
{
public:
    [[nodiscard]] bool prepare(
        const LowPassGateConfiguration& configuration) noexcept
    {
        if (!validConfiguration(configuration))
            return false;

        auto candidateAttackCoefficient = 0.0;
        auto candidateReleaseCoefficient = 0.0;
        if (!smoothingCoefficient(
                configuration.attackTime,
                configuration.sampleRate,
                candidateAttackCoefficient)
            || !smoothingCoefficient(
                configuration.releaseTime,
                configuration.sampleRate,
                candidateReleaseCoefficient))
        {
            return false;
        }
        const auto candidateFrequencyRatio = configuration.openFrequency
            / configuration.closedFrequency;
        if (!std::isfinite(candidateFrequencyRatio)
            || candidateFrequencyRatio < 1.0)
        {
            return false;
        }

        configuration_ = configuration;
        attackCoefficient_ = candidateAttackCoefficient;
        releaseCoefficient_ = candidateReleaseCoefficient;
        frequencyRatio_ = candidateFrequencyRatio;
        prepared_ = true;
        reset();
        return true;
    }

    void reset() noexcept
    {
        controlState_ = 0.0;
        filterState_ = 0.0;
    }

    [[nodiscard]] bool copyProcessingStateFrom(
        const LowPassGate& source) noexcept
    {
        if (configuration_.sampleRate != source.configuration_.sampleRate
            || configuration_.closedFrequency
                != source.configuration_.closedFrequency
            || configuration_.openFrequency
                != source.configuration_.openFrequency
            || configuration_.attackTime != source.configuration_.attackTime
            || configuration_.releaseTime != source.configuration_.releaseTime
            || configuration_.curve != source.configuration_.curve
            || configuration_.scale != source.configuration_.scale
            || attackCoefficient_ != source.attackCoefficient_
            || releaseCoefficient_ != source.releaseCoefficient_
            || frequencyRatio_ != source.frequencyRatio_
            || prepared_ != source.prepared_)
        {
            return false;
        }
        controlState_ = source.controlState_;
        filterState_ = source.filterState_;
        return true;
    }

    [[nodiscard]] bool processSample(
        double input,
        double control,
        double& output) noexcept
    {
        if (!prepared_ || !std::isfinite(input) || !std::isfinite(control))
            return false;

        const auto target = std::clamp(control, 0.0, 1.0);
        const auto coefficient = target > controlState_
            ? attackCoefficient_
            : releaseCoefficient_;
        auto candidateControlState =
            target + coefficient * (controlState_ - target);
        candidateControlState =
            std::clamp(candidateControlState, 0.0, 1.0);

        const auto cutoff = configuration_.closedFrequency
            * std::pow(frequencyRatio_, candidateControlState);
        const auto normalizedCutoff = cutoff / configuration_.sampleRate;
        const auto filterExponent =
            -2.0 * std::numbers::pi * normalizedCutoff;
        const auto filterCoefficient = 1.0 - std::exp(filterExponent);
        const auto gain = std::pow(
            candidateControlState, configuration_.curve)
            * configuration_.scale;
        const auto inputDifference = input - filterState_;
        const auto filteredDelta = filterCoefficient * inputDifference;
        const auto candidateFilterState = std::fma(
            filterCoefficient, inputDifference, filterState_);
        const auto candidateOutput = candidateFilterState * gain;
        if (!std::isfinite(candidateControlState)
            || !std::isfinite(cutoff)
            || !std::isfinite(normalizedCutoff)
            || !std::isfinite(filterExponent)
            || !std::isfinite(filterCoefficient)
            || filterCoefficient < 0.0 || filterCoefficient > 1.0
            || !std::isfinite(gain)
            || !std::isfinite(inputDifference)
            || !std::isfinite(filteredDelta)
            || !std::isfinite(candidateFilterState)
            || !std::isfinite(candidateOutput))
        {
            return false;
        }

        controlState_ = candidateControlState;
        filterState_ = candidateFilterState;
        output = candidateOutput;
        return true;
    }

    [[nodiscard]] double controlState() const noexcept
    {
        return controlState_;
    }

    [[nodiscard]] double filterState() const noexcept
    {
        return filterState_;
    }

private:
    [[nodiscard]] static bool validConfiguration(
        const LowPassGateConfiguration& configuration) noexcept
    {
        const auto nyquist = configuration.sampleRate * 0.5;
        return std::isfinite(configuration.sampleRate)
            && configuration.sampleRate > 0.0
            && std::isfinite(configuration.closedFrequency)
            && configuration.closedFrequency > 0.0
            && std::isfinite(configuration.openFrequency)
            && configuration.openFrequency >= configuration.closedFrequency
            && configuration.openFrequency < nyquist
            && std::isfinite(configuration.attackTime)
            && configuration.attackTime >= 0.0
            && std::isfinite(configuration.releaseTime)
            && configuration.releaseTime >= 0.0
            && std::isfinite(configuration.curve)
            && configuration.curve >= 0.25
            && configuration.curve <= 8.0
            && std::isfinite(configuration.scale);
    }

    [[nodiscard]] static bool smoothingCoefficient(
        double time,
        double sampleRate,
        double& coefficient) noexcept
    {
        if (time == 0.0)
        {
            coefficient = 0.0;
            return true;
        }

        const auto frames = time * sampleRate;
        if (!std::isfinite(frames) || frames <= 0.0)
            return false;
        const auto reciprocalFrames = 1.0 / frames;
        if (!std::isfinite(reciprocalFrames))
            return false;
        const auto exponent = -reciprocalFrames;
        if (!std::isfinite(exponent))
            return false;
        const auto candidate = std::exp(exponent);
        if (!std::isfinite(candidate))
            return false;
        coefficient = candidate;
        return true;
    }

    LowPassGateConfiguration configuration_;
    double attackCoefficient_ = 0.0;
    double releaseCoefficient_ = 0.0;
    double frequencyRatio_ = 1.0;
    double controlState_ = 0.0;
    double filterState_ = 0.0;
    bool prepared_ = false;
};
}
