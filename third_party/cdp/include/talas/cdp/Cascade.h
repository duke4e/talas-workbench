/*
 * Derived from the CDP System, Release 8: dev/science/cascade.c,
 * dev/sfsys/osbind.c and dev/cdp2k/readdata.c
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

#include "talas/cdp/Drand48.h"

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `cascade cascade` modes 1-5: the input cut into successive clips, each
// clip followed by Echos echoes of itself, every echo set laid over the
// input from its clip's place. The first clip is Clipsize long (rounded to
// frames); each later one is Clipsize plus a 5 ms splice long, read from
// where the last read ended less the splice (so that reads overlap by the
// splice) but placed a further splice earlier, crossfading with the last
// clip (CDP seeks its file before it backs up by the splice). Each copy has 5 ms splices at its ends
// and is quieter than the last, falling to 0.01 by the last echo (along a
// power, or with linear, by equal steps of 0.99 / Echos). With Clipmax
// (not 0) each clip's size is drawn between Clipsize and Clipmax, with
// Echosmax each echo count between Echos and Echosmax, and with Rand each
// echo but the first and last moved by up to Rand times the clip's length
// less its splice, from CDP's drand48 (rand() / RAND_MAX) seeded with Seed.
//
// | Mode       | CDP | output                                            |
// | Echoes     | 1   | 1 channel: the echoes spliced end to end          |
// | PanRight   | 2   | 2 channels: each echo set panned left to right    |
// |            |     | (faster at its start), the input on the left      |
// | Alternate  | 3   | 2 channels: from the centre, to the right for one |
// |            |     | clip, to the left for the next                    |
// | Rotate     | 4   | 8 channels: each echo one channel to the right of |
// |            |     | the last, after it rather than spliced to it       |
// | RotateBack | 5   | 8 channels: as Rotate, turning back the other way |
// |            |     | for every other clip                              |
//
// CDP takes Clipsize, Echos, Clipmax and Echosmax as breakpoint files but
// reads them only at time 0 (its time never advances): they are fixed
// here. A clip's echoes need the whole clip, read a splice ahead of its
// place: the output comes latencyFrames() = the longest clip plus two
// splices less 1 frames late, and each clip is computed in the call that
// takes its last frame.
//
// CDP runs the whole process once to find its output's peak and, if that
// passes 0.95, scales the whole output to peak at 0.95 (with -n, always). A
// stream cannot know its peak in advance: it matches CDP bit for bit
// whenever CDP's first pass finds a peak at or below 0.95, and otherwise
// plays unscaled, clipped at -1 and 1; -n is not ported. CDP shortens the
// clip its file ends in to the samples left, and drops it when it holds no
// more than two splices: the stream goes on, so it matches CDP before that
// clip's place. CDP's modes 6-10 (clips cut at the
// times of a text file) and its shredding (-N, -C, -a) are not ported.
//
// Mono in (CDP's mode 1 echoes each channel of a multichannel file alike,
// with the same draws: one processor per channel with the same seed plays
// it).
class Cascade
{
public:
    enum class Mode
    {
        Echoes,
        PanRight,
        Alternate,
        Rotate,
        RotateBack,
    };

    struct Settings
    {
        Mode mode = Mode::Echoes;
        // Seconds, 0.005 to 60.
        double clipSize = 0.5;
        // 1 to 64.
        int echoes = 8;
        // Seconds, 0 (off) or 0.005 to 60.
        double clipMax = 0.0;
        // 0 (off) or 1 to 64.
        int echoesMax = 0;
        // 0 to 1.
        double rand = 0.0;
        // 1 to 64 (CDP's 0, a clock seed, is refused when anything is
        // drawn).
        int seed = 1;
        // CDP's -l.
        bool linear = false;
        bool operator==(const Settings&) const = default;
    };

    // The longest latency (and echo set) the stream takes.
    static constexpr std::size_t maximumFrames = std::size_t{1} << 22;

    // False, with no change, for a sample rate that is not finite and
    // positive, values outside the ranges (within 0.000002 of a range end is
    // that end), a seed of 0 with anything drawn, or a latency plus the
    // longest echo set over maximumFrames.
    [[nodiscard]] bool prepare(double sampleRate, const Settings& settings);

    // Silence, the first clip from the next frame, the draws from Seed.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return latency_; }

    // 1, 2 or 8.
    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The clip being read: its place in the output, the first input frame
    // it reads (a splice later, but for the first clip) and its size in
    // frames. A file ending inside its read is where CDP shortens (or drops)
    // its last clip, from its place on.
    [[nodiscard]] std::size_t clipStart() const noexcept { return clipStart_; }
    [[nodiscard]] std::size_t readStart() const noexcept
    {
        return clipStart_ + (firstClip_ ? 0 : static_cast<std::size_t>(spliclen_));
    }
    [[nodiscard]] std::size_t clipFrames() const noexcept { return static_cast<std::size_t>(cliplen_); }

    // The output is interleaved, outputChannels() samples a frame. False,
    // with no change, before prepare, for an output that is not
    // outputChannels() times the input, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const Cascade&) const = default;

private:
    void drawClip() noexcept;
    void computeClip() noexcept;
    [[nodiscard]] float input(std::size_t frame) const noexcept { return ring_[frame & ringMask_]; }
    [[nodiscard]] float& out(std::size_t sample) noexcept
    {
        return out_[((sample / channels_) & outMask_) * channels_ + sample % channels_];
    }

    Settings settings_;
    std::size_t channels_ = 0;
    std::size_t latency_ = 0;
    long spliclen_ = 0;
    long clip_ = 0;
    long clipMax_ = 0;
    double atten_ = 0.0;

    std::vector<float> ring_;
    std::size_t ringMask_ = 0;
    std::vector<float> out_;
    std::size_t outMask_ = 0;
    std::vector<float> echobuf_;

    struct Random
    {
        Drand48 generator;
        bool operator==(const Random& other) const noexcept { return generator.state == other.generator.state; }
    };

    Random random_;
    std::size_t frame_ = 0;
    std::size_t clipStart_ = 0;
    long cliplen_ = 0;
    int echocnt_ = 0;
    bool firstClip_ = true;
    int panright_ = 1;
};
}
