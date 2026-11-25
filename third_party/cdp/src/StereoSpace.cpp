/*
 * Derived from the CDP System, Release 8: dev/modify/pan.c and
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
#include "talas/cdp/StereoSpace.h"

#include <algorithm>
#include <cmath>

// do_narrow's products and sum are separate statements, which CDP8's build
// does not fuse; nor may this file.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR.
constexpr double FLTERR = 0.000002;

// tklib1.c's flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const auto upperbnd = f2 + FLTERR;
    const auto lowerbnd = f2 - FLTERR;
    return !(f1 > upperbnd || f1 < lowerbnd);
}
}

bool StereoSpace::prepareMirror() noexcept
{
    *this = StereoSpace{};
    prepared_ = true;
    mode_ = Mode::Mirror;
    kind_ = Kind::Mirror;
    return true;
}

bool StereoSpace::prepareNarrow(double narrowing) noexcept
{
    if (!std::isfinite(narrowing))
        return false;
    // readdata.c: a value within FLTERR of a range end is that end.
    if (flteq(narrowing, minimumNarrowing))
        narrowing = minimumNarrowing;
    if (flteq(narrowing, maximumNarrowing))
        narrowing = maximumNarrowing;
    if (narrowing > maximumNarrowing || narrowing < minimumNarrowing)
        return false;
    // narrow_sound.
    if (flteq(narrowing, 1.0))
        return false;
    StereoSpace fresh;
    fresh.prepared_ = true;
    fresh.mode_ = Mode::Narrow;
    fresh.kind_ = Kind::Narrow;
    if (flteq(narrowing, -1.0))
        fresh.kind_ = Kind::Mirror;
    else if (flteq(narrowing, 0.0))
        fresh.kind_ = Kind::Mono;
    double narrow = narrowing + 1.0;
    narrow /= 2.0;
    fresh.own_ = narrow;
    fresh.other_ = 1.0 - narrow;
    *this = fresh;
    return true;
}

bool StereoSpace::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared_ || input.size() != output.size() || input.size() % 2 != 0
        || !std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); }))
        return false;
    for (std::size_t i = 0; i < input.size(); i += 2)
    {
        const float left = static_cast<float>(input[i]);
        const float right = static_cast<float>(input[i + 1]);
        float newLeft = left;
        float newRight = right;
        switch (kind_)
        {
        case Kind::Mirror:
            // do_mirror.
            newLeft = right;
            newRight = left;
            break;
        case Kind::Narrow:
        {
            // do_narrow.
            const double addToRight = static_cast<double>(left) * other_;
            const double addToLeft = static_cast<double>(right) * other_;
            double l = static_cast<double>(left) * own_;
            double r = static_cast<double>(right) * own_;
            l += addToLeft;
            r += addToRight;
            newLeft = static_cast<float>(l);
            newRight = static_cast<float>(r);
            break;
        }
        case Kind::Mono:
        {
            // do_mono: a float sum.
            const float sum = left + right;
            const double temp = static_cast<double>(sum) * 0.5;
            newLeft = static_cast<float>(temp);
            newRight = newLeft;
            break;
        }
        }
        output[i] = static_cast<double>(newLeft);
        output[i + 1] = static_cast<double>(newRight);
    }
    return true;
}
}
