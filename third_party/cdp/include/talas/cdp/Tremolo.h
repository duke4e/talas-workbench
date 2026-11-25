/*
 * Derived from the CDP System, Release 8: dev/env/envfuncs.c, dev/env/envprepro.c
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
// CDP `envel tremolo`: multiplies the input by a raised sine read from a
// 4096-point table, scaled by Depth, lifted by one less Depth, and scaled by
// Gain. Frequency, Depth and Gain hold for each processed block.
class Tremolo
{
public:
    static constexpr std::size_t tableSize = 4096;
    static constexpr double maximumFrequency = 500.0;

    // False, leaving the processor unprepared, for a sample rate that is not
    // finite and positive.
    [[nodiscard]] bool prepare(double sampleRate) noexcept;
    [[nodiscard]] bool prepared() const noexcept;
    void reset() noexcept;

    // False, with no change, for a frequency outside 0 to 500 Hz, a depth or
    // gain outside 0 to 1, a non-finite input, or spans of different sizes.
    [[nodiscard]] bool process(
        std::span<const double> input,
        double frequency,
        double depth,
        double gain,
        std::span<double> output) noexcept;

    bool operator==(const Tremolo&) const = default;

private:
    double sampleRate_ = 0.0;
    double tablePosition_ = 0.0;
};
}
