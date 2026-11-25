/*
 * Derived from the CDP System, Release 8: dev/modify/gain.c,
 * dev/cdp2k/tklib1.c and dev/cdp2k/dzsetup.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/LoudnessGain.h"

#include <algorithm>
#include <cmath>

#if defined(__APPLE__)
// Apple's libm, where CDP8's build turns pow(10.0, x) into __exp10(x).
extern "C" double __exp10(double);
#endif

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR.
constexpr double flterr = 0.000002;

// pow(10.0, x) as CDP8's build calls it.
[[nodiscard]] double cdpExp10(double x) noexcept
{
#if defined(__APPLE__)
    return __exp10(x);
#else
    return std::pow(10.0, x);
#endif
}

// tklib1.c's dbtogain, statement for statement: -96 dB and below is
// silence, and within FLTERR of 0 dB (flteq) is exactly 1.
[[nodiscard]] double dbtogain(double val) noexcept
{
    bool isNeg = false;
    if (val <= LoudnessGain::minimumDecibels)
        return 0.0;
    if (val <= flterr && val >= -flterr)
        return 1.0;
    if (val < 0.0)
    {
        val = -val;
        isNeg = true;
    }
    val /= 20.0;
    val = cdpExp10(val);
    if (isNeg)
        val = 1.0 / val;
    return val;
}
}

bool LoudnessGain::valid(double gain) const noexcept
{
    if (!std::isfinite(gain))
        return false;
    // tklib1.c's ranges for LOUDNESS_GAIN and LOUDNESS_DBGAIN.
    if (mode_ == Mode::Decibels)
        return gain >= minimumDecibels && gain <= maximumDecibels;
    return gain >= 0.0 && gain <= maximumGain;
}

double LoudnessGain::factor(double gain) const noexcept
{
    // loudness_process converts a dB gain, or each value of its breakpoint
    // file, with dbtogain before gain_process reads it.
    return mode_ == Mode::Decibels ? dbtogain(gain) : gain;
}

bool LoudnessGain::prepare(Mode mode, double gain, bool clip, bool liveGain) noexcept
{
    if (mode < Mode::Gain || mode > Mode::Decibels)
        return false;
    LoudnessGain candidate;
    candidate.mode_ = mode;
    if (!candidate.valid(gain))
        return false;
    const auto start = candidate.factor(gain);
    // gain_process refuses a fixed gain at or below FLTERR ("the soundfile
    // will be reduced to SILENCE"), but not a breakpoint file's.
    if (!liveGain && start <= flterr)
        return false;
    prepared_ = true;
    mode_ = mode;
    clip_ = clip;
    liveGain_ = liveGain;
    startGain_ = start;
    reset();
    return true;
}

bool LoudnessGain::setGain(double gain) noexcept
{
    if (!prepared_ || !liveGain_ || !valid(gain))
        return false;
    gain_ = factor(gain);
    return true;
}

void LoudnessGain::reset() noexcept
{
    gain_ = startGain_;
}

bool LoudnessGain::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared_ || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t i = 0; i < input.size(); i++)
    {
        // gainenv (and gain, whose fixed gain is never negative), statement
        // for statement, on CDP's float buffer; F_MAXSAMP is 1.
        double s = static_cast<float>(input[i]);
        s *= gain_;
        if (s >= 0)
        {
            if (clip_ && s > 1.0)
                s = 1.0;
        }
        else
        {
            if (clip_ && s < -1.0)
                s = -1.0;
        }
        output[i] = static_cast<double>(static_cast<float>(s));
    }
    return true;
}
}
