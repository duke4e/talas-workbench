/*
 * Derived from the CDP System, Release 8: dev/modify/radical.c and
 * dev/cdp2k/tklib1.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Quantise.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// do_lowbit2 assumes a 16-bit source.
constexpr int sourceBits = 16;

// radical.c defines round(x) as lround(x), whose long CDP8's arm64 build
// converts with saturation at the long's range (fcvtas) before turning it
// back to a double. Within that range this is std::round; a zero's sign,
// where they differ, is lost when do_lowbit2 subtracts 0.5.
[[nodiscard]] double cdpRound(double value) noexcept
{
    constexpr double longLimit = 9223372036854775808.0;
    return std::round(std::clamp(value, -longLimit, longLimit));
}

// do_lowbit2's rescalefac, pow(2.0, 16 - bits): exact.
[[nodiscard]] double rescaleFactor(int bitResolution) noexcept
{
    return std::ldexp(1.0, sourceBits - bitResolution);
}
}

bool Quantise::prepare(int bitResolution) noexcept
{
    // tklib1.c's range for MOD_LOBIT2: 1 to MAX_BIT_DIV (16) bits.
    if (bitResolution < 1 || bitResolution > maximumBitResolution)
        return false;
    rescale_ = rescaleFactor(bitResolution);
    return true;
}

bool Quantise::prepared() const noexcept
{
    return rescale_ > 0.0;
}

void Quantise::reset() noexcept
{
}

bool Quantise::setBitResolution(int bitResolution) noexcept
{
    if (!prepared() || bitResolution < 1 || bitResolution > maximumBitResolution)
        return false;
    rescale_ = rescaleFactor(bitResolution);
    return true;
}

bool Quantise::process(
    std::span<const double> input,
    std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t n = 0; n < input.size(); ++n)
    {
        // do_lowbit2, statement for statement, on CDP's float buffer.
        const auto sample = static_cast<float>(input[n]);
        double samp = static_cast<double>(sample) * 32767.0;
        if (samp <= -32767.0)
            samp = -32767.0;
        samp /= rescale_;
        samp = cdpRound(samp + 0.5) - 0.5;
        samp *= rescale_;
        output[n] = static_cast<double>(static_cast<float>(samp / 32768.0));
    }
    return true;
}
}
