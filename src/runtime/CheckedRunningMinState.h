#pragma once

#include <algorithm>
#include <cmath>

namespace talas::runtime::checked_running_min
{
struct State
{
    bool initialized = false;
    double minimum = 0.0;
};

enum class Error
{
    None,
    ValueNonFinite,
    ResetNonFinite,
    MaximumAmplitudeNonFinite,
};

[[nodiscard]] inline Error advance(
    State& state,
    double value,
    double reset,
    double maximumAmplitude,
    double& output) noexcept
{
    if (!std::isfinite(value))
        return Error::ValueNonFinite;
    if (!std::isfinite(reset))
        return Error::ResetNonFinite;
    if (!std::isfinite(maximumAmplitude))
        return Error::MaximumAmplitudeNonFinite;

    if (reset != 0.0)
    {
        state.initialized = true;
        state.minimum = maximumAmplitude;
    }
    if (!state.initialized)
    {
        state.initialized = true;
        state.minimum = value;
    }
    else
    {
        state.minimum = std::min(state.minimum, value);
    }
    output = state.minimum;
    return Error::None;
}
}
