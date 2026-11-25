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
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// CDP's shuffle data, "domain-image" (or "domain:image"), as
// read_shuffle_data reads it: for each letter of the image, the place of
// that letter in the domain. Nothing for anything but letters around one
// separator, a domain letter repeated, an image letter the domain lacks, or
// an image that is not the domain's letters each once (a stream cannot
// lengthen or shorten).
[[nodiscard]] std::optional<std::vector<int>> readDistortShuffle(std::string_view text);

// CDP `distort shuffle`: the wavecycles fall into groups of Cycles, the
// groups into domains of one group a domain letter, and each domain plays
// its groups in the image's order.
//
// CDP reads its whole file; the stream waits for each domain to end, so the
// output comes latencyFrames late, and a longer domain is cut short and
// plays as it came in.
class DistortShuffle
{
public:
    // The longest domain the stream waits for, as Distort Reform waits.
    static constexpr std::size_t latencyFrames = 2047;
    static constexpr int maximumCycles = 32767;

    // False, with no change, for cycles outside 1 to 32767 or a map that is
    // no permutation.
    [[nodiscard]] bool prepare(std::span<const int> map, int cycles);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    // CDP takes Cycles as a breakpoint file and reads it, rounded, where each
    // domain starts: the frames of the next process call take this. False,
    // with no change, before prepare or for a value that does not round to 1
    // to 32767.
    [[nodiscard]] bool setCycles(double cycles) noexcept;

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    void shuffle(std::size_t end) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    std::vector<int> map_;
    int cycles_ = 1;
    // The cycles at each frame of the ring, those the next frames take, and
    // those of the domain under way.
    std::vector<std::uint16_t> counts_;
    int nextCycles_ = 1;
    int domainCycles_ = 1;
    std::vector<float> ring_;
    std::vector<float> domain_;
    std::vector<std::size_t> starts_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    int phase_ = 0;
    bool secondHalf_ = false;
    int done_ = 0;
};
}
