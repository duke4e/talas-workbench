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
#pragma once

#include "talas/cdp/PhaseVocoder.h"
#include "talas/cdp/SpectralEnvelope.h"

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP's two-file spectral programs, `combine sum`, `diff`, `max`, `mean` and
// `cross` and `formants vocode`, on two streams analysed in step between
// `pvoc anal` and `pvoc synth`: the input is CDP's first file (infile), the
// side its second (infile2), and the output is the input's analysis, edited
// frame by frame with the side's, then synthesised. Both streams take the
// same points and overlap; CDP stops at the shorter file, and a stream's two
// inputs are always alike in length. The output comes latencyFrames late.
//
// CDP takes the crossover (sum, diff) and the interpolation (cross) as
// breakpoint files and reads them at each analysis frame's time: a value
// given before a process call reaches the frames analysed during it. A
// value CDP is given as a number is read once, and one equal to the default
// (within CDP's FLTERR) leaves the flag unset, as CDP leaves it.

// `combine sum`: each channel's amplitude gains the side's, or the
// crossover's share of it (CDP's -c, 0 to 1).
class CombineSum
{
public:
    static constexpr double defaultCrossover = 1.0;

    // liveCrossover is CDP's breakpoint file, which always takes the
    // crossover's share (even of 1); a fixed crossover of 1 is a plain sum.
    // setCrossover reaches the frames only with liveCrossover. False, with
    // no change, for a crossover outside 0 to 1 or what the phase vocoder
    // refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double crossover, bool liveCrossover);
    // False, with no change, before prepare or outside 0 to 1.
    [[nodiscard]] bool setCrossover(double crossover) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    PhaseVocoder sideVocoder_;
    // SUM_IS_CROSS.
    bool cross_ = false;
    bool liveCrossover_ = false;
    double crossover_ = defaultCrossover;
    double startCrossover_ = defaultCrossover;
};

// `combine diff`: each channel's amplitude loses the side's, or the
// crossover's share of it (CDP's -c, 0 to 1), stopping at zero unless
// keepNegative (CDP's -a) keeps the amplitudes below zero.
class CombineDiff
{
public:
    static constexpr double defaultCrossover = 1.0;

    // As CombineSum's: a fixed crossover of 1 subtracts all of the side.
    // False, with no change, for a crossover outside 0 to 1 or what the
    // phase vocoder refuses.
    [[nodiscard]] bool prepare(
        double sampleRate, int points, int overlap, double crossover, bool liveCrossover, bool keepNegative);
    // False, with no change, before prepare or outside 0 to 1.
    [[nodiscard]] bool setCrossover(double crossover) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    PhaseVocoder sideVocoder_;
    // DIFF_IS_CROSS and DIFF_SUBZERO.
    bool cross_ = false;
    bool keepNegative_ = false;
    bool liveCrossover_ = false;
    double crossover_ = defaultCrossover;
    double startCrossover_ = defaultCrossover;
};

// `combine max`: in each channel the louder of the two, with its frequency
// (the input's where they are equal).
class CombineMax
{
public:
    // False, with no change, for what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    PhaseVocoder sideVocoder_;
};

// `combine mean`: between the low and high frequencies' channels, the
// loudest channel of each frame pairs with the side's loudest, the next with
// the next and so on, each pair giving an amplitude (by the mode: their mean,
// the input's, the side's or the louder) at the mean of their frequencies,
// taken in pitch (modes 1, 3, 5, 7) or in Hz (2, 4, 6, 8), in the channel of
// that frequency, the loudest pair a channel receives kept. CDP builds its
// lists of loudest channels with a shuffle that drops some of them, and
// this keeps that. The channels outside the range keep the input's. With
// zeroOutside (CDP's -z) CDP only zeroes the input's channels outside the
// range and leaves the rest of the frame as it is. CDP reads the frequencies
// and its channel count once; CDP8 checks the count but never uses it.
class CombineMean
{
public:
    enum class Mode
    {
        MeanAmplitudeMeanPitch = 1,
        MeanAmplitudeMeanFrequency,
        InputAmplitudeMeanPitch,
        InputAmplitudeMeanFrequency,
        SideAmplitudeMeanPitch,
        SideAmplitudeMeanFrequency,
        MaximumAmplitudeMeanPitch,
        MaximumAmplitudeMeanFrequency,
    };

    // CDP's PITCHZERO, the default low frequency (the high defaults to half
    // the sample rate and the channel count to points / 2 + 1).
    static constexpr double defaultLowFrequency = 5.0;

    // The low frequency (CDP's -l) from 5 Hz to half the sample rate less
    // half a channel width, the high (-h) from 5 Hz and a channel width to
    // half the sample rate (CDP swaps their channels when the high is the
    // lower), and the channel count (-c) from 2 to points / 2 + 1. False,
    // with no change, outside those ranges or for what the phase vocoder
    // refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, double lowFrequency,
        double highFrequency, int channels, bool zeroOutside);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

private:
    void meanFrame(std::span<float> frame, std::span<const float> sideFrame) noexcept;
    void putInList(float* ampstore, int* locstore, float val, int location) const noexcept;
    void generateMeanValues(float* frame, const float* sideFrame) noexcept;

    PhaseVocoder vocoder_;
    PhaseVocoder sideVocoder_;
    Mode mode_ = Mode::MeanAmplitudeMeanPitch;
    bool zeroOutside_ = false;
    int clength_ = 0;
    double chwidth_ = 0.0;
    double halfchwidth_ = 0.0;
    // MEAN_BOT and MEAN_TOP, as float indices into the frame.
    int bot_ = 0;
    int top_ = 0;
    // The side frame, which CDP rectifies; CDP's windowbuf[0] (the output
    // frame), windowbuf[1] and [2] (the loudest amplitudes) and MEAN_LOC1
    // and MEAN_LOC2 (their places).
    std::vector<float> side_;
    std::vector<float> out_;
    std::vector<float> loud1_;
    std::vector<float> loud2_;
    std::vector<int> loc1_;
    std::vector<int> loc2_;
};

// `combine cross`: the input's channel amplitudes move toward the side's by
// the interpolation (CDP's -i, 0 none, 1 all the way, where they are
// replaced).
class CombineCross
{
public:
    static constexpr double defaultInterpolation = 1.0;

    // liveInterpolation is CDP's breakpoint file, which always interpolates
    // (even by 1); a fixed interpolation of 1 replaces the amplitudes.
    // setInterpolation reaches the frames only with liveInterpolation.
    // False, with no change, for an interpolation outside 0 to 1 or what the
    // phase vocoder refuses.
    [[nodiscard]] bool prepare(
        double sampleRate, int points, int overlap, double interpolation, bool liveInterpolation);
    // False, with no change, before prepare or outside 0 to 1.
    [[nodiscard]] bool setInterpolation(double interpolation) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    PhaseVocoder sideVocoder_;
    // CROSS_INTP.
    bool interpolate_ = false;
    bool liveInterpolation_ = false;
    double interpolation_ = defaultInterpolation;
    double startInterpolation_ = defaultInterpolation;
};

// `formants vocode`: the side's formant envelope imposed on the input. Each
// channel's amplitude is scaled by the side's envelope over the input's,
// both taken with CDP's -fN or -pN with N bands, and -i for its quick search
// (see SpectralEnvelope), the input's read at its channel's frequency and the
// side's at the side's channel's; channels below the low or above the high
// frequency are zeroed, the frame keeps its total amplitude, and the gain
// (CDP's -g) scales it. CDP leaves the first frame as it is and reads the
// frequencies and gain once.
class FormantsVocode
{
public:
    // CDP's PITCHZERO, the default low frequency (the high defaults to half
    // the sample rate), and VOCO_MAX_GAIN.
    static constexpr double defaultLowFrequency = 5.0;
    static constexpr double defaultGain = 1.0;
    static constexpr double maximumGain = 10.0;

    // The low (-l) and high (-h) frequencies from 5 Hz to half the sample
    // rate, either under 10 Hz taken as 0, as setup_pitchzero makes them;
    // the gain from CDP's FLTERR (0.000002) to 10, one within FLTERR of 1
    // leaving the frame as it is. False, with no change, outside those
    // ranges, for bands outside SpectralEnvelope::bandRange for the frame
    // or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, bool pitchwise, int bands,
        bool quickSearch, double lowFrequency, double highFrequency, double gain);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

private:
    void vocodeFrame(std::span<float> frame, std::span<const float> sideFrame) noexcept;

    PhaseVocoder vocoder_;
    PhaseVocoder sideVocoder_;
    // specenvamp and specenvamp2: the input's and the side's envelopes.
    SpectralEnvelope envelope_;
    SpectralEnvelope sideEnvelope_;
    double lowFrequency_ = 0.0;
    double highFrequency_ = 0.0;
    double gain_ = defaultGain;
    // The side frame, which CDP rectifies.
    std::vector<float> side_;
    std::size_t frames_ = 0;
};
}
