/*
 * Derived from the CDP System, Release 8: dev/modify/pan.c
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
// CDP `modify space` mode 1, PAN: places a mono input in the stereo field,
// scaled by Prescale. Between the speakers (positions -1 to 1) the gains
// follow CDP's distance law; beyond them the far speaker falls silent and the
// near one falls off with the square of the distance.
class Pan
{
public:
    static constexpr std::size_t controlBlockFrames = 128;
    static constexpr double maximumPosition = 256.0;
    static constexpr double minimumPrescale = 1.0 / 32767.0;
    static constexpr double maximumPrescale = 2.0;

    void reset() noexcept;

    // Position and Prescale are read at the start of each 128-frame block.
    // False, with no change, for a position beyond 256, a prescale outside
    // 1/32767 to 2, a non-finite input, or spans of different sizes.
    [[nodiscard]] bool process(
        std::span<const double> input,
        double position,
        double prescale,
        std::span<double> left,
        std::span<double> right) noexcept;

    bool operator==(const Pan&) const = default;

private:
    double leftCoefficient_ = 0.0;
    double rightCoefficient_ = 0.0;
    std::size_t framesLeftInBlock_ = 0;
};
}
