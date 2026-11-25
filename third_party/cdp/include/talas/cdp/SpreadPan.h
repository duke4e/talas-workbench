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

#include <array>
#include <cstddef>
#include <span>

namespace talas::cdp
{
// CDP `mchanpan mchanpan 4` (panspread): a mono or stereo input spread
// from a Centre channel over Spread channels of a ring of N outputs. The
// channels wholly within the spread play at a level that Rolloff lowers as
// the spread widens (1 - Rolloff (1 - 1 / Spread)); the two channels at its
// edges take the fractional part, equal-power. When Depth (the channels
// used to each side, behind the spread's edges) is less than half the
// spread, a hole opens in the middle, the channels in it fading out as it
// widens. A stereo input sends its left to the channels left of the centre
// pair and its right to those right of it (outputs/2 - 1 each side; with an
// odd count the channel opposite gets nothing), and a mix of the two to the
// centre pair.
//
// CDP's quirk is kept: when Spread reaches N and the hole is closed, CDP
// leaves the stereo centre where the last levels left it (0, before any,
// so channels 1 and 2 are the centre pair).
//
// CDP reads Spread, Depth and Rolloff every 256 frames from the start; the
// stream takes the values set when such a frame (counted from prepare or
// reset) comes in. No latency.
class SpreadPan
{
public:
    // mchanpan.c's ranges for mode 4.
    static constexpr int minimumChannels = 3;
    static constexpr int maximumChannels = 16;
    static constexpr int maximumCentre = 8;
    static constexpr double maximumSpread = 16.0;
    static constexpr double maximumDepth = 8.0;
    // The frames between CDP's reads.
    static constexpr std::size_t readFrames = 256;

    // False, with no change, for a sample rate that is not finite and
    // positive, 0 or more than 2 input channels, output channels outside 3
    // to 16, a centre outside 0 to 8 or past the output channels (0 is
    // channel N), or a spread, depth or rolloff outside 0 to 16, 0 to 8 and
    // 0 to 1 (within 0.000002 of a range end is that end).
    [[nodiscard]] bool prepare(double sampleRate, int inputChannels, int outputChannels, int centre, double spread,
        double depth, double rolloff);

    // From the next frame CDP reads them on. False, with no change, before
    // prepare or outside the ranges.
    [[nodiscard]] bool setSpread(double spread) noexcept;
    [[nodiscard]] bool setDepth(double depth) noexcept;
    [[nodiscard]] bool setRolloff(double rolloff) noexcept;

    // Prepare's controls, frame 0 next.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return outChannels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t inputChannels() const noexcept { return inChannels_; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return outChannels_; }

    // The input is interleaved, inputChannels() samples a frame; the output
    // too, outputChannels() a frame. False, with no change, before prepare,
    // for spans whose frames differ, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const SpreadPan&) const = default;

private:
    void spreadPan() noexcept;

    std::size_t inChannels_ = 0;
    std::size_t outChannels_ = 0;
    int centreParam_ = 0;
    double startSpread_ = 0.0;
    double startDepth_ = 0.0;
    double startRolloff_ = 0.0;
    double spread_ = 0.0;
    double depth_ = 0.0;
    double rolloff_ = 0.0;
    double centre_ = 0.0;
    std::array<double, maximumChannels> levels_{};
    std::size_t frame_ = 0;
};
}
