/*
 * Derived from the CDP System, Release 8: dev/science/spin.c and
 * dev/cdp2k/readdata.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#pragma once

#include <cstddef>
#include <span>

namespace talas::cdp
{
// CDP `spin stereo` with no doppler (Dopl 0): a stereo image spun round the
// listener at Rate cycles a second, the left edge's position -cos(2 pi
// phase) from left to right and back, the right edge mirrored. As the edges
// pass the centre, the one passing in front is raised by up to Boost + 1
// times and the one behind lowered as much (Boost 0: no change), and with
// Attenuation both are lowered by up to that much (1 - Attenuation).
//
// | Mode           | CDP | output                                       |
// | Stereo         | 1   | 2 channels, the edges panned between them    |
// | Wide           | 2   | Channels; the edges panned between the       |
// |                |     | channels either side of Centre, which takes  |
// |                |     | the mono mix at a level from Centre minimum  |
// |                |     | (edges at the sides) to Centre maximum (at   |
// |                |     | the centre)                                  |
// | WideBareCentre | 3   | Channels; each edge panned across the three  |
// |                |     | channels around Centre, Boost acting on the  |
// |                |     | centre only, the centre lowered by           |
// |                |     | Attenuation and the mono mix added there at  |
// |                |     | Centre minimum                               |
//
// CDP reads its Rate breakpoint file every frame: mode 1 at frame / sr, but
// modes 2 and 3 at floor((frame Channels + left channel) / 2) / sr (its
// output sample count halved), Channels / 2 times too fast; a Rate set
// before a process call plays from its first frame. The spin starts at the
// left and turns clockwise for a Rate at or above 0 at the first frame.
//
// CDP's mode 1 (spindopl) does not clear its half-cycle flag between its
// two passes: its output pass starts with the flag its level pass ended
// with, a whole-file fact (for a steady Rate, set when the file ends in the
// second half of a cycle) that skips the first half-cycle's swap of the
// edges passing in front and behind, so that they stay swapped. startFlipped gives it; halfCycleFlag() reads it, so
// playing the file once from false gives the value. Modes 2 and 3 clear it.
//
// CDP reads each input frame by interpolating towards the next with a
// fraction of 0, which keeps the frame but for a zero's sign (-0 comes out
// 0 unless the next frame is negative); the centre channel of modes 2 and
// 3 shows that sign, so those modes come out latencyFrames() = 1 frame
// late, mode 1 with no latency.
//
// CDP runs the whole process once to find its output's peak and, if that
// passes 0.95, scales the whole output to peak at 0.95. A stream cannot know
// its peak in advance: it matches CDP bit for bit whenever CDP's first pass
// finds a peak at or below 0.95, and otherwise plays unscaled, clipped at
// -1 and 1. CDP's doppler shift (Dopl > 0) moves its read points by a
// ratio that depends on the largest Rate in its file, without bound: not
// ported (with a largest Rate of 0, CDP's factor is 0 / 0 even without
// doppler, and its output is not a number; the stream plays on). CDP's
// spin quad, with two stereo inputs, is not ported.
class Spin
{
public:
    enum class Mode
    {
        Stereo,
        Wide,
        WideBareCentre,
    };

    // spin.c's ranges.
    static constexpr double minimumRate = -100.0;
    static constexpr double maximumRate = 100.0;
    static constexpr double maximumBoost = 16.0;
    static constexpr int minimumChannels = 4;
    static constexpr int maximumChannels = 16;

    // False, with no change, for a sample rate that is not finite and
    // positive, values outside the ranges (Rate -100 to 100, Boost 0 to 16,
    // Attenuation, Centre minimum and maximum 0 to 1; within 0.000002 of a
    // range end is that end), and in the wide modes Channels outside 4 to
    // 16, Centre outside 1 to Channels, or (mode Wide) a Centre minimum over
    // the maximum. CDP's defaults: Boost 2, Attenuation 0, Centre minimum 0,
    // maximum 0.5. Mode Stereo ignores the channel arguments; mode
    // WideBareCentre ignores centreMaximum; only mode Stereo takes
    // startFlipped.
    [[nodiscard]] bool prepare(double sampleRate, Mode mode, double rate, double boost = 2.0,
        double attenuation = 0.0, int channels = 8, int centre = 1, double centreMinimum = 0.0,
        double centreMaximum = 0.5, bool startFlipped = false);

    // Rate from the next process call's first frame. False, with no change,
    // before prepare or for a Rate outside -100 to 100.
    [[nodiscard]] bool setRate(double rate) noexcept;

    // The spin back to its start, prepare's Rate.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    // 0 for mode Stereo, 1 for the wide modes.
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return mode_ == Mode::Stereo ? 0 : 1; }

    // CDP's half-cycle flag after the frames played so far.
    [[nodiscard]] bool halfCycleFlag() const noexcept { return flipped_ != 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The output is interleaved, outputChannels() samples a frame. False,
    // with no change, before prepare, for inputs of different sizes, an
    // output that is not outputChannels() times them, or a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> left, std::span<const double> right, std::span<double> output) noexcept;

    bool operator==(const Spin&) const = default;

private:
    // One frame's input and gains, waiting for the next frame in the wide
    // modes.
    struct Frame
    {
        float left = 0.0F;
        float right = 0.0F;
        double lleftgain = 0.0;
        double lrightgain = 0.0;
        double rleftgain = 0.0;
        double rrightgain = 0.0;
        double lcentregain = 0.0;
        double rcentregain = 0.0;
        double centre = 0.0;
        bool operator==(const Frame&) const = default;
    };

    void advance(double rate) noexcept;
    [[nodiscard]] Frame gains(float left, float right) noexcept;
    void render(const Frame& frame, float nextLeft, float nextRight, double* out) const noexcept;

    Mode mode_ = Mode::Stereo;
    double sampleRate_ = 0.0;
    double startRate_ = 0.0;
    double rate_ = 0.0;
    double boost_ = 0.0;
    double attenuation_ = 0.0;
    double centreMinimum_ = 0.0;
    double centreRange_ = 0.0;
    std::size_t channels_ = 0;
    std::size_t leftChannel_ = 0;
    std::size_t rightChannel_ = 1;
    std::size_t centreChannel_ = 0;

    bool startFlipped_ = false;
    bool started_ = false;
    bool held_ = false;
    Frame last_;
    double ch1pos_ = 0.0;
    double lastspin_ = 0.0;
    int flipped_ = 0;
    int movingforward_ = 1;
};
}
