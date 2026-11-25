#pragma once

#include "CheckedFrequencyTrackerState.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_monotonizer_tracking
{
// The Monotonizer removes the pitch changes of its Input and plays it at
// Frequency, for any Input pitch between MinInputPitch and MaxInputPitch.
// The pitch is the FrequencyTracker's estimate
// over that range, updated every hop; the Input is shifted by Frequency over
// the estimate, and plays unshifted until the first estimate.
inline constexpr double confidence = 0.5;

// Frames of history the estimate reads: two of the longest period.
[[nodiscard]] inline std::size_t analysisFrames(
    double sampleRate,
    double minimumPitch) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(minimumPitch) || minimumPitch <= 0.0)
    {
        return 0;
    }
    const auto period = std::floor(sampleRate / minimumPitch);
    if (!std::isfinite(period) || period < 1.0
        || period > static_cast<double>(
            checked_frequency_tracker::maximumSupportedLag))
    {
        return 0;
    }
    return static_cast<std::size_t>(period) * 2U;
}

// The history keeps a block beyond the analysis, so a rolled-back block runs
// again from the state it started with.
[[nodiscard]] inline std::size_t historyFrames(
    double sampleRate,
    double minimumPitch,
    std::size_t maximumBlockFrames) noexcept
{
    const auto frames = analysisFrames(sampleRate, minimumPitch);
    return frames == 0 ? 0 : frames + maximumBlockFrames;
}

// Scratch frames for the difference function: the longest lag and one.
[[nodiscard]] inline std::size_t scratchFrames(
    double sampleRate,
    double minimumPitch) noexcept
{
    const auto frames = analysisFrames(sampleRate, minimumPitch);
    return frames == 0 ? 0 : frames / 2U + 1U;
}

// A range a Sound feeds may open anywhere, so its history holds the longest
// analysis the FrequencyTracker admits.
[[nodiscard]] inline std::size_t fedRangeAnalysisFrames(
    std::size_t maxRealtimeHistoryFrames) noexcept
{
    return std::min(
        maxRealtimeHistoryFrames,
        checked_frequency_tracker::maximumSupportedLag * 2U);
}

// Pitches that pass what they are bound by saturate against it, as a live
// value saturates at its range (owner, 2026-10-12, taken as recommended on
// 2026-10-13): a MaxInputPitch below MinInputPitch stops there, closing the
// range on a known pitch; an open range's MinInputPitch rises to the lowest
// pitch the analysis can track and its MaxInputPitch stays a 24-bit step of
// the sample rate below Nyquist; for a known pitch, Frequency stays within
// the half to double of it the vocoder shifts by. Values that are not finite
// or not positive are left to be refused.
struct Pitches
{
    double target = 0.0;
    double low = 0.0;
    double high = 0.0;
};

[[nodiscard]] inline Pitches saturated(
    double sampleRate,
    std::size_t analysisCapacity,
    Pitches pitches) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(pitches.low) || pitches.low <= 0.0
        || !std::isfinite(pitches.high) || pitches.high <= 0.0
        || !std::isfinite(pitches.target) || pitches.target <= 0.0)
    {
        return pitches;
    }
    pitches.high = std::max(pitches.high, pitches.low);
    if (pitches.low != pitches.high)
    {
        const auto lag = std::min(
            checked_frequency_tracker::maximumSupportedLag, analysisCapacity / 2U);
        if (lag != 0)
        {
            pitches.low = std::max(pitches.low,
                checked_frequency_tracker::lowestFrequencyForLag(sampleRate, lag));
        }
        pitches.high = std::max(pitches.high, pitches.low);
        if (pitches.low != pitches.high && pitches.high >= sampleRate * 0.5)
            pitches.high = std::max(pitches.low, sampleRate * 0.5 - sampleRate / 16777216.0);
    }
    if (pitches.low == pitches.high)
        pitches.target = std::clamp(pitches.target, 0.5 * pitches.low, 2.0 * pitches.low);
    return pitches;
}

// State carries writeIndex, count, hopPhase, estimate and hasEstimate, as
// MonotonizerTrackerState does. While the range is closed the history still
// fills, but nothing is estimated.
template <typename State>
[[nodiscard]] inline bool advance(
    std::span<double> history,
    std::span<double> difference,
    std::span<double> normalized,
    State& state,
    double sampleRate,
    double minimumPitch,
    double maximumPitch,
    double input,
    bool estimating = true) noexcept
{
    if (history.empty() || !std::isfinite(input))
        return false;
    history[state.writeIndex] = input;
    state.writeIndex = (state.writeIndex + 1) % history.size();
    if (state.count < history.size())
        ++state.count;
    if (++state.hopPhase
        < checked_frequency_tracker::estimateHopFrames(sampleRate))
    {
        return true;
    }
    state.hopPhase = 0;
    if (!estimating)
        return true;
    const auto view = checked_frequency_tracker::HistoryView{
        history,
        (state.writeIndex + history.size() - state.count) % history.size(),
        state.count,
    };
    auto estimate = checked_frequency_tracker::Estimate{};
    if (checked_frequency_tracker::estimateWithConfidence(
            {minimumPitch, maximumPitch, confidence, 1.0},
            sampleRate, view, difference, normalized, estimate)
        != checked_frequency_tracker::Error::None)
    {
        return false;
    }
    if (checked_frequency_tracker::acceptEstimate(
            estimate, confidence, state.hasEstimate))
    {
        state.estimate = estimate.frequency;
        state.hasEstimate = true;
    }
    return true;
}

// Half steps from the estimate to Frequency, or none before an estimate.
template <typename State>
[[nodiscard]] inline double interval(
    const State& state,
    double frequency) noexcept
{
    if (!state.hasEstimate || state.estimate <= 0.0)
        return 0.0;
    // An estimate refined a little past the range stays within an octave.
    const auto halfSteps = 12.0 * std::log2(frequency / state.estimate);
    return halfSteps < -12.0 ? -12.0 : (halfSteps > 12.0 ? 12.0 : halfSteps);
}
}
