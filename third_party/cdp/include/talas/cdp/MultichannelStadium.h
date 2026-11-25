/*
 * Derived from the CDP System, Release 8: dev/standalone/mchanrev.c and
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

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `mchanrev mchanrev`: a mono input heard again through CDP's table of
// stadium echoes, each at its own time, level and place, spread around a
// ring of N output channels. Each echo sits Pan / 2 * Spread channels from
// Centre (1 to N; below 1 lies between channel N and channel 1), panned
// equal-power between the two channels either side. Size scales the times,
// Rolloff below 1 flattens the levels' fall (level ^ Rolloff), and Gain
// scales them all. Count echoes: up to 23 take CDP's short table (its own
// levels, the 23rd at 2.310554 s), more take the long one with levels
// falling from 1 to 0.0001 in equal ratios.
//
// The tap at time 0 (the direct sound) is as this build of CDP8 reads the
// memory its setup leaves unset: with up to 23 echoes, level 0 (no direct
// sound); with more, level 1 at pan 0 (Centre).
//
// CDP adds the echoes into float samples buffer by buffer: for each input
// buffer of B frames, every echo in turn. An output frame takes its echoes
// from the earliest input buffer first, and within a buffer in echo order;
// the stream keeps that order, B as CDP's buffer for N outputs (its 1 MB of
// memory, 262144 samples, shared by the input and N outputs) or as given.
// CDP's tail past the input is dropped. No latency.
class MultichannelStadium
{
public:
    // mchanrev.c's ranges; the least Gain and Rolloff, 1/32767.
    static constexpr double minimumGain = 1.0 / 32767.0;
    static constexpr int minimumCount = 2;
    static constexpr int maximumCount = 1000;
    static constexpr double maximumSize = 100.0;
    static constexpr int minimumChannels = 2;
    static constexpr int maximumChannels = 16;
    static constexpr double minimumSpread = 2.0;
    // The longest echo the stream holds input for.
    static constexpr double maximumDelaySeconds = 100.0;

    // CDP8's buffer for N outputs: 262144 / (1 + N) frames.
    [[nodiscard]] static std::size_t defaultBufferFrames(int outputChannels) noexcept;

    // False, with no change, for a sample rate that is not finite and
    // positive, a gain or rolloff outside 1/32767 to 1, a size outside 0.2 s
    // over the sample rate to 100, a count outside 2 to 1000, output
    // channels outside 2 to 16, a centre outside 0 to the channels, a spread
    // outside 2 to the channels (within 0.000002 of a range end is that
    // end), a last echo over maximumDelaySeconds, or no buffer frames.
    // bufferFrames 0 is defaultBufferFrames(outputChannels).
    [[nodiscard]] bool prepare(double sampleRate, double gain, double rolloff, double size, int count,
        int outputChannels, double centre, double spread, std::size_t bufferFrames = 0);

    // Silence, frame 0 next (the start of CDP's first buffer).
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The output is interleaved, outputChannels() samples a frame. False,
    // with no change, before prepare, for an output that is not
    // outputChannels() times the input, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const MultichannelStadium&) const = default;

private:
    struct Tap
    {
        std::size_t delay = 0;
        // The delay in CDP's buffers: whole buffers and the remainder.
        std::size_t buffers = 0;
        std::size_t remainder = 0;
        std::size_t left = 0;
        std::size_t right = 0;
        double leftGain = 0.0;
        double rightGain = 0.0;
        bool operator==(const Tap&) const = default;
    };

    std::size_t channels_ = 0;
    std::size_t bufferFrames_ = 0;
    std::vector<Tap> taps_;
    std::vector<float> history_;
    std::size_t mask_ = 0;
    std::size_t sounding_ = 0;
    std::size_t position_ = 0;
    std::size_t bufferOffset_ = 0;
    std::size_t frame_ = 0;
};
}
