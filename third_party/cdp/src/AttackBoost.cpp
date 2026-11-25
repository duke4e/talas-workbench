/*
 * Derived from the CDP System, Release 8: dev/env/envprepro.c,
 * dev/env/envfuncs.c, dev/env/envprocess.c, dev/env/envel.c,
 * dev/env/ap_envel.c and dev/cdp2k/tklib1.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/AttackBoost.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR, MS_TO_SECS; envlcon.h's ENV_ATK_SRCH; envel.h's
// ENV_FSECSIZE; envfuncs.c's MINSPAN.
constexpr double flterr = 0.000002;
constexpr double msToSecs = 0.001;
constexpr double envAtkSrch = 200.0;
constexpr std::size_t envFsecsize = 256;
constexpr double minspan = 16.0;

// tklib1.c's flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const auto upperbnd = f2 + flterr;
    const auto lowerbnd = f2 - flterr;
    return !(f1 > upperbnd || f1 < lowerbnd);
}

// CDP's max and min macros.
[[nodiscard]] double cdpMax(double a, double b) noexcept
{
    return a > b ? a : b;
}

[[nodiscard]] double cdpMin(double a, double b) noexcept
{
    return a < b ? a : b;
}
}

bool AttackBoost::prepare(double sampleRate, Mode mode, double level, double gain, double onsetMs, double decayMs,
    Envelope envelope, std::size_t bufferFrames)
{
    const auto within = [](double value, double lo, double hi) {
        return std::isfinite(value) && value >= lo && value <= hi;
    };
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || sampleRate > 1.0e7
        || (mode != Mode::Gate && mode != Mode::ApproximateTime && mode != Mode::ExactTime)
        || !within(level, 0.0, mode == Mode::Gate ? 1.0 : maximumTimeSeconds)
        || !within(gain, minimumGain, maximumGain) || !within(onsetMs, minimumOnsetMs, maximumOnsetMs)
        || !within(decayMs, minimumDecayMs, maximumDecayMs)
        || (envelope != Envelope::Linear && envelope != Envelope::Exponential) || bufferFrames == 0)
    {
        return false;
    }
    sampleRate_ = sampleRate;
    mode_ = mode;
    level_ = level;
    gain_ = gain;
    onsetMs_ = onsetMs;
    decayMs_ = decayMs;
    // envel_preprocess turns ENVTYPE_EXP into ENVTYPE_STEEP.
    steep_ = envelope == Envelope::Exponential;
    bufferFrames_ = bufferFrames;
    const auto onsetFrames = static_cast<std::size_t>(std::ceil(onsetMs * msToSecs * sampleRate)) + 1;
    // go_to_search_start and find_local_maximum's window.
    const auto starttime = cdpMax(0.0, level - (envAtkSrch * msToSecs));
    windowStart_ = static_cast<std::size_t>(std::round(starttime * sampleRate)) / envFsecsize * envFsecsize;
    searchLimit_ = static_cast<std::size_t>(std::round((envAtkSrch * 2.0) * msToSecs * sampleRate));
    switch (mode)
    {
    case Mode::Gate:
        latency_ = onsetFrames + peakWaitFrames;
        break;
    case Mode::ApproximateTime:
        latency_ = onsetFrames + searchLimit_;
        break;
    case Mode::ExactTime:
        latency_ = 0;
        break;
    }
    ring_.assign(std::bit_ceil(latency_ + 1), 0.0F);
    mask_ = ring_.size() - 1;
    reset();
    return true;
}

void AttackBoost::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    frame_ = 0;
    found_ = false;
    decided_ = false;
    attackPoint_ = 0;
    attackLevel_ = -1.0;
    samples_ = {};
    gains_ = {};
    breakpoints_ = 0;
    envelopeReady_ = false;
    segment_ = 0;
    nextSample_ = 0;
    thisGain_ = 1.0;
    thisIncr_ = 0.0;
    thisStep_ = 0.0;
    passSkip_ = true;
    // find_and_test_attack_peak: mode 3 takes the given time itself.
    if (prepared() && mode_ == Mode::ExactTime)
    {
        decided_ = true;
        buildEnvelope(level_);
    }
}

bool AttackBoost::process(std::span<const double> input, std::span<double> output) noexcept
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
        if (!decided_)
            find(frame_, sample);
        if (frame_ >= latency_)
        {
            const auto frame = frame_ - latency_;
            output[index] = static_cast<double>(envelopeReady_ ? envelopeFrame(frame, at(frame)) : at(frame));
        }
        else
        {
            output[index] = 0.0;
        }
        ++frame_;
    }
    return true;
}

// find_attack_point_using_gate and attack_point_definitively_found (mode
// 1): the first sample over the gate, then on while no sample is lower,
// moving to each higher one; find_local_maximum (mode 2): the first of the
// loudest samples in the search window.
void AttackBoost::find(std::size_t frame, float sample) noexcept
{
    const auto thislevel = std::fabs(static_cast<double>(sample));
    if (mode_ == Mode::Gate)
    {
        if (!found_)
        {
            if (thislevel > level_)
            {
                found_ = true;
                attackLevel_ = thislevel;
                attackPoint_ = frame;
            }
            return;
        }
        if (thislevel < attackLevel_)
            decided_ = true;
        else if (thislevel > attackLevel_)
        {
            attackLevel_ = thislevel;
            attackPoint_ = frame;
        }
        // The stream waits no longer for a held peak to fall.
        else if (frame - attackPoint_ >= peakWaitFrames)
            decided_ = true;
    }
    else
    {
        if (frame < windowStart_)
            return;
        if (thislevel > attackLevel_)
        {
            attackLevel_ = thislevel;
            attackPoint_ = frame;
        }
        if (frame + 1 >= windowStart_ + searchLimit_)
            decided_ = true;
    }
    if (decided_)
        buildEnvelope(static_cast<double>(attackPoint_) / sampleRate_);
}

// create_initial_envelope_params, create_envelope and store_brkpnt_envelope
// for ENV_ATTACK, statement for statement (CDP8's build fuses the curve's
// times into fma), then init_brkpnts' and advance_brkpnts' sample numbers;
// the file's end becomes 1 for good.
void AttackBoost::buildEnvelope(double attackTime) noexcept
{
    const auto onset = onsetMs_ * msToSecs;
    const auto tail = decayMs_ * msToSecs;
    const auto attackStart = cdpMax(0.0, attackTime - onset);
    const auto attackEnd = attackTime + tail;
    struct Point
    {
        double time;
        double level;
        bool steep;
    };
    std::array<Point, 4> in{};
    std::size_t items = 0;
    if (attackTime > 0.0)
    {
        in[items++] = Point{0.0, 1.0, false};
        if (attackStart > 0.0)
            in[items++] = Point{attackStart, 1.0, false};
        in[items++] = Point{attackTime, gain_, steep_};
    }
    else
    {
        in[items++] = Point{0.0, gain_, false};
    }
    in[items++] = Point{attackEnd, 1.0, steep_};

    std::array<double, maximumBreakpoints> times{};
    times[0] = in[0].time;
    gains_[0] = in[0].level;
    std::size_t cnt = 1;
    for (std::size_t n = 1; n < items; n++)
    {
        const auto thisduration = in[n].time - in[n - 1].time;
        const auto timeQuantum = cdpMax(thisduration / minspan, 2.0 / sampleRate_);
        const auto levelstep = in[n].level - in[n - 1].level;
        const bool falling = levelstep <= 0.0;
        if (!in[n].steep)
        {
            times[cnt] = in[n].time;
            gains_[cnt] = in[n].level;
            ++cnt;
            continue;
        }
        const auto base = cnt;
        auto newcnt = static_cast<std::size_t>(thisduration / timeQuantum);
        if (newcnt == 0)
            newcnt = 1;
        newcnt = std::min<std::size_t>(newcnt, 16);
        const auto timestep = thisduration / static_cast<double>(newcnt);
        cnt += newcnt;
        double m = 1.0;
        for (std::size_t k = base; k < cnt; k++, m += 1.0)
        {
            times[k] = std::fma(m, timestep, in[n - 1].time);
            double ratio = 0.0;
            if (falling)
            {
                ratio = 1.0 - ((timestep * m) / thisduration);
                ratio = cdpMax(ratio, 0.0);
                ratio = std::pow(ratio, attenator_);
                gains_[k] = std::fabs(levelstep) * ratio;
                gains_[k] += in[n].level;
            }
            else
            {
                ratio = (timestep * m) / thisduration;
                ratio = std::pow(ratio, attenator_);
                ratio = cdpMin(ratio, 1.0);
                gains_[k] = std::fabs(levelstep) * ratio;
                gains_[k] += in[n - 1].level;
            }
        }
        times[cnt - 1] = in[n].time;
    }
    // init_brkpnts and advance_brkpnts: each time from the first, in
    // samples.
    for (std::size_t k = 0; k < cnt; k++)
        samples_[k] = static_cast<std::size_t>(std::round((times[k] - times[0]) * sampleRate_));
    samples_[cnt] = std::numeric_limits<std::size_t>::max();
    gains_[cnt] = 1.0;
    breakpoints_ = cnt + 1;
    // The output has not yet reached the gain's first change.
    envelopeReady_ = true;
    startSegment(0);
    passSkip_ = flteq(thisGain_, 1.0) && flteq(thisStep_, 0.0);
}

// init_brkpnts (the first segment) and advance_brkpnts: the segment's gain
// starts at its breakpoint's, stepping by the difference over its samples.
void AttackBoost::startSegment(std::size_t index) noexcept
{
    segment_ = index;
    thisGain_ = gains_[index];
    nextSample_ = samples_[index + 1];
    thisStep_ = gains_[index + 1] - gains_[index];
    const auto sampdist = static_cast<double>(samples_[index + 1] - samples_[index]);
    thisIncr_ = index + 2 == breakpoints_ ? 0.0 : thisStep_ / sampdist;
}

// do_envelope, a frame at a time: a pass runs from each buffer start or
// breakpoint to the next; one starting with a gain within FLTERR of 1 and a
// step within FLTERR of 0 leaves its samples as they are, otherwise each
// sample steps the gain on, then takes it.
float AttackBoost::envelopeFrame(std::size_t frame, float sample) noexcept
{
    bool newPass = frame % bufferFrames_ == 0;
    while (frame >= nextSample_ && segment_ + 2 < breakpoints_)
    {
        startSegment(segment_ + 1);
        newPass = true;
    }
    if (newPass)
        passSkip_ = flteq(thisGain_, 1.0) && flteq(thisStep_, 0.0);
    if (passSkip_)
        return sample;
    thisGain_ += thisIncr_;
    return static_cast<float>(static_cast<double>(sample) * thisGain_);
}
}
