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

#include "talas/cdp/Drand48.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace talas::cdp
{
struct VaryingDelaySettings
{
    double delayMilliseconds = 20.0;
    double mix = 0.5;
    double feedback = 0.0;
    // How far the oscillator sweeps the delay, 0 to 1.
    double modulation = 0.0;
    // Hz; a negative frequency gives CDP's random oscillation.
    double lfoFrequency = 0.0;
    // 0 to 1, where the sweep starts.
    double lfoPhase = 0.0;
    // Seconds before the sweep begins.
    double lfoDelay = 0.0;
    double prescale = 1.0;
    // 1 to 256, seeding the random oscillation. CDP8 reads an unseeded
    // rand() whatever its seed, which seed 1 reproduces.
    int seed = 1;
};

// CDP `modify revecho` mode 2, varying delay: a delay line with feedback
// whose read point a sine oscillator, or CDP's 1/f random walk for a
// negative frequency, sweeps through the line, interpolated linearly.
// CDP8 never adds the dry input in this mode (it leaves the dry path's
// factor at zero), and the port keeps that.
class VaryingDelay
{
public:
    static constexpr double maximumDelayMilliseconds = 10000.0;
    static constexpr double maximumLfoFrequency = 50.0;
    static constexpr double maximumLfoDelaySeconds = 3600.0;

    // False, with no change, for values outside CDP's ranges, Feedback of
    // -1, a delay under one sample, or a seed outside 1 to 256 (CDP takes
    // its seed from the clock for 0).
    [[nodiscard]] bool prepare(double sampleRate, const VaryingDelaySettings& settings);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !line_.empty(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    // delay.c's SOSCIL.
    struct Oscillator
    {
        double phase = 0.0;
        double increment = 0.0;
        double value = 0.0;
        unsigned int count = 0;
        unsigned int delay = 0;
    };

    std::vector<double> line_;
    int frameDelay_ = 0;
    int inputPointer_ = 0;
    int outputPointer_ = 0;
    bool sineOscillator_ = true;
    double modulationLength_ = 0.0;
    double modulationRange_ = 0.0;
    double lfoPhase_ = 0.0;
    int lfoDelayFrames_ = 0;
    double gain_ = 0.0;
    double feedback_ = 0.0;
    double feedForward_ = 0.0;
    double prescale_ = 0.0;
    Oscillator startOscillator_;
    Oscillator oscillator_;
    Drand48 startRandom_;
    Drand48 random_;
    int randomIndex_ = 0;
    int randomCounter_ = 0;
    double randomOut_ = 0.0;
    double randomDifference_ = 0.0;
};
}
