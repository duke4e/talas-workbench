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
#include "talas/cdp/EnvelopeWarpStretch.h"

#include <algorithm>
#include <cmath>
#include <utility>

// CDP8's build fuses envintpl's base + tratio * valdif; the std::fma below
// stands for it, and contraction is off elsewhere.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace talas::cdp
{
namespace
{
// globcon.h's MIN_FRACTION_OF_LEVEL and FLTERR.
constexpr double minimumLevel = 1.0 / 32767.0;
constexpr double flterr = 0.000002;
// The most envelope windows kept.
constexpr std::size_t mostWindows = std::size_t{1} << 24;

// envreplace for one window, as EnvelopeWarp's.
[[nodiscard]] float replaceWindow(float value, float original, bool first, float previous) noexcept
{
    if (static_cast<double>(original) <= minimumLevel)
    {
        if (static_cast<double>(value) <= minimumLevel || first)
            return 0.0F;
        return previous;
    }
    return value / original;
}

[[nodiscard]] std::size_t powerOfTwoAtLeast(std::size_t n) noexcept
{
    std::size_t size = 1;
    while (size < n)
        size <<= 1;
    return size;
}
}

bool EnvelopeWarpStretch::prepare(
    double sampleRate, double windowMs, double stretch, double maximumLagSeconds, std::size_t bufferFrames)
{
    const std::size_t window = EnvelopeWindows::windowFrames(sampleRate, windowMs);
    if (window == 0 || !std::isfinite(stretch) || !std::isfinite(maximumLagSeconds))
        return false;
    // readdata.c: within FLTERR of the range's top is the top.
    if (!(stretch > maximumStretch + flterr || stretch < maximumStretch - flterr))
        stretch = maximumStretch;
    if (stretch < minimumStretch || stretch > maximumStretch)
        return false;
    if (maximumLagSeconds < 0.0 || maximumLagSeconds > longestLagSeconds)
        return false;
    const double lagWindows = std::ceil(maximumLagSeconds * sampleRate / static_cast<double>(window));
    if (!(lagWindows + 4.0 <= static_cast<double>(mostWindows)))
        return false;
    EnvelopeWarpStretch fresh;
    if (!fresh.engine_.prepare(window, 0, bufferFrames))
        return false;
    fresh.stretch_ = stretch;
    fresh.skip_ = 1.0 / stretch;
    fresh.raw_.assign(powerOfTwoAtLeast(static_cast<std::size_t>(lagWindows) + 4), 0.0F);
    fresh.mask_ = fresh.raw_.size() - 1;
    *this = std::move(fresh);
    return true;
}

void EnvelopeWarpStretch::reset() noexcept
{
    engine_.reset();
    std::fill(raw_.begin(), raw_.end(), 0.0F);
    previous_ = 0.0F;
}

float EnvelopeWarpStretch::raw(std::size_t k, std::size_t m) const noexcept
{
    // Window k, or, past what the ring keeps, the oldest it does.
    const std::size_t oldest = m + 1 > raw_.size() ? m + 1 - raw_.size() : 0;
    return raw_[std::max(k, oldest) & mask_];
}

float EnvelopeWarpStretch::stretched(std::size_t m) const noexcept
{
    // envtstretch: nothing to do unless timestretch > 1.0.
    if (!(stretch_ > 1.0))
        return raw(m, m);
    // envlenthn: window m at here = skip * m lies on the segment from
    // window n - 1 to window n, n the first from 1 with here <= n.
    const double here = skip_ * static_cast<double>(m);
    std::size_t n = static_cast<std::size_t>(std::ceil(here));
    if (n < 1)
        n = 1;
    const double winbase = static_cast<double>(n - 1);
    const double base = static_cast<double>(raw(n - 1, m));
    const double next = static_cast<double>(raw(n, m));
    // envintpl.
    const double tratio = here - winbase;
    const double valdif = next - base;
    const double val = std::fma(tratio, valdif, base);
    return static_cast<float>(val);
}

bool EnvelopeWarpStretch::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); }))
        return false;
    for (std::size_t i = 0; i < input.size(); i++)
    {
        if (engine_.push(static_cast<float>(input[i]), 0.0F))
        {
            const std::size_t m = engine_.windowIndex() - 1;
            raw_[m & mask_] = engine_.peak();
            const float value = replaceWindow(stretched(m), engine_.peak(), m == 0, previous_);
            if (m > 0)
                engine_.render(m - 1, previous_, value);
            previous_ = value;
        }
        output[i] = engine_.pull();
    }
    return true;
}
}
