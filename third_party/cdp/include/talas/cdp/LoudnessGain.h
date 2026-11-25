/*
 * Derived from the CDP System, Release 8: dev/modify/gain.c,
 * dev/cdp2k/tklib1.c and dev/cdp2k/dzsetup.c
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
// CDP `modify loudness` modes 1 and 2, GAIN and dBGAIN: each sample times a
// gain, given as a factor (Gain) or in decibels (Decibels), clipped to -1
// to 1 unless clipping is off, as CDP leaves a float output unclipped when
// the environment variable CDP_NOCLIP_FLOATS is set.
class LoudnessGain
{
public:
    enum class Mode
    {
        Gain = 1,
        Decibels,
    };

    static constexpr double maximumGain = 32767.0;
    static constexpr double minimumDecibels = -96.0;
    static constexpr double maximumDecibels = 96.0;

    // False, with no change, for a gain outside CDP's range: 0 to 32767 as
    // a factor, -96 to 96 dB in decibels. A fixed gain is also refused where
    // CDP refuses one that leaves silence: a factor at or below 0.000002
    // (FLTERR), or -96 dB, which CDP reads as a factor of 0. A live gain,
    // which CDP takes as a breakpoint file, may be 0 (or -96 dB) and follows
    // setGain.
    [[nodiscard]] bool prepare(Mode mode, double gain, bool clip = true, bool liveGain = false) noexcept;

    // The gain for the next process call: CDP reads its gain breakpoint file
    // at every frame (gainenv), so a block plays as CDP plays a file holding
    // this gain over it. Decibels convert as CDP converts each breakpoint
    // value (dbtogain). False, with no change, unless prepared with a live
    // gain, or for a gain outside prepare's range.
    [[nodiscard]] bool setGain(double gain) noexcept;

    // Back to the gain prepare gave.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const LoudnessGain&) const = default;

private:
    [[nodiscard]] bool valid(double gain) const noexcept;
    [[nodiscard]] double factor(double gain) const noexcept;

    bool prepared_ = false;
    Mode mode_ = Mode::Gain;
    bool clip_ = true;
    bool liveGain_ = false;
    // As factors.
    double startGain_ = 1.0;
    double gain_ = 1.0;
};
}
