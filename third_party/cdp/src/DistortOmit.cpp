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
#include "talas/cdp/DistortOmit.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
bool DistortOmit::prepare(int omit, int outOf) noexcept
{
    if (omit < 1 || omit > maximumOmit || outOf <= omit || outOf > maximumOmit + 1)
        return false;
    omit_ = omit;
    keep_ = outOf - omit;
    outOf_ = outOf;
    startOmit_ = omit;
    nextOmit_ = omit;
    reset();
    return true;
}

bool DistortOmit::setOmit(double omit) noexcept
{
    if (keep_ == 0 || !std::isfinite(omit))
        return false;
    // read_value_from_brktable's rounding of an integer field.
    const auto rounded = std::round(omit);
    if (rounded < 1.0 || rounded >= static_cast<double>(outOf_))
        return false;
    nextOmit_ = static_cast<int>(rounded);
    return true;
}

void DistortOmit::reset() noexcept
{
    if (outOf_ != 0)
    {
        omit_ = nextOmit_ = startOmit_;
        keep_ = outOf_ - omit_;
    }
    groupStarting_ = true;
    phase_ = 0;
    omitting_ = false;
    secondHalf_ = false;
    cycles_ = 0;
}

bool DistortOmit::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (keep_ == 0 || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        auto b = static_cast<float>(input[index]);
        // distort_omt's group reads its Omit where it starts.
        if (groupStarting_)
        {
            omit_ = nextOmit_;
            keep_ = outOf_ - omit_;
            groupStarting_ = false;
        }
        if (phase_ == 0 && b != 0.0F)
            phase_ = b > 0.0F ? 1 : -1;
        // distort_omt: each cycle runs while the samples keep to its first
        // half's side of zero (zero counting on both sides), then while they
        // keep to its second's; the sample that leaves the second starts the
        // next cycle. Zeros before the phase is known belong to the first.
        for (;;)
        {
            const auto firstSide = phase_ >= 0 ? b >= 0.0F : b <= 0.0F;
            const auto secondSide = phase_ >= 0 ? b <= 0.0F : b >= 0.0F;
            if (!secondHalf_ && firstSide)
                break;
            secondHalf_ = true;
            if (secondSide)
                break;
            secondHalf_ = false;
            if (++cycles_ == (omitting_ ? omit_ : keep_))
            {
                cycles_ = 0;
                omitting_ = !omitting_;
                // The next group starts here, with the sample that left the
                // last omitted cycle.
                if (!omitting_)
                {
                    omit_ = nextOmit_;
                    keep_ = outOf_ - omit_;
                }
            }
        }
        if (omitting_)
            b = 0.0F;
        output[index] = static_cast<double>(b);
    }
    return true;
}
}
