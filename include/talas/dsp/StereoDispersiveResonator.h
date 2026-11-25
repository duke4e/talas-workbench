#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

namespace talas::dsp
{
struct StereoDispersiveResonatorFrame
{
    double left = 0.0;
    double right = 0.0;
};

// Two detuned comb loops, each a fractional delay, an optional one-pole
// low-pass (Damping) and two optional first-order all-passes (Dispersion).
//
// Damping is the loop low-pass's position, not an amount of damping: 1
// leaves the loop open (brightest, longest ring) and lower values pull its
// cut-off down to 20 Hz * (0.45 * sampleRate / 20 Hz) ^ damping, so 0, a
// 20 Hz cut-off, mutes the ring within a few periods.
//
// Each loop's delay is shortened by the phase delay its low-pass and
// all-passes add at that loop's frequency, so the fundamental rings at
// Frequency (Detune apart) whatever Damping and Dispersion are; Dispersion
// still moves the upper partials. The delay keeps at least one sample, so
// near the top of the range with strong Dispersion (above about 0.36 times
// the sample rate) the fundamental falls flat.
//
// A comb loop also resonates at 0 Hz, and the loop low-pass passes 0 Hz
// unchanged, so any DC or sub-audio content in the input built up into a
// slowly decaying offset. The excitation is therefore DC-blocked first: a
// one-pole high-pass at 20 Hz, the lowest Frequency, with unity gain at
// Nyquist. The dry signal is not filtered.
class StereoDispersiveResonator
{
public:
    static constexpr double minimumSampleRate = 8000.0;
    static constexpr double maximumSampleRate = 384000.0;
    static constexpr double minimumFrequency = 20.0;
    static constexpr double dcBlockerFrequency = 20.0;
    static constexpr double minimumTunedDelay = 1.0;

    [[nodiscard]] bool prepare(double sampleRate) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate < minimumSampleRate
            || sampleRate > maximumSampleRate)
        {
            return false;
        }

        const auto requiredSize = std::ceil(sampleRate / minimumFrequency) + 4.0;
        if (!std::isfinite(requiredSize) || requiredSize < 5.0
            || requiredSize > static_cast<double>(maximumDelayLineSize))
        {
            return false;
        }

        std::array<Branch, 2> candidates;
        try
        {
            for (auto& candidate : candidates)
                candidate.delayLine.assign(
                    static_cast<std::size_t>(requiredSize), 0.0);
        }
        catch (...)
        {
            return false;
        }

        branches_ = std::move(candidates);
        sampleRate_ = sampleRate;
        dcBlockerPole_ = std::exp(
            -2.0 * std::numbers::pi * dcBlockerFrequency / sampleRate);
        dcBlockerGain_ = 0.5 * (1.0 + dcBlockerPole_);
        prepared_ = true;
        reset();
        return true;
    }

    void reset() noexcept
    {
        for (auto& branch : branches_)
        {
            std::ranges::fill(branch.delayLine, 0.0);
            branch.writeIndex = 0;
            branch.dampingState = 0.0;
            branch.allpassState = {};
        }
        dcBlockerInput_ = 0.0;
        dcBlockerOutput_ = 0.0;
    }

    [[nodiscard]] bool copyProcessingStateFrom(
        const StereoDispersiveResonator& source) noexcept
    {
        if (sampleRate_ != source.sampleRate_
            || prepared_ != source.prepared_)
        {
            return false;
        }
        for (std::size_t index = 0; index < branches_.size(); ++index)
        {
            auto& destination = branches_[index];
            const auto& sourceBranch = source.branches_[index];
            if (destination.delayLine.size()
                != sourceBranch.delayLine.size())
            {
                return false;
            }
        }
        for (std::size_t index = 0; index < branches_.size(); ++index)
        {
            auto& destination = branches_[index];
            const auto& sourceBranch = source.branches_[index];
            std::ranges::copy(
                sourceBranch.delayLine, destination.delayLine.begin());
            destination.writeIndex = sourceBranch.writeIndex;
            destination.dampingState = sourceBranch.dampingState;
            destination.allpassState = sourceBranch.allpassState;
        }
        dcBlockerInput_ = source.dcBlockerInput_;
        dcBlockerOutput_ = source.dcBlockerOutput_;
        return true;
    }

    [[nodiscard]] bool processSample(
        double input,
        double frequency,
        double decayTime,
        double detune,
        double damping,
        double dispersion,
        double width,
        double mix,
        double inputGain,
        StereoDispersiveResonatorFrame& output) noexcept
    {
        if (!prepared_ || !std::isfinite(input)
            || !std::isfinite(frequency) || !std::isfinite(decayTime)
            || !std::isfinite(detune) || !inUnitInterval(damping)
            || !inUnitInterval(dispersion) || !inUnitInterval(width)
            || !inUnitInterval(mix) || !std::isfinite(inputGain)
            || frequency < minimumFrequency || decayTime < 0.01
            || decayTime > 20.0 || detune < 0.0 || detune > 100.0
            || inputGain < 0.0 || inputGain > 8.0)
        {
            return false;
        }

        const auto detuneRatio = std::exp2(detune / 2400.0);
        const std::array branchFrequencies{
            frequency / detuneRatio,
            frequency * detuneRatio,
        };
        const auto maximumFrequency = sampleRate_ * 0.45;
        if (!std::isfinite(detuneRatio)
            || !validBranchFrequency(branchFrequencies[0], maximumFrequency)
            || !validBranchFrequency(branchFrequencies[1], maximumFrequency))
        {
            return false;
        }

        const auto excitation = dcBlockerGain_ * (input - dcBlockerInput_)
            + dcBlockerPole_ * dcBlockerOutput_;
        if (!std::isfinite(excitation))
            return false;

        std::array<BranchResult, 2> results;
        for (std::size_t index = 0; index < branches_.size(); ++index)
        {
            if (!processBranch(
                    branches_[index], excitation, branchFrequencies[index],
                    decayTime, damping, dispersion, inputGain, results[index]))
            {
                return false;
            }
        }

        const auto wetMid = (results[0].wet + results[1].wet) * 0.5;
        const auto wetSide = (results[0].wet - results[1].wet) * 0.5 * width;
        const auto wetLeft = wetMid + wetSide;
        const auto wetRight = wetMid - wetSide;
        const auto dryMix = 1.0 - mix;
        const StereoDispersiveResonatorFrame candidate{
            input * dryMix + wetLeft * mix,
            input * dryMix + wetRight * mix,
        };
        if (!std::isfinite(candidate.left) || !std::isfinite(candidate.right))
            return false;

        for (std::size_t index = 0; index < branches_.size(); ++index)
        {
            auto& branch = branches_[index];
            const auto& result = results[index];
            branch.delayLine[branch.writeIndex] = result.writeValue;
            branch.writeIndex = branch.writeIndex + 1 == branch.delayLine.size()
                ? 0
                : branch.writeIndex + 1;
            branch.dampingState = result.dampingState;
            branch.allpassState = result.allpassState;
        }
        dcBlockerInput_ = input;
        dcBlockerOutput_ = excitation;
        output = candidate;
        return true;
    }

    [[nodiscard]] std::size_t delayLineSize() const noexcept
    {
        return branches_.front().delayLine.size();
    }

private:
    static constexpr std::size_t maximumDelayLineSize = 19204;

    struct Branch
    {
        std::vector<double> delayLine;
        std::size_t writeIndex = 0;
        double dampingState = 0.0;
        std::array<double, 2> allpassState{};
    };

    struct BranchResult
    {
        double wet = 0.0;
        double writeValue = 0.0;
        double dampingState = 0.0;
        std::array<double, 2> allpassState{};
    };

    [[nodiscard]] static bool inUnitInterval(double value) noexcept
    {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    }

    [[nodiscard]] static bool validBranchFrequency(
        double frequency,
        double maximumFrequency) noexcept
    {
        return std::isfinite(frequency) && frequency >= minimumFrequency
            && frequency <= maximumFrequency;
    }

    [[nodiscard]] static double readDelay(
        const Branch& branch,
        double delaySamples) noexcept
    {
        auto readPosition = static_cast<double>(branch.writeIndex) - delaySamples;
        if (readPosition < 0.0)
            readPosition += static_cast<double>(branch.delayLine.size());
        const auto left = static_cast<std::size_t>(std::floor(readPosition));
        const auto right = left + 1 == branch.delayLine.size() ? 0 : left + 1;
        const auto fraction = readPosition - static_cast<double>(left);
        return branch.delayLine[left]
            + (branch.delayLine[right] - branch.delayLine[left]) * fraction;
    }

    // The phase lag, in radians at angular frequency omega, of the one-pole
    // low-pass y += alpha * (x - y), whose pole is 1 - alpha.
    [[nodiscard]] static double lowPassLag(double alpha, double omega) noexcept
    {
        const auto pole = 1.0 - alpha;
        return std::atan2(pole * std::sin(omega), 1.0 - pole * std::cos(omega));
    }

    // The phase lag of the all-pass (c + z^-1) / (1 + c z^-1).
    [[nodiscard]] static double allpassLag(
        double coefficient,
        double omega) noexcept
    {
        const auto sine = std::sin(omega);
        const auto cosine = std::cos(omega);
        return std::atan2(sine, coefficient + cosine)
            - std::atan2(coefficient * sine, 1.0 + coefficient * cosine);
    }

    [[nodiscard]] bool processBranch(
        const Branch& branch,
        double input,
        double frequency,
        double decayTime,
        double damping,
        double dispersion,
        double inputGain,
        BranchResult& result) const noexcept
    {
        const auto period = sampleRate_ / frequency;
        if (!std::isfinite(period) || period < 2.0
            || period > static_cast<double>(branch.delayLine.size() - 4))
        {
            return false;
        }

        const auto damped = damping < 1.0;
        auto alpha = 1.0;
        if (damped)
        {
            const auto maximumCutoff = sampleRate_ * 0.45;
            const auto cutoff = minimumFrequency
                * std::pow(maximumCutoff / minimumFrequency, damping);
            alpha = 1.0
                - std::exp(-2.0 * std::numbers::pi * cutoff / sampleRate_);
        }
        const auto dispersed = dispersion != 0.0;
        const auto coefficient = 1.0 - 0.75 * dispersion;

        // The loop's filters delay the fundamental too; the delay line
        // gives that time back so the loop's period stays 1 / frequency.
        auto delaySamples = period;
        if (damped || dispersed)
        {
            const auto omega = 2.0 * std::numbers::pi / period;
            auto lag = damped ? lowPassLag(alpha, omega) : 0.0;
            if (dispersed)
                lag += 2.0 * allpassLag(coefficient, omega);
            delaySamples = std::max(period - lag / omega, minimumTunedDelay);
            if (!std::isfinite(lag) || !std::isfinite(delaySamples))
                return false;
        }
        const auto delayed = readDelay(branch, delaySamples);

        auto dampingState = delayed;
        if (damped)
        {
            dampingState = branch.dampingState
                + alpha * (delayed - branch.dampingState);
        }

        auto dispersedValue = dampingState;
        auto allpassState = branch.allpassState;
        if (!dispersed)
        {
            allpassState = {};
        }
        else
        {
            for (auto& state : allpassState)
            {
                const auto next = coefficient * dispersedValue + state;
                state = dispersedValue - coefficient * next;
                dispersedValue = next;
            }
        }

        const auto feedback = std::pow(
            0.001, 1.0 / (decayTime * frequency));
        const auto writeValue = input * inputGain + feedback * dispersedValue;
        if (!std::isfinite(delayed) || !std::isfinite(dampingState)
            || !std::isfinite(dispersedValue) || !std::isfinite(feedback)
            || feedback < 0.0 || feedback >= 1.0
            || !std::isfinite(writeValue)
            || !std::ranges::all_of(allpassState, [](double value) {
                   return std::isfinite(value);
               }))
        {
            return false;
        }

        result = {
            .wet = dispersedValue,
            .writeValue = writeValue,
            .dampingState = dampingState,
            .allpassState = allpassState,
        };
        return true;
    }

    std::array<Branch, 2> branches_;
    double sampleRate_ = 0.0;
    double dcBlockerPole_ = 0.0;
    double dcBlockerGain_ = 0.0;
    double dcBlockerInput_ = 0.0;
    double dcBlockerOutput_ = 0.0;
    bool prepared_ = false;
};
}
