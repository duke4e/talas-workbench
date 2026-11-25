#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::detail::lossy_integrator
{
struct State
{
    double value = 0.0;
};

[[nodiscard]] inline bool advanceBlock(
    State& state,
    std::span<const double> input,
    double scale,
    double releaseTime,
    double sampleRate,
    std::span<double> output) noexcept
{
    if (input.size() != output.size()
        || !std::isfinite(state.value)
        || !std::isfinite(scale)
        || !std::isfinite(releaseTime)
        || releaseTime < 0.0
        || !std::isfinite(sampleRate)
        || sampleRate <= 0.0)
        return false;

    const auto blend = releaseTime == 0.0
        ? 1.0
        : -std::expm1(-1.0 / (releaseTime * sampleRate));
    if (!std::isfinite(blend) || blend < 0.0 || blend > 1.0)
        return false;

    auto candidate = state.value;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto target = input[frame] * scale;
        if (!std::isfinite(target))
            return false;
        const auto next = (1.0 - blend) * candidate + blend * target;
        if (!std::isfinite(next))
            return false;
        candidate = next;
        output[frame] = candidate;
    }
    state.value = candidate;
    return true;
}
}
