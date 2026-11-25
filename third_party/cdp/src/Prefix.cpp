/*
 * Derived from the CDP System, Release 8: dev/standalone/prefix.c and
 * dev/cdp2k/readdata.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Prefix.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR.
constexpr double FLTERR = 0.000002;
}

bool Prefix::prepare(double sampleRate, int channels, double duration)
{
    if (!std::isfinite(sampleRate) || !(sampleRate > 0.0) || channels < 1 || channels > maximumChannels)
        return false;
    // check_prefix_param_validity_and_consistency.
    if (!std::isfinite(duration) || duration < 0.0 || duration > maximumSeconds)
        return false;
    if (!(duration > FLTERR || duration < -FLTERR))
        return false;
    // prefix: round(dur * srate) frames of silence.
    const double frames = std::round(duration * sampleRate);
    if (!(frames >= 1.0))
        return false;
    Prefix fresh;
    fresh.channels_ = static_cast<std::size_t>(channels);
    fresh.delay_ = static_cast<std::size_t>(frames);
    fresh.line_.assign(fresh.delay_ * fresh.channels_, 0.0F);
    *this = std::move(fresh);
    return true;
}

void Prefix::reset() noexcept
{
    std::fill(line_.begin(), line_.end(), 0.0F);
    position_ = 0;
}

bool Prefix::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size() || input.size() % channels_ != 0
        || !std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); }))
        return false;
    const std::size_t size = line_.size();
    for (std::size_t i = 0; i < input.size(); i++)
    {
        const float delayed = line_[position_];
        line_[position_] = static_cast<float>(input[i]);
        output[i] = static_cast<double>(delayed);
        if (++position_ == size)
            position_ = 0;
    }
    return true;
}
}
