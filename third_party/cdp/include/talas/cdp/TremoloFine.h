/*
 * Derived from the CDP System, Release 8: dev/new/tremolo.c and
 * dev/cdp2k/tklib3.c
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
// CDP `tremolo tremolo`: multiplies the input by a raised sine read from a
// 4096-point table, scaled by Depth, lifted by one less Depth and scaled by
// Gain, Fineness times over, each product stored as a float: the larger
// the fineness, the narrower the tremolo's peaks. Linear and Logarithmic
// are CDP's modes 1 and 2, which differ only in how a frequency breakpoint
// file is read.
class TremoloFine
{
public:
    enum class Mode
    {
        Linear,
        Logarithmic,
    };

    static constexpr std::size_t tableSize = 4096;
    static constexpr double maximumFrequency = 500.0;
    static constexpr int maximumFineness = 100;

    // False, with no change, for a sample rate that is not finite and
    // positive, a frequency outside 0 to 500 Hz, a depth or gain outside 0
    // to 1, or a fineness outside 1 to 100.
    // A live frequency, which CDP takes as a breakpoint file, is read in
    // Logarithmic mode as CDP reads that file: its log10, raised back to a
    // frequency (by __exp10, as CDP8's build calls pow(10.0, x)). CDP first
    // raises a value at or below one over its input file's duration to
    // that; a stream has no duration, so a live Logarithmic frequency at or
    // below zero is refused, and any above is read as CDP reads it from a
    // file longer than one cycle of that frequency. (CDP's modes differ
    // only there: a fixed frequency plays alike in both.)
    [[nodiscard]] bool prepare(
        double sampleRate, Mode mode, double frequency, double depth, double gain, int fineness,
        bool liveFrequency = false);

    // CDP takes the frequency, depth and gain as breakpoint files and reads
    // them at every frame: the frames of the next process call take these.
    // False, with no change, before prepare or for values outside
    // prepare's ranges.
    [[nodiscard]] bool setControls(double frequency, double depth, double gain) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return sampleRate_ > 0.0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const TremoloFine&) const = default;

private:
    [[nodiscard]] bool valid(double frequency, double depth, double gain) const noexcept;
    [[nodiscard]] double readFrequency(double frequency) const noexcept;

    double sampleRate_ = 0.0;
    double tabsizeOverSrate_ = 0.0;
    Mode mode_ = Mode::Linear;
    bool liveFrequency_ = false;
    int fineness_ = 1;
    double startFrequency_ = 0.0;
    double startDepth_ = 1.0;
    double startGain_ = 1.0;
    double frequency_ = 0.0;
    double depth_ = 1.0;
    double gain_ = 1.0;
    double fsinpos_ = 0.0;
};
}
