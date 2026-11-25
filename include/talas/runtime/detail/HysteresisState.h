#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::detail::hysteresis
{
struct State
{
    double value = 0.0;
};

// Bounded local play-operator model: the output retains its previous value
// while that value remains within the input's width-wide interval.
[[nodiscard]] inline bool advanceBlock(
    State& state,
    std::span<const double> input,
    double width,
    std::span<double> output) noexcept
{
    if (input.size() != output.size()
        || !std::isfinite(state.value)
        || !std::isfinite(width) || width < 0.0)
        return false;

    auto candidate = state.value;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto sample = input[frame];
        const auto lower = sample - width;
        const auto upper = sample + width;
        if (!std::isfinite(sample) || !std::isfinite(lower)
            || !std::isfinite(upper))
            return false;
        candidate = std::clamp(candidate, lower, upper);
        output[frame] = candidate;
    }
    state.value = candidate;
    return true;
}
}
