#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace talas::runtime::checked_frequency_tracker
{
constexpr std::size_t maximumSupportedLag = 65536;
constexpr double silenceEnergyThreshold = 1.0e-18;
constexpr double neutralEmphasisTolerance = 1.0e-12;
// The tracker estimates 750 times a second (every 64 frames at 48 kHz), on
// frames counted from its start, so every partition of an Input tracks
// alike.
constexpr double estimatesPerSecond = 750.0;

[[nodiscard]] inline std::size_t estimateHopFrames(double sampleRate) noexcept
{
    const auto frames = std::floor(sampleRate / estimatesPerSecond);
    return std::isfinite(frames) && frames > 1.0
        ? static_cast<std::size_t>(frames) : 1U;
}
// The amplitude envelope on the right channel
// follows a peak at once and releases it over 50 ms, each sample keeping
// this share of the last.
[[nodiscard]] inline double amplitudeRelease(double sampleRate) noexcept
{
    const auto share = std::exp(-1.0 / (0.05 * sampleRate));
    return std::isfinite(share) && share >= 0.0 && share < 1.0 ? share : 0.0;
}
// The first lag whose normalized difference dips below this is the period;
// a later, deeper dip is a multiple of it (an octave below).
constexpr double periodDipThreshold = 0.1;
// A window whose quarters differ in energy by more than this factor holds
// an onset or an ending, which would bend the estimate: it gives none.
constexpr double steadyEnergyRatio = 0.5;

enum class Error
{
    None,
    SampleRateNonFiniteOrNonPositive,
    MinFrequencyNonFiniteOrNonPositive,
    MaxFrequencyNonFiniteOrNonPositive,
    MaxFrequencyAtOrAboveNyquist,
    FrequencyRangeInvalid,
    ConfidenceNonFiniteOrOutOfRange,
    ScaleNonFiniteOrNegative,
    LagIntermediateNonFinite,
    MaximumLagExceeded,
    HistoryCapacityExceeded,
    HistoryViewInvalid,
    ScratchCapacityInvalid,
    AudioInNonFinite,
    EmphasisNonFiniteOrNegative,
    PreviousInputNonFinite,
    PreEmphasisIntermediateNonFinite,
    HistorySampleNonFinite,
    EnergyIntermediateNonFinite,
    DifferenceIntermediateNonFinite,
    NormalizationIntermediateNonFinite,
    ConfidenceIntermediateNonFinite,
    RefinementIntermediateNonFinite,
    FrequencyIntermediateNonFinite,
    ScaledOutputNonFinite,
};

struct Controls
{
    double minFrequency = 0.0;
    double maxFrequency = 0.0;
    double confidence = 0.0;
    double scale = 1.0;
};

// One estimate: a frequency (0 when there is none) and how confident the
// tracker is of it, from 0 to 1.
struct Estimate
{
    double frequency = 0.0;
    double confidence = 0.0;
};

struct LagWindow
{
    std::size_t minLag = 0;
    std::size_t maxLag = 0;
    std::size_t requiredSamples = 0;
};

struct HistoryView
{
    std::span<const double> storage;
    std::size_t oldestIndex = 0;
    std::size_t count = 0;

    [[nodiscard]] bool valid() const noexcept
    {
        if (count > storage.size())
            return false;
        if (storage.empty())
            return oldestIndex == 0 && count == 0;
        return oldestIndex < storage.size();
    }

    [[nodiscard]] double sampleAt(std::size_t index) const noexcept
    {
        return storage[(oldestIndex + index) % storage.size()];
    }
};

// The lowest frequency whose longest period, floor(sampleRate / frequency),
// fits in maximumLag frames; 0 for no lag.
[[nodiscard]] inline double lowestFrequencyForLag(
    double sampleRate,
    std::size_t maximumLag) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || maximumLag == 0)
        return 0.0;
    auto frequency = sampleRate / static_cast<double>(maximumLag);
    while (std::isfinite(frequency)
        && std::floor(sampleRate / frequency) > static_cast<double>(maximumLag))
    {
        frequency = std::nextafter(frequency, std::numeric_limits<double>::infinity());
    }
    return frequency;
}

// Owner decision 2026-10-12, taken as recommended on 2026-10-13: a bound
// that passes what it is bound by saturates against it, as a live value
// saturates at its range. A MinFrequency whose period the history cannot
// hold rises to the lowest it holds; a MaxFrequency at or below MinFrequency
// stays a 24-bit step of the sample rate above it, and one at Nyquist a step
// below it. Values that are not finite or not positive are left to be
// refused; bounds in order and within the history are left as they are.
[[nodiscard]] inline Controls saturated(
    Controls controls,
    double sampleRate,
    std::size_t historyCapacity,
    bool enabled = true) noexcept
{
    if (!enabled || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(controls.minFrequency) || controls.minFrequency <= 0.0
        || !std::isfinite(controls.maxFrequency) || controls.maxFrequency <= 0.0)
    {
        return controls;
    }
    const auto step = sampleRate / 16777216.0;
    const auto lag = std::min(maximumSupportedLag, historyCapacity / 2U);
    if (lag != 0)
        controls.minFrequency = std::max(controls.minFrequency, lowestFrequencyForLag(sampleRate, lag));
    if (controls.maxFrequency <= controls.minFrequency)
        controls.maxFrequency = controls.minFrequency + step;
    if (controls.maxFrequency >= sampleRate * 0.5)
        controls.maxFrequency = sampleRate * 0.5 - step;
    return controls;
}

[[nodiscard]] inline Error deriveLagWindow(
    const Controls& controls,
    double sampleRate,
    std::size_t historyCapacity,
    LagWindow& destination) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return Error::SampleRateNonFiniteOrNonPositive;
    if (!std::isfinite(controls.minFrequency)
        || controls.minFrequency <= 0.0)
    {
        return Error::MinFrequencyNonFiniteOrNonPositive;
    }
    if (!std::isfinite(controls.maxFrequency)
        || controls.maxFrequency <= 0.0)
    {
        return Error::MaxFrequencyNonFiniteOrNonPositive;
    }
    const auto nyquist = sampleRate * 0.5;
    if (!std::isfinite(nyquist)
        || controls.maxFrequency >= nyquist)
    {
        return Error::MaxFrequencyAtOrAboveNyquist;
    }
    if (controls.maxFrequency <= controls.minFrequency)
        return Error::FrequencyRangeInvalid;
    if (!std::isfinite(controls.confidence)
        || controls.confidence < 0.0 || controls.confidence > 1.0)
    {
        return Error::ConfidenceNonFiniteOrOutOfRange;
    }
    if (!std::isfinite(controls.scale) || controls.scale < 0.0)
        return Error::ScaleNonFiniteOrNegative;

    const auto minimumLagValue = std::ceil(
        sampleRate / controls.maxFrequency);
    const auto maximumLagValue = std::floor(
        sampleRate / controls.minFrequency);
    if (!std::isfinite(minimumLagValue)
        || !std::isfinite(maximumLagValue)
        || minimumLagValue < 0.0 || maximumLagValue < 0.0)
    {
        return Error::LagIntermediateNonFinite;
    }
    if (maximumLagValue > static_cast<double>(maximumSupportedLag))
        return Error::MaximumLagExceeded;
    const auto minLag = static_cast<std::size_t>(
        std::max(1.0, minimumLagValue));
    const auto maxLag = static_cast<std::size_t>(maximumLagValue);
    if (minLag > maxLag || maxLag == 0)
    {
        destination = {minLag, maxLag, 0};
        return Error::None;
    }
    const auto requiredSamples = maxLag * 2U;
    if (requiredSamples > historyCapacity)
        return Error::HistoryCapacityExceeded;

    destination = {minLag, maxLag, requiredSamples};
    return Error::None;
}

[[nodiscard]] inline Error preEmphasize(
    double sample,
    double emphasis,
    double previousInput,
    bool hasPreviousInput,
    double& destination) noexcept
{
    if (!std::isfinite(sample))
        return Error::AudioInNonFinite;
    if (!std::isfinite(emphasis) || emphasis < 0.0)
        return Error::EmphasisNonFiniteOrNegative;
    if (hasPreviousInput && !std::isfinite(previousInput))
        return Error::PreviousInputNonFinite;

    auto candidate = sample;
    if (std::abs(emphasis) > neutralEmphasisTolerance)
    {
        const auto product = emphasis
            * (hasPreviousInput ? previousInput : 0.0);
        if (!std::isfinite(product))
            return Error::PreEmphasisIntermediateNonFinite;
        candidate = sample - product;
        if (!std::isfinite(candidate))
            return Error::PreEmphasisIntermediateNonFinite;
    }
    destination = candidate;
    return Error::None;
}

[[nodiscard]] inline Error estimateWithConfidence(
    const Controls& controls,
    double sampleRate,
    const HistoryView& history,
    std::span<double> difference,
    std::span<double> normalized,
    Estimate& destination) noexcept
{
    if (!history.valid())
        return Error::HistoryViewInvalid;
    if (difference.size() < 2 || normalized.size() != difference.size())
        return Error::ScratchCapacityInvalid;

    const auto maximumScratchLag = difference.size() - 1U;
    if (maximumScratchLag > maximumSupportedLag)
        return Error::ScratchCapacityInvalid;
    auto window = LagWindow{};
    const auto controlsError = deriveLagWindow(
        controls,
        sampleRate,
        maximumScratchLag * 2U,
        window);
    if (controlsError != Error::None)
        return controlsError;
    if (window.requiredSamples == 0)
    {
        destination = {};
        return Error::None;
    }
    if (history.count < window.requiredSamples)
    {
        destination = {};
        return Error::None;
    }

    const auto analysisSamples = std::min(
        history.count, window.requiredSamples);
    const auto analysisStart = history.count - analysisSamples;
    const auto sampleAt = [&](std::size_t index) noexcept {
        return history.sampleAt(analysisStart + index);
    };

    double energy = 0.0;
    std::array<double, 4> quarterEnergy{};
    for (std::size_t index = 0; index < analysisSamples; ++index)
    {
        const auto sample = sampleAt(index);
        if (!std::isfinite(sample))
            return Error::HistorySampleNonFinite;
        const auto square = sample * sample;
        if (!std::isfinite(square))
            return Error::EnergyIntermediateNonFinite;
        const auto candidateEnergy = energy + square;
        if (!std::isfinite(candidateEnergy))
            return Error::EnergyIntermediateNonFinite;
        energy = candidateEnergy;
        quarterEnergy[index * 4U / analysisSamples] += square;
    }
    if (energy <= silenceEnergyThreshold
        || *std::ranges::min_element(quarterEnergy)
            < steadyEnergyRatio * *std::ranges::max_element(quarterEnergy))
    {
        destination = {};
        return Error::None;
    }

    for (std::size_t lag = 1; lag <= window.maxLag; ++lag)
    {
        double sum = 0.0;
        const auto comparisonSamples = analysisSamples - lag;
        for (std::size_t index = 0; index < comparisonSamples; ++index)
        {
            const auto left = sampleAt(index);
            const auto right = sampleAt(index + lag);
            if (!std::isfinite(left) || !std::isfinite(right))
                return Error::HistorySampleNonFinite;
            const auto delta = left - right;
            if (!std::isfinite(delta))
                return Error::DifferenceIntermediateNonFinite;
            const auto square = delta * delta;
            if (!std::isfinite(square))
                return Error::DifferenceIntermediateNonFinite;
            const auto candidateSum = sum + square;
            if (!std::isfinite(candidateSum))
                return Error::DifferenceIntermediateNonFinite;
            sum = candidateSum;
        }
        difference[lag] = sum;
    }

    double runningSum = 0.0;
    for (std::size_t lag = 1; lag <= window.maxLag; ++lag)
    {
        const auto candidateRunningSum = runningSum + difference[lag];
        if (!std::isfinite(candidateRunningSum))
            return Error::NormalizationIntermediateNonFinite;
        runningSum = candidateRunningSum;
        if (runningSum > 0.0)
        {
            const auto numerator = difference[lag]
                * static_cast<double>(lag);
            if (!std::isfinite(numerator))
                return Error::NormalizationIntermediateNonFinite;
            const auto value = numerator / runningSum;
            if (!std::isfinite(value))
                return Error::NormalizationIntermediateNonFinite;
            normalized[lag] = value;
        }
        else
        {
            normalized[lag] = 1.0;
        }
    }

    // The period is the first dip under the threshold, followed down to
    // its own minimum; without one, the deepest dip.
    std::size_t bestLag = 0;
    auto bestDifference = std::numeric_limits<double>::infinity();
    for (std::size_t lag = window.minLag; lag <= window.maxLag; ++lag)
    {
        if (!std::isfinite(normalized[lag]))
            return Error::NormalizationIntermediateNonFinite;
        if (normalized[lag] < bestDifference)
        {
            bestDifference = normalized[lag];
            bestLag = lag;
        }
    }
    if (bestLag == 0 || !std::isfinite(bestDifference))
        return Error::NormalizationIntermediateNonFinite;
    for (std::size_t lag = window.minLag; lag <= window.maxLag; ++lag)
    {
        if (normalized[lag] < periodDipThreshold)
        {
            while (lag < window.maxLag && normalized[lag + 1] < normalized[lag])
                ++lag;
            bestLag = lag;
            bestDifference = normalized[lag];
            break;
        }
    }

    const auto rawConfidence = 1.0 - bestDifference;
    if (!std::isfinite(rawConfidence))
        return Error::ConfidenceIntermediateNonFinite;
    const auto confidence = std::clamp(rawConfidence, 0.0, 1.0);

    auto refinedLag = static_cast<double>(bestLag);
    if (bestLag > 1 && bestLag < window.maxLag)
    {
        const auto left = normalized[bestLag - 1];
        const auto center = normalized[bestLag];
        const auto right = normalized[bestLag + 1];
        const auto doubledCenter = 2.0 * center;
        if (!std::isfinite(doubledCenter))
            return Error::RefinementIntermediateNonFinite;
        const auto leftMinusCenter = left - doubledCenter;
        if (!std::isfinite(leftMinusCenter))
            return Error::RefinementIntermediateNonFinite;
        const auto denominator = leftMinusCenter + right;
        if (!std::isfinite(denominator))
            return Error::RefinementIntermediateNonFinite;
        if (std::abs(denominator) > 1.0e-12)
        {
            const auto numerator = 0.5 * (left - right);
            if (!std::isfinite(numerator))
                return Error::RefinementIntermediateNonFinite;
            const auto offset = numerator / denominator;
            if (!std::isfinite(offset))
                return Error::RefinementIntermediateNonFinite;
            refinedLag += offset;
        }
    }
    if (!std::isfinite(refinedLag) || refinedLag <= 0.0)
        return Error::RefinementIntermediateNonFinite;

    const auto frequency = sampleRate / refinedLag;
    if (!std::isfinite(frequency))
        return Error::FrequencyIntermediateNonFinite;
    if (frequency < controls.minFrequency
        || frequency > controls.maxFrequency)
    {
        destination = {};
        return Error::None;
    }
    destination = {frequency, confidence};
    return Error::None;
}

// The estimate when the tracker is at least as confident as Confidence
// asks, and 0 otherwise.
[[nodiscard]] inline Error estimate(
    const Controls& controls,
    double sampleRate,
    const HistoryView& history,
    std::span<double> difference,
    std::span<double> normalized,
    double& destination) noexcept
{
    auto candidate = Estimate{};
    const auto error = estimateWithConfidence(
        controls, sampleRate, history, difference, normalized, candidate);
    if (error != Error::None)
        return error;
    destination = candidate.confidence < controls.confidence
        ? 0.0 : candidate.frequency;
    return Error::None;
}

// The tracker keeps its previous estimate until it is as confident of a
// new one as Confidence asks; its first
// estimate stands whatever its confidence, and silence changes nothing.
[[nodiscard]] inline bool acceptEstimate(
    const Estimate& candidate,
    double confidence,
    bool hasEstimate) noexcept
{
    return candidate.frequency > 0.0
        && (!hasEstimate || candidate.confidence >= confidence);
}

[[nodiscard]] inline Error applyScale(
    double frequency,
    double scale,
    double& destination) noexcept
{
    if (!std::isfinite(frequency))
        return Error::FrequencyIntermediateNonFinite;
    if (!std::isfinite(scale) || scale < 0.0)
        return Error::ScaleNonFiniteOrNegative;
    const auto scaled = frequency * scale;
    if (!std::isfinite(scaled))
        return Error::ScaledOutputNonFinite;
    destination = scaled == 0.0 ? 0.0 : scaled;
    return Error::None;
}
}
