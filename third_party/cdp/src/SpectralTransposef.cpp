/*
 * Derived from the CDP System, Release 8: dev/repitch/repitch.c and
 * ap_repitch.c, dev/cdp2k/formantsg.c, tklib1.c, tklib3.c and readdata.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralTransposef.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace talas::cdp
{
namespace
{
using Units = RepitchTransposeFormants::Units;

// dev/include/globcon.h.
constexpr double VERY_TINY_VAL = 0.00000000000000000001;
constexpr double OCTAVES_PER_SEMITONE = 0.08333333333;
constexpr double FLTERR = 0.000002;
constexpr double MINPITCH = 9.0;
// dev/include/speccon.h.
constexpr double SPEC_MINFRQ = 10.0;
constexpr double MIN_TRANSPOS = RepitchTransposeFormants::minimumRatio;
constexpr double MAX_TRANSPOS = RepitchTransposeFormants::maximumRatio;

[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    return !(f1 > f2 + FLTERR || f1 < f2 - FLTERR);
}

// The transposition as CDP's transpos array holds it, a float ratio for each
// frame: read_and_test_pitch_or_transposition_brkvals for a breakpoint file,
// get_transposition_value for a constant (both as RepitchTranspose converts
// them), then check_for_valid_transposition_ratios.
[[nodiscard]] std::optional<float> transpositionRatio(
    Units units, bool live, double nyquist, double transposition) noexcept
{
    if (!std::isfinite(transposition))
        return std::nullopt;
    double val = transposition;
    if (live)
    {
        switch (units)
        {
        case Units::Semitones:
            val = val / 12.0; /* semitone to octave */
            [[fallthrough]];
        case Units::Octaves:
            val = std::pow(2.0, val); /* octave to frqratio */
            break;
        case Units::Ratio:
            break;
        }
        if (flteq(val, MIN_TRANSPOS))
            val = MIN_TRANSPOS;
        else if (flteq(val, MAX_TRANSPOS))
            val = MAX_TRANSPOS;
    }
    else
    {
        switch (units)
        {
        case Units::Semitones:
            val *= OCTAVES_PER_SEMITONE; /* semitones -> octaves */
            [[fallthrough]];
        case Units::Octaves:
            val = (float)std::pow(2.0, val); /* octaves -> frqratios */
            break;
        case Units::Ratio:
            break;
        }
    }
    if (!std::isfinite(val) || val < MIN_TRANSPOS || val > MAX_TRANSPOS)
        return std::nullopt;
    const auto transpos = (float)val;
    // check_for_valid_transposition_ratios(MINPITCH/nyquist, nyquist/MINPITCH).
    if (transpos < MINPITCH / nyquist || transpos > nyquist / MINPITCH)
        return std::nullopt;
    return transpos;
}

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

// normalise.
void normalise(std::span<float> frame, double pre_totalamp, double post_totalamp) noexcept
{
    if (post_totalamp < VERY_TINY_VAL)
        return;
    const double normaliser = pre_totalamp / post_totalamp;
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        frame[vc] = (float)(frame[vc] * normaliser);
}
}

// CDP's -l and -h, whose defaults are PITCHZERO (zero, as setup_pitchzero
// makes a fixed value under 10 Hz) and the Nyquist frequency.
double RepitchTransposeFormants::lowLimit(double frequency) const noexcept
{
    if (!lowFlag_)
        return 0.0;
    if (frequency < SPEC_MINFRQ)
        return liveLow_ ? SPEC_MINFRQ : 0.0;
    return frequency;
}

double RepitchTransposeFormants::highLimit(double frequency) const noexcept
{
    if (!highFlag_)
        return nyquist_;
    if (frequency < SPEC_MINFRQ)
        return liveHigh_ ? SPEC_MINFRQ : 0.0;
    return frequency;
}

bool RepitchTransposeFormants::prepare(double sampleRate, int points, int overlap, Units units, bool pitchwise,
    int bands, bool quickSearch, double transposition, bool liveTransposition, double lowFrequency,
    double highFrequency, bool liveLow, bool liveHigh, bool fuller)
{
    if (units < Units::Ratio || units > Units::Semitones)
        return false;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    SpectralEnvelope envelope;
    if (!envelope.prepare(sampleRate, vocoder.frameSize(), pitchwise, bands, quickSearch))
        return false;
    const double nyquist = envelope.nyquist();
    const auto start = transpositionRatio(units, liveTransposition, nyquist, transposition);
    if (!start.has_value() || !std::isfinite(lowFrequency) || !std::isfinite(highFrequency) || lowFrequency < 0.0
        || highFrequency < 0.0 || lowFrequency > nyquist || highFrequency > nyquist)
    {
        return false;
    }
    vocoder_ = std::move(vocoder);
    envelope_ = std::move(envelope);
    units_ = units;
    live_ = liveTransposition;
    nyquist_ = nyquist;
    chwidth_ = envelope_.channelWidth();
    halfchwidth_ = envelope_.halfChannelWidth();
    fuller_ = fuller;
    liveLow_ = liveLow;
    liveHigh_ = liveHigh;
    lowFlag_ = liveLow || lowFrequency > 0.0;
    highFlag_ = liveHigh || highFrequency > 0.0;
    band_ = lowFlag_ || highFlag_;
    startLowFrequency_ = lowFrequency;
    startHighFrequency_ = highFrequency;
    windowbuf_.assign(vocoder_.frameSize(), 0.0F);
    startTranspos_ = *start;
    reset();
    return true;
}

bool RepitchTransposeFormants::setControls(double transposition, double lowFrequency, double highFrequency) noexcept
{
    if (!prepared() || !std::isfinite(lowFrequency) || !std::isfinite(highFrequency) || lowFrequency < 0.0
        || highFrequency < 0.0 || lowFrequency > nyquist_ || highFrequency > nyquist_)
    {
        return false;
    }
    const auto value = transpositionRatio(units_, live_, nyquist_, transposition);
    if (!value.has_value())
        return false;
    transpos_ = *value;
    lowFrequency_ = lowLimit(lowFrequency);
    highFrequency_ = highLimit(highFrequency);
    return true;
}

void RepitchTransposeFormants::reset() noexcept
{
    vocoder_.reset();
    std::ranges::fill(windowbuf_, 0.0F);
    transpos_ = startTranspos_;
    lowFrequency_ = lowLimit(startLowFrequency_);
    highFrequency_ = highLimit(startHighFrequency_);
    frames_ = 0;
}

bool RepitchTransposeFormants::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        if (frames_++ == 0)
        {
            // skip_or_special_operation_on_window_zero.
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
                windowbuf_[vc + 1] = frame[vc + 1];
            return;
        }
        transposeFrame(frame);
    });
}

// spectrnsf.
void RepitchTransposeFormants::transposeFrame(std::span<float> frame) noexcept
{
    float* flbuf = frame.data();
    const int clength = static_cast<int>(frame.size() / 2);
    const float transpos = transpos_;
    int cc, vc;
    double pre_totalamp, post_totalamp = 0.0;
    rectify_window(frame);
    envelope_.extract(frame);
    pre_totalamp = get_totalamp(frame);
    // tranpose_within_formant_envelope.
    for (cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        double thisspecamp, newspecamp, thisamp, formantamp_ratio;
        thisspecamp = envelope_.amplitudeAt((double)flbuf[vc + 1]);
        flbuf[vc + 1] = (float)(std::fabs((double)flbuf[vc + 1]) * transpos);
        if (flbuf[vc + 1] < nyquist_)
        {
            if (thisspecamp < VERY_TINY_VAL)
                flbuf[vc] = 0.0f;
            else
            {
                newspecamp = envelope_.amplitudeAt((double)flbuf[vc + 1]);
                if (newspecamp < VERY_TINY_VAL)
                    flbuf[vc] = 0.0f;
                else
                {
                    formantamp_ratio = newspecamp / thisspecamp;
                    if ((thisamp = flbuf[vc] * formantamp_ratio) < VERY_TINY_VAL)
                        flbuf[vc] = 0.0f;
                    else
                        flbuf[vc] = (float)thisamp;
                }
            }
        }
    }
    reposition(frame);
    if (band_)
    {
        double lofrq_limit = lowFrequency_;
        double hifrq_limit = highFrequency_;
        if (hifrq_limit < lofrq_limit)
            std::swap(hifrq_limit, lofrq_limit);
        // zero_outofrange_channels.
        for (cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        {
            if (flbuf[vc + 1] < lofrq_limit || flbuf[vc + 1] > hifrq_limit)
                flbuf[vc] = 0.0f;
            else
                post_totalamp += flbuf[vc];
        }
    }
    else
        post_totalamp = get_totalamp(frame);
    normalise(frame, pre_totalamp, post_totalamp);
}

// reposition_partials_in_appropriate_channels, with
// get_channel_corresponding_to_frq, move_data_into_appropriate_channel and
// (for -x going up) move_data_into_some_appropriate_channel, whose last
// resort compares the amplitude with the source frame's.
void RepitchTransposeFormants::reposition(std::span<float> frame) noexcept
{
    float* flbuf = frame.data();
    float* windowbuf = windowbuf_.data();
    const int clength = static_cast<int>(frame.size() / 2);
    const int wanted = static_cast<int>(frame.size());
    const float transpos = transpos_;
    int cc, vc;
    const auto channel = [&](double thisfrq) { return (int)((std::fabs(thisfrq) + halfchwidth_) / chwidth_); };
    const auto move = [&](int from, int truevc, float thisamp, float thisfrq) {
        if (flbuf[from] > windowbuf[truevc])
        {
            windowbuf[truevc++] = thisamp;
            windowbuf[truevc] = thisfrq;
        }
    };
    const auto moveSome = [&](int truevc, float thisamp, float thisfrq) {
        constexpr int THISCHANSCAN = 4;
        int minscan, maxscan, n;
        if (windowbuf[truevc] == 0.0f)
        {
            windowbuf[truevc++] = thisamp;
            windowbuf[truevc] = thisfrq;
            return;
        }
        minscan = std::max(0, truevc - (THISCHANSCAN * 2));
        maxscan = std::min(clength - 1, truevc + (THISCHANSCAN * 2));
        for (n = truevc - 2; n > minscan; n -= 2)
        {
            if (windowbuf[n] == 0.0f)
            {
                windowbuf[n++] = thisamp;
                windowbuf[n] = thisfrq;
                return;
            }
        }
        for (n = truevc + 2; n < maxscan; n += 2)
        {
            if (windowbuf[n] == 0.0f)
            {
                windowbuf[n++] = thisamp;
                windowbuf[n] = thisfrq;
                return;
            }
        }
        if (thisamp > flbuf[truevc])
        {
            windowbuf[truevc++] = thisamp;
            windowbuf[truevc] = thisfrq;
            return;
        }
        for (n = truevc - 2; n > minscan; n -= 2)
        {
            if (thisamp > windowbuf[n])
            {
                windowbuf[n++] = thisamp;
                windowbuf[n] = thisfrq;
                return;
            }
        }
        for (n = truevc + 2; n < maxscan; n += 2)
        {
            if (thisamp > windowbuf[n])
            {
                windowbuf[n++] = thisamp;
                windowbuf[n] = thisfrq;
                return;
            }
        }
    };
    for (vc = 0; vc < wanted; vc += 2)
        windowbuf[vc] = 0.0f;
    if (transpos > 1.0f)
    {
        // Down the spectrum, upward transposition.
        for (cc = clength - 1, vc = wanted - 2; cc >= 0; cc--, vc -= 2)
        {
            if (flbuf[vc + 1] < nyquist_ && flbuf[vc] > 0.0f)
            {
                const int truevc = channel((double)flbuf[vc + 1]) * 2;
                if (fuller_)
                    moveSome(truevc, flbuf[vc], flbuf[vc + 1]);
                else
                    move(vc, truevc, flbuf[vc], flbuf[vc + 1]);
            }
        }
        std::copy(windowbuf, windowbuf + wanted, flbuf);
    }
    else if (transpos < 1.0f)
    {
        // Up the spectrum, downward transposition.
        for (cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        {
            if (flbuf[vc + 1] < nyquist_ && flbuf[vc + 1] > 0.0)
            {
                const int truevc = channel((double)flbuf[vc + 1]) * 2;
                move(vc, truevc, flbuf[vc], flbuf[vc + 1]);
            }
        }
        std::copy(windowbuf, windowbuf + wanted, flbuf);
    }
}
}
