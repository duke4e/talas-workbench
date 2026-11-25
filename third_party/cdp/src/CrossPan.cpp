/*
 * Derived from the CDP System, Release 8: dev/standalone/mchanpan.c,
 * dev/cdp2k/readdata.c and dev/sfsys/osbind.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/CrossPan.h"

#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR; mchanpan.c's crossing types.
constexpr double FLTERR = 0.000002;
constexpr int ATCENTRE = 0;
constexpr int TOCENTRE = 1;
constexpr int FROMCENTRE = 2;
constexpr int CROSSCENTRE = 3;
constexpr int ATEDGE = 4;
// Malloc(-1)'s 1 MB, in float samples, over the input and output buffers.
constexpr std::size_t cdpBufferSamples = 1024 * 1024 / sizeof(float);

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

std::size_t CrossPan::defaultBufferFrames(int channels) noexcept
{
    if (channels < 1)
        return 0;
    return cdpBufferSamples / (2 * static_cast<std::size_t>(channels));
}

bool CrossPan::prepare(
    double sampleRate, int channels, std::span<const Point> table, double rolloff, std::size_t bufferFrames,
    bool corrected)
{
    if (!std::isfinite(sampleRate) || !(sampleRate > 0.0))
        return false;
    if (channels < minimumChannels || channels > maximumChannels || table.empty())
        return false;
    if (!inRange(rolloff, 0.0, 1.0))
        return false;
    if (bufferFrames == 0)
        bufferFrames = defaultBufferFrames(channels);
    const auto chans = static_cast<std::size_t>(channels);
    // read_mchancross_data.
    std::vector<std::size_t> events;
    std::vector<std::array<int, maximumChannels>> maps;
    auto lasttime = -1.0;
    for (const auto& p : table)
    {
        if (!std::isfinite(p.time) || p.time < 0.0 || p.time <= lasttime)
            return false;
        lasttime = p.time;
        const auto f = p.time * sampleRate;
        if (!(f < 1e15))
            return false;
        const auto frame = static_cast<std::size_t>(std::lround(f));
        if (!events.empty() && frame <= events.back())
            return false;
        std::array<int, maximumChannels> map{};
        map.fill(-1);
        const auto centre = p.map[0] == 0;
        std::array<bool, maximumChannels> seen{};
        for (std::size_t n = 0; n < chans; n++)
        {
            const auto c = p.map[n];
            if (centre ? c != 0 : (c < 1 || c > channels || seen[static_cast<std::size_t>(c - 1)]))
                return false;
            if (!centre)
            {
                seen[static_cast<std::size_t>(c - 1)] = true;
                map[n] = c - 1;
            }
        }
        events.push_back(frame);
        maps.push_back(map);
    }
    // CDP inserts a point at 0 for a table starting later, but moves only
    // one entry of each map up to make room, scrambling them; corrected,
    // the point at 0 copies the first map.
    if (events[0] > 0)
    {
        if (!corrected)
            return false;
        events.insert(events.begin(), 0);
        maps.insert(maps.begin(), maps.front());
    }
    channels_ = chans;
    corrected_ = corrected;
    rolloff_ = rolloff;
    // crosspan.
    const auto chandiv = 1.0 / static_cast<double>(chans);
    centrelevel_ = (1.0 - chandiv) * (1.0 - rolloff);
    centrelevel_ += chandiv;
    bufferFrames_ = bufferFrames;
    events_ = std::move(events);
    maps_ = std::move(maps);
    reset();
    return true;
}

void CrossPan::startSegment() noexcept
{
    const auto& thismap = maps_[segment_];
    const auto& nextmap = segment_ + 1 < maps_.size() ? maps_[segment_ + 1] : maps_[segment_];
    if (thismap[0] < 0)
        type_ = nextmap[0] < 0 ? ATCENTRE : FROMCENTRE;
    else if (nextmap[0] < 0)
        type_ = TOCENTRE;
    else
    {
        type_ = ATEDGE;
        for (std::size_t n = 0; n < channels_; n++)
        {
            if (thismap[n] != nextmap[n])
            {
                type_ = CROSSCENTRE;
                break;
            }
        }
    }
}

void CrossPan::reset() noexcept
{
    segment_ = 0;
    frame_ = 0;
    bufferPosition_ = 0;
    carry_.fill(0.0F);
    if (!maps_.empty())
        startSegment();
}

// do_span, as CDP8's build fuses it. The right edge's index is the one CDP
// leaves unwrapped: past the last channel it lands in the next frame.
void CrossPan::doSpan(
    const std::array<int, maximumChannels>& map, double span, const float* ibuf, Frame& obuf) const noexcept
{
    const auto chans = static_cast<int>(channels_);
    const auto chanspan = span * static_cast<double>(chans);
    const auto halfspan = chanspan / 2.0;
    double levelmin = 1.0;
    if (chanspan > 1.0)
        levelmin = 1.0 / chanspan;
    auto level = (1.0 - levelmin) * (1.0 - rolloff_);
    level += levelmin;
    const auto sidelevel = chanspan < 1.0 ? level * chanspan : level;
    for (int n = 0; n < chans; n++)
    {
        const auto outchan = map[static_cast<std::size_t>(n)];
        const auto in = static_cast<double>(ibuf[n]);
        const auto maxchan = static_cast<double>(outchan) + halfspan;
        auto maxfullchan = static_cast<int>(std::floor(maxchan));
        const auto minchan = static_cast<double>(outchan) - halfspan;
        auto minfullchan = 0;
        if (minchan < 0.0)
            minfullchan = -static_cast<int>(std::floor(-minchan));
        else
            minfullchan = static_cast<int>(std::ceil(minchan));
        auto truminfullchan = minfullchan;
        if (truminfullchan < 0)
            truminfullchan += chans;
        auto trumaxfullchan = maxfullchan;
        if (trumaxfullchan >= chans)
            trumaxfullchan -= chans;
        if (span > 1.0 && truminfullchan == trumaxfullchan)
            maxfullchan--;
        for (auto m = minfullchan; m <= maxfullchan; m++)
        {
            auto truchan = m;
            if (truchan < 0)
                truchan += chans;
            else if (truchan >= chans)
                truchan -= chans;
            auto& o = obuf[static_cast<std::size_t>(truchan)];
            o = static_cast<float>(std::fma(in, level, static_cast<double>(o)));
        }
        const auto partialLevel = maxchan - static_cast<double>(maxfullchan);
        if (!flteq(partialLevel, 0.0))
        {
            maxfullchan++;
            trumaxfullchan = maxfullchan;
            if (corrected_ && trumaxfullchan >= chans)
                trumaxfullchan -= chans;
            minfullchan--;
            truminfullchan = minfullchan;
            if (truminfullchan < 0)
                truminfullchan += chans;
            const auto side = in * sidelevel;
            auto& lo = obuf[static_cast<std::size_t>(truminfullchan)];
            lo = static_cast<float>(std::fma(side, partialLevel, static_cast<double>(lo)));
            if (truminfullchan != trumaxfullchan)
            {
                auto& hi = obuf[static_cast<std::size_t>(trumaxfullchan)];
                hi = static_cast<float>(std::fma(side, partialLevel, static_cast<double>(hi)));
            }
        }
    }
}

bool CrossPan::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || output.size() != input.size() || input.size() % channels_ != 0)
        return false;
    for (const auto x : input)
    {
        if (!std::isfinite(x))
            return false;
    }
    const auto chans = channels_;
    const auto frames = input.size() / chans;
    for (std::size_t i = 0; i < frames; i++)
    {
        while (segment_ + 1 < events_.size() && frame_ >= events_[segment_ + 1])
        {
            segment_++;
            startSegment();
        }
        std::array<float, maximumChannels> ibuf{};
        for (std::size_t m = 0; m < chans; m++)
            ibuf[m] = static_cast<float>(input[i * chans + m]);
        Frame obuf{};
        // The last frame's spill, unless a CDP buffer starts here.
        if (bufferPosition_ != 0)
        {
            for (std::size_t c = 0; c < chans; c++)
                obuf[c] = carry_[c];
        }
        const auto& thismap = maps_[segment_];
        const auto last = segment_ + 1 >= events_.size();
        const auto& nextmap = last ? thismap : maps_[segment_ + 1];
        const auto timefrac = last ? 0.0
                                   : static_cast<double>((frame_ - events_[segment_]) * chans)
                / static_cast<double>((events_[segment_ + 1] - events_[segment_]) * chans);
        switch (type_)
        {
        case ATCENTRE:
            for (std::size_t n = 0; n < chans; n++)
            {
                for (std::size_t m = 0; m < chans; m++)
                    obuf[n] = obuf[n] + ibuf[m];
                obuf[n] = static_cast<float>(static_cast<double>(obuf[n]) * centrelevel_);
            }
            break;
        case FROMCENTRE:
            doSpan(nextmap, 1.0 - timefrac, ibuf.data(), obuf);
            break;
        case TOCENTRE:
            doSpan(thismap, timefrac, ibuf.data(), obuf);
            break;
        case CROSSCENTRE:
            if (timefrac <= 0.5)
                doSpan(thismap, timefrac * 2.0, ibuf.data(), obuf);
            else
                doSpan(nextmap, (1.0 - timefrac) * 2.0, ibuf.data(), obuf);
            break;
        default:
            for (std::size_t n = 0; n < chans; n++)
            {
                if (corrected_)
                    obuf[static_cast<std::size_t>(thismap[n])] = ibuf[n];
                else
                    obuf[n] = ibuf[static_cast<std::size_t>(thismap[n])];
            }
            break;
        }
        for (std::size_t c = 0; c < chans; c++)
        {
            output[i * chans + c] = static_cast<double>(obuf[c]);
            carry_[c] = obuf[chans + c];
        }
        frame_++;
        if (++bufferPosition_ == bufferFrames_)
            bufferPosition_ = 0;
    }
    return true;
}
}
