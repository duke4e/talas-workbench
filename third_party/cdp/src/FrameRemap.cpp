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
#include "talas/cdp/FrameRemap.h"

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

[[nodiscard]] bool validFrame(double sampleRate, int channels) noexcept
{
    return std::isfinite(sampleRate) && sampleRate > 0.0 && channels >= FrameRemap::minimumChannels
        && channels <= FrameRemap::maximumChannels;
}
}

bool FrameRemap::accept(double sampleRate, int channels, const Map& map) noexcept
{
    if (!validFrame(sampleRate, channels))
        return false;
    channels_ = static_cast<std::size_t>(channels);
    map_ = map;
    return true;
}

// read_the_special_data and reorient_test: every channel once.
bool FrameRemap::prepareReorient(double sampleRate, int channels, std::span<const int> route)
{
    if (!validFrame(sampleRate, channels) || route.size() != static_cast<std::size_t>(channels))
        return false;
    Map map{};
    std::array<bool, maximumChannels> seen{};
    for (std::size_t n = 0; n < route.size(); n++)
    {
        const auto c = route[n];
        if (c < 1 || c > channels || seen[static_cast<std::size_t>(c - 1)])
            return false;
        seen[static_cast<std::size_t>(c - 1)] = true;
        map[n] = static_cast<std::size_t>(c - 1);
    }
    return accept(sampleRate, channels, map);
}

// check_the_param_validity_and_consistency and mirror.
bool FrameRemap::prepareMirror(double sampleRate, int channels, double mirrorPlane)
{
    if (!validFrame(sampleRate, channels) || !inRange(mirrorPlane, 0.0, maximumMirrorPlane))
        return false;
    const auto k = static_cast<int>(std::floor(mirrorPlane));
    if (k > channels)
        return false;
    if (k != mirrorPlane && !flteq(mirrorPlane - static_cast<double>(k), 0.5))
        return false;
    const auto outchans = channels;
    Map mirrormap{};
    auto mirrorplane = static_cast<int>(std::lround(mirrorPlane * 2.0));
    if (mirrorplane % 2 == 0)
        mirrorplane = static_cast<int>(std::lround(mirrorPlane));
    else
        mirrorplane = -(mirrorplane / 2);
    if (mirrorplane > 0)
    {
        mirrorplane--;
        for (int n = 0; n < outchans; n++)
        {
            auto ichan = mirrorplane + n;
            while (ichan >= outchans)
                ichan -= outchans;
            auto ochan = mirrorplane - n;
            while (ochan < 0)
                ochan += outchans;
            mirrormap[static_cast<std::size_t>(ichan)] = static_cast<std::size_t>(ochan);
        }
    }
    else
    {
        mirrorplane = -mirrorplane;
        mirrorplane--;
        if (outchans % 2 == 0)
        {
            for (int n = 1; n <= outchans; n++)
            {
                auto ichan = mirrorplane + n;
                while (ichan >= outchans)
                    ichan -= outchans;
                auto ochan = mirrorplane - n + 1;
                while (ochan < 0)
                    ochan += outchans;
                mirrormap[static_cast<std::size_t>(ichan)] = static_cast<std::size_t>(ochan);
            }
        }
        else
        {
            const auto lastchan = outchans / 2 + 1;
            for (int n = 1; n <= lastchan; n++)
            {
                auto ichan = mirrorplane + n;
                while (ichan >= outchans)
                    ichan -= outchans;
                auto ochan = mirrorplane - n + 1;
                while (ochan < 0)
                    ochan += outchans;
                mirrormap[static_cast<std::size_t>(ichan)] = static_cast<std::size_t>(ochan);
                mirrormap[static_cast<std::size_t>(ochan)] = static_cast<std::size_t>(ichan);
            }
        }
    }
    return accept(sampleRate, channels, mirrormap);
}

// bilateral.
bool FrameRemap::prepareBilateral(double sampleRate, int channels, bool toRing)
{
    if (!validFrame(sampleRate, channels))
        return false;
    const auto outchans = channels;
    Map bi{};
    const auto split = static_cast<int>(std::ceil(static_cast<double>(outchans) / 2.0));
    auto ichan = outchans - 1;
    auto ochan = 1;
    while (ichan >= split)
    {
        if (toRing)
            bi[static_cast<std::size_t>(ochan)] = static_cast<std::size_t>(ichan);
        else
            bi[static_cast<std::size_t>(ichan)] = static_cast<std::size_t>(ochan);
        ichan--;
        ochan += 2;
    }
    while (ichan >= 0)
    {
        if (toRing)
            bi[static_cast<std::size_t>(ichan * 2)] = static_cast<std::size_t>(ichan);
        else
            bi[static_cast<std::size_t>(ichan)] = static_cast<std::size_t>(ichan * 2);
        ichan--;
    }
    return accept(sampleRate, channels, bi);
}

// check_the_param_validity_and_consistency and swapchans.
bool FrameRemap::prepareSwap(double sampleRate, int channels, int a, int b)
{
    if (!validFrame(sampleRate, channels) || a < 1 || b < 1 || a > channels || b > channels || a == b)
        return false;
    Map map{};
    for (std::size_t n = 0; n < map.size(); n++)
        map[n] = n;
    map[static_cast<std::size_t>(a - 1)] = static_cast<std::size_t>(b - 1);
    map[static_cast<std::size_t>(b - 1)] = static_cast<std::size_t>(a - 1);
    return accept(sampleRate, channels, map);
}

// beast_bilateral.
bool FrameRemap::prepareBeastBilateral(double sampleRate, int channels, bool toRing)
{
    if (!validFrame(sampleRate, channels))
        return false;
    const auto outchans = channels;
    Map bi{};
    const auto at = [](int n) { return static_cast<std::size_t>(n); };
    auto even = true;
    auto lastevenchanno = 0;
    auto lastoddchanno = 0;
    if ((outchans / 2) * 2 != outchans)
    {
        even = false;
        lastevenchanno = outchans - 1;
    }
    else
    {
        lastevenchanno = outchans - 2;
        lastoddchanno = outchans - 1;
    }
    if (toRing)
        bi[at(lastevenchanno)] = 0;
    else
        bi[0] = at(lastevenchanno);
    if (even)
    {
        if (toRing)
            bi[at(lastoddchanno)] = at(outchans / 2);
        else
            bi[at(outchans / 2)] = at(lastoddchanno);
    }
    auto limit = outchans / 2;
    if (!even)
        limit++;
    auto ichan = 1;
    while (ichan < limit)
    {
        if (toRing)
            bi[at(ichan * 2 - 1)] = at(ichan);
        else
            bi[at(ichan)] = at(ichan * 2 - 1);
        ichan++;
    }
    auto ochan = 0;
    ichan = outchans - 1;
    limit = outchans / 2;
    while (ichan > limit)
    {
        if (toRing)
            bi[at(ochan)] = at(ichan);
        else
            bi[at(ichan)] = at(ochan);
        ichan--;
        ochan += 2;
    }
    return accept(sampleRate, channels, bi);
}

bool FrameRemap::process(std::span<const double> input, std::span<double> output) noexcept
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
        std::array<double, maximumChannels> obuf{};
        for (std::size_t m = 0; m < channels_; m++)
            obuf[map_[m]] = static_cast<double>(static_cast<float>(in[m]));
        for (std::size_t c = 0; c < channels_; c++)
            out[c] = obuf[c];
    }
    return true;
}
}
