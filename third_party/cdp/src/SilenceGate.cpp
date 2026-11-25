/*
 * Derived from the CDP System, Release 8: dev/standalone/gate.c,
 * dev/cdp2k/tklib1.c and dev/cdp2k/tklib3.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SilenceGate.h"

#include <algorithm>
#include <bit>
#include <cmath>

#if defined(__APPLE__)
// Apple's libm, where CDP8's build turns pow(10.0, x) into __exp10(x).
extern "C" double __exp10(double);
#endif

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR and MS_TO_SECS.
constexpr double flterr = 0.000002;
constexpr double msToSecs = 0.001;

// pow(10.0, x) as CDP8's build calls it.
[[nodiscard]] double cdpExp10(double x) noexcept
{
#if defined(__APPLE__)
    return __exp10(x);
#else
    return std::pow(10.0, x);
#endif
}

// gate.c's dbtolevel, statement for statement: within FLTERR of 0 dB
// (flteq) is exactly 1.
[[nodiscard]] double dbtolevel(double val) noexcept
{
    bool isneg = false;
    if (val <= flterr && val >= -flterr)
        return 1.0;
    if (val < 0.0)
    {
        val = -val;
        isneg = true;
    }
    val /= 20.0;
    val = cdpExp10(val);
    if (isneg)
        val = 1.0 / val;
    return val;
}
}

bool SilenceGate::prepare(double sampleRate, double gateDecibels, std::size_t bufferFrames)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || !std::isfinite(gateDecibels)
        || gateDecibels < minimumDecibels || gateDecibels > maximumDecibels || bufferFrames == 0)
    {
        return false;
    }
    // gate()'s splicelen and check_gate_param_validity_and_consistency's
    // refusal of a level at or under one over the sample rate.
    const auto splicelen = std::round(1.0 * msToSecs * sampleRate);
    const auto level = dbtolevel(gateDecibels);
    if (splicelen < 1.0 || splicelen > 1.0e6 || level <= 1.0 / sampleRate)
        return false;
    level_ = level;
    splicelen_ = static_cast<std::size_t>(splicelen);
    incr_ = 1.0 / static_cast<double>(splicelen_);
    bufferFrames_ = bufferFrames;
    // The ring holds the wait and the frame arriving.
    ring_.assign(std::bit_ceil(2 * splicelen_ + 1), 0.0F);
    mask_ = ring_.size() - 1;
    reset();
    return true;
}

void SilenceGate::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    frame_ = 0;
    nextScan_ = 0;
    inRun_ = false;
    runStart_ = 0;
    runQueued_ = false;
    cuts_ = {};
    cutHead_ = 0;
    cutCount_ = 0;
    stage_ = Stage::Copy;
    drift_ = 0;
    count_ = 0;
    splice_ = 0.0;
}

bool SilenceGate::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    const auto latency = latencyFrames();
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto sample = static_cast<float>(input[index]);
        at(frame_) = sample;
        search(frame_, sample);
        output[index] = frame_ >= latency ? static_cast<double>(gateFrame(frame_ - latency)) : 0.0;
        ++frame_;
    }
    return true;
}

// gate()'s first pass, a sample at a time: from each sample its search
// tests, a run is every sample from there under the level. A run longer
// than two splices (one at the stream's start) is queued for gating as
// soon as it is known to be, its end filled in when it comes.
void SilenceGate::search(std::size_t frame, float sample) noexcept
{
    const auto magnitude = std::fabs(static_cast<double>(sample));
    if (inRun_)
    {
        if (magnitude < level_)
        {
            const auto length = frame - runStart_ + 1;
            const auto least = runStart_ == 0 ? splicelen_ : 2 * splicelen_;
            if (!runQueued_ && length > least && cutCount_ < maximumCuts)
            {
                cut(cutCount_++) = Cut{runStart_, 0, true};
                runQueued_ = true;
            }
            return;
        }
        // The run ends at the first sample at or over the level, which the
        // search then steps past; but if the run began in an earlier
        // buffer, CDP's search starts this buffer again at zero and steps
        // on the run's whole length, so skipping as many samples as the run
        // had before this buffer, to at most its end.
        inRun_ = false;
        const auto length = frame - runStart_;
        if (runQueued_)
        {
            cut(cutCount_ - 1).end = frame;
            cut(cutCount_ - 1).open = false;
        }
        else if (length > (runStart_ == 0 ? splicelen_ : 2 * splicelen_) && cutCount_ < maximumCuts)
        {
            cut(cutCount_++) = Cut{runStart_, frame, false};
        }
        runQueued_ = false;
        const auto bufferStart = frame / bufferFrames_ * bufferFrames_;
        const auto next = runStart_ >= bufferStart ? frame + 1 : bufferStart + length + 1;
        nextScan_ = std::min(next, bufferStart + bufferFrames_);
        return;
    }
    if (frame < nextScan_)
        return;
    // A sample over the level is passed by; one at it starts a run of no
    // samples (CDP's while loop tests strictly under).
    if (magnitude < level_)
    {
        inRun_ = true;
        runStart_ = frame;
        // A run of one sample at the start may already be long enough.
        search(frame, sample);
        return;
    }
    nextScan_ = frame + 1;
}

// gate()'s second pass, a sample at a time: ungated samples are copied
// until CDP's count of them reaches the next gating's start; a gating at
// the stream's start is a splice of zeros, any other fades out over a
// splice, from 1 down in steps of one over the splice; zeros follow up to a
// splice before the run's end; then a fade in from 0 over a splice. Any
// frames the fade in runs past the run's end delay CDP's count.
float SilenceGate::gateFrame(std::size_t frame) noexcept
{
    const auto sample = at(frame);
    for (;;)
    {
        switch (stage_)
        {
        case Stage::Copy:
            if (cutCount_ > 0 && cut(0).start == frame - drift_)
            {
                stage_ = cut(0).start == 0 ? Stage::ZeroStart : Stage::DownSplice;
                count_ = 0;
                splice_ = 1.0;
                continue;
            }
            return sample;
        case Stage::ZeroStart:
            if (count_ < splicelen_)
            {
                ++count_;
                return 0.0F;
            }
            stage_ = Stage::Zeros;
            continue;
        case Stage::DownSplice:
            if (count_ < splicelen_)
            {
                ++count_;
                const auto out = static_cast<float>(static_cast<double>(sample) * splice_);
                splice_ -= incr_;
                return out;
            }
            stage_ = Stage::Zeros;
            continue;
        case Stage::Zeros:
            // A run still under way ends at least two splices on.
            if (cut(0).open || frame + splicelen_ < cut(0).end)
                return 0.0F;
            stage_ = Stage::UpSplice;
            count_ = 0;
            splice_ = 0.0;
            continue;
        case Stage::UpSplice:
            if (count_ < splicelen_)
            {
                ++count_;
                const auto out = static_cast<float>(static_cast<double>(sample) * splice_);
                splice_ += incr_;
                return out;
            }
            drift_ = frame - cut(0).end;
            cutHead_ = (cutHead_ + 1) % maximumCuts;
            --cutCount_;
            stage_ = Stage::Copy;
            continue;
        }
    }
}
}
