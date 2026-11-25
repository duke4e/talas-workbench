/*
 * Derived from the CDP System, Release 8: dev/distort/distortr.c,
 * distorte.c and distortion.c
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
#include <cstdint>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `distort reverse`: each group of Cycles wavecycles plays backwards. A
// wavecycle runs from where the signal turns the way its first sample went
// to where it does so again.
//
// CDP reads its whole file; the stream waits for each group to end, so the
// output comes latencyFrames late, and a longer group is cut where CDP's
// would run on.
class DistortReverse
{
public:
    // The longest group the stream waits for, as Distort Reform waits for a
    // half-cycle.
    static constexpr std::size_t latencyFrames = 2047;
    // MAX_CYCLECNT.
    static constexpr int maximumCycles = 32767;

    // False, with no change, for a cycle count outside 1 to 32767.
    [[nodiscard]] bool prepare(int cycles);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

    // CDP takes Cycles as a breakpoint file and reads it, rounded, where each
    // group starts: the frames of the next process call take this. False,
    // with no change, before prepare or for a value that does not round to 1
    // to 32767.
    [[nodiscard]] bool setCycles(double cycles) noexcept;

private:
    void reverseGroup(std::size_t end) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    int cycles_ = 1;
    // The cycles at each frame of the ring, those the next frames take, and
    // those of the group under way.
    std::vector<std::uint16_t> counts_;
    int nextCycles_ = 1;
    int groupCycles_ = 1;
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    std::size_t start_ = 0;
    // get_initial_phase: 0 until the first sample that is not zero.
    int phase_ = 0;
    bool secondHalf_ = false;
    int done_ = 0;
};
}
