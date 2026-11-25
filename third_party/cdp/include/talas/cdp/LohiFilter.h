/*
 * Derived from the CDP System, Release 8: dev/filter/filters0.c and
 * fltprepro.c
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
// CDP `filter lohi`: a low-pass filter when the stop band lies above the
// pass band, a high-pass one when it lies below. CDP designs a Butterworth
// cascade whose order meets the attenuation between the bands, but its loop
// feeds every section the input and keeps the last section's output, so the
// filter plays that one second-order section, then prescales and clips the
// result to -1 to 1.
class LohiFilter
{
public:
    static constexpr double minimumAttenuation = -96.0;
    static constexpr double minimumFrequency = 0.1;
    static constexpr double minimumPrescale = 1.0 / 200.0;
    static constexpr double maximumPrescale = 200.0;
    static constexpr int maximumSections = 200;

    // False, with no change, for a sample rate that is not positive, an
    // attenuation outside -96 to 0 dB, a pass or stop band outside 0.1 Hz
    // to half the sample rate, equal bands, or a prescale outside 1/200 to
    // 200.
    [[nodiscard]] bool prepare(
        double sampleRate, double attenuationDb, double passBand,
        double stopBand, double prescale);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    bool prepared_ = false;
    double multiplier_ = 0.0;
    double den1_ = 0.0;
    double den2_ = 0.0;
    double cn_ = 0.0;
    double prescale_ = 1.0;
    double s1_ = 0.0;
    double s2_ = 0.0;
    double e1_ = 0.0;
    double e2_ = 0.0;
};
}
