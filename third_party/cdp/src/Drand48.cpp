/*
 * Derived from the CDP System, Release 8: dev/sfsys/osbind.c
 * Copyright (c) 1983-2013 Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Drand48.h"

namespace talas::cdp
{
namespace
{
constexpr int randomMaximum = 0x7fffffff;
}

double Drand48::next() noexcept
{
    if (state == 0)
        state = 123459876;
    const auto hi = static_cast<long>(state / 127773);
    const auto lo = static_cast<long>(state % 127773);
    auto x = 16807 * lo - 2836 * hi;
    if (x < 0)
        x += 0x7fffffff;
    state = static_cast<unsigned long>(x);
    const auto value = static_cast<int>(state % (static_cast<unsigned long>(randomMaximum) + 1));
    return static_cast<double>(value) / static_cast<double>(randomMaximum);
}
}
