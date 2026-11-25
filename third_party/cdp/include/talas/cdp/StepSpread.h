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
#include <span>

namespace talas::cdp
{
// CDP `mchanpan mchanpan 3` (stepspread): a mono input of events
// separated by silences (runs of at least Minimum silence exact zeros),
// each event spread from a Centre channel over Spread channels of a ring
// of N outputs and held there until the next event starts. The channels
// wholly within the spread play at 1 - Rolloff (1 - 1 / floor(Spread)), the
// two at its edges the fraction; Depth below half the spread leaves a hole
// in the middle. Stepped: the first event on one channel, each later event
// one channel wider each side, up to Spread (Spread and Depth taken as
// whole numbers).
//
// An event starts at the first nonzero sample after a long enough silence;
// a silence from the stream's start does not start one (the first event
// runs from frame 0). The stream takes Centre, Depth and Rolloff as each
// event starts, and Spread then too, except that for the first event it
// takes Spread again where a leading silence ends. Where CDP reads them is
// its quirk, which a host replaying CDP's breakpoint files reproduces by
// setting the values CDP reads: Spread at the event's time, but Centre,
// Depth and Rolloff at a time within CDP's buffer (cdpReadFrame), and the
// first event's Spread at its first sound. CDP refuses a file without a
// long enough silence; the stream plays it as one event. No latency.
class StepSpread
{
public:
    // mchanpan.c's ranges for mode 3.
    static constexpr int minimumChannels = 3;
    static constexpr int maximumChannels = 16;
    static constexpr double maximumCentre = 8.0;
    static constexpr double maximumSpread = 16.0;
    static constexpr double maximumDepth = 8.0;

    // CDP8's buffer for N outputs: 262144 / (1 + N) frames.
    [[nodiscard]] static std::size_t defaultBufferFrames(int outputChannels) noexcept;

    // The frame whose time CDP reads Centre, Depth and Rolloff at for an
    // event starting at eventFrame (counted from the start): the event's
    // frame within the buffer that held the frame before it (1 to B), 0 for
    // the first event.
    [[nodiscard]] static std::size_t cdpReadFrame(std::size_t eventFrame, std::size_t bufferFrames) noexcept;

    // False, with no change, for a sample rate that is not finite and
    // positive, output channels outside 3 to 16, a centre outside 0.5 to 8
    // or past the channels (CDP's centre rounding to 0 writes outside its
    // level array), a spread outside 1 to 16 (below 1 CDP's levels are
    // infinite), a depth outside 0 to 8, a rolloff outside 0 to 1 (within
    // 0.000002 of a range end is that end), or a minimum silence outside
    // 2 to 32767 samples (CDP: milliseconds, rounded to samples).
    [[nodiscard]] bool prepare(double sampleRate, int outputChannels, double centre, double spread, double depth,
        double rolloff, double minimumSilenceMs, bool stepped = false);

    // Taken as the next event starts. False, with no change, before prepare,
    // outside the ranges, or (Centre, Spread, Depth) when stepped.
    [[nodiscard]] bool setCentre(double centre) noexcept;
    [[nodiscard]] bool setSpread(double spread) noexcept;
    [[nodiscard]] bool setDepth(double depth) noexcept;
    [[nodiscard]] bool setRolloff(double rolloff) noexcept;

    // Prepare's controls, the first event starting at frame 0 next.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The output is interleaved, outputChannels() samples a frame. False,
    // with no change, before prepare, for an output that is not
    // outputChannels() times the input, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const StepSpread&) const = default;

private:
    [[nodiscard]] bool centreOk(double& centre) const noexcept;
    void spreadSet(double spread) noexcept;

    std::size_t channels_ = 0;
    bool stepped_ = false;
    std::size_t minsil_ = 0;
    int stepSpread_ = 0;
    int stepDepth_ = 0;
    double startCentre_ = 0.0;
    double startSpread_ = 0.0;
    double startDepth_ = 0.0;
    double startRolloff_ = 0.0;

    double centre_ = 0.0;
    double spread_ = 0.0;
    double depth_ = 0.0;
    double rolloff_ = 0.0;
    // The controls the current event took.
    double eventCentre_ = 0.0;
    double eventDepth_ = 0.0;
    double eventRolloff_ = 0.0;
    std::array<double, maximumChannels> levels_{};
    std::size_t frame_ = 0;
    std::size_t insil_ = 0;
    std::size_t events_ = 0;
    bool leading_ = true;
};
}
