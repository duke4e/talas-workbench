/*
 * Derived from the CDP System, Release 8: dev/blur/blur.c and ap_blur.c,
 * dev/cdp2k/tklib3.c and dev/sfsys/osbind.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralChorus.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h.
constexpr double VERY_TINY_VAL = 0.00000000000000000001;
// ap_blur.c: frames' floats in each random table.
constexpr int RSIZE = 4;
// dev/include/speccon.h.
constexpr int BOTH_ACCESSES_OF_RANDTABLE_REACH_END_TOGETHER = 0;
constexpr int FIRST_ACCESS_OF_RANDTABLE_REACHES_END_FIRST = 1;
constexpr int SECOND_ACCESS_OF_RANDTABLE_REACHES_END_FIRST = 2;

using Mode = BlurChorus::Mode;

[[nodiscard]] bool readsAmplitude(Mode mode) noexcept
{
    return mode == Mode::Amplitude || mode >= Mode::AmplitudeAndFrequency;
}

[[nodiscard]] bool readsFrequency(Mode mode) noexcept
{
    return mode != Mode::Amplitude;
}

[[nodiscard]] bool validSpreads(Mode mode, double amplitudeSpread, double frequencySpread) noexcept
{
    if (readsAmplitude(mode)
        && (!std::isfinite(amplitudeSpread) || amplitudeSpread < BlurChorus::minimumAmplitudeSpread
            || amplitudeSpread > BlurChorus::maximumAmplitudeSpread))
    {
        return false;
    }
    return !readsFrequency(mode)
        || (std::isfinite(frequencySpread) && frequencySpread >= BlurChorus::minimumFrequencySpread
            && frequencySpread <= BlurChorus::maximumFrequencySpread);
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
}

bool BlurChorus::valid(double amplitudeSpread, double frequencySpread) const noexcept
{
    return validSpreads(mode_, amplitudeSpread, frequencySpread);
}

bool BlurChorus::prepare(double sampleRate, int points, int overlap, Mode mode, double amplitudeSpread,
    double frequencySpread, bool liveAmplitudeSpread, bool liveFrequencySpread)
{
    if (mode < Mode::Amplitude || mode > Mode::AmplitudeAndFrequencyDown
        || !validSpreads(mode, amplitudeSpread, frequencySpread))
    {
        return false;
    }
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    const auto wanted = static_cast<int>(vocoder.frameSize());
    const auto clength = wanted / 2;
    // chor_preprocess and setup_randtables_for_rapid_rand_processing: the
    // amplitude table, then the frequency table, from CDP's unseeded
    // drand48. CDP's fixed spreads (no breakpoint file) turn each table into
    // the spread's powers at once; the port takes the powers at each frame,
    // as CDP does for breakpoint spreads, to the same products.
    const auto rtabsize = static_cast<std::size_t>(wanted * RSIZE);
    Drand48 random;
    std::vector<double> rtaba;
    std::vector<double> rtabf;
    double z;
    if (readsAmplitude(mode))
    {
        rtaba.resize(rtabsize);
        for (std::size_t n = 0; n < rtabsize; n++)
        {
            z = random.next() * 2.0;
            rtaba[n] = z - 1.0;
        }
    }
    if (readsFrequency(mode))
    {
        rtabf.resize(rtabsize);
        switch (mode)
        {
        case Mode::FrequencyDown:
        case Mode::AmplitudeAndFrequencyDown:
            for (std::size_t n = 0; n < rtabsize; n++)
            {
                z = random.next();
                rtabf[n] = z - 1.0;
            }
            break;
        case Mode::FrequencyUp:
        case Mode::AmplitudeAndFrequencyUp:
            for (std::size_t n = 0; n < rtabsize; n++)
                rtabf[n] = random.next();
            break;
        default:
            for (std::size_t n = 0; n < rtabsize; n++)
            {
                z = random.next() * 2.0;
                rtabf[n] = z - 1.0;
            }
            break;
        }
    }
    vocoder_ = std::move(vocoder);
    mode_ = mode;
    nyquist_ = sampleRate / 2.0;
    chwidth_ = nyquist_ / (double)(clength - 1);
    halfchwidth_ = chwidth_ / 2.0;
    rtaba_ = std::move(rtaba);
    rtabf_ = std::move(rtabf);
    startRandom_ = random;
    // set_chorus_processing_type's AF_VAR, both spreads from breakpoint
    // files, where do_a_f takes getnewfrq_choru, made for a table of powers,
    // and scales each frequency by the bare random number.
    bareFrequencyFactor_ = mode >= Mode::AmplitudeAndFrequency && liveAmplitudeSpread && liveFrequencySpread;
    windowbuf_.assign(static_cast<std::size_t>(wanted), 0.0F);
    // A spread the mode does not read stays at CDP's default.
    startAmplitudeSpread_ = readsAmplitude(mode) ? amplitudeSpread : 2.0;
    startFrequencySpread_ = readsFrequency(mode) ? frequencySpread : 1.1;
    reset();
    return true;
}

bool BlurChorus::setSpreads(double amplitudeSpread, double frequencySpread) noexcept
{
    if (!prepared() || !valid(amplitudeSpread, frequencySpread))
        return false;
    if (readsAmplitude(mode_))
        amplitudeSpread_ = amplitudeSpread;
    if (readsFrequency(mode_))
        frequencySpread_ = frequencySpread;
    return true;
}

void BlurChorus::reset() noexcept
{
    vocoder_.reset();
    random_ = startRandom_;
    amplitudeSpread_ = startAmplitudeSpread_;
    frequencySpread_ = startFrequencySpread_;
}

bool BlurChorus::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) { chorus(frame); });
}

// specchorus and chorusing.
void BlurChorus::chorus(std::span<float> frame) noexcept
{
    const auto wanted = static_cast<int>(frame.size());
    const auto clength = wanted / 2;
    const auto rtabsize = wanted * RSIZE;
    float* flbufptr = frame.data();
    float* windowbuf = windowbuf_.data();
    const double* rtaba = rtaba_.data();
    const double* rtabf = rtabf_.data();
    const auto ampr = amplitudeSpread_;
    const auto frqr = frequencySpread_;
    const auto both = mode_ >= Mode::AmplitudeAndFrequency;
    const auto amplitude = readsAmplitude(mode_);
    int cc, cc2 = 0, order = 0, vc;
    int randhere, randhere2 = 0, randend, randend2 = 0;
    double pre_totalamp = 0.0;
    // get_randplace.
    randhere = (int)(random_.next() * rtabsize);
    if ((randend = randhere + clength) >= rtabsize)
        randend = rtabsize;
    cc = randhere;
    if (both)
    {
        // get_other_randplace.
        randhere2 = (int)(random_.next() * rtabsize);
        if ((randend2 = randhere2 + clength) > rtabsize)
            randend2 = rtabsize;
        const auto firstlen = randend - randhere;
        const auto firstlen2 = randend2 - randhere2;
        if (firstlen < firstlen2)
            order = FIRST_ACCESS_OF_RANDTABLE_REACHES_END_FIRST;
        else if (firstlen2 < firstlen)
            order = SECOND_ACCESS_OF_RANDTABLE_REACHES_END_FIRST;
        else
            order = BOTH_ACCESSES_OF_RANDTABLE_REACH_END_TOGETHER;
        cc2 = randhere2;
    }
    if (amplitude)
        pre_totalamp = get_totalamp(frame);
    // do_a: the amplitude by the spread to the random power.
    const auto scaleAmp = [&](int c, int v) { flbufptr[v] = (float)(std::pow(ampr, rtaba[c]) * flbufptr[v]); };
    // getnewfrq2_choru: the frequency by the spread to the random power, the
    // partial taking the channel of its new frequency if louder than what it
    // holds there; getnewfrq_choru, for AF_VAR, by the random number itself.
    const auto bare = bareFrequencyFactor_;
    const auto scatterFrq = [&](int c, int v) {
        float amp;
        double newval, newscat = bare ? rtabf[c] : std::pow(frqr, rtabf[c]), frq;
        int new_vc;
        newval = flbufptr[v] * newscat;
        if (newval > nyquist_ || newval < halfchwidth_)
            return;
        // get_sbufposition_corresponding_to_frq.
        frq = newval + halfchwidth_;
        new_vc = (int)(frq / chwidth_);
        new_vc = std::min(clength - 1, std::max(0, new_vc));
        new_vc *= 2;
        if ((amp = flbufptr[v - 1]) > windowbuf[new_vc])
        {
            windowbuf[new_vc++] = amp;
            windowbuf[new_vc] = (float)newval;
        }
    };
    // do_a_f and its kin: a channel's amplitude, then its frequency.
    const auto scaleBoth = [&] {
        scaleAmp(cc, vc);
        cc++;
        vc++;
        scatterFrq(cc2, vc);
        cc2++;
        vc++;
    };
    if (!both && amplitude)
    {
        // do_a.
        vc = 0;
        while (cc < randend)
        {
            scaleAmp(cc, vc);
            cc++;
            vc += 2;
        }
        cc = 0;
        while (vc < wanted)
        {
            scaleAmp(cc, vc);
            cc++;
            vc += 2;
        }
    }
    else if (!both)
    {
        // do_fvar.
        vc = 1;
        std::fill(windowbuf_.begin(), windowbuf_.end(), 0.0F);
        while (cc < randend)
        {
            scatterFrq(cc, vc);
            cc++;
            vc += 2;
        }
        cc = 0;
        while (vc < wanted)
        {
            scatterFrq(cc, vc);
            cc++;
            vc += 2;
        }
        std::copy(windowbuf_.begin(), windowbuf_.end(), frame.begin());
    }
    else
    {
        vc = 0;
        std::fill(windowbuf_.begin(), windowbuf_.end(), 0.0F);
        switch (order)
        {
        case BOTH_ACCESSES_OF_RANDTABLE_REACH_END_TOGETHER:
            while (cc < randend)
                scaleBoth();
            cc = cc2 = 0;
            while (vc < wanted)
                scaleBoth();
            break;
        case FIRST_ACCESS_OF_RANDTABLE_REACHES_END_FIRST:
            while (cc < randend)
                scaleBoth();
            cc = 0;
            while (cc2 < randend2)
                scaleBoth();
            cc2 = 0;
            while (vc < wanted)
                scaleBoth();
            break;
        default:
            while (cc2 < randend2)
                scaleBoth();
            cc2 = 0;
            while (cc < randend)
                scaleBoth();
            cc = 0;
            while (vc < wanted)
                scaleBoth();
            break;
        }
        std::copy(windowbuf_.begin(), windowbuf_.end(), frame.begin());
    }
    if (amplitude)
        normalise(frame, pre_totalamp, get_totalamp(frame));
}

bool BlurNoise::prepare(double sampleRate, int points, int overlap, double noise)
{
    if (!std::isfinite(noise) || noise < 0.0 || noise > 1.0)
        return false;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    startNoise_ = noise;
    reset();
    return true;
}

bool BlurNoise::setNoise(double noise) noexcept
{
    if (!prepared() || !std::isfinite(noise) || noise < 0.0 || noise > 1.0)
        return false;
    noise_ = noise;
    return true;
}

void BlurNoise::reset() noexcept
{
    vocoder_.reset();
    noise_ = startNoise_;
}

bool BlurNoise::process(std::span<const double> input, std::span<double> output) noexcept
{
    const auto noise = noise_;
    return vocoder_.process(input, output, [noise](std::span<float> frame) {
        // specnoise.
        const auto clength = static_cast<int>(frame.size() / 2);
        auto totalamp = get_totalamp(frame);
        totalamp /= clength;
        // CDP8's build fuses the multiply and add.
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            frame[vc] = (float)std::fma(totalamp - frame[vc], noise, (double)frame[vc]);
    });
}
}
