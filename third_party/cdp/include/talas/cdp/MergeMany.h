/*
 * Derived from the CDP System, Release 8: dev/submix/mixmerge.c and
 * dev/submix/ap_mix.c
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
#include <span>

namespace talas::cdp
{
// CDP `submix mergemany` (mixmany): two or more sounds of the same channels
// summed in double, sample by sample in the order given, times 1 / Peak,
// then to float.
//
// CDP always runs the mix once first and sets Peak to its largest |sum|
// over the whole file, so that the output peaks at exactly 1 whether the mix
// is louder or quieter: the gain depends on the whole file, so mergemany
// does not stream. The port is the mix with Peak given: given the peak CDP
// measures, it matches CDP bit for bit (Peak 1 is the plain sum, which CDP
// never plays). CDP plays its longest input on, the others silent past
// their ends; the stream plays all its inputs on. No latency.
class MergeMany
{
public:
    static constexpr int minimumInputs = 2;
    static constexpr int maximumInputs = 16;
    static constexpr int maximumChannels = 8;

    // False, with no change, for inputs outside 2 to 16, channels outside 1
    // to 8, or a Peak that is not finite and positive.
    [[nodiscard]] bool prepare(int inputs, int channels, double peak = 1.0) noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t inputs() const noexcept { return inputs_; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // mixmany's gain, F_MAXSAMP / Peak.
    [[nodiscard]] double gain() const noexcept { return gain_; }

    // Each input and the output interleaved, outputChannels() samples a
    // frame, all of one size. False, with no change, before prepare, for not
    // inputs() inputs, sizes that differ or are not whole frames, or a
    // non-finite input.
    [[nodiscard]] bool process(std::span<const std::span<const double>> inputs, std::span<double> output) noexcept;

    bool operator==(const MergeMany&) const = default;

private:
    std::size_t inputs_ = 0;
    std::size_t channels_ = 0;
    double gain_ = 0.0;
};
}
