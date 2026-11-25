/*
 * Derived from the CDP System, Release 8: dev/standnew/distmore.c, clip.c
 * and quirk.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/WavesetLocal.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// The ring holds the wait and the frame arriving.
constexpr std::size_t ringFrames = 4096;

// Each process finds its (half-)wavesets its own way, sample by sample,
// across its buffers alike. half_cycles::Scanner, do_distort's scan, would
// end a half-cycle at a zero and give the zeros to the next, and restart
// at CDP distort's buffer ends: clip keeps zeros with the half-waveset
// before them, quirk with the positive side, and distmore with either.

[[nodiscard]] bool finite(std::span<const double> input) noexcept
{
    return std::ranges::all_of(input, [](double sample) {
        return std::isfinite(sample);
    });
}
}

bool DistmoreDouble::prepare(int multiplier)
{
    // check_distmore_param_validity_and_consistency's group, 2 to the power
    // of the multiplier: only a group of 2 keeps each waveset's length.
    if (multiplier != 1)
        return false;
    ring_.assign(ringFrames, 0.0F);
    mask_ = ringFrames - 1;
    reset();
    return true;
}

void DistmoreDouble::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    frame_ = 0;
    start_ = 0;
    phase_ = 0;
    secondHalf_ = false;
}

bool DistmoreDouble::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (ring_.empty() || input.size() != output.size() || !finite(input))
        return false;
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto k = frame_;
        const auto b = static_cast<float>(input[index]);
        at(k) = b;
        // The stream waits no longer for a waveset to end.
        if (k - start_ >= waitFrames)
            doubleWaveset(k);
        // distdouble: zeros keep the phase; the second change of phase, the
        // signal back on its first side, ends the waveset, the sample that
        // changed it starting the next.
        if (phase_ == 0 && b != 0.0F)
            phase_ = b > 0.0F ? 1 : -1;
        const auto firstSide = phase_ >= 0 ? b >= 0.0F : b <= 0.0F;
        const auto secondSide = phase_ >= 0 ? b <= 0.0F : b >= 0.0F;
        if (secondHalf_ || !firstSide)
        {
            secondHalf_ = true;
            if (!secondSide)
            {
                secondHalf_ = false;
                doubleWaveset(k);
            }
        }
        output[index] = k >= waitFrames ? static_cast<double>(at(k - waitFrames)) : 0.0;
        ++frame_;
    }
    return true;
}

// distdouble's group of 2, statement for statement, in the ring: every
// other sample copied to the waveset's start, then that copy again (from
// its second sample for an odd length).
void DistmoreDouble::doubleWaveset(std::size_t end) noexcept
{
    const auto wavestart = start_;
    const auto wbufpos = static_cast<int>(end - wavestart);
    start_ = end;
    const auto wbuf = [this, wavestart](int index) -> float& {
        return at(wavestart + static_cast<std::size_t>(index));
    };
    int j = 0;
    int k = 0;
    int jj = 0;
    if ((wbufpos & 1) == 0)
    {
        for (k = 0, j = 0; k < wbufpos; k += 2, j++)
            wbuf(j) = wbuf(k);
        jj = j;
        for (k = 0; k < jj; k++, j++)
            wbuf(j) = wbuf(k);
    }
    else
    {
        for (k = 0, j = 0; k < wbufpos; k += 2, j++)
            wbuf(j) = wbuf(k);
        jj = j;
        for (k = 1; k < jj; k++, j++)
            wbuf(j) = wbuf(k);
    }
}

bool ClipWaveset::prepare(double fraction)
{
    // check_clip_param_validity_and_consistency refuses a fraction of 0.
    if (!std::isfinite(fraction) || fraction <= 0.0 || fraction > 1.0)
        return false;
    fraction_ = fraction;
    gain_ = 1.0 / fraction;
    ring_.assign(ringFrames, 0.0F);
    mask_ = ringFrames - 1;
    reset();
    return true;
}

void ClipWaveset::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    frame_ = 0;
    start_ = 0;
    phase_ = 0;
    maxsamp_ = 0.0F;
    minsamp_ = 0.0F;
}

bool ClipWaveset::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (ring_.empty() || input.size() != output.size() || !finite(input))
        return false;
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto k = frame_;
        const auto b = static_cast<float>(input[index]);
        at(k) = b;
        // The stream waits no longer for a half-waveset to end: the part
        // so far is clipped at its peak, the rest from its own.
        if (k - start_ >= waitFrames)
        {
            clipHalfWaveset(k);
            if (phase_ == 1)
                maxsamp_ = 0.0F;
            else if (phase_ == -1)
                minsamp_ = 0.0F;
        }
        // clip's mode 2, statement for statement.
        switch (phase_)
        {
        case 0:
            if (b > maxsamp_)
            {
                maxsamp_ = b;
                phase_ = 1;
            }
            else if (b < minsamp_)
            {
                minsamp_ = b;
                phase_ = -1;
            }
            break;
        case 1:
            if (b > 0)
            {
                if (b > maxsamp_)
                    maxsamp_ = b;
            }
            else if (b < 0)
            {
                clipHalfWaveset(k);
                minsamp_ = b;
                maxsamp_ = 0;
                phase_ = -1;
            }
            break;
        default:
            if (b < 0)
            {
                if (b < minsamp_)
                    minsamp_ = b;
            }
            else if (b > 0)
            {
                clipHalfWaveset(k);
                maxsamp_ = b;
                minsamp_ = 0;
                phase_ = 1;
            }
            break;
        }
        // clip's gain over the whole signal, as it writes its buffer.
        output[index] = k >= waitFrames
            ? static_cast<double>(static_cast<float>(static_cast<double>(at(k - waitFrames)) * gain_))
            : 0.0;
        ++frame_;
    }
    return true;
}

// clip's clipping of the half-waveset from halfwavsetstart up to the sample
// that changes phase.
void ClipWaveset::clipHalfWaveset(std::size_t end) noexcept
{
    if (phase_ == 1)
    {
        const auto localmax = static_cast<float>(static_cast<double>(maxsamp_) * fraction_);
        for (auto j = start_; j < end; j++)
        {
            if (at(j) > localmax)
                at(j) = localmax;
        }
    }
    else if (phase_ == -1)
    {
        const auto localmin = static_cast<float>(static_cast<double>(minsamp_) * fraction_);
        for (auto j = start_; j < end; j++)
        {
            if (at(j) < localmin)
                at(j) = localmin;
        }
    }
    start_ = end;
}

bool QuirkWaveset::prepare(double exponent)
{
    if (!std::isfinite(exponent) || exponent < minimumExponent || exponent > maximumExponent)
        return false;
    powfac_ = exponent;
    ring_.assign(ringFrames, 0.0F);
    mask_ = ringFrames - 1;
    reset();
    return true;
}

void QuirkWaveset::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    frame_ = 0;
    start_ = 0;
    // quirk_param_preprocess starts on the first sample's side, its peak at
    // 0; a file starting below zero (after any zeros, which count as
    // positive) changes side at once, as here.
    positive_ = true;
    maxsamp_ = 0.0F;
}

bool QuirkWaveset::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (ring_.empty() || input.size() != output.size() || !finite(input))
        return false;
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto k = frame_;
        const auto b = static_cast<float>(input[index]);
        at(k) = b;
        // The stream waits no longer for a half-waveset to end: the part so
        // far takes its peak, the rest its own.
        if (k - start_ >= waitFrames)
        {
            powerHalfWaveset(k);
            maxsamp_ = 0.0F;
        }
        // quirk_param_preprocess's half-wavesets: samples at or above zero,
        // then below, each starting its peak at its first sample.
        const auto positive = b >= 0.0F;
        if (positive != positive_)
        {
            powerHalfWaveset(k);
            maxsamp_ = b;
            positive_ = positive;
        }
        else if (positive ? b > maxsamp_ : b < maxsamp_)
            maxsamp_ = b;
        output[index] = k >= waitFrames ? static_cast<double>(at(k - waitFrames)) : 0.0;
        ++frame_;
    }
    return true;
}

// quirk's mode 1, statement for statement: each sample over the peak (a
// float division), raised to the power, back to the peak.
void QuirkWaveset::powerHalfWaveset(std::size_t end) noexcept
{
    const auto localmaxsamp = maxsamp_;
    for (auto j = start_; j < end; j++)
    {
        if (localmaxsamp != 0.0F)
        {
            double samp = static_cast<double>(at(j) / localmaxsamp);
            samp = std::pow(samp, powfac_);
            samp *= static_cast<double>(localmaxsamp);
            at(j) = static_cast<float>(samp);
        }
    }
    start_ = end;
}
}
