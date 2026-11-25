/*
 * Derived from the CDP System, Release 8: dev/focus/focus.c and
 * ap_focus.c, and dev/cdp2k/tklib3.c
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
// CDP `focus exag`, `focus fold`, `focus accu` and `focus step` on each
// frame between
// `pvoc anal` and `pvoc synth`. CDP takes their fields as breakpoint files
// and reads them at each analysis frame's time: a value given before a
// process call reaches the frames analysed during it.

// `focus exag`: each frame's contour, its amplitudes over the loudest,
// raised to the power of one over the exaggeration, at the frame's first
// total amplitude. CDP leaves the first frame as it is.
class FocusExaggerate
{
public:
    static constexpr double minimumExaggeration = 0.001;
    static constexpr double maximumExaggeration = 1000.0;

    // False, with no change, for an exaggeration outside 0.001 to 1000 or
    // what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double exaggeration);
    // False, with no change, before prepare or outside 0.001 to 1000.
    [[nodiscard]] bool setExaggeration(double exaggeration) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    double exaggeration_ = 2.0;
    double startExaggeration_ = 2.0;
    std::size_t frames_ = 0;
};

// `focus fold`: partials outside the band from the low to the high
// frequency (in either order) move by octaves into it, each taking the
// channel of its new frequency if louder than what it holds there, or, with
// the body (CDP's -x), the nearest channel within four on either side.
class FocusFold
{
public:
    // CDP's PITCHZERO: half its lowest spectral frequency.
    static constexpr double minimumFrequency = 5.0;

    // A live frequency is CDP's breakpoint value, raised to 10 Hz; a static
    // one under 10 Hz is zero, as setup_pitchzero makes them. False, with no
    // change, for frequencies outside 5 Hz to half the sample rate or what
    // the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double lowFrequency,
        double highFrequency, bool withBody, bool liveLow, bool liveHigh);
    // False, with no change, before prepare or outside 5 Hz to half the
    // sample rate.
    [[nodiscard]] bool setFrequencies(double lowFrequency, double highFrequency) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void fold(std::span<float> frame) noexcept;
    void foldIn(std::span<float> frame, int vc, double thisfrq) noexcept;

    PhaseVocoder vocoder_;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    double halfchwidth_ = 0.0;
    double low_ = 0.0;
    double high_ = 0.0;
    double startLow_ = 0.0;
    double startHigh_ = 0.0;
    bool withBody_ = false;
    bool liveLow_ = false;
    bool liveHigh_ = false;
    // FOLD_LOCHAN and FOLD_HICHAN, as float indices.
    int lochan_ = 0;
    int hichan_ = 0;
};

// `focus accu`: each channel holds the loudest it has had, which decays by
// the decay ratio and glides by the glissando in octaves each second, until
// a louder one replaces it. CDP takes the first frame's amplitudes as the
// start and leaves that frame as it is.
class FocusAccumulate
{
public:
    static constexpr double minimumDecay = 0.001;
    // CDP's MAXGLISRATE: about a sixteenth of an octave a frame.
    static constexpr double maximumGlissPerFrame = 0.0625;

    // False, with no change, for a decay outside 0.001 to 1, a glissando
    // beyond a sixteenth of an octave a frame or what the phase vocoder
    // refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double decay, double glissando);
    // False, with no change, before prepare or outside those ranges.
    [[nodiscard]] bool setControls(double decay, double glissando) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    // The glissando, in octaves a second, beyond which prepare refuses.
    [[nodiscard]] static double maximumGlissando(double sampleRate, int points, int overlap) noexcept;

private:
    [[nodiscard]] bool valid(double decay, double glissando) const noexcept;

    PhaseVocoder vocoder_;
    float frametime_ = 0.0F;
    double decay_ = 1.0;
    double glissando_ = 0.0;
    double startDecay_ = 1.0;
    double startGlissando_ = 0.0;
    std::vector<float> windowbuf_;
    std::size_t frames_ = 0;
};

// `focus step`: the spectrum held for steps of the given time, each step
// playing its first frame, the time rounded to whole frames.
class FocusStep
{
public:
    // An hour, where CDP's longest step is its whole file.
    static constexpr double maximumStep = 3600.0;

    // False, with no change, for a step under two frames or over an hour or
    // what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double step);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    long long step_ = 2;
    std::vector<float> held_;
    long long frames_ = 0;
};
}
