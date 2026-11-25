/*
 * Derived from the CDP System, Release 8: dev/distort/distort.c and
 * disprepro.c, and dev/sfsys/osbind.c
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

#include <span>

namespace talas::cdp
{
// CDP `distort overload`: samples at or beyond the clip level are held at
// it, each scaled by a random amount up to Depth below it (noise) or by a
// raised cosine at the waveform frequency that restarts at each clipped
// stretch (waveform), and the whole output is scaled by 1 over one more than
// the clip level.
class DistortOverload
{
public:
    enum class Mode
    {
        Noise = 1,
        Waveform,
    };

    static constexpr double minimumFrequency = 0.01;

    // False, with no change, for a sample rate that is not positive, a clip
    // level or depth outside 0 to 1, a waveform frequency outside 0.01 Hz to
    // half the sample rate, or a seed outside 1 to 256. CDP8 reads an
    // unseeded rand(), which seed 1 reproduces.
    [[nodiscard]] bool prepare(
        double sampleRate, Mode mode, double clipLevel, double depth,
        double frequency, int seed);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    // CDP takes Depth and the waveform frequency as breakpoint files and
    // reads them every 0.01 s of the stream: these take effect at its next
    // read. (It reads the clip level once, when it starts, though it takes
    // a breakpoint file.) False, with no change, before prepare or for
    // values outside prepare's ranges.
    [[nodiscard]] bool setControls(double depth, double frequency) noexcept;

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    bool prepared_ = false;
    Mode mode_ = Mode::Noise;
    double maxgate_ = 0.0;
    double depth_ = 0.0;
    double frequency_ = 0.0;
    // distort_overload's stepper, the frames to its next read, and what
    // that read takes and prepare gave.
    int stepper_ = 1;
    int toRead_ = 0;
    double nextDepth_ = 0.0;
    double nextFrequency_ = 0.0;
    double startDepth_ = 0.0;
    double startFrequency_ = 0.0;
    double sampleRate_ = 0.0;
    double incRatio_ = 0.0;
    double normaliser_ = 1.0;
    Drand48 startRandom_;
    Drand48 random_;
    int tabpos_ = 0;
    double tabposd_ = 0.0;
    bool inMaxgate_ = false;
    bool inMingate_ = false;
};
}
