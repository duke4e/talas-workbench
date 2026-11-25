#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace talas::dsp
{
struct DynamicBreakpointOscillatorTestAccess;

class DynamicBreakpointOscillator
{
public:
    static constexpr double minimumSampleRate = 8000.0;
    static constexpr double maximumSampleRate = 384000.0;
    static constexpr double minimumFrequency = 0.1;
    static constexpr std::uint32_t minimumPointCount = 4;
    static constexpr std::uint32_t maximumPointCount = 64;

    [[nodiscard]] bool prepare(
        double sampleRate,
        double pointCount,
        double seed) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate < minimumSampleRate
            || sampleRate > maximumSampleRate || !validPointCount(pointCount)
            || !validSeed(seed))
        {
            return false;
        }

        sampleRate_ = sampleRate;
        pointCount_ = static_cast<std::uint32_t>(pointCount);
        seed_ = static_cast<std::uint64_t>(seed);
        prepared_ = true;
        reset();
        return true;
    }

    void reset() noexcept
    {
        randomState_ = seed_;
        currentValue_ = 0.0;
        targetValue_ = 0.0;
        velocity_ = 0.0;
        segmentFrames_ = 0;
        elapsedFrames_ = 0;
        segmentIndex_ = 0;
        previousResetHigh_ = false;
    }

    [[nodiscard]] bool processSample(
        double resetValue,
        double frequency,
        double volatility,
        double correlation,
        double timingJitter,
        double curve,
        double level,
        double& output) noexcept
    {
        if (!prepared_ || !std::isfinite(resetValue)
            || !validFrequency(frequency) || !inUnitInterval(volatility)
            || !inBipolarInterval(correlation)
            || !std::isfinite(timingJitter) || timingJitter < 0.0
            || timingJitter > 0.5 || !inBipolarInterval(curve)
            || !std::isfinite(level) || level < 0.0 || level > 4.0)
        {
            return false;
        }

        auto candidateRandomState = randomState_;
        auto candidateCurrentValue = currentValue_;
        auto candidateTargetValue = targetValue_;
        auto candidateVelocity = velocity_;
        auto candidateSegmentFrames = segmentFrames_;
        auto candidateElapsedFrames = elapsedFrames_;
        auto candidateSegmentIndex = segmentIndex_;
        const auto candidatePreviousResetHigh = resetValue > 0.5;

        if (candidatePreviousResetHigh && !previousResetHigh_)
        {
            candidateRandomState = seed_;
            candidateCurrentValue = 0.0;
            candidateTargetValue = 0.0;
            candidateVelocity = 0.0;
            candidateSegmentFrames = 0;
            candidateElapsedFrames = 0;
            candidateSegmentIndex = 0;
        }

        if (candidateSegmentFrames == 0
            || candidateElapsedFrames >= candidateSegmentFrames)
        {
            if (candidateSegmentIndex
                == std::numeric_limits<std::uint64_t>::max())
            {
                return false;
            }

            candidateCurrentValue = candidateTargetValue;
            const auto motion = nextSignedUnit(candidateRandomState);
            const auto durationVariation = nextSignedUnit(candidateRandomState);
            candidateVelocity = candidateSegmentIndex == 0
                ? motion
                : correlation * candidateVelocity
                    + (1.0 - std::abs(correlation)) * motion;
            candidateVelocity = std::clamp(candidateVelocity, -1.0, 1.0);
            candidateTargetValue = reflectUnit(
                candidateCurrentValue + volatility * candidateVelocity);

            const auto nominalFrames = sampleRate_
                / (frequency * static_cast<double>(pointCount_));
            const auto duration = std::round(
                nominalFrames * (1.0 + timingJitter * durationVariation));
            if (!std::isfinite(nominalFrames) || nominalFrames < 4.0
                || !std::isfinite(duration) || duration < 2.0
                || duration > static_cast<double>(maximumSegmentFrames))
            {
                return false;
            }
            candidateSegmentFrames = static_cast<std::size_t>(duration);
            candidateElapsedFrames = 0;
            ++candidateSegmentIndex;
        }

        const auto denominator = static_cast<double>(
            candidateSegmentFrames - 1);
        const auto phase = static_cast<double>(candidateElapsedFrames)
            / denominator;
        const auto shapedPhase = shapePhase(phase, curve);
        const auto interpolated = candidateCurrentValue
            + (candidateTargetValue - candidateCurrentValue) * shapedPhase;
        const auto candidateOutput = level * interpolated;
        if (!std::isfinite(candidateCurrentValue)
            || !std::isfinite(candidateTargetValue)
            || !std::isfinite(candidateVelocity)
            || candidateCurrentValue < -1.0 || candidateCurrentValue > 1.0
            || candidateTargetValue < -1.0 || candidateTargetValue > 1.0
            || candidateVelocity < -1.0 || candidateVelocity > 1.0
            || !std::isfinite(phase) || phase < 0.0 || phase > 1.0
            || !std::isfinite(shapedPhase) || shapedPhase < 0.0
            || shapedPhase > 1.0 || !std::isfinite(interpolated)
            || !std::isfinite(candidateOutput))
        {
            return false;
        }

        ++candidateElapsedFrames;
        randomState_ = candidateRandomState;
        currentValue_ = candidateCurrentValue;
        targetValue_ = candidateTargetValue;
        velocity_ = candidateVelocity;
        segmentFrames_ = candidateSegmentFrames;
        elapsedFrames_ = candidateElapsedFrames;
        segmentIndex_ = candidateSegmentIndex;
        previousResetHigh_ = candidatePreviousResetHigh;
        output = candidateOutput;
        return true;
    }

    [[nodiscard]] std::uint64_t segmentIndex() const noexcept
    {
        return segmentIndex_;
    }

    [[nodiscard]] std::size_t segmentFrames() const noexcept
    {
        return segmentFrames_;
    }

private:
    friend struct DynamicBreakpointOscillatorTestAccess;

    static constexpr std::size_t maximumSegmentFrames = 2000000;

    [[nodiscard]] bool validFrequency(double value) const noexcept
    {
        return std::isfinite(value) && value >= minimumFrequency
            && value <= sampleRate_
                / (4.0 * static_cast<double>(pointCount_));
    }

    [[nodiscard]] static bool validPointCount(double value) noexcept
    {
        return std::isfinite(value)
            && value >= static_cast<double>(minimumPointCount)
            && value <= static_cast<double>(maximumPointCount)
            && std::floor(value) == value;
    }

    [[nodiscard]] static bool validSeed(double value) noexcept
    {
        return std::isfinite(value) && value >= 1.0
            && value <= 4294967295.0 && std::floor(value) == value;
    }

    [[nodiscard]] static bool inUnitInterval(double value) noexcept
    {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    }

    [[nodiscard]] static bool inBipolarInterval(double value) noexcept
    {
        return std::isfinite(value) && value >= -1.0 && value <= 1.0;
    }

    [[nodiscard]] static double reflectUnit(double value) noexcept
    {
        if (value > 1.0)
            return 2.0 - value;
        if (value < -1.0)
            return -2.0 - value;
        return value;
    }

    [[nodiscard]] static double shapePhase(
        double phase,
        double curve) noexcept
    {
        if (phase == 0.0 || phase == 1.0 || curve == 0.0)
            return phase;
        if (curve > 0.0)
            return std::pow(phase, 1.0 + 4.0 * curve);
        return 1.0 - std::pow(1.0 - phase, 1.0 - 4.0 * curve);
    }

    [[nodiscard]] static double nextSignedUnit(std::uint64_t& state) noexcept
    {
        state += 0x9e3779b97f4a7c15ULL;
        auto value = state;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return static_cast<double>(value >> 11U)
                * (2.0 / 9007199254740992.0)
            - 1.0;
    }

    double sampleRate_ = 0.0;
    std::uint32_t pointCount_ = minimumPointCount;
    std::uint64_t seed_ = 1;
    std::uint64_t randomState_ = 1;
    double currentValue_ = 0.0;
    double targetValue_ = 0.0;
    double velocity_ = 0.0;
    std::size_t segmentFrames_ = 0;
    std::size_t elapsedFrames_ = 0;
    std::uint64_t segmentIndex_ = 0;
    bool previousResetHigh_ = false;
    bool prepared_ = false;
};
}
