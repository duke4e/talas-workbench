/*
 * Derived from the CDP System, Release 8: dev/modify/delay.c
 * Copyright (c) 1983-2013 Trevor Wishart, Richard Dobson and Composers
 * Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Echo.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// delay.c's ROOTLEN_DELBUF: the line holds two frames beyond the delay.
constexpr std::size_t rootFrames = 2;
// globcon.h's MS_TO_SECS.
constexpr double millisecondsToSeconds = 0.001;
}

bool Echo::prepare(
    double sampleRate,
    double delayMilliseconds,
    double mix,
    double feedback,
    double prescale,
    bool invert,
    bool live)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(delayMilliseconds)
        || delayMilliseconds < 1000.0 / sampleRate
        || delayMilliseconds > maximumDelayMilliseconds
        || !std::isfinite(mix) || mix < 0.0 || mix > 1.0
        || !std::isfinite(feedback) || feedback <= -1.0 || feedback > 1.0
        || !std::isfinite(prescale) || prescale < minimumPrescale || prescale > 1.0)
    {
        return false;
    }
    // do_delay, mode 1, for one channel.
    const auto delay = std::lround(delayMilliseconds * sampleRate * millisecondsToSeconds);
    if (delay < 1)
        return false;
    auto fdfrwd = 1.0 - mix;
    const auto gain = mix;
    const auto fdbk = feedback;
    const auto rescale = (1.0 / (fdbk + 1.0));
    const auto scaledInput = prescale * rescale;
    fdfrwd /= rescale;
    fdfrwd *= invert ? -1.0 : 1.0;

    line_.assign(static_cast<std::size_t>(delay) + rootFrames, 0.0);
    delayFrames_ = static_cast<std::size_t>(delay);
    pointer_ = 0;
    gain_ = gain;
    feedback_ = fdbk;
    // domultiv's dry path is one less Mix, neither raised nor inverted.
    feedForward_ = live ? 1.0 - mix : fdfrwd;
    prescale_ = scaledInput;
    live_ = live;
    startMix_ = nextMix_ = mix;
    startFeedback_ = nextFeedback_ = feedback;
    blockFrame_ = 0;
    return true;
}

bool Echo::setControls(double mix, double feedback) noexcept
{
    if (line_.empty() || !live_ || !std::isfinite(mix) || mix < 0.0 || mix > 1.0
        || !std::isfinite(feedback) || feedback <= -1.0 || feedback > 1.0)
    {
        return false;
    }
    nextMix_ = mix;
    nextFeedback_ = feedback;
    return true;
}

void Echo::reset() noexcept
{
    std::ranges::fill(line_, 0.0);
    pointer_ = 0;
    if (live_)
    {
        nextMix_ = startMix_;
        nextFeedback_ = startFeedback_;
        gain_ = startMix_;
        feedback_ = startFeedback_;
        feedForward_ = 1.0 - startMix_;
        blockFrame_ = 0;
    }
}

bool Echo::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (line_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    // domulti for one channel, keeping CDP's float samples. Its read and
    // write pointers stay together, so each frame reads the line before it
    // writes there.
    auto* delbuf = line_.data();
    auto pointer = pointer_;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        // domultiv reads its breakpoints every 128 frames.
        if (live_)
        {
            if (blockFrame_ == 0)
            {
                feedForward_ = 1.0 - nextMix_;
                gain_ = nextMix_;
                feedback_ = nextFeedback_;
            }
            if (++blockFrame_ >= 128)
                blockFrame_ = 0;
        }
        const double inputs = static_cast<float>(input[frame]) * prescale_;
        const double outputs = (inputs * feedForward_) + (delbuf[pointer] * gain_);
        delbuf[pointer] = delbuf[pointer] * feedback_;
        delbuf[pointer] += inputs;
        output[frame] = static_cast<double>(static_cast<float>(outputs));
        if (++pointer >= delayFrames_)
            pointer -= delayFrames_;
    }
    pointer_ = pointer;
    return true;
}
}
