/*
 * Derived from the CDP System, Release 8: dev/new/rejoin.c,
 * dev/cdp2k/tklib1.c and dev/cdp2k/readdata.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Rejoin.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR.
constexpr double FLTERR = 0.000002;

// tklib1.c's flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const auto upperbnd = f2 + FLTERR;
    const auto lowerbnd = f2 - FLTERR;
    return !(f1 > upperbnd || f1 < lowerbnd);
}
}

bool Rejoin::prepare(double gain) noexcept
{
    // readdata.c's range check, then check_rejoin_param_validity_and_consistency.
    if (!std::isfinite(gain))
        return false;
    if (flteq(gain, 0.0))
        return false;
    if (flteq(gain, maximumGain))
        gain = maximumGain;
    if (gain > maximumGain || gain < 0.0)
        return false;
    gain_ = gain;
    return true;
}

bool Rejoin::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    const auto finite = [](double sample) {
        return std::isfinite(sample);
    };
    if (gain_ <= 0.0 || input.size() != side.size() || input.size() != output.size()
        || !std::ranges::all_of(input, finite) || !std::ranges::all_of(side, finite))
    {
        return false;
    }
    for (std::size_t n = 0; n < input.size(); n++)
    {
        // rejoin: the longest file times Gain, then each other added.
        auto obuf = static_cast<float>(static_cast<double>(static_cast<float>(input[n])) * gain_);
        const double ibuf = static_cast<float>(side[n]);
        obuf = static_cast<float>(std::fma(ibuf, gain_, static_cast<double>(obuf)));
        // CDP would scale the whole output where this passes 0.95.
        output[n] = std::clamp(static_cast<double>(obuf), -1.0, 1.0);
    }
    return true;
}
}
