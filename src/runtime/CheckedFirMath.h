#pragma once

#include "CheckedBiquadMath.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace talas::runtime::checked_fir
{

inline constexpr std::size_t maximumPreparedTapCount = 4096;

[[nodiscard]] inline bool processSample(
    std::span<const double> coefficients,
    std::span<double> history,
    double input,
    double& output) noexcept
{
    if (coefficients.empty()
        || history.size() + 1 != coefficients.size()
        || !std::isfinite(input))
    {
        return false;
    }

    double accumulator = 0.0;
    if (!checked_biquad::checkedMultiply(
            coefficients.front(), input, accumulator))
    {
        return false;
    }
    for (std::size_t tap = 1; tap < coefficients.size(); ++tap)
    {
        double product = 0.0;
        if (!checked_biquad::checkedMultiply(
                coefficients[tap], history[tap - 1], product)
            || !checked_biquad::checkedAdd(
                accumulator, product, accumulator))
        {
            return false;
        }
    }

    if (history.size() > 1)
    {
        std::move_backward(
            history.begin(), history.end() - 1, history.end());
    }
    if (!history.empty())
        history.front() = input;
    output = accumulator;
    return true;
}

}
