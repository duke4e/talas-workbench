/*
 * Derived from the CDP System, Release 8: dev/houskeep/clean.c,
 * dev/houskeep/ap_house.c and dev/cdp2k/tklib1.c
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
// CDP `housekeep deglitch`: a glitch, a burst of at most Glitch ms with at
// least Silence ms of quiet on both sides, is cut out: faded out over the
// Splice before it, zeroed, and faded back in over the Splice after it.
// Quiet means a mean magnitude over the last Window ms (fewer at the
// stream's start) at or under Threshold, and the sample itself not over
// it; a burst starts at the last sample over the threshold.
//
// CDP finds every glitch in its whole file, then cuts them. A glitch is
// known once the quiet after it has lasted Silence ms; the stream waits
// that long, after the longest glitch and the splice before it, so the
// output comes latencyFrames() late. CDP refuses a file with no glitch, a
// whole-file test a stream cannot make, so the stream plays on; at its
// file's end CDP cuts a glitch still under way to the end, which a stream
// has not. CDP stops with "Bad initial cut" for a glitch less than a splice
// after its file's start; the stream fades only the frames it has. CDP's
// -s, which lists the glitches, changes no sample.
class Deglitch
{
public:
    static constexpr double maximumGlitchMs = 1000.0;
    static constexpr double minimumSilenceMs = 0.3;
    static constexpr double maximumSilenceMs = 1000.0;
    static constexpr double maximumThreshold = 1.0;
    static constexpr double maximumSpliceMs = 50.0;
    static constexpr double maximumWindowMs = 4.0;

    // False, with no change, for a sample rate that is not finite and
    // positive, or values outside CDP's ranges: Glitch from one frame
    // (1000 / sampleRate ms) to 1000 ms, Silence 0.3 to 1000 ms, Threshold 0
    // to 1, Splice 0 to 50 ms, Window from one frame to 4 ms; nor a splice
    // longer than half the silence once each is rounded to frames (at least
    // one each), which CDP refuses too.
    [[nodiscard]] bool prepare(double sampleRate, double glitchMs, double silenceMs, double threshold,
        double spliceMs, double windowMs);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    // The longest glitch (or the window, if longer), the silence after it
    // less one frame, and the splice before it, in frames.
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return latency_; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const Deglitch&) const = default;

private:
    void search(std::size_t frame, float sample) noexcept;
    void cutOut(std::size_t frame, std::size_t startcut, std::size_t endcut) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    double level_ = 0.0;
    std::size_t dur_ = 1;
    std::size_t zeros_ = 1;
    std::size_t splicelen_ = 1;
    std::size_t filtlen_ = 1;
    double spliceincr_ = 1.0;
    std::size_t latency_ = 0;
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    // house_gate2's search: its window of recent samples, in the order it
    // sums them, and its counts and marks.
    std::vector<float> filtbuf_;
    std::size_t filtIndex_ = 0;
    std::size_t cnt_ = 0;
    int searchState_ = 0;
    std::size_t zerosgot_ = 0;
    std::size_t nonzerosgot_ = 0;
    std::size_t possibleStart_ = 0;
    std::size_t startcut_ = 0;
    std::size_t endcut_ = 0;
};
}
