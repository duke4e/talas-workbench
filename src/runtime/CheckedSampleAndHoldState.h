#pragma once

#include <cmath>
#include <cstddef>
#include <limits>

namespace talas::runtime::checked_sample_and_hold
{
struct State
{
    std::size_t renderedFrames = 0;
    double heldValue = 0.0;
    bool hasHeldValue = false;
};

struct Controls
{
    double input = 0.0;
    double holdTimeSeconds = 0.0;
    double offsetTimeSeconds = 0.0;
    double sampleRate = 0.0;
};

enum class Error
{
    None,
    SampleRateNonFiniteOrNonPositive,
    HoldTimeNonFiniteOrNonPositive,
    HoldFramesNonFiniteOrOutOfRange,
    OffsetTimeNonFiniteOrNegative,
    OffsetFramesNonFiniteOrOutOfRange,
    HeldValueNonFinite,
    CaptureInputNonFinite,
    OutputNonFinite,
    RenderedFrameOverflow,
};

[[nodiscard]] inline Error advance(
    const State& state,
    const Controls& controls,
    State& candidate,
    double& output) noexcept
{
    constexpr auto maxRoundedFrames =
        static_cast<double>(std::numeric_limits<long long>::max());
    if (!std::isfinite(controls.sampleRate)
        || controls.sampleRate <= 0.0)
    {
        return Error::SampleRateNonFiniteOrNonPositive;
    }
    if (!std::isfinite(controls.holdTimeSeconds)
        || controls.holdTimeSeconds <= 0.0)
    {
        return Error::HoldTimeNonFiniteOrNonPositive;
    }
    const auto holdFramesDouble =
        controls.holdTimeSeconds * controls.sampleRate;
    if (!std::isfinite(holdFramesDouble)
        || holdFramesDouble <= 0.0
        || holdFramesDouble >= maxRoundedFrames)
    {
        return Error::HoldFramesNonFiniteOrOutOfRange;
    }
    if (!std::isfinite(controls.offsetTimeSeconds)
        || controls.offsetTimeSeconds < 0.0)
    {
        return Error::OffsetTimeNonFiniteOrNegative;
    }
    const auto offsetFramesDouble =
        controls.offsetTimeSeconds * controls.sampleRate;
    if (!std::isfinite(offsetFramesDouble)
        || offsetFramesDouble < 0.0
        || offsetFramesDouble >= maxRoundedFrames)
    {
        return Error::OffsetFramesNonFiniteOrOutOfRange;
    }
    if (state.hasHeldValue && !std::isfinite(state.heldValue))
        return Error::HeldValueNonFinite;
    if (state.renderedFrames
        == std::numeric_limits<std::size_t>::max())
    {
        return Error::RenderedFrameOverflow;
    }

    const auto roundedHoldFrames = std::llround(holdFramesDouble);
    const auto holdFrames = static_cast<std::size_t>(
        roundedHoldFrames < 1 ? 1 : roundedHoldFrames);
    const auto offsetFrames = static_cast<std::size_t>(
        std::llround(offsetFramesDouble));

    auto next = state;
    auto candidateOutput = 0.0;
    if (state.renderedFrames >= offsetFrames)
    {
        const auto frameSinceOffset =
            state.renderedFrames - offsetFrames;
        const auto capture = !next.hasHeldValue
            || frameSinceOffset % holdFrames == 0;
        if (capture)
        {
            if (!std::isfinite(controls.input))
                return Error::CaptureInputNonFinite;
            next.heldValue = controls.input;
            next.hasHeldValue = true;
        }
        candidateOutput = next.heldValue;
    }
    if (!std::isfinite(candidateOutput))
        return Error::OutputNonFinite;

    ++next.renderedFrames;
    candidate = next;
    output = candidateOutput;
    return Error::None;
}

inline void reset(State& state) noexcept
{
    state = {};
}
}
