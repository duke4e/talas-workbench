#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_equality
{
enum class Error
{
    None,
    SizeMismatch,
    LeftNonFinite,
    RightNonFinite,
    ToleranceInvalid,
    ResultNonFinite,
};

// The output is one wherever the inputs are
// equal plus or minus the Tolerance. A zero Tolerance keeps the exact
// comparison, so subnormal differences still count as unequal.
[[nodiscard]] inline Error advance(
    double left,
    double right,
    double& output,
    double tolerance = 0.0) noexcept
{
    if (!std::isfinite(left))
        return Error::LeftNonFinite;
    if (!std::isfinite(right))
        return Error::RightNonFinite;
    if (!std::isfinite(tolerance) || tolerance < 0.0)
        return Error::ToleranceInvalid;

    const auto equal = tolerance == 0.0
        ? left == right
        : std::abs(left - right) <= tolerance;
    const auto result = equal ? 1.0 : 0.0;
    if (!std::isfinite(result))
        return Error::ResultNonFinite;

    output = result;
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> left,
    std::span<const double> right,
    std::span<double> output,
    double tolerance = 0.0) noexcept
{
    if (left.size() != right.size() || left.size() != output.size())
        return Error::SizeMismatch;

    for (std::size_t frame = 0; frame < left.size(); ++frame)
    {
        auto candidate = 0.0;
        const auto error = advance(
            left[frame], right[frame], candidate, tolerance);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < left.size(); ++frame)
    {
        auto candidate = 0.0;
        const auto error = advance(
            left[frame], right[frame], candidate, tolerance);
        if (error != Error::None)
            return error;
        output[frame] = candidate;
    }
    return Error::None;
}
}
