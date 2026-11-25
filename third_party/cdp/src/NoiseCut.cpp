/*
 * Derived from the CDP System, Release 8: dev/editsf/cut.c,
 * dev/editsf/ap_edit.c and dev/cdp2k/tklib1.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/NoiseCut.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's MS_TO_SECS and tkglobals.h's SMP_FLTERR.
constexpr double msToSecs = 0.001;
constexpr double smpFltErr = 0.0000005;
}

void NoiseCut::Queue::push(std::size_t value) noexcept
{
    // Sized in prepare for the most positions the stream can hold; never
    // full.
    if (count == items.size())
        return;
    items[(head + count) % items.size()] = value;
    ++count;
}

void NoiseCut::Queue::popFront() noexcept
{
    head = (head + 1) % items.size();
    --count;
}

void NoiseCut::Queue::popBack() noexcept
{
    --count;
}

bool NoiseCut::prepare(double sampleRate, double spliceMs, double noiseFrequency, double maxNoiseMs, double minToneMs,
    bool keepNoise, bool lastWavesetNoise, std::size_t bufferFrames)
{
    const auto within = [](double value, double lo, double hi) {
        return std::isfinite(value) && value >= lo && value <= hi;
    };
    // tklib1.c's ranges for NOISE_SUPRESS.
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || !within(spliceMs, 0.0, maximumSpliceMs)
        || !within(noiseFrequency, minimumNoiseFrequency, sampleRate / 2.0)
        || !within(maxNoiseMs, 0.0, maximumMaxNoiseMs) || !within(minToneMs, 0.0, maximumMinToneMs)
        || bufferFrames == 0)
    {
        return false;
    }
    // ap_edit.c's param_preprocess, which refuses splices as long as the
    // buffer.
    const auto splicelen = static_cast<std::size_t>(std::round(spliceMs * msToSecs * sampleRate));
    if (splicelen >= bufferFrames)
        return false;
    splicelen_ = splicelen;
    minfrq_ = static_cast<std::size_t>(std::round(sampleRate / noiseFrequency));
    minNoise_ = static_cast<std::size_t>(std::round(maxNoiseMs * msToSecs * sampleRate));
    minTone_ = static_cast<std::size_t>(std::round(minToneMs * msToSecs * sampleRate));
    maxMin_ = std::max(minNoise_, minTone_);
    splincr_ = 1.0 / static_cast<double>(splicelen_);
    keepNoise_ = keepNoise;
    lastWavesetNoise_ = lastWavesetNoise;
    bufferFrames_ = bufferFrames;
    latency_ = std::max(3 * splicelen_ + maxMin_, minfrq_ - 1);
    // The ring holds the wait and the frame arriving; switches are at
    // waveset ends, at least two frames apart.
    ring_.assign(std::bit_ceil(latency_ + 1), 0.0F);
    mask_ = ring_.size() - 1;
    const auto positions = latency_ / 2 + 8;
    stored_.items.assign(positions, 0);
    stack_.items.assign(positions, 0);
    starts_.items.assign(positions, 0);
    reset();
    return true;
}

void NoiseCut::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    frame_ = 0;
    started_ = false;
    phase0_ = true;
    secondHalf_ = false;
    isnoise1_ = -1;
    noisefirst1_ = false;
    brkpntcnt1_ = 0;
    lastzcross1_ = 0;
    lastpos1_ = 0;
    isnoise2_ = lastWavesetNoise_ ? 1 : 0;
    firstWaveset_ = true;
    brkpntcnt2_ = 0;
    lastzcross2_ = 0;
    last_ = 0;
    previous_ = 0;
    noisefirstSet_ = false;
    noisefirst2_ = false;
    stored_.clear();
    stack_.clear();
    arrived_ = 0;
    starts_.clear();
    assigned_ = 0;
    firstDropped_ = false;
    inNoise_ = false;
    inSplice_ = false;
    spliceCnt_ = 0;
    spliceval_ = 0.0;
    incr_ = 0.0;
    skip_ = 0;
    std::ranges::fill(stored_.items, 0);
    std::ranges::fill(stack_.items, 0);
    std::ranges::fill(starts_.items, 0);
}

bool NoiseCut::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    // CDP's first search can change noisefirst only at wavesets ending by
    // here.
    const auto firstSearchEnd = std::max(maxMin_, minfrq_);
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto sample = static_cast<float>(input[index]);
        at(frame_) = sample;
        if (scan(frame_, sample))
        {
            if (frame_ <= firstSearchEnd)
                firstSearch(frame_);
            secondSearch(frame_);
        }
        settle(frame_);
        output[index] = frame_ >= latency_ ? static_cast<double>(edit(frame_ - latency_)) : 0.0;
        ++frame_;
    }
    return true;
}

// do_noise_suppression's waveset scan: after samples within SMP_FLTERR of
// zero, the first sample's side sets the phase; a waveset runs while the
// samples keep to that side (zero counting as negative), then to the other,
// ending at the first sample back on the first side. At each buffer's start
// CDP's scan begins again with the first half, so a waveset that should end
// on a buffer's first sample runs on.
bool NoiseCut::scan(std::size_t frame, float sample) noexcept
{
    const auto value = static_cast<double>(sample);
    if (!started_)
    {
        if (value <= smpFltErr && value >= -smpFltErr)
            return false;
        started_ = true;
        phase0_ = sample > 0.0F;
        secondHalf_ = false;
    }
    else if (frame % bufferFrames_ == 0)
    {
        secondHalf_ = false;
    }
    const bool firstSide = phase0_ ? sample > 0.0F : sample <= 0.0F;
    if (!secondHalf_)
    {
        if (!firstSide)
            secondHalf_ = true;
        return false;
    }
    if (!firstSide)
        return false;
    secondHalf_ = false;
    return true;
}

// do_noise_suppression's first search, only for the noisefirst it leaves:
// it compares each switch with the last switch it kept, even once removed.
void NoiseCut::firstSearch(std::size_t position) noexcept
{
    auto thispos = position;
    bool doincr = false;
    if (thispos - lastzcross1_ < minfrq_)
    {
        if (isnoise1_ < 0)
            noisefirst1_ = true;
        else if (isnoise1_ == 0)
        {
            if (brkpntcnt1_ == 0)
            {
                if (thispos > minTone_)
                    doincr = true;
                else
                    noisefirst1_ = true;
            }
            else if (thispos - lastpos1_ > minTone_)
                doincr = true;
            else
            {
                --brkpntcnt1_;
                thispos = lastpos1_;
            }
        }
        isnoise1_ = 1;
    }
    else
    {
        if (isnoise1_ < 0)
            noisefirst1_ = false;
        else if (isnoise1_ == 1)
        {
            if (brkpntcnt1_ == 0)
            {
                if (thispos > minNoise_)
                    doincr = true;
                else
                    noisefirst1_ = false;
            }
            else if (thispos - lastpos1_ > minNoise_)
                doincr = true;
            else
            {
                --brkpntcnt1_;
                thispos = lastpos1_;
            }
        }
        isnoise1_ = 0;
    }
    lastzcross1_ = thispos;
    if (doincr)
    {
        lastpos1_ = thispos;
        ++brkpntcnt1_;
    }
}

// do_noise_suppression's second search, which keeps the switches. It
// begins with the first search's isnoise and last waveset end, that of the
// file's end, so its first waveset is noise.
void NoiseCut::secondSearch(std::size_t position) noexcept
{
    const bool noise = firstWaveset_ || position - lastzcross2_ < minfrq_;
    firstWaveset_ = false;
    bool doincr = false;
    bool removed = false;
    if (noise)
    {
        if (isnoise2_ == 0)
        {
            if (brkpntcnt2_ == 0)
            {
                if (position > minTone_)
                    doincr = true;
                else
                {
                    noisefirstSet_ = true;
                    noisefirst2_ = true;
                }
            }
            else if (position - last_ > minTone_)
                doincr = true;
            else
                removed = true;
        }
        isnoise2_ = 1;
    }
    else
    {
        if (isnoise2_ == 1)
        {
            if (brkpntcnt2_ == 0)
            {
                if (position > minNoise_)
                    doincr = true;
                else
                {
                    noisefirstSet_ = true;
                    noisefirst2_ = false;
                }
            }
            else if (position - last_ > minNoise_)
                doincr = true;
            else
                removed = true;
        }
        isnoise2_ = 0;
    }
    if (removed)
    {
        // The switch that began the short segment goes; the next waveset is
        // measured from it.
        lastzcross2_ = last_;
        --brkpntcnt2_;
        if (!stored_.empty())
            stored_.popBack();
        last_ = previous_;
    }
    else
    {
        lastzcross2_ = position;
    }
    if (doincr)
    {
        previous_ = last_;
        last_ = position;
        ++brkpntcnt2_;
        stored_.push(position);
    }
}

bool NoiseCut::noisefirst() const noexcept
{
    return noisefirstSet_ ? noisefirst2_ : noisefirst1_;
}

// Passes each switch on once settled: a stored switch, once no switch
// within the longer minimum length can remove it (and noisefirst is
// settled); a switch past the pair test, once no switch within two splices
// can still come.
void NoiseCut::settle(std::size_t frame) noexcept
{
    if (frame + 1 < minfrq_)
        return;
    while (!stored_.empty() && frame >= stored_.front() + maxMin_)
    {
        const auto point = stored_.front();
        stored_.popFront();
        eliminate(point);
    }
    while (!stack_.empty() && frame >= stack_.front() + 2 * splicelen_ + maxMin_)
    {
        const auto point = stack_.front();
        stack_.popFront();
        assignSplice(point);
    }
}

// "ELIMINATE ANY TONE-SEGS SHORTER THAN 2 SPLICELENS": past the first one
// (noise first) or two switches, a switch no more than two splices after
// the last one kept removes both.
void NoiseCut::eliminate(std::size_t point) noexcept
{
    ++arrived_;
    const std::size_t unconditional = noisefirst() ? 1 : 2;
    if (arrived_ <= unconditional || stack_.empty() || point - stack_.back() > 2 * splicelen_)
        stack_.push(point);
    else
        stack_.popBack();
}

// A switch into tone fades in over the splice after it, one into noise
// fades out over the splice before it; a first switch into noise less than
// a splice into the stream is dropped, the stream then starting in noise.
void NoiseCut::assignSplice(std::size_t point) noexcept
{
    const bool intoTone = (assigned_ % 2 == 0) == noisefirst();
    if (assigned_ == 0 && !intoTone && point < splicelen_)
        firstDropped_ = true;
    else
        starts_.push(intoTone || point < splicelen_ ? point : point - splicelen_);
    ++assigned_;
}

// do_noise_suppression's edit and do_alternating_splices, a frame at a
// time: noise is zeroed, and at each splice start the gain steps from 0 up
// (into tone) or from 1 down (into noise) by one over the splice. A splice
// running into a new buffer restarts its gain from its count there, and CDP
// then skips the next splice start.
float NoiseCut::edit(std::size_t frame) noexcept
{
    if (frame == 0)
        inNoise_ = (firstDropped_ || noisefirst()) != keepNoise_;
    const auto sample = at(frame);
    if (inSplice_ && frame % bufferFrames_ == 0)
    {
        spliceval_ = static_cast<double>(spliceCnt_) / static_cast<double>(splicelen_);
        incr_ = splincr_;
        if (!inNoise_)
        {
            spliceval_ = 1.0 - spliceval_;
            incr_ = -incr_;
        }
        ++skip_;
    }
    if (!inSplice_)
    {
        while (skip_ > 0 && !starts_.empty())
        {
            starts_.popFront();
            --skip_;
        }
        // (A start can lie no earlier than the frame; were it to, it would
        // begin at once rather than block the starts after it.)
        if (!starts_.empty() && starts_.front() <= frame)
        {
            starts_.popFront();
            spliceCnt_ = 0;
            spliceval_ = 0.0 / static_cast<double>(splicelen_);
            incr_ = splincr_;
            if (!inNoise_)
            {
                spliceval_ = 1.0 - spliceval_;
                incr_ = -incr_;
            }
            if (splicelen_ == 0)
                inNoise_ = !inNoise_;
            else
                inSplice_ = true;
        }
    }
    if (inSplice_)
    {
        const auto out = static_cast<float>(static_cast<double>(sample) * spliceval_);
        ++spliceCnt_;
        if ((frame + 1) % bufferFrames_ != 0)
            spliceval_ += incr_;
        if (spliceCnt_ >= splicelen_)
        {
            inNoise_ = !inNoise_;
            inSplice_ = false;
        }
        return out;
    }
    return inNoise_ ? 0.0F : sample;
}
}
