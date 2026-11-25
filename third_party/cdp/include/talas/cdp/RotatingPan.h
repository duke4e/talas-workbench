/*
 * Derived from the CDP System, Release 8: dev/standalone/mchanpan.c and
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
// CDP `mchanpan mchanpan 9` (mchanpan2): a mono input rotating around a
// ring of output channels, Speed turns a second, clockwise (rising channel
// numbers) or anticlockwise. The position starts at Start channel (1 to N;
// 0 to 1 lies between channel N and channel 1) and moves on before each
// frame plays. Each frame pans equal-power (CDP's hole-in-the-middle
// compensation) between the two channels either side of the position;
// Focus below 1 gives (1 - Focus) / 2 of the signal to the channels beyond
// those two. CDP sums the four contributions into float in the order far
// left, left, right, far right (they meet on one channel with 3 outputs).
//
// CDP reads its Speed breakpoint file every 256 frames from the start; the
// stream takes the Speed set when such a frame (counted from prepare or
// reset) comes in. No latency.
class RotatingPan
{
public:
    // mchanpan.c's ranges for mode 9.
    static constexpr int minimumChannels = 3;
    static constexpr int maximumChannels = 16;
    static constexpr double maximumSpeed = 64.0;
    // The frames between CDP's Speed reads.
    static constexpr std::size_t speedReadFrames = 256;

    // False, with no change, for a sample rate that is not finite or below
    // maximumSpeed (CDP's position wraps once a frame), channels outside 3
    // to 16, a start channel outside 0 to the channels, a speed outside 0 to
    // 64 or a focus outside 0 to 1 (within 0.000002 of a range end is that
    // end, as CDP's readdata takes it).
    [[nodiscard]] bool prepare(double sampleRate, int outputChannels, double startChannel, double speed,
        double focus = 1.0, bool anticlockwise = false);

    // Speed (turns a second) from the next frame CDP reads it on. False,
    // with no change, before prepare or outside 0 to 64.
    [[nodiscard]] bool setSpeed(double speed) noexcept;

    // The start position, prepare's speed, frame 0 next.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The output is interleaved, outputChannels() samples a frame. False,
    // with no change, before prepare, for an output that is not
    // outputChannels() times the input, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const RotatingPan&) const = default;

private:
    double sampleRate_ = 0.0;
    std::size_t channels_ = 0;
    double start_ = 0.0;
    double startSpeed_ = 0.0;
    double focus_ = 1.0;
    double scalefact_ = 0.0;
    bool anticlockwise_ = false;
    double speed_ = 0.0;
    double increment_ = 0.0;
    double position_ = 0.0;
    std::size_t frame_ = 0;
};
}
