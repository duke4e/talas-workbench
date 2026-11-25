/*
 * Derived from the CDP System, Release 8: dev/blur/blur.c and ap_blur.c,
 * dev/strange/strange.c and ap_strange.c, and dev/cdp2k/tklib1.c and
 * tklib3.c
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
#include "talas/cdp/SpectralEnvelope.h"

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `blur spread` and `strange glis` on each frame between `pvoc anal` and
// `pvoc synth`, both within each frame's formant envelope (CDP's -fN or -pN
// with N bands, and -i for its quick search; see SpectralEnvelope). CDP
// takes the spread and the glissando rate as breakpoint files and reads
// them at each analysis frame's time: a value given before a process call
// reaches the frames analysed during it.

// `blur spread`: each channel's amplitude moves toward the envelope's at
// its frequency by the spread (0 none, 1 all the way), and the frame keeps
// its total amplitude. CDP leaves the first frame as it is.
class BlurSpread
{
public:
    static constexpr double defaultSpread = 1.0;

    // False, with no change, for a spread outside 0 to 1, bands outside
    // SpectralEnvelope::bandRange for the frame or what the phase vocoder
    // refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, bool pitchwise, int bands,
        bool quickSearch, double spread);
    // False, with no change, before prepare or outside 0 to 1.
    [[nodiscard]] bool setSpread(double spread) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void spreadFrame(std::span<float> frame) noexcept;

    PhaseVocoder vocoder_;
    SpectralEnvelope envelope_;
    double spread_ = defaultSpread;
    double startSpread_ = defaultSpread;
    std::size_t frames_ = 0;
};

// `strange glis`: glissandi inside each frame's formant envelope. Mode 1
// writes Shepard tones, a harmonic series on a fundamental gliding up or
// down an octave and back, its odd partials fading in or out; mode 2 an
// inharmonic series spaced by the step in Hz; mode 3 moves the frame's own
// partials by the glide, each at the envelope's amplitude at its new
// frequency in proportion to its old one. CDP's -t top frequency rolls
// the spectrum off over 500 Hz above it. The frame keeps its total
// amplitude. CDP leaves the first frame's amplitudes as they are and, in
// modes 1 and 2, sets its channel frequencies from twice the channel width
// up; the glide continues from frame to frame.
class StrangeGlis
{
public:
    enum class Mode
    {
        Shepard = 1,
        Inharmonic,
        Self,
    };

    // CDP's GLIS_SHIFT_DEFAULT, the step's default in mode 2.
    static constexpr double defaultStep = 50.0;
    // CDP's PITCHZERO, the least top frequency.
    static constexpr double minimumTopFrequency = 5.0;

    // The glissando rate in semitones a second (negative downward), within
    // eight over CDP's frame time ((float)(1 / arate)) either way; the step
    // in Hz (mode 2 only) from one channel width to a quarter of the sample
    // rate; with hasTop (CDP's -t), the top frequency from 5 Hz to half the
    // sample rate. False, with no change, outside those ranges, for bands
    // outside SpectralEnvelope::bandRange for the frame or what the phase
    // vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, bool pitchwise, int bands,
        bool quickSearch, double glisRate, double step, bool hasTop, double topFrequency);
    // False, with no change, before prepare or outside the rate's range.
    [[nodiscard]] bool setGlisRate(double glisRate) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // After prepare: the rate's limit either way, and CDP's channel width,
    // the least step.
    [[nodiscard]] double maximumGlisRate() const noexcept { return maximumGlisRate_; }
    [[nodiscard]] double channelWidth() const noexcept { return envelope_.channelWidth(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void glisFrame(std::span<float> frame) noexcept;
    void initialiseWindowFrqs(std::span<float> frame) const noexcept;
    [[nodiscard]] double establishFrqParams(double rate) noexcept;
    void writePartialsMovingDown(std::span<float> frame, double scalefact) noexcept;
    void writePartialsMovingUp(std::span<float> frame, double scalefact) noexcept;
    [[nodiscard]] int channelFor(double thisfrq) const noexcept;

    PhaseVocoder vocoder_;
    SpectralEnvelope envelope_;
    Mode mode_ = Mode::Shepard;
    int clength_ = 0;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    double halfchwidth_ = 0.0;
    double maximumGlisRate_ = 0.0;
    // GLIS_CONVERTOR, GLIS_SHIFT, GLIS_HALF_SHIFT, GLIS_REFPITCH,
    // GLIS_HALF_REFPITCH, GLIS_FRQTOP_TOP and GLIS_ROLL_OFF.
    double convertor_ = 0.0;
    double shift_ = 0.0;
    double halfShift_ = 0.0;
    double refpitch_ = 0.0;
    double halfRefpitch_ = 0.0;
    bool hasTop_ = false;
    double hifrq_ = 0.0;
    double frqtopTop_ = 0.0;
    double rollOff_ = 0.0;
    double glisRate_ = 0.0;
    double startGlisRate_ = 0.0;
    // GLIS_BASEFRQ, the glide's place, carried from frame to frame.
    double basefrq_ = 0.0;
    // CDP's amp and freq arrays (freq one longer: mode 3 going down writes
    // a frequency one channel above the amplitude it moves).
    std::vector<float> amp_;
    std::vector<float> freq_;
    // Mode 3 going up reads its channel after a failed lookup above the
    // Nyquist frequency: CDP8's build leaves the last channel found.
    int selfChannel_ = 0;
    std::size_t frames_ = 0;
};
}
