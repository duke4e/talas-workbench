/*
 * Derived from the CDP System, Release 8: dev/focus/focus.c and
 * ap_focus.c, and dev/cdp2k/tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralFocus.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h.
constexpr double VERY_TINY_VAL = 0.00000000000000000001;
// dev/include/speccon.h.
constexpr double SPEC_MINFRQ = 10.0;

// setup_pitchzero: a breakpoint frequency under SPEC_MINFRQ is raised to
// it, a fixed one is zero.
[[nodiscard]] double pitchzero(double frequency, bool live) noexcept
{
    if (frequency < SPEC_MINFRQ)
        return live ? SPEC_MINFRQ : 0.0;
    return frequency;
}

// normalise, after a process that changes a frame's total amplitude.
void normalise(std::span<float> frame, double pre_totalamp, double post_totalamp) noexcept
{
    if (post_totalamp < VERY_TINY_VAL)
        return;
    const auto normaliser = pre_totalamp / post_totalamp;
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        frame[vc] = (float)(frame[vc] * normaliser);
}
}

bool FocusExaggerate::prepare(double sampleRate, int points, int overlap, double exaggeration)
{
    if (!std::isfinite(exaggeration) || exaggeration < minimumExaggeration
        || exaggeration > maximumExaggeration)
    {
        return false;
    }
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    startExaggeration_ = exaggeration;
    reset();
    return true;
}

bool FocusExaggerate::setExaggeration(double exaggeration) noexcept
{
    if (!prepared() || !std::isfinite(exaggeration) || exaggeration < minimumExaggeration
        || exaggeration > maximumExaggeration)
    {
        return false;
    }
    exaggeration_ = exaggeration;
    return true;
}

void FocusExaggerate::reset() noexcept
{
    vocoder_.reset();
    exaggeration_ = startExaggeration_;
    frames_ = 0;
}

bool FocusExaggerate::process(std::span<const double> input, std::span<double> output) noexcept
{
    // convert_params_for_exag's reciprocal.
    const auto power = 1.0 / exaggeration_;
    return vocoder_.process(input, output, [this, power](std::span<float> frame) {
        // skip_or_special_operation_on_window_zero leaves the first frame.
        if (frames_++ == 0)
            return;
        // specexag.
        double post_totalamp = 0.0, pre_totalamp = 0.0;
        double maxamp = 0.0, normaliser;
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        {
            pre_totalamp += frame[vc];
            if (frame[vc] > maxamp)
                maxamp = frame[vc];
        }
        if (maxamp <= 0.0)
            return;
        normaliser = 1.0 / maxamp;
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        {
            frame[vc] = (float)(frame[vc] * normaliser);
            frame[vc] = (float)(std::pow(frame[vc], power));
            post_totalamp += frame[vc];
        }
        normalise(frame, pre_totalamp, post_totalamp);
    });
}

bool FocusFold::prepare(double sampleRate, int points, int overlap, double lowFrequency,
    double highFrequency, bool withBody, bool liveLow, bool liveHigh)
{
    const auto nyquist = sampleRate / 2.0;
    if (!std::isfinite(lowFrequency) || !std::isfinite(highFrequency) || lowFrequency < minimumFrequency
        || highFrequency < minimumFrequency || lowFrequency > nyquist || highFrequency > nyquist)
    {
        return false;
    }
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    const auto clength = static_cast<int>(vocoder_.frameSize() / 2);
    nyquist_ = nyquist;
    chwidth_ = nyquist_ / (double)(clength - 1);
    halfchwidth_ = chwidth_ / 2.0;
    startLow_ = lowFrequency;
    startHigh_ = highFrequency;
    withBody_ = withBody;
    liveLow_ = liveLow;
    liveHigh_ = liveHigh;
    reset();
    return true;
}

bool FocusFold::setFrequencies(double lowFrequency, double highFrequency) noexcept
{
    if (!prepared() || !std::isfinite(lowFrequency) || !std::isfinite(highFrequency)
        || lowFrequency < minimumFrequency || highFrequency < minimumFrequency || lowFrequency > nyquist_
        || highFrequency > nyquist_)
    {
        return false;
    }
    low_ = pitchzero(lowFrequency, liveLow_);
    high_ = pitchzero(highFrequency, liveHigh_);
    return true;
}

void FocusFold::reset() noexcept
{
    vocoder_.reset();
    low_ = pitchzero(startLow_, liveLow_);
    high_ = pitchzero(startHigh_, liveHigh_);
    // fold_preprocess, from the fixed frequencies as given.
    if (!liveHigh_)
    {
        hichan_ = (int)((high_ + halfchwidth_) / chwidth_);
        if (hichan_ != 0)
            hichan_ <<= 1;
    }
    if (!liveLow_)
    {
        lochan_ = (int)((low_ + halfchwidth_) / chwidth_);
        if (lochan_ != 0)
            lochan_ <<= 1;
    }
}

bool FocusFold::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) { fold(frame); });
}

// specfold.
void FocusFold::fold(std::span<float> frame) noexcept
{
    double hifrq_limit = high_;
    double lofrq_limit = low_;
    if (hifrq_limit < lofrq_limit)
        std::swap(lofrq_limit, hifrq_limit);
    if (liveHigh_)
    {
        hichan_ = (int)((hifrq_limit + halfchwidth_) / chwidth_);
        if (hichan_ != 0)
            hichan_ <<= 1;
    }
    if (liveLow_)
    {
        lochan_ = (int)((lofrq_limit + halfchwidth_) / chwidth_);
        if (lochan_ != 0)
            lochan_ <<= 1;
    }
    // do_fold.
    for (int vc = 0; vc < static_cast<int>(frame.size()); vc += 2)
    {
        double thisfrq = std::fabs((double)frame[static_cast<std::size_t>(vc) + 1]);
        if (thisfrq < 1.0)
        {
            frame[static_cast<std::size_t>(vc)] = 0.0f;
            continue;
        }
        if (thisfrq < lofrq_limit)
        {
            while (thisfrq < lofrq_limit)
                thisfrq *= 2.0;
            if (thisfrq < hifrq_limit)
                foldIn(frame, vc, thisfrq);
            frame[static_cast<std::size_t>(vc)] = 0.0f;
        }
        else if (thisfrq > hifrq_limit)
        {
            while (thisfrq > hifrq_limit)
                thisfrq /= 2.0;
            if (thisfrq > lofrq_limit)
                foldIn(frame, vc, thisfrq);
            frame[static_cast<std::size_t>(vc)] = 0.0f;
        }
    }
}

// fold_in.
void FocusFold::foldIn(std::span<float> frame, int vc, double thisfrq) noexcept
{
    constexpr int THISCHANSCAN = 4;
    const auto amp = [&](int index) -> float& { return frame[static_cast<std::size_t>(index)]; };
    const int truecc = (int)((thisfrq + halfchwidth_) / chwidth_);
    int truevc = truecc * 2;
    if (!withBody_)
    {
        if (amp(vc) > amp(truevc))
        {
            amp(truevc++) = amp(vc);
            amp(truevc) = amp(vc + 1);
        }
        return;
    }
    if (amp(vc) > amp(truevc))
    {
        amp(truevc++) = amp(vc);
        amp(truevc) = amp(vc + 1);
        return;
    }
    const int minscan = std::max(lochan_ - 2, truevc - (THISCHANSCAN * 2));
    const int maxscan = std::min(hichan_ + 2, truevc + (THISCHANSCAN * 2));
    for (int n = truevc - 2; n > minscan; n -= 2)
    {
        if (amp(vc) > amp(n))
        {
            amp(n++) = amp(vc);
            amp(n) = amp(vc + 1);
            return;
        }
    }
    for (int n = truevc + 2; n < maxscan; n += 2)
    {
        if (amp(vc) > amp(n))
        {
            amp(n++) = amp(vc);
            amp(n) = amp(vc + 1);
            return;
        }
    }
}

double FocusAccumulate::maximumGlissando(double sampleRate, int points, int overlap) noexcept
{
    const auto d = PhaseVocoder::decimationFor(points, overlap);
    if (!d.has_value() || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return 0.0;
    // The frame time CDP reads from the .ana file: one over its frame rate.
    const auto arate = (float)((float)sampleRate / (float)*d);
    const auto frametime = (float)(1.0 / arate);
    return maximumGlissPerFrame / frametime;
}

bool FocusAccumulate::valid(double decay, double glissando) const noexcept
{
    const auto limit = maximumGlissPerFrame / frametime_;
    return std::isfinite(decay) && std::isfinite(glissando) && decay >= minimumDecay && decay <= 1.0
        && glissando >= -limit && glissando <= limit;
}

bool FocusAccumulate::prepare(double sampleRate, int points, int overlap, double decay, double glissando)
{
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    const auto arate = (float)((float)sampleRate / (float)vocoder.decimation());
    const auto frametime = (float)(1.0 / arate);
    const auto previous = frametime_;
    frametime_ = frametime;
    if (!valid(decay, glissando))
    {
        frametime_ = previous;
        return false;
    }
    vocoder_ = std::move(vocoder);
    windowbuf_.assign(vocoder_.frameSize(), 0.0F);
    startDecay_ = decay;
    startGlissando_ = glissando;
    reset();
    return true;
}

bool FocusAccumulate::setControls(double decay, double glissando) noexcept
{
    if (!prepared() || !valid(decay, glissando))
        return false;
    decay_ = decay;
    glissando_ = glissando;
    return true;
}

void FocusAccumulate::reset() noexcept
{
    vocoder_.reset();
    std::ranges::fill(windowbuf_, 0.0F);
    decay_ = startDecay_;
    glissando_ = startGlissando_;
    frames_ = 0;
}

bool FocusAccumulate::process(std::span<const double> input, std::span<double> output) noexcept
{
    // scale_accu_parameters: decay ratio a second to a ratio a frame, and
    // glissando in octaves a second to a frequency ratio a frame.
    const auto dindex = std::exp(std::log(decay_) * frametime_);
    const auto gindex = std::pow(2.0, glissando_ * frametime_);
    return vocoder_.process(input, output, [this, dindex, gindex](std::span<float> frame) {
        float* windowbuf = windowbuf_.data();
        if (frames_++ == 0)
        {
            // skip_or_special_operation_on_window_zero.
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
                windowbuf[vc] = frame[vc];
            return;
        }
        // specaccu, whose glissando test reads ACCU_GLIS, the frequency
        // ratio, which is always positive.
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        {
            if (dindex < 1.0)
                windowbuf[vc] = (float)(windowbuf[vc] * dindex);
            windowbuf[vc + 1] = (float)(windowbuf[vc + 1] * gindex);
            // rwd_accumulate.
            if (frame[vc] > windowbuf[vc])
            {
                windowbuf[vc] = frame[vc];
                windowbuf[vc + 1] = frame[vc + 1];
            }
            else
            {
                frame[vc] = windowbuf[vc];
                frame[vc + 1] = windowbuf[vc + 1];
            }
        }
    });
}

bool FocusStep::prepare(double sampleRate, int points, int overlap, double step)
{
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    // The frame time CDP reads from the .ana file, and step_preprocess's
    // conversion of the step to frames.
    const auto arate = (float)((float)sampleRate / (float)vocoder.decimation());
    const auto frametime = (float)(1.0 / arate);
    if (!std::isfinite(step) || step < 2.0 * frametime || step > maximumStep)
        return false;
    vocoder_ = std::move(vocoder);
    step_ = static_cast<long long>(std::round(step / frametime));
    held_.assign(vocoder_.frameSize(), 0.0F);
    reset();
    return true;
}

void FocusStep::reset() noexcept
{
    vocoder_.reset();
    std::ranges::fill(held_, 0.0F);
    frames_ = 0;
}

bool FocusStep::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        // do_specstep: each step's first frame, then copies of it.
        if (frames_++ % step_ == 0)
            std::ranges::copy(frame, held_.begin());
        else
            std::ranges::copy(held_, frame.begin());
    });
}
}
