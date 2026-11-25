/*
 * Derived from the CDP System, Release 8: dev/houskeep/clean.c and
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
// CDP `housekeep extract` mode 4, RECTIFY: shifts every sample by a fixed
// amount, to remove a DC offset. CDP first scans its whole input and refuses
// a shift that would take the file's highest sample above 1 (or its lowest
// below -1); a stream cannot, so this shifts every sample, and plays as CDP
// whenever CDP would accept the input.
class DcShift
{
public:
    static constexpr double maximumShift = 1.0;

    // False, with no change, for a shift outside CDP's range of -1 to 1, or
    // within 0.000002 (FLTERR) of 0, which CDP refuses as no change.
    [[nodiscard]] bool prepare(double shift) noexcept;

    // Nothing to clear: each sample is shifted on its own.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return shift_ != 0.0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const DcShift&) const = default;

private:
    double shift_ = 0.0;
};
}
