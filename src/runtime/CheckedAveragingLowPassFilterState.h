#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace talas::runtime::checked_averaging_low_pass_filter
{
constexpr double fractionalSampleThreshold = 1.0e-12;

enum class Error
{
    None,
    SampleRateNonFiniteOrNonPositive,
    CutoffNonFiniteOrNonPositive,
    WindowNonFiniteOrUnrepresentable,
    HistoryCapacityExceeded,
    StateInvalid,
    AudioInNonFinite,
    ScaleNonFinite,
    ScaledInputNonFinite,
    HistorySampleNonFinite,
    SumNonFinite,
    FractionalContributionNonFinite,
    OutputNonFinite,
};

struct Window
{
    double samples = 1.0;
    std::size_t wholeSamples = 1;
    double fractionalSample = 0.0;
    std::size_t requiredHistory = 1;
};

// A field's Cutoff whose window the history cannot hold rises to the lowest
// whose window it holds, as a live value saturates at its range (owner,
// 2026-10-12, taken as recommended on 2026-10-13); one it holds, or one that
// is not finite or positive, is left as it is.
[[nodiscard]] inline double withinHistory(
    double sampleRate,
    double cutoff,
    std::size_t historyCapacity) noexcept
{
    if (historyCapacity == 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(cutoff) || cutoff <= 0.0
        || sampleRate / cutoff <= static_cast<double>(historyCapacity))
    {
        return cutoff;
    }
    auto lowest = sampleRate / static_cast<double>(historyCapacity);
    while (std::isfinite(lowest) && sampleRate / lowest > static_cast<double>(historyCapacity))
        lowest = std::nextafter(lowest, sampleRate);
    return lowest;
}

[[nodiscard]] inline Error deriveWindow(
    double sampleRate,
    double cutoff,
    std::size_t historyCapacity,
    Window& destination) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return Error::SampleRateNonFiniteOrNonPositive;
    if (!std::isfinite(cutoff) || cutoff <= 0.0)
        return Error::CutoffNonFiniteOrNonPositive;

    const auto ratio = sampleRate / cutoff;
    const auto samples = std::max(1.0, ratio);
    const auto wholeValue = std::floor(samples);
    const auto exclusiveWholeLimit = std::ldexp(
        1.0, std::numeric_limits<std::size_t>::digits);
    if (!std::isfinite(ratio) || !std::isfinite(samples)
        || !std::isfinite(wholeValue) || wholeValue < 1.0
        || !std::isfinite(exclusiveWholeLimit)
        || wholeValue >= exclusiveWholeLimit)
    {
        return Error::WindowNonFiniteOrUnrepresentable;
    }

    const auto whole = static_cast<std::size_t>(wholeValue);
    const auto fractional = samples - wholeValue;
    if (!std::isfinite(fractional) || fractional < 0.0
        || fractional >= 1.0)
    {
        return Error::WindowNonFiniteOrUnrepresentable;
    }
    const auto hasFractionalSample =
        fractional > fractionalSampleThreshold;
    if (hasFractionalSample
        && whole == std::numeric_limits<std::size_t>::max())
    {
        return Error::WindowNonFiniteOrUnrepresentable;
    }
    const auto required = whole + (hasFractionalSample ? 1U : 0U);
    if (historyCapacity != 0 && required > historyCapacity)
        return Error::HistoryCapacityExceeded;

    destination = {samples, whole, fractional, required};
    return Error::None;
}

[[nodiscard]] inline Error advance(
    std::span<double> history,
    std::size_t& writeIndex,
    const Window& window,
    double audioIn,
    double scale,
    double& output) noexcept
{
    if (history.empty() || writeIndex >= history.size()
        || window.wholeSamples == 0
        || window.requiredHistory > history.size()
        || !std::isfinite(window.samples) || window.samples < 1.0
        || !std::isfinite(window.fractionalSample)
        || window.fractionalSample < 0.0
        || window.fractionalSample >= 1.0)
    {
        return Error::StateInvalid;
    }
    if (!std::isfinite(audioIn))
        return Error::AudioInNonFinite;
    if (!std::isfinite(scale))
        return Error::ScaleNonFinite;

    const auto scaledInput = audioIn * scale;
    if (!std::isfinite(scaledInput))
        return Error::ScaledInputNonFinite;
    history[writeIndex] = scaledInput;

    double sum = 0.0;
    for (std::size_t offset = 0; offset < window.wholeSamples; ++offset)
    {
        const auto historyIndex =
            (writeIndex + history.size() - offset) % history.size();
        const auto sample = history[historyIndex];
        if (!std::isfinite(sample))
            return Error::HistorySampleNonFinite;
        const auto candidate = sum + sample;
        if (!std::isfinite(candidate))
            return Error::SumNonFinite;
        sum = candidate;
    }

    if (window.fractionalSample > fractionalSampleThreshold)
    {
        const auto historyIndex =
            (writeIndex + history.size() - window.wholeSamples)
            % history.size();
        const auto sample = history[historyIndex];
        if (!std::isfinite(sample))
            return Error::HistorySampleNonFinite;
        const auto contribution = sample * window.fractionalSample;
        if (!std::isfinite(contribution))
            return Error::FractionalContributionNonFinite;
        const auto candidate = sum + contribution;
        if (!std::isfinite(candidate))
            return Error::SumNonFinite;
        sum = candidate;
    }

    const auto candidateOutput = sum / window.samples;
    if (!std::isfinite(candidateOutput))
        return Error::OutputNonFinite;
    output = candidateOutput;
    writeIndex = (writeIndex + 1U) % history.size();
    return Error::None;
}

inline void reset(std::span<double> history, std::size_t& writeIndex) noexcept
{
    std::ranges::fill(history, 0.0);
    writeIndex = 0;
}
}
