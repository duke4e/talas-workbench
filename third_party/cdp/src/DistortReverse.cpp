/*
 * Derived from the CDP System, Release 8: dev/distort/distortr.c,
 * distorte.c and distortion.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/DistortReverse.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace talas::cdp
{
namespace
{
// The ring holds the latency and the frame arriving.
constexpr std::size_t ringFrames = 4096;
}

bool DistortReverse::prepare(int cycles)
{
    if (cycles < 1 || cycles > maximumCycles)
        return false;
    cycles_ = cycles;
    ring_.assign(ringFrames, 0.0F);
    counts_.assign(ringFrames, static_cast<std::uint16_t>(cycles));
    mask_ = ringFrames - 1;
    reset();
    return true;
}

bool DistortReverse::setCycles(double cycles) noexcept
{
    if (ring_.empty() || !std::isfinite(cycles))
        return false;
    // read_value_from_brktable's rounding of an integer field.
    const auto rounded = std::round(cycles);
    if (rounded < 1.0 || rounded > maximumCycles)
        return false;
    nextCycles_ = static_cast<int>(rounded);
    return true;
}

void DistortReverse::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    std::ranges::fill(counts_, static_cast<std::uint16_t>(cycles_));
    nextCycles_ = cycles_;
    groupCycles_ = cycles_;
    frame_ = 0;
    start_ = 0;
    phase_ = 0;
    secondHalf_ = false;
    done_ = 0;
}

bool DistortReverse::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (ring_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto k = frame_;
        const auto b = static_cast<float>(input[index]);
        at(k) = b;
        counts_[k & mask_] = static_cast<std::uint16_t>(nextCycles_);
        // process_with_swapped_bufs_on_full_cycles reads Cycles where the
        // group starts.
        if (k == start_)
            groupCycles_ = counts_[k & mask_];
        // The stream waits no longer for a group to end.
        if (k - start_ >= latencyFrames)
        {
            reverseGroup(k);
            done_ = 0;
            groupCycles_ = counts_[k & mask_];
        }
        if (phase_ == 0 && b != 0.0F)
            phase_ = b > 0.0F ? 1 : -1;
        // get_full_cycle: each cycle runs while the samples keep to its
        // first half's side of zero (zero counting on both sides), then while
        // they keep to its second's; the sample that leaves the second starts
        // the next cycle, and the group ends after Cycles of them.
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
            if (++done_ == groupCycles_)
            {
                reverseGroup(k);
                done_ = 0;
                groupCycles_ = counts_[k & mask_];
            }
        }
        output[index] = k >= latencyFrames ? static_cast<double>(at(k - latencyFrames)) : 0.0;
        ++frame_;
    }
    return true;
}

// reverse_cycles; reverse_cycles_crosbuf turns a group across two buffers
// the same way.
void DistortReverse::reverseGroup(std::size_t end) noexcept
{
    auto startpos = start_;
    auto endpos = end;
    while (endpos > startpos + 1)
    {
        --endpos;
        std::swap(at(startpos), at(endpos));
        ++startpos;
    }
    start_ = end;
}
}
