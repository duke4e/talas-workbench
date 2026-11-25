/*
 * Derived from the CDP System, Release 8: dev/filter/filters0.c and
 * fltpcon.c (after Andy Moorer's presence and shelving filter designs)
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
// CDP `filter fixed`: a second-order filter that boosts or cuts below a
// frequency (a low shelf), above it (a high shelf) or in a band around it,
// by Moorer's designs, on the prescaled input.
class FixedFilter
{
public:
    enum class Mode
    {
        BelowFrequency = 1,
        AboveFrequency,
        Band,
    };

    static constexpr double minimumBoost = -96.0;
    static constexpr double maximumBoost = 36.0;
    static constexpr double minimumFrequency = 0.1;
    static constexpr double minimumBandwidth = 0.001;
    static constexpr double minimumPrescale = 1.0 / 200.0;
    static constexpr double maximumPrescale = 200.0;

    // False, with no change, for a boost or cut outside -96 to 36 dB, a
    // frequency outside 0.1 Hz to half the sample rate, a bandwidth (for the
    // band) outside 0.001 Hz to half the sample rate, or a prescale outside
    // 1/200 to 200.
    [[nodiscard]] bool prepare(
        double sampleRate, Mode mode, double boostDb, double frequency,
        double bandwidth, double prescale);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    bool prepared_ = false;
    double a0_ = 0.0;
    double a1_ = 0.0;
    double a2_ = 0.0;
    double b1_ = 0.0;
    double b2_ = 0.0;
    double prescale_ = 1.0;
    double xx1_ = 0.0;
    double xx2_ = 0.0;
    double yy1_ = 0.0;
    double yy2_ = 0.0;
};
}
