#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::dsp::re_resonator
{
// An RE table (ADR-0022) is the filter order, an integer from 1 to
// maximumOrder, followed by the coefficient sets in time order, each the
// order's number of reflection coefficients between -1 and 1 exclusive.
inline constexpr std::size_t maximumOrder = 128;

struct Table
{
    std::size_t order = 0;
    std::size_t frameCount = 0;
    std::span<const double> coefficients;
};

[[nodiscard]] inline std::optional<Table> parseTable(
    std::span<const double> samples) noexcept
{
    if (samples.size() < 2 || !std::isfinite(samples.front())
        || samples.front() != std::floor(samples.front())
        || samples.front() < 1.0
        || samples.front() > static_cast<double>(maximumOrder))
    {
        return std::nullopt;
    }
    const auto order = static_cast<std::size_t>(samples.front());
    const auto coefficients = samples.subspan(1);
    if (coefficients.size() % order != 0)
        return std::nullopt;
    for (const auto coefficient : coefficients)
    {
        if (!std::isfinite(coefficient) || std::abs(coefficient) >= 1.0)
            return std::nullopt;
    }
    return Table{order, coefficients.size() / order, coefficients};
}

// TimeIndex -1 reads the first coefficient
// set and 1 the last. Values outside that range read the nearer end.
[[nodiscard]] inline double framePosition(
    double timeIndex,
    std::size_t frameCount) noexcept
{
    if (frameCount < 2 || !(timeIndex > -1.0))
        return 0.0;
    const auto last = static_cast<double>(frameCount - 1);
    if (timeIndex >= 1.0)
        return last;
    return (timeIndex + 1.0) * 0.5 * last;
}

// The coefficients between the two sets around a frame position, linearly
// interpolated; reflection coefficients between -1 and 1 keep the filter
// stable wherever they are read.
inline void interpolate(
    const Table& table,
    double position,
    std::span<double> coefficients) noexcept
{
    const auto last = table.frameCount - 1;
    const auto lower = std::min(
        static_cast<std::size_t>(std::max(position, 0.0)), last);
    const auto upper = std::min(lower + 1, last);
    const auto fraction = position - static_cast<double>(lower);
    const auto* from = table.coefficients.data() + lower * table.order;
    const auto* to = table.coefficients.data() + upper * table.order;
    for (std::size_t index = 0; index < table.order; ++index)
        coefficients[index] = from[index] + fraction * (to[index] - from[index]);
}

// One sample through the all-pole lattice 1 / A(z) the reflection
// coefficients describe. `state` holds the delayed backward errors b0 to
// b(order - 1). Both renderers call this definition, so they agree to the
// bit.
[[gnu::noinline]] inline bool synthesize(
    std::span<const double> coefficients,
    std::span<double> state,
    double input,
    double& output) noexcept
{
    auto forward = input;
    for (auto stage = coefficients.size(); stage > 0; --stage)
    {
        const auto delayed = state[stage - 1];
        forward -= coefficients[stage - 1] * delayed;
        if (stage < coefficients.size())
            state[stage] = delayed + coefficients[stage - 1] * forward;
    }
    if (!std::isfinite(forward))
        return false;
    state[0] = forward;
    output = forward;
    return true;
}

// One sample through the inverse lattice A(z), which turns the analyzed
// sound into the excitation that recreates it.
inline bool analyze(
    std::span<const double> coefficients,
    std::span<double> state,
    double input,
    double& residual) noexcept
{
    auto forward = input;
    auto backward = input;
    for (std::size_t stage = 0; stage < coefficients.size(); ++stage)
    {
        const auto delayed = state[stage];
        state[stage] = backward;
        const auto nextForward = forward + coefficients[stage] * delayed;
        backward = coefficients[stage] * forward + delayed;
        forward = nextForward;
    }
    if (!std::isfinite(forward))
        return false;
    residual = forward;
    return true;
}
}
