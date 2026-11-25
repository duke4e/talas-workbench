#pragma once

#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>

namespace talas::runtime::checked_arc_tan
{
enum class Error
{
    None,
    SizeMismatch,
    InputNonFinite,
    AngleNonFinite,
};

[[nodiscard]] inline Error advance(
    double input,
    double& output) noexcept
{
    if (!std::isfinite(input))
        return Error::InputNonFinite;

    const auto angle = std::atan(input);
    if (!std::isfinite(angle))
        return Error::AngleNonFinite;

    output = angle;
    return Error::None;
}

// ArcTan: the four-quadrant arctangent of right over left, as a
// fraction of a half turn.
[[nodiscard]] inline Error advanceRightOverLeft(
    double left,
    double right,
    double& output) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return Error::InputNonFinite;

    const auto angle = std::atan2(right, left) / std::numbers::pi;
    if (!std::isfinite(angle))
        return Error::AngleNonFinite;

    output = angle;
    return Error::None;
}

// Checks every frame before writing any; output may be left or right.
[[nodiscard]] inline Error advanceRightOverLeftBlock(
    std::span<const double> left,
    std::span<const double> right,
    std::span<double> output) noexcept
{
    if (left.size() != output.size() || right.size() != output.size())
        return Error::SizeMismatch;

    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        auto candidate = 0.0;
        const auto error = advanceRightOverLeft(
            left[frame], right[frame], candidate);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        auto candidate = 0.0;
        static_cast<void>(advanceRightOverLeft(
            left[frame], right[frame], candidate));
        output[frame] = candidate;
    }
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
