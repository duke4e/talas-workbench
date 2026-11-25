/*
 * Derived from the CDP System, Release 8: dev/standalone/mchanpan.c,
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `mchanpan mchanpan 6` (antiphony): the input switched back and forth
// between two sets of output channels, an event (Event duration long) at a
// time, each event ending Gap early with a splice out while the next
// splices in. With a side chain, the events alternate between the input
// and the side chain too. A set lists output channels (CDP's letters a-p);
// with an input of C channels, each set is C groups, input channel n going
// to the n-th group.
//
// By default CDP8's mapping bugs are kept, as its output shows them: it uses
// only the first half (rounded up) of each set's channels, and it sends
// channel k of a set to output channel k + 1; channel N goes to the next
// frame's first channel, except at the end of one of CDP's buffers (1 MB of
// samples shared by the input(s) and the output, or as given), where it is
// lost.
//
// Corrected (a prepare flag; no CDP reference) plays what CDP evidently
// intends: every channel of each set, channel k on output channel k, so
// nothing spills into the next frame and CDP's buffers play no part. The
// stream never stops a boundary early in either mode. The events, splices
// and controls are as CDP8's.
//
// CDP works its event times out in advance: each event's length from Event
// duration at its start time, and each event's gap at its end. The stream
// takes both as each event's splice in ends (frame 0 for the first), for
// the event after it: a host replaying CDP's files sets Event duration at
// the time of that next event's start and Gap at the time of its end. CDP
// refuses events no longer than the splice; the stream plays them. CDP's
// loop also stops a boundary early, its last event before the file's end
// playing on to the end (a dependence on the file's length); the stream
// switches on. No latency.
class Antiphony
{
public:
    // mchanpan.c's ranges for mode 6.
    static constexpr int minimumChannels = 3;
    static constexpr int maximumChannels = 16;
    static constexpr double minimumEventDuration = 0.05;
    static constexpr double maximumEventDuration = 60.0;
    static constexpr double maximumGap = 60.0;
    static constexpr double maximumSpliceMs = 1000.0;

    // CDP8's buffer: 1 MB over (input channels x inputs + output channels)
    // floats a frame.
    [[nodiscard]] static std::size_t defaultBufferFrames(int inputChannels, int outputChannels, int inputs) noexcept;

    // The sets hold output channel numbers 1 to outputChannels. False, with
    // no change, for a sample rate that is not finite and positive, input
    // channels outside 1 to 16, output channels outside 3 to 16, an empty
    // set, a channel repeated in a set or past the output channels, a set
    // whose size is not a multiple of the input channels, an event duration
    // outside 0.05 to 60, a gap outside 0 to 60, a splice outside 0 to 1000
    // ms (within 0.000002 of a range end is that end), or no buffer frames.
    // bufferFrames 0 is defaultBufferFrames.
    [[nodiscard]] bool prepare(double sampleRate, int inputChannels, int outputChannels,
        std::span<const int> firstSet, std::span<const int> secondSet, double eventDuration, double gap,
        double spliceMs, bool sideChain = false, std::size_t bufferFrames = 0, bool corrected = false);

    [[nodiscard]] bool corrected() const noexcept { return corrected_; }

    // Taken as the current event's splice in ends. False, with no change,
    // before prepare or outside the ranges.
    [[nodiscard]] bool setEventDuration(double seconds) noexcept;
    [[nodiscard]] bool setGap(double seconds) noexcept;

    // Prepare's controls, the first set and the input, frame 0 next (the
    // start of CDP's first buffer).
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return outChannels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t inputChannels() const noexcept { return inChannels_; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return outChannels_; }

    // Inputs interleaved, inputChannels() samples a frame, the output
    // outputChannels(). False, with no change, before prepare, for the
    // wrong form (the side chain one with a side chain prepared, the other
    // without), spans whose frames differ, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> sideChain, std::span<double> output) noexcept;

    bool operator==(const Antiphony&) const = default;

private:
    struct Pair
    {
        std::size_t in = 0;
        std::size_t out = 0;
        bool operator==(const Pair&) const = default;
    };
    using Pairs = std::array<Pair, maximumChannels>;

    [[nodiscard]] bool run(std::span<const double> a, std::span<const double> b, std::span<double> output) noexcept;
    void nextEvent() noexcept;

    double sampleRate_ = 0.0;
    std::size_t inChannels_ = 0;
    std::size_t outChannels_ = 0;
    bool sideChain_ = false;
    bool corrected_ = false;
    std::size_t bufferFrames_ = 0;
    std::size_t splice_ = 0;
    double splicincr_ = 0.0;
    std::array<Pairs, 2> sets_{};
    std::array<std::size_t, 2> setSizes_{};
    double startDuration_ = 0.0;
    double startGap_ = 0.0;

    double duration_ = 0.0;
    double gap_ = 0.0;
    double time_ = 0.0;
    std::size_t state_ = 0;
    std::size_t thisfile_ = 0;
    bool inSplice_ = false;
    std::int64_t lasteventend_ = 0;
    std::int64_t thisevent_ = 0;
    double dnsplicval_ = 1.0;
    double upsplicval_ = 0.0;
    std::size_t frame_ = 0;
    std::size_t bufferPosition_ = 0;
    float carry_ = 0.0F;
};
}
