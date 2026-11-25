#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_difference
{
enum class Error
{
    None,
    SizeMismatch,
    LeftNonFinite,
    RightNonFinite,
    ResultNonFinite,
};

[[nodiscard]] inline Error advance(
    double left,
    double right,
    double& output) noexcept
{
    if (!std::isfinite(left))
        return Error::LeftNonFinite;
    if (!std::isfinite(right))
        return Error::RightNonFinite;

    const auto result = left - right;
    if (!std::isfinite(result))
        return Error::ResultNonFinite;

    output = result;
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> left,
    std::span<const double> right,
    std::span<double> candidates,
    std::span<double> output) noexcept
{
    if (left.size() != right.size()
        || left.size() != candidates.size()
        || left.size() != output.size())
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < left.size(); ++frame)
    {
        const auto error = advance(
            left[frame], right[frame], candidates[frame]);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < left.size(); ++frame)
        output[frame] = candidates[frame];
    return Error::None;
}
}
