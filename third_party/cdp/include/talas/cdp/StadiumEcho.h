/*
 * Derived from the CDP System, Release 8: dev/modify/delay.c
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
#include <cstdint>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `modify revecho` mode 3, stadium echo: a mono input heard again
// through CDP's table of echoes, each at its own time, level and place
// between the speakers, as a stadium's public address answers itself.
// Size scales the times, Rolloff below 1 flattens the levels' fall, and
// Gain scales the input. CDP adds the echoes into float samples in the order
// its file buffers give, which the port keeps.
class StadiumEcho
{
public:
    // The least Gain and Rolloff, 1/32767.
    static constexpr double minimumGain = 1.0 / 32767.0;
    static constexpr int minimumCount = 2;
    static constexpr int maximumCount = 1000;
    static constexpr double maximumSize = 100.0;
    // The longest echo, as revecho's other modes bound their delays.
    static constexpr double maximumDelaySeconds = 10.0;
    // CDP8's buffer for a mono input, a third of its default 1 MB of memory.
    static constexpr std::size_t defaultBufferFrames = 87381;

    // False, with no change, for a sample rate that is not positive, a gain
    // or rolloff outside 1/32767 to 1, a size outside 0.2 s over the sample
    // rate to 100, a count outside 2 to 1000, a last echo over 10 s, or no
    // buffer frames.
    [[nodiscard]] bool prepare(
        double sampleRate, double gain, double rolloff, double size, int count,
        std::size_t bufferFrames = defaultBufferFrames);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !history_.empty(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> left,
        std::span<double> right) noexcept;

private:
    // Each echo's delay, and its whole buffers and remainder in CDP's input
    // buffers.
    std::vector<std::size_t> delays_;
    std::vector<std::size_t> delayBuffers_;
    std::vector<std::size_t> delayRemainders_;
    std::vector<double> leftGains_;
    std::vector<double> rightGains_;
    // The input, the last echo's delay back.
    std::vector<float> history_;
    std::size_t bufferFrames_ = defaultBufferFrames;
    std::size_t position_ = 0;
    std::size_t bufferOffset_ = 0;
    std::size_t sounding_ = 0;
    std::size_t frame_ = 0;
};
}
