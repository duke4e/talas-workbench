/*
 * Derived from the CDP System, Release 8: dev/morph/morph.c and ap_morph.c,
 * and dev/cdp2k/readdata.c and tklib1.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralMorph.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h and speccon.h.
constexpr double FLTERR = 0.000002;
constexpr double PI = 3.141592654;
constexpr int MPH_COSTABSIZE = 512;

// flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const double upperbnd = f2 + FLTERR;
    const double lowerbnd = f2 - FLTERR;
    return !((f1 > upperbnd) || (f1 < lowerbnd));
}

// read_param_as_value_or_brkfile_and_check_range: a value within FLTERR of
// a range's end is that end.
[[nodiscard]] double snapToRange(double value, double lo, double hi) noexcept
{
    if (flteq(value, lo))
        value = lo;
    if (flteq(value, hi))
        value = hi;
    return value;
}

[[nodiscard]] double snapToLow(double value, double lo) noexcept
{
    return flteq(value, lo) ? lo : value;
}

[[nodiscard]] bool within(double value, double lo, double hi) noexcept
{
    return std::isfinite(value) && value >= lo && value <= hi;
}

// The frame time CDP reads from the .ana header (its arate).
[[nodiscard]] float frameTime(double sampleRate, const PhaseVocoder& vocoder) noexcept
{
    const auto arate = (float)((float)sampleRate / (float)vocoder.decimation());
    return (float)(1.0 / arate);
}

// CDP's round(time/frametime): morph.c defines round as lround.
[[nodiscard]] long long windows(double time, float frametime) noexcept
{
    return std::lround(time / frametime);
}

// Both phase vocoders, prepared alike.
[[nodiscard]] bool preparePair(
    PhaseVocoder& vocoder, PhaseVocoder& sideVocoder, double sampleRate, int points, int overlap)
{
    return vocoder.prepare(sampleRate, points, overlap) && sideVocoder.prepare(sampleRate, points, overlap);
}

// The side's frame from delay frames back, held in entry (zeros before the
// side has given that many frames); the current side frame when there is no
// delay. Call storeEntry once the frame is done with it.
[[nodiscard]] std::span<const float> enteredSide(
    std::vector<float>& entry, std::span<const float> sideFrame, long long delay, long long window) noexcept
{
    if (delay == 0)
        return sideFrame;
    return {entry.data() + (std::size_t)(window % delay) * sideFrame.size(), sideFrame.size()};
}

void storeEntry(std::vector<float>& entry, std::span<const float> sideFrame, long long delay, long long window) noexcept
{
    if (delay == 0)
        return;
    const auto slot = (std::size_t)(window % delay) * sideFrame.size();
    std::ranges::copy(sideFrame, entry.begin() + (std::ptrdiff_t)slot);
}

// time_warp.
[[nodiscard]] double time_warp(double interpratio, double exponent) noexcept
{
    const double doubled = interpratio * 2.0;
    interpratio = (doubled > 1.0) ? 1.0 : doubled;
    interpratio = std::pow(interpratio, exponent);
    interpratio /= 2.0;
    return interpratio;
}
}

bool MorphMorph::prepare(double sampleRate, int points, int overlap, Mode mode, double amplitudeStart,
    double amplitudeEnd, double frequencyStart, double frequencyEnd, double amplitudeExponent,
    double frequencyExponent, double stagger)
{
    if (mode != Mode::Linear && mode != Mode::Cosine)
        return false;
    PhaseVocoder vocoder;
    PhaseVocoder sideVocoder;
    if (!preparePair(vocoder, sideVocoder, sampleRate, points, overlap))
        return false;
    const float frametime = frameTime(sampleRate, vocoder);
    // tklib1.c's ranges, read as readdata.c reads them.
    const double endLow = frametime * 2;
    amplitudeStart = snapToLow(amplitudeStart, 0.0);
    frequencyStart = snapToLow(frequencyStart, 0.0);
    amplitudeEnd = snapToLow(amplitudeEnd, endLow);
    frequencyEnd = snapToLow(frequencyEnd, endLow);
    amplitudeExponent = snapToRange(amplitudeExponent, minimumExponent, maximumExponent);
    frequencyExponent = snapToRange(frequencyExponent, minimumExponent, maximumExponent);
    stagger = snapToLow(stagger, 0.0);
    if (!within(amplitudeStart, 0.0, maximumTime) || !within(frequencyStart, 0.0, maximumTime)
        || !within(amplitudeEnd, endLow, maximumTime) || !within(frequencyEnd, endLow, maximumTime)
        || !within(amplitudeExponent, minimumExponent, maximumExponent)
        || !within(frequencyExponent, minimumExponent, maximumExponent) || !within(stagger, 0.0, maximumStagger))
    {
        return false;
    }
    // check_consistency_of_morph_params (but for the files' lengths).
    if (amplitudeStart < stagger || frequencyStart < stagger || amplitudeEnd <= amplitudeStart
        || frequencyEnd <= frequencyStart)
    {
        return false;
    }
    vocoder_ = std::move(vocoder);
    sideVocoder_ = std::move(sideVocoder);
    mode_ = mode;
    amplitudeExponent_ = amplitudeExponent;
    frequencyExponent_ = frequencyExponent;
    // setup_internal_morph_params.
    amplitudeEnd_ = windows(amplitudeEnd, frametime);
    amplitudeStart_ = windows(amplitudeStart, frametime);
    frequencyEnd_ = windows(frequencyEnd, frametime);
    frequencyStart_ = windows(frequencyStart, frametime);
    stagger_ = windows(stagger, frametime);
    // make_morph_costable.
    costable_.assign(MPH_COSTABSIZE + 2, 1.0);
    const double d = PI / (double)MPH_COSTABSIZE;
    for (int n = 0; n < MPH_COSTABSIZE; n++)
    {
        double d2 = std::cos((double)n * d);
        d2 += 1.0;
        d2 /= 2.0;
        d2 = 1.0 - d2;
        d2 = (d2 > 1.0) ? 1.0 : d2;
        d2 = (d2 > 0.0) ? d2 : 0.0;
        costable_[(std::size_t)n] = d2;
    }
    // costable_[MPH_COSTABSIZE] is CDP's wrap-around point, 1. At an
    // interpratio of exactly 1 cosin_lookup reads one past CDP's table, and
    // takes none of it; the extra 1 stands for it.
    entry_.assign((std::size_t)stagger_ * vocoder_.frameSize(), 0.0F);
    reset();
    return true;
}

void MorphMorph::reset() noexcept
{
    vocoder_.reset();
    sideVocoder_.reset();
    std::ranges::fill(entry_, 0.0F);
    frames_ = 0;
}

bool MorphMorph::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    return vocoder_.process(input, side, sideVocoder_, output,
        [this](std::span<float> frame, std::span<const float> sideFrame) {
            const long long window = frames_++;
            morphFrame(frame, enteredSide(entry_, sideFrame, stagger_, window), window);
            storeEntry(entry_, sideFrame, stagger_, window);
        });
}

// warped_cosin with cosin_lookup, or MPH_LINE's pow.
double MorphMorph::curve(double interpratio, double exponent) const noexcept
{
    if (mode_ == Mode::Linear)
        return std::pow(interpratio, exponent);
    if (interpratio < .5)
        interpratio = time_warp(interpratio, exponent);
    else if (interpratio > .5)
    {
        interpratio = 1.0 - interpratio;
        interpratio = time_warp(interpratio, exponent);
        interpratio = 1.0 - interpratio;
    }
    // cosin_lookup.
    const double dtabindex = interpratio * (double)MPH_COSTABSIZE;
    int tabindex = (int)std::floor(dtabindex);
    const double frac = dtabindex - (double)tabindex;
    const double thisval = costable_[(std::size_t)tabindex++];
    const double nextval = costable_[(std::size_t)tabindex];
    const double diff = nextval - thisval;
    const double step = diff * frac;
    return thisval + step;
}

// specmorph, at its output window: the first file's window as it is before
// the morph starts, do_specmorph's mix until it ends, then the second file's
// window (staggered).
void MorphMorph::morphFrame(std::span<float> frame, std::span<const float> side, long long window) noexcept
{
    const long long start_of_morph = std::min(amplitudeStart_, frequencyStart_);
    const long long end_of_morph = std::max(amplitudeEnd_, frequencyEnd_);
    if (window < start_of_morph)
        return;
    if (window >= end_of_morph)
    {
        std::ranges::copy(side, frame.begin());
        return;
    }
    // do_specmorph: diff is a float difference; the multiply and add are
    // separate statements, which CDP8's build leaves unfused.
    const std::size_t wanted = frame.size();
    if (window > amplitudeStart_)
    {
        if (window < amplitudeEnd_)
        {
            const double alen = (double)(amplitudeEnd_ - amplitudeStart_);
            double amp_interpratio = (double)(window - amplitudeStart_) / (double)alen;
            amp_interpratio = curve(amp_interpratio, amplitudeExponent_);
            for (std::size_t vc = 0; vc < wanted; vc += 2)
            {
                const double diff = side[vc] - frame[vc];
                const double incr = diff * amp_interpratio;
                frame[vc] = (float)(frame[vc] + incr);
            }
        }
        else
        {
            for (std::size_t vc = 0; vc < wanted; vc += 2)
                frame[vc] = side[vc];
        }
    }
    if (window > frequencyStart_)
    {
        if (window < frequencyEnd_)
        {
            const double flen = (double)(frequencyEnd_ - frequencyStart_);
            double frq_interpratio = (double)(window - frequencyStart_) / flen;
            frq_interpratio = curve(frq_interpratio, frequencyExponent_);
            for (std::size_t vc = 0; vc < wanted; vc += 2)
            {
                const double diff = side[vc + 1] - frame[vc + 1];
                const double incr = diff * frq_interpratio;
                frame[vc + 1] = (float)(frame[vc + 1] + incr);
            }
        }
        else
        {
            for (std::size_t vc = 0; vc < wanted; vc += 2)
                frame[vc + 1] = side[vc + 1];
        }
    }
}

bool MorphBridge::prepare(double sampleRate, int points, int overlap, Mode mode, double offset,
    double startSideFrequency, double startSideAmplitude, double endSideFrequency, double endSideAmplitude,
    double start, double end)
{
    if (mode < Mode::Direct || mode > Mode::SideToInput)
        return false;
    PhaseVocoder vocoder;
    PhaseVocoder sideVocoder;
    if (!preparePair(vocoder, sideVocoder, sampleRate, points, overlap))
        return false;
    const float frametime = frameTime(sampleRate, vocoder);
    // tklib1.c's ranges and defaults, read as readdata.c reads them.
    const double endLow = frametime * 2.0;
    offset = snapToLow(offset, 0.0);
    startSideFrequency = snapToRange(startSideFrequency, 0.0, 1.0);
    startSideAmplitude = snapToRange(startSideAmplitude, 0.0, 1.0);
    endSideFrequency = snapToRange(endSideFrequency, 0.0, 1.0);
    endSideAmplitude = snapToRange(endSideAmplitude, 0.0, 1.0);
    start = snapToLow(start, 0.0);
    end = snapToLow(end, endLow);
    if (!within(offset, 0.0, maximumOffset) || !within(startSideFrequency, 0.0, 1.0)
        || !within(startSideAmplitude, 0.0, 1.0) || !within(endSideFrequency, 0.0, 1.0)
        || !within(endSideAmplitude, 0.0, 1.0) || !within(start, 0.0, maximumTime)
        || !within(end, endLow, maximumTime))
    {
        return false;
    }
    // get_variants_and_flags sets IS_BRG_START and IS_BRG_END only for a
    // value other than the default (0 and two frames). Without IS_BRG_END
    // check_compatibility_of_bridge_params ends at the shorter file's end,
    // which a stream cannot know.
    if (flteq(end, endLow))
        return false;
    // check_compatibility_of_bridge_params (but for the files' lengths).
    if (flteq(start, 0.0))
        start = offset;
    if (end - start < frametime || offset > start)
        return false;
    // determine_process_startwindow: CDP's output starts where the side
    // enters, not with the input.
    const long long offsetWindows = windows(offset, frametime);
    const bool startsAtEntry = mode == Mode::Minimum || mode == Mode::FollowSide || mode == Mode::SideToInput
        || startSideFrequency > 0.0 || startSideAmplitude > 0.0;
    if (startsAtEntry && offsetWindows != 0)
        return false;
    // setup_internal_params_for_bridge and
    // establish_interplen_and_if_head_or_tail_to_the_interp. CDP keeps a tail
    // (3, or 2 or 1 for an end in the side or the input as they are) where
    // the interpolation ends before the shorter file; the stream reaches
    // frames after it only where its files are longer.
    const long long endWindow = windows(end, frametime);
    long long startWindow = 0;
    int startIs = 0;
    int tailIs = 3;
    if (flteq(endSideAmplitude, 1.0) && flteq(endSideFrequency, 1.0))
        tailIs = 2;
    if (flteq(endSideAmplitude, 0.0) && flteq(endSideFrequency, 0.0))
        tailIs = 1;
    if (start > 0.0)
    {
        startIs = 3;
        startWindow = windows(start, frametime);
        if (flteq(startSideAmplitude, 0.0) && flteq(startSideFrequency, 0.0))
            startIs = 1;
        if (flteq(startSideAmplitude, 1.0) && flteq(startSideFrequency, 1.0))
            startIs = 2;
    }
    const long long interplen = endWindow - startWindow;
    if (interplen < 0)
        return false;
    vocoder_ = std::move(vocoder);
    sideVocoder_ = std::move(sideVocoder);
    mode_ = mode;
    startSideFrequency_ = startSideFrequency;
    startSideAmplitude_ = startSideAmplitude;
    frequencyStep_ = (endSideFrequency - startSideFrequency) / (double)interplen;
    amplitudeStep_ = (endSideAmplitude - startSideAmplitude) / (double)interplen;
    levelStep_ = 2.0 * (1.0 / (double)interplen);
    startWindow_ = startWindow;
    endWindow_ = endWindow;
    startIs_ = startIs;
    tailIs_ = tailIs;
    offset_ = offsetWindows;
    entry_.assign((std::size_t)offset_ * vocoder_.frameSize(), 0.0F);
    reset();
    return true;
}

void MorphBridge::reset() noexcept
{
    vocoder_.reset();
    sideVocoder_.reset();
    sideFrequency_ = startSideFrequency_;
    sideAmplitude_ = startSideAmplitude_;
    levelFactor_ = 0.0;
    std::ranges::fill(entry_, 0.0F);
    frames_ = 0;
}

bool MorphBridge::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    return vocoder_.process(input, side, sideVocoder_, output,
        [this](std::span<float> frame, std::span<const float> sideFrame) {
            const long long window = frames_++;
            bridgeFrame(frame, enteredSide(entry_, sideFrame, offset_, window), window);
            storeEntry(entry_, sideFrame, offset_, window);
        });
}

// do_specbridge.
void MorphBridge::bridgeFrame(std::span<float> frame, std::span<const float> side, long long window) noexcept
{
    if (window < startWindow_)
    {
        switch (startIs_)
        {
        case 1:
            break;
        case 2:
            std::ranges::copy(side, frame.begin());
            break;
        default:
            interpolate(Where::Before, frame, side);
            break;
        }
    }
    else if (window < endWindow_)
    {
        sideFrequency_ += frequencyStep_;
        sideAmplitude_ += amplitudeStep_;
        interpolate(Where::In, frame, side);
    }
    else
    {
        switch (tailIs_)
        {
        case 1:
            break;
        case 2:
            std::ranges::copy(side, frame.begin());
            break;
        default:
            interpolate(Where::After, frame, side);
            break;
        }
    }
}

// do_interpolation with set_normalisation_type. pvoc's amplitudes are never
// negative, so CDP's "Amplitude anomaly" cannot arise. CDP8's build fuses
// each mix's second product with the sum.
void MorphBridge::interpolate(Where where, std::span<float> frame, std::span<const float> side) noexcept
{
    enum
    {
        NO_NORMALISE,
        NORMALISE_FILE1,
        NORMALISE_FILE2,
    } normalisation_type = NO_NORMALISE;
    double normaliser = 0.0;
    const std::size_t wanted = frame.size();
    if (mode_ != Mode::Direct)
    {
        double sum1 = 0.0;
        double sum2 = 0.0;
        for (std::size_t vc = 0; vc < wanted; vc += 2)
        {
            sum1 += frame[vc];
            sum2 += side[vc];
        }
        // The input's level over the side's (NORMALISE_FILE2) and the side's
        // over the input's (NORMALISE_FILE1).
        const auto toInput = [&] {
            normalisation_type = NORMALISE_FILE2;
            normaliser = flteq(sum2, 0.0) ? 0.0 : sum1 / sum2;
        };
        const auto toSide = [&] {
            normalisation_type = NORMALISE_FILE1;
            normaliser = flteq(sum1, 0.0) ? 0.0 : sum2 / sum1;
        };
        switch (mode_)
        {
        case Mode::Minimum:
            if (sum1 > sum2)
                toSide();
            else
                toInput();
            break;
        case Mode::FollowInput:
            toInput();
            break;
        case Mode::FollowSide:
            toSide();
            break;
        case Mode::InputToSide:
            if (where == Where::Before)
                toInput();
            else if (where == Where::After)
                toSide();
            else
            {
                if (flteq(sum2, 0.0))
                    normaliser = 0.0;
                else if (flteq(sum1, 0.0))
                    normaliser = 0.0;
                else
                {
                    const double z1 = sum1 / sum2;
                    const double z2 = sum2 / sum1;
                    normaliser = z1 * std::pow(z2, levelFactor_);
                }
                normalisation_type = levelFactor_ <= 1.0 ? NORMALISE_FILE2 : NORMALISE_FILE1;
                levelFactor_ += levelStep_;
            }
            break;
        case Mode::SideToInput:
            if (where == Where::Before)
                toSide();
            else if (where == Where::After)
                toInput();
            else
            {
                if (flteq(sum1, 0.0))
                    normaliser = 0.0;
                else if (flteq(sum2, 0.0))
                    normaliser = 0.0;
                else
                {
                    const double z1 = sum2 / sum1;
                    const double z2 = sum1 / sum2;
                    normaliser = z1 * std::pow(z2, levelFactor_);
                }
                normalisation_type = levelFactor_ <= 1.0 ? NORMALISE_FILE1 : NORMALISE_FILE2;
                levelFactor_ += levelStep_;
            }
            break;
        case Mode::Direct:
            break;
        }
    }
    const double sa2 = sideAmplitude_;
    const double sf2 = sideFrequency_;
    switch (normalisation_type)
    {
    case NO_NORMALISE:
        for (std::size_t n = 0; n < wanted; n++)
            frame[n] = (float)std::fma((double)side[n], sa2, (1.0 - sa2) * (double)frame[n]);
        break;
    case NORMALISE_FILE1:
        for (std::size_t n = 0; n < wanted; n += 2)
        {
            frame[n] = (float)std::fma((double)side[n], sa2, normaliser * ((1.0 - sa2) * (double)frame[n]));
            frame[n + 1] = (float)std::fma((double)side[n + 1], sf2, (1.0 - sf2) * (double)frame[n + 1]);
        }
        break;
    case NORMALISE_FILE2:
        for (std::size_t n = 0; n < wanted; n += 2)
        {
            frame[n] = (float)std::fma(sa2 * (double)side[n], normaliser, (1.0 - sa2) * (double)frame[n]);
            frame[n + 1] = (float)std::fma((double)side[n + 1], sf2, (1.0 - sf2) * (double)frame[n + 1]);
        }
        break;
    }
}
}
