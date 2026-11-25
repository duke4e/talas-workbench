/*
 * Derived from the CDP System, Release 8: dev/combine/combine.c and
 * ap_combine.c, dev/formants/formants.c and ap_formants.c, and
 * dev/cdp2k/formantsg.c, tklib1.c and tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralCombine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h and speccon.h.
constexpr double FLTERR = 0.000002;
constexpr double VERY_TINY_VAL = 0.00000000000000000001;
constexpr double ONE_OVER_LN2 = 1.442695;
constexpr double SPEC_MINFRQ = 10.0;
constexpr double PITCHZERO = SPEC_MINFRQ / 2.0;

// flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const double upperbnd = f2 + FLTERR;
    const double lowerbnd = f2 - FLTERR;
    return !((f1 > upperbnd) || (f1 < lowerbnd));
}

[[nodiscard]] bool isFraction(double value) noexcept
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
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
    const auto normaliser = pre_totalamp / post_totalamp;
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        frame[vc] = (float)(frame[vc] * normaliser);
}

// pichwise_mean_of_frqs: LOG2 is CDP's log(x) * ONE_OVER_LN2, and CDP8's
// build calls exp2 for pow(2.0,...).
[[nodiscard]] double pichwise_mean_of_frqs(double frq1, double frq2) noexcept
{
    if (frq1 == 0.0)
        return frq2;
    if (frq2 == 0.0)
        return frq1;
    const double half_of_interval_in_octaves = (std::log(frq2 / frq1) * ONE_OVER_LN2) / 2.0;
    return std::exp2(half_of_interval_in_octaves) * frq1;
}

// Both phase vocoders, prepared alike.
[[nodiscard]] bool preparePair(
    PhaseVocoder& vocoder, PhaseVocoder& sideVocoder, double sampleRate, int points, int overlap)
{
    return vocoder.prepare(sampleRate, points, overlap) && sideVocoder.prepare(sampleRate, points, overlap);
}
}

bool CombineSum::prepare(double sampleRate, int points, int overlap, double crossover, bool liveCrossover)
{
    // tklib1.c: the crossover from 0 to 1.
    if (!isFraction(crossover))
        return false;
    PhaseVocoder vocoder;
    PhaseVocoder sideVocoder;
    if (!preparePair(vocoder, sideVocoder, sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    sideVocoder_ = std::move(sideVocoder);
    // get_variants_and_flags: a breakpoint file or a value other than the
    // default sets the flag.
    liveCrossover_ = liveCrossover;
    cross_ = liveCrossover || !flteq(crossover, defaultCrossover);
    startCrossover_ = crossover;
    reset();
    return true;
}

bool CombineSum::setCrossover(double crossover) noexcept
{
    if (!prepared() || !isFraction(crossover))
        return false;
    crossover_ = crossover;
    return true;
}

void CombineSum::reset() noexcept
{
    vocoder_.reset();
    sideVocoder_.reset();
    crossover_ = startCrossover_;
}

bool CombineSum::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    const auto crossover = liveCrossover_ ? crossover_ : startCrossover_;
    return vocoder_.process(input, side, sideVocoder_, output,
        [this, crossover](std::span<float> frame, std::span<const float> sideFrame) {
            // specsum.
            if (cross_)
            {
                // CDP8's build fuses the multiply and add.
                for (std::size_t vc = 0; vc < frame.size(); vc += 2)
                    frame[vc] = (float)std::fma(crossover, (double)sideFrame[vc], (double)frame[vc]);
            }
            else
            {
                for (std::size_t vc = 0; vc < frame.size(); vc += 2)
                    frame[vc] = (float)(frame[vc] + sideFrame[vc]);
            }
        });
}

bool CombineDiff::prepare(
    double sampleRate, int points, int overlap, double crossover, bool liveCrossover, bool keepNegative)
{
    // tklib1.c: the crossover from 0 to 1.
    if (!isFraction(crossover))
        return false;
    PhaseVocoder vocoder;
    PhaseVocoder sideVocoder;
    if (!preparePair(vocoder, sideVocoder, sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    sideVocoder_ = std::move(sideVocoder);
    liveCrossover_ = liveCrossover;
    cross_ = liveCrossover || !flteq(crossover, defaultCrossover);
    keepNegative_ = keepNegative;
    startCrossover_ = crossover;
    reset();
    return true;
}

bool CombineDiff::setCrossover(double crossover) noexcept
{
    if (!prepared() || !isFraction(crossover))
        return false;
    crossover_ = crossover;
    return true;
}

void CombineDiff::reset() noexcept
{
    vocoder_.reset();
    sideVocoder_.reset();
    crossover_ = startCrossover_;
}

bool CombineDiff::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    const auto crossover = liveCrossover_ ? crossover_ : startCrossover_;
    return vocoder_.process(input, side, sideVocoder_, output,
        [this, crossover](std::span<float> frame, std::span<const float> sideFrame) {
            // specdiff: max(0.0,...) is CDP's macro; CDP8's build fuses the
            // multiply and subtraction.
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            {
                if (!keepNegative_)
                {
                    if (!cross_)
                    {
                        const float diff = frame[vc] - sideFrame[vc];
                        frame[vc] = (float)((0.0 > diff) ? 0.0 : (double)diff);
                    }
                    else
                    {
                        const double diff = std::fma(-(double)sideFrame[vc], crossover, (double)frame[vc]);
                        frame[vc] = (float)((0.0 > diff) ? 0.0 : diff);
                    }
                }
                else if (!cross_)
                    frame[vc] = (float)(frame[vc] - sideFrame[vc]);
                else
                    frame[vc] = (float)std::fma(-(double)sideFrame[vc], crossover, (double)frame[vc]);
            }
        });
}

bool CombineMax::prepare(double sampleRate, int points, int overlap)
{
    PhaseVocoder vocoder;
    PhaseVocoder sideVocoder;
    if (!preparePair(vocoder, sideVocoder, sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    sideVocoder_ = std::move(sideVocoder);
    reset();
    return true;
}

void CombineMax::reset() noexcept
{
    vocoder_.reset();
    sideVocoder_.reset();
}

bool CombineMax::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    return vocoder_.process(
        input, side, sideVocoder_, output, [](std::span<float> frame, std::span<const float> sideFrame) {
            // do_specmax.
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            {
                if (sideFrame[vc] > frame[vc])
                {
                    frame[vc] = sideFrame[vc];
                    frame[vc + 1] = sideFrame[vc + 1];
                }
            }
        });
}

bool CombineMean::prepare(double sampleRate, int points, int overlap, Mode mode, double lowFrequency,
    double highFrequency, int channels, bool zeroOutside)
{
    if (mode < Mode::MeanAmplitudeMeanPitch || mode > Mode::MaximumAmplitudeMeanFrequency)
        return false;
    PhaseVocoder vocoder;
    PhaseVocoder sideVocoder;
    if (!preparePair(vocoder, sideVocoder, sampleRate, points, overlap))
        return false;
    // set_chunklens_and_establish_windowbufs and tklib1.c's ranges.
    const int wanted = static_cast<int>(vocoder.frameSize());
    const int clength = wanted / 2;
    const double nyquist = sampleRate / 2.0;
    const double chwidth = nyquist / (double)(clength - 1);
    const double halfchwidth = chwidth / 2.0;
    if (!std::isfinite(lowFrequency) || lowFrequency < PITCHZERO || lowFrequency > nyquist - halfchwidth
        || !std::isfinite(highFrequency) || highFrequency < PITCHZERO + chwidth || highFrequency > nyquist
        || channels < 2 || channels > clength)
    {
        return false;
    }
    // mean_preprocess: get_channel_corresponding_to_frq for each limit,
    // swapped when the high is the lower, as float indices, the top
    // inclusive. (The channel count is cut to the range and not used.)
    int bot = (int)((std::fabs(lowFrequency) + halfchwidth) / chwidth); /* TRUNCATE */
    int top = (int)((std::fabs(highFrequency) + halfchwidth) / chwidth); /* TRUNCATE */
    if (top < bot)
        std::swap(top, bot);
    top *= 2;
    bot *= 2;
    top += 2;
    top = std::min(top, wanted);
    vocoder_ = std::move(vocoder);
    sideVocoder_ = std::move(sideVocoder);
    mode_ = mode;
    zeroOutside_ = zeroOutside;
    clength_ = clength;
    chwidth_ = chwidth;
    halfchwidth_ = halfchwidth;
    bot_ = bot;
    top_ = top;
    side_.assign(static_cast<std::size_t>(wanted), 0.0F);
    out_.assign(static_cast<std::size_t>(wanted), 0.0F);
    loud1_.assign(static_cast<std::size_t>(clength), 0.0F);
    loud2_.assign(static_cast<std::size_t>(clength), 0.0F);
    loc1_.assign(static_cast<std::size_t>(clength), 0);
    loc2_.assign(static_cast<std::size_t>(clength), 0);
    reset();
    return true;
}

void CombineMean::reset() noexcept
{
    vocoder_.reset();
    sideVocoder_.reset();
    std::ranges::fill(out_, 0.0F);
}

bool CombineMean::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    return vocoder_.process(input, side, sideVocoder_, output,
        [this](std::span<float> frame, std::span<const float> sideFrame) { meanFrame(frame, sideFrame); });
}

// specmean.
void CombineMean::meanFrame(std::span<float> frame, std::span<const float> sideFrame) noexcept
{
    std::ranges::copy(sideFrame, side_.begin());
    float* flbufptr0 = frame.data();
    float* flbufptr1 = side_.data();
    float* windowbuf0 = out_.data();
    const int wanted = static_cast<int>(frame.size());
    int vc, cc;
    /* SUBZERO ALL LOUDEST-VALS TO BE USED IN THIS PASS */
    rectify_window(frame);
    rectify_window(side_);
    for (cc = 0; cc < clength_; cc++)
    {
        loud1_[static_cast<std::size_t>(cc)] = -1.0f;
        loud2_[static_cast<std::size_t>(cc)] = -1.0f;
    }
    if (zeroOutside_)
    { /* zero out-of-range channels */
        for (vc = 0; vc < bot_; vc += 2)
            flbufptr0[vc] = 0.0f;
        for (vc = top_; vc < wanted; vc += 2)
            flbufptr0[vc] = 0.0f;
        return;
    }
    for (vc = bot_; vc < top_; vc += 2)
    {
        putInList(loud1_.data(), loc1_.data(), flbufptr0[vc], vc);
        putInList(loud2_.data(), loc2_.data(), flbufptr1[vc], vc);
        windowbuf0[vc] = 0.0f; /* initialise each output to zero amp */
        windowbuf0[vc + 1] = flbufptr0[vc + 1];
    }
    generateMeanValues(flbufptr0, flbufptr1); /* SWITCH AMPLITUDES */
}

// if_loud_enough_to_go_in_list_put_in_list: CDP shuffles the quieter values
// down over the place, not up, so the value there and the one after it are
// lost and the last is doubled.
void CombineMean::putInList(float* ampstore, int* locstore, float val, int location) const noexcept
{
    int n = 0;
    if (val < 0.0) /* eliminate any subzero amplitudes */
        return;
    while (ampstore[n] >= val)
    { /* Move along until stored vals are less than input val */
        if (++n >= clength_)
            return;
    }
    if (n < clength_ - 1)
    { /* If ness, shuffle existing vals */
        std::memmove(ampstore + n, ampstore + n + 1, static_cast<std::size_t>(clength_ - n - 1) * sizeof(float));
        std::memmove(locstore + n, locstore + n + 1, static_cast<std::size_t>(clength_ - n - 1) * sizeof(int));
    }
    ampstore[n] = val;      /* Store new value */
    locstore[n] = location; /* store its location in original buffer */
}

// generate_mean_values: CDP writes the channel of a frequency above the
// Nyquist channel past windowbuf[0] (it bounds the channel by the frame's
// float count), where it is never read back; this skips it.
void CombineMean::generateMeanValues(float* frame, const float* sideFrame) noexcept
{
    float* windowbuf0 = out_.data();
    const float* windowbuf1 = loud1_.data();
    const float* windowbuf2 = loud2_.data();
    double newfrq = 0.0, newamp = 0.0;
    int j, n, a, b;
    n = 0;
    while (n < clength_ && windowbuf1[n] >= 0.0 && windowbuf2[n] >= 0.0)
    {
        a = loc1_[static_cast<std::size_t>(n)];
        b = loc2_[static_cast<std::size_t>(n)];
        switch (mode_)
        {
        case Mode::MeanAmplitudeMeanPitch:
            newamp = (frame[a] + sideFrame[b]) / 2.0;
            a++;
            b++;
            newfrq = pichwise_mean_of_frqs(std::fabs((double)frame[a]), std::fabs((double)sideFrame[b]));
            break;
        case Mode::MeanAmplitudeMeanFrequency:
            newamp = (frame[a] + sideFrame[b]) / 2.0;
            a++;
            b++;
            newfrq = (std::fabs((double)frame[a]) + std::fabs((double)sideFrame[b])) / 2.0;
            break;
        case Mode::InputAmplitudeMeanPitch:
            newamp = frame[a];
            a++;
            b++;
            newfrq = pichwise_mean_of_frqs(std::fabs((double)frame[a]), std::fabs((double)sideFrame[b]));
            break;
        case Mode::InputAmplitudeMeanFrequency:
            newamp = frame[a];
            a++;
            b++;
            newfrq = (std::fabs((double)frame[a]) + std::fabs((double)sideFrame[b])) / 2.0;
            break;
        case Mode::SideAmplitudeMeanPitch:
            newamp = sideFrame[b];
            a++;
            b++;
            newfrq = pichwise_mean_of_frqs(std::fabs((double)frame[a]), std::fabs((double)sideFrame[b]));
            break;
        case Mode::SideAmplitudeMeanFrequency:
            newamp = sideFrame[b];
            a++;
            b++;
            newfrq = (std::fabs((double)frame[a]) + std::fabs((double)sideFrame[b])) / 2.0;
            break;
        case Mode::MaximumAmplitudeMeanPitch:
            newamp = (frame[a] > sideFrame[b]) ? frame[a] : sideFrame[b]; /* max(...) */
            a++;
            b++;
            newfrq = pichwise_mean_of_frqs(std::fabs((double)frame[a]), std::fabs((double)sideFrame[b]));
            break;
        case Mode::MaximumAmplitudeMeanFrequency:
            newamp = (frame[a] > sideFrame[b]) ? frame[a] : sideFrame[b]; /* max(...) */
            a++;
            b++;
            newfrq = (std::fabs((double)frame[a]) + std::fabs((double)sideFrame[b])) / 2.0;
            break;
        }
        j = (int)((newfrq + halfchwidth_) / chwidth_); /* TRUNCATE */ /*FIND APPROP CHAN */
        if (j < clength_)
        {
            j *= 2;
            if (newamp > windowbuf0[j])
            {
                windowbuf0[j++] = (float)newamp;
                windowbuf0[j] = (float)newfrq;
            }
        }
        n++;
    }
    for (j = bot_; j < top_; j++)
        frame[j] = windowbuf0[j];
}

bool CombineCross::prepare(double sampleRate, int points, int overlap, double interpolation, bool liveInterpolation)
{
    // tklib1.c: the interpolation from 0 to 1.
    if (!isFraction(interpolation))
        return false;
    PhaseVocoder vocoder;
    PhaseVocoder sideVocoder;
    if (!preparePair(vocoder, sideVocoder, sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    sideVocoder_ = std::move(sideVocoder);
    liveInterpolation_ = liveInterpolation;
    interpolate_ = liveInterpolation || !flteq(interpolation, defaultInterpolation);
    startInterpolation_ = interpolation;
    reset();
    return true;
}

bool CombineCross::setInterpolation(double interpolation) noexcept
{
    if (!prepared() || !isFraction(interpolation))
        return false;
    interpolation_ = interpolation;
    return true;
}

void CombineCross::reset() noexcept
{
    vocoder_.reset();
    sideVocoder_.reset();
    interpolation_ = startInterpolation_;
}

bool CombineCross::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    const auto interpolation = liveInterpolation_ ? interpolation_ : startInterpolation_;
    return vocoder_.process(input, side, sideVocoder_, output,
        [this, interpolation](std::span<float> frame, std::span<const float> sideFrame) {
            // speccross: CDP8's build leaves these statements unfused.
            if (!interpolate_)
            {
                for (std::size_t vc = 0; vc < frame.size(); vc += 2)
                    frame[vc] = sideFrame[vc];
                return;
            }
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            {
                double valdiff = sideFrame[vc] - frame[vc];
                valdiff *= interpolation;
                frame[vc] = (float)(frame[vc] + valdiff);
            }
        });
}

bool FormantsVocode::prepare(double sampleRate, int points, int overlap, bool pitchwise, int bands,
    bool quickSearch, double lowFrequency, double highFrequency, double gain)
{
    PhaseVocoder vocoder;
    PhaseVocoder sideVocoder;
    if (!preparePair(vocoder, sideVocoder, sampleRate, points, overlap))
        return false;
    SpectralEnvelope envelope;
    if (!envelope.prepare(sampleRate, vocoder.frameSize(), pitchwise, bands, quickSearch))
        return false;
    // tklib1.c's ranges.
    const double nyquist = envelope.nyquist();
    if (!std::isfinite(lowFrequency) || lowFrequency < PITCHZERO || lowFrequency > nyquist
        || !std::isfinite(highFrequency) || highFrequency < PITCHZERO || highFrequency > nyquist
        || !std::isfinite(gain) || gain < FLTERR || gain > maximumGain)
    {
        return false;
    }
    vocoder_ = std::move(vocoder);
    sideVocoder_ = std::move(sideVocoder);
    sideEnvelope_ = envelope;
    envelope_ = std::move(envelope);
    // handle_pitch_zeros: setup_pitchzero for VOCO_LOF and VOCO_HIF.
    lowFrequency_ = lowFrequency < SPEC_MINFRQ ? 0.0 : lowFrequency;
    highFrequency_ = highFrequency < SPEC_MINFRQ ? 0.0 : highFrequency;
    gain_ = gain;
    side_.assign(vocoder_.frameSize(), 0.0F);
    reset();
    return true;
}

void FormantsVocode::reset() noexcept
{
    vocoder_.reset();
    sideVocoder_.reset();
    frames_ = 0;
}

bool FormantsVocode::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    return vocoder_.process(input, side, sideVocoder_, output,
        [this](std::span<float> frame, std::span<const float> sideFrame) {
            // skip_or_special_operation_on_window_zero.
            if (frames_++ == 0)
                return;
            vocodeFrame(frame, sideFrame);
        });
}

// specvocode, with vocode_channel, eliminate_out_of_range_data and
// vocogain.
void FormantsVocode::vocodeFrame(std::span<float> frame, std::span<const float> sideFrame) noexcept
{
    std::ranges::copy(sideFrame, side_.begin());
    float* flbufptr0 = frame.data();
    const float* flbufptr1 = side_.data();
    const std::size_t wanted = frame.size();
    double pre_amptotal, post_amptotal;
    rectify_window(frame);
    envelope_.extract(frame);
    rectify_window(side_);
    sideEnvelope_.extract(side_);
    pre_amptotal = get_totalamp(frame);
    for (std::size_t vc = 0; vc < wanted; vc += 2)
    {
        double thisvalue;
        const double thisspecamp1 = envelope_.amplitudeAt((double)flbufptr0[vc + 1]);
        const double thisspecamp2 = sideEnvelope_.amplitudeAt((double)flbufptr1[vc + 1]);
        if (thisspecamp1 <= 0.0)
            flbufptr0[vc] = 0.0f;
        else
        {
            if ((thisvalue = flbufptr0[vc] * thisspecamp2 / thisspecamp1) < VERY_TINY_VAL)
                flbufptr0[vc] = 0.0f;
            else
                flbufptr0[vc] = (float)thisvalue;
        }
    }
    for (std::size_t vc = 0; vc < wanted; vc += 2)
    {
        if (flbufptr0[vc + 1] < lowFrequency_ || flbufptr0[vc + 1] > highFrequency_)
            flbufptr0[vc] = 0.0f;
    }
    post_amptotal = get_totalamp(frame);
    normalise(frame, pre_amptotal, post_amptotal);
    if (!flteq(gain_, 1.0))
    {
        for (std::size_t vc = 0; vc < wanted; vc += 2)
            flbufptr0[vc] = (float)(flbufptr0[vc] * gain_);
    }
}
}
