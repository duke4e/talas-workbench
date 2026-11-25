/*
 * Derived from the CDP System, Release 8: dev/modify/radical.c,
 * dev/new/subtract.c, dev/submix/mixmerge.c and mixprepro.c, and
 * dev/cdp2k/tklib1.c
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
// CDP's programs that combine two sounds sample by sample: the input is
// CDP's first file and the side its second, both mono. CDP reads both
// files in step, so each output frame takes the frames of both that arrive
// with it (or, in a staggered crossfade, the side's from that much
// earlier), and nothing waits.

// CDP `modify radical` mode 6, CROSS MODULATE: the product of the input and
// the side, multiplied as floats. CDP stops at the end of the shorter file.
class CrossModulate
{
public:
    // Always true: the program has no parameters.
    [[nodiscard]] bool prepare() noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

    bool operator==(const CrossModulate&) const = default;

private:
    bool prepared_ = false;
};

// CDP `subtract subtract`: the input less the side, subtracted as floats.
// CDP's -c picks the channel of a multichannel first file the mono second
// file is taken from; a mono input has only the first, which -c0 and -c1
// both name. CDP pads the shorter file with silence, to the longer's end.
class Subtract
{
public:
    // Always true: a mono input leaves the program no choice.
    [[nodiscard]] bool prepare() noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

    bool operator==(const Subtract&) const = default;

private:
    bool prepared_ = false;
};

// CDP `submix balance`: the input times Balance plus the side times one
// less Balance, so 1 plays the input alone and 0 the side alone. CDP pads
// a shorter second file with silence and stops at the first file's end.
//
// CDP's -b starts both files and the output later and its -e stops the
// output early, which a stream, keeping its length, cannot do: the
// processor plays as CDP's defaults, from the start of both to the end.
class MixBalance
{
public:
    static constexpr double defaultBalance = 0.5;

    // False, with no change, for a balance outside 0 to 1.
    [[nodiscard]] bool prepare(double balance = defaultBalance) noexcept;

    // CDP takes the balance as a breakpoint file, which it reads at every
    // frame (stepping by a fixed increment from each breakpoint's value
    // across each segment, its breakpoint times rounded to frames): the
    // frames of the next process call take this balance. False, with no
    // change, before prepare or for a balance outside 0 to 1.
    [[nodiscard]] bool setBalance(double balance) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

    bool operator==(const MixBalance&) const = default;

private:
    bool prepared_ = false;
    double startBalance_ = defaultBalance;
    double balance_ = defaultBalance;
};

// CDP `submix crossfade`: the input alone until Begin, then a fade from the
// input to the side, ending at End, then the side alone. Linear and Cosine
// are CDP's modes 1 and 2: the side's level rises linearly, or along a
// raised cosine read from CDP's 513-point table, its position first raised
// to the power factor (unless that is within 0.000002 of 1). Stagger delays
// the side, which CDP's second file enters that much later; the input
// plays alone until then. Times run from the stream's start, and all are
// rounded to frames as CDP rounds them.
//
// CDP's End defaults to the shorter file's end, which a stream does not
// have: End must be given. After End CDP plays its second file to its end,
// dropping the rest of the first; the stream plays the side on.
class MixCrossfade
{
public:
    enum class Mode
    {
        Linear,
        Cosine,
    };

    // mixxcon.h's MCR_TABLEN: the cosine table holds one more point.
    static constexpr std::size_t tableSize = 512;
    // globcon.h's BIG_TIME, CDP's longest End; Begin is held to it too, as
    // a stream has no duration (CDP's Begin and Stagger run to the first
    // file's duration).
    static constexpr double maximumTime = 32767.0;
    // The longest stagger the side's delay line holds.
    static constexpr double maximumStaggerSeconds = 10.0;
    // mixxcon.h's MIN_MCR_POWFAC and MAX_MCR_POWFAC.
    static constexpr double minimumPowerFactor = 0.125;
    static constexpr double maximumPowerFactor = 8.0;

    // False, with no change, for a sample rate that is not finite and
    // positive, a begin or end outside 0 to 32767 s, a stagger outside 0 to
    // 10 s, a power factor outside 0.125 to 8, a time beyond 2^31 - 1
    // frames (CDP's int), or times that, rounded to frames, start the fade
    // before the stagger ends or give it no length. Linear mode, CDP's mode
    // 1, has no power factor and ignores it.
    [[nodiscard]] bool prepare(
        double sampleRate, Mode mode, double begin, double end, double stagger = 0.0,
        double powerFactor = 1.0);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return crossfadeFactor_ > 0.0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

    bool operator==(const MixCrossfade&) const = default;

private:
    enum class Contour
    {
        Linear,
        Cosine,
        Skewed,
    };

    [[nodiscard]] double sideLevel(long long index) const noexcept;

    Contour contour_ = Contour::Linear;
    double powerFactor_ = 1.0;
    // mixcross_preprocess's MCR_BEGIN, MCR_END and MCR_STAGGER in frames,
    // and MCR_CROSFACT.
    long long begin_ = 0;
    long long end_ = 0;
    std::size_t stagger_ = 0;
    double crossfadeFactor_ = 0.0;
    // The side's last stagger_ frames, as CDP's float buffer holds them.
    std::vector<float> delay_;
    std::size_t delayPosition_ = 0;
    // The frames since reset, counted until End.
    long long frame_ = 0;
};
}
