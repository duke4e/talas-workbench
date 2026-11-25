/*
 * Derived from the CDP System, Release 8: dev/standalone/flutter.c,
 * dev/cdp2k/readdata.c and dev/sfsys/osbind.c
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

#include "talas/cdp/Drand48.h"

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `flutter flutter`: a tremolo that moves around the channels of a
// multichannel input. Every channel is scaled by Gain and lowered to
// 1 - Depth; the channels of the current channel set get the tremolo's
// raised-cosine swing on top (Frequency cycles a second; a Depth above 1
// narrows the peaks, the cosine raised to the power Depth). Each cycle
// moves to the next channel set, round the list; with random order, each
// round after the first is a new random order (CDP's unseeded drand48, as
// cyclicperm draws it) that never starts with the set the last round ended
// on.
//
// CDP reads Frequency and Depth every frame. Its quirk is kept: where Depth
// is 0, CDP leaves the frame alone (no gain, no tremolo) and its
// breakpoint clock stops, so it reads Depth 0 again for every frame left in
// its buffer: the rest of that buffer passes untouched. The stream does the
// same over CDP's buffers (1 MB of samples, 262144 / N frames for N
// channels, or as given), counted from prepare or reset, taking no
// Frequency or Depth until the next buffer starts. No latency.
class Flutter
{
public:
    // flutter.c's ranges.
    static constexpr double maximumFrequency = 120.0;
    static constexpr double maximumDepth = 16.0;
    static constexpr double maximumGain = 10.0;
    static constexpr int maximumChannels = 16;
    static constexpr std::size_t maximumSets = 1024;

    // CDP8's buffer for N channels: 262144 / N frames.
    [[nodiscard]] static std::size_t defaultBufferFrames(int channels) noexcept;

    // channelSets lists the sets in order, each a list of channel numbers 1
    // to channels (a set may be empty, as a blank line in CDP's file).
    // False, with no change, for a sample rate that is not finite and
    // positive, channels outside 1 to 16, no sets or more than 1024, a
    // channel number outside 1 to channels, a frequency, depth or gain
    // outside 0 to 120, 0 to 16 and 0 to 10 (within 0.000002 of a range end
    // is that end), a gain under 0.000002 (CDP: silent), random order with
    // one set (CDP never finds a new order), or no buffer frames.
    // bufferFrames 0 is defaultBufferFrames(channels).
    [[nodiscard]] bool prepare(double sampleRate, int channels, std::span<const std::vector<int>> channelSets,
        double frequency, double depth, double gain, bool randomOrder = false, std::size_t bufferFrames = 0);

    // From the next frame. False, with no change, before prepare or outside
    // the ranges.
    [[nodiscard]] bool setFrequency(double frequency) noexcept;
    [[nodiscard]] bool setDepth(double depth) noexcept;

    // Prepare's controls, the first set, the cosine at 0, drand48 from its
    // start, frame 0 next (the start of CDP's first buffer).
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The input and output are interleaved, outputChannels() samples a
    // frame. False, with no change, before prepare, for spans of different
    // sizes or not whole frames, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const Flutter&) const = default;

private:
    void cyclicperm() noexcept;

    std::size_t channels_ = 0;
    double tabsizeOverSrate_ = 0.0;
    double gain_ = 0.0;
    double startFrequency_ = 0.0;
    double startDepth_ = 0.0;
    bool random_ = false;
    std::size_t bufferFrames_ = 0;
    // The sets, flat, with each one's start and length.
    std::vector<std::size_t> setChannels_;
    std::vector<std::size_t> setStarts_;
    std::vector<std::size_t> setLengths_;
    std::vector<double> costab_;

    double frequency_ = 0.0;
    double depth_ = 0.0;
    double fcospos_ = 0.0;
    std::size_t chansetcnt_ = 0;
    std::size_t lastchanset_ = 0;
    std::vector<std::size_t> perm_;
    std::size_t setStart_ = 0;
    std::size_t setLength_ = 0;
    // CDP's drand48 state (Drand48::state).
    unsigned long randomState_ = Drand48{}.state;
    std::size_t bufferPosition_ = 0;
    bool frozen_ = false;
};
}
