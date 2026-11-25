/*
 * Derived from the CDP System, Release 8: dev/distort/distorts.c and
 * distortion.c, and dev/cdp2k/tklib3.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/DistortShuffle.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// The ring holds the latency and the frame arriving.
constexpr std::size_t ringFrames = 4096;

[[nodiscard]] bool letter(char character) noexcept
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}
}

std::optional<std::vector<int>> readDistortShuffle(std::string_view text)
{
    // get_domain_and_image: letters, a dash or colon, letters.
    const auto separator = text.find_first_of("-:");
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size())
        return std::nullopt;
    const auto domain = text.substr(0, separator);
    const auto image = text.substr(separator + 1);
    if (!std::ranges::all_of(domain, letter) || !std::ranges::all_of(image, letter))
        return std::nullopt;
    // consistent_domain_and_image and make_map.
    for (std::size_t n = 0; n + 1 < domain.size(); n++)
    {
        if (domain.find(domain[n], n + 1) != std::string_view::npos)
            return std::nullopt;
    }
    std::vector<int> map;
    for (const auto character : image)
    {
        const auto place = domain.find(character);
        if (place == std::string_view::npos)
            return std::nullopt;
        map.push_back(static_cast<int>(place));
    }
    auto sorted = map;
    std::ranges::sort(sorted);
    for (std::size_t n = 0; n < sorted.size(); n++)
    {
        if (sorted.size() != domain.size() || sorted[n] != static_cast<int>(n))
            return std::nullopt;
    }
    return map;
}

bool DistortShuffle::prepare(std::span<const int> map, int cycles)
{
    std::vector<int> sorted(map.begin(), map.end());
    std::ranges::sort(sorted);
    bool permutation = !sorted.empty();
    for (std::size_t n = 0; n < sorted.size(); n++)
        permutation = permutation && sorted[n] == static_cast<int>(n);
    if (!permutation || cycles < 1 || cycles > maximumCycles)
        return false;
    map_.assign(map.begin(), map.end());
    cycles_ = cycles;
    ring_.assign(ringFrames, 0.0F);
    counts_.assign(ringFrames, static_cast<std::uint16_t>(cycles));
    mask_ = ringFrames - 1;
    domain_.assign(latencyFrames + 1, 0.0F);
    // A cycle takes two frames or more, and a domain is cut at the latency.
    starts_.assign(latencyFrames + 2, 0);
    reset();
    return true;
}

bool DistortShuffle::setCycles(double cycles) noexcept
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

void DistortShuffle::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    std::ranges::fill(counts_, static_cast<std::uint16_t>(cycles_));
    nextCycles_ = cycles_;
    domainCycles_ = cycles_;
    frame_ = 0;
    phase_ = 0;
    secondHalf_ = false;
    done_ = 0;
    std::ranges::fill(starts_, 0);
}

bool DistortShuffle::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (ring_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    const auto domains = static_cast<int>(map_.size());
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto k = frame_;
        const auto b = static_cast<float>(input[index]);
        at(k) = b;
        counts_[k & mask_] = static_cast<std::uint16_t>(nextCycles_);
        // process_with_swapped_bufs_on_full_cycles_with_newsize_output_and_skipcycles
        // reads Cycles where the domain starts.
        if (k == starts_[0])
            domainCycles_ = counts_[k & mask_];
        // The stream waits no longer for a domain to end: the domain so far
        // plays as it came in.
        if (k - starts_[0] >= latencyFrames)
        {
            starts_[0] = k;
            done_ = 0;
            domainCycles_ = counts_[k & mask_];
        }
        const auto cyclecnt = domainCycles_ * domains;
        if (phase_ == 0 && b != 0.0F)
            phase_ = b > 0.0F ? 1 : -1;
        // distort_shuf's cycles, as get_full_cycle counts them.
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
            if (++done_ == cyclecnt)
            {
                shuffle(k);
                done_ = 0;
                domainCycles_ = counts_[k & mask_];
            }
            else
                starts_[static_cast<std::size_t>(done_)] = k;
        }
        output[index] = k >= latencyFrames ? static_cast<double>(at(k - latencyFrames)) : 0.0;
        ++frame_;
    }
    return true;
}

// do_shuffle; do_shuffle_crosbuf copies the same samples across two
// buffers.
void DistortShuffle::shuffle(std::size_t end) noexcept
{
    const auto cycles = static_cast<std::size_t>(domainCycles_);
    starts_[cycles * map_.size()] = end;
    const auto first = starts_[0];
    for (auto k = first; k < end; k++)
        domain_[k - first] = at(k);
    auto outbufpos = first;
    for (const auto group : map_)
    {
        const auto grpbase = static_cast<std::size_t>(group) * cycles;
        for (std::size_t m = 0; m < cycles; m++)
        {
            const auto thiscycle = grpbase + m;
            for (auto k = starts_[thiscycle]; k < starts_[thiscycle + 1]; k++)
                at(outbufpos++) = domain_[k - first];
        }
    }
    starts_[0] = end;
}
}
