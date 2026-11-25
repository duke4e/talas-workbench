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
#include <optional>
#include <span>

namespace talas::cdp
{
// CDP `modify radical` mode 5, RING MODULATE: multiplies the input by a sine
// read from CDP's 512-point table with linear interpolation. The modulating
// frequency is read every 128 frames and its logarithm ramps towards each
// new reading over the following block.
class RingModulate
{
public:
    static constexpr std::size_t tableSize = 512;
    static constexpr std::size_t controlBlockFrames = 128;
    static constexpr double minimumFrequency = 0.1;

    // False, leaving the processor unprepared, for a sample rate that is not
    // finite and positive.
    [[nodiscard]] bool prepare(double sampleRate) noexcept;
    [[nodiscard]] bool prepared() const noexcept;
    void reset() noexcept;

    // The frequency at each frame is the connected value or else the
    // fallback. False, with no change, when an input sample is not finite, a
    // frequency read falls outside 0.1 Hz to half the sample rate, or the
    // spans differ in size.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::optional<std::span<const double>> connectedFrequency,
        double fallbackFrequency,
        std::span<double> output) noexcept;

    bool operator==(const RingModulate&) const = default;

private:
    [[nodiscard]] bool validFrequency(double frequency) const noexcept;

    double sampleRate_ = 0.0;
    double tableIndex_ = 0.0;
    double pitch_ = 0.0;
    double nextPitch_ = 0.0;
    double pitchStep_ = 0.0;
    std::size_t framesLeftInBlock_ = 0;
    bool started_ = false;
};
}
