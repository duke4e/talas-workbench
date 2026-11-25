#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_product
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

    const auto result = left * right;
    if (!std::isfinite(result))
        return Error::ResultNonFinite;

    output = result;
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> left,
    std::span<const double> right,
    std::span<double> output) noexcept
{
    if (left.size() != right.size() || left.size() != output.size())
        return Error::SizeMismatch;

    for (std::size_t frame = 0; frame < left.size(); ++frame)
    {
        auto candidate = 0.0;
        const auto error = advance(left[frame], right[frame], candidate);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < left.size(); ++frame)
    {
        auto candidate = 0.0;
        const auto error = advance(left[frame], right[frame], candidate);
        if (error != Error::None)
            return error;
        output[frame] = candidate;
    }
    return Error::None;
}
}
