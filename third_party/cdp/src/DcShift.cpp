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
#include "talas/cdp/DcShift.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR.
constexpr double flterr = 0.000002;
}

bool DcShift::prepare(double shift) noexcept
{
    // tklib1.c's range for HOUSE_RECTIFY; do_rectify refuses a shift that
    // flteq finds equal to 0 ("NO CHANGE to original sound file").
    if (!std::isfinite(shift) || shift < -maximumShift || shift > maximumShift
        || (shift <= flterr && shift >= -flterr))
    {
        return false;
    }
    shift_ = shift;
    return true;
}

void DcShift::reset() noexcept
{
}

bool DcShift::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t n = 0; n < input.size(); n++)
    {
        // do_rectify, on CDP's float buffer: rectify_shift is the shift times
        // F_MAXSAMP, which is 1.
        const auto sample = static_cast<float>(input[n]);
        output[n] = static_cast<double>(static_cast<float>(static_cast<double>(sample) + shift_));
    }
    return true;
}
}
