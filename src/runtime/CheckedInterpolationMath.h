#pragma once

#include <cmath>

namespace talas::runtime::checked_interpolation
{
enum class Error
{
    None,
    FirstNonFinite,
    SecondNonFinite,
    AmountOutOfRange,
    ArithmeticNonFinite,
};

[[nodiscard]] inline Error interpolate(
    double first,
    double second,
    double amount,
    double& result) noexcept
{
    if (!std::isfinite(first))
        return Error::FirstNonFinite;
    if (!std::isfinite(second))
        return Error::SecondNonFinite;
    if (!std::isfinite(amount) || amount < 0.0 || amount > 1.0)
        return Error::AmountOutOfRange;

    if (amount == 0.0)
    {
        result = first;
        return Error::None;
    }
    if (amount == 1.0)
    {
        result = second;
        return Error::None;
    }

    const auto delta = second - first;
    if (std::isfinite(delta))
    {
        const auto adjustment = delta * amount;
        const auto guarded = first + adjustment;
        if (!std::isfinite(adjustment) || !std::isfinite(guarded))
            return Error::ArithmeticNonFinite;

        const auto published = std::fma(delta, amount, first);
        if (!std::isfinite(published))
            return Error::ArithmeticNonFinite;
        result = published;
        return Error::None;
    }

    const auto inverseAmount = 1.0 - amount;
    const auto firstTerm = inverseAmount * first;
    const auto secondTerm = amount * second;
    const auto published = firstTerm + secondTerm;
    if (!std::isfinite(inverseAmount)
        || !std::isfinite(firstTerm)
        || !std::isfinite(secondTerm)
        || !std::isfinite(published))
    {
        return Error::ArithmeticNonFinite;
    }
    result = published;
    return Error::None;
}
}
