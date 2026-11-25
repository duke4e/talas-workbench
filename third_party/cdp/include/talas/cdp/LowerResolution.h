/*
 * Derived from the CDP System, Release 8: dev/modify/radical.c
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
// CDP `modify radical` mode 4, LOWER RESOLUTION: each group of
// sampleRateDivision samples becomes its mean, quantized to bitResolution
// bits as CDP quantizes a 32-bit float file. Like CDP, 16 bits quantizes in
// steps of 16 sixteen-bit levels, the same as 12 bits.
class LowerResolution
{
public:
    static constexpr int maximumBitResolution = 16;
    static constexpr int maximumSampleRateDivision = 256;

    // False, leaving the processor unprepared, outside CDP's ranges of 1 to
    // 16 bits and a division of 1 to 256.
    [[nodiscard]] bool prepare(int bitResolution, int sampleRateDivision) noexcept;
    [[nodiscard]] bool prepared() const noexcept;
    void reset() noexcept;
    // Changes the resolution for later groups; false, with no change, outside
    // 1 to 16 bits or before prepare.
    [[nodiscard]] bool setBitResolution(int bitResolution) noexcept;

    // A group's mean is output from the frame that completes the group, so
    // the output lags CDP's by latencyFrames(). False, with no change, when an
    // input sample is not finite or the spans differ in size.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

    [[nodiscard]] std::size_t latencyFrames() const noexcept;

    bool operator==(const LowerResolution&) const = default;

private:
    [[nodiscard]] double quantize(double mean) const noexcept;

    double step_ = 0.0;
    int division_ = 0;
    int count_ = 0;
    double sum_ = 0.0;
    double held_ = 0.0;
};
}
