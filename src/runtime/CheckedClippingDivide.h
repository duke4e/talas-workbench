#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_clipping_divide
{
enum class Error
{
    None,
    SizeMismatch,
    InputNonFinite,
    ScaleNonFinite,
    QuotientNonFinite,
    OutputNonFinite,
};

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> numerator,
    std::span<const double> denominator,
    double scaleFallback,
    std::optional<std::span<const double>> connectedScale,
    std::span<double> candidates,
    std::span<double> output) noexcept
{
    const auto frames = numerator.size();
    if (denominator.size() != frames || candidates.size() != frames
        || output.size() != frames
        || (connectedScale.has_value() && connectedScale->size() != frames))
    {
        return Error::SizeMismatch;
    }
    if (!connectedScale.has_value() && !std::isfinite(scaleFallback))
        return Error::ScaleNonFinite;

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto left = numerator[frame];
        const auto right = denominator[frame];
        const auto scale = connectedScale.has_value()
            ? (*connectedScale)[frame] : scaleFallback;
        if (!std::isfinite(left) || !std::isfinite(right))
            return Error::InputNonFinite;
        if (!std::isfinite(scale))
            return Error::ScaleNonFinite;
        // A zero Denominator clips the quotient to the Numerator's sign,
        // and 0 / 0 gives 0 (owner decision 2026-10-04).
        const auto quotient = right != 0.0 ? left / right
            : left > 0.0 ? 1.0
            : left < 0.0 ? -1.0
            : 0.0;
        if (!std::isfinite(quotient))
            return Error::QuotientNonFinite;
        const auto value = quotient * scale;
        if (!std::isfinite(value))
            return Error::OutputNonFinite;
        candidates[frame] = value;
    }
    for (std::size_t frame = 0; frame < frames; ++frame)
        output[frame] = candidates[frame];
    return Error::None;
}
}
