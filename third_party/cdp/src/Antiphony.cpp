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
#include "talas/cdp/Antiphony.h"

#include <cmath>
#include <cstdint>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR and MS_TO_SECS.
constexpr double FLTERR = 0.000002;
constexpr double MS_TO_SECS = 0.001;
// Malloc(-1)'s 1 MB.
constexpr std::size_t cdpBufferBytes = 1024 * 1024;

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

[[nodiscard]] std::int64_t signedFrame(std::size_t frame) noexcept
{
    return static_cast<std::int64_t>(frame);
}
}

std::size_t Antiphony::defaultBufferFrames(int inputChannels, int outputChannels, int inputs) noexcept
{
    if (inputChannels < 1 || outputChannels < 1 || inputs < 1)
        return 0;
    // allocate_mchanpan_buffer: 1 MB / bufcnt bytes, in floats.
    const auto bufcnt = static_cast<std::size_t>(inputChannels * inputs + outputChannels);
    return cdpBufferBytes / bufcnt / sizeof(float);
}

bool Antiphony::prepare(double sampleRate, int inputChannels, int outputChannels, std::span<const int> firstSet,
    std::span<const int> secondSet, double eventDuration, double gap, double spliceMs, bool sideChain,
    std::size_t bufferFrames, bool corrected)
{
    if (!std::isfinite(sampleRate) || !(sampleRate > 0.0))
        return false;
    if (inputChannels < 1 || inputChannels > maximumChannels || outputChannels < minimumChannels
        || outputChannels > maximumChannels)
        return false;
    if (!inRange(eventDuration, 0.0, maximumEventDuration) || eventDuration < minimumEventDuration
        || !inRange(gap, 0.0, maximumGap) || !inRange(spliceMs, 0.0, maximumSpliceMs))
        return false;
    if (bufferFrames == 0)
        bufferFrames = defaultBufferFrames(inputChannels, outputChannels, sideChain ? 2 : 1);
    if (bufferFrames == 0)
        return false;
    Antiphony next;
    // setup_antiphon_arrays.
    const std::array<std::span<const int>, 2> sets{firstSet, secondSet};
    for (std::size_t s = 0; s < 2; s++)
    {
        const auto& set = sets[s];
        const auto len = set.size();
        if (len == 0 || len > static_cast<std::size_t>(maximumChannels)
            || len % static_cast<std::size_t>(inputChannels) != 0)
            return false;
        std::array<bool, maximumChannels> seen{};
        for (const auto c : set)
        {
            if (c < 1 || c > outputChannels || seen[static_cast<std::size_t>(c - 1)])
                return false;
            seen[static_cast<std::size_t>(c - 1)] = true;
        }
        const auto distribution = len / static_cast<std::size_t>(inputChannels);
        // antiphony steps two ints a pair but stops at the set's length:
        // the first half of the pairs. A letter's number is its index.
        // Corrected: every pair, channel k at index k - 1.
        const auto used = corrected ? len : (len + 1) / 2;
        for (std::size_t j = 0; j < used; j++)
        {
            const auto out = static_cast<std::size_t>(set[j]) - (corrected ? 1U : 0U);
            next.sets_[s][j] = {j / distribution, out};
        }
        next.setSizes_[s] = used;
    }
    next.sampleRate_ = sampleRate;
    next.inChannels_ = static_cast<std::size_t>(inputChannels);
    next.outChannels_ = static_cast<std::size_t>(outputChannels);
    next.sideChain_ = sideChain;
    next.corrected_ = corrected;
    next.bufferFrames_ = bufferFrames;
    next.splice_ = static_cast<std::size_t>(std::lround(spliceMs * MS_TO_SECS * sampleRate));
    next.splicincr_ = 1.0 / static_cast<double>(next.splice_);
    next.startDuration_ = eventDuration;
    next.startGap_ = gap;
    *this = next;
    reset();
    return true;
}

bool Antiphony::setEventDuration(double seconds) noexcept
{
    if (!prepared() || !inRange(seconds, 0.0, maximumEventDuration) || seconds < minimumEventDuration)
        return false;
    duration_ = seconds;
    return true;
}

bool Antiphony::setGap(double seconds) noexcept
{
    if (!prepared() || !inRange(seconds, 0.0, maximumGap))
        return false;
    gap_ = seconds;
    return true;
}

// generate_antiphonal_events_array, one event at a time.
void Antiphony::nextEvent() noexcept
{
    time_ += duration_;
    const auto boundary = std::lround(time_ * sampleRate_);
    const auto gapFrames = std::lround(gap_ * sampleRate_);
    thisevent_ = boundary;
    lasteventend_ = boundary - gapFrames;
}

void Antiphony::reset() noexcept
{
    duration_ = startDuration_;
    gap_ = startGap_;
    time_ = 0.0;
    state_ = 0;
    thisfile_ = 0;
    inSplice_ = false;
    dnsplicval_ = 1.0;
    upsplicval_ = 0.0;
    frame_ = 0;
    bufferPosition_ = 0;
    carry_ = 0.0F;
    if (prepared())
        nextEvent();
}

bool Antiphony::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || sideChain_)
        return false;
    return run(input, input, output);
}

bool Antiphony::process(
    std::span<const double> input, std::span<const double> sideChain, std::span<double> output) noexcept
{
    if (!prepared() || !sideChain_ || sideChain.size() != input.size())
        return false;
    return run(input, sideChain, output);
}

bool Antiphony::run(std::span<const double> a, std::span<const double> b, std::span<double> output) noexcept
{
    if (a.size() % inChannels_ != 0 || output.size() != a.size() / inChannels_ * outChannels_)
        return false;
    for (const auto x : a)
    {
        if (!std::isfinite(x))
            return false;
    }
    for (const auto x : b)
    {
        if (!std::isfinite(x))
            return false;
    }
    const auto frames = a.size() / inChannels_;
    const auto outchans = outChannels_;
    const std::array<std::span<const double>, 2> files{a, b};
    const auto inputs = sideChain_ ? std::size_t{2} : std::size_t{1};
    const auto splice = static_cast<std::int64_t>(splice_);
    for (std::size_t i = 0; i < frames; i++)
    {
        const auto pos = signedFrame(frame_);
        // antiphony: the full event, its splice out and the next one's in,
        // then on to the next event.
        for (;;)
        {
            if (!inSplice_)
            {
                if (pos < lasteventend_)
                    break;
                inSplice_ = true;
                dnsplicval_ = 1.0;
                upsplicval_ = 0.0;
            }
            if (pos < thisevent_ + splice)
                break;
            state_ ^= 1U;
            thisfile_ = (thisfile_ + 1) % inputs;
            nextEvent();
            inSplice_ = false;
        }
        const auto nextfile = (thisfile_ + 1) % inputs;
        const auto* ibuf1 = files[thisfile_].data() + i * inChannels_;
        const auto* ibuf2 = files[nextfile].data() + i * inChannels_;
        const auto& array0 = sets_[state_];
        const auto size0 = setSizes_[state_];
        const auto& array1 = sets_[state_ ^ 1U];
        const auto size1 = setSizes_[state_ ^ 1U];
        std::array<float, maximumChannels + 1> obuf{};
        if (bufferPosition_ != 0)
            obuf[0] = carry_;
        if (!inSplice_)
        {
            for (std::size_t p = 0; p < size0; p++)
            {
                auto& o = obuf[array0[p].out];
                o = o + static_cast<float>(ibuf1[array0[p].in]);
            }
        }
        else
        {
            if (pos < lasteventend_ + splice)
            {
                // sfsys.h's max and min.
                const auto dn = dnsplicval_ - splicincr_;
                dnsplicval_ = 0.0 > dn ? 0.0 : dn;
                for (std::size_t p = 0; p < size0; p++)
                {
                    auto& o = obuf[array0[p].out];
                    const auto in = static_cast<double>(static_cast<float>(ibuf1[array0[p].in]));
                    o = static_cast<float>(std::fma(in, dnsplicval_, static_cast<double>(o)));
                }
            }
            if (pos >= thisevent_)
            {
                const auto up = upsplicval_ + splicincr_;
                upsplicval_ = 1.0 > up ? up : 1.0;
                for (std::size_t p = 0; p < size1; p++)
                {
                    auto& o = obuf[array1[p].out];
                    const auto in = static_cast<double>(static_cast<float>(ibuf2[array1[p].in]));
                    o = static_cast<float>(std::fma(in, upsplicval_, static_cast<double>(o)));
                }
            }
        }
        for (std::size_t c = 0; c < outchans; c++)
            output[i * outchans + c] = static_cast<double>(obuf[c]);
        carry_ = obuf[outchans];
        frame_++;
        if (++bufferPosition_ == bufferFrames_)
            bufferPosition_ = 0;
    }
    return true;
}
}
