/*
 * Derived from the CDP System, Release 8: dev/standalone/prefix.c and
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
// CDP `prefix silence`: Duration seconds of silence (round(Duration x rate)
// frames) before the sound. CDP's output is that much longer than its input;
// the stream, keeping the input's length, is the input delayed by those
// frames, silent until then: CDP's output frame for frame, its end dropped.
// Any channels, interleaved. No latency (the delay is the effect).
class Prefix
{
public:
    static constexpr int maximumChannels = 8;
    // prefix.c takes up to 32767 s; the stream's delay line holds 10 s.
    static constexpr double maximumSeconds = 10.0;

    // False, with no change, for a sample rate that is not finite and
    // positive, channels outside 1 to 8, a Duration that is not finite,
    // negative, within 0.000002 of 0 (CDP: no change to the sound), over
    // 10 s, or of no frames at this rate (CDP fails writing no samples).
    [[nodiscard]] bool prepare(double sampleRate, int channels, double duration);

    // Silence in the delay line, frame 0 next.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The delay in frames.
    [[nodiscard]] std::size_t delayFrames() const noexcept { return delay_; }

    // False, with no change, before prepare, for spans of different sizes or
    // not whole frames, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const Prefix&) const = default;

private:
    std::size_t channels_ = 0;
    std::size_t delay_ = 0;
    // The last delay_ frames of the input, as floats.
    std::vector<float> line_;
    std::size_t position_ = 0;
};
}
