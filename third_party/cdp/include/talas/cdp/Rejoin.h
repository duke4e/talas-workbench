/*
 * Derived from the CDP System, Release 8: dev/new/rejoin.c,
 * dev/cdp2k/tklib1.c and dev/cdp2k/readdata.c
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
// CDP `rejoin rejoin` (mode 1 or 2, without -r) of two mono files: their
// sum, each times Gain. CDP starts from its longest file (the first, of
// files as long), times Gain rounded to float, and adds each other file
// times Gain in turn, rounded to float (a fused multiply-add in CDP8's
// build): the input is that longest file, the side the other. Mode 2's
// remnant file mixes alike without -r.
//
// CDP runs the mix once to find its peak and, if that passes 0.95, scales
// the whole output to peak at 1. A stream cannot know its peak in advance:
// it matches CDP bit for bit whenever CDP's first pass finds a peak at or
// below 0.95, and otherwise plays unscaled, clipped at -1 and 1. CDP's -r
// reads the segment files backwards from their ends, aligned at the
// longest file's end: not streamable. CDP stops at its longest file's end;
// the stream plays both on. No latency.
class Rejoin
{
public:
    // rejoin.c's range for Gain, 0 to 1; CDP refuses a Gain within
    // 0.000002 of 0.
    static constexpr double maximumGain = 1.0;

    // False, with no change, for a Gain outside 0 to 1 (within 0.000002 of 1
    // is 1) or within 0.000002 of 0.
    [[nodiscard]] bool prepare(double gain = 1.0) noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return gain_ > 0.0; }

    [[nodiscard]] static constexpr std::size_t latencyFrames() noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

    bool operator==(const Rejoin&) const = default;

private:
    double gain_ = 0.0;
};
}
