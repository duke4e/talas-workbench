/*
 * Derived from the CDP System, Release 8: dev/modify/radical.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/LowerResolution.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// The integer range do_lowbit gives a 32-bit float file.
constexpr double maximumSampleValue = 2147483647.0;
}

bool LowerResolution::prepare(int bitResolution, int sampleRateDivision) noexcept
{
    if (sampleRateDivision < 1
        || sampleRateDivision > maximumSampleRateDivision)
    {
        *this = {};
        return false;
    }
    division_ = sampleRateDivision;
    if (!setBitResolution(bitResolution))
    {
        *this = {};
        return false;
    }
    reset();
    return true;
}

bool LowerResolution::prepared() const noexcept
{
    return division_ > 0;
}

bool LowerResolution::setBitResolution(int bitResolution) noexcept
{
    if (!prepared() || bitResolution < 1
        || bitResolution > maximumBitResolution)
    {
        return false;
    }
    // lobit_pconsistency: below 16 bits a step spans 2^(16 - bits) sixteen-bit
    // levels, while 16 bits keeps the value 16; a float file scales it by 2^16.
    const auto levels = bitResolution == maximumBitResolution
        ? static_cast<double>(maximumBitResolution)
        : std::ldexp(1.0, maximumBitResolution - bitResolution);
    step_ = levels * 65536.0;
    return true;
}

void LowerResolution::reset() noexcept
{
    count_ = 0;
    sum_ = 0.0;
    held_ = 0.0;
}

bool LowerResolution::process(
    std::span<const double> input,
    std::span<double> output) noexcept
{
    if (division_ == 0 || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        sum_ += input[frame];
        if (++count_ == division_)
        {
            held_ = quantize(sum_ / division_);
            sum_ = 0.0;
            count_ = 0;
        }
        output[frame] = held_;
    }
    return true;
}

std::size_t LowerResolution::latencyFrames() const noexcept
{
    return division_ > 0 ? static_cast<std::size_t>(division_ - 1) : 0;
}

// do_lowbit: scale to the file's integer range, round to a multiple of the
// step, scale back, clip, and store in CDP's float sample buffer.
double LowerResolution::quantize(double mean) const noexcept
{
    auto value = mean * maximumSampleValue;
    value /= step_;
    value = std::round(value);
    value *= step_;
    value /= maximumSampleValue;
    return static_cast<float>(std::clamp(value, -1.0, 1.0));
}
}
