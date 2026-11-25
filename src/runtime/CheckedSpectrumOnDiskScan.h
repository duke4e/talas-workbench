#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace talas::runtime::checked_spectrum_on_disk
{
inline constexpr std::size_t maximumLogicalFrames = 4096;
inline constexpr std::size_t maximumTotalTriples = 1U << 20U;

struct State
{
    double cursor = 0.0;
    bool triggerWasPositive = false;
};

struct Transition
{
    std::size_t baseFrame = 0;
    std::size_t nextFrame = 0;
    double fraction = 0.0;
    State candidate;
};

enum class TransitionError
{
    None,
    InvalidState,
    InvalidRate,
    InvalidTrigger,
    InvalidFrameCount,
};

[[nodiscard]] inline TransitionError transition(
    const State& committed,
    double rateScale,
    double trigger,
    std::size_t logicalFrameCount,
    Transition& output) noexcept
{
    output = {};
    if (!std::isfinite(committed.cursor) || committed.cursor < 0.0)
        return TransitionError::InvalidState;
    if (!std::isfinite(rateScale) || rateScale < 0.0)
        return TransitionError::InvalidRate;
    if (!std::isfinite(trigger))
        return TransitionError::InvalidTrigger;
    if (logicalFrameCount == 0
        || logicalFrameCount > maximumLogicalFrames)
    {
        return TransitionError::InvalidFrameCount;
    }

    const auto finalFrame = static_cast<double>(logicalFrameCount - 1U);
    const auto triggerPositive = trigger > 0.0;
    const auto restarted = triggerPositive
        && !committed.triggerWasPositive;
    const auto selected = restarted
        ? 0.0
        : std::min(committed.cursor, finalFrame);
    const auto baseValue = std::floor(selected);
    output.baseFrame = static_cast<std::size_t>(baseValue);
    output.nextFrame = std::min(
        output.baseFrame + 1U,
        logicalFrameCount - 1U);
    output.fraction = selected - baseValue;

    output.candidate.triggerWasPositive = triggerPositive;
    output.candidate.cursor = rateScale >= finalFrame - selected
        ? finalFrame
        : selected + rateScale;
    return TransitionError::None;
}
}
