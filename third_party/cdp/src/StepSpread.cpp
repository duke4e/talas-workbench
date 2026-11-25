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
#include "talas/cdp/StepSpread.h"

#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR and MS_TO_SECS.
constexpr double FLTERR = 0.000002;
constexpr double MS_TO_SECS = 0.001;
// Malloc(-1)'s 1 MB, in float samples.
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

// sfsys.h's min.
[[nodiscard]] double cdpMin(double x, double y) noexcept
{
    return x > y ? y : x;
}
}

std::size_t StepSpread::defaultBufferFrames(int outputChannels) noexcept
{
    if (outputChannels < 1)
        return 0;
    return cdpBufferSamples / (1 + static_cast<std::size_t>(outputChannels));
}

std::size_t StepSpread::cdpReadFrame(std::size_t eventFrame, std::size_t bufferFrames) noexcept
{
    if (eventFrame == 0 || bufferFrames == 0)
        return 0;
    return eventFrame - (eventFrame - 1) / bufferFrames * bufferFrames;
}

bool StepSpread::centreOk(double& centre) const noexcept
{
    return inRange(centre, 0.0, maximumCentre) && centre <= static_cast<double>(channels_) && std::lround(centre) >= 1;
}

bool StepSpread::prepare(double sampleRate, int outputChannels, double centre, double spread, double depth,
    double rolloff, double minimumSilenceMs, bool stepped)
{
    if (!std::isfinite(sampleRate) || !(sampleRate > 0.0))
        return false;
    if (outputChannels < minimumChannels || outputChannels > maximumChannels)
        return false;
    StepSpread next;
    next.channels_ = static_cast<std::size_t>(outputChannels);
    if (!next.centreOk(centre) || !inRange(spread, 0.0, maximumSpread) || spread < 1.0
        || !inRange(depth, 0.0, maximumDepth) || !inRange(rolloff, 0.0, 1.0))
        return false;
    // The range of Minimum silence: 2 to 32767 samples' worth of ms.
    if (!inRange(minimumSilenceMs, (2.0 / sampleRate) * 1000.0, (32767.0 / sampleRate) * 1000.0))
        return false;
    const auto doutchans = static_cast<double>(outputChannels);
    next.minsil_ = static_cast<std::size_t>(std::lround(minimumSilenceMs * MS_TO_SECS * sampleRate));
    next.stepped_ = stepped;
    if (stepped)
    {
        // check_the_param_validity_and_consistency.
        next.stepSpread_ = static_cast<int>(std::lround(spread));
        next.stepDepth_ = static_cast<int>(std::lround(cdpMin(depth, doutchans / 2.0)));
    }
    next.startCentre_ = centre;
    next.startSpread_ = spread;
    next.startDepth_ = depth;
    next.startRolloff_ = rolloff;
    *this = next;
    reset();
    return true;
}

bool StepSpread::setCentre(double centre) noexcept
{
    if (!prepared() || stepped_ || !centreOk(centre))
        return false;
    centre_ = centre;
    return true;
}

bool StepSpread::setSpread(double spread) noexcept
{
    if (!prepared() || stepped_ || !inRange(spread, 0.0, maximumSpread) || spread < 1.0)
        return false;
    spread_ = spread;
    return true;
}

bool StepSpread::setDepth(double depth) noexcept
{
    if (!prepared() || stepped_ || !inRange(depth, 0.0, maximumDepth))
        return false;
    depth_ = depth;
    return true;
}

bool StepSpread::setRolloff(double rolloff) noexcept
{
    if (!prepared() || !inRange(rolloff, 0.0, 1.0))
        return false;
    rolloff_ = rolloff;
    return true;
}

void StepSpread::reset() noexcept
{
    centre_ = startCentre_;
    spread_ = startSpread_;
    depth_ = startDepth_;
    rolloff_ = startRolloff_;
    eventCentre_ = centre_;
    eventDepth_ = depth_;
    eventRolloff_ = rolloff_;
    levels_.fill(0.0);
    frame_ = 0;
    insil_ = 0;
    events_ = 0;
    leading_ = true;
}

// spread_set, as CDP8's build fuses it.
void StepSpread::spreadSet(double spread) noexcept
{
    const auto outchans = static_cast<int>(channels_);
    const auto centre = static_cast<int>(std::lround(eventCentre_)) - 1;
    auto depth = cdpMin(eventDepth_, static_cast<double>(outchans) / 2.0);
    const auto rolloff = eventRolloff_;
    auto* levels = levels_.data();
    if (stepped_)
        depth = stepDepth_;
    const auto N = static_cast<int>(std::floor(spread));
    for (int chan = 0; chan < outchans; chan++)
        levels[chan] = 0.0;
    const auto range = 1.0 - (1.0 / static_cast<double>(N));
    const auto maxlevel = std::fma(-rolloff, range, 1.0);
    auto K = N / 2;
    if (N % 2 == 0)
        K--;
    auto minchan = centre - K;
    auto maxchan = centre + K;
    for (auto chan = minchan; chan <= maxchan; chan++)
    {
        auto ochan = chan;
        while (ochan < 0)
            ochan += outchans;
        while (ochan >= outchans)
            ochan -= outchans;
        levels[ochan] = maxlevel;
    }
    double hole = 0.0;
    if ((hole = (spread / 2.0) - depth) > 0)
    {
        K = static_cast<int>(std::floor(hole));
        int j = 0;
        for (j = 0; j < K; j++)
        {
            auto ochan = centre + j;
            while (ochan >= outchans)
                ochan -= outchans;
            levels[ochan] = 0;
            ochan = centre - j;
            while (ochan < 0)
                ochan += outchans;
            levels[ochan] = 0;
        }
        if (!stepped_)
        {
            auto fraclevel = hole - static_cast<double>(K);
            fraclevel = 1.0 - fraclevel;
            fraclevel *= maxlevel;
            auto ochan = centre + j;
            while (ochan >= outchans)
                ochan -= outchans;
            levels[ochan] = fraclevel;
            ochan = centre - j;
            while (ochan < 0)
                ochan += outchans;
            levels[ochan] = fraclevel;
        }
    }
    if (stepped_ && spread != outchans)
        return;
    auto fraclevel = spread - static_cast<double>(N);
    if (N % 2 != 0)
        fraclevel = fraclevel / 2.0;
    else
        fraclevel = (fraclevel + 1.0) / 2.0;
    fraclevel *= maxlevel;
    maxchan++;
    while (maxchan >= outchans)
        maxchan -= outchans;
    while (maxchan < 0)
        maxchan += outchans;
    minchan--;
    while (minchan >= outchans)
        minchan -= outchans;
    while (minchan < 0)
        minchan += outchans;
    levels[maxchan] = static_cast<float>(levels[maxchan] + fraclevel);
    levels[minchan] = static_cast<float>(levels[minchan] + fraclevel);
}

bool StepSpread::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || output.size() != input.size() * channels_)
        return false;
    for (const auto x : input)
    {
        if (!std::isfinite(x))
            return false;
    }
    const auto doutchans = static_cast<double>(channels_);
    // stepspread's spreads: stepped, 1, 3, 5, ... up to Spread; else Spread.
    const auto eventSpread = [&]() {
        if (stepped_)
        {
            const auto m = static_cast<int>(2 * events_ + 1);
            const auto s = static_cast<double>(m > stepSpread_ ? stepSpread_ : m);
            return cdpMin(s, doutchans);
        }
        return cdpMin(spread_, doutchans);
    };
    for (std::size_t i = 0; i < input.size(); i++)
    {
        const auto x = static_cast<float>(input[i]);
        if (frame_ == 0)
        {
            eventCentre_ = centre_;
            eventDepth_ = depth_;
            eventRolloff_ = rolloff_;
            spreadSet(eventSpread());
        }
        // count_events: an event starts after a long enough run of zeros.
        if (x == 0.0F)
            insil_++;
        else
        {
            if (leading_)
            {
                leading_ = false;
                if (insil_ >= minsil_)
                    spreadSet(eventSpread());
            }
            else if (insil_ >= minsil_)
            {
                events_++;
                eventCentre_ = centre_;
                eventDepth_ = depth_;
                eventRolloff_ = rolloff_;
                spreadSet(eventSpread());
            }
            insil_ = 0;
        }
        const auto sample = static_cast<double>(x);
        auto* frame = output.data() + i * channels_;
        for (std::size_t m = 0; m < channels_; m++)
            frame[m] = static_cast<double>(static_cast<float>(sample * levels_[m]));
        frame_++;
    }
    return true;
}
}
