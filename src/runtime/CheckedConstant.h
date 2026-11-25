#pragma once

#include <cmath>
#include <span>

namespace talas::runtime::checked_constant
{
enum class Error
{
    None,
    ValueNonFinite,
};

[[nodiscard]] inline Error advanceBlock(
    double value,
    std::span<double> output) noexcept
{
    if (output.empty())
        return Error::None;
    if (!std::isfinite(value))
        return Error::ValueNonFinite;

    for (auto& frame : output)
        frame = value;
    return Error::None;
}
}
