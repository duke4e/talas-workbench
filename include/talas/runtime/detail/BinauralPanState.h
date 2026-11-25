#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::detail::binaural_pan
{
// Local bounded model: 0 is left, 0.5 is centered, 1 is right. The far ear
// receives a delayed and quieter copy.
inline constexpr double maximumInterauralDelaySeconds = 0.0006;
inline constexpr std::size_t historyCapacity = 256;

struct State
{
    std::array<double, historyCapacity> history{};
    std::size_t writeIndex = 0;
};

[[nodiscard]] inline bool advanceBlock(
    State& state,
    std::span<const double> input,
    std::span<const double> panInput,
    double staticPan,
    double sampleRate,
    std::span<double> leftOutput,
    std::span<double> rightOutput) noexcept
{
    if (input.size() != leftOutput.size()
        || input.size() != rightOutput.size()
        || (!panInput.empty() && panInput.size() != input.size())
        || !std::isfinite(staticPan) || staticPan < 0.0 || staticPan > 1.0
        || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || sampleRate * maximumInterauralDelaySeconds
            >= static_cast<double>(historyCapacity - 1)
        || state.writeIndex >= historyCapacity)
        return false;

    const auto maximumDelayFrames =
        sampleRate * maximumInterauralDelaySeconds;
    const auto delayed = [&](double delayFrames) noexcept {
        const auto wholeFrames =
            static_cast<std::size_t>(std::floor(delayFrames));
        const auto fraction = delayFrames - static_cast<double>(wholeFrames);
        const auto newer = state.history[
            (state.writeIndex + historyCapacity - wholeFrames)
                % historyCapacity];
        const auto older = state.history[
            (state.writeIndex + historyCapacity - wholeFrames - 1)
                % historyCapacity];
        return (1.0 - fraction) * newer + fraction * older;
    };
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto sample = input[frame];
        const auto pan = panInput.empty() ? staticPan : panInput[frame];
        if (!std::isfinite(sample) || !std::isfinite(pan)
            || pan < 0.0 || pan > 1.0)
            return false;

        state.history[state.writeIndex] = sample;
        const auto leftFar = std::max(0.0, 2.0 * pan - 1.0);
        const auto rightFar = std::max(0.0, 1.0 - 2.0 * pan);
        const auto left = delayed(maximumDelayFrames * leftFar)
            * (1.0 - 0.5 * leftFar);
        const auto right = delayed(maximumDelayFrames * rightFar)
            * (1.0 - 0.5 * rightFar);
        if (!std::isfinite(left) || !std::isfinite(right))
            return false;
        leftOutput[frame] = left;
        rightOutput[frame] = right;
        state.writeIndex = (state.writeIndex + 1) % historyCapacity;
    }
    return true;
}
}
