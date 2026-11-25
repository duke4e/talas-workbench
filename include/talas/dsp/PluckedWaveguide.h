#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>
#include <vector>

namespace talas::dsp
{
struct PluckedWaveguideTestAccess;

class PluckedWaveguide
{
public:
    static constexpr double minimumSampleRate = 8000.0;
    static constexpr double maximumSampleRate = 384000.0;
    static constexpr double minimumFrequency = 20.0;

    [[nodiscard]] bool prepare(double sampleRate, double seed) noexcept
    {
        if (!std::isfinite(sampleRate) || sampleRate < minimumSampleRate
            || sampleRate > maximumSampleRate || !validSeed(seed))
        {
            return false;
        }

        const auto requiredSize = std::ceil(sampleRate / minimumFrequency) + 4.0;
        if (!std::isfinite(requiredSize) || requiredSize < 5.0
            || requiredSize > static_cast<double>(maximumDelayLineSize))
        {
            return false;
        }

        std::vector<double> stringCandidate;
        std::vector<double> excitationCandidate;
        std::vector<std::uint64_t> excitationGenerationCandidate;
        try
        {
            const auto size = static_cast<std::size_t>(requiredSize);
            stringCandidate.assign(size, 0.0);
            excitationCandidate.assign(size, 0.0);
            excitationGenerationCandidate.assign(size, 0);
        }
        catch (...)
        {
            return false;
        }

        stringDelay_ = std::move(stringCandidate);
        excitationHistory_ = std::move(excitationCandidate);
        excitationHistoryGeneration_ = std::move(excitationGenerationCandidate);
        sampleRate_ = sampleRate;
        seed_ = static_cast<std::uint64_t>(seed);
        prepared_ = true;
        reset();
        return true;
    }

    // Seeds a prepared waveguide again and starts it afresh, without
    // allocating: a voice reads its Seed as it starts.
    [[nodiscard]] bool reseed(double seed) noexcept
    {
        if (!prepared_ || !validSeed(seed))
            return false;
        seed_ = static_cast<std::uint64_t>(seed);
        reset();
        return true;
    }

    [[nodiscard]] bool copyProcessingStateFrom(
        const PluckedWaveguide& source) noexcept
    {
        if (sampleRate_ != source.sampleRate_ || seed_ != source.seed_
            || prepared_ != source.prepared_
            || stringDelay_.size() != source.stringDelay_.size()
            || excitationHistory_.size()
                != source.excitationHistory_.size()
            || excitationHistoryGeneration_.size()
                != source.excitationHistoryGeneration_.size())
        {
            return false;
        }

        std::ranges::copy(source.stringDelay_, stringDelay_.begin());
        std::ranges::copy(
            source.excitationHistory_, excitationHistory_.begin());
        std::ranges::copy(
            source.excitationHistoryGeneration_,
            excitationHistoryGeneration_.begin());
        stringWriteIndex_ = source.stringWriteIndex_;
        excitationWriteIndex_ = source.excitationWriteIndex_;
        excitationFramesRemaining_ = source.excitationFramesRemaining_;
        excitationPeriod_ = source.excitationPeriod_;
        pluckIndex_ = source.pluckIndex_;
        randomState_ = source.randomState_;
        excitationGeneration_ = source.excitationGeneration_;
        excitationFilterState_ = source.excitationFilterState_;
        feedbackDampingState_ = source.feedbackDampingState_;
        stiffnessState_ = source.stiffnessState_;
        previousTriggerHigh_ = source.previousTriggerHigh_;
        return true;
    }

    void reset() noexcept
    {
        std::ranges::fill(stringDelay_, 0.0);
        std::ranges::fill(excitationHistory_, 0.0);
        std::ranges::fill(excitationHistoryGeneration_, 0);
        stringWriteIndex_ = 0;
        excitationWriteIndex_ = 0;
        excitationFramesRemaining_ = 0;
        excitationPeriod_ = 0;
        excitationGeneration_ = 0;
        pluckIndex_ = 0;
        randomState_ = seed_;
        excitationFilterState_ = 0.0;
        feedbackDampingState_ = 0.0;
        stiffnessState_ = 0.0;
        previousTriggerHigh_ = false;
    }

    [[nodiscard]] bool processSample(
        double trigger,
        double frequency,
        double decayTime,
        double brightness,
        double pickPosition,
        double stiffness,
        double level,
        double& output) noexcept
    {
        if (!prepared_ || !std::isfinite(trigger)
            || !validFrequency(frequency) || !std::isfinite(decayTime)
            || decayTime < 0.02 || decayTime > 20.0
            || !inUnitInterval(brightness)
            || !std::isfinite(pickPosition) || pickPosition < 0.02
            || pickPosition > 0.98 || !inUnitInterval(stiffness)
            || !std::isfinite(level) || level < 0.0 || level > 4.0)
        {
            return false;
        }

        const auto delaySamples = sampleRate_ / frequency;
        const auto periodDouble = std::ceil(delaySamples);
        if (!std::isfinite(delaySamples) || delaySamples < 2.5
            || delaySamples > static_cast<double>(stringDelay_.size() - 4)
            || !std::isfinite(periodDouble) || periodDouble < 3.0
            || periodDouble > static_cast<double>(stringDelay_.size() - 4))
        {
            return false;
        }
        const auto period = static_cast<std::size_t>(periodDouble);

        auto candidatePreviousTriggerHigh = trigger > 0.5;
        auto candidatePluckIndex = pluckIndex_;
        auto candidateRandomState = randomState_;
        auto candidateExcitationGeneration = excitationGeneration_;
        auto candidateExcitationWriteIndex = excitationWriteIndex_;
        auto candidateExcitationFramesRemaining = excitationFramesRemaining_;
        auto candidateExcitationPeriod = excitationPeriod_;
        auto candidateExcitationFilterState = excitationFilterState_;

        const auto risingEdge = candidatePreviousTriggerHigh
            && !previousTriggerHigh_;
        if (risingEdge)
        {
            if (candidatePluckIndex == std::numeric_limits<std::uint64_t>::max()
                || candidateExcitationGeneration
                    == std::numeric_limits<std::uint64_t>::max())
            {
                return false;
            }
            ++candidatePluckIndex;
            ++candidateExcitationGeneration;
            candidateRandomState = seed_
                ^ (candidatePluckIndex * 0x9e3779b97f4a7c15ULL);
            candidateExcitationWriteIndex = 0;
            candidateExcitationFramesRemaining = period;
            candidateExcitationPeriod = period;
            candidateExcitationFilterState = 0.0;
        }

        auto excitation = 0.0;
        auto writeExcitationHistory = false;
        auto excitationHistoryValue = 0.0;
        auto excitationHistoryIndex = candidateExcitationWriteIndex;
        if (candidateExcitationFramesRemaining > 0)
        {
            auto nextRandomState = candidateRandomState;
            const auto noise = nextSignedUnit(nextRandomState);
            auto filteredNoise = noise;
            if (brightness < 1.0)
            {
                const auto cutoff = logarithmicCutoff(brightness);
                const auto alpha = 1.0
                    - std::exp(-twoPi * cutoff / sampleRate_);
                candidateExcitationFilterState += alpha
                    * (noise - candidateExcitationFilterState);
                filteredNoise = candidateExcitationFilterState;
            }
            else
            {
                candidateExcitationFilterState = noise;
            }

            const auto pickDelay = std::clamp<std::size_t>(
                static_cast<std::size_t>(std::llround(
                    pickPosition * static_cast<double>(candidateExcitationPeriod))),
                1,
                candidateExcitationPeriod - 1);
            const auto historyIndex = excitationHistoryIndex >= pickDelay
                ? excitationHistoryIndex - pickDelay
                : excitationHistory_.size() - (pickDelay - excitationHistoryIndex);
            const auto history = excitationHistoryGeneration_[historyIndex]
                    == candidateExcitationGeneration
                ? excitationHistory_[historyIndex]
                : 0.0;
            excitation = filteredNoise - history;
            writeExcitationHistory = true;
            excitationHistoryValue = filteredNoise;
            candidateRandomState = nextRandomState;
            candidateExcitationWriteIndex = candidateExcitationWriteIndex + 1
                    == excitationHistory_.size()
                ? 0
                : candidateExcitationWriteIndex + 1;
            --candidateExcitationFramesRemaining;
        }

        const auto delayed = readDelay(stringDelay_, stringWriteIndex_, delaySamples);
        auto dampingState = delayed;
        if (brightness < 1.0)
        {
            const auto cutoff = logarithmicCutoff(brightness);
            const auto alpha = 1.0
                - std::exp(-twoPi * cutoff / sampleRate_);
            dampingState = feedbackDampingState_
                + alpha * (delayed - feedbackDampingState_);
        }

        auto dispersed = dampingState;
        auto candidateStiffnessState = stiffnessState_;
        if (stiffness == 0.0)
        {
            candidateStiffnessState = 0.0;
        }
        else
        {
            const auto coefficient = 0.85 * stiffness;
            const auto next = coefficient * dispersed + candidateStiffnessState;
            candidateStiffnessState = dispersed - coefficient * next;
            dispersed = next;
        }

        const auto feedback = std::pow(
            0.001, 1.0 / (decayTime * frequency));
        const auto writeValue = excitation + feedback * dispersed;
        const auto candidateOutput = level * dispersed;
        if (!std::isfinite(excitation)
            || !std::isfinite(candidateExcitationFilterState)
            || !std::isfinite(delayed) || !std::isfinite(dampingState)
            || !std::isfinite(dispersed) || !std::isfinite(feedback)
            || feedback < 0.0 || feedback >= 1.0
            || !std::isfinite(writeValue) || !std::isfinite(candidateOutput)
            || !std::isfinite(candidateStiffnessState))
        {
            return false;
        }

        stringDelay_[stringWriteIndex_] = writeValue;
        stringWriteIndex_ = stringWriteIndex_ + 1 == stringDelay_.size()
            ? 0
            : stringWriteIndex_ + 1;
        if (writeExcitationHistory)
        {
            excitationHistory_[excitationHistoryIndex] = excitationHistoryValue;
            excitationHistoryGeneration_[excitationHistoryIndex]
                = candidateExcitationGeneration;
        }
        excitationWriteIndex_ = candidateExcitationWriteIndex;
        excitationFramesRemaining_ = candidateExcitationFramesRemaining;
        excitationPeriod_ = candidateExcitationPeriod;
        excitationGeneration_ = candidateExcitationGeneration;
        pluckIndex_ = candidatePluckIndex;
        randomState_ = candidateRandomState;
        excitationFilterState_ = candidateExcitationFilterState;
        feedbackDampingState_ = dampingState;
        stiffnessState_ = candidateStiffnessState;
        previousTriggerHigh_ = candidatePreviousTriggerHigh;
        output = candidateOutput;
        return true;
    }

    [[nodiscard]] std::size_t delayLineSize() const noexcept
    {
        return stringDelay_.size();
    }

    [[nodiscard]] std::uint64_t pluckIndex() const noexcept
    {
        return pluckIndex_;
    }

    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

private:
    friend struct PluckedWaveguideTestAccess;

    static constexpr std::size_t maximumDelayLineSize = 19204;
    static constexpr double twoPi = 6.283185307179586476925286766559;

    [[nodiscard]] bool validFrequency(double frequency) const noexcept
    {
        return std::isfinite(frequency) && frequency >= minimumFrequency
            && frequency <= sampleRate_ * 0.4;
    }

    [[nodiscard]] static bool validSeed(double seed) noexcept
    {
        return std::isfinite(seed) && seed >= 1.0
            && seed <= 4294967295.0 && std::floor(seed) == seed;
    }

    [[nodiscard]] static bool inUnitInterval(double value) noexcept
    {
        return std::isfinite(value) && value >= 0.0 && value <= 1.0;
    }

    [[nodiscard]] double logarithmicCutoff(double brightness) const noexcept
    {
        constexpr double minimumCutoff = 40.0;
        const auto maximumCutoff = sampleRate_ * 0.4;
        return minimumCutoff
            * std::pow(maximumCutoff / minimumCutoff, brightness);
    }

    [[nodiscard]] static double readDelay(
        const std::vector<double>& delay,
        std::size_t writeIndex,
        double delaySamples) noexcept
    {
        auto readPosition = static_cast<double>(writeIndex) - delaySamples;
        if (readPosition < 0.0)
            readPosition += static_cast<double>(delay.size());
        const auto left = static_cast<std::size_t>(std::floor(readPosition));
        const auto right = left + 1 == delay.size() ? 0 : left + 1;
        const auto fraction = readPosition - static_cast<double>(left);
        return delay[left] + (delay[right] - delay[left]) * fraction;
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

    std::vector<double> stringDelay_;
    std::vector<double> excitationHistory_;
    std::vector<std::uint64_t> excitationHistoryGeneration_;
    double sampleRate_ = 0.0;
    std::size_t stringWriteIndex_ = 0;
    std::size_t excitationWriteIndex_ = 0;
    std::size_t excitationFramesRemaining_ = 0;
    std::size_t excitationPeriod_ = 0;
    std::uint64_t seed_ = 1;
    std::uint64_t pluckIndex_ = 0;
    std::uint64_t randomState_ = 1;
    std::uint64_t excitationGeneration_ = 0;
    double excitationFilterState_ = 0.0;
    double feedbackDampingState_ = 0.0;
    double stiffnessState_ = 0.0;
    bool previousTriggerHigh_ = false;
    bool prepared_ = false;
};
}
