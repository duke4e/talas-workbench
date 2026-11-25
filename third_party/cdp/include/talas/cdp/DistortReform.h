/*
 * Derived from the CDP System, Release 8: dev/distort/distort.c and
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

#include "talas/cdp/Drand48.h"
#include "talas/cdp/HalfCycleScanner.h"

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `distort reform`: each half-wavecycle, from one zero crossing to the
// next, takes a new shape with the old one's length and peak: a square at a
// fixed level or at the peak, a triangle at a fixed level or at the peak,
// the half-cycle turned over against its peak, a stream of clicks, a sine,
// or its contour exaggerated. Zeros before a half-cycle belong to it.
//
// CDP reads its whole file; the stream waits for each half-cycle to end, so
// the output comes latencyFrames late, and a half-cycle (with the zeros
// before it) longer than that is cut where CDP's would run on. CDP's
// buffers, whose ends change where a half-cycle starts and how clicks
// fall, are kept.
class DistortReform
{
public:
    enum class Mode
    {
        FixedSquare,
        Square,
        FixedTriangle,
        Triangle,
        Invert,
        Click,
        Sine,
        Exaggerate,
    };

    // The longest half-cycle the stream waits for; CDP's sine table holds
    // half-cycles under 2048 frames.
    static constexpr std::size_t latencyFrames = 2047;
    static constexpr double minimumExaggeration = 0.000002;
    static constexpr double maximumExaggeration = 40.0;

    // CDP8's sound buffers in its default 1 MB of memory: two of 131072
    // frames, or three of 87296 for Exaggerate.
    [[nodiscard]] static std::size_t defaultBufferFrames(Mode mode) noexcept;

    // False, with no change, for a sample rate that is not positive, an
    // exaggeration outside 0.000002 to 40, or buffers under twice the
    // latency. Zero buffer frames take CDP's default.
    [[nodiscard]] bool prepare(
        double sampleRate, Mode mode, double exaggeration,
        std::size_t bufferFrames = 0);

    // CDP takes Exaggerate's exaggeration as a breakpoint file and reads it
    // at each half-cycle's midpoint: a value given before a process call
    // reaches the half-cycles whose midpoint falls in its frames. (In the
    // buffer where its file ends, get_powfac counts from the buffer before,
    // which a stream that does not end never reaches.) False, with no
    // change, before prepare, outside Exaggerate, or outside 0.000002 to 40.
    [[nodiscard]] bool setExaggeration(double exaggeration) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    void reshape(const half_cycles::HalfCycle& halfCycle) noexcept;
    void genpulse(int step, float maxamp, float* bufptr, float* bufend) noexcept;
    void exaggerate(int lastzero, int thiszero, int phase, float* b) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    // The exaggeration from frame on, kept for the midpoints the latency
    // reaches back to.
    struct Exaggeration
    {
        std::size_t frame = 0;
        double value = 0.0;
    };
    [[nodiscard]] double exaggerationAt(double frame) const noexcept;

    Mode mode_ = Mode::FixedSquare;
    double powfac_ = 2.0;
    double startPowfac_ = 2.0;
    double nextPowfac_ = 2.0;
    std::vector<Exaggeration> exaggerations_;
    std::size_t exaggerationCount_ = 0;
    int pulse_ = 48;
    std::size_t bufferFrames_ = 0;
    // gen_distort_sin_table's 1024 entries, on the heap so that a
    // realtime operation stays small.
    std::vector<double> sine_;
    std::vector<float> ring_;
    std::vector<float> cycle_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    half_cycles::Scanner scanner_;
    // The first sample of the last half-cycle Exaggerate reshaped across
    // buffers, left at the start of CDP's work buffer.
    float crossingFirst_ = 0.0F;
    Drand48 random_;
};
}
