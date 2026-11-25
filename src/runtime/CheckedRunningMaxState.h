#pragma once

#include <algorithm>
#include <cmath>

namespace talas::runtime::checked_running_max
{
struct State
{
    bool initialized = false;
    double maximum = 0.0;
};

enum class Error
{
    None,
    ValueNonFinite,
    ResetNonFinite,
};

[[nodiscard]] inline Error advance(
    State& state,
    double value,
    double reset,
    double& output) noexcept
{
    if (!std::isfinite(value))
        return Error::ValueNonFinite;
    if (!std::isfinite(reset))
        return Error::ResetNonFinite;

    if (reset != 0.0)
    {
        state.initialized = true;
        state.maximum = 0.0;
    }
    if (!state.initialized)
    {
        state.initialized = true;
        state.maximum = value;
    }
    else
    {
        state.maximum = std::max(state.maximum, value);
    }
    output = state.maximum;
    return Error::None;
}
}
