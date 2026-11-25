/*
 * Derived from the CDP System, Release 8: dev/distort/distort.c,
 * distortm.c and distortion.c, and dev/cdp2k/tklib1.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#pragma once

#include <cstddef>

namespace talas::cdp::half_cycles
{
// CDP's buffers in its default 1 MB of memory, in whole 256-frame sectors,
// as create_sndbufs shares them out.
[[nodiscard]] constexpr std::size_t defaultBufferFrames(std::size_t buffers) noexcept
{
    return ((std::size_t{1024} * 1024 / buffers) / sizeof(float)) / 256 * 256;
}

// One half-wavecycle, from start up to end: its first sample past the zeros
// skipped before it (marker), the CDP buffer end it runs to or across (0 if
// none), its phase and its peak.
struct HalfCycle
{
    std::size_t start = 0;
    std::size_t end = 0;
    std::size_t marker = 0;
    std::size_t boundary = 0;
    int phase = 1;
    float cyclemax = 0.0F;
};

// do_distort's and mdistort's scan of a stream for half-wavecycles, sample
// by sample, with CDP's buffer ends: the zeros before a half-cycle belong to
// it, skipped within SMP_FLTERR at a buffer's start and exactly after a
// zero crossing; a half-cycle under way at a buffer's end runs into the
// next buffer, which starts afresh once the work begun across the end is
// finished. The callback given each half-cycle returns whether it finished
// that work (mdistort's divide goes on until its group of half-cycles
// ends).
class Scanner
{
public:
    void prepare(std::size_t bufferFrames) noexcept
    {
        bufferFrames_ = bufferFrames;
        reset();
    }

    void reset() noexcept
    {
        lastzero_ = 0;
        marker_ = 0;
        resolved_ = false;
        crossover_ = false;
        state_ = State::SkipNearZeros;
        phase_ = 1;
        cyclemax_ = 0.0F;
    }

    [[nodiscard]] std::size_t lastzero() const noexcept { return lastzero_; }

    // Ends the half-cycle under way at frame k, as if k crossed zero.
    template <typename End>
    void cut(std::size_t k, End&& end)
    {
        finish(k, end);
    }

    // Examines frame k, whose sample is b. pastOdd is what CDP reads one
    // sample past an odd buffer's end, where it finishes such a buffer
    // (after an even buffer it reads the next buffer's first sample, b).
    template <typename End>
    void examine(std::size_t k, float b, float pastOdd, End&& end)
    {
        // do_distort's end of a buffer, k the first frame of the next, with
        // zeros skipped to it.
        if (k != 0 && k % bufferFrames_ == 0 && state_ != State::HalfCycle && lastzero_ < k)
        {
            const auto even = ((k / bufferFrames_) - 1) % 2 == 0;
            const auto past = even ? b : pastOdd;
            if (state_ == State::SkipZeros)
            {
                // reset's zeros ran to the buffer's end.
                if (phase_ == 1 && past < 0.0F)
                    phase_ = -1;
                else if (phase_ == -1 && past > 0.0F)
                    phase_ = 1;
            }
            else
            {
                // The buffer's near zeros ran to its end.
                phase_ = past < 0.0F ? -1 : 1;
            }
            state_ = State::HalfCycle;
            marker_ = k;
            resolved_ = true;
        }
        for (;;)
        {
            if (state_ == State::SkipNearZeros)
            {
                if (smpflteq(b))
                    return;
                phase_ = b < 0.0F ? -1 : 1;
                resolve(k);
            }
            else if (state_ == State::SkipZeros)
            {
                if (b == 0.0F)
                    return;
                phase_ = b < 0.0F ? -1 : 1;
                resolve(k);
            }
            if (phase_ == 1 ? b > 0.0F : b < 0.0F)
            {
                if (phase_ == 1 ? b > cyclemax_ : b < cyclemax_)
                    cyclemax_ = b;
                return;
            }
            // The half-cycle ends at k, which starts the next.
            finish(k, end);
        }
    }

private:
    enum class State
    {
        // do_distort's start of a buffer: zeros within SMP_FLTERR skipped.
        SkipNearZeros,
        // reset's zeros skipped.
        SkipZeros,
        HalfCycle,
    };

    // tkglobals.h's SMP_FLTERR.
    [[nodiscard]] static bool smpflteq(double f1) noexcept
    {
        constexpr double smpFltErr = 0.0000005;
        return !((f1 > smpFltErr) || (f1 < -smpFltErr));
    }

    void resolve(std::size_t k) noexcept
    {
        state_ = State::HalfCycle;
        marker_ = k;
        resolved_ = true;
    }

    template <typename End>
    void finish(std::size_t k, End& end)
    {
        // A half-cycle under way at a buffer's end is reshaped across the two
        // buffers, and the next buffer starts afresh.
        const auto boundary = (k / bufferFrames_) * bufferFrames_;
        const auto crossing = boundary > lastzero_;
        const bool finished = end(
            HalfCycle{lastzero_, k, resolved_ ? marker_ : k, crossing ? boundary : 0, phase_, cyclemax_});
        crossover_ = crossover_ || crossing;
        lastzero_ = k;
        cyclemax_ = 0.0F;
        resolved_ = false;
        state_ = crossover_ && finished ? State::SkipNearZeros : State::SkipZeros;
        if (finished)
            crossover_ = false;
    }

    std::size_t bufferFrames_ = defaultBufferFrames(2);
    std::size_t lastzero_ = 0;
    std::size_t marker_ = 0;
    bool resolved_ = false;
    bool crossover_ = false;
    State state_ = State::SkipNearZeros;
    int phase_ = 1;
    float cyclemax_ = 0.0F;
};
}
