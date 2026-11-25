#pragma once

#include <cmath>

namespace talas::runtime::checked_triggered_sample_and_hold
{
struct State
{
    double heldValue = 0.0;
    bool hasHeldValue = false;
    bool triggerWasPositive = false;
};

enum class Error
{
    None,
    TriggerNonFinite,
    CaptureValueNonFinite,
};

[[nodiscard]] inline Error advance(
    const State& state,
    double value,
    double trigger,
    State& candidate,
    double& output) noexcept
{
    if (!std::isfinite(trigger))
        return Error::TriggerNonFinite;

    candidate = state;
    const auto triggerIsPositive = trigger > 0.0;
    if (triggerIsPositive && !state.triggerWasPositive)
    {
        if (!std::isfinite(value))
            return Error::CaptureValueNonFinite;
        candidate.heldValue = value;
        candidate.hasHeldValue = true;
    }
    candidate.triggerWasPositive = triggerIsPositive;
    output = candidate.hasHeldValue ? candidate.heldValue : 0.0;
    return Error::None;
}
}
