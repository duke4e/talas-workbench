/*
 * Derived from the CDP System, Release 8: dev/blur/blur.c and ap_blur.c,
 * dev/strange/strange.c and ap_strange.c, and dev/cdp2k/tklib1.c and
 * tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralGlis.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h.
constexpr double VERY_TINY_VAL = 0.00000000000000000001;
constexpr double SEMITONES_PER_OCTAVE = 12.0;
// dev/strange/strange.c and ap_strange.c.
constexpr double RATIO_LIMIT = 20.0;
constexpr double VERY_SMALL = 0.0000000000000001;
constexpr double ROLL_OFF = 500.0;

// rectify_window.
void rectify_window(std::span<float> frame) noexcept
{
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
    {
        if (frame[vc + 1] < 0.0)
            frame[vc + 1] = -frame[vc + 1];
    }
}

// get_totalamp.
[[nodiscard]] double get_totalamp(std::span<const float> frame) noexcept
{
    double totalamp = 0.0;
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        totalamp += frame[vc];
    return totalamp;
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

// pow(x, SMOOTHER) as CDP8's build computes it: a square root, +inf at
// -inf and +0 at -0.
[[nodiscard]] double smoother(double x) noexcept
{
    if (x == -std::numeric_limits<double>::infinity())
        return std::numeric_limits<double>::infinity();
    return std::fabs(std::sqrt(x));
}
}

bool BlurSpread::prepare(double sampleRate, int points, int overlap, bool pitchwise, int bands, bool quickSearch,
    double spread)
{
    // tklib1.c: the spread from 0 to 1.
    if (!std::isfinite(spread) || spread < 0.0 || spread > 1.0)
        return false;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    SpectralEnvelope envelope;
    if (!envelope.prepare(sampleRate, vocoder.frameSize(), pitchwise, bands, quickSearch))
        return false;
    vocoder_ = std::move(vocoder);
    envelope_ = std::move(envelope);
    startSpread_ = spread;
    reset();
    return true;
}

bool BlurSpread::setSpread(double spread) noexcept
{
    if (!prepared() || !std::isfinite(spread) || spread < 0.0 || spread > 1.0)
        return false;
    spread_ = spread;
    return true;
}

void BlurSpread::reset() noexcept
{
    vocoder_.reset();
    spread_ = startSpread_;
    frames_ = 0;
}

bool BlurSpread::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        // skip_or_special_operation_on_window_zero.
        if (frames_++ == 0)
            return;
        spreadFrame(frame);
    });
}

// specspread.
void BlurSpread::spreadFrame(std::span<float> frame) noexcept
{
    float* flbufptr = frame.data();
    const int clength = static_cast<int>(frame.size() / 2);
    int cc, vc;
    double specenv_amp, ampdiff, pre_totalamp, post_totalamp;
    rectify_window(frame);
    envelope_.extract(frame);
    pre_totalamp = get_totalamp(frame);
    for (cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        specenv_amp = envelope_.amplitudeAt((double)flbufptr[vc + 1]);
        ampdiff = specenv_amp - flbufptr[vc];
        // CDP8's build fuses the multiply and add.
        flbufptr[vc] = (float)std::fma(ampdiff, spread_, (double)flbufptr[vc]);
    }
    post_totalamp = get_totalamp(frame);
    normalise(frame, pre_totalamp, post_totalamp);
}

bool StrangeGlis::prepare(double sampleRate, int points, int overlap, Mode mode, bool pitchwise, int bands,
    bool quickSearch, double glisRate, double step, bool hasTop, double topFrequency)
{
    if (mode < Mode::Shepard || mode > Mode::Self)
        return false;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    SpectralEnvelope envelope;
    if (!envelope.prepare(sampleRate, vocoder.frameSize(), pitchwise, bands, quickSearch))
        return false;
    // The .ana header's arate and CDP's frame time; tklib1.c's ranges.
    const auto arate = (float)((float)sampleRate / (float)vocoder.decimation());
    const auto frametime = (float)(1.0 / arate);
    const double maximumRate = 8.0 / frametime;
    const double nyquist = envelope.nyquist();
    const double chwidth = envelope.channelWidth();
    if (!std::isfinite(glisRate) || glisRate < -maximumRate || glisRate > maximumRate
        || (mode == Mode::Inharmonic && (!std::isfinite(step) || step < chwidth || step > nyquist / 2))
        || (hasTop && (!std::isfinite(topFrequency) || topFrequency < minimumTopFrequency || topFrequency > nyquist)))
    {
        return false;
    }
    vocoder_ = std::move(vocoder);
    envelope_ = std::move(envelope);
    mode_ = mode;
    clength_ = static_cast<int>(vocoder_.frameSize() / 2);
    nyquist_ = nyquist;
    chwidth_ = chwidth;
    halfchwidth_ = envelope_.halfChannelWidth();
    maximumGlisRate_ = maximumRate;
    // establish_internal_params_for_glis (GLIS_REFERENCE_FRQ is chwidth *
    // 2.0).
    convertor_ = frametime / SEMITONES_PER_OCTAVE;
    shift_ = mode == Mode::Inharmonic ? step : 0.0;
    halfShift_ = shift_ / 2.0;
    refpitch_ = std::log(chwidth_ * 2.0);
    halfRefpitch_ = std::log(chwidth_ * 2.0 / 2.0);
    hasTop_ = hasTop;
    hifrq_ = hasTop ? topFrequency : nyquist;
    frqtopTop_ = 0.0;
    rollOff_ = 0.0;
    if (hasTop_)
    {
        const double top = hifrq_ + ROLL_OFF;
        frqtopTop_ = nyquist_ > top ? top : nyquist_; /* min(dz->nyquist,...) */
        const double rolloff = frqtopTop_ - hifrq_;
        rollOff_ = rolloff > ROLL_OFF ? ROLL_OFF : rolloff; /* min(...,ROLL_OFF) */
    }
    amp_.assign(static_cast<std::size_t>(clength_), 0.0F);
    freq_.assign(static_cast<std::size_t>(clength_) + 1, 0.0F);
    startGlisRate_ = glisRate;
    reset();
    return true;
}

bool StrangeGlis::setGlisRate(double glisRate) noexcept
{
    if (!prepared() || !std::isfinite(glisRate) || glisRate < -maximumGlisRate_ || glisRate > maximumGlisRate_)
        return false;
    glisRate_ = glisRate;
    return true;
}

void StrangeGlis::reset() noexcept
{
    vocoder_.reset();
    glisRate_ = startGlisRate_;
    basefrq_ = chwidth_ * 2.0;
    std::ranges::fill(amp_, 0.0F);
    std::ranges::fill(freq_, 0.0F);
    selfChannel_ = 0;
    frames_ = 0;
}

bool StrangeGlis::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        // skip_or_special_operation_on_window_zero.
        if (frames_++ == 0)
        {
            initialiseWindowFrqs(frame);
            return;
        }
        glisFrame(frame);
    });
}

// get_channel_corresponding_to_frq: -1 where CDP fails before setting the
// channel (a negative frequency or one above the Nyquist frequency).
int StrangeGlis::channelFor(double thisfrq) const noexcept
{
    if (thisfrq < 0.0 || thisfrq > nyquist_)
        return -1;
    return (int)((std::fabs(thisfrq) + halfchwidth_) / chwidth_); /* TRUNCATE */
}

// initialise_window_frqs: start frequencies for the first frame's channels.
void StrangeGlis::initialiseWindowFrqs(std::span<float> frame) const noexcept
{
    if (mode_ == Mode::Self)
        return;
    int cc, vc;
    double thisfrq = chwidth_ * 2.0;
    do
    {
        cc = (int)((thisfrq + halfchwidth_) / chwidth_); /* TRUNCATE */
        vc = cc * 2;
        if (vc + 1 < static_cast<int>(frame.size()))
            frame[static_cast<std::size_t>(vc) + 1] = (float)thisfrq;
        thisfrq += chwidth_;
    } while (thisfrq < nyquist_);
}

// specglis: CDP multiplies a breakpoint rate by GLIS_CONVERTOR at each
// frame and a fixed one once, alike.
void StrangeGlis::glisFrame(std::span<float> frame) noexcept
{
    const double rate = glisRate_ * convertor_;
    double pre_totalamp, post_totalamp;
    double scalefact;
    rectify_window(frame);
    envelope_.extract(frame);
    pre_totalamp = get_totalamp(frame);
    if (mode_ != Mode::Self)
    {
        // zero_sampbuf.
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            frame[vc] = 0.0F;
    }
    scalefact = establishFrqParams(rate);
    if (rate <= 0.0)
        writePartialsMovingDown(frame, scalefact);
    else
        writePartialsMovingUp(frame, scalefact);
    if (hasTop_)
    {
        // cut_or_rolloff_high_frq.
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        {
            if (frame[vc + 1] > hifrq_)
            {
                if (frame[vc + 1] >= frqtopTop_)
                    frame[vc] = 0.0F;
                else
                    frame[vc] = (float)(frame[vc] * (frqtopTop_ - frame[vc + 1]) / rollOff_);
            }
        }
    }
    post_totalamp = get_totalamp(frame);
    normalise(frame, pre_totalamp, post_totalamp);
}

// establish_frq_params: the glide moves GLIS_BASEFRQ, kept within the
// octave above (going up) or below (going down) the reference frequency;
// the fade of the alternate partials follows its place in that octave.
double StrangeGlis::establishFrqParams(double rate) noexcept
{
    const double reference = chwidth_ * 2.0; /* GLIS_REFERENCE_FRQ */
    double thispitch;
    // CDP8's build calls exp2 for pow(2.0,...).
    basefrq_ *= std::exp2(rate);
    if (rate >= 0.0)
    { /* GOING UP */
        while (basefrq_ > reference * 2.0)
            basefrq_ /= 2.0;
        thispitch = std::log(basefrq_);
        return thispitch - refpitch_;
    }
    /* GOING DOWN */
    while (basefrq_ < reference / 2.0)
        basefrq_ *= 2.0;
    thispitch = std::log(basefrq_);
    return thispitch - halfRefpitch_;
}

// write_partials_moving_down, with selfglis_down_within_spectral_envelope
// and glis_down_within_spectral_envelope.
void StrangeGlis::writePartialsMovingDown(std::span<float> frame, double scalefact) noexcept
{
    float* flbufptr = frame.data();
    const int wanted = static_cast<int>(frame.size());
    float* amp = amp_.data();
    float* freq = freq_.data();
    const double reference = chwidth_ * 2.0; /* GLIS_REFERENCE_FRQ */
    int n, cc, vc;
    if (mode_ == Mode::Self)
    {
        rectify_window(frame);
        const double transpos = basefrq_ / reference;
        const double lo = (transpos * 2.0) - 1.0;
        const double hi = 2.0 - (transpos * 2.0);
        const double smoothLo = smoother((0.0 > lo) ? 0.0 : lo); /* max(0.0,...) */
        const double smoothHi = smoother((0.0 > hi) ? 0.0 : hi);
        for (cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
        {
            amp[cc] = 0.0F;
            freq[cc] = flbufptr[vc + 1];
        }
        for (vc = 0; vc < wanted; vc += 2)
        {
            double ampratio, amphere, thisfrq, thisamp, frqhi, amphi;
            amphere = envelope_.amplitudeAt(flbufptr[vc + 1]);
            if (amphere <= VERY_SMALL)
                ampratio = 0.0;
            else
            {
                if ((ampratio = std::fabs(flbufptr[vc] / amphere)) > RATIO_LIMIT)
                    ampratio = RATIO_LIMIT;
            }
            if ((thisfrq = std::fabs((double)flbufptr[vc + 1]) * transpos) < nyquist_)
            {
                thisamp = envelope_.amplitudeAt(thisfrq);
                thisamp *= ampratio;
                thisamp *= smoothLo;
                n = channelFor(thisfrq);
                selfChannel_ = n;
                // CDP stores the amplitude at the channel and the frequency
                // one above it.
                if (n >= 0 && n < clength_ && thisamp > amp[n])
                {
                    amp[n++] = (float)thisamp;
                    freq[n] = (float)thisfrq;
                    selfChannel_ = n;
                }
            }
            if ((frqhi = thisfrq * 2.0) < nyquist_)
            {
                amphi = envelope_.amplitudeAt(frqhi);
                amphi *= ampratio;
                amphi *= smoothHi;
                // CDP looks up thisfrq's channel again, not frqhi's.
                n = channelFor(thisfrq);
                selfChannel_ = n;
                if (n >= 0 && n < clength_ && amphi > amp[n])
                {
                    amp[n++] = (float)amphi;
                    freq[n] = (float)frqhi;
                }
            }
        }
        // put_amp_and_frq.
        for (cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
        {
            flbufptr[vc] = amp[cc];
            flbufptr[vc + 1] = freq[cc];
        }
        return;
    }
    // SHEPARD and INHARMONIC: from the fundamental, the odd partials faded.
    const double fundamental = basefrq_;
    double thisfrq = basefrq_;
    n = 1;
    while (thisfrq < nyquist_)
    {
        cc = channelFor(thisfrq);
        vc = cc * 2;
        flbufptr[vc + 1] = (float)thisfrq;
        flbufptr[vc] = (float)envelope_.amplitudeAt(thisfrq);
        if (n & 1)
            flbufptr[vc] = (float)(flbufptr[vc] * scalefact);
        n++;
        if (mode_ == Mode::Inharmonic)
            thisfrq += shift_;
        else
            thisfrq = fundamental * (double)n;
    }
}

// write_partials_moving_up, with selfglis_up_within_spectral_envelope and
// glis_up_within_spectral_envelope.
void StrangeGlis::writePartialsMovingUp(std::span<float> frame, double scalefact) noexcept
{
    float* flbufptr = frame.data();
    const int wanted = static_cast<int>(frame.size());
    float* amp = amp_.data();
    float* freq = freq_.data();
    const double reference = chwidth_ * 2.0; /* GLIS_REFERENCE_FRQ */
    int n, cc, vc;
    if (mode_ == Mode::Self)
    {
        const double transpos = basefrq_ / reference;
        const double smoothHere = smoother(2.0 - transpos);
        const double smoothLo = smoother(transpos - 1.0);
        for (cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
        {
            amp[cc] = 0.0F;
            freq[cc] = flbufptr[vc + 1];
        }
        for (vc = 0; vc < wanted; vc += 2)
        {
            double amphere, ampratio;
            double thisfrq, thisamp, frqlo, amplo;
            amphere = envelope_.amplitudeAt(flbufptr[vc + 1]);
            if (amphere <= VERY_SMALL)
                ampratio = 0.0;
            else
            {
                if ((ampratio = std::fabs(flbufptr[vc] / amphere)) > RATIO_LIMIT)
                    ampratio = RATIO_LIMIT;
            }
            if ((thisfrq = std::fabs((double)flbufptr[vc + 1]) * transpos) < nyquist_)
            {
                thisamp = envelope_.amplitudeAt(thisfrq);
                thisamp *= ampratio;
                thisamp *= smoothHere;
                n = channelFor(thisfrq);
                selfChannel_ = n;
                if (n >= 0 && n < clength_ && thisamp > amp[n])
                {
                    amp[n] = (float)thisamp;
                    freq[n] = (float)thisfrq;
                }
            }
            if ((frqlo = thisfrq / 2.0) < nyquist_)
            {
                amplo = envelope_.amplitudeAt(frqlo);
                amplo *= ampratio;
                amplo *= smoothLo;
                // CDP ignores the lookup's failure above the Nyquist
                // frequency and uses the channel it last found.
                if (const int found = channelFor(thisfrq); found >= 0)
                    selfChannel_ = found;
                n = selfChannel_;
                if (n < clength_ && amplo > amp[n])
                {
                    amp[n] = (float)amplo;
                    freq[n] = (float)frqlo;
                }
            }
        }
        // put_amp_and_frq.
        for (cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
        {
            flbufptr[vc] = amp[cc];
            flbufptr[vc + 1] = freq[cc];
        }
        return;
    }
    // SHEPARD and INHARMONIC: from the second harmonic of a fundamental an
    // octave below, the lowest and odd partials faded.
    const double fundamental = basefrq_ / 2.0;
    double thisfrq = basefrq_;
    n = 2;
    while (thisfrq < nyquist_)
    {
        cc = channelFor(thisfrq);
        vc = cc * 2;
        flbufptr[vc + 1] = (float)thisfrq;
        flbufptr[vc] = (float)envelope_.amplitudeAt(thisfrq);
        if ((n & 1) || n == 2)
            flbufptr[vc] = (float)(flbufptr[vc] * scalefact);
        n++;
        if (mode_ == Mode::Inharmonic)
            thisfrq += halfShift_;
        else
            thisfrq = fundamental * (double)n;
    }
}
}
