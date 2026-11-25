/*
 * Derived from the CDP System, Release 8: dev/env/envfuncs.c,
 * dev/env/envxtract.c, dev/env/envimpos.c, dev/cdp2k/tklib1.c and
 * dev/cdp2k/readdata.c
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

#include "talas/cdp/EnvelopeWarp.h"

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `envel warp` mode 6, TIMESTRETCH, for a Timestretch S of 1 or more:
// the input's own windowed envelope (EnvelopeWindows) slowed by S replaces
// it. Window m takes the envelope at window m / S (envlenthn: linear
// between the windows either side, rounded to float; with S exactly 1 the
// envelope is left as it is), divided by the input's own (envreplace, as
// EnvelopeWarp: the last ratio held where the input's is at or below
// 1/32767, 0 at the start or where the stretched value is that low too),
// imposed as EnvelopeWarp imposes, latencyFrames() = 2W - 1 frames late.
//
// m / S never passes m, so the stream reads only windows it has had, but
// ever further back: it keeps the last maximumLagSeconds of envelope, and
// is exact while m - m / S stays within it (for the first
// maximumLagSeconds x S / (S - 1) seconds); past that it reads the oldest
// window it keeps instead. Below 1 the stretch reads ahead without bound
// (envshortn): not streamable. CDP's whole-file overload prescale, its last
// window and its refusal of an envelope that is everywhere at or below
// 1/32767 are as EnvelopeWarp's. Mono.
class EnvelopeWarpStretch
{
public:
    // envlcon.h's MAX_ENV_TSTRETCH (MIN_ENV_TSTRETCH, 0.001, being below
    // what streams).
    static constexpr double minimumStretch = 1.0;
    static constexpr double maximumStretch = 1000.0;
    static constexpr double defaultMaximumLagSeconds = 600.0;
    static constexpr double longestLagSeconds = 3600.0;

    // False, with no change, for a sample rate that is not finite and
    // positive, a window outside 5 to 10000 ms, a Timestretch that is not
    // finite or outside 1 to 1000 (within 0.000002 of 1000 is 1000), a lag
    // that is not finite or outside 0 to 3600 s, or a zero buffer (CDP's,
    // as EnvelopeWarp's).
    [[nodiscard]] bool prepare(double sampleRate, double windowMs, double stretch,
        double maximumLagSeconds = defaultMaximumLagSeconds,
        std::size_t bufferFrames = EnvelopeWindows::defaultBufferFrames);

    // Silence and frame 0 next.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return engine_.prepared(); }

    // 2W - 1 frames, W the window.
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return engine_.latencyFrames(); }

    [[nodiscard]] std::size_t windowFrames() const noexcept { return engine_.frames(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const EnvelopeWarpStretch&) const = default;

private:
    // The stretched envelope's window m (envlenthn).
    [[nodiscard]] float stretched(std::size_t m) const noexcept;
    [[nodiscard]] float raw(std::size_t k, std::size_t m) const noexcept;

    EnvelopeWindows engine_;
    double stretch_ = 1.0;
    // envlenthn's skip, 1 / Timestretch.
    double skip_ = 1.0;
    // The input's envelope, the last mask_ + 1 windows.
    std::vector<float> raw_;
    std::size_t mask_ = 0;
    float previous_ = 0.0F;
};
}
