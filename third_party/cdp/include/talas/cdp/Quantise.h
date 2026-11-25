/*
 * Derived from the CDP System, Release 8: dev/modify/radical.c and
 * dev/cdp2k/tklib1.c
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
// CDP `modify radical` mode 7, QUANTISE: a mid-rise quantiser. Each
// sample, scaled by 32767 to CDP's 16-bit range, is rounded to the middle of
// a step of 2^(16 - bitResolution) levels, so that no output is zero
// (silence becomes half a step), and scaled back by 1/32768. As in CDP,
// samples at or below -1 are first held at -32767 levels while samples above
// 1 are not held, and a value too large for CDP's lround saturates as there.
class Quantise
{
public:
    static constexpr int maximumBitResolution = 16;

    // False, with no change, outside CDP's range of 1 to 16 bits.
    [[nodiscard]] bool prepare(int bitResolution) noexcept;
    [[nodiscard]] bool prepared() const noexcept;
    // Nothing to clear: each sample is quantised on its own.
    void reset() noexcept;
    // CDP takes the resolution as a fixed value; as each sample is quantised
    // on its own, a change plays from the next processed frame as CDP plays
    // that resolution there. False, with no change, outside 1 to 16 bits or
    // before prepare.
    [[nodiscard]] bool setBitResolution(int bitResolution) noexcept;

    // False, with no change, before prepare, when an input sample is not
    // finite or the spans differ in size.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    bool operator==(const Quantise&) const = default;

private:
    // 2^(16 - bits), do_lowbit2's rescalefac; 0 before prepare.
    double rescale_ = 0.0;
};
}
