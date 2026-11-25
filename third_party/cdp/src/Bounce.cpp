/*
 * Derived from the CDP System, Release 8: dev/science/bounce.c and
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
#include "talas/cdp/Bounce.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR and MS_TO_SECS; bounce.c's BOUNCESPLICE.
constexpr double FLTERR = 0.000002;
constexpr double MS_TO_SECS = 0.001;
constexpr int BOUNCESPLICE = 3;

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

bool Bounce::prepare(double sampleRate, int count, double startGap, double shorten, double endLevel, double warp)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || count < minimumCount || count > maximumCount
        || !inRange(startGap, minimumStartGap, maximumStartGap) || !inRange(shorten, minimumShorten, maximumShorten)
        || !inRange(endLevel, minimumEndLevel, maximumEndLevel) || !inRange(warp, minimumWarp, maximumWarp))
    {
        return false;
    }
    // bounce: the gaps and levels, for one channel.
    const auto srate = sampleRate;
    const auto gpsplen = std::lround(BOUNCESPLICE * MS_TO_SECS * srate);
    const auto minstep = gpsplen / 2;
    const auto levchange = 1.0 - endLevel;
    std::vector<long> steps;
    std::vector<double> levels;
    steps.push_back(std::lround(startGap * srate));
    levels.push_back(1.0);
    auto lastratio = startGap;
    auto levcnt = count - 1;
    for (int bouncecnt = 1; bouncecnt <= count; bouncecnt++, levcnt--)
    {
        auto levpos = static_cast<double>(levcnt) / static_cast<double>(count);
        levpos = std::pow(levpos, warp);
        auto value = levchange * levpos;
        value += endLevel;
        levels.push_back(value);
        const auto thisratio = lastratio * shorten;
        const auto step = std::lround(thisratio * srate);
        if (step < minstep)
            break;
        steps.push_back(step);
        lastratio = thisratio;
    }
    // Copy k starts after the gaps before it.
    std::vector<std::size_t> delays;
    std::vector<double> copyLevels;
    std::size_t at = 0;
    for (std::size_t k = 1; k < levels.size(); k++)
    {
        at += static_cast<std::size_t>(steps[k - 1]);
        if (at > maximumDelayFrames)
            return false;
        delays.push_back(at);
        copyLevels.push_back(levels[k]);
    }
    if (delays.empty())
        return false;
    const auto frames = std::bit_ceil(delays.back() + 1);
    line_.assign(frames, 0.0F);
    mask_ = frames - 1;
    delays_ = std::move(delays);
    levels_ = std::move(copyLevels);
    frame_ = 0;
    return true;
}

void Bounce::reset() noexcept
{
    std::ranges::fill(line_, 0.0F);
    frame_ = 0;
}

bool Bounce::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (line_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    const auto count = delays_.size();
    for (std::size_t n = 0; n < input.size(); n++)
    {
        const auto f = frame_;
        const auto sample = static_cast<float>(input[n]);
        line_[f & mask_] = sample;
        // The source, then each copy added in turn.
        auto obuf = sample;
        for (std::size_t k = 0; k < count && delays_[k] <= f; k++)
        {
            const auto ibuf = static_cast<double>(line_[(f - delays_[k]) & mask_]);
            obuf = static_cast<float>(std::fma(ibuf, levels_[k], static_cast<double>(obuf)));
        }
        // CDP would fade the whole output down where this passes 0.95.
        output[n] = std::clamp(static_cast<double>(obuf), -1.0, 1.0);
        frame_ = f + 1;
    }
    return true;
}
}
