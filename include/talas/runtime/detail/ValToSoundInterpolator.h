#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace talas::runtime::detail::val_to_sound
{
struct State
{
    std::uint64_t renderedFrames = 0;
    std::uint64_t tickIndex = 0;
    std::uint64_t tickStartFrame = 0;
    std::uint64_t nextTickFrame = 0;
    double from = 0.0;
    double to = 0.0;
    bool initialized = false;
};

[[nodiscard]] inline bool nextTick(
    std::uint64_t tickIndex,
    std::uint64_t currentFrame,
    double sampleRate,
    std::uint64_t& frame) noexcept
{
    if (tickIndex == std::numeric_limits<std::uint64_t>::max())
        return false;
    const auto position = std::ceil(
        static_cast<long double>(tickIndex + 1)
        * static_cast<long double>(sampleRate) / 1000.0L);
    if (!std::isfinite(position)
        || position >= static_cast<long double>(
                           std::numeric_limits<std::uint64_t>::max())
        || currentFrame == std::numeric_limits<std::uint64_t>::max())
        return false;
    frame = std::max(currentFrame + 1,
        static_cast<std::uint64_t>(position));
    return true;
}

[[nodiscard]] inline bool advanceBlock(
    State& state,
    double value,
    double sampleRate,
    std::span<double> output) noexcept
{
    if (!std::isfinite(value) || !std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || output.size() > std::numeric_limits<std::uint64_t>::max()
            - state.renderedFrames)
        return false;

    auto candidate = state;
    for (std::size_t index = 0; index < output.size(); ++index)
    {
        const auto frame = candidate.renderedFrames;
        if (!candidate.initialized)
        {
            candidate.from = value;
            candidate.to = value;
            candidate.tickIndex = 0;
            candidate.tickStartFrame = frame;
            if (!nextTick(0, frame, sampleRate,
                    candidate.nextTickFrame))
                return false;
            candidate.initialized = true;
        }
        else if (frame >= candidate.nextTickFrame)
        {
            if (candidate.tickIndex
                == std::numeric_limits<std::uint64_t>::max())
                return false;
            ++candidate.tickIndex;
            candidate.from = candidate.to;
            candidate.to = value;
            candidate.tickStartFrame = frame;
            if (!nextTick(candidate.tickIndex, frame, sampleRate,
                    candidate.nextTickFrame))
                return false;
        }

        const auto span = candidate.nextTickFrame
            - candidate.tickStartFrame;
        const auto elapsed = frame - candidate.tickStartFrame;
        const auto amount = static_cast<double>(elapsed)
            / static_cast<double>(span);
        const auto sample = std::fma(
            candidate.to - candidate.from, amount,
            candidate.from);
        if (!std::isfinite(sample))
            return false;
        output[index] = sample;
        ++candidate.renderedFrames;
    }
    state = candidate;
    return true;
}
}
