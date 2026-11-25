/*
 * Derived from the CDP System, Release 8: dev/distort/distorto.c and
 * distortion.c
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
// CDP `distort omit`: of every OutOf wavecycles, the last Omit fall silent.
// A wavecycle runs from where the signal turns the way its first sample went
// to where it does so again; the cycles keep the phase the first sample
// gave. Each sample's cycle is known as it arrives, so the stream needs no
// latency.
class DistortOmit
{
public:
    // MAX_CYCLECNT.
    static constexpr int maximumOmit = 32767;

    // False, with no change, unless 1 <= omit < outOf <= 32768.
    [[nodiscard]] bool prepare(int omit, int outOf) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return keep_ > 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

    // CDP takes Omit as a breakpoint file and reads it, rounded, where each
    // group of OutOf cycles starts: a group starting in the next process
    // call takes this. False, with no change, before prepare or for a value
    // that does not round to 1 to one less than OutOf.
    [[nodiscard]] bool setOmit(double omit) noexcept;

private:
    int omit_ = 0;
    int keep_ = 0;
    int outOf_ = 0;
    int startOmit_ = 0;
    int nextOmit_ = 0;
    // get_initial_phase: 0 until the first sample that is not zero.
    int phase_ = 0;
    bool omitting_ = false;
    bool secondHalf_ = false;
    int cycles_ = 0;
    // The first sample of the stream starts a group.
    bool groupStarting_ = true;
};
}
