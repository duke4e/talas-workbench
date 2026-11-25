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
#pragma once

namespace talas::cdp
{
// CDP's drand48, rand() / RAND_MAX, over the C library's rand(): Park and
// Miller's minimal standard generator. srand(1) is rand()'s own start.
struct Drand48
{
    unsigned long state = 1;

    void seed(unsigned long value) noexcept { state = value; }
    [[nodiscard]] double next() noexcept;
};
}
