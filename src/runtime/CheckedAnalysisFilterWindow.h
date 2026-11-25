#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace talas::runtime::checked_analysis_filter_window
{
// The AnalysisFilter isolates one harmonic of its Input with a quadrature
// output, and the filters at every harmonic up to half the sample rate add
// up to the Input. Averaging the Input,
// demodulated by the harmonic, over exactly one period of the Fundamental
// does both: every other harmonic completes whole cycles in the window and
// cancels, and the window's responses at the harmonics partition unity. A
// fractional period weighs its oldest sample by the fraction. The left
// channel is the cosine part and the right the sine part; above harmonic 0
// both are doubled for the negative frequencies a real signal also has.

// Frames between exact recomputations of the running sum, counted from the
// start, so rounding cannot accumulate and any partition renders alike.
inline constexpr std::uint64_t resynchronizationFrames = 4096;


// The window of a Fundamental in frames: its whole frames and the fraction
// of the next one.
[[nodiscard]] inline bool windowFrames(
    double sampleRate,
    double fundamental,
    std::size_t capacity,
    std::size_t headroomFrames,
    std::size_t& wholeFrames,
    double& fraction) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(fundamental) || fundamental <= 0.0)
    {
        return false;
    }
    const auto period = sampleRate / fundamental;
    if (!std::isfinite(period) || period < 1.0)
        return false;
    const auto whole = std::floor(period);
    // One more frame for the fractional tail, and a block's headroom.
    if (whole + 1.0 + static_cast<double>(headroomFrames)
        > static_cast<double>(capacity))
    {
        return false;
    }
    wholeFrames = static_cast<std::size_t>(whole);
    fraction = period - whole;
    return true;
}

// A Fundamental whose period the history cannot hold rises to the lowest
// whose period it holds, as a live value saturates at its range (owner,
// 2026-10-12, taken as recommended on 2026-10-13); one it holds, or one that
// is not finite or positive, is left as it is.
[[nodiscard]] inline double withinHistory(
    double sampleRate,
    double fundamental,
    std::size_t capacity,
    std::size_t headroomFrames) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(fundamental) || fundamental <= 0.0
        || capacity < headroomFrames + 2)
    {
        return fundamental;
    }
    const auto longest = static_cast<double>(capacity - 1 - headroomFrames);
    if (std::floor(sampleRate / fundamental) <= longest)
        return fundamental;
    auto lowest = sampleRate / longest;
    while (std::isfinite(lowest) && std::floor(sampleRate / lowest) > longest)
        lowest = std::nextafter(lowest, sampleRate);
    return lowest;
}

// The history holds the newest sample at writeIndex - 1; it must keep the
// window and one block beyond it, so a rolled-back block can run again from
// the state it started with.
template <typename State>
[[nodiscard]] inline double sampleAgo(
    std::span<const double> history,
    const State& state,
    std::size_t framesAgo) noexcept
{
    if (framesAgo >= state.count)
        return 0.0;
    const auto index = (state.writeIndex + history.size() - 1 - framesAgo)
        % history.size();
    return history[index];
}

// One frame: stores the input and gives the cosine and sine parts. State
// carries writeIndex, count, frame, fundamental, harmonic, the running sum
// and the rotations, as AnalysisFilterWindowState does.
template <typename State>
[[nodiscard]] inline bool process(
    std::span<double> history,
    std::size_t headroomFrames,
    State& state,
    double sampleRate,
    double fundamental,
    double harmonic,
    double input,
    double& left,
    double& right) noexcept
{
    if (history.size() < 2 || !std::isfinite(input)
        || !std::isfinite(harmonic) || harmonic < 0.0)
    {
        return false;
    }
    std::size_t wholeFrames = 0;
    double fraction = 0.0;
    if (!windowFrames(sampleRate, fundamental, history.size(),
            headroomFrames, wholeFrames, fraction))
    {
        return false;
    }
    const auto angle = 2.0 * std::numbers::pi * fundamental * harmonic
        / sampleRate;
    if (!std::isfinite(angle))
        return false;

    history[state.writeIndex] = input;
    state.writeIndex = (state.writeIndex + 1) % history.size();
    if (state.count < history.size())
        ++state.count;

    // S = sum over the window's whole frames of x[n - m] e^(i angle m).
    const auto recompute = !state.hasSum
        || state.fundamental != fundamental
        || state.harmonic != harmonic
        || state.frame % resynchronizationFrames == 0;
    if (recompute)
    {
        double sumReal = 0.0;
        double sumImaginary = 0.0;
        for (std::size_t m = 0; m < wholeFrames; ++m)
        {
            const auto sample = sampleAgo(
                std::span<const double>{history}, state, m);
            sumReal += sample * std::cos(angle * static_cast<double>(m));
            sumImaginary += sample * std::sin(angle * static_cast<double>(m));
        }
        state.sumReal = sumReal;
        state.sumImaginary = sumImaginary;
        state.rotationReal = std::cos(angle);
        state.rotationImaginary = std::sin(angle);
        const auto lastAngle = angle * static_cast<double>(wholeFrames - 1);
        state.lastReal = std::cos(lastAngle);
        state.lastImaginary = std::sin(lastAngle);
        const auto tailAngle = angle * static_cast<double>(wholeFrames);
        state.tailReal = std::cos(tailAngle);
        state.tailImaginary = std::sin(tailAngle);
    }
    else
    {
        // S[n] = x[n] + e^(i angle) (S[n - 1] - x[n - N] e^(i angle (N - 1))).
        const auto leaving = sampleAgo(
            std::span<const double>{history}, state, wholeFrames);
        const auto keptReal = state.sumReal - leaving * state.lastReal;
        const auto keptImaginary = state.sumImaginary
            - leaving * state.lastImaginary;
        state.sumReal = input + keptReal * state.rotationReal
            - keptImaginary * state.rotationImaginary;
        state.sumImaginary = keptReal * state.rotationImaginary
            + keptImaginary * state.rotationReal;
    }
    state.hasSum = true;
    state.fundamental = fundamental;
    state.harmonic = harmonic;
    ++state.frame;

    const auto tail = fraction * sampleAgo(
        std::span<const double>{history}, state, wholeFrames);
    const auto period = static_cast<double>(wholeFrames) + fraction;
    const auto real = (state.sumReal + tail * state.tailReal) / period;
    const auto imaginary = (state.sumImaginary + tail * state.tailImaginary)
        / period;
    const auto gain = harmonic == 0.0 ? 1.0 : 2.0;
    left = gain * real;
    right = harmonic == 0.0 ? 0.0 : gain * imaginary;
    return std::isfinite(left) && std::isfinite(right)
        && std::isfinite(state.sumReal) && std::isfinite(state.sumImaginary);
}

// History frames a Fundamental needs: its window and a block beyond it.
[[nodiscard]] inline std::size_t historyCapacity(
    double sampleRate,
    double lowestFundamental,
    std::size_t maximumBlockFrames) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(lowestFundamental) || lowestFundamental <= 0.0)
    {
        return 0;
    }
    const auto period = std::floor(sampleRate / lowestFundamental);
    if (!std::isfinite(period) || period > 1.0e8)
        return 0;
    return static_cast<std::size_t>(period) + 2 + maximumBlockFrames;
}

// The history an AnalysisFilter keeps: for a fixed Fundamental, its period
// and a block; for a changing one, the longest period the history admits and
// a block, or 0 without history to admit it.
[[nodiscard]] inline std::size_t historyFrames(
    bool changingFundamental,
    double fundamental,
    double sampleRate,
    std::size_t maximumBlockFrames,
    std::size_t maximumHistoryFrames) noexcept
{
    if (!changingFundamental)
        return historyCapacity(sampleRate, fundamental, maximumBlockFrames);
    if (maximumHistoryFrames == 0)
        return 0;
    return maximumHistoryFrames + 2 + maximumBlockFrames;
}
}
