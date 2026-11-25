/*
 * Derived from the CDP System, Release 8: dev/standalone/frame.c and
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
// CDP `frame shift` modes 1 and 2: the whole frame of a multichannel input
// turned around its ring of channels, Rotation turns a second (negative
// anticlockwise). Each input channel starts on its own channel and moves
// between neighbouring channels, panned equal-power; Smear bleeds that much
// of each channel's signal onto the channels either side (the channel
// itself lowered by twice as much). Mode 2 (prepareDual, an even channel
// count) turns channels 1, 3, 5, ... by Rotation and channels 2, 4, 6, ...
// by Second rotation, each along its own channels.
//
// A snake (a route through all the channels) redefines which channel comes
// next: CDP looks the channel that a moving input reaches up in the route
// each time the input passes a channel, so a new route takes effect at the
// next pass.
//
// CDP sets each rotation's starting pair by the first nonzero value in the
// first half of its breakpoint file (get_first_rotation_orientation steps
// two values at a time but stops at the breakpoint count), a lookahead the
// stream cannot make: clockwise, an input
// starts on its channel moving up; anticlockwise, it starts on the channel
// below its own (at that pair's left end), moving down; with no nonzero
// value, mode 1 takes clockwise and mode 2 anticlockwise. First direction
// gives it: 1 clockwise, -1 anticlockwise, 0 the sign of prepare's
// rotation.
//
// CDP reads its Rotation breakpoint files every round(sr / 500) frames
// (96 at 48 kHz) from frame 0; the stream takes prepare's rotations for
// the frames before the first such read, and from then the values set when
// each read frame (counted from prepare or reset) comes in. No latency.
class FrameRotate
{
public:
    // frame.c's ranges.
    static constexpr double maximumRotation = 500.0;
    static constexpr double maximumSmear = 0.5;
    static constexpr int minimumChannels = 2;
    static constexpr int maximumChannels = 16;
    // Below this CDP's reads come every 0 frames and it never ends.
    static constexpr double minimumSampleRate = 250.0;

    // The frames between CDP's Rotation reads: round(sr / 500).
    [[nodiscard]] static std::size_t readFrames(double sampleRate) noexcept;

    // Mode 1. snake is a route through all the channels (numbers 1 to
    // channels, each once), or empty for 1, 2, 3, .... False, with no
    // change, for a sample rate that is not finite or under 250, channels
    // outside 2 to 16, a rotation outside -500 to 500, a smear outside 0 to
    // 0.5 (within 0.000002 of a range end is that end), a first direction
    // outside -1 to 1, or a snake that is not such a route.
    [[nodiscard]] bool prepare(double sampleRate, int channels, double rotation, double smear = 0.0,
        int firstDirection = 0, std::span<const int> snake = {});

    // Mode 2: also false for an odd channel count, or both directions 0
    // (CDP: no rotation).
    [[nodiscard]] bool prepareDual(double sampleRate, int channels, double rotation, double secondRotation,
        double smear = 0.0, int firstDirection = 0, int secondFirstDirection = 0, std::span<const int> snake = {});

    // From the next frame CDP reads them on. False, with no change, before
    // prepare, outside -500 to 500, or (the second) in mode 1.
    [[nodiscard]] bool setRotation(double rotation) noexcept;
    [[nodiscard]] bool setSecondRotation(double rotation) noexcept;

    // A new route, for the passes from the next frame on. False, with no
    // change, before prepare or for one that is not a route through all the
    // channels.
    [[nodiscard]] bool setSnake(std::span<const int> snake) noexcept;

    // Prepare's rotations and route, every input on its own channel, frame
    // 0 next.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The input and output are interleaved, outputChannels() samples a
    // frame. False, with no change, before prepare, for spans of different
    // sizes or not whole frames, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const FrameRotate&) const = default;

private:
    using Route = std::array<int, maximumChannels>;

    [[nodiscard]] bool setup(double sampleRate, int channels, bool dual, double rotation, double secondRotation,
        double smear, int firstDirection, int secondFirstDirection, std::span<const int> snake);
    [[nodiscard]] bool route(std::span<const int> snake, Route& out) const noexcept;
    void advance(std::size_t which, int first) noexcept;

    double sampleRate_ = 0.0;
    std::size_t channels_ = 0;
    bool dual_ = false;
    double smear_ = 0.0;
    double smearcompensate_ = 1.0;
    std::size_t click_ = 0;
    std::array<double, 2> startRotation_{};
    std::array<int, 2> orient_{};
    Route startSnake_{};

    std::array<double, 2> rotation_{};
    std::array<double, 2> step_{};
    std::array<double, 2> position_{};
    Route snake_{};
    Route ochanLeft_{};
    Route ochanRight_{};
    Route ol_{};
    Route or_{};
    std::size_t clickPosition_ = 0;
};
}
