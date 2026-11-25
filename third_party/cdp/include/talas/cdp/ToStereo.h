/*
 * Derived from the CDP System, Release 8: dev/standnew/tostereo.c and
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

#include <array>
#include <cstddef>
#include <span>

namespace talas::cdp
{
// CDP `tostereo tostereo`: a stereo input that starts mixed to mono and
// opens out to true stereo between Start and End (seconds from the
// stream's start), or, with End before Start, closes from stereo to mono.
// The two channels go to output channels Left and Right of N; mixed, each
// side keeps 1/2 of its own channel and takes 1/2 of the other, boosted by
// up to (2 Mix level - 1) where the mix is complete. With Right 0 (and N of
// 3 or more), the input forks instead: the mono mix (both channels times
// Mix level) on channel Left, opening out to the left input on the channel
// below it and the right input on the channel above it.
//
// CDP crossfades along its breakpoint lines (0, Start, End, 10000 s); the
// stream follows the same lines frame by frame. The output channels CDP
// never writes are 0 here (CDP leaves them as its memory was, 0 for all
// but very short files). No latency.
class ToStereo
{
public:
    // tostereo.c's ranges.
    static constexpr int minimumChannels = 2;
    static constexpr int maximumChannels = 16;

    // False, with no change, for a sample rate that is not finite and
    // positive, a start or end that is negative or not finite, a start
    // within 0.000002 s of the end, output channels outside 2 to 16, a left
    // or right channel outside 0 to 16 or past the output channels, the
    // channels CDP refuses (with 2 outputs both or neither of left and
    // right set, Left and Right 1 and 2 if neither; with more, Left set;
    // never the same channel), or a mix level outside 0.5 to 1 other than
    // 0 (0 is CDP's default: 0.707 forked, 0.5 otherwise). CDP also bounds
    // the times by its file's duration, which a stream cannot know.
    [[nodiscard]] bool prepare(double sampleRate, double start, double end, int outputChannels = 2,
        int leftChannel = 0, int rightChannel = 0, double mixLevel = 0.0);

    // Frame 0 (time 0) next.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The input is interleaved stereo; the output interleaved,
    // outputChannels() samples a frame. False, with no change, before
    // prepare, for an output that is not outputChannels() / 2 times the
    // input, an odd input size, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const ToStereo&) const = default;

private:
    double sampleRate_ = 0.0;
    std::size_t channels_ = 0;
    double start_ = 0.0;
    double timestep_ = 0.0;
    bool converges_ = false;
    int fork_ = -1;
    std::size_t left_ = 0;
    std::size_t right_ = 0;
    double mixLevel_ = 0.0;
    // The two breakpoint lines: time, value pairs.
    std::array<double, 8> ll_{};
    std::array<double, 8> lr_{};
    std::size_t frame_ = 0;
};
}
