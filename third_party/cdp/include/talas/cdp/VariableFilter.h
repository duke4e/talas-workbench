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
// CDP `filter variable`: a state-variable filter with notch, band-pass,
// low-pass and high-pass outputs, whose coefficient follows the frequency
// every 128 frames and whose acuity sets its damping. An output beyond ±1 is
// clipped and lowers the gain by 0.9999 for good, as CDP does to keep the
// filter from blowing up.
class VariableFilter
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

    // False, with no change, for an acuity outside 0.0001 to 1, a gain
    // outside 0.001 to 10000, or a frequency outside 0.1 Hz to half the
    // sample rate.
    [[nodiscard]] bool prepare(
        double sampleRate, Mode mode, double acuity, double gain, double frequency);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    // CDP takes the acuity and frequency as breakpoint files and reads them
    // at the start of each 128-frame block: these take effect at the next
    // block. False, with no change, before prepare or for an acuity outside
    // 0.0001 to 1 or a frequency outside 0.1 Hz to half the sample rate.
    [[nodiscard]] bool setControls(double acuity, double frequency) noexcept;

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    bool prepared_ = false;
    Mode mode_ = Mode::Notch;
    // CDP's FLT_DLS, FLT_DBS, FLT_DHS and FLT_DNS for one channel.
    double dls_ = 0.0;
    double dbs_ = 0.0;
    double dhs_ = 0.0;
    double dns_ = 0.0;
    double acuity_ = 0.0;
    // The acuity and frequency the next block takes, and the ones prepare
    // gave, which reset returns to.
    double nextAcuity_ = 0.0;
    double nextFrequency_ = 0.0;
    double startAcuity_ = 0.0;
    double startFrequency_ = 0.0;
    double sampleRate_ = 0.0;
    double startGain_ = 1.0;
    double gain_ = 1.0;
    double frequency_ = 0.0;
    double inverseSampleRate_ = 0.0;
    double coefficient_ = 0.0;
    int blockCount_ = 0;
};
}
