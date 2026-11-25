/*
 * Derived from the CDP System, Release 8: dev/filter/filters0.c and
 * fltpcon.c
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
// CDP `filter phasing`: an allpass filter over a delay of whole frames that
// shifts the phase of the prescaled input or, as a phaser, averages it with
// the input itself, so that a gain of 1 cancels the phaser's output.
class Phasing
{
public:
    enum class Mode
    {
        PhaseShift = 1,
        Phaser,
    };

    static constexpr double maximumDelayMilliseconds = 10000.0;

    // False, with no change, for a sample rate that is not positive, a gain
    // or prescale outside -1 to 1, or a delay under one sample or over 10 s.
    // A live delay, which CDP takes as a breakpoint file, keeps lines for
    // the longest delay and follows setDelay.
    [[nodiscard]] bool prepare(
        double sampleRate, Mode mode, double gain, double delayMilliseconds, double prescale,
        bool liveDelay = false);

    // varidelay_allpass's delay from the next frame: CDP reads its delay
    // breakpoints at each frame, rounded to whole frames and at least one.
    // False, with no change, unless prepared with a live delay, or for a
    // delay outside one sample to 10 s.
    [[nodiscard]] bool setDelay(double delayMilliseconds) noexcept;

    // Silence in both lines.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !first_.empty(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    [[nodiscard]] double delayFrames(double delayMilliseconds) const noexcept;

    Mode mode_ = Mode::PhaseShift;
    bool liveDelay_ = false;
    double sampleRate_ = 0.0;
    // A live delay in frames, and the one prepare gave.
    double delay_ = 1.0;
    double startDelay_ = 1.0;
    std::vector<double> first_;
    std::vector<double> second_;
    std::size_t position_ = 0;
    double gain_ = 0.0;
    double prescale_ = 1.0;
};
}
