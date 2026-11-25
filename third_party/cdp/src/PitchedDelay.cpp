/*
 * Derived from the CDP System, Release 8: dev/new/newdelay.c,
 * dev/cdp2k/tklib1.c and dev/cdp2k/readdata.c
 * Copyright (c) 1983-2013 Trevor Wishart, Richard Dobson and Composers
 * Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/PitchedDelay.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's LOW_A, ONE_OVER_LN2 and FLTERR.
constexpr double LOW_A = 6.875;
constexpr double ONE_OVER_LN2 = 1.442695;
constexpr double FLTERR = 0.000002;
// newdelay.c's ROOTLEN_DELBUF: the line holds two frames beyond the delay.
constexpr std::size_t rootFrames = 2;
// The longest line prepare takes: 10 s (0.1 Hz) at 768 kHz.
constexpr long maximumLineFrames = 7680000;

// tklib1.c's miditohz: CDP8's build calls exp2 for pow(2.0,...).
[[nodiscard]] double miditohz(double val) noexcept
{
    val += 3.0;
    val /= 12.0;
    val = std::exp2(val);
    val *= LOW_A;
    return val;
}

// tklib1.c's hztomidi (LOG2 is log(x) * ONE_OVER_LN2).
[[nodiscard]] double hztomidi(double hz) noexcept
{
    auto val = hz;
    val /= LOW_A;
    val = std::log(val) * ONE_OVER_LN2;
    val *= 12.0;
    val -= 3.0;
    return val;
}

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

double PitchedDelay::lowestPitch() noexcept
{
    return hztomidi(lowestHz);
}

double PitchedDelay::highestPitch(double sampleRate) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    return hztomidi(sampleRate / 2.0);
}

long PitchedDelay::delayForPitch(double sampleRate, double midiPitch) noexcept
{
    // newdelay and domulti: ddelay = 1.0/miditohz(pitch); delay =
    // round(ddelay * sr).
    const auto ddelay = 1.0 / miditohz(midiPitch);
    return std::lround(ddelay * sampleRate);
}

bool PitchedDelay::snap(double& pitch, double lowest) const noexcept
{
    const auto lo = lowestPitch();
    if (!inRange(pitch, lo, highest_))
        return false;
    return pitch >= lowest;
}

bool PitchedDelay::prepare(double sampleRate, double midiPitch, double mix, double feedback, double lowestLivePitch)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return false;
    const auto lo = lowestPitch();
    const auto hi = highestPitch(sampleRate);
    if (!inRange(midiPitch, lo, hi) || !inRange(mix, minimumMix, maximumMix)
        || !inRange(feedback, minimumFeedback, maximumFeedback))
    {
        return false;
    }
    auto lowest = midiPitch;
    if (!std::isnan(lowestLivePitch))
    {
        if (!inRange(lowestLivePitch, lo, hi))
            return false;
        lowest = std::min(lowestLivePitch, midiPitch);
    }
    const auto longest = delayForPitch(sampleRate, lowest);
    const auto start = delayForPitch(sampleRate, midiPitch);
    if (longest < 1 || start < 1 || longest > maximumLineFrames)
        return false;
    // newdelay: the dry path is 1 - Mix divided by 1 / (Feedback + 1).
    auto fdfrwd = 1.0 - mix;
    const auto rescale = (1.0 / (feedback + 1.0));
    fdfrwd /= rescale;

    line_.assign(static_cast<std::size_t>(longest) + rootFrames, 0.0);
    sampleRate_ = sampleRate;
    startPitch_ = midiPitch;
    lowest_ = lowest;
    highest_ = hi;
    gain_ = mix;
    feedback_ = feedback;
    feedForward_ = fdfrwd;
    startDelay_ = static_cast<std::size_t>(start);
    delay_ = startDelay_;
    pointer_ = 0;
    return true;
}

bool PitchedDelay::setPitch(double midiPitch) noexcept
{
    if (line_.empty() || !snap(midiPitch, lowest_))
        return false;
    const auto delay = delayForPitch(sampleRate_, midiPitch);
    if (delay < 1)
        return false;
    delay_ = static_cast<std::size_t>(delay);
    return true;
}

void PitchedDelay::reset() noexcept
{
    std::ranges::fill(line_, 0.0);
    delay_ = startDelay_;
    pointer_ = 0;
}

bool PitchedDelay::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (line_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    // domulti for one channel with the prescale at 1. Its read and write
    // pointers stay together: each frame reads the line before writing
    // there. CDP8's build fuses the output's first product into a
    // multiply-add; the line's feedback is rounded before the input joins
    // it.
    auto* delbuf = line_.data();
    auto pointer = pointer_;
    const auto delay = delay_;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const double inputs = static_cast<float>(input[frame]);
        const auto outputs = std::fma(inputs, feedForward_, delbuf[pointer] * gain_);
        delbuf[pointer] = delbuf[pointer] * feedback_;
        delbuf[pointer] += inputs;
        // CDP would scale the whole file where this passes 1.
        output[frame] = std::clamp(static_cast<double>(static_cast<float>(outputs)), -1.0, 1.0);
        if (++pointer >= delay)
            pointer -= delay;
    }
    pointer_ = pointer;
    return true;
}
}
