/*
 * Derived from the CDP System, Release 8: dev/standalone/mchanpan.c and
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
#include "talas/cdp/RotatingPan.h"

#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR; mchanpan.c's ROOT2.
constexpr double FLTERR = 0.000002;
constexpr double ROOT2 = 1.4142136;

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

bool RotatingPan::prepare(double sampleRate, int outputChannels, double startChannel, double speed, double focus,
    bool anticlockwise)
{
    if (!std::isfinite(sampleRate) || sampleRate < maximumSpeed)
        return false;
    if (outputChannels < minimumChannels || outputChannels > maximumChannels)
        return false;
    if (!inRange(startChannel, 0.0, 16.0) || startChannel > static_cast<double>(outputChannels))
        return false;
    if (!inRange(speed, 0.0, maximumSpeed) || !inRange(focus, 0.0, 1.0))
        return false;
    sampleRate_ = sampleRate;
    channels_ = static_cast<std::size_t>(outputChannels);
    start_ = startChannel;
    startSpeed_ = speed;
    focus_ = focus;
    // check_the_param_validity_and_consistency.
    scalefact_ = (1.0 - focus) / 2.0;
    anticlockwise_ = anticlockwise;
    reset();
    return true;
}

bool RotatingPan::setSpeed(double speed) noexcept
{
    if (!prepared() || !inRange(speed, 0.0, maximumSpeed))
        return false;
    speed_ = speed;
    return true;
}

void RotatingPan::reset() noexcept
{
    speed_ = startSpeed_;
    increment_ = 0.0;
    position_ = start_;
    frame_ = 0;
}

bool RotatingPan::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || output.size() != input.size() * channels_)
        return false;
    for (const auto x : input)
    {
        if (!std::isfinite(x))
            return false;
    }
    const auto outchans = static_cast<int>(channels_);
    const auto doutchans = static_cast<double>(outchans);
    for (std::size_t i = 0; i < input.size(); i++)
    {
        // mchanpan2: the speed every 256 frames.
        if (frame_ % speedReadFrames == 0)
        {
            const auto chansPerSec = speed_ * doutchans;
            increment_ = chansPerSec / sampleRate_;
            if (anticlockwise_)
                increment_ = -increment_;
        }
        position_ += increment_;
        if (position_ > doutchans)
            position_ -= doutchans;
        else if (position_ < 0.0)
            position_ += doutchans;
        // mchan_pancalc, rotation, focus as CDP8's build fuses it.
        auto leftchan = static_cast<int>(std::floor(position_));
        const auto stereoposition = position_ - static_cast<double>(leftchan);
        const auto zerocentredposition = std::fma(stereoposition, 2.0, -1.0);
        const auto relpos = zerocentredposition < 0 ? -zerocentredposition : zerocentredposition;
        const auto temp = std::fma(relpos, relpos, 1.0);
        const auto holecompensate = ROOT2 / std::sqrt(temp);
        auto rightgain = stereoposition * holecompensate;
        auto leftgain = (1.0 - stereoposition) * holecompensate;
        rightgain = std::fma(rightgain, focus_, scalefact_);
        leftgain = std::fma(leftgain, focus_, scalefact_);
        const auto farleftgain = scalefact_ * (1.0 - stereoposition);
        const auto farrightgain = scalefact_ * stereoposition;
        if (leftchan == 0)
            leftchan = outchans;
        leftchan--;
        const auto farleftchan = (leftchan + outchans - 1) % outchans;
        const auto rightchan = (leftchan + 1) % outchans;
        const auto farrightchan = (rightchan + 1) % outchans;

        const auto sample = static_cast<double>(static_cast<float>(input[i]));
        auto* frame = output.data() + i * channels_;
        float obuf[maximumChannels] = {};
        obuf[farleftchan] = static_cast<float>(std::fma(sample, farleftgain, static_cast<double>(obuf[farleftchan])));
        obuf[leftchan] = static_cast<float>(std::fma(sample, leftgain, static_cast<double>(obuf[leftchan])));
        obuf[rightchan] = static_cast<float>(std::fma(sample, rightgain, static_cast<double>(obuf[rightchan])));
        obuf[farrightchan]
            = static_cast<float>(std::fma(sample, farrightgain, static_cast<double>(obuf[farrightchan])));
        for (std::size_t c = 0; c < channels_; c++)
            frame[c] = static_cast<double>(obuf[c]);
        frame_++;
    }
    return true;
}
}
