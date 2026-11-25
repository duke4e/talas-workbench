/*
 * Derived from the CDP System, Release 8: dev/standalone/frame.c and
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
#include "talas/cdp/ChannelEnvelope.h"

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

// readdata.c: a value within FLTERR of a range end is that end.
[[nodiscard]] bool inRange(double& value, double lo, double hi) noexcept
{
    if (!std::isfinite(value))
        return false;
    if (flteq(value, lo))
        value = lo;
    if (flteq(value, hi))
        value = hi;
    return !(value > hi || value < lo);
}
}

bool ChannelEnvelope::prepare(double sampleRate, int channels, std::span<const int> enveloped, double gain)
{
    if (!std::isfinite(sampleRate) || !(sampleRate > 0.0))
        return false;
    if (channels < minimumChannels || channels > maximumChannels || enveloped.empty())
        return false;
    std::array<bool, maximumChannels> set{};
    for (const auto c : enveloped)
    {
        // read_the_special_data and editchans_test.
        if (c < 1 || c > channels || set[static_cast<std::size_t>(c - 1)])
            return false;
        set[static_cast<std::size_t>(c - 1)] = true;
    }
    if (!inRange(gain, 0.0, 1.0))
        return false;
    channels_ = static_cast<std::size_t>(channels);
    enveloped_ = set;
    startGain_ = gain;
    reset();
    return true;
}

bool ChannelEnvelope::setGain(double gain, std::size_t rampFrames) noexcept
{
    if (!prepared() || !inRange(gain, 0.0, 1.0))
        return false;
    if (rampFrames == 0)
    {
        gain_ = gain;
        increment_ = 0.0;
    }
    else
    {
        // envchans at a breakpoint: thisgain = nextgain, then the step.
        gain_ = target_;
        const auto gainstep = gain - gain_;
        const auto timestep = static_cast<double>(rampFrames);
        increment_ = gainstep / timestep;
    }
    target_ = gain;
    return true;
}

bool ChannelEnvelope::holdGain() noexcept
{
    if (!prepared())
        return false;
    increment_ = 0.0;
    return true;
}

void ChannelEnvelope::reset() noexcept
{
    gain_ = startGain_;
    target_ = startGain_;
    increment_ = 0.0;
}

bool ChannelEnvelope::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || output.size() != input.size() || input.size() % channels_ != 0)
        return false;
    for (const auto x : input)
    {
        if (!std::isfinite(x))
            return false;
    }
    const auto frames = input.size() / channels_;
    for (std::size_t i = 0; i < frames; i++)
    {
        const auto* in = input.data() + i * channels_;
        auto* out = output.data() + i * channels_;
        for (std::size_t c = 0; c < channels_; c++)
        {
            const auto buf = static_cast<float>(in[c]);
            out[c] = enveloped_[c] ? static_cast<double>(static_cast<float>(static_cast<double>(buf) * gain_))
                                   : static_cast<double>(buf);
        }
        // envchans: the step, held at the target (sfsys.h's min and max).
        gain_ += increment_;
        if (increment_ > 0.0)
            gain_ = gain_ > target_ ? target_ : gain_;
        else if (increment_ < 0.0)
            gain_ = gain_ > target_ ? gain_ : target_;
    }
    return true;
}
}
