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
#pragma once

#include "talas/cdp/PhaseVocoder.h"

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP's spectral processes that read a whole analysis file before the one
// they process, between `pvoc anal` and `pvoc synth`. The file read first
// is the analysis of a sample, made whole when the process is prepared (as
// getPitch makes a pitch file): CDP's result when that sample is the one
// CDP was given (spec clean's noise and good-signal files, analyses of
// samples of noise and of clean signal; strange invert's own input, so the
// Input should play that sample from its start). None of their fields is a
// breakpoint file in CDP8.

// `spec clean 1-4 infile nfile [gfile] outfile`: removes noise. Each
// channel's level is the loudest the noise sample's analysis gives it,
// times the noise gain (-g). Mode 1 (FromTime) silences a channel, after
// the skip time, from the window its amplitude first falls below its
// level; mode 2 (Anywhere) wherever it falls below it after the skip time;
// mode 3 (Filtering) wherever it does, but only in the channels whose top
// reaches the frequency; mode 4 (Comparing) silences, throughout, the
// channels the good-signal sample's analysis never takes above their level
// (CDP refuses a good signal that takes every channel above it).
class SpecClean
{
public:
    enum class Mode
    {
        FromTime = 1,
        Anywhere,
        Filtering,
        Comparing,
    };

    // CDP's DEFAULT_NOISEGAIN and CL_MAX_GAIN; the least gain is 1.
    static constexpr double defaultNoiseGain = 2.0;
    static constexpr double maximumNoiseGain = 40.0;
    // CDP's SKIPWDWS: windows always skipped in modes 1 and 2.
    static constexpr int minimumSkipWindows = 4;

    // value is mode 1's and 2's skip time in seconds (CDP's limit, the
    // input's last window, is not known to a stream: from 0 to an hour) or
    // mode 3's frequency in Hz (CDP's PITCHZERO, 5 Hz, to half the sample
    // rate less half a channel); mode 4 takes none. goodSignal is mode 4's
    // sample, and is not read in the others. False, with no change, for a
    // mode outside 1 to 4, a value or gain outside its range, an empty
    // noise sample (or good signal in mode 4), a good signal that leaves
    // no channel to silence or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode,
        std::span<const double> noise, std::span<const double> goodSignal, double value, double noiseGain);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void cleanFrame(std::span<float> frame) noexcept;

    PhaseVocoder vocoder_;
    Mode mode_ = Mode::FromTime;
    int clength_ = 0;
    // CL_SKIPW, and the windows seen of them.
    std::size_t skipWindows_ = 0;
    std::size_t windows_ = 0;
    // The levels as prepared (dz->amp after adjust_noise_level and
    // setup_cleaning_filter), and as mode 1 raises them.
    std::vector<float> levels_;
    std::vector<float> amp_;
    // Mode 4's marked amplitudes, as reorganise_marks leaves them (float
    // offsets into the frame).
    std::vector<int> marks_;
};

// `strange invert 1-2 infile outfile`: each channel's amplitude becomes its
// loudest over the whole file less itself (mode 1), or, keeping each
// frame's total amplitude, its loudest share of a frame's total less its
// own share (mode 2, frames whose total is under CDP's MINAMP left as they
// are). CDP leaves the first window as it is. The loudest amplitudes and
// shares come from the source sample's analysis, as CDP reads its whole
// input first: CDP's result when the Input plays that sample from its
// start.
class StrangeInvert
{
public:
    enum class Mode
    {
        Normal = 1,
        KeepAmplitude,
    };

    // False, with no change, for a mode outside 1 and 2, an empty source
    // or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(
        double sampleRate, int points, int overlap, Mode mode, std::span<const double> source);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void invertFrame(std::span<float> frame) const noexcept;

    PhaseVocoder vocoder_;
    Mode mode_ = Mode::Normal;
    int clength_ = 0;
    // dz->amp (mode 1) and INV_AMPRATIO (mode 2) after
    // extract_normalising_criteria_by_reading_infile.
    std::vector<float> loudest_;
    std::vector<double> ratios_;
    std::size_t windows_ = 0;
};
}
