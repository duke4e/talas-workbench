/*
 * Derived from the CDP System, Release 8: dev/houskeep/clean.c,
 * dev/houskeep/ap_house.c and dev/cdp2k/tklib1.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Deglitch.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's MS_TO_SECS and SECS_TO_MS.
constexpr double msToSecs = 0.001;
constexpr double secsToMs = 1000.0;

// house_gate2's search states.
constexpr int nothingFound = 0;
constexpr int interglitchFound = 1;
constexpr int measuringGlitch = 2;
constexpr int measuringInterglitch = 3;

// ap_house.c's param_preprocess: max(1, (int)round(ms * MS_TO_SECS * srate)).
[[nodiscard]] std::size_t toFrames(double ms, double sampleRate) noexcept
{
    return static_cast<std::size_t>(std::max(1.0, std::round(ms * msToSecs * sampleRate)));
}
}

bool Deglitch::prepare(
    double sampleRate, double glitchMs, double silenceMs, double threshold, double spliceMs, double windowMs)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return false;
    // tklib1.c's ranges for HOUSE_GATE2.
    const auto oneFrameMs = (1.0 / sampleRate) * secsToMs;
    const auto within = [](double value, double lo, double hi) {
        return std::isfinite(value) && value >= lo && value <= hi;
    };
    if (!within(glitchMs, oneFrameMs, maximumGlitchMs) || !within(silenceMs, minimumSilenceMs, maximumSilenceMs)
        || !within(threshold, 0.0, maximumThreshold) || !within(spliceMs, 0.0, maximumSpliceMs)
        || !within(windowMs, oneFrameMs, maximumWindowMs))
    {
        return false;
    }
    const auto dur = toFrames(glitchMs, sampleRate);
    const auto zeros = toFrames(silenceMs, sampleRate);
    const auto splicelen = toFrames(spliceMs, sampleRate);
    const auto filtlen = toFrames(windowMs, sampleRate);
    // "Splicelength must be no more than half as long as minimum duration
    // of 'silence' around glitch."
    if (splicelen * 2 > zeros)
        return false;
    level_ = threshold;
    dur_ = dur;
    zeros_ = zeros;
    splicelen_ = splicelen;
    filtlen_ = filtlen;
    spliceincr_ = 1.0 / static_cast<double>(splicelen);
    latency_ = std::max(dur, filtlen) + zeros - 1 + splicelen;
    // The ring holds the wait and the frame arriving.
    ring_.assign(std::bit_ceil(latency_ + 1), 0.0F);
    mask_ = ring_.size() - 1;
    filtbuf_.assign(filtlen, 0.0F);
    reset();
    return true;
}

void Deglitch::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    std::ranges::fill(filtbuf_, 0.0F);
    frame_ = 0;
    filtIndex_ = 0;
    cnt_ = 0;
    searchState_ = nothingFound;
    zerosgot_ = 0;
    nonzerosgot_ = 0;
    possibleStart_ = 0;
    startcut_ = 0;
    endcut_ = 0;
}

bool Deglitch::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto sample = static_cast<float>(input[index]);
        at(frame_) = sample;
        search(frame_, sample);
        output[index] = frame_ >= latency_ ? static_cast<double>(at(frame_ - latency_)) : 0.0;
        ++frame_;
    }
    return true;
}

// house_gate2's first pass for one channel, statement for statement: the
// window's mean is summed in the order of its slots, as CDP sums its
// cyclic buffer.
void Deglitch::search(std::size_t frame, float sample) noexcept
{
    cnt_ += 1;
    bool gotzero = false;
    filtbuf_[filtIndex_++] = sample;
    auto thissamp = std::fabs(static_cast<double>(sample));
    filtIndex_ %= filtlen_;
    thissamp /= 1.0;
    cnt_ = std::min(cnt_, filtlen_);
    double sum = 0.0;
    for (std::size_t f = 0; f < cnt_; f++)
        sum += std::fabs(static_cast<double>(filtbuf_[f]));
    sum /= static_cast<double>(cnt_);
    if (sum <= level_)
        gotzero = true;
    if (thissamp > level_)
    {
        possibleStart_ = frame;
        gotzero = false;
    }
    if (gotzero)
    {
        nonzerosgot_ = 0;
        zerosgot_++;
        switch (searchState_)
        {
        case nothingFound:
            if (zerosgot_ >= zeros_)
                searchState_ = interglitchFound;
            break;
        case interglitchFound:
            break;
        case measuringGlitch:
            endcut_ = frame;
            searchState_ = measuringInterglitch;
            break;
        case measuringInterglitch:
            if (zerosgot_ >= zeros_)
            {
                cutOut(frame, startcut_, endcut_);
                searchState_ = interglitchFound;
            }
            break;
        default:
            break;
        }
    }
    else
    {
        nonzerosgot_++;
        zerosgot_ = 0;
        switch (searchState_)
        {
        case nothingFound:
            break;
        case interglitchFound:
            startcut_ = possibleStart_;
            nonzerosgot_ += frame - possibleStart_;
            searchState_ = measuringGlitch;
            break;
        case measuringGlitch:
            if (nonzerosgot_ > dur_)
                searchState_ = nothingFound;
            break;
        case measuringInterglitch:
            searchState_ = nothingFound;
            break;
        default:
            break;
        }
    }
}

// house_gate2's second pass for one glitch, in place on the frames still
// waiting: the splice before the glitch from 1 down, a step of one over the
// splice taken before each frame; zeros; the splice after it from 0 up,
// likewise. The splice after has arrived, being no longer than half the
// silence after the glitch; frames before the stream's start, or (were the
// glitch to start further back than its longest, which CDP's arithmetic
// rules out) frames already played, are passed by.
void Deglitch::cutOut(std::size_t frame, std::size_t startcut, std::size_t endcut) noexcept
{
    const auto oldest = frame >= latency_ ? frame - latency_ : 0;
    double spliceratio = 1.0;
    for (std::size_t j = 0; j < splicelen_; ++j)
    {
        spliceratio -= spliceincr_;
        if (startcut + j < splicelen_)
            continue;
        const auto n = startcut + j - splicelen_;
        if (n >= oldest)
            at(n) = static_cast<float>(static_cast<double>(at(n)) * spliceratio);
    }
    for (std::size_t n = std::max(startcut, oldest); n < endcut; ++n)
        at(n) = 0.0F;
    spliceratio = 0.0;
    for (std::size_t n = endcut; n < endcut + splicelen_; ++n)
    {
        spliceratio += spliceincr_;
        if (n >= oldest)
            at(n) = static_cast<float>(static_cast<double>(at(n)) * spliceratio);
    }
}
}
