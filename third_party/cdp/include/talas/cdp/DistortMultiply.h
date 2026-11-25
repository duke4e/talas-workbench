/*
 * Derived from the CDP System, Release 8: dev/distort/distortm.c and
 * distortion.c
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

#include "talas/cdp/HalfCycleScanner.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `distort multiply`: each half-wavecycle plays Factor times faster,
// Factor times over, alternately turned over, so its frequency multiplies;
// leftover samples pad halved copies. With an odd Factor the output's
// phase turns at each half-cycle.
//
// CDP reads its whole file; the stream waits for each half-cycle to end, so
// the output comes latencyFrames late, and a longer half-cycle (with the
// zeros before it) is cut where CDP's would run on. CDP's buffer ends,
// which change where a half-cycle starts, are kept.
class DistortMultiply
{
public:
    // The longest half-cycle the stream waits for, as Distort Reform waits.
    static constexpr std::size_t latencyFrames = 2047;
    static constexpr int minimumFactor = 2;
    static constexpr int maximumFactor = 16;
    // CDP8's six buffers in its default 1 MB of memory.
    static constexpr std::size_t defaultBufferFrames = half_cycles::defaultBufferFrames(6);

    // False, with no change, for a factor outside 2 to 16 or buffers under
    // twice the latency.
    [[nodiscard]] bool prepare(int factor, std::size_t bufferFrames = defaultBufferFrames);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    // CDP takes the factor as a breakpoint file and reads it, rounded, where
    // each half-cycle starts: the frames of the next process call take this
    // factor. False, with no change, before prepare or for a factor that
    // does not round to 2 to 16.
    [[nodiscard]] bool setFactor(double factor) noexcept;

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    void multiply(const half_cycles::HalfCycle& halfCycle) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    int factor_ = 2;
    // The factor at each frame of the ring, and the one the next frames
    // take.
    std::vector<std::uint8_t> factors_;
    int nextFactor_ = 2;
    std::vector<float> ring_;
    std::vector<float> in_;
    std::vector<float> out_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    int outputPhase_ = 1;
    half_cycles::Scanner scanner_;
};

// CDP `distort divide`: of each Factor half-wavecycles, the first is
// stretched over all of them, so the frequency divides; Interpolate reads
// it between samples. The output's phase turns at each group.
//
// CDP reads its whole file; the stream waits for each group to end, so the
// output comes latencyFrames late, and a longer group is cut where CDP's
// would run on. CDP's buffer ends are kept.
class DistortDivide
{
public:
    // The longest group the stream waits for.
    static constexpr std::size_t latencyFrames = 2047;
    static constexpr int minimumFactor = 2;
    static constexpr int maximumFactor = 16;
    // CDP8's four buffers in its default 1 MB of memory.
    static constexpr std::size_t defaultBufferFrames = half_cycles::defaultBufferFrames(4);

    // False, with no change, for a factor outside 2 to 16 or buffers under
    // twice the latency.
    [[nodiscard]] bool prepare(
        int factor, bool interpolate, std::size_t bufferFrames = defaultBufferFrames);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    // CDP takes the factor as a breakpoint file and reads it, rounded, where
    // each group starts: the frames of the next process call take this
    // factor. False, with no change, before prepare or for a factor that
    // does not round to 2 to 16.
    [[nodiscard]] bool setFactor(double factor) noexcept;

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    bool addHalfCycle(const half_cycles::HalfCycle& halfCycle) noexcept;
    void divide(std::size_t end) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    int factor_ = 2;
    std::vector<std::uint8_t> factors_;
    int nextFactor_ = 2;
    // The factor of the group under way.
    int groupFactor_ = 2;
    bool interpolate_ = false;
    std::vector<float> ring_;
    std::vector<float> in_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    int outputPhase_ = 1;
    int halfCycles_ = 0;
    bool cutting_ = false;
    std::size_t startIndex_ = 0;
    std::size_t startMarker_ = 0;
    std::size_t endIndex_ = 0;
    half_cycles::Scanner scanner_;
};
}
