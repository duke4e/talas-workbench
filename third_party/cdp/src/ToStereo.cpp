/*
 * Derived from the CDP System, Release 8: dev/standnew/tostereo.c and
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
#include "talas/cdp/ToStereo.h"

#include <cmath>
#include <utility>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR; tostereo.c's default mix levels.
constexpr double FLTERR = 0.000002;
constexpr double STEREO_DEFAULT_MIXLEVEL = 0.5;
constexpr double FORK_DEFAULT_MIXLEVEL = 0.707;

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

// tklib3.c's read_value_from_brktable over a 4-point line.
[[nodiscard]] double readBrk(const std::array<double, 8>& brk, double thistime) noexcept
{
    if (thistime <= brk[0])
        return brk[1];
    if (thistime >= brk[6])
        return brk[7];
    std::size_t p = 0;
    while (brk[p] < thistime)
        p += 2;
    const auto hival = brk[p + 1];
    const auto hiind = brk[p];
    const auto loval = brk[p - 1];
    const auto loind = brk[p - 2];
    auto val = (thistime - loind) / (hiind - loind);
    val *= (hival - loval);
    val += loval;
    return val;
}
}

bool ToStereo::prepare(double sampleRate, double start, double end, int outputChannels, int leftChannel,
    int rightChannel, double mixLevel)
{
    if (!std::isfinite(sampleRate) || !(sampleRate > 0.0))
        return false;
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0.0 || end < 0.0)
        return false;
    if (outputChannels < minimumChannels || outputChannels > maximumChannels)
        return false;
    if (leftChannel < 0 || leftChannel > maximumChannels || rightChannel < 0 || rightChannel > maximumChannels)
        return false;
    if (!inRange(mixLevel, 0.0, 1.0))
        return false;
    // check_tostereo_param_validity_and_consistency.
    if (flteq(start, end))
        return false;
    auto converges = false;
    if (start > end)
    {
        converges = true;
        std::swap(start, end);
    }
    auto isset = false;
    auto fork = -1;
    auto lchan = leftChannel;
    auto rchan = rightChannel;
    const auto ochans = outputChannels;
    if (rchan == 0 && lchan == 0 && ochans == 2)
    {
        lchan = 1;
        rchan = 2;
        isset = true;
    }
    if (!isset)
    {
        if (lchan > ochans || rchan > ochans || lchan == 0)
            return false;
        if (rchan == 0)
        {
            if (ochans == 2)
                return false;
            fork = lchan;
            rchan = fork + 1;
            if (rchan > ochans)
                rchan -= ochans;
            lchan = fork - 1;
            if (lchan <= 0)
                lchan += ochans;
        }
        if (rchan == lchan)
            return false;
    }
    if (mixLevel > 0.0 && mixLevel < 0.5)
        return false;
    if (mixLevel == 0)
        mixLevel = fork > 0 ? FORK_DEFAULT_MIXLEVEL : STEREO_DEFAULT_MIXLEVEL;
    if (fork > 0)
        fork--;

    // tostereo's lines.
    std::array<double, 8> ll{};
    std::array<double, 8> lr{};
    if (fork >= 0)
    {
        if (converges)
        {
            ll = {0.0, 1.0, start, 1.0, end, 0.0, 10000.0, 0.0};
            lr = {0.0, 0.0, start, 0.0, end, mixLevel, 10000.0, mixLevel};
        }
        else
        {
            ll = {0.0, 0.0, start, 0.0, end, 1.0, 10000.0, 1.0};
            lr = {0.0, mixLevel, start, mixLevel, end, 0.0, 10000.0, 0.0};
        }
    }
    else
    {
        if (converges)
            ll = {0.0, 1.0, start, 1.0, end, 0.5, 10000.0, 0.5};
        else
            ll = {0.0, 0.5, start, 0.5, end, 1.0, 10000.0, 1.0};
        for (std::size_t t = 0, v = 1; t < 8; t += 2, v += 2)
        {
            lr[t] = ll[t];
            lr[v] = 1.0 - ll[v];
        }
    }
    sampleRate_ = sampleRate;
    channels_ = static_cast<std::size_t>(ochans);
    start_ = start;
    timestep_ = end - start;
    converges_ = converges;
    fork_ = fork;
    left_ = static_cast<std::size_t>(lchan - 1);
    right_ = static_cast<std::size_t>(rchan - 1);
    mixLevel_ = mixLevel;
    ll_ = ll;
    lr_ = lr;
    reset();
    return true;
}

void ToStereo::reset() noexcept
{
    frame_ = 0;
}

bool ToStereo::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() % 2 != 0 || output.size() != input.size() / 2 * channels_)
        return false;
    for (const auto x : input)
    {
        if (!std::isfinite(x))
            return false;
    }
    const auto frames = input.size() / 2;
    for (std::size_t i = 0; i < frames; i++)
    {
        const auto l = static_cast<float>(input[2 * i]);
        const auto r = static_cast<float>(input[2 * i + 1]);
        const auto time = static_cast<double>(frame_) / sampleRate_;
        const auto p0 = readBrk(ll_, time);
        const auto p1 = readBrk(lr_, time);
        auto* obuf = output.data() + i * channels_;
        for (std::size_t c = 0; c < channels_; c++)
            obuf[c] = 0.0;
        if (fork_ >= 0)
        {
            obuf[left_] = static_cast<double>(static_cast<float>(static_cast<double>(l) * p0));
            obuf[right_] = static_cast<double>(static_cast<float>(static_cast<double>(r) * p0));
            const auto sum = l + r;
            obuf[static_cast<std::size_t>(fork_)] = static_cast<double>(static_cast<float>(static_cast<double>(sum) * p1));
        }
        else
        {
            const auto llval = static_cast<double>(l) * p0;
            const auto lrval = static_cast<double>(l) * p1;
            const auto rrval = static_cast<double>(r) * p0;
            const auto rlval = static_cast<double>(r) * p1;
            const auto tpos = time - start_;
            auto tratio = tpos / timestep_;
            // sfsys.h's max and min.
            tratio = tratio > 0.0 ? tratio : 0.0;
            tratio = tratio > 1.0 ? 1.0 : tratio;
            double levboost = 0.0;
            if (converges_)
                levboost = std::fma(mixLevel_, 2.0, -1.0) * tratio;
            else
                levboost = std::fma(mixLevel_, 2.0, -1.0) * (1.0 - tratio);
            levboost += 1.0;
            obuf[left_] = static_cast<double>(static_cast<float>((llval + rlval) * levboost));
            obuf[right_] = static_cast<double>(static_cast<float>((rrval + lrval) * levboost));
        }
        frame_++;
    }
    return true;
}
}
