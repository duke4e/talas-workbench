/*
 * Derived from the CDP System, Release 8: dev/externals/reverb/tdelaymain.cpp
 * and reverberator.cpp
 * Copyright (c) 1983-2013 Richard Dobson and Composers Desktop Project Ltd
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
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// One line of CDP's tap file: a delay in seconds, an amplitude from 0 to 1
// and, in a stereo tap list, a pan position (-1 left, 1 right, beyond them
// farther away).
struct DelayTap
{
    double time = 0.0;
    double amplitude = 0.0;
    double pan = 0.0;

    bool operator==(const DelayTap&) const = default;
};

// The taps a tap file gives, read as CDP's `tapdelay` reads them: comment
// and empty lines skipped, a repeated time dropped, a time under one sample
// taken as zero (the direct sound, which then replaces Mix), and the list
// stereo once any line gives a pan.
struct DelayTapList
{
    std::vector<DelayTap> taps;
    bool stereo = false;
    bool directTap = false;
};

// Nothing for a negative or decreasing time, an amplitude outside 0 to 1,
// a line with fewer than two numbers, no taps, or more than maximumTaps.
[[nodiscard]] std::optional<DelayTapList> readDelayTaps(
    std::string_view text,
    double sampleRate);

// CDP `tapdelay`: a mono or stereo multi-tapped delay line with feedback.
// Each output channel sums its taps (in stereo, each panned with CDP's
// constant-power law), averages them, and is scaled by the gain that would
// bring taps of full amplitude to unity, times TapGain, a second time on the
// way out, as CDP does. Mix blends in the dry input unless a direct tap
// takes its place.
class TapDelay
{
public:
    static constexpr std::size_t maximumTaps = 256;
    static constexpr double maximumTapSeconds = 10.0;

    // False, with no change, for a sample rate that is not positive, a
    // TapGain that is not positive, Feedback outside -1 to 1, Mix outside
    // 0 up to 1, taps beyond maximumTapSeconds, or taps whose amplitudes all
    // vanish in a channel.
    [[nodiscard]] bool prepare(
        double sampleRate,
        const DelayTapList& taps,
        double tapGain,
        double feedback,
        double mix);

    // Silence in the lines and in the fed-back outputs.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }
    [[nodiscard]] bool stereo() const noexcept { return stereo_; }

    // A mono input. A mono tap list writes the same samples to both outputs.
    // False, with no change, before prepare, for spans of different sizes
    // or a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> left,
        std::span<double> right) noexcept;

private:
    struct Line
    {
        std::vector<double> buffer;
        std::vector<unsigned int> pointers;
        std::vector<unsigned int> startPointers;
        std::vector<double> coefficients;
        unsigned int reader = 0;
        double oneOverTaps = 0.0;
        double outputGain = 1.0;
    };

    [[nodiscard]] static double tick(Line& line, double sample) noexcept;

    Line left_;
    Line right_;
    bool prepared_ = false;
    bool stereo_ = false;
    bool mixInput_ = true;
    float feedback_ = 0.0F;
    float dryFactor_ = 0.0F;
    float wetFactor_ = 1.0F;
    float maximumGain_ = 1.0F;
    float leftFedBack_ = 0.0F;
    float rightFedBack_ = 0.0F;
};
}
