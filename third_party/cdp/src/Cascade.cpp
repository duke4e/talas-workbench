/*
 * Derived from the CDP System, Release 8: dev/science/cascade.c,
 * dev/sfsys/osbind.c and dev/cdp2k/readdata.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Cascade.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR and MS_TO_SECS; cascade.c's CASC_SPLICELEN, MINECHO,
// CASPANCURVE and ROOT2.
constexpr double FLTERR = 0.000002;
constexpr double MS_TO_SECS = 0.001;
constexpr int CASC_SPLICELEN = 5;
constexpr double MINECHO = 0.01;
constexpr double CASPANCURVE = 0.4;
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

// cascade.c's pancalc between the speakers, as CDP8's build fuses it.
void pancalc(double position, double& leftgain, double& rightgain) noexcept
{
    const auto relpos = position < 0.0 ? -position : position;
    auto temp = std::fma(relpos, relpos, 1.0);
    const auto reldist = ROOT2 / std::sqrt(temp);
    temp = (position + 1.0) / 2.0;
    rightgain = temp * reldist;
    leftgain = (1.0 - temp) * reldist;
}
}

bool Cascade::prepare(double sampleRate, const Settings& settings)
{
    auto s = settings;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || s.mode < Mode::Echoes || s.mode > Mode::RotateBack
        || !inRange(s.clipSize, 0.005, 60.0) || s.echoes < 1 || s.echoes > 64 || !inRange(s.clipMax, 0.0, 60.0)
        || (s.clipMax > 0.0 && s.clipMax < 0.005) || s.echoesMax < 0 || s.echoesMax > 64
        || !inRange(s.rand, 0.0, 1.0) || s.seed < 0 || s.seed > 64)
    {
        return false;
    }
    const bool draws = s.clipMax > 0.0 || s.echoesMax > 0 || s.rand > 0.0;
    if (draws && s.seed == 0)
        return false;
    // cascade: the splice and clip sizes in frames.
    const auto spliclen = std::lround(CASC_SPLICELEN * MS_TO_SECS * sampleRate);
    const auto clip = std::lround(s.clipSize * sampleRate);
    const auto clipMax = s.clipMax == 0.0 ? 0L : std::lround(s.clipMax * sampleRate);
    const auto longest = static_cast<std::size_t>(std::max(clip, clipMax) + spliclen);
    const auto mostEchoes = static_cast<std::size_t>(std::max(s.echoes, s.echoesMax));
    const auto set = (mostEchoes + 1) * longest + 2;
    // A clip after the first is read a splice later in the input than it
    // is placed (CDP seeks its file before backing up by the splice).
    const auto latency = longest + static_cast<std::size_t>(spliclen) - 1;
    if (spliclen < 1 || latency + set > maximumFrames)
        return false;
    const std::size_t channels
        = s.mode == Mode::Echoes ? 1 : (s.mode == Mode::Rotate || s.mode == Mode::RotateBack ? 8 : 2);

    const auto ringFrames = std::bit_ceil(longest + 2);
    const auto outFrames = std::bit_ceil(latency + set + 2);
    ring_.assign(ringFrames, 0.0F);
    ringMask_ = ringFrames - 1;
    out_.assign(outFrames * channels, 0.0F);
    outMask_ = outFrames - 1;
    echobuf_.assign(set, 0.0F);
    settings_ = s;
    channels_ = channels;
    latency_ = latency;
    spliclen_ = spliclen;
    clip_ = clip;
    clipMax_ = clipMax;
    reset();
    return true;
}

void Cascade::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    std::ranges::fill(out_, 0.0F);
    // initialise_cascade_random_sequence: srand(seed), which CDP's drand48
    // (rand() / RAND_MAX) follows.
    random_ = {};
    random_.generator.seed(static_cast<unsigned long>(settings_.seed));
    frame_ = 0;
    clipStart_ = 0;
    firstClip_ = true;
    panright_ = 1;
    drawClip();
}

void Cascade::drawClip() noexcept
{
    // cascade: each clip's size and echo count, drawn in this order.
    auto cliplen = clip_;
    if (clipMax_ != 0)
    {
        const auto diff = static_cast<double>(clipMax_ - clip_);
        const auto rval = static_cast<long>(std::floor(random_.generator.next() * diff));
        cliplen = clip_ + rval;
    }
    if (!firstClip_)
        cliplen += spliclen_;
    auto echocnt = settings_.echoes;
    if (settings_.echoesMax != 0)
    {
        const auto diff = static_cast<double>(settings_.echoesMax - settings_.echoes);
        const auto rval = static_cast<int>(std::floor(random_.generator.next() * diff));
        echocnt = settings_.echoes + rval;
    }
    cliplen_ = cliplen;
    echocnt_ = echocnt;
}

void Cascade::computeClip() noexcept
{
    const auto len = cliplen_;
    const auto s = spliclen_;
    const auto S = static_cast<long>(clipStart_);
    const auto echocnt = echocnt_;
    const auto rand = settings_.rand;
    const auto randrange = static_cast<double>(len - s - 1);
    const auto atten = settings_.linear ? (1.0 - MINECHO) / static_cast<double>(echocnt)
                                        : std::pow(MINECHO, 1.0 / static_cast<double>(echocnt));
    const auto splicendstt = len - s;
    const auto splice = [&](long n) noexcept {
        if (n < s)
            return static_cast<double>(n) / static_cast<double>(s);
        if (n >= splicendstt)
            return static_cast<double>(len - n - 1) / static_cast<double>(s);
        return 1.0;
    };
    const auto R = static_cast<long>(readStart());
    const auto ibuf = [&](long n) noexcept {
        return static_cast<double>(input(static_cast<std::size_t>(R + n)));
    };
    auto* echobuf = echobuf_.data();
    auto level = 1.0;
    const auto next = [&]() noexcept {
        if (settings_.linear)
            level -= atten;
        else
            level *= atten;
    };
    const auto drawOffset = [&]() noexcept {
        auto offset = std::fma(random_.generator.next(), 2.0, -1.0);
        offset *= rand;
        offset *= randrange;
        return static_cast<long>(std::floor(offset));
    };
    const auto mode = settings_.mode;
    if (mode == Mode::Echoes || mode == Mode::PanRight || mode == Mode::Alternate)
    {
        std::fill(echobuf, echobuf + echobuf_.size(), 0.0F);
        long ebufpos = 0;
        long trueGp = 0;
        for (int e = 0; e <= echocnt; e++)
        {
            for (long n = 0; n < len; n++, ebufpos++)
            {
                auto& slot = echobuf[ebufpos];
                slot = static_cast<float>(std::fma(ibuf(n) * level, splice(n), static_cast<double>(slot)));
            }
            if (rand > 0.0 && e < echocnt)
            {
                trueGp += len;
                ebufpos = e < echocnt - 1 ? trueGp + drawOffset() : trueGp;
            }
            ebufpos -= s;
            next();
        }
        ebufpos += s;
        if (mode == Mode::Echoes)
        {
            for (long n = 0; n < ebufpos; n++)
            {
                auto& slot = out(static_cast<std::size_t>(S + n));
                slot = slot + echobuf[n];
            }
            return;
        }
        const auto panfactor = static_cast<double>(ebufpos - 1);
        for (long n = 0; n < ebufpos; n++)
        {
            auto pan = static_cast<double>(n) / panfactor;
            pan = std::pow(pan, CASPANCURVE);
            if (mode == Mode::PanRight)
                pan = std::fma(pan, 2.0, -1.0);
            else if (panright_ < 0)
                pan *= -1.0;
            auto leftgain = 0.0;
            auto rightgain = 0.0;
            pancalc(pan, leftgain, rightgain);
            const auto echo = static_cast<double>(echobuf[n]);
            auto& l = out(static_cast<std::size_t>((S + n) * 2));
            l = static_cast<float>(std::fma(echo, leftgain, static_cast<double>(l)));
            auto& r = out(static_cast<std::size_t>((S + n) * 2 + 1));
            r = static_cast<float>(std::fma(echo, rightgain, static_cast<double>(r)));
        }
        if (mode == Mode::Alternate)
            panright_ = -panright_;
        return;
    }
    // Mono to 8 channels: each echo written as it is made, one channel on.
    constexpr long outchans = 8;
    auto obufpos = S * outchans;
    auto trueGp = S;
    for (int e = 0; e <= echocnt; e++)
    {
        for (long n = 0; n < len; n++)
            echobuf[n] = static_cast<float>(ibuf(n) * level * splice(n));
        for (long n = 0; n < len; n++)
        {
            auto& slot = out(static_cast<std::size_t>(obufpos));
            slot = slot + echobuf[n];
            obufpos += outchans;
        }
        if (rand > 0.0)
        {
            const auto thischan = obufpos % 8;
            trueGp += len - s;
            obufpos = e < echocnt - 1 ? trueGp + drawOffset() : trueGp;
            obufpos *= outchans;
            obufpos += thischan;
        }
        if (e < echocnt)
            obufpos += panright_ > 0 ? 1 : -1;
        next();
    }
    if (mode == Mode::RotateBack)
        panright_ = -panright_;
}

bool Cascade::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (channels_ == 0 || output.size() != input.size() * channels_
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    const auto ch = channels_;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        const auto f = frame_;
        ring_[f & ringMask_] = static_cast<float>(input[n]);
        if (f + 1 == readStart() + static_cast<std::size_t>(cliplen_))
        {
            computeClip();
            // The next clip starts a splice before this one ends.
            clipStart_ += static_cast<std::size_t>(cliplen_ - spliclen_);
            firstClip_ = false;
            drawClip();
        }
        for (std::size_t c = 0; c < ch; c++)
        {
            auto value = 0.0;
            if (f >= latency_)
            {
                auto& slot = out_[((f - latency_) & outMask_) * ch + c];
                value = static_cast<double>(slot);
                slot = 0.0F;
            }
            // CDP would scale the whole output where this passes 0.95.
            output[n * ch + c] = std::clamp(value, -1.0, 1.0);
        }
        frame_ = f + 1;
    }
    return true;
}
}
