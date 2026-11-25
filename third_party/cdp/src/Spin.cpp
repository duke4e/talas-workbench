/*
 * Derived from the CDP System, Release 8: dev/science/spin.c and
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
#include "talas/cdp/Spin.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR, PI and TWOPI; spin.c's ROOT2.
constexpr double FLTERR = 0.000002;
constexpr double PI = 3.141592654;
constexpr double TWOPI = 2.0 * PI;
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

// spin.c's pancalc between the speakers, as CDP8's build fuses it.
void pancalc(double position, double& leftgain, double& rightgain) noexcept
{
    const auto relpos = position < 0.0 ? -position : position;
    auto temp = std::fma(relpos, relpos, 1.0);
    const auto reldist = ROOT2 / std::sqrt(temp);
    temp = (position + 1.0) / 2.0;
    rightgain = temp * reldist;
    leftgain = (1.0 - temp) * reldist;
}

// output += value * gain, fused, rounded to float.
void add(float& output, double value, double gain) noexcept
{
    output = static_cast<float>(std::fma(value, gain, static_cast<double>(output)));
}
}

bool Spin::prepare(double sampleRate, Mode mode, double rate, double boost, double attenuation, int channels,
    int centre, double centreMinimum, double centreMaximum, bool startFlipped)
{
    if ((mode != Mode::Stereo && mode != Mode::Wide && mode != Mode::WideBareCentre) || !std::isfinite(sampleRate)
        || sampleRate <= 0.0 || !inRange(rate, minimumRate, maximumRate)
        || !inRange(boost, 0.0, maximumBoost) || !inRange(attenuation, 0.0, 1.0))
    {
        return false;
    }
    std::size_t outputs = 2;
    std::size_t lchan = 0;
    std::size_t rchan = 1;
    std::size_t cchan = 0;
    if (mode != Mode::Stereo)
    {
        if (channels < minimumChannels || channels > maximumChannels || centre < 1 || centre > channels
            || !inRange(centreMinimum, 0.0, 1.0))
        {
            return false;
        }
        if (mode == Mode::Wide && (!inRange(centreMaximum, 0.0, 1.0) || centreMinimum > centreMaximum))
            return false;
        // spinwdopl: the channels either side of Centre, wrapping round.
        auto l = centre - 1;
        if (l < 1)
            l += channels;
        auto r = centre + 1;
        if (r > channels)
            r -= channels;
        outputs = static_cast<std::size_t>(channels);
        lchan = static_cast<std::size_t>(l - 1);
        rchan = static_cast<std::size_t>(r - 1);
        cchan = static_cast<std::size_t>(centre - 1);
    }
    mode_ = mode;
    sampleRate_ = sampleRate;
    startRate_ = rate;
    boost_ = boost;
    attenuation_ = attenuation;
    centreMinimum_ = mode == Mode::Stereo ? 0.0 : centreMinimum;
    centreRange_ = mode == Mode::Wide ? centreMaximum - centreMinimum : 0.0;
    channels_ = outputs;
    startFlipped_ = mode == Mode::Stereo && startFlipped;
    leftChannel_ = lchan;
    rightChannel_ = rchan;
    centreChannel_ = cchan;
    reset();
    return true;
}

bool Spin::setRate(double rate) noexcept
{
    if (channels_ == 0 || !inRange(rate, minimumRate, maximumRate))
        return false;
    rate_ = rate;
    return true;
}

void Spin::reset() noexcept
{
    rate_ = startRate_;
    started_ = false;
    ch1pos_ = 0.0;
    lastspin_ = 0.0;
    // spindopl keeps its level pass's flag; spinwdopl and spinwdopl2 clear
    // it.
    flipped_ = startFlipped_ ? 1 : 0;
    movingforward_ = 1;
    held_ = false;
    last_ = {};
}

void Spin::advance(double rate) noexcept
{
    // calcgains: the phase, and which edge passes in front.
    const auto cycleincr = rate / sampleRate_;
    ch1pos_ += cycleincr;
    if (cycleincr > 0.0)
    {
        if (lastspin_ < 0.0)
            flipped_ = !flipped_;
        if (!flipped_)
        {
            if (ch1pos_ > 0.5)
            {
                movingforward_ = -movingforward_;
                flipped_ = 1;
            }
        }
        if (ch1pos_ > 1.0)
        {
            flipped_ = 0;
            movingforward_ = -movingforward_;
            ch1pos_ -= 1.0;
        }
        lastspin_ = rate;
    }
    else if (cycleincr < 0.0)
    {
        if (lastspin_ > 0.0)
            flipped_ = !flipped_;
        if (!flipped_)
        {
            if (ch1pos_ < 0.5)
            {
                movingforward_ = -movingforward_;
                flipped_ = 1;
            }
        }
        if (ch1pos_ < 0.0)
        {
            flipped_ = 0;
            movingforward_ = -movingforward_;
            ch1pos_ += 1.0;
        }
        lastspin_ = rate;
    }
}

Spin::Frame Spin::gains(float left, float right) noexcept
{
    if (!started_)
    {
        // spindopl: the first Rate sets the direction.
        movingforward_ = rate_ >= 0.0 ? 1 : -1;
        lastspin_ = rate_;
        started_ = true;
    }
    advance(rate_);
    const auto pos = -std::cos(ch1pos_ * TWOPI);
    const auto away = 1.0 - std::fabs(pos);
    Frame f;
    f.left = left;
    f.right = right;
    if (mode_ != Mode::WideBareCentre)
    {
        auto leftgain = 0.0;
        auto rightgain = 0.0;
        pancalc(pos, leftgain, rightgain);
        if (attenuation_ > 0.0)
        {
            auto atten = away * attenuation_;
            atten = 1.0 - atten;
            leftgain *= atten;
            rightgain *= atten;
        }
        f.lleftgain = leftgain;
        f.lrightgain = rightgain;
        f.rleftgain = rightgain;
        f.rrightgain = leftgain;
        if (boost_ > 0.0)
        {
            auto boost = away * boost_;
            boost += 1.0;
            if (movingforward_ < 0)
            {
                f.lleftgain *= boost;
                f.lrightgain *= boost;
                f.rleftgain /= boost;
                f.rrightgain /= boost;
            }
            else
            {
                f.lleftgain /= boost;
                f.lrightgain /= boost;
                f.rleftgain *= boost;
                f.rrightgain *= boost;
            }
        }
        // spinwdopl: the centre's level for the mono mix.
        auto boost = away * centreRange_;
        boost += centreMinimum_;
        f.centre = boost;
    }
    else
    {
        // calcgains2: across left, centre and right.
        auto leftgain = 0.0;
        auto centregain = 0.0;
        auto rightgain = 0.0;
        auto lrcpos = pos * 2.0;
        if (pos <= 0.0)
        {
            lrcpos += 1.0;
            pancalc(lrcpos, leftgain, centregain);
        }
        else
        {
            lrcpos -= 1.0;
            pancalc(lrcpos, centregain, rightgain);
        }
        f.lleftgain = leftgain;
        f.lcentregain = centregain;
        f.lrightgain = rightgain;
        f.rleftgain = rightgain;
        f.rcentregain = centregain;
        f.rrightgain = leftgain;
        if (boost_ > 0.0)
        {
            auto boost = away * boost_;
            boost += 1.0;
            if (movingforward_ < 0)
            {
                f.lcentregain *= boost;
                f.rcentregain /= boost;
            }
            else
            {
                f.lcentregain /= boost;
                f.rcentregain *= boost;
            }
        }
    }
    return f;
}

void Spin::render(const Frame& f, float nextLeft, float nextRight, double* out) const noexcept
{
    // spindopl: each edge's read point interpolates towards the next frame
    // with a fraction of 0 (the difference in float).
    const auto lval = std::fma(static_cast<double>(nextLeft - f.left), 0.0, static_cast<double>(f.left));
    const auto rval = std::fma(static_cast<double>(nextRight - f.right), 0.0, static_cast<double>(f.right));
    float o1 = 0.0F;
    float o2 = 0.0F;
    add(o1, lval, f.lleftgain);
    add(o2, lval, f.lrightgain);
    add(o1, rval, f.rleftgain);
    add(o2, rval, f.rrightgain);
    std::fill(out, out + channels_, 0.0);
    // CDP would scale the whole output where these pass 0.95.
    out[leftChannel_] = std::clamp(static_cast<double>(o1), -1.0, 1.0);
    out[rightChannel_] = std::clamp(static_cast<double>(o2), -1.0, 1.0);
    if (mode_ == Mode::Stereo)
        return;
    float oc = 0.0F;
    if (mode_ == Mode::Wide)
    {
        // spinwdopl: the centre takes the mono mix.
        oc = static_cast<float>((lval + rval) * f.centre);
    }
    else
    {
        // spinwdopl2: the centre, lowered by Attenuation, plus the mono mix
        // at Centre minimum.
        const auto atten = 1.0 - attenuation_;
        oc = static_cast<float>(std::fma(lval, f.lcentregain, rval * f.rcentregain) * atten);
        add(oc, lval + rval, centreMinimum_);
    }
    out[centreChannel_] = std::clamp(static_cast<double>(oc), -1.0, 1.0);
}

bool Spin::process(std::span<const double> left, std::span<const double> right, std::span<double> output) noexcept
{
    const auto finite = [](double sample) {
        return std::isfinite(sample);
    };
    if (channels_ == 0 || left.size() != right.size() || output.size() != left.size() * channels_
        || !std::ranges::all_of(left, finite) || !std::ranges::all_of(right, finite))
    {
        return false;
    }
    const auto ch = channels_;
    for (std::size_t n = 0; n < left.size(); n++)
    {
        const auto l = static_cast<float>(left[n]);
        const auto r = static_cast<float>(right[n]);
        auto* out = output.data() + n * ch;
        if (mode_ == Mode::Stereo)
        {
            // The next frame only sets a zero's sign, which mode 1 does not
            // show.
            render(gains(l, r), l, r, out);
            continue;
        }
        if (held_)
            render(last_, l, r, out);
        else
            std::fill(out, out + ch, 0.0);
        last_ = gains(l, r);
        held_ = true;
    }
    return true;
}
}
