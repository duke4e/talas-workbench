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
// CDP `frame shift` modes 3, 4, 5, 6 and 8: a multichannel input's
// channels sent, unchanged, to other output channels.
// - Mode 3 (prepareReorient): input channel n to output channel route[n].
// - Mode 4 (prepareMirror): the ring mirrored about a channel (a whole
//   number) or about the point halfway between two (a half; N.5 lies
//   between channel N and channel 1).
// - Mode 5 (prepareBilateral): ring numbering to bilateral (channel 1 at
//   the front, then left-right pairs front to back), or back (toRing).
// - Mode 6 (prepareSwap): two channels swapped.
// - Mode 8 (prepareBeastBilateral): ring numbering to the BEAST's
//   bilateral numbering, or back (toRing).
// Every map CDP builds for 2 to 16 channels sends each input to its own
// output. No latency.
class FrameRemap
{
public:
    static constexpr int minimumChannels = 2;
    static constexpr int maximumChannels = 16;
    // frame.c's range for the mirror plane.
    static constexpr double maximumMirrorPlane = 16.5;

    // Each is false, with no change, for a sample rate that is not finite
    // and positive or channels outside 2 to 16, and:
    // a route that is not channels 1 to N each once;
    [[nodiscard]] bool prepareReorient(double sampleRate, int channels, std::span<const int> route);
    // a plane outside 0 to 16.5, past channel N + 0.5, or neither a whole
    // number nor a half (within 0.000002, as CDP's flteq takes it);
    [[nodiscard]] bool prepareMirror(double sampleRate, int channels, double mirrorPlane);
    [[nodiscard]] bool prepareBilateral(double sampleRate, int channels, bool toRing = false);
    // channels outside 1 to N, or one channel twice;
    [[nodiscard]] bool prepareSwap(double sampleRate, int channels, int a, int b);
    [[nodiscard]] bool prepareBeastBilateral(double sampleRate, int channels, bool toRing = false);

    // Nothing to clear.
    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The output channel (0 to N - 1) that input channel `channel` goes to.
    [[nodiscard]] std::size_t destination(std::size_t channel) const noexcept { return map_[channel]; }

    // The input and output are interleaved, outputChannels() samples a
    // frame. False, with no change, before prepare, for spans of different
    // sizes or not whole frames, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const FrameRemap&) const = default;

private:
    using Map = std::array<std::size_t, maximumChannels>;

    [[nodiscard]] bool accept(double sampleRate, int channels, const Map& map) noexcept;

    std::size_t channels_ = 0;
    Map map_{};
};
}
