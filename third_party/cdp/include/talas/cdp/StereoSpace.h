/*
 * Derived from the CDP System, Release 8: dev/modify/pan.c and
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
// CDP `modify space` modes 2 and 4 on a stereo input:
// - Mode 2, MIRROR (prepareMirror, do_mirror): left and right swapped.
// - Mode 4, NARROW (prepareNarrow, do_narrow): with N the Narrowing (-1 to
//   1) and a = (N + 1) / 2, left' = a left + (1 - a) right and right' =
//   a right + (1 - a) left, each product rounded to double and summed (no
//   fused multiply-add, as CDP8's build keeps the separate statements),
//   then to float. A Narrowing within 0.000002 of -1 mirrors instead
//   (do_mirror), and one within 0.000002 of 0 mixes to mono (do_mono: the
//   float sum of both times 0.5 on each side). CDP refuses a Narrowing
//   within 0.000002 of 1 (no effect).
// Each frame on its own; no latency.
class StereoSpace
{
public:
    enum class Mode
    {
        Mirror,
        Narrow,
    };

    // ap_modify.c's range for Narrowing.
    static constexpr double minimumNarrowing = -1.0;
    static constexpr double maximumNarrowing = 1.0;

    [[nodiscard]] bool prepareMirror() noexcept;

    // False, with no change, for a Narrowing that is not finite, outside -1
    // to 1 (within 0.000002 of either end is that end, as CDP reads it), or
    // within 0.000002 of 1.
    [[nodiscard]] bool prepareNarrow(double narrowing) noexcept;

    // Nothing to clear.
    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] Mode mode() const noexcept { return mode_; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return 2; }

    // The input and output are interleaved stereo. False, with no change,
    // before prepare, for spans of different sizes or an odd size, or a
    // non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const StereoSpace&) const = default;

private:
    enum class Kind
    {
        Mirror,
        Narrow,
        Mono,
    };

    bool prepared_ = false;
    Mode mode_ = Mode::Mirror;
    Kind kind_ = Kind::Mirror;
    // narrow_sound's narrow and one_less_narrow: the other side's share and
    // the own side's.
    double other_ = 0.0;
    double own_ = 0.0;
};
}
