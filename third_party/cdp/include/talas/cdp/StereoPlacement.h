/*
 * Derived from the CDP System, Release 8: dev/standalone/mchstereo.c and
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
#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace talas::cdp
{
// CDP `mchstereo mchstereo`: several stereo inputs placed in an N-channel
// output, each centred on its own output channel (1 to N, the ring wrapping
// from N to 1). Centred, the input's left and right each send Pregain / 2
// to the centre channel (as their float sum times Pregain / 2) and Pregain
// / 2 to the channel below (left) or above (right); shifted (-s), the left
// goes to the centre channel and the right to the one above, each times
// Pregain. The inputs add in their order, one channel at a time, each sum
// rounded to float (a fused multiply-add in CDP8's build).
//
// CDP takes the centres from a text file, or, for one input, as a number on
// its command line, which it does not convert to its internal numbering:
// a centre given that way lands one channel up (N + 1 lands in the next
// frame's channel 1, as does a centre of N + 1 from the file, which CDP
// takes too). The port takes the centres as the file does, and refuses
// any past N. CDP plays its longest input on, the others silent past their
// ends; the stream plays all its inputs on. No latency.
class StereoPlacement
{
public:
    static constexpr int maximumInputs = 16;
    // mchstereo.c's ranges: 2 to 16 output channels (here to 8), Pregain
    // above 0 to 1.
    static constexpr int minimumChannels = 2;
    static constexpr int maximumChannels = 8;

    // False, with no change, for inputs outside 1 to 16, output channels
    // outside 2 to 8, not one centre for each input, a centre outside 1 to
    // the output channels, or a Pregain that is not finite, outside 0 to 1
    // (within 0.000002 of either end is that end, as CDP reads it), or 0.
    [[nodiscard]] bool prepare(int inputs, int outputChannels, std::span<const int> centres, double pregain,
        bool shifted = false) noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t inputs() const noexcept { return inputs_; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // Each input is interleaved stereo, the output interleaved,
    // outputChannels() samples a frame, as many frames as each input. False,
    // with no change, before prepare, for not inputs() inputs, sizes that
    // do not match, or a non-finite input.
    [[nodiscard]] bool process(std::span<const std::span<const double>> inputs, std::span<double> output) noexcept;

    bool operator==(const StereoPlacement&) const = default;

private:
    std::size_t inputs_ = 0;
    std::size_t channels_ = 0;
    // Each input's centre, 0 to N - 1.
    std::array<std::size_t, maximumInputs> centres_{};
    double pregain_ = 0.0;
    double halfPregain_ = 0.0;
    bool shifted_ = false;
};
}
