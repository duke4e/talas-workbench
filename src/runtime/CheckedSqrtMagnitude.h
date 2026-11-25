#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_sqrt_magnitude
{
enum class Error
{
    None,
    SizeMismatch,
    LeftNonFinite,
    RightNonFinite,
    MagnitudeInvalid,
    ResultNonFinite,
};

[[nodiscard]] inline Error advance(
    double left,
    std::optional<double> right,
    double& output) noexcept
{
    if (!std::isfinite(left))
        return Error::LeftNonFinite;

    const auto rightValue = right.value_or(0.0);
    if (!std::isfinite(rightValue))
        return Error::RightNonFinite;

    const auto magnitude = std::hypot(left, rightValue);
    if (std::isnan(magnitude) || magnitude < 0.0)
        return Error::MagnitudeInvalid;

    const auto result = magnitude >= 1.0 ? 1.0 : magnitude;
    if (!std::isfinite(result) || result < 0.0)
        return Error::ResultNonFinite;

    output = result;
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> left,
    std::optional<std::span<const double>> right,
    std::span<double> candidates,
    std::span<double> output) noexcept
{
    if (left.size() != candidates.size()
        || left.size() != output.size()
        || (right.has_value() && left.size() != right->size()))
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < left.size(); ++frame)
    {
        const auto rightValue = right.has_value()
            ? std::optional<double>{(*right)[frame]}
            : std::optional<double>{};
        const auto error = advance(
            left[frame], rightValue, candidates[frame]);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < left.size(); ++frame)
        output[frame] = candidates[frame];
    return Error::None;
}
}
