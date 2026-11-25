/*
 * Derived from the CDP System, Release 8: dev/spec/simple.c and
 * dev/cdp2k/tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#pragma once

#include "talas/cdp/PhaseVocoder.h"

#include <cstddef>
#include <span>

namespace talas::cdp
{
// CDP `spec gain` and `spec gate` on each frame between `pvoc anal` and
// `pvoc synth`: the gain scales every channel's amplitude, and the gate
// silences a channel whose amplitude is under its threshold. CDP takes the
// gain and the threshold as breakpoint files and reads them at each
// analysis frame's time: a value given before a process call reaches the
// frames analysed during it.
class SpectralGain
{
public:
    static constexpr double maximumGain = 10000.0;

    // False, with no change, for a gain outside 0 to 10000 or what the
    // phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double gain);

    // False, with no change, before prepare or outside 0 to 10000.
    [[nodiscard]] bool setGain(double gain) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    double gain_ = 1.0;
    double startGain_ = 1.0;
};

class SpectralGate
{
public:
    // False, with no change, for a threshold outside 0 to 1 or what the
    // phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double threshold);

    // False, with no change, before prepare or outside 0 to 1.
    [[nodiscard]] bool setThreshold(double threshold) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    double threshold_ = 0.0;
    double startThreshold_ = 0.0;
};
}
