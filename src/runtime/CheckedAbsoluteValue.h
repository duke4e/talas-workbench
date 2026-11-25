#pragma once

#include <cmath>
#include <span>

namespace talas::runtime::checked_absolute_value
{
enum class Error
{
    None,
    SizeMismatch,
    InputNonFinite,
    MagnitudeNonFinite,
};

[[nodiscard]] inline Error advance(
    double input,
    double& output) noexcept
{
    if (!std::isfinite(input))
        return Error::InputNonFinite;

    const auto magnitude = std::abs(input);
    if (!std::isfinite(magnitude) || magnitude < 0.0)
        return Error::MagnitudeNonFinite;

    output = magnitude;
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> input,
    std::span<double> output) noexcept
{
    if (input.size() != output.size())
        return Error::SizeMismatch;

    for (const auto value : input)
    {
        auto candidate = 0.0;
        const auto error = advance(value, candidate);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        auto candidate = 0.0;
        const auto error = advance(input[frame], candidate);
        if (error != Error::None)
            return error;
        output[frame] = candidate;
    }
    return Error::None;
}
}
