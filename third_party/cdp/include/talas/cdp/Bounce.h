/*
 * Derived from the CDP System, Release 8: dev/science/bounce.c and
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
#include <vector>

namespace talas::cdp
{
// CDP `bounce bounce ... -s0`: the input followed by Count copies of
// itself, closer and closer together and quieter and quieter, like a
// bouncing ball. Copy 1 starts Startgap seconds in; each gap after is the
// last times Shorten (each rounded to frames, lround(gap * sr)); copy k's
// level is (1 - Endlevel) ((Count - k) / Count)^Ewarp + Endlevel. CDP stops
// at the first gap under half its 3 ms splice (72 frames at 48 kHz), so
// fewer copies may play (bounces()). Each copy is added to the output in
// turn, rounded to float (a fused multiply-add in CDP8's build).
//
// CDP's -s (shrinking each copy in proportion to its gap, by trimming its
// end, or with -e its start), on by default with a minimum of 0.02 s, and
// -c (cutting copies that overlap), trim copies by the input's duration:
// the stream plays CDP's -s0 without -c, whole copies. No latency; the
// copies past the stream's start ring on as it goes on (CDP's output runs
// past its input by the copies' tail).
//
// When its output would peak above 0.95, CDP fades the whole output down
// from the first copy's start to the peak's position, coming back up after
// it (normalisation_fade). A stream cannot know its peak in advance: it
// matches CDP bit for bit whenever CDP's first pass finds a peak at or
// below 0.95, and otherwise plays unscaled, clipped at -1 and 1.
//
// Mono: CDP bounces each channel of a multichannel file alike, so one
// processor per channel plays it.
class Bounce
{
public:
    // bounce.c's ranges.
    static constexpr int minimumCount = 1;
    static constexpr int maximumCount = 100;
    static constexpr double minimumStartGap = 0.04;
    static constexpr double maximumStartGap = 10.0;
    static constexpr double minimumShorten = 0.1;
    static constexpr double maximumShorten = 1.0;
    static constexpr double minimumEndLevel = 0.0;
    static constexpr double maximumEndLevel = 1.0;
    static constexpr double minimumWarp = 0.01;
    static constexpr double maximumWarp = 100.0;
    // The longest delay the stream keeps, the last copy's start (CDP: up
    // to 1000 s).
    static constexpr std::size_t maximumDelayFrames = std::size_t{1} << 22;

    // False, with no change, for a sample rate that is not finite and
    // positive, values outside the ranges (within 0.000002 of a range end is
    // that end, as CDP's readdata takes it), or a last copy starting past
    // maximumDelayFrames.
    [[nodiscard]] bool prepare(
        double sampleRate, int count, double startGap, double shorten, double endLevel, double warp);

    // Silence in the line.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !line_.empty(); }

    [[nodiscard]] static constexpr std::size_t latencyFrames() noexcept { return 0; }

    // The copies CDP plays: Count, or fewer where the gaps get too short.
    [[nodiscard]] std::size_t bounces() const noexcept { return delays_.size(); }

    // Copy k's (from 0) start in frames and level.
    [[nodiscard]] std::size_t delayFrames(std::size_t k) const noexcept { return delays_[k]; }
    [[nodiscard]] double level(std::size_t k) const noexcept { return levels_[k]; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const Bounce&) const = default;

private:
    std::vector<std::size_t> delays_;
    std::vector<double> levels_;
    std::vector<float> line_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
};
}
