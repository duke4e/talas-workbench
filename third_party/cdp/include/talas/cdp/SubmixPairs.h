/*
 * Derived from the CDP System, Release 8: dev/submix/mixmerge.c, inbetwee.c,
 * mixprepro.c and ap_mix.c, dev/standnew/repair.c, and dev/cdp2k/tklib1.c,
 * tklib3.c and readdata.c
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace talas::cdp
{
// More of CDP's programs that combine two mono sounds sample by sample, as
// in TwoInput.h: the input is CDP's first file and the side its second.
// CDP reads both files in step, so each output frame takes the frames of
// both that arrive with it (or, with a stagger, the side's from that much
// earlier), and nothing waits.

// CDP `submix merge`: the input times Skew / (Skew + 1) plus the side times
// one less that. Stagger delays the side, which CDP's second file enters
// that much later, the input playing alone until then (at its own gain);
// Skip starts CDP's second file that far in, which a stream can follow only
// while the side is not read ahead of the input, so Skip may be at most the
// Stagger. CDP rounds both to frames and then down to a whole number of
// 256-frame sectors (F_SECSIZE); the processor does the same. Times run
// from the stream's start.
//
// CDP's -b starts the output later and its -e stops it early, which a
// stream, keeping its length, cannot do: the processor plays as CDP's
// defaults, from the start of both to the end. CDP plays its longer file
// on, alone, past the other's end; the stream plays both on.
class MixMerge
{
public:
    // tkglobals.h's F_SECSIZE: Stagger and Skip are whole sectors of frames.
    static constexpr std::size_t sectorFrames = 256;
    static constexpr double defaultSkew = 1.0;
    // tklib1.c's MIXTWO ranges: Skew from 1/MAXSHORT to MAXSHORT.
    static constexpr double minimumSkew = 1.0 / 32767.0;
    static constexpr double maximumSkew = 32767.0;
    // The longest stagger the side's delay line holds (CDP's runs to its
    // first file's duration).
    static constexpr double maximumStaggerSeconds = 10.0;

    // False, with no change, for a sample rate that is not finite and
    // positive, a stagger outside 0 to 10 s, a skip outside 0 to the
    // stagger (compared in frames, after the rounding to sectors), a skew
    // outside 1/32767 to 32767, or a stagger or skip beyond 2^31 - 1 frames
    // (CDP's int). A value within 0.000002 of either end of its range (CDP's
    // FLTERR) is taken as that end, as CDP takes it.
    [[nodiscard]] bool prepare(
        double sampleRate, double stagger = 0.0, double skew = defaultSkew, double skip = 0.0);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

    bool operator==(const MixMerge&) const = default;

private:
    bool prepared_ = false;
    // mixtwo_preprocess's MIX2_GAIN1 and MIX2_GAIN2.
    double inputGain_ = 0.5;
    double sideGain_ = 0.5;
    // MIX_STAGGER in frames: the input plays alone until then.
    std::size_t stagger_ = 0;
    // The side's last (Stagger - Skip) frames, as CDP's float buffer holds
    // them.
    std::vector<float> delay_;
    std::size_t delayPosition_ = 0;
    // The frames since reset, counted until the stagger ends.
    std::size_t frame_ = 0;
};

// CDP `submix faders` with two files: the input and the side at the levels
// of a balance table, summed and multiplied by Gain. Each line of the table
// gives a time and a level for each file; CDP scales each line's levels to
// sum to one, holds its times in frames (rounded) and ramps each level by a
// fixed increment, added at every frame, from one line to the next, holding
// the last line's levels after it. Times run from the stream's start.
//
// CDP checks each time only against the first line's, so its times need not
// rise: a line whose frame CDP's count has already reached or passed ends
// its ramp at once, and its levels then start the next ramp from where the
// count stands, with the increment CDP worked out from the line's own frame
// (over an unsigned difference of frames). The processor follows the lines
// as CDP does. CDP takes any number of files; the processor takes two.
class MixFaders
{
public:
    struct Line
    {
        double time = 0.0;
        double input = 0.0;
        double side = 0.0;
    };

    static constexpr double defaultGain = 1.0;
    // tklib1.c's AUTOMIX range for the envelope.
    static constexpr double maximumGain = 256.0;

    // False, with no change, for a sample rate that is not finite and
    // positive, an empty table, a first time other than 0, a later time
    // under one over the sample rate (CDP's errtime), a time beyond
    // 2^32 - 1 frames (CDP's unsigned int), a negative or non-finite level,
    // a line whose levels sum to 0, or a gain outside 0 to 256. A gain
    // within 0.000002 of either end (CDP's FLTERR) is taken as that end, as
    // CDP takes it.
    [[nodiscard]] bool prepare(double sampleRate, std::span<const Line> balance, double gain = defaultGain);

    // CDP takes the envelope as a breakpoint file, which it reads at every
    // frame (do_automix reads the next frame's value after each frame): the
    // frames of the next process call take this gain. With a breakpoint
    // file CDP plays its first frame at gain 1 (the default it has not yet
    // replaced), whatever the file's first value. False, with no change,
    // before prepare or for a gain outside 0 to 256 (snapped as prepare's).
    [[nodiscard]] bool setGain(double gain) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !lines_.empty(); }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

    bool operator==(const MixFaders&) const = default;

private:
    struct ScaledLine
    {
        std::uint32_t frame = 0;
        std::array<double, 2> level{};

        bool operator==(const ScaledLine&) const = default;
    };

    void startSegment() noexcept;

    // read_and_scale_balance_data's table, its times in frames.
    std::vector<ScaledLine> lines_;
    double startGain_ = defaultGain;
    double gain_ = defaultGain;
    // do_automix's state: the line that ends the current ramp, its frame,
    // and the levels and their increments.
    std::size_t nextLine_ = 0;
    std::uint32_t nextFrame_ = 0;
    bool lastLine_ = false;
    std::array<double, 2> level_{};
    std::array<double, 2> increment_{};
    // do_automix's outcnt, counted until the last line.
    std::uint64_t frame_ = 0;
};

// CDP `submix interleave` with two mono files, and standnew `repair` with
// two mono files and 2 channels: the input as the left channel and the side
// as the right, sample for sample (as floats). CDP's interleave pads the
// shorter file with silence; repair refuses files of different lengths.
class MixInterleave
{
public:
    // Always true: the programs have no parameters.
    [[nodiscard]] bool prepare() noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(std::span<const double> input, std::span<const double> side,
        std::span<double> left, std::span<double> right) noexcept;

    bool operator==(const MixInterleave&) const = default;

private:
    bool prepared_ = false;
};

// CDP `submix inbetween` (mode 1 or 2), one of its output files: the input
// times one less Ratio plus the side times Ratio. Mode 2 takes the ratios
// from a file, each 0 to 1; mode 1 makes Count files, the nth at
// n * (1 / (Count + 1)). CDP plays the longer file on, alone, past the
// other's end; the stream plays both on.
class MixInbetween
{
public:
    // mixxcon.h's MAXBETWEEN.
    static constexpr int maximumCount = 999;

    // Mode 2: false, with no change, for a ratio outside 0 to 1.
    [[nodiscard]] bool prepare(double ratio) noexcept;

    // Mode 1: the file-th of count files. False, with no change, unless
    // 1 <= file <= count <= 999.
    [[nodiscard]] bool prepareCount(int count, int file) noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] double ratio() const noexcept { return ratio_; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

    bool operator==(const MixInbetween&) const = default;

private:
    bool prepared_ = false;
    double ratio_ = 0.5;
};
}
