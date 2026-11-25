/*
 * Derived from the CDP System, Release 8: dev/blur/blur.c and ap_blur.c,
 * dev/hilite/hilite.c, and dev/cdp2k/tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralBlur.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h.
constexpr double BIGAMP = 10000.0;
// dev/include/speccon.h.
constexpr double SPEC_MINFRQ = 10.0;

[[nodiscard]] double pitchzero(double frequency, bool live) noexcept
{
    if (frequency < SPEC_MINFRQ)
        return live ? SPEC_MINFRQ : 0.0;
    return frequency;
}

// read_value_from_brktable's rounding of an integer field, within a range.
[[nodiscard]] bool roundsWithin(double value, int low, int high) noexcept
{
    if (!std::isfinite(value))
        return false;
    const auto rounded = std::round(value);
    return rounded >= low && rounded <= high;
}

// CDP's ring of the size loudest (or quietest) channels, which
// initialise_ring_vals fills with the initial amplitude: an amplitude
// enters before the first entry it beats, ties staying behind, and the last
// entry falls out (if_one_of_loudest_chans_store_in_ring,
// if_one_of_quietest_chans_store_in_ring and
// rearrange_ring_to_allow_new_entry_and_return_entry_address).
class Ring
{
public:
    Ring(std::vector<std::pair<int, float>>& entries, std::size_t size, bool loudest) noexcept
        : entries_(entries), size_(size), loudest_(loudest)
    {
        const auto initial = (float)(loudest ? -BIGAMP : BIGAMP);
        std::fill(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(size_),
            std::pair{0, initial});
    }

    void store(int loc, float val) noexcept
    {
        const auto begin = entries_.begin();
        const auto end = begin + static_cast<std::ptrdiff_t>(size_);
        const auto place = std::partition_point(begin, end, [&](const std::pair<int, float>& entry) {
            return loudest_ ? !(val > entry.second) : !(val < entry.second);
        });
        if (place == end)
            return;
        std::move_backward(place, end - 1, end);
        *place = {loc, val};
    }

    [[nodiscard]] std::span<const std::pair<int, float>> entries() const noexcept
    {
        return {entries_.data(), size_};
    }

private:
    std::vector<std::pair<int, float>>& entries_;
    std::size_t size_;
    bool loudest_;
};
}

bool BlurAverage::prepare(double sampleRate, int points, int overlap, double n)
{
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    const auto clength = static_cast<int>(vocoder.frameSize() / 2);
    if (!roundsWithin(n, 2, clength))
        return false;
    vocoder_ = std::move(vocoder);
    amp_.assign(static_cast<std::size_t>(clength), 0.0F);
    startN_ = static_cast<int>(std::round(n));
    reset();
    return true;
}

bool BlurAverage::setN(double n) noexcept
{
    if (!prepared() || !roundsWithin(n, 2, static_cast<int>(amp_.size())))
        return false;
    n_ = static_cast<int>(std::round(n));
    return true;
}

void BlurAverage::reset() noexcept
{
    vocoder_.reset();
    n_ = startN_;
    frames_ = 0;
}

bool BlurAverage::process(std::span<const double> input, std::span<double> output) noexcept
{
    // An odd span about each channel.
    const auto span = n_ / 2;
    const auto avrg = (span * 2) + 1;
    return vocoder_.process(input, output, [this, span, avrg](std::span<float> frame) {
        // skip_or_special_operation_on_window_zero leaves the first frame.
        if (frames_++ == 0 || span <= 0)
            return;
        // specavrg.
        const auto clength = static_cast<int>(amp_.size());
        float* amp = amp_.data();
        const float* flbufptr = frame.data();
        int cc, vc, k = 0, n, m, q;
        for (cc = 0; cc < clength; cc++)
            amp[cc] = flbufptr[cc * 2];
        for (cc = 0, vc = 0; cc < span; cc++, vc += 2)
        {
            amp[cc] = 0.0f;
            k = cc + span + 1;
            for (n = 0, m = 0; n < k; n++, m += 2)
                amp[cc] = (float)(amp[cc] + flbufptr[m]);
            amp[cc] = (float)(amp[cc] / (double)(k));
        }
        q = clength - span;
        m = span * 2;
        for (cc = span, vc = m; cc < q; cc++, vc += 2)
        {
            amp[cc] = 0.0f;
            for (n = vc - m; n <= vc + m; n += 2)
                amp[cc] = (float)(amp[cc] + flbufptr[n]);
            amp[cc] = (float)(amp[cc] / (double)avrg);
        }
        for (cc = q; cc < clength; cc++)
        {
            amp[cc] = 0.0f;
            k = cc - span;
            for (n = k, m = k * 2; n < clength; n++, m += 2)
                amp[cc] = (float)(amp[cc] + flbufptr[m]);
            amp[cc] = (float)(amp[cc] / (double)(clength - k));
        }
        for (cc = 0; cc < clength; cc++)
            frame[static_cast<std::size_t>(cc) * 2] = amp[cc];
    });
}

bool BlurSuppress::prepare(double sampleRate, int points, int overlap, double n)
{
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    const auto clength = static_cast<int>(vocoder.frameSize() / 2);
    if (!roundsWithin(n, 1, clength - 1))
        return false;
    vocoder_ = std::move(vocoder);
    ring_.assign(static_cast<std::size_t>(clength), {0, 0.0F});
    startN_ = static_cast<int>(std::round(n));
    reset();
    return true;
}

bool BlurSuppress::setN(double n) noexcept
{
    if (!prepared() || !roundsWithin(n, 1, static_cast<int>(ring_.size()) - 1))
        return false;
    n_ = static_cast<int>(std::round(n));
    return true;
}

void BlurSuppress::reset() noexcept
{
    vocoder_.reset();
    n_ = startN_;
}

bool BlurSuppress::process(std::span<const double> input, std::span<double> output) noexcept
{
    const auto indx = n_;
    return vocoder_.process(input, output, [this, indx](std::span<float> frame) {
        // specsupr.
        const auto clength = static_cast<int>(ring_.size());
        if (indx > (clength / 2))
        {
            // More channels to suppress than to keep: keep the quietest.
            Ring quietest(ring_, static_cast<std::size_t>(clength - indx), false);
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            {
                quietest.store(static_cast<int>(vc), frame[vc]);
                frame[vc] = 0.0f;
            }
            for (const auto& [loc, val] : quietest.entries())
                frame[static_cast<std::size_t>(loc)] = val;
        }
        else
        {
            Ring loudest(ring_, static_cast<std::size_t>(indx), true);
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
                loudest.store(static_cast<int>(vc), frame[vc]);
            for (const auto& [loc, val] : loudest.entries())
                frame[static_cast<std::size_t>(loc)] = 0.0f;
        }
    });
}

bool HiliteTrace::validN(double n) const noexcept
{
    const auto clength = static_cast<int>(ring_.size());
    return roundsWithin(n, 1, mode_ == Mode::All ? clength : clength - 1);
}

bool HiliteTrace::prepare(double sampleRate, int points, int overlap, Mode mode, double n,
    double lowFrequency, double highFrequency, bool retain, bool liveLow, bool liveHigh)
{
    const auto nyquist = sampleRate / 2.0;
    if (mode < Mode::All || mode > Mode::Between || !std::isfinite(lowFrequency)
        || !std::isfinite(highFrequency) || lowFrequency < minimumFrequency
        || highFrequency < minimumFrequency || lowFrequency > nyquist || highFrequency > nyquist)
    {
        return false;
    }
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    const auto clength = static_cast<int>(vocoder.frameSize() / 2);
    const auto previousMode = mode_;
    const auto previousRing = ring_.size();
    mode_ = mode;
    ring_.resize(static_cast<std::size_t>(clength));
    if (!validN(n))
    {
        mode_ = previousMode;
        ring_.resize(previousRing);
        return false;
    }
    vocoder_ = std::move(vocoder);
    nyquist_ = nyquist;
    retain_ = retain && mode != Mode::All;
    // The frequencies a mode does not read stay at CDP's defaults: fixed
    // PITCHZERO and the Nyquist frequency.
    liveLow_ = liveLow && (mode == Mode::Above || mode == Mode::Between);
    liveHigh_ = liveHigh && (mode == Mode::Below || mode == Mode::Between);
    startN_ = static_cast<int>(std::round(n));
    startLow_ = mode == Mode::Above || mode == Mode::Between ? lowFrequency : minimumFrequency;
    startHigh_ = mode == Mode::Below || mode == Mode::Between ? highFrequency : nyquist;
    reset();
    return true;
}

bool HiliteTrace::setControls(double n, double lowFrequency, double highFrequency) noexcept
{
    if (!prepared() || !validN(n) || !std::isfinite(lowFrequency) || !std::isfinite(highFrequency)
        || lowFrequency < minimumFrequency || highFrequency < minimumFrequency || lowFrequency > nyquist_
        || highFrequency > nyquist_)
    {
        return false;
    }
    n_ = static_cast<int>(std::round(n));
    if (mode_ == Mode::Above || mode_ == Mode::Between)
        low_ = pitchzero(lowFrequency, liveLow_);
    if (mode_ == Mode::Below || mode_ == Mode::Between)
        high_ = pitchzero(highFrequency, liveHigh_);
    return true;
}

void HiliteTrace::reset() noexcept
{
    vocoder_.reset();
    n_ = startN_;
    low_ = pitchzero(startLow_, liveLow_);
    high_ = pitchzero(startHigh_, liveHigh_);
}

bool HiliteTrace::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) { trace(frame); });
}

// spectrace.
void HiliteTrace::trace(std::span<float> frame) noexcept
{
    const auto clength = static_cast<int>(ring_.size());
    const auto indx = n_;
    int chans_outside_fltband_to_keep = 0, chans_in_filtband;
    double hifrq_limit = 0.0, lofrq_limit = 0.0;
    if ((mode_ == Mode::All || retain_) && indx >= clength)
        return;
    const auto outside = [&](std::size_t vc) {
        return frame[vc + 1] < lofrq_limit || frame[vc + 1] > hifrq_limit;
    };
    if (mode_ != Mode::All)
    {
        hifrq_limit = high_;
        lofrq_limit = low_;
        if (hifrq_limit < lofrq_limit)
            std::swap(hifrq_limit, lofrq_limit);
        if (retain_)
        {
            chans_in_filtband = clength;
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            {
                if (outside(vc))
                    chans_in_filtband--;
            }
            if ((chans_outside_fltband_to_keep = indx - chans_in_filtband) <= 0)
            {
                chans_outside_fltband_to_keep = 0;
                for (std::size_t vc = 0; vc < frame.size(); vc += 2)
                {
                    if (outside(vc))
                        frame[vc] = 0.0f;
                }
            }
        }
        else
        {
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            {
                if (outside(vc))
                    frame[vc] = 0.0f;
            }
        }
    }
    if (chans_outside_fltband_to_keep > 0)
    {
        // The loudest outside the band keep their amplitudes.
        Ring loudest(ring_, static_cast<std::size_t>(chans_outside_fltband_to_keep), true);
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        {
            if (outside(vc))
            {
                loudest.store(static_cast<int>(vc), frame[vc]);
                frame[vc] = 0.0F;
            }
        }
        for (const auto& [loc, val] : loudest.entries())
            frame[static_cast<std::size_t>(loc)] = val;
    }
    else if (indx > (clength / 2))
    {
        // More channels to keep than to reject: silence the quietest.
        Ring quietest(ring_, static_cast<std::size_t>(clength - indx), false);
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            quietest.store(static_cast<int>(vc), frame[vc]);
        for (const auto& [loc, val] : quietest.entries())
            frame[static_cast<std::size_t>(loc)] = 0.0F;
    }
    else
    {
        Ring loudest(ring_, static_cast<std::size_t>(indx), true);
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        {
            loudest.store(static_cast<int>(vc), frame[vc]);
            frame[vc] = 0.0F;
        }
        for (const auto& [loc, val] : loudest.entries())
            frame[static_cast<std::size_t>(loc)] = val;
    }
}

namespace
{
// read_value_from_brktable on a table convert_brkpnt_table_vals_to_log made,
// at a time between two equal points, and specfilt's exponential.
[[nodiscard]] double logInterpolated(double value, bool live) noexcept
{
    return live ? std::exp(std::log(value)) : value;
}

[[nodiscard]] bool bandMode(HiliteFilter::Mode mode) noexcept
{
    return mode >= HiliteFilter::Mode::BandPass;
}
}

bool HiliteFilter::valid(double frequency, double highFrequency, double q) const noexcept
{
    return std::isfinite(frequency) && std::isfinite(highFrequency) && std::isfinite(q)
        && frequency >= minimumFrequency && frequency <= nyquist_ && highFrequency >= minimumFrequency
        && highFrequency <= nyquist_ && q >= minimumQ && q <= nyquist_;
}

bool HiliteFilter::prepare(double sampleRate, int points, int overlap, Mode mode, double frequency,
    double highFrequency, double q, double gain, bool liveFrequency, bool liveHighFrequency, bool liveQ)
{
    const auto previousNyquist = nyquist_;
    nyquist_ = sampleRate / 2.0;
    if (mode < Mode::HighPass || mode > Mode::NotchGain || !std::isfinite(gain) || gain < 0.0
        || gain > maximumGain || !valid(frequency, bandMode(mode) ? highFrequency : nyquist_, q))
    {
        nyquist_ = previousNyquist;
        return false;
    }
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
    {
        nyquist_ = previousNyquist;
        return false;
    }
    vocoder_ = std::move(vocoder);
    mode_ = mode;
    gain_ = gain;
    startFrequency_ = frequency;
    startHighFrequency_ = bandMode(mode) ? highFrequency : nyquist_;
    startQ_ = q;
    liveFrequency_ = liveFrequency;
    liveHighFrequency_ = liveHighFrequency && bandMode(mode);
    liveQ_ = liveQ;
    reset();
    return true;
}

bool HiliteFilter::setControls(double frequency, double highFrequency, double q) noexcept
{
    if (!prepared() || !valid(frequency, bandMode(mode_) ? highFrequency : nyquist_, q))
        return false;
    frequency_ = logInterpolated(frequency, liveFrequency_);
    if (bandMode(mode_))
        highFrequency_ = logInterpolated(highFrequency, liveHighFrequency_);
    q_ = logInterpolated(q, liveQ_);
    return true;
}

void HiliteFilter::reset() noexcept
{
    vocoder_.reset();
    frequency_ = logInterpolated(startFrequency_, liveFrequency_);
    highFrequency_ = logInterpolated(startHighFrequency_, liveHighFrequency_);
    q_ = logInterpolated(startQ_, liveQ_);
}

bool HiliteFilter::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) { filter(frame); });
}

// specfilt and do_filtering.
void HiliteFilter::filter(std::span<float> frame) noexcept
{
    double frq_limit = frequency_, hifrq_limit = 0.0, pre_amptotal = 0.0, post_amptotal = 0.0;
    if (bandMode(mode_))
    {
        hifrq_limit = highFrequency_;
        if (frq_limit > hifrq_limit)
            std::swap(frq_limit, hifrq_limit);
    }
    const auto amp = [&](std::size_t vc) -> float& { return frame[vc]; };
    const auto frq = [&](std::size_t vc) { return frame[vc + 1]; };
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        pre_amptotal += amp(vc);
    double loskirt, loskirt_bwidth, hiskirt, hiskirt_bwidth;
    switch (mode_)
    {
    case Mode::HighPass:
    case Mode::HighPassNormalised:
    case Mode::HighPassGain:
        loskirt = std::max(0.0, (frq_limit - q_));
        loskirt_bwidth = frq_limit - loskirt;
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        {
            if (frq(vc) < loskirt)
                amp(vc) = 0.0F;
            else if (frq(vc) < frq_limit)
                amp(vc) = (float)(amp(vc) * ((frq(vc) - loskirt) / loskirt_bwidth));
        }
        break;
    case Mode::LowPass:
    case Mode::LowPassNormalised:
    case Mode::LowPassGain:
        hiskirt = std::min(nyquist_, (frq_limit + q_));
        hiskirt_bwidth = hiskirt - frq_limit;
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        {
            if (frq(vc) > hiskirt)
                amp(vc) = 0.0F;
            else if (frq(vc) > frq_limit)
                amp(vc) = (float)(amp(vc) * ((hiskirt - frq(vc)) / hiskirt_bwidth));
        }
        break;
    case Mode::BandPass:
    case Mode::BandPassNormalised:
    case Mode::BandPassGain:
        loskirt = std::max(0.0, (frq_limit - q_));
        loskirt_bwidth = frq_limit - loskirt;
        hiskirt = std::min(nyquist_, (hifrq_limit + q_));
        hiskirt_bwidth = hiskirt - hifrq_limit;
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        {
            if (frq(vc) < loskirt)
                amp(vc) = 0.0F;
            else if (frq(vc) < frq_limit)
                amp(vc) = (float)(amp(vc) * ((frq(vc) - loskirt) / loskirt_bwidth));
            else if (frq(vc) > hiskirt)
                amp(vc) = 0.0F;
            else if (frq(vc) > hifrq_limit)
                amp(vc) = (float)(amp(vc) * ((hiskirt - frq(vc)) / hiskirt_bwidth));
        }
        break;
    case Mode::Notch:
    case Mode::NotchNormalised:
    case Mode::NotchGain:
        loskirt = std::max(0.0, (frq_limit - q_));
        loskirt_bwidth = frq_limit - loskirt;
        hiskirt = std::min(nyquist_, (hifrq_limit + q_));
        hiskirt_bwidth = hiskirt - hifrq_limit;
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        {
            if (frq(vc) < loskirt || frq(vc) > hiskirt)
                continue;
            if (frq(vc) < frq_limit)
                amp(vc) = (float)(amp(vc) * (1.0 - ((frq(vc) - loskirt) / loskirt_bwidth)));
            else if (frq(vc) > hifrq_limit)
                amp(vc) = (float)(amp(vc) * (1.0 - ((hiskirt - frq(vc)) / hiskirt_bwidth)));
            else
                amp(vc) = 0.0F;
        }
        break;
    }
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        post_amptotal += amp(vc);
    switch (mode_)
    {
    case Mode::HighPassNormalised:
    case Mode::LowPassNormalised:
    case Mode::BandPassNormalised:
    case Mode::NotchNormalised:
        // normalise.
        if (post_amptotal >= 0.00000000000000000001)
        {
            const auto normaliser = pre_amptotal / post_amptotal;
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
                amp(vc) = (float)(amp(vc) * normaliser);
        }
        break;
    case Mode::HighPassGain:
    case Mode::LowPassGain:
    case Mode::BandPassGain:
    case Mode::NotchGain:
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            amp(vc) = (float)(amp(vc) * gain_);
        break;
    default:
        break;
    }
}

bool HilitePluck::prepare(double sampleRate, int points, int overlap, double gain)
{
    if (!std::isfinite(gain) || gain < 0.0 || gain > maximumGain)
        return false;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    sounding_.assign(vocoder_.frameSize() / 2, false);
    startGain_ = gain;
    reset();
    return true;
}

bool HilitePluck::setGain(double gain) noexcept
{
    if (!prepared() || !std::isfinite(gain) || gain < 0.0 || gain > maximumGain)
        return false;
    gain_ = gain;
    return true;
}

void HilitePluck::reset() noexcept
{
    vocoder_.reset();
    std::fill(sounding_.begin(), sounding_.end(), false);
    gain_ = startGain_;
    frames_ = 0;
}

bool HilitePluck::process(std::span<const double> input, std::span<double> output) noexcept
{
    const auto gain = gain_;
    return vocoder_.process(input, output, [this, gain](std::span<float> frame) {
        // flteq against zero, within FLTERR.
        const auto zero = [](float amp) { return (double)amp <= 0.000002 && (double)amp >= -0.000002; };
        const auto frameIndex = frames_++;
        // skip_or_special_operation_on_window_zero leaves the first frame.
        if (frameIndex == 0)
            return;
        // specpluck's bit flags, set from the second frame.
        for (std::size_t cc = 0, vc = 0; cc < sounding_.size(); cc++, vc += 2)
        {
            if (frameIndex == 1)
            {
                if (!zero(frame[vc]))
                    sounding_[cc] = true;
            }
            else if (!zero(frame[vc]))
            {
                if (!sounding_[cc])
                {
                    frame[vc] = (float)(frame[vc] * gain);
                    sounding_[cc] = true;
                }
            }
            else if (sounding_[cc])
                sounding_[cc] = false;
        }
    });
}
}
