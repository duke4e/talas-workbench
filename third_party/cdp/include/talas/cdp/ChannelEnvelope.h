/*
 * Derived from the CDP System, Release 8: dev/standalone/frame.c and
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
// CDP `frame shift 7` (envchans): chosen channels of a multichannel input
// scaled by Gain, the others passed unchanged.
//
// CDP steps its own gain frame by frame between the breakpoints of a Gain
// file: at each breakpoint's frame (its time times the rate, rounded) the
// gain starts again from that breakpoint's value and then moves by
// (next value - this value) / (frames to the next breakpoint) each frame,
// held at the next value once it gets there; past the last breakpoint it
// stays where its steps left it. setGain with a ramp is that step (the
// ramp starting from the last target set), setGain without one a constant
// gain, holdGain the end of the file. CDP's file starting after time 0 makes
// it hold the first value to the second breakpoint and then jump; a host
// that wants that sets it so. No latency.
class ChannelEnvelope
{
public:
    static constexpr int minimumChannels = 2;
    static constexpr int maximumChannels = 16;

    // enveloped lists channel numbers 1 to channels, each at most once.
    // False, with no change, for a sample rate that is not finite and
    // positive, channels outside 2 to 16, no channels to envelope, a channel
    // outside 1 to channels or twice, or a gain outside 0 to 1 (within
    // 0.000002 of a range end is that end).
    [[nodiscard]] bool prepare(double sampleRate, int channels, std::span<const int> enveloped, double gain);

    // From the next frame. rampFrames 0: the gain is `gain`, held. Else the
    // gain starts from the last gain set (the last ramp's target) and moves
    // by (gain - that) / rampFrames a frame, held at `gain`. False, with no
    // change, before prepare or outside 0 to 1.
    [[nodiscard]] bool setGain(double gain, std::size_t rampFrames = 0) noexcept;

    // From the next frame, the gain stays where it is (CDP past its last
    // breakpoint). False before prepare.
    [[nodiscard]] bool holdGain() noexcept;

    // Prepare's gain, held.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The input and output are interleaved, outputChannels() samples a
    // frame. False, with no change, before prepare, for spans of different
    // sizes or not whole frames, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const ChannelEnvelope&) const = default;

private:
    std::size_t channels_ = 0;
    std::array<bool, maximumChannels> enveloped_{};
    double startGain_ = 0.0;
    double gain_ = 0.0;
    double target_ = 0.0;
    double increment_ = 0.0;
};
}
