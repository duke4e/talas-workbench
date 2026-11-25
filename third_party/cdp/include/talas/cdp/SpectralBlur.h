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
#pragma once

#include "talas/cdp/PhaseVocoder.h"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace talas::cdp
{
// CDP `blur avrg`, `blur suppress`, `hilite trace`, `hilite filter` and
// `hilite pluck` on each frame between `pvoc anal` and `pvoc synth`. CDP takes their fields as breakpoint files
// and reads them, counts rounded, at each analysis frame's time: a value
// given before a process call reaches the frames analysed during it.

// `blur avrg`: each channel's amplitude becomes the mean over the channels
// within N / 2 of it, fewer at the spectrum's ends. CDP leaves the first
// frame as it is.
class BlurAverage
{
public:
    // False, with no change, for N that does not round to 2 to the channel
    // count or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double n);
    // False, with no change, before prepare or for N outside that range.
    [[nodiscard]] bool setN(double n) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    int n_ = 2;
    int startN_ = 2;
    std::vector<float> amp_;
    std::size_t frames_ = 0;
};

// `blur suppress`: the N loudest channels fall silent.
class BlurSuppress
{
public:
    // False, with no change, for N that does not round to 1 to one less
    // than the channel count (CDP's ring of none keeps its last size) or
    // what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double n);
    [[nodiscard]] bool setN(double n) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    int n_ = 1;
    int startN_ = 1;
    std::vector<std::pair<int, float>> ring_;
};

// `hilite trace`: only the N loudest channels sound; in its modes 2 to 4
// only those within a band from the low or to the high frequency, or both,
// with -r keeping the loudest outside it while the band holds fewer than N.
class HiliteTrace
{
public:
    enum class Mode
    {
        All = 1,
        Above,
        Below,
        Between,
    };

    // CDP's PITCHZERO.
    static constexpr double minimumFrequency = 5.0;

    // A live frequency is CDP's breakpoint value, raised to 10 Hz; a static
    // one under 10 Hz is zero, as setup_pitchzero makes them; a mode leaves
    // its unused frequency at CDP's default (5 Hz, which is zero, or half the
    // sample rate). False, with no change, for N that does not round to 1 to
    // the channel count (one less outside mode 1, as CDP's ring of none keeps
    // its last size), frequencies outside 5 Hz to half the sample rate or
    // what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, double n,
        double lowFrequency, double highFrequency, bool retain, bool liveLow, bool liveHigh);
    [[nodiscard]] bool setControls(double n, double lowFrequency, double highFrequency) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    [[nodiscard]] bool validN(double n) const noexcept;
    void trace(std::span<float> frame) noexcept;

    PhaseVocoder vocoder_;
    Mode mode_ = Mode::All;
    double nyquist_ = 0.0;
    int n_ = 1;
    double low_ = 0.0;
    double high_ = 0.0;
    int startN_ = 1;
    double startLow_ = 0.0;
    double startHigh_ = 0.0;
    bool retain_ = false;
    bool liveLow_ = false;
    bool liveHigh_ = false;
    std::vector<std::pair<int, float>> ring_;
};

// `hilite filter`: a high-pass, low-pass, band-pass or notch filter on
// each frame's channels, by their frequencies, with skirts Q Hz wide,
// plain, normalised to the frame's first total amplitude, or with a gain.
// CDP interpolates its breakpoint frequencies and Q as logarithms, so a
// live one is read as the exponential of its logarithm.
class HiliteFilter
{
public:
    enum class Mode
    {
        HighPass = 1,
        HighPassNormalised,
        LowPass,
        LowPassNormalised,
        HighPassGain,
        LowPassGain,
        BandPass,
        BandPassNormalised,
        Notch,
        NotchNormalised,
        BandPassGain,
        NotchGain,
    };

    // CDP's SPEC_MINFRQ, and FILT_MAX_PG.
    static constexpr double minimumFrequency = 10.0;
    static constexpr double minimumQ = 1.0;
    static constexpr double maximumGain = 10000.0;

    // False, with no change, for frequencies outside 10 Hz to half the
    // sample rate, a Q outside 1 to half the sample rate, a gain outside 0 to
    // 10000 or what the phase vocoder refuses. Modes 1 to 6 read no high
    // frequency, and only modes 5, 6, 11 and 12 the gain.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, double frequency,
        double highFrequency, double q, double gain, bool liveFrequency, bool liveHighFrequency, bool liveQ);
    // False, with no change, before prepare or outside those ranges.
    [[nodiscard]] bool setControls(double frequency, double highFrequency, double q) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    [[nodiscard]] bool valid(double frequency, double highFrequency, double q) const noexcept;
    void filter(std::span<float> frame) noexcept;

    PhaseVocoder vocoder_;
    Mode mode_ = Mode::HighPass;
    double nyquist_ = 0.0;
    double gain_ = 1.0;
    double frequency_ = 0.0;
    double highFrequency_ = 0.0;
    double q_ = 0.0;
    double startFrequency_ = 0.0;
    double startHighFrequency_ = 0.0;
    double startQ_ = 0.0;
    bool liveFrequency_ = false;
    bool liveHighFrequency_ = false;
    bool liveQ_ = false;
};

// `hilite pluck`: a channel whose amplitude turns from zero (within CDP's
// FLTERR) to sounding is raised by the gain. CDP leaves the first frame as
// it is and takes the second's channels as the start.
class HilitePluck
{
public:
    static constexpr double maximumGain = 1000.0;

    // False, with no change, for a gain outside 0 to 1000 or what the phase
    // vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double gain);
    // False, with no change, before prepare or outside 0 to 1000.
    [[nodiscard]] bool setGain(double gain) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    double gain_ = 1.0;
    double startGain_ = 1.0;
    // Whether each channel sounded in the last frame.
    std::vector<bool> sounding_;
    std::size_t frames_ = 0;
};
}
