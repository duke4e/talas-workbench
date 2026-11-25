/*
 * Derived from the CDP System, Release 8: dev/filter/filters0.c and
 * fltpcon.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Phasing.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
bool Phasing::prepare(
    double sampleRate, Mode mode, double gain, double delayMilliseconds, double prescale,
    bool liveDelay)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || (mode != Mode::PhaseShift && mode != Mode::Phaser)
        || !std::isfinite(gain) || gain < -1.0 || gain > 1.0
        || !std::isfinite(delayMilliseconds) || delayMilliseconds < 1000.0 / sampleRate
        || delayMilliseconds > maximumDelayMilliseconds
        || !std::isfinite(prescale) || prescale < -1.0 || prescale > 1.0)
    {
        return false;
    }
    // convert_delay_times_to_samples_and_get_maxdelay for a fixed delay, or
    // for breakpoints that may reach the longest delay.
    sampleRate_ = sampleRate;
    auto delay = std::lround(delayMilliseconds * 0.001 * sampleRate);
    if (delay < 1)
        delay = 1;
    const auto frames = liveDelay
        ? static_cast<std::size_t>(delayFrames(maximumDelayMilliseconds)) + 1
        : static_cast<std::size_t>(delay);
    mode_ = mode;
    gain_ = gain;
    prescale_ = prescale;
    liveDelay_ = liveDelay;
    startDelay_ = static_cast<double>(delay);
    delay_ = startDelay_;
    first_.assign(frames, 0.0);
    second_.assign(frames, 0.0);
    position_ = 0;
    return true;
}

double Phasing::delayFrames(double delayMilliseconds) const noexcept
{
    // The conversion of a breakpoint file's delays.
    auto frames = static_cast<double>(std::lround(delayMilliseconds * 0.001 * sampleRate_));
    if (frames < 1.0)
        frames = 1.0;
    return frames;
}

bool Phasing::setDelay(double delayMilliseconds) noexcept
{
    if (first_.empty() || !liveDelay_ || !std::isfinite(delayMilliseconds)
        || delayMilliseconds < 1000.0 / sampleRate_ || delayMilliseconds > maximumDelayMilliseconds)
    {
        return false;
    }
    delay_ = delayFrames(delayMilliseconds);
    return true;
}

void Phasing::reset() noexcept
{
    std::ranges::fill(first_, 0.0);
    std::ranges::fill(second_, 0.0);
    position_ = 0;
    delay_ = startDelay_;
}

bool Phasing::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (first_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    if (liveDelay_)
    {
        // varidelay_allpass for one channel, statement for statement, its
        // delay a whole number of frames as CDP rounds its breakpoints.
        auto* delbuf1 = first_.data();
        auto* delbuf2 = second_.data();
        const auto maxdelsamps = static_cast<std::ptrdiff_t>(first_.size());
        auto delbufpos = static_cast<std::ptrdiff_t>(position_);
        const auto modulus = [&](std::ptrdiff_t value) {
            value %= maxdelsamps;
            return value < 0 ? value + maxdelsamps : value;
        };
        for (std::size_t frame = 0; frame < input.size(); ++frame)
        {
            const auto delay = static_cast<std::ptrdiff_t>(delay_);
            const auto frac = delay_ - static_cast<double>(delay);
            const auto thisdelbfpos1 = modulus(delbufpos - delay);
            const auto thisdelbfpos2 = modulus(thisdelbfpos1 + 1);
            const auto sample = static_cast<float>(input[frame]);
            const auto ip = static_cast<double>(sample) * prescale_;
            auto op = (-gain_) * ip;
            auto delval1 = delbuf1[thisdelbfpos1];
            auto delval2 = delbuf1[thisdelbfpos2];
            auto delval = delval1 + ((delval2 - delval1) * frac);
            op += delval;
            delval1 = delbuf2[thisdelbfpos1];
            delval2 = delbuf2[thisdelbfpos2];
            delval = delval1 + ((delval2 - delval1) * frac);
            op += gain_ * delval;
            delbuf1[delbufpos] = ip;
            delbuf2[delbufpos] = op;
            if (++delbufpos >= maxdelsamps)
                delbufpos -= maxdelsamps;
            output[frame] = mode_ == Mode::PhaseShift
                ? static_cast<double>(static_cast<float>(op))
                : static_cast<double>(static_cast<float>((op + static_cast<double>(sample)) * 0.5));
        }
        position_ = static_cast<std::size_t>(delbufpos);
        return true;
    }
    // allpass for one channel, statement for statement, keeping CDP's float
    // samples.
    auto* delbuf1 = first_.data();
    auto* delbuf2 = second_.data();
    const auto maxdelsamps = first_.size();
    auto delbufpos = position_;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto sample = static_cast<float>(input[frame]);
        const auto ip = static_cast<double>(sample) * prescale_;
        auto op = (-gain_) * ip;
        op += delbuf1[delbufpos];
        op += gain_ * delbuf2[delbufpos];
        delbuf1[delbufpos] = ip;
        delbuf2[delbufpos] = op;
        if (++delbufpos >= maxdelsamps)
            delbufpos -= maxdelsamps;
        output[frame] = mode_ == Mode::PhaseShift
            ? static_cast<double>(static_cast<float>(op))
            : static_cast<double>(static_cast<float>((op + static_cast<double>(sample)) * 0.5));
    }
    position_ = delbufpos;
    return true;
}
}
