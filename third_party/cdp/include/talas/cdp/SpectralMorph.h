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
#pragma once

#include "talas/cdp/PhaseVocoder.h"

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP's two-file morphs, `morph morph` and `morph bridge`, on two streams
// analysed in step between `pvoc anal` and `pvoc synth`: the input is CDP's
// first file (infile), the side its second (infile2), and the output is the
// two analyses joined frame by frame, then synthesised. Both streams take the
// same points and overlap, and the output comes latencyFrames late.
//
// CDP takes every parameter as a number and reads it once. Its times, in
// seconds of the first file, count from the stream's start (or a reset), and
// CDP rounds each to whole frames of its frame time (the decimation over the
// sample rate, in float), half a frame up. A value within CDP's FLTERR
// (0.000002) of an end of its range is taken as that end, as CDP takes it,
// but for the ends CDP takes from its files' length, which the stream cannot
// know: there the stream limits a time to an hour, and an entry delay, which
// holds frames, to ten seconds. The second file's later entry (morph's
// stagger, bridge's offset) delays the side's frames by as many frames, as
// CDP reads its second file later.

// `morph morph`: from the amplitude start to the amplitude end the input's
// channel amplitudes move to the side's, and from the frequency start to the
// frequency end its channel frequencies, each along the mode's curve. Before
// the earlier start the output is the input as it is, after the later end the
// side as it is. The side enters the stagger late (CDP's -s): the stream
// delays its frames by the stagger, and both starts come at or after it.
//
// CDP's output runs past the input's end by the stagger, which a stream
// cannot play. CDP8 also writes its last, partly filled 20-frame buffer with
// the count of frames the morph ended at in its first file's buffer: where
// that count is the larger, frames repeated from earlier in the buffer follow
// the side's last frames, past the input's end; where it is the smaller, the
// side's last frames are dropped and CDP's output stops short of the input's
// end, which a stream cannot know.
class MorphMorph
{
public:
    enum class Mode
    {
        // Each fraction of the way raised to the exponent (1 is linear).
        Linear = 1,
        // A raised cosine, each half of the way warped by the exponent.
        Cosine,
    };

    // MPH_MIN_EXP and MPH_MAX_EXP.
    static constexpr double minimumExponent = 0.02;
    static constexpr double maximumExponent = 50.0;
    // An hour, where CDP's latest start and end are its file's length.
    static constexpr double maximumTime = 3600.0;
    // Ten seconds of the side's frames, where CDP's longest stagger is its
    // file less a frame.
    static constexpr double maximumStagger = 10.0;

    // CDP takes the four times and two exponents on its command line (no
    // defaults) and the stagger defaults to 0. Times in seconds: the starts
    // from 0 and the ends from two frames to maximumTime, each end after its
    // start (in seconds; equal frames are allowed, as CDP allows them); the
    // exponents from 0.02 to 50; the stagger from 0 to maximumStagger, at or
    // before both starts. False, with no change, outside those or for what
    // the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, double amplitudeStart,
        double amplitudeEnd, double frequencyStart, double frequencyEnd, double amplitudeExponent,
        double frequencyExponent, double stagger);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

private:
    void morphFrame(std::span<float> frame, std::span<const float> side, long long window) noexcept;
    [[nodiscard]] double curve(double interpratio, double exponent) const noexcept;

    PhaseVocoder vocoder_;
    PhaseVocoder sideVocoder_;
    Mode mode_ = Mode::Linear;
    double amplitudeExponent_ = 1.0;
    double frequencyExponent_ = 1.0;
    // MPH_ASTTW, MPH_AENDW, MPH_FSTTW, MPH_FENDW and MPH_STAGW.
    long long amplitudeStart_ = 0;
    long long amplitudeEnd_ = 0;
    long long frequencyStart_ = 0;
    long long frequencyEnd_ = 0;
    long long stagger_ = 0;
    // MPH_COS, with one more 1 past its wrap-around point.
    std::vector<double> costable_;
    // The side's last stagger_ frames, the oldest at the frame's slot.
    std::vector<float> entry_;
    long long frames_ = 0;
};

// `morph bridge`: from the start to the end time the output moves, a step a
// frame, from a mix holding the start fractions of the side's frequency and
// amplitude (CDP's -b sf2 and -c sa2, defaults 0) to one holding the end
// fractions (-d ef2 and -e ea2, defaults 1). Before the start the output is
// the start mix (the input as it is where both start fractions are 0, the
// side where both are 1), after the end the end mix alike. The side enters
// the offset late (-a, default 0): the stream delays its frames by the
// offset, holding silent frames (zero amplitude and frequency) before it, as
// CDP pads its second file. Where CDP then runs to its padded second file's
// end (end fractions of 1 in modes 1 and 5), its output runs past the input's
// end by the offset, which a stream cannot play.
//
// The mode sets the level: the mix's own (mode 1, which CDP8 mixes the
// frequencies by the amplitude fraction too, leaving sf2 and ef2 unused
// there save for choosing the plain input or side before the start and after
// the end), the quieter file's (2), the input's (3), the side's (4), the
// input's moving to the side's through the bridge (5) or the side's moving
// to the input's (6), each frame's total amplitude scaling one file's
// amplitudes before the mix.
//
// Refused where CDP's output would not start with the input: an offset of
// half a frame or more in modes 2, 4 and 6, or with sf2 or sa2 above 0 (CDP
// then starts its output where the side enters). CDP's default end (the end
// of the shorter file) depends on the files' length, so the end is required,
// and an end within FLTERR of two frames (its default, which CDP takes as no
// end given) is refused.
class MorphBridge
{
public:
    enum class Mode
    {
        Direct = 1,
        Minimum,
        FollowInput,
        FollowSide,
        InputToSide,
        SideToInput,
    };

    static constexpr double defaultOffset = 0.0;
    static constexpr double defaultStartFraction = 0.0;
    static constexpr double defaultEndFraction = 1.0;
    // An hour, where CDP's latest start and end are its file's length.
    static constexpr double maximumTime = 3600.0;
    // Ten seconds of the side's frames, where CDP's longest offset is its
    // first file less two frames.
    static constexpr double maximumOffset = 10.0;

    // The offset from 0 to maximumOffset seconds; the fractions from 0 to 1;
    // the start from 0 to maximumTime, where a start within FLTERR of 0 is
    // CDP's default, the offset; the end from two frames to maximumTime, at
    // least a frame after the start; the offset at or before the start.
    // False, with no change, outside those, for the settings refused above
    // or for what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, double offset,
        double startSideFrequency, double startSideAmplitude, double endSideFrequency, double endSideAmplitude,
        double start, double end);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

private:
    enum class Where
    {
        Before,
        In,
        After,
    };

    void bridgeFrame(std::span<float> frame, std::span<const float> side, long long window) noexcept;
    void interpolate(Where where, std::span<float> frame, std::span<const float> side) noexcept;

    PhaseVocoder vocoder_;
    PhaseVocoder sideVocoder_;
    Mode mode_ = Mode::Direct;
    // BRG_SF2 and BRG_SA2 as prepared, and as they step through the bridge.
    double startSideFrequency_ = defaultStartFraction;
    double startSideAmplitude_ = defaultStartFraction;
    double sideFrequency_ = defaultStartFraction;
    double sideAmplitude_ = defaultStartFraction;
    // BRG_FSTEP, BRG_ASTEP, BRG_TSTEP and BRG_TFAC.
    double frequencyStep_ = 0.0;
    double amplitudeStep_ = 0.0;
    double levelStep_ = 0.0;
    double levelFactor_ = 0.0;
    // BRG_SWIN, BRG_INTPEND, BRG_STARTIS and BRG_TAILIS, and the offset in
    // frames.
    long long startWindow_ = 0;
    long long endWindow_ = 0;
    int startIs_ = 0;
    int tailIs_ = 0;
    long long offset_ = 0;
    // The side's last offset_ frames, the oldest at the frame's slot.
    std::vector<float> entry_;
    long long frames_ = 0;
};
}
