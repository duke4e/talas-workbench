/*
 * Derived from the CDP System, Release 8: dev/standalone/mchanpan.c,
 * dev/cdp2k/readdata.c and dev/sfsys/osbind.c
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
#include <vector>

namespace talas::cdp
{
// CDP `mchanpan mchanpan 7` (crosspan): a multichannel input moved from one
// channel map to the next along a table of (time, map) points, from the
// stream's start. A map sends each input channel to an output channel (all
// zeros: every input to the centre, all channels at once). Between two
// different maps each input spreads out from its channel until it fills the
// ring (halfway), then gathers onto its new channel; to or from the centre
// it spreads or gathers over the whole move. Spread over S channels the
// fully covered channels play at 1/S raised by (1 - Rolloff) of the way to
// 1, the partly covered edges at that times the part.
//
// After the last point the last map holds. By default CDP8's quirks are
// kept, as its output shows them: holding a map, output channel n takes
// input channel map[n] (moving, input n goes to channel map[n]); the
// right-hand edge of a spread that wraps past the last channel lands that
// many channels into the next frame (CDP adjusts the wrong index), except
// at the end of one of CDP's buffers (131072 / N frames, or as given),
// where it is lost; and a table starting after time 0 is refused (CDP adds
// a point at 0 but moves only one entry of each map up to make room,
// scrambling them).
//
// Corrected (a prepare flag; no CDP reference) plays what CDP evidently
// intends: a held map sends input n to channel map[n], as moving does and
// as CDP's usage says; the wrapping edge lands on its channel within the
// frame (nothing spills, so CDP's buffers play no part); and a table
// starting after time 0 holds a copy of its first map from time 0. The
// rest is as CDP8 (including its spread over the whole ring at the very
// start or end of a move to or from the centre loading the channel opposite
// twice, whose intended fix is not evident). No latency.
class CrossPan
{
public:
    static constexpr int minimumChannels = 2;
    static constexpr int maximumChannels = 16;

    struct Point
    {
        double time = 0.0;
        // Output channels 1 to N for the first N input channels, or all 0.
        std::array<int, maximumChannels> map{};
        bool operator==(const Point&) const = default;
    };

    // CDP8's buffer for N channels: 131072 / N frames.
    [[nodiscard]] static std::size_t defaultBufferFrames(int channels) noexcept;

    // False, with no change, for a sample rate that is not finite and
    // positive, channels outside 2 to 16, an empty table, a first point
    // after frame 0 (unless corrected), a negative time, a time that does
    // not rise, two points in one frame (rounded), a map that neither sends
    // the channels each to a different channel 1 to N nor is all 0, a
    // rolloff outside 0 to 1 (within 0.000002 of a range end is that end),
    // or no buffer frames. bufferFrames 0 is defaultBufferFrames(channels).
    [[nodiscard]] bool prepare(double sampleRate, int channels, std::span<const Point> table, double rolloff,
        std::size_t bufferFrames = 0, bool corrected = false);

    [[nodiscard]] bool corrected() const noexcept { return corrected_; }

    // The table's start, frame 0 next (the start of CDP's first buffer).
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The input and output are interleaved, outputChannels() samples a
    // frame. False, with no change, before prepare, for spans of different
    // sizes or not whole frames, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const CrossPan&) const = default;

private:
    using Frame = std::array<float, 2 * maximumChannels>;

    void doSpan(const std::array<int, maximumChannels>& map, double span, const float* ibuf, Frame& obuf) const noexcept;
    void startSegment() noexcept;

    std::size_t channels_ = 0;
    double rolloff_ = 0.0;
    bool corrected_ = false;
    double centrelevel_ = 0.0;
    std::size_t bufferFrames_ = 0;
    // Event frames and maps (0-based outputs, -1 for the centre), with CDP's
    // added first and last points.
    std::vector<std::size_t> events_;
    std::vector<std::array<int, maximumChannels>> maps_;

    std::size_t segment_ = 0;
    int type_ = 0;
    std::size_t frame_ = 0;
    std::size_t bufferPosition_ = 0;
    std::array<float, maximumChannels> carry_{};
};
}
