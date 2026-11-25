/*
 * Derived from the CDP System, Release 8: dev/standnew/distmore.c, clip.c
 * and quirk.c
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
#include <vector>

namespace talas::cdp
{
// CDP `distmore double` with a multiplier of 1: each waveset plays at twice
// its frequency, as every other sample of it played twice over, in its own
// length. A waveset runs from where the signal turns the way its first
// sample went to where it does so again, zeros counting on both sides.
//
// CDP reads its whole file; the stream waits for each waveset to end, so
// the output comes latencyFrames() late, and a longer waveset is cut where
// CDP's would run on. CDP drops the partial waveset at its file's end,
// which a stream, having no end, plays when it is cut.
class DistmoreDouble
{
public:
    // The longest waveset the stream waits for, as Distort Harmonic waits
    // for a cycle.
    static constexpr std::size_t waitFrames = 2047;

    // False, with no change, for any multiplier but 1: CDP's 2 to 4 (up to
    // four octaves) give each waveset a new length, which a stream at a
    // fixed latency cannot follow.
    [[nodiscard]] bool prepare(int multiplier = 1);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return waitFrames; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void doubleWaveset(std::size_t end) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    std::size_t start_ = 0;
    // distdouble's lastphase: 0 until the first sample that is not zero.
    int phase_ = 0;
    bool secondHalf_ = false;
};

// CDP `clip clip` mode 2: each half-waveset is clipped at Fraction of its
// own peak, and the whole signal raised by one over Fraction. A
// half-waveset runs from the first sample on one side of zero to the first
// on the other side, zeros keeping to the side before them.
//
// CDP reads its whole file; the stream waits for each half-waveset to end,
// so the output comes latencyFrames() late, and a longer half-waveset is
// cut where CDP's would run on, each part clipped at its own peak. CDP
// leaves the half-waveset at its file's end unclipped, which a stream,
// having no end, clips when it is cut.
class ClipWaveset
{
public:
    // The longest half-waveset the stream waits for.
    static constexpr std::size_t waitFrames = 2047;

    // False, with no change, for a fraction that is not above 0 and at
    // most 1.
    [[nodiscard]] bool prepare(double fraction);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return waitFrames; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void clipHalfWaveset(std::size_t end) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    double fraction_ = 0.5;
    double gain_ = 2.0;
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    std::size_t start_ = 0;
    // clip's phase, 0 until the first sample that is not zero, and the
    // half-waveset's peaks.
    int phase_ = 0;
    float maxsamp_ = 0.0F;
    float minsamp_ = 0.0F;
};

// CDP `quirk quirk` mode 1: each half-waveset's samples, over its peak,
// raised to the power of Exponent and scaled back to the peak: an exponent
// under 1 exaggerates its contour, one over 1 smooths it. A half-waveset
// runs while the samples keep to one side, zero counting as positive.
//
// CDP reads its whole file; the stream waits for each half-waveset to end,
// so the output comes latencyFrames() late, and a longer half-waveset is
// cut where CDP's would run on, each part scaled to its own peak. CDP
// drops the partial half-waveset at its file's end, which a stream, having
// no end, plays when it is cut.
class QuirkWaveset
{
public:
    // The longest half-waveset the stream waits for.
    static constexpr std::size_t waitFrames = 2047;
    static constexpr double minimumExponent = 0.01;
    static constexpr double maximumExponent = 100.0;

    // False, with no change, for an exponent outside 0.01 to 100.
    [[nodiscard]] bool prepare(double exponent);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return waitFrames; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void powerHalfWaveset(std::size_t end) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    double powfac_ = 1.0;
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    std::size_t start_ = 0;
    // The side of the half-waveset under way, and its peak.
    bool positive_ = true;
    float maxsamp_ = 0.0F;
};
}
