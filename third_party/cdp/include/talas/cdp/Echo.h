/*
 * Derived from the CDP System, Release 8: dev/modify/delay.c
 * Copyright (c) 1983-2013 Trevor Wishart, Richard Dobson and Composers
 * Desktop Project Ltd
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
// CDP `modify revecho` mode 1, standard delay: the prescaled input enters a
// delay line that feeds back on itself, and Mix sets how much of the delayed
// signal the output takes over the dry one. CDP divides the input by one more
// than Feedback and raises the dry path to match; Invert turns the dry path
// over, for phasing.
class Echo
{
public:
    static constexpr double maximumDelayMilliseconds = 10000.0;
    static constexpr double minimumPrescale = 1.0 / 32767.0;

    // False, with no change, for a sample rate that is not positive, a delay
    // under one sample or over 10 s, Mix outside 0 to 1, Feedback above 1 or
    // at or below -1 (CDP divides by one more than it), or a prescale
    // outside 1/32767 to 1. Live Mix and Feedback, which CDP takes as
    // breakpoint files, play as its domultiv: read every 128 frames, the dry
    // path neither raised nor inverted, and the input divided by one more
    // than the starting Feedback.
    [[nodiscard]] bool prepare(
        double sampleRate,
        double delayMilliseconds,
        double mix,
        double feedback,
        double prescale,
        bool invert,
        bool live = false);

    // domultiv's Mix and Feedback from the next 128-frame block. False, with
    // no change, unless prepared live, or for values outside prepare's
    // ranges.
    [[nodiscard]] bool setControls(double mix, double feedback) noexcept;

    // Silence in the line.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !line_.empty(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    std::vector<double> line_;
    bool live_ = false;
    std::size_t blockFrame_ = 0;
    double nextMix_ = 0.0;
    double nextFeedback_ = 0.0;
    double startMix_ = 0.0;
    double startFeedback_ = 0.0;
    std::size_t delayFrames_ = 0;
    std::size_t pointer_ = 0;
    double gain_ = 0.0;
    double feedback_ = 0.0;
    double feedForward_ = 0.0;
    double prescale_ = 0.0;
};
}
