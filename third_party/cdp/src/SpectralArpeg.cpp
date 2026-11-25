/*
 * Derived from the CDP System, Release 8: dev/hilite/hilite.c and
 * ap_hilite.c, and dev/cdp2k/tklib1.c and tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralArpeg.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <tuple>
#include <utility>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h.
constexpr double PI = 3.141592654;
// dev/include/speccon.h.
constexpr double SPEC_MINFRQ = 10.0;
constexpr int ARPE_TABSIZE = 1024;
constexpr int AP_SUSTAIN_PITCH = 1;
constexpr int AP_LIMIT_SUSTAIN = 2;
constexpr int AP_NONLIN = 4;

using Mode = HiliteArpeg::Mode;
using Wave = HiliteArpeg::Wave;

[[nodiscard]] double pitchzero(double frequency, bool live) noexcept
{
    if (frequency < SPEC_MINFRQ)
        return live ? SPEC_MINFRQ : 0.0;
    return frequency;
}

[[nodiscard]] bool readsBandwidth(Mode mode) noexcept
{
    return mode != Mode::Below && mode != Mode::Above;
}

// Modes 1 to 4 (CDP's ON, BOOST, BELOW_BOOST and ABOVE_BOOST, before BELOW).
[[nodiscard]] bool sustains(Mode mode) noexcept
{
    return mode < Mode::Below;
}

// The frame rate and frame time CDP reads from the .ana file.
[[nodiscard]] std::optional<float> frameRate(double sampleRate, int points, int overlap) noexcept
{
    const auto d = PhaseVocoder::decimationFor(points, overlap);
    if (!d.has_value() || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return std::nullopt;
    return (float)((float)sampleRate / (float)*d);
}

// The channel width, nyquist / (clength - 1).
[[nodiscard]] double channelWidth(double sampleRate, int points) noexcept
{
    const auto chans = points + (points % 2);
    const auto clength = chans / 2 + 1;
    return (sampleRate / 2.0) / (double)(clength - 1);
}
}

double HiliteArpeg::maximumRate(double sampleRate, int points, int overlap) noexcept
{
    const auto arate = frameRate(sampleRate, points, overlap);
    return arate.has_value() ? (double)*arate : 0.0;
}

double HiliteArpeg::minimumBandwidth(double sampleRate, int points, int overlap) noexcept
{
    if (!frameRate(sampleRate, points, overlap).has_value())
        return 0.0;
    return channelWidth(sampleRate, points);
}

bool HiliteArpeg::valid(double rate, double lowFrequency, double highFrequency, double bandwidth,
    double amplification, double sustain) const noexcept
{
    if (!std::isfinite(rate) || !std::isfinite(lowFrequency) || !std::isfinite(highFrequency)
        || !std::isfinite(amplification) || rate < 0.0 || rate > arate_ || lowFrequency < minimumFrequency
        || lowFrequency > nyquist_ || highFrequency < minimumFrequency || highFrequency > nyquist_
        || amplification < 0.0 || amplification > maximumAmplification)
    {
        return false;
    }
    if (readsBandwidth(mode_)
        && (!std::isfinite(bandwidth) || bandwidth < chwidth_ || bandwidth > nyquist_))
    {
        return false;
    }
    return !sustains(mode_)
        || (std::isfinite(sustain) && sustain >= 1.0 && sustain <= maximumSustainSeconds / frametime_);
}

bool HiliteArpeg::prepare(double sampleRate, int points, int overlap, Mode mode, Wave wave, double rate,
    double startPhase, double lowFrequency, double highFrequency, double bandwidth, double amplification,
    double decay, double sustain, bool track, bool keep, bool liveLow, bool liveHigh, bool liveAmplification,
    bool liveSustain)
{
    if (mode < Mode::On || mode > Mode::OnceAbove || wave < Wave::DownRamp || wave > Wave::UpRamp)
        return false;
    // The waves CDP allows each mode, and the start phase check_arpe_parameters_are_compatible asks.
    const auto sineOrSaw = wave == Wave::Sine || wave == Wave::Saw;
    if (((mode == Mode::BelowBoost || mode == Mode::OnceBelow) && wave == Wave::DownRamp)
        || ((mode == Mode::AboveBoost || mode == Mode::OnceAbove) && wave == Wave::UpRamp)
        || !std::isfinite(startPhase) || startPhase < 0.0 || startPhase > 1.0
        || ((mode == Mode::AboveBoost || mode == Mode::OnceAbove) && sineOrSaw && startPhase < 0.5))
    {
        return false;
    }
    if (sustains(mode) && (!std::isfinite(decay) || decay < minimumDecay || decay > maximumDecay))
        return false;
    const auto arate = frameRate(sampleRate, points, overlap);
    if (!arate.has_value())
        return false;
    const auto previous = std::tuple{mode_, arate_, frametime_, nyquist_, chwidth_};
    mode_ = mode;
    arate_ = *arate;
    frametime_ = (float)(1.0 / arate_);
    nyquist_ = sampleRate / 2.0;
    chwidth_ = channelWidth(sampleRate, points);
    PhaseVocoder vocoder;
    if (!valid(rate, lowFrequency, highFrequency, bandwidth, amplification, sustain)
        || !vocoder.prepare(sampleRate, points, overlap))
    {
        std::tie(mode_, arate_, frametime_, nyquist_, chwidth_) = previous;
        return false;
    }
    vocoder_ = std::move(vocoder);
    const auto clength = vocoder_.frameSize() / 2;
    wave_ = wave;
    startPhase_ = startPhase;
    decay_ = sustains(mode) ? decay : 1.0;
    // set_arpegtones_sustain_type: -N, which the port takes as a decay
    // other than 1, its exponent's identity.
    susflag_ = 0;
    if (sustains(mode))
    {
        if (!track)
            susflag_ |= AP_SUSTAIN_PITCH;
        if (!keep)
            susflag_ |= AP_LIMIT_SUSTAIN;
        if (decay_ != 1.0)
            susflag_ |= AP_NONLIN;
    }
    liveLow_ = liveLow;
    liveHigh_ = liveHigh;
    liveAmplification_ = liveAmplification;
    liveSustain_ = liveSustain && sustains(mode);
    startRate_ = rate;
    startLow_ = lowFrequency;
    startHigh_ = highFrequency;
    // Modes 5 and 6 keep CDP's default.
    startBandwidth_ = readsBandwidth(mode) ? bandwidth : chwidth_;
    startAmplification_ = amplification;
    startSustain_ = sustains(mode) ? sustain : 3.0;
    // gen_arpe_waveform_table, whose sine loop CDP's build fuses; past the
    // end CDP's heap holds the zero its rounded position can reach.
    const int halftab = ARPE_TABSIZE / 2;
    const double anglestep = (PI * 2.0) / ARPE_TABSIZE;
    table_.assign(static_cast<std::size_t>(ARPE_TABSIZE) + 1, 0.0);
    double* tab = table_.data();
    switch (wave)
    {
    case Wave::Sine:
        for (int n = 0; n < ARPE_TABSIZE; n++)
        {
            tab[n] = std::sin(std::fma((double)n, anglestep, PI * 1.5));
            tab[n] += 1.0;
            tab[n] /= 2.0;
        }
        break;
    case Wave::Saw:
        for (int n = 0; n < halftab; n++)
            tab[n] = (double)n / (double)halftab;
        for (int n = 0, m = halftab; n < halftab; n++)
            tab[m++] = 1.0 - tab[n];
        break;
    case Wave::DownRamp:
        for (int n = 0; n < ARPE_TABSIZE; n++)
            tab[n] = 1.0 - (double)n / (double)ARPE_TABSIZE;
        break;
    case Wave::UpRamp:
        for (int n = 0; n < ARPE_TABSIZE; n++)
            tab[n] = (double)n / (double)ARPE_TABSIZE;
        break;
    }
    keep_.assign(clength, 0);
    windowbuf_.assign(vocoder_.frameSize(), 0.0F);
    reset();
    return true;
}

bool HiliteArpeg::setControls(double rate, double lowFrequency, double highFrequency, double bandwidth,
    double amplification, double sustain) noexcept
{
    if (!prepared() || !valid(rate, lowFrequency, highFrequency, bandwidth, amplification, sustain))
        return false;
    rate_ = rate;
    low_ = pitchzero(lowFrequency, liveLow_);
    high_ = pitchzero(highFrequency, liveHigh_);
    // establish_arpe_halfband_param.
    if (readsBandwidth(mode_))
        halfBand_ = bandwidth / 2.0;
    amplification_ = amplification;
    // read_value_from_brktable rounds the sustain.
    if (sustains(mode_))
        sustain_ = (int)std::lround(sustain);
    return true;
}

void HiliteArpeg::reset() noexcept
{
    vocoder_.reset();
    rate_ = startRate_;
    low_ = pitchzero(startLow_, liveLow_);
    high_ = pitchzero(startHigh_, liveHigh_);
    halfBand_ = startBandwidth_ / 2.0;
    amplification_ = startAmplification_;
    sustain_ = (int)std::lround(startSustain_);
    // setup_internal_arpe_params_and_preprocess_params; the amplification
    // is divided at each frame.
    wavetabpos_ = startPhase_ * (double)ARPE_TABSIZE;
    lastTabpos_ = 0.0;
    lastRate_ = rate_;
    ampl_ = amplification_;
    started_ = false;
    // outer_loop's in_start_portion.
    inStartPortion_ = true;
    std::fill(keep_.begin(), keep_.end(), 0);
    std::fill(windowbuf_.begin(), windowbuf_.end(), 0.0F);
}

bool HiliteArpeg::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) { arpeggiate(frame); });
}

// specarpe.
void HiliteArpeg::arpeggiate(std::span<float> frame) noexcept
{
    // reset_timechanging_arpe_variables: CDP reads a breakpoint rate at time
    // zero before the first frame, so that frame's mean is its own rate.
    if (!started_)
    {
        lastRate_ = rate_;
        started_ = true;
    }
    const auto this_arpfrq = (lastRate_ + rate_) / 2.0;
    lastRate_ = rate_;
    if (sustains(mode_))
    {
        if (liveAmplification_ || liveSustain_)
        {
            if (liveAmplification_)
                ampl_ = amplification_;
            ampl_ /= (double)sustain_;
        }
        else
            ampl_ = amplification_ / (double)sustain_;
    }
    else
        ampl_ = amplification_;
    auto lofrq = low_;
    auto hifrq = high_;
    if (hifrq < lofrq)
        std::swap(hifrq, lofrq);
    const auto frqrang = hifrq - lofrq;
    // locate_current_wavetable_position.
    const double tabincr = frametime_ * this_arpfrq * ARPE_TABSIZE;
    lastTabpos_ = wavetabpos_;
    wavetabpos_ = std::fmod(wavetabpos_ + tabincr, (double)ARPE_TABSIZE);
    const auto tabindex = std::lround(wavetabpos_);
    // The current band, its middle fused as CDP's build fuses it.
    const auto bandmid = std::fma(table_[static_cast<std::size_t>(tabindex)], frqrang, lofrq);
    const auto bandhilimit = std::min(bandmid + halfBand_, nyquist_);
    const auto bandlolimit = std::max(bandmid - halfBand_, 0.0);

    const auto clength = keep_.size();
    switch (mode_)
    {
    case Mode::On:
    case Mode::Boost:
        onOrBoost(frame, bandhilimit, bandlolimit);
        break;
    // do_below.
    case Mode::Below:
        for (std::size_t cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        {
            if (frame[vc + 1] > bandmid)
                frame[vc] = 0.0f;
            else
                frame[vc] = (float)(frame[vc] * ampl_);
        }
        break;
    // do_above.
    case Mode::Above:
        for (std::size_t cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        {
            if (frame[vc + 1] < bandmid)
                frame[vc] = 0.0f;
            else
                frame[vc] = (float)(frame[vc] * ampl_);
        }
        break;
    // do_below_boost.
    case Mode::BelowBoost:
        if (!stillInStartPortion())
        {
            onOrBoost(frame, bandhilimit, bandlolimit);
            break;
        }
        for (std::size_t cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        {
            if (frame[vc + 1] < bandhilimit)
                frame[vc] = 0.0f;
            else
                frame[vc] = (float)(frame[vc] * ampl_ * sustain_);
        }
        break;
    // do_above_boost.
    case Mode::AboveBoost:
        if (!stillInStartPortion())
        {
            onOrBoost(frame, bandhilimit, bandlolimit);
            break;
        }
        for (std::size_t cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        {
            if (frame[vc + 1] > bandlolimit)
                frame[vc] = 0.0f;
            else
                frame[vc] = (float)(frame[vc] * ampl_ * sustain_);
        }
        break;
    // do_once_below.
    case Mode::OnceBelow:
        if (!stillInStartPortion())
            break;
        for (std::size_t cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        {
            if (frame[vc + 1] > bandlolimit)
                frame[vc] = 0.0f;
            else
                frame[vc] = (float)(frame[vc] * ampl_);
        }
        break;
    // do_once_above.
    case Mode::OnceAbove:
        if (!stillInStartPortion())
            break;
        for (std::size_t cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        {
            if (frame[vc + 1] < bandhilimit)
                frame[vc] = 0.0f;
            else
                frame[vc] = (float)(frame[vc] * ampl_);
        }
        break;
    }
}

// The start portion's end in do_above_boost and do_once_below (a sine or
// saw at its middle, another wave at its wrap) and in do_below_boost and
// do_once_above (the wrap).
bool HiliteArpeg::stillInStartPortion() noexcept
{
    if (inStartPortion_)
    {
        if ((mode_ == Mode::AboveBoost || mode_ == Mode::OnceBelow) && (wave_ == Wave::Sine || wave_ == Wave::Saw))
        {
            if (wavetabpos_ >= ARPE_TABSIZE / 2)
                inStartPortion_ = false;
        }
        else if (wavetabpos_ < lastTabpos_)
            inStartPortion_ = false;
    }
    return inStartPortion_;
}

// do_on and do_boost, which differ only in do_on silencing the channels
// outside the band that sustain nothing, their eight sustain types folded
// into its three bits: initiate_new_arpeg_note (with_fixed_pitch),
// temporarily_sustain_arpeg_note, get_decimation and
// get_non_linear_decimation, sustain_arpeg_note and nonlin_sustain_arpeg_note.
void HiliteArpeg::onOrBoost(std::span<float> frame, double bandhilimit, double bandlolimit) noexcept
{
    const auto fixedPitch = (susflag_ & AP_SUSTAIN_PITCH) != 0;
    const auto limitSustain = (susflag_ & AP_LIMIT_SUSTAIN) != 0;
    const auto nonlinear = (susflag_ & AP_NONLIN) != 0;
    const auto silence = mode_ == Mode::On;
    const auto decimation = [&](int keep) {
        double z = (double)keep / (double)sustain_;
        if (nonlinear)
            z = std::pow(z, decay_);
        return z;
    };
    const auto initiate = [&](std::size_t cc, std::size_t vc) {
        keep_[cc] = sustain_;
        frame[vc] *= (float)(ampl_ * keep_[cc]);
        windowbuf_[vc] = frame[vc];
        if (fixedPitch)
            windowbuf_[vc + 1] = frame[vc + 1];
        keep_[cc]--;
    };
    const auto sustain = [&](std::size_t cc, std::size_t vc) {
        const auto z = decimation(keep_[cc]);
        frame[vc] = (float)(windowbuf_[vc] * z);
        if (fixedPitch)
            frame[vc + 1] = windowbuf_[vc + 1];
        keep_[cc]--;
    };
    for (std::size_t cc = 0, vc = 0; cc < keep_.size(); cc++, vc += 2)
    {
        const auto inBand = frame[vc + 1] >= bandlolimit && frame[vc + 1] <= bandhilimit;
        if (limitSustain)
        {
            if (inBand)
            {
                double thisamp;
                if (keep_[cc] && ((thisamp = windowbuf_[vc] * decimation(keep_[cc])) > frame[vc]))
                {
                    frame[vc] = (float)thisamp;
                    if (fixedPitch)
                        frame[vc + 1] = windowbuf_[vc + 1];
                    keep_[cc]--;
                }
                else
                    initiate(cc, vc);
            }
            else if (keep_[cc])
                sustain(cc, vc);
            else if (silence)
                frame[vc] = 0.0f;
        }
        else if (keep_[cc])
            sustain(cc, vc);
        else if (inBand)
            initiate(cc, vc);
        else if (silence)
            frame[vc] = 0.0f;
    }
}
}
