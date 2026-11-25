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
#include "talas/cdp/VaryingDelay.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
constexpr double pi = 3.1415926535897932;
constexpr double twoPi = 2.0 * pi;
// delay.c's ROOTLEN_VDELBUF: the line holds four frames beyond the delay.
constexpr int rootFrames = 4;
constexpr double millisecondsToSeconds = 0.001;
}

bool VaryingDelay::prepare(double sampleRate, const VaryingDelaySettings& settings)
{
    const auto& p = settings;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(p.delayMilliseconds) || p.delayMilliseconds < 1000.0 / sampleRate
        || p.delayMilliseconds > maximumDelayMilliseconds
        || !std::isfinite(p.mix) || p.mix < 0.0 || p.mix > 1.0
        || !std::isfinite(p.feedback) || p.feedback <= -1.0 || p.feedback > 1.0
        || !std::isfinite(p.modulation) || p.modulation < 0.0 || p.modulation > 1.0
        || !std::isfinite(p.lfoFrequency) || std::abs(p.lfoFrequency) > maximumLfoFrequency
        || !std::isfinite(p.lfoPhase) || p.lfoPhase < 0.0 || p.lfoPhase > 1.0
        || !std::isfinite(p.lfoDelay) || p.lfoDelay < 0.0 || p.lfoDelay > maximumLfoDelaySeconds
        || !std::isfinite(p.prescale) || p.prescale < 1.0 / 32767.0 || p.prescale > 1.0
        || p.seed < 1 || p.seed > 256)
    {
        return false;
    }
    // delay_preprocess, mode 2.
    const auto sineOscillator = !(p.lfoFrequency < 0.0);
    const auto modulationLength = p.lfoFrequency != 0.0 ? sampleRate / std::fabs(p.lfoFrequency) : 0.0;
    const auto lfoDelayFrames = static_cast<int>(sampleRate * p.lfoDelay);
    // do_delay, mode 2, for one channel. CDP sets the dry path's invert
    // factor only for mode 1, so here it stays zero.
    const auto delay = std::lround(p.delayMilliseconds * sampleRate * millisecondsToSeconds);
    if (delay < 1)
        return false;
    constexpr double invert = 0.0;
    auto fdfrwd = 1.0 - p.mix;
    const auto gain = p.mix;
    const auto fdbk = p.feedback;
    const auto rescale = (1.0 / (fdbk + 1.0));
    const auto prescale = p.prescale * rescale;
    fdfrwd /= rescale;
    fdfrwd *= invert;
    auto modulationRange = 0.0;
    if (p.modulation > 0.0)
    {
        modulationRange = static_cast<double>(delay) * p.modulation;
        if (modulationRange >= static_cast<double>(delay))
            modulationRange = static_cast<double>(delay) - 1.0;
    }

    line_.assign(static_cast<std::size_t>(delay) + rootFrames, 0.0);
    frameDelay_ = static_cast<int>(delay);
    sineOscillator_ = sineOscillator;
    modulationLength_ = modulationLength;
    modulationRange_ = modulationRange;
    lfoPhase_ = p.lfoPhase;
    lfoDelayFrames_ = lfoDelayFrames;
    gain_ = gain;
    feedback_ = fdbk;
    feedForward_ = fdfrwd;
    prescale_ = prescale;
    // new_soscil.
    startOscillator_.phase = twoPi * p.lfoPhase;
    startOscillator_.increment = twoPi / sampleRate * p.lfoFrequency;
    startOscillator_.delay = static_cast<unsigned int>(lfoDelayFrames);
    startOscillator_.count = 0;
    startOscillator_.value = std::sin(startOscillator_.phase);
    startRandom_.seed(static_cast<unsigned long>(p.seed));
    reset();
    return true;
}

void VaryingDelay::reset() noexcept
{
    std::ranges::fill(line_, 0.0);
    inputPointer_ = 0;
    outputPointer_ = 0;
    oscillator_ = startOscillator_;
    random_ = startRandom_;
    randomIndex_ = 0;
    randomCounter_ = 0;
    randomOut_ = 0.0;
    randomDifference_ = 0.0;
}

bool VaryingDelay::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (line_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    auto* delbuf = line_.data();
    const auto framedelay = frameDelay_;
    // soscil_tick and soscil_tickf.
    const auto tick = [&](double frequencyModulation, bool modulated) {
        auto& osc = oscillator_;
        if (osc.count >= osc.delay)
        {
            if (modulated)
            {
                const auto nuincr = osc.increment * frequencyModulation;
                osc.phase += nuincr;
            }
            else
            {
                osc.phase += osc.increment;
            }
            if (osc.phase >= pi)
                osc.phase -= twoPi;
            osc.value = std::sin(osc.phase);
        }
        else
        {
            osc.count++;
        }
        return osc.value;
    };
    // count and randfac.
    const auto randomFactor = [&] {
        auto counted = lfoDelayFrames_;
        if (randomCounter_ <= counted)
        {
            counted -= randomCounter_;
            randomCounter_++;
        }
        else
        {
            counted = 0;
        }
        if (!counted)
        {
            if (randomIndex_ == 0)
            {
                const auto dummy = random_.next();
                const auto temp = random_.next();
                randomDifference_ = randomOut_ - (0.5 * (dummy + temp));
            }
            randomOut_ -= (randomDifference_) / modulationLength_;
            randomIndex_++;
            if (randomIndex_ >= static_cast<int>(modulationLength_))
                randomIndex_ = 0;
            return randomOut_;
        }
        return lfoPhase_;
    };
    // do_vdelay_multi for one channel, keeping CDP's float writes.
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        double val = 0.0;
        if (sineOscillator_)
        {
            val = 0.5 + (0.5 * tick(0.0, false));
        }
        else
        {
            val = randomFactor();
            val = 0.5 + (0.5 * tick(val, true));
        }
        const auto offset = modulationRange_ * val;
        const auto inputSample = (static_cast<double>(static_cast<float>(input[frame]))) * prescale_;
        if (inputPointer_ >= framedelay)
            inputPointer_ -= framedelay;
        if (outputPointer_ >= framedelay)
            outputPointer_ -= framedelay;
        auto optr1 = outputPointer_ + static_cast<int>(offset);
        if (optr1 >= framedelay)
            optr1 -= framedelay;
        auto optr2 = optr1 + 1;
        if (optr2 >= framedelay)
            optr2 -= framedelay;
        const auto dinterp = static_cast<double>(delbuf[optr1])
            + (std::fmod(offset, 1.0) * (static_cast<double>(delbuf[optr2]) - static_cast<double>(delbuf[optr1])));
        const auto outputSample = (inputSample * feedForward_) + (dinterp * gain_);
        delbuf[inputPointer_] = static_cast<float>(dinterp * feedback_);
        delbuf[inputPointer_] += static_cast<float>(inputSample);
        inputPointer_++;
        output[frame] = static_cast<double>(static_cast<float>(outputSample));
        outputPointer_++;
    }
    return true;
}
}
