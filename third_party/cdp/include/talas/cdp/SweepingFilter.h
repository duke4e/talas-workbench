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

#include <span>

namespace talas::cdp
{
// CDP `filter sweeping`: CDP's variable filter whose frequency sweeps on a
// raised cosine between a low and a high frequency, at the sweep frequency,
// from a phase where 0 starts at the low frequency rising and 0.5 at the high
// one falling. The frequency moves every 128 frames, and an output beyond ±1
// is clipped and lowers the gain by 0.9999 for good, as in CDP.
class SweepingFilter
{
public:
    enum class Mode
    {
        Notch = 1,
        BandPass,
        LowPass,
        HighPass,
    };

    static constexpr double minimumAcuity = 0.0001;
    static constexpr double minimumGain = 0.001;
    static constexpr double maximumGain = 10000.0;
    static constexpr double minimumFrequency = 0.1;
    static constexpr double maximumSweepFrequency = 200.0;

    // False, with no change, for a sample rate that is not positive, an
    // acuity outside 0.0001 to 1, a gain outside 0.001 to 10000, a low or
    // high frequency outside 0.1 Hz to half the sample rate, a sweep
    // frequency outside 0 to 200 Hz, or a phase outside 0 to 1.
    [[nodiscard]] bool prepare(
        double sampleRate, Mode mode, double acuity, double gain,
        double lowFrequency, double highFrequency, double sweepFrequency,
        double phase);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    // CDP takes the acuity, the low, high and sweep frequencies as breakpoint
    // files and reads them at the start of each 128-frame block: these take
    // effect at the next block. False, with no change, before prepare or for
    // values outside the ranges prepare takes.
    [[nodiscard]] bool setControls(
        double acuity, double lowFrequency, double highFrequency, double sweepFrequency) noexcept;

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    // The sweep's advance over one block at a sweep frequency.
    [[nodiscard]] double sweepStepFor(double sweepFrequency) const noexcept;

    bool prepared_ = false;
    Mode mode_ = Mode::Notch;
    // CDP's FLT_DLS, FLT_DBS, FLT_DHS and FLT_DNS for one channel.
    double dls_ = 0.0;
    double dbs_ = 0.0;
    double dhs_ = 0.0;
    double dns_ = 0.0;
    double acuity_ = 0.0;
    double startGain_ = 1.0;
    double gain_ = 1.0;
    double lowFrequency_ = 0.0;
    double highFrequency_ = 0.0;
    // The sweep's advance over one 128-frame block, in radians.
    double sweepStep_ = 0.0;
    // What the next block takes, and what prepare gave, which reset
    // returns to.
    double nextAcuity_ = 0.0;
    double nextLowFrequency_ = 0.0;
    double nextHighFrequency_ = 0.0;
    double nextSweepStep_ = 0.0;
    double startAcuity_ = 0.0;
    double startLowFrequency_ = 0.0;
    double startHighFrequency_ = 0.0;
    double startSweepStep_ = 0.0;
    double sampleRate_ = 0.0;
    double startPosition_ = 0.0;
    double position_ = 0.0;
    double inverseSampleRate_ = 0.0;
    double coefficient_ = 0.0;
    int blockCount_ = 0;
};
}
