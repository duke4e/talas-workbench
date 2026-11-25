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
#include "talas/cdp/FrameRotate.h"

#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR; frame.c's MAX_ROT and ROOT2.
constexpr double FLTERR = 0.000002;
constexpr double MAX_ROT = 500.0;
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

// set_stereo_levels, as CDP8's build fuses it.
void setStereoLevels(double pos, double& llevel, double& rlevel) noexcept
{
    const auto zerocentredposition = std::fma(pos, 2.0, -1.0);
    const auto relpos = zerocentredposition < 0 ? -zerocentredposition : zerocentredposition;
    const auto temp = std::fma(relpos, relpos, 1.0);
    const auto holecompensate = ROOT2 / std::sqrt(temp);
    rlevel = pos * holecompensate;
    llevel = (1.0 - pos) * holecompensate;
}

// do_smear, as CDP8's build fuses it.
void doSmear(int loutchan, int routchan, double lsig, double rsig, int chans, float* obuf, double smear) noexcept
{
    int smearleft = loutchan - 1;
    if (smearleft < 0)
        smearleft += chans;
    int smearright = loutchan + 1;
    if (smearright >= chans)
        smearright -= chans;
    obuf[smearleft] = static_cast<float>(std::fma(lsig, smear, static_cast<double>(obuf[smearleft])));
    obuf[smearright] = static_cast<float>(std::fma(lsig, smear, static_cast<double>(obuf[smearright])));
    smearleft = routchan - 1;
    if (smearleft < 0)
        smearleft += chans;
    smearright = routchan + 1;
    if (smearright >= chans)
        smearright -= chans;
    obuf[smearleft] = static_cast<float>(std::fma(rsig, smear, static_cast<double>(obuf[smearleft])));
    obuf[smearright] = static_cast<float>(std::fma(rsig, smear, static_cast<double>(obuf[smearright])));
}
}

std::size_t FrameRotate::readFrames(double sampleRate) noexcept
{
    if (!std::isfinite(sampleRate) || !(sampleRate >= minimumSampleRate))
        return 0;
    return static_cast<std::size_t>(std::lround(sampleRate / MAX_ROT));
}

bool FrameRotate::route(std::span<const int> snake, Route& out) const noexcept
{
    if (snake.size() != channels_)
        return false;
    const auto chans = static_cast<int>(channels_);
    Route r{};
    std::array<bool, maximumChannels> seen{};
    for (std::size_t n = 0; n < channels_; n++)
    {
        const auto c = snake[n];
        if (c < 1 || c > chans || seen[static_cast<std::size_t>(c - 1)])
            return false;
        seen[static_cast<std::size_t>(c - 1)] = true;
        r[n] = c - 1;
    }
    out = r;
    return true;
}

bool FrameRotate::prepare(
    double sampleRate, int channels, double rotation, double smear, int firstDirection, std::span<const int> snake)
{
    return setup(sampleRate, channels, false, rotation, 0.0, smear, firstDirection, 0, snake);
}

bool FrameRotate::prepareDual(double sampleRate, int channels, double rotation, double secondRotation, double smear,
    int firstDirection, int secondFirstDirection, std::span<const int> snake)
{
    return setup(
        sampleRate, channels, true, rotation, secondRotation, smear, firstDirection, secondFirstDirection, snake);
}

bool FrameRotate::setup(double sampleRate, int channels, bool dual, double rotation, double secondRotation,
    double smear, int firstDirection, int secondFirstDirection, std::span<const int> snake)
{
    const auto click = readFrames(sampleRate);
    if (click == 0)
        return false;
    if (channels < minimumChannels || channels > maximumChannels || (dual && channels % 2 != 0))
        return false;
    if (!inRange(rotation, -MAX_ROT, MAX_ROT) || !inRange(secondRotation, -MAX_ROT, MAX_ROT)
        || !inRange(smear, 0.0, maximumSmear))
        return false;
    if (firstDirection < -1 || firstDirection > 1 || secondFirstDirection < -1 || secondFirstDirection > 1)
        return false;
    // get_first_rotation_orientation.
    const auto sign = [](double r) { return r == 0.0 ? 0 : (r > 0.0 ? 1 : -1); };
    const auto orient0 = firstDirection != 0 ? firstDirection : sign(rotation);
    const auto orient1 = dual ? (secondFirstDirection != 0 ? secondFirstDirection : sign(secondRotation)) : 0;
    if (dual && orient0 == 0 && orient1 == 0)
        return false;
    FrameRotate next;
    next.channels_ = static_cast<std::size_t>(channels);
    Route r{};
    for (std::size_t n = 0; n < r.size(); n++)
        r[n] = static_cast<int>(n);
    if (!snake.empty() && !next.route(snake, r))
        return false;
    next.sampleRate_ = sampleRate;
    next.dual_ = dual;
    next.smear_ = smear;
    next.smearcompensate_ = 1.0;
    if (smear > 0.0)
        next.smearcompensate_ = 1 - (2.0 * smear);
    next.click_ = click;
    next.startRotation_ = {rotation, dual ? secondRotation : 0.0};
    next.orient_ = {orient0, orient1};
    next.startSnake_ = r;
    *this = next;
    reset();
    return true;
}

bool FrameRotate::setRotation(double rotation) noexcept
{
    if (!prepared() || !inRange(rotation, -MAX_ROT, MAX_ROT))
        return false;
    rotation_[0] = rotation;
    return true;
}

bool FrameRotate::setSecondRotation(double rotation) noexcept
{
    if (!prepared() || !dual_ || !inRange(rotation, -MAX_ROT, MAX_ROT))
        return false;
    rotation_[1] = rotation;
    return true;
}

bool FrameRotate::setSnake(std::span<const int> snake) noexcept
{
    if (!prepared())
        return false;
    Route r{};
    if (!route(snake, r))
        return false;
    snake_ = r;
    return true;
}

void FrameRotate::reset() noexcept
{
    const auto chans = static_cast<int>(channels_);
    rotation_ = startRotation_;
    snake_ = startSnake_;
    // rotate: the first steps, from the rotations at time 0.
    step_[0] = (rotation_[0] / sampleRate_) * static_cast<double>(chans);
    step_[1] = dual_ ? (rotation_[1] / sampleRate_) * static_cast<double>(chans) : 0.0;
    const auto outLspkrStep = dual_ ? 2 : 1;
    const auto setUp = [&](int first, int stride, bool clockwise, std::size_t which) {
        for (int n = first; n < chans; n += stride)
        {
            const auto k = static_cast<std::size_t>(n);
            if (clockwise)
            {
                ochanLeft_[k] = n;
                ol_[k] = ochanLeft_[k];
                ochanRight_[k] = (n + outLspkrStep) % chans;
                or_[k] = snake_[static_cast<std::size_t>(ochanRight_[k])];
            }
            else
            {
                ochanRight_[k] = n;
                or_[k] = ochanRight_[k];
                ochanLeft_[k] = n - outLspkrStep;
                if (ochanLeft_[k] < 0)
                    ochanLeft_[k] += chans;
                ol_[k] = snake_[static_cast<std::size_t>(ochanLeft_[k])];
            }
        }
        position_[which] = clockwise ? -step_[which] : step_[which];
    };
    if (!dual_)
    {
        setUp(0, 1, orient_[0] >= 0, 0);
        position_[1] = 0.0;
    }
    else
    {
        setUp(0, 2, orient_[0] > 0, 0);
        setUp(1, 2, orient_[1] > 0, 1);
    }
    clickPosition_ = 0;
}

// rotate: one rotation's move, its inputs passing to the next pair.
void FrameRotate::advance(std::size_t which, int first) noexcept
{
    const auto chans = static_cast<int>(channels_);
    const auto stride = dual_ ? 2 : 1;
    const auto outLspkrStep = stride;
    auto& pos = position_[which];
    if (pos < 0.0 || pos > 1.0)
    {
        if (pos > 1.0)
        {
            for (int n = first; n < chans; n += stride)
            {
                const auto k = static_cast<std::size_t>(n);
                ochanLeft_[k] = ochanRight_[k];
                ol_[k] = ochanLeft_[k];
                ochanRight_[k] = (ochanRight_[k] + outLspkrStep) % chans;
                or_[k] = ochanRight_[k];
            }
            pos -= 1.0;
        }
        else
        {
            for (int n = first; n < chans; n += stride)
            {
                const auto k = static_cast<std::size_t>(n);
                ochanRight_[k] = ochanLeft_[k];
                or_[k] = ochanRight_[k];
                ochanLeft_[k] = ochanLeft_[k] - outLspkrStep;
                if (ochanLeft_[k] < 0)
                    ochanLeft_[k] += chans;
                ol_[k] = ochanLeft_[k];
            }
            pos += 1.0;
        }
        for (int n = first; n < chans; n += stride)
        {
            const auto k = static_cast<std::size_t>(n);
            or_[k] = snake_[static_cast<std::size_t>(or_[k])];
            ol_[k] = snake_[static_cast<std::size_t>(ol_[k])];
        }
    }
}

bool FrameRotate::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || output.size() != input.size() || input.size() % channels_ != 0)
        return false;
    for (const auto x : input)
    {
        if (!std::isfinite(x))
            return false;
    }
    const auto chans = static_cast<int>(channels_);
    const auto frames = input.size() / channels_;
    for (std::size_t i = 0; i < frames; i++)
    {
        // rotate reads the rotations as each click passes (frame 0 took
        // prepare's).
        if (clickPosition_ == click_)
        {
            clickPosition_ = 0;
            step_[0] = (rotation_[0] / sampleRate_) * static_cast<double>(chans);
            if (dual_)
                step_[1] = (rotation_[1] / sampleRate_) * static_cast<double>(chans);
        }
        clickPosition_++;
        position_[0] += step_[0];
        if (!dual_)
            advance(0, 0);
        else
        {
            position_[1] += step_[1];
            advance(0, 0);
            advance(1, 1);
        }
        std::array<double, 2> llevel{};
        std::array<double, 2> rlevel{};
        setStereoLevels(position_[0], llevel[0], rlevel[0]);
        if (dual_)
            setStereoLevels(position_[1], llevel[1], rlevel[1]);

        const auto* in = input.data() + i * channels_;
        std::array<float, maximumChannels> obuf{};
        const auto one = [&](int n, std::size_t which) {
            const auto k = static_cast<std::size_t>(n);
            const auto ibuf = static_cast<double>(static_cast<float>(in[k]));
            const auto lsig = llevel[which] * ibuf * smearcompensate_;
            obuf[static_cast<std::size_t>(ol_[k])]
                = static_cast<float>(static_cast<double>(obuf[static_cast<std::size_t>(ol_[k])]) + lsig);
            const auto rsig = rlevel[which] * ibuf * smearcompensate_;
            obuf[static_cast<std::size_t>(or_[k])]
                = static_cast<float>(static_cast<double>(obuf[static_cast<std::size_t>(or_[k])]) + rsig);
            if (smear_ > 0.0)
                doSmear(ol_[k], or_[k], lsig, rsig, chans, obuf.data(), smear_);
        };
        if (!dual_)
        {
            for (int n = 0; n < chans; n++)
                one(n, 0);
        }
        else
        {
            for (int n = 0; n < chans; n += 2)
                one(n, 0);
            for (int n = 1; n < chans; n += 2)
                one(n, 1);
        }
        auto* out = output.data() + i * channels_;
        for (std::size_t c = 0; c < channels_; c++)
            out[c] = static_cast<double>(obuf[c]);
    }
    return true;
}
}
