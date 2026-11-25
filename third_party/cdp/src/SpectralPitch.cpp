/*
 * Derived from the CDP System, Release 8: dev/strange/strange.c and
 * ap_strange.c, dev/pitch/pitch.c and ap_pitch.c, dev/stretch/stretch.c and
 * ap_stretch.c, dev/repitch/repitch.c and ap_repitch.c, dev/cdp2k/tklib1.c,
 * tklib3.c and readdata.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralPitch.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h.
constexpr double VERY_TINY_VAL = 0.00000000000000000001;
constexpr double OCTAVES_PER_SEMITONE = 0.08333333333;
constexpr double FLTERR = 0.000002;
// dev/include/speccon.h.
constexpr double SPEC_MINFRQ = 10.0;

[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    return !(f1 > f2 + FLTERR || f1 < f2 - FLTERR);
}

void normalise(std::span<float> frame, double pre_totalamp, double post_totalamp) noexcept
{
    if (post_totalamp < VERY_TINY_VAL)
        return;
    const auto normaliser = pre_totalamp / post_totalamp;
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        frame[vc] = (float)(frame[vc] * normaliser);
}
}

bool StrangeShift::valid(double shift, double frequency, double highFrequency) const noexcept
{
    const auto low = halfchwidth_ / 2.0;
    const auto high = nyquist_ - (halfchwidth_ / 2.0);
    const auto inRange = [&](double value) { return std::isfinite(value) && value >= low && value <= high; };
    return std::isfinite(shift) && shift >= -nyquist_ && shift <= nyquist_
        && (mode_ == Mode::All || inRange(frequency))
        && ((mode_ != Mode::Between && mode_ != Mode::Outside) || inRange(highFrequency));
}

bool StrangeShift::prepare(double sampleRate, int points, int overlap, Mode mode, double shift,
    double frequency, double highFrequency)
{
    if (mode < Mode::All || mode > Mode::Outside)
        return false;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    const auto clength = static_cast<int>(vocoder.frameSize() / 2);
    const auto previous = std::tuple{mode_, nyquist_, chwidth_, halfchwidth_};
    mode_ = mode;
    nyquist_ = sampleRate / 2.0;
    chwidth_ = nyquist_ / (double)(clength - 1);
    halfchwidth_ = chwidth_ / 2.0;
    if (!valid(shift, frequency, highFrequency))
    {
        std::tie(mode_, nyquist_, chwidth_, halfchwidth_) = previous;
        return false;
    }
    vocoder_ = std::move(vocoder);
    // setup_internal_arrays_for_specshift.
    chtop_.assign(static_cast<std::size_t>(clength), 0.0);
    chmid_.assign(static_cast<std::size_t>(clength), 0.0);
    over_.assign(static_cast<std::size_t>(clength), 0);
    done_.assign(static_cast<std::size_t>(clength), 0);
    chtop_[0] = halfchwidth_;
    chmid_[0] = 0.0;
    for (int n = 1; n < clength; n++)
    {
        chtop_[static_cast<std::size_t>(n)] = chtop_[static_cast<std::size_t>(n) - 1] + chwidth_;
        chmid_[static_cast<std::size_t>(n)] = chmid_[static_cast<std::size_t>(n) - 1] + chwidth_;
    }
    chmid_[0] = 1.0;
    startShift_ = shift;
    startFrequency_ = frequency;
    startHighFrequency_ = highFrequency;
    reset();
    return true;
}

bool StrangeShift::setControls(double shift, double frequency, double highFrequency) noexcept
{
    if (!prepared() || !valid(shift, frequency, highFrequency))
        return false;
    shift_ = shift;
    frequency_ = frequency;
    highFrequency_ = highFrequency;
    return true;
}

void StrangeShift::reset() noexcept
{
    vocoder_.reset();
    shift_ = startShift_;
    frequency_ = startFrequency_;
    highFrequency_ = startHighFrequency_;
}

bool StrangeShift::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) { shiftFrame(frame); });
}

// do_the_specshift and reassign_frqs_to_appropriate_chans.
void StrangeShift::shiftFrame(std::span<float> frame) noexcept
{
    const auto clength = static_cast<int>(chtop_.size());
    const auto frq = [&](int vc) -> float& { return frame[static_cast<std::size_t>(vc) + 1]; };
    for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        auto& f = frq(vc);
        const auto shifted = mode_ == Mode::All ? true
            : mode_ == Mode::Above              ? f > frequency_
            : mode_ == Mode::Below              ? f < frequency_
            : mode_ == Mode::Between            ? f > frequency_ && f < highFrequency_
                                                : f < frequency_ || f > highFrequency_;
        if (shifted)
            f = (float)(f + shift_);
    }
    std::ranges::fill(over_, 0);
    std::ranges::fill(done_, 0);
    if (shift_ > 0.0)
    {
        for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        {
            if (frq(vc) > (float)chtop_[static_cast<std::size_t>(cc)])
                over_[static_cast<std::size_t>(cc)] = 1;
        }
        // Work down from the top, shifting up.
        for (int cc = clength - 1, vc = (clength - 1) * 2; cc >= 0; cc--, vc -= 2)
        {
            if (over_[static_cast<std::size_t>(cc)])
                chanShift(frame, cc, vc);
        }
    }
    // Work up from the bottom, shifting down.
    if (shift_ < 0.0)
    {
        for (int cc = 1, vc = 2; cc < clength; cc++, vc += 2)
        {
            if (frq(vc) <= (float)chtop_[static_cast<std::size_t>(cc) - 1])
                chanShift(frame, cc, vc);
        }
    }
}

// do_chan_shifting.
void StrangeShift::chanShift(std::span<float> frame, int cc, int vc) noexcept
{
    const auto clength = static_cast<int>(chtop_.size());
    const auto at = [&](int index) -> float& { return frame[static_cast<std::size_t>(index)]; };
    const int k = (int)((at(vc + 1) + halfchwidth_) / chwidth_);
    int newvc = k * 2;
    if (k >= 0 && k < clength)
    {
        auto& done = done_[static_cast<std::size_t>(k)];
        if (!done || (done && (at(vc) > at(newvc))))
        {
            at(newvc++) = at(vc);
            at(newvc) = at(vc + 1);
            done = 1;
        }
    }
    at(vc) = 0.0f;
    at(vc + 1) = (float)chmid_[static_cast<std::size_t>(cc)];
}

std::optional<float> RepitchTranspose::ratio(double transposition) const noexcept
{
    if (!std::isfinite(transposition))
        return std::nullopt;
    double val = transposition;
    if (live_)
    {
        // read_and_test_pitch_or_transposition_brkvals.
        switch (units_)
        {
        case Units::Semitones:
            val = val / 12.0;
            [[fallthrough]];
        case Units::Octaves:
            val = std::pow(2.0, val);
            break;
        case Units::Ratio:
            break;
        }
        if (flteq(val, minimumRatio))
            val = minimumRatio;
        else if (flteq(val, maximumRatio))
            val = maximumRatio;
    }
    else
    {
        // get_transposition_value.
        switch (units_)
        {
        case Units::Semitones:
            val *= OCTAVES_PER_SEMITONE;
            [[fallthrough]];
        case Units::Octaves:
            val = (float)std::pow(2.0, val);
            break;
        case Units::Ratio:
            break;
        }
    }
    if (!std::isfinite(val) || val < minimumRatio || val > maximumRatio)
        return std::nullopt;
    return (float)val;
}

// CDP's -l and -h, whose defaults are PITCHZERO (zero, as setup_pitchzero
// makes a fixed value under 10 Hz) and the Nyquist frequency.
double RepitchTranspose::lowLimit(double frequency) const noexcept
{
    if (!lowFlag_)
        return 0.0;
    if (frequency < SPEC_MINFRQ)
        return liveLow_ ? SPEC_MINFRQ : 0.0;
    return frequency;
}

double RepitchTranspose::highLimit(double frequency) const noexcept
{
    if (!highFlag_)
        return nyquist_;
    if (frequency < SPEC_MINFRQ)
        return liveHigh_ ? SPEC_MINFRQ : 0.0;
    return frequency;
}

bool RepitchTranspose::prepare(double sampleRate, int points, int overlap, Units units, double transposition,
    bool liveTransposition, double lowFrequency, double highFrequency, bool liveLow, bool liveHigh, bool fuller)
{
    const auto nyquist = sampleRate / 2.0;
    if (units < Units::Ratio || units > Units::Semitones || !std::isfinite(lowFrequency)
        || !std::isfinite(highFrequency) || lowFrequency < 0.0 || highFrequency < 0.0 || lowFrequency > nyquist
        || highFrequency > nyquist)
    {
        return false;
    }
    const auto previous = std::pair{units_, live_};
    units_ = units;
    live_ = liveTransposition;
    const auto start = ratio(transposition);
    PhaseVocoder vocoder;
    if (!start.has_value() || !vocoder.prepare(sampleRate, points, overlap))
    {
        std::tie(units_, live_) = previous;
        return false;
    }
    vocoder_ = std::move(vocoder);
    const auto clength = static_cast<int>(vocoder_.frameSize() / 2);
    nyquist_ = nyquist;
    chwidth_ = nyquist_ / (double)(clength - 1);
    halfchwidth_ = chwidth_ / 2.0;
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

bool RepitchTranspose::setControls(double transposition, double lowFrequency, double highFrequency) noexcept
{
    if (!prepared() || !std::isfinite(lowFrequency) || !std::isfinite(highFrequency) || lowFrequency < 0.0
        || highFrequency < 0.0 || lowFrequency > nyquist_ || highFrequency > nyquist_)
    {
        return false;
    }
    const auto value = ratio(transposition);
    if (!value.has_value())
        return false;
    transpos_ = *value;
    lowFrequency_ = lowLimit(lowFrequency);
    highFrequency_ = highLimit(highFrequency);
    return true;
}

void RepitchTranspose::reset() noexcept
{
    vocoder_.reset();
    std::ranges::fill(windowbuf_, 0.0F);
    transpos_ = startTranspos_;
    lowFrequency_ = lowLimit(startLowFrequency_);
    highFrequency_ = highLimit(startHighFrequency_);
    frames_ = 0;
}

bool RepitchTranspose::process(std::span<const double> input, std::span<double> output) noexcept
{
    const auto transpos = transpos_;
    return vocoder_.process(input, output, [this, transpos](std::span<float> frame) {
        if (frames_++ == 0)
        {
            // skip_or_special_operation_on_window_zero.
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
                windowbuf_[vc + 1] = frame[vc + 1];
            return;
        }
        transpose(frame, transpos);
    });
}

// spectrnsp.
void RepitchTranspose::transpose(std::span<float> frame, float transpos) noexcept
{
    const auto clength = static_cast<int>(frame.size() / 2);
    const auto wanted = static_cast<int>(frame.size());
    float* flbuf = frame.data();
    float* windowbuf = windowbuf_.data();
    // rectify_window.
    for (int vc = 0; vc < wanted; vc += 2)
    {
        if (flbuf[vc + 1] < 0.0)
            flbuf[vc + 1] = -flbuf[vc + 1];
    }
    double pre_totalamp = 0.0, post_totalamp = 0.0;
    for (int vc = 0; vc < wanted; vc += 2)
        pre_totalamp += flbuf[vc];
    for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        flbuf[vc + 1] = (float)(flbuf[vc + 1] * transpos);
    // reposition_partials_in_appropriate_channels.
    const auto channel = [&](double thisfrq) { return (int)((std::fabs(thisfrq) + halfchwidth_) / chwidth_); };
    // move_data_into_appropriate_channel.
    const auto move = [&](int vc, int truevc, float thisamp, float thisfrq) {
        if (flbuf[vc] > windowbuf[truevc])
        {
            windowbuf[truevc++] = thisamp;
            windowbuf[truevc] = thisfrq;
        }
    };
    // move_data_into_some_appropriate_channel, whose last resort compares
    // the amplitude with the source frame's.
    const auto moveSome = [&](int truevc, float thisamp, float thisfrq) {
        constexpr int THISCHANSCAN = 4;
        if (windowbuf[truevc] == 0.0f)
        {
            windowbuf[truevc++] = thisamp;
            windowbuf[truevc] = thisfrq;
            return;
        }
        const int minscan = std::max(0, truevc - (THISCHANSCAN * 2));
        const int maxscan = std::min(clength - 1, truevc + (THISCHANSCAN * 2));
        for (int n = truevc - 2; n > minscan; n -= 2)
        {
            if (windowbuf[n] == 0.0f)
            {
                windowbuf[n++] = thisamp;
                windowbuf[n] = thisfrq;
                return;
            }
        }
        for (int n = truevc + 2; n < maxscan; n += 2)
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
        for (int n = truevc - 2; n > minscan; n -= 2)
        {
            if (thisamp > windowbuf[n])
            {
                windowbuf[n++] = thisamp;
                windowbuf[n] = thisfrq;
                return;
            }
        }
        for (int n = truevc + 2; n < maxscan; n += 2)
        {
            if (thisamp > windowbuf[n])
            {
                windowbuf[n++] = thisamp;
                windowbuf[n] = thisfrq;
                return;
            }
        }
    };
    for (int vc = 0; vc < wanted; vc += 2)
        windowbuf[vc] = 0.0f;
    if (transpos > 1.0f)
    {
        for (int cc = clength - 1, vc = wanted - 2; cc >= 0; cc--, vc -= 2)
        {
            if (flbuf[vc + 1] < nyquist_ && flbuf[vc] > 0.0f)
            {
                const int truevc = channel(flbuf[vc + 1]) * 2;
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
        for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        {
            if (flbuf[vc + 1] < nyquist_ && flbuf[vc + 1] > 0.0)
            {
                const int truevc = channel(flbuf[vc + 1]) * 2;
                move(vc, truevc, flbuf[vc], flbuf[vc + 1]);
            }
        }
        std::copy(windowbuf, windowbuf + wanted, flbuf);
    }
    if (band_)
    {
        double lofrq_limit = lowFrequency_;
        double hifrq_limit = highFrequency_;
        if (hifrq_limit < lofrq_limit)
            std::swap(hifrq_limit, lofrq_limit);
        // zero_outofrange_channels.
        for (int vc = 0; vc < wanted; vc += 2)
        {
            if (flbuf[vc + 1] < lofrq_limit || flbuf[vc + 1] > hifrq_limit)
                flbuf[vc] = 0.0f;
            else
                post_totalamp += flbuf[vc];
        }
    }
    else
    {
        for (int vc = 0; vc < wanted; vc += 2)
            post_totalamp += flbuf[vc];
    }
    normalise(frame, pre_totalamp, post_totalamp);
}

bool StrangeWaver::valid(double vibrato, double stretch) const noexcept
{
    return std::isfinite(vibrato) && std::isfinite(stretch) && vibrato >= minimumVibrato
        && vibrato <= 0.5 / frametime_ && stretch >= 1.0 && stretch <= nyquist_ / SPEC_MINFRQ;
}

bool StrangeWaver::prepare(double sampleRate, int points, int overlap, Mode mode, double vibrato,
    double stretch, double bottomFrequency, double exponent)
{
    PhaseVocoder vocoder;
    if (mode < Mode::Standard || mode > Mode::Specified || !vocoder.prepare(sampleRate, points, overlap))
        return false;
    const auto nyquist = sampleRate / 2.0;
    const auto arate = (float)((float)sampleRate / (float)vocoder.decimation());
    const auto previous = std::pair{frametime_, nyquist_};
    frametime_ = (float)(1.0 / arate);
    nyquist_ = nyquist;
    if (!valid(vibrato, stretch) || !std::isfinite(bottomFrequency) || bottomFrequency < 5.0
        || bottomFrequency > nyquist || (mode == Mode::Specified
            && (!std::isfinite(exponent) || exponent < minimumExponent || exponent > maximumExponent)))
    {
        std::tie(frametime_, nyquist_) = previous;
        return false;
    }
    vocoder_ = std::move(vocoder);
    mode_ = mode;
    exponent_ = exponent;
    const auto clength = static_cast<int>(vocoder_.frameSize() / 2);
    const auto chwidth = nyquist_ / (double)(clength - 1);
    // setup_pitchzero makes a fixed bottom frequency under 10 Hz zero.
    const auto lofrq = bottomFrequency < SPEC_MINFRQ ? 0.0 : bottomFrequency;
    // setup_array_of_botfrq_of_each_channel.
    chfbot_.assign(static_cast<std::size_t>(clength) + 1, 0.0);
    int n = 1;
    int botchan = 0;
    double boundary = chwidth / 2.0;
    while (boundary < nyquist_ && n < clength)
    {
        chfbot_[static_cast<std::size_t>(n)] = boundary;
        if (botchan == 0 && (lofrq < boundary))
            botchan = n;
        boundary += chwidth;
        n++;
    }
    botchan--;
    chfbot_[static_cast<std::size_t>(clength)] = nyquist_;
    botchan_ = botchan;
    strchans_ = clength - botchan;
    amp_.assign(static_cast<std::size_t>(clength), 0.0F);
    freq_.assign(static_cast<std::size_t>(clength), 0.0F);
    startVibrato_ = vibrato;
    startStretch_ = stretch;
    reset();
    return true;
}

bool StrangeWaver::setControls(double vibrato, double stretch) noexcept
{
    if (!prepared() || !valid(vibrato, stretch))
        return false;
    vibrato_ = vibrato;
    stretch_ = stretch;
    return true;
}

void StrangeWaver::reset() noexcept
{
    vocoder_.reset();
    std::ranges::fill(amp_, 0.0F);
    std::ranges::fill(freq_, 0.0F);
    vibrato_ = startVibrato_;
    stretch_ = startStretch_;
    upwcnt_ = 0;
    dnwcnt_ = 0;
    done_ = 0;
}

bool StrangeWaver::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        // specwaver: a new cycle reads the fields.
        if (done_ == upwcnt_ + dnwcnt_)
        {
            const auto wcnt = (int)std::round(1.0 / (vibrato_ * frametime_));
            dnwcnt_ = upwcnt_ = wcnt / 2;
            oddCycle_ = (wcnt % 2) != 0;
            if (oddCycle_)
                upwcnt_++;
            strstep_ = stretch_ / (double)upwcnt_;
            thisstr_ = stretch_;
            done_ = 0;
        }
        // do_waver.
        stretchWindow(frame, thisstr_);
        if (++done_ <= upwcnt_)
        {
            thisstr_ += strstep_;
            if (done_ == upwcnt_ && oddCycle_)
                thisstr_ -= strstep_;
        }
        else
            thisstr_ -= strstep_;
    });
}

// spectrally_stretch_window and assign_new_frqs_to_appropriate_channels.
void StrangeWaver::stretchWindow(std::span<float> frame, double thisstr) noexcept
{
    const auto clength = static_cast<int>(amp_.size());
    const auto wanted = static_cast<int>(frame.size());
    float* flbuf = frame.data();
    int dd, cc, vc;
    double herestr, thislocation, strdiff = thisstr - 1.0;
    for (dd = 0, cc = botchan_, vc = botchan_ * 2; cc < clength; dd++, cc++, vc += 2)
    {
        thislocation = (double)dd / (double)strchans_;
        if (mode_ == Mode::Specified)
            thislocation = std::pow(thislocation, exponent_);
        herestr = (thislocation * strdiff) + 1.0;
        amp_[static_cast<std::size_t>(cc)] = flbuf[vc];
        freq_[static_cast<std::size_t>(cc)] = (float)std::fabs(flbuf[vc + 1] * herestr);
    }
    const auto chfbot = [&](int index) { return chfbot_[static_cast<std::size_t>(index)]; };
    dd = botchan_;
    int ok = 1;
    cc = 0;
    while (freq_[static_cast<std::size_t>(dd)] >= (float)(chfbot(cc + 1)))
    {
        if (++cc >= clength)
        {
            for (vc = botchan_ * 2; vc < wanted; vc += 2)
                flbuf[vc] = 0.0F;
            return;
        }
    }
    for (vc = cc * 2; cc < clength; cc++, vc += 2)
    {
        flbuf[vc] = 0.0F;
        while (freq_[static_cast<std::size_t>(dd)] < (float)(chfbot(cc + 1)))
        {
            if ((freq_[static_cast<std::size_t>(dd)] >= chfbot(cc))
                && (amp_[static_cast<std::size_t>(dd)] > flbuf[vc]))
            {
                flbuf[vc] = amp_[static_cast<std::size_t>(dd)];
                flbuf[vc + 1] = freq_[static_cast<std::size_t>(dd)];
            }
            if (++dd >= clength)
            {
                ok = 0;
                break;
            }
        }
        if (!ok)
            break;
    }
    if (!ok)
    {
        while (vc < wanted)
        {
            flbuf[vc] = 0.0F;
            vc += 2;
        }
    }
}

std::optional<PitchTransp::Shifts> PitchTransp::shifts(double split, double transposition, double transposition2) const
    noexcept
{
    if (!std::isfinite(split) || !std::isfinite(transposition) || !std::isfinite(transposition2) || split < 5.0
        || split > nyquist_)
    {
        return std::nullopt;
    }
    const auto semitoneLimit = std::log2(nyquist_ / SPEC_MINFRQ) * 12.0;
    if (shiftMode()
        && (transposition < -semitoneLimit || transposition > semitoneLimit
            || (mode_ == Mode::ShiftUpAndDown && (transposition2 < -semitoneLimit || transposition2 > semitoneLimit))))
    {
        return std::nullopt;
    }
    // setup_pitchzero.
    const auto ffrq = split < SPEC_MINFRQ ? (liveSplit_ ? SPEC_MINFRQ : 0.0) : split;
    const auto clength = static_cast<int>(amp_.size());
    Shifts result;
    result.shf1 = shiftMode() ? transposition : 0.0;
    result.shf2 = mode_ == Mode::ShiftUpAndDown ? transposition2 : 0.0;
    // sub_check_shiftp and convert_shiftp_vals: semitones to ratios, and
    // shifts too great for the split.
    const double frq0 = ffrq + chwidth_ / 2.0;
    const int fdcno0 = (int)std::floor(frq0 / chwidth_);
    if (shiftMode())
    {
        result.shf1 *= OCTAVES_PER_SEMITONE;
        result.shf1 = std::pow(2.0, result.shf1);
        if (mode_ == Mode::ShiftUpAndDown)
        {
            result.shf2 *= OCTAVES_PER_SEMITONE;
            result.shf2 = std::pow(2.0, result.shf2);
        }
        const auto top = (double)(clength - 1);
        if (result.shf1 > 1.0 && std::round(top / result.shf1) < fdcno0)
            return std::nullopt;
        if (result.shf1 <= 1.0
            && (mode_ == Mode::ShiftDown ? std::round(top * result.shf1) < fdcno0
                                         : std::round(top * result.shf1) <= fdcno0))
        {
            return std::nullopt;
        }
        if (mode_ == Mode::ShiftUpAndDown
            && (result.shf2 > ffrq / SPEC_MINFRQ
                || (result.shf2 <= 1.0 && (1.0 / result.shf2) >= ffrq / SPEC_MINFRQ)))
        {
            return std::nullopt;
        }
    }
    // adjust_params_and_setup_internal_params_for_shiftp.
    const double frq = ffrq + chwidth_ / 2.0;
    result.fdcno = (int)std::floor(frq / chwidth_);
    result.s1l1 = result.shf1 - 1.0;
    result.ls1 = 1.0 - result.shf1;
    result.ns1 = result.shf1;
    result.ns2 = mode_ == Mode::ShiftUpAndDown ? result.shf2 : 0.0;
    return result;
}

bool PitchTransp::prepare(double sampleRate, int points, int overlap, Mode mode, double split,
    double transposition, double transposition2, bool hasDepth, double depth, bool liveSplit,
    bool liveTransposition, bool liveTransposition2)
{
    // In the shift modes CDP8 converts its transpositions again at every
    // frame once any of them or the split follows a file: only fresh,
    // live, values convert once.
    const auto liveTranspositions = liveTransposition && (mode != Mode::ShiftUpAndDown || liveTransposition2);
    if (mode >= Mode::ShiftUp && (liveSplit || liveTransposition || (mode == Mode::ShiftUpAndDown && liveTransposition2))
        && !liveTranspositions)
    {
        return false;
    }
    PhaseVocoder vocoder;
    if (mode < Mode::OctaveUp || mode > Mode::ShiftUpAndDown || !vocoder.prepare(sampleRate, points, overlap)
        || (hasDepth && (!std::isfinite(depth) || depth < 0.0 || depth > 1.0)))
    {
        return false;
    }
    const auto clength = static_cast<int>(vocoder.frameSize() / 2);
    const auto previousMode = mode_;
    const auto previousNyquist = nyquist_;
    const auto previousChwidth = chwidth_;
    const auto previousLive = liveSplit_;
    const auto previousSize = amp_.size();
    mode_ = mode;
    nyquist_ = sampleRate / 2.0;
    chwidth_ = nyquist_ / (double)(clength - 1);
    liveSplit_ = liveSplit;
    amp_.resize(static_cast<std::size_t>(clength));
    const auto start = shifts(split, transposition, transposition2);
    if (!start.has_value())
    {
        mode_ = previousMode;
        nyquist_ = previousNyquist;
        chwidth_ = previousChwidth;
        liveSplit_ = previousLive;
        amp_.resize(previousSize);
        return false;
    }
    vocoder_ = std::move(vocoder);
    freq_.assign(static_cast<std::size_t>(clength), 0.0F);
    hasDepth_ = hasDepth;
    liveTranspositions_ = liveTranspositions;
    startShifts_ = *start;
    startDepth_ = hasDepth ? depth : 1.0;
    reset();
    return true;
}

bool PitchTransp::setControls(double split, double transposition, double transposition2, double depth) noexcept
{
    if (!prepared() || (hasDepth_ && (!std::isfinite(depth) || depth < 0.0 || depth > 1.0)))
        return false;
    const auto current = shifts(split, transposition, transposition2);
    if (!current.has_value())
        return false;
    shifts_ = *current;
    depth_ = hasDepth_ ? depth : 1.0;
    return true;
}

void PitchTransp::reset() noexcept
{
    vocoder_.reset();
    shifts_ = startShifts_;
    depth_ = startDepth_;
}

bool PitchTransp::process(std::span<const double> input, std::span<double> output) noexcept
{
    const auto current = shifts_;
    const auto depth = depth_;
    return vocoder_.process(input, output, [this, current, depth](std::span<float> frame) {
        shiftFrame(frame, current, depth);
    });
}

// specshiftp and do_spectral_shiftp.
void PitchTransp::shiftFrame(std::span<float> frame, Shifts current, double depth) noexcept
{
    if (hasDepth_ && flteq(depth, 0.0))
        return;
    const auto clength = static_cast<int>(amp_.size());
    float* amp = amp_.data();
    float* freq = freq_.data();
    // get_amp_and_frq.
    for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        amp[cc] = frame[static_cast<std::size_t>(vc)];
        freq[cc] = frame[static_cast<std::size_t>(vc) + 1];
    }
    // reset_shiftps_according_to_depth_value, whose case for mode 6 reads
    // CDP's mode number where the mode index is meant, so never runs.
    if (hasDepth_)
    {
        if (current.shf1 > 1.0)
            current.ns1 = (current.s1l1 * depth) + 1.0;
        else
            current.ns1 = 1.0 - (current.ls1 * depth);
    }
    const auto fdcno = current.fdcno;
    double shft = current.ns1;
    int j, k, kk;
    const auto octaveDown = [&] {
        kk = fdcno - 1;
        j = (int)std::round((double)kk / 2.0);
        if (kk != (2 * j))
            kk = kk - 1;
        for (j = 0; j <= kk; j += 2)
        {
            k = j / 2;
            amp[k] = amp[j];
            freq[k] = (float)(freq[j] / 2.0);
            amp[k + 1] = 0.0f;
            freq[k + 1] = 0.0f;
        }
        kk = kk / 2;
        kk = kk + 2;
        for (j = kk; j < fdcno; j++)
        {
            amp[j] = 0.0f;
            freq[j] = 0.0f;
        }
    };
    const auto shiftDown = [&] {
        if (shft > 1.0)
        {
            j = fdcno - 1;
            k = (int)std::round((double)j / shft);
            while (k > 0)
            {
                amp[j] = amp[k];
                freq[j] = (float)(shft * freq[k]);
                j--;
                k = (int)std::round((double)j / shft);
            }
            for (k = j; k > 0; k--)
            {
                amp[k] = 0.0f;
                freq[k] = 0.0f;
            }
        }
        else
        {
            j = 1;
            k = (int)std::round((double)j / shft);
            while (k <= fdcno - 1)
            {
                amp[j] = amp[k];
                freq[j] = (float)(shft * freq[k]);
                j++;
                k = (int)std::round((double)j / shft);
            }
            for (k = j; k < fdcno; k++)
            {
                amp[k] = 0.0f;
                freq[k] = 0.0f;
            }
        }
    };
    switch (mode_)
    {
    case Mode::OctaveUp:
    case Mode::OctaveUpAndDown:
        k = fdcno * 2;
        for (j = clength - 1; j >= k; j += -2)
        {
            amp[j] = amp[j / 2];
            freq[j] = (float)(2.0 * freq[j / 2]);
            amp[j - 1] = 0.0f;
            freq[j - 1] = 0.0f;
        }
        for (j = fdcno; j < k; j++)
        {
            amp[j] = 0.0f;
            freq[j] = 0.0f;
        }
        if (mode_ == Mode::OctaveUpAndDown)
            octaveDown();
        break;
    case Mode::OctaveDown:
        octaveDown();
        break;
    case Mode::ShiftUp:
    case Mode::ShiftUpAndDown:
        if (shft > 1.0)
        {
            j = clength - 1;
            k = (int)std::round((double)j / shft);
            while (k >= fdcno)
            {
                amp[j] = amp[k];
                freq[j] = (float)(shft * freq[k]);
                j--;
                k = (int)std::round((double)j / shft);
            }
            for (k = j; k > fdcno; k--)
            {
                amp[k] = 0.0f;
                freq[k] = 0.0f;
            }
        }
        else
        {
            j = fdcno;
            k = (int)std::round((double)j / shft);
            while (k <= (clength - 1))
            {
                amp[j] = amp[k];
                freq[j] = (float)(shft * freq[k]);
                j++;
                k = (int)std::round((double)j / shft);
            }
            for (k = j; k < clength; k++)
            {
                amp[k] = 0.0f;
                freq[k] = 0.0f;
            }
        }
        if (mode_ == Mode::ShiftUpAndDown)
        {
            shft = current.ns2;
            shiftDown();
        }
        break;
    case Mode::ShiftDown:
        shiftDown();
        break;
    }
    // put_amp_and_frq.
    for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        frame[static_cast<std::size_t>(vc)] = amp[cc];
        frame[static_cast<std::size_t>(vc) + 1] = freq[cc];
    }
}

bool StretchSpectrum::prepare(double sampleRate, int points, int overlap, Mode mode, double divide,
    double stretch, double exponent, bool hasDepth, double depth, bool liveDepth)
{
    const auto nyquist = sampleRate / 2.0;
    PhaseVocoder vocoder;
    if (mode < Mode::Above || mode > Mode::Below || !std::isfinite(divide) || divide < 5.0 || divide > nyquist
        || !std::isfinite(stretch) || stretch < SPEC_MINFRQ / nyquist || stretch > nyquist / SPEC_MINFRQ
        || !std::isfinite(exponent) || exponent < minimumExponent || exponent > maximumExponent
        || (hasDepth && (!std::isfinite(depth) || depth < 0.0 || depth > 1.0))
        || !vocoder.prepare(sampleRate, points, overlap))
    {
        return false;
    }
    vocoder_ = std::move(vocoder);
    mode_ = mode;
    exponent_ = exponent;
    const auto clength = static_cast<int>(vocoder_.frameSize() / 2);
    const auto chwidth = nyquist / (double)(clength - 1);
    // setup_pitchzero and set_internal_stretch_params.
    auto ffrq = divide < SPEC_MINFRQ ? 0.0 : divide;
    ffrq += chwidth / 2.0;
    fdcno_ = (int)std::floor(ffrq / chwidth);
    liveDepth_ = hasDepth && liveDepth;
    if (hasDepth)
    {
        sl1_ = stretch - 1.0;
        nshift_ = (sl1_ * depth) + 1.0;
    }
    else
        nshift_ = stretch;
    amp_.assign(static_cast<std::size_t>(clength), 0.0F);
    freq_.assign(static_cast<std::size_t>(clength), 0.0F);
    startDepth_ = depth;
    reset();
    return true;
}

bool StretchSpectrum::setDepth(double depth) noexcept
{
    if (!prepared() || !std::isfinite(depth) || depth < 0.0 || depth > 1.0)
        return false;
    depth_ = depth;
    return true;
}

void StretchSpectrum::reset() noexcept
{
    vocoder_.reset();
    depth_ = startDepth_;
}

bool StretchSpectrum::process(std::span<const double> input, std::span<double> output) noexcept
{
    const auto depth = depth_;
    return vocoder_.process(input, output, [this, depth](std::span<float> frame) {
        // specstretch: a breakpoint depth is read at each frame.
        if (liveDepth_ && flteq(depth, 0.0))
            return;
        stretchFrame(frame, liveDepth_ ? (sl1_ * depth) + 1.0 : nshift_);
    });
}

// get_amp_and_frq, stretch_spectrum and put_amp_and_frq.
void StretchSpectrum::stretchFrame(std::span<float> frame, double shift) noexcept
{
    const auto clength = static_cast<int>(amp_.size());
    float* amp = amp_.data();
    float* freq = freq_.data();
    for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        amp[cc] = frame[static_cast<std::size_t>(vc)];
        freq[cc] = frame[static_cast<std::size_t>(vc) + 1];
    }
    const auto fdcno = fdcno_;
    int j, k, spectop, specbot;
    double chshift, ratio, str_part;
    double slone, onels, flone;
    switch (mode_)
    {
    case Mode::Above:
        if (shift >= 1.0)
        {
            slone = shift - 1.0;
            j = clength - 1;
            str_part = (double)((clength - 1) - fdcno);
            chshift = shift;
            k = (int)std::round((double)j / chshift);
            while (k >= fdcno)
            {
                amp[j] = amp[k];
                freq[j] = (float)chshift * freq[k];
                if (--j <= fdcno)
                    break;
                ratio = (double)(j - fdcno) / str_part;
                ratio = std::pow(ratio, exponent_);
                chshift = (ratio * (slone)) + 1.0;
                k = (int)std::round((double)j / chshift);
            }
        }
        else
        {
            onels = 1.0 - shift;
            spectop = (int)std::round((double)(clength - 1) * shift);
            str_part = (float)(spectop - fdcno);
            chshift = 1.0;
            k = fdcno;
            j = k;
            while ((k <= (clength - 1)) && (j <= spectop))
            {
                amp[j] = amp[k];
                freq[j] = (float)chshift * freq[k];
                j++;
                ratio = (double)(j - fdcno) / str_part;
                ratio = std::pow(ratio, exponent_);
                chshift = 1.0 - (ratio * onels);
                k = (int)std::round((double)j / chshift);
            }
        }
        break;
    case Mode::Below:
        flone = fdcno - 1;
        if (shift <= 1.0)
        {
            onels = 1.0 - shift;
            j = 1;
            k = (int)std::round((double)j / shift);
            chshift = shift;
            while (k < fdcno)
            {
                amp[j] = amp[k];
                freq[j] = (float)chshift * freq[k];
                j++;
                ratio = (double)j / flone;
                ratio = std::pow(ratio, exponent_);
                chshift = shift + (ratio * onels);
                k = (int)std::round((double)j / chshift);
            }
        }
        else
        {
            slone = shift - 1.0;
            j = fdcno - 1;
            k = j;
            specbot = (int)std::round(shift);
            str_part = flone - (double)specbot;
            chshift = 1.0;
            while ((k > 0) && (j >= specbot))
            {
                amp[j] = amp[k];
                freq[j] = (float)chshift * freq[k];
                j--;
                ratio = (flone - (double)j) / str_part;
                ratio = std::pow(ratio, exponent_);
                chshift = (ratio * slone) + 1.0;
                k = (int)std::round((double)j / chshift);
            }
        }
        break;
    }
    for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        frame[static_cast<std::size_t>(vc)] = amp[cc];
        frame[static_cast<std::size_t>(vc) + 1] = freq[cc];
    }
}
}
