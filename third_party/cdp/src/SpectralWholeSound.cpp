/*
 * Derived from the CDP System, Release 8: dev/spec/simple.c and
 * ap_simple.c, dev/strange/strange.c and ap_strange.c, and dev/cdp2k/
 * parstruct.c, tklib1.c and tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralWholeSound.h"

#include "talas/cdp/PitchTrack.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h and speccon.h, and dev/spec/simple.c.
constexpr double MINAMP = 0.000000003;
constexpr double BIGAMP = 10000.0;
constexpr double SPEC_MINFRQ = 10.0;
constexpr double PITCHZERO = SPEC_MINFRQ / 2.0;
constexpr double VERYBIG = 10000.0;
// The longest skip time a stream takes: an hour.
constexpr double maximumSkipTime = 3600.0;

// readfiles.c's frametime, from the .ana header's arate.
[[nodiscard]] float frametimeFor(double sampleRate, int decimation) noexcept
{
    const auto arate = (float)((float)sampleRate / (float)decimation);
    return (float)(1.0 / arate);
}
}

bool SpecClean::prepare(double sampleRate, int points, int overlap, Mode mode, std::span<const double> noise,
    std::span<const double> goodSignal, double value, double noiseGain)
{
    if (mode < Mode::FromTime || mode > Mode::Comparing || !std::isfinite(noiseGain) || noiseGain < 1.0
        || noiseGain > maximumNoiseGain || noise.empty() || (mode == Mode::Comparing && goodSignal.empty()))
    {
        return false;
    }
    SpecClean clean;
    if (!clean.vocoder_.prepare(sampleRate, points, overlap))
        return false;
    const auto clength = static_cast<int>(clean.vocoder_.frameSize() / 2);
    const double nyquist = (double)(int)sampleRate / 2.0;
    const double chwidth = nyquist / (double)(clength - 1);
    const double halfchwidth = chwidth / 2.0;
    const float frametime = frametimeFor(sampleRate, clean.vocoder_.decimation());
    clean.mode_ = mode;
    clean.clength_ = clength;
    // tklib1.c's ranges.
    if (mode == Mode::FromTime || mode == Mode::Anywhere)
    {
        if (!std::isfinite(value) || value < 0.0 || value > maximumSkipTime)
            return false;
        // setup_internal_params_for_clean.
        auto skip = std::lround(value / frametime);
        if (skip < minimumSkipWindows)
            skip = minimumSkipWindows;
        clean.skipWindows_ = static_cast<std::size_t>(skip);
    }
    else if (mode == Mode::Filtering && (!std::isfinite(value) || value < PITCHZERO || value > nyquist - halfchwidth))
    {
        return false;
    }
    std::vector<float> amp(static_cast<std::size_t>(clength), 0.0F);
    // read_noise_data over the whole noise file.
    if (!analyseWholeSound(noise, sampleRate, points, overlap, [&](std::span<const float> frame) {
            for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
            {
                if (frame[static_cast<std::size_t>(vc)] > amp[static_cast<std::size_t>(cc)])
                    amp[static_cast<std::size_t>(cc)] = frame[static_cast<std::size_t>(vc)];
            }
        }))
    {
        return false;
    }
    // adjust_noise_level.
    for (int cc = 0; cc < clength; cc++)
        amp[static_cast<std::size_t>(cc)] = (float)(amp[static_cast<std::size_t>(cc)] * noiseGain);
    if (mode == Mode::Filtering)
    {
        // setup_cleaning_filter.
        double chtop = chwidth / 2.0;
        if (chtop < value)
            amp[0] = 0.0F;
        for (int cc = 1; cc < clength; cc++)
        {
            chtop += chwidth;
            if (chtop < value)
                amp[static_cast<std::size_t>(cc)] = 0.0F;
        }
    }
    else if (mode == Mode::Comparing)
    {
        // read_comparison_data over the whole good-signal file, then
        // reorganise_marks.
        std::vector<int> mark(static_cast<std::size_t>(clength), 1);
        if (!analyseWholeSound(goodSignal, sampleRate, points, overlap, [&](std::span<const float> frame) {
                for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
                {
                    if (frame[static_cast<std::size_t>(vc)] > amp[static_cast<std::size_t>(cc)])
                        mark[static_cast<std::size_t>(cc)] = 0;
                }
            }))
        {
            return false;
        }
        for (int n = 0; n < clength; n++)
        {
            if (mark[static_cast<std::size_t>(n)] != 0)
                clean.marks_.push_back(n * 2);
        }
        if (clean.marks_.empty())
            return false;
    }
    clean.levels_ = amp;
    clean.amp_ = std::move(amp);
    *this = std::move(clean);
    reset();
    return true;
}

void SpecClean::reset() noexcept
{
    vocoder_.reset();
    std::ranges::copy(levels_, amp_.begin());
    windows_ = 0;
}

bool SpecClean::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) { cleanFrame(frame); });
}

// do_cleaning for one window, with skip_skipwindows.
void SpecClean::cleanFrame(std::span<float> frame) noexcept
{
    const auto window = windows_++;
    if (mode_ == Mode::Comparing)
    {
        for (const auto mark : marks_)
            frame[static_cast<std::size_t>(mark)] = 0.0F;
        return;
    }
    if ((mode_ == Mode::FromTime || mode_ == Mode::Anywhere) && window < skipWindows_)
        return;
    float* amp = amp_.data();
    for (int cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
    {
        if (frame[static_cast<std::size_t>(vc)] < amp[cc])
        {
            frame[static_cast<std::size_t>(vc)] = 0.0F;
            if (mode_ == Mode::FromTime)
                amp[cc] = (float)VERYBIG;
        }
    }
}

bool StrangeInvert::prepare(double sampleRate, int points, int overlap, Mode mode, std::span<const double> source)
{
    if (mode < Mode::Normal || mode > Mode::KeepAmplitude || source.empty())
        return false;
    StrangeInvert invert;
    if (!invert.vocoder_.prepare(sampleRate, points, overlap))
        return false;
    const auto clength = static_cast<int>(invert.vocoder_.frameSize() / 2);
    const auto wanted = clength * 2;
    invert.mode_ = mode;
    invert.clength_ = clength;
    // setup_internal_arrays_for_invert.
    std::vector<float> amp(static_cast<std::size_t>(clength), (float)(-BIGAMP));
    std::vector<double> ratio(static_cast<std::size_t>(clength), -BIGAMP);
    // extract_normalising_criteria_by_reading_infile.
    if (!analyseWholeSound(source, sampleRate, points, overlap, [&](std::span<const float> frame) {
            if (mode == Mode::Normal)
            {
                for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
                    amp[static_cast<std::size_t>(cc)]
                        = std::max(amp[static_cast<std::size_t>(cc)], frame[static_cast<std::size_t>(vc)]);
                return;
            }
            // get_totalamp.
            double preTotalamp = 0.0;
            for (int vc = 0; vc < wanted; vc += 2)
                preTotalamp += frame[static_cast<std::size_t>(vc)];
            if (preTotalamp > MINAMP)
            {
                for (int cc = 0, vc = 0; cc < clength; cc++, vc += 2)
                    ratio[static_cast<std::size_t>(cc)] = std::max(
                        ratio[static_cast<std::size_t>(cc)], frame[static_cast<std::size_t>(vc)] / preTotalamp);
            }
        }))
    {
        return false;
    }
    invert.loudest_ = std::move(amp);
    invert.ratios_ = std::move(ratio);
    *this = std::move(invert);
    reset();
    return true;
}

void StrangeInvert::reset() noexcept
{
    vocoder_.reset();
    windows_ = 0;
}

bool StrangeInvert::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        // skip_or_special_operation_on_window_zero.
        if (windows_++ == 0)
            return;
        invertFrame(frame);
    });
}

// specinvert.
void StrangeInvert::invertFrame(std::span<float> frame) const noexcept
{
    if (mode_ == Mode::Normal)
    {
        for (int cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
            frame[static_cast<std::size_t>(vc)]
                = loudest_[static_cast<std::size_t>(cc)] - frame[static_cast<std::size_t>(vc)];
        return;
    }
    double preTotalamp = 0.0;
    for (int vc = 0; vc < clength_ * 2; vc += 2)
        preTotalamp += frame[static_cast<std::size_t>(vc)];
    if (preTotalamp > MINAMP)
    {
        for (int cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
        {
            const double zort
                = ratios_[static_cast<std::size_t>(cc)] - (frame[static_cast<std::size_t>(vc)] / preTotalamp);
            frame[static_cast<std::size_t>(vc)] = (float)(zort * preTotalamp);
        }
    }
}
}
