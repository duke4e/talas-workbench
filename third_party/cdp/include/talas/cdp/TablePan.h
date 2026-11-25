/*
 * Derived from the CDP System, Release 8: dev/standalone/mchanpan.c and
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
#include <cstdint>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `mchanpan mchanpan 1`: a mono input moved around a ring of output
// channels along a table of (time, position, type) points, from the
// stream's start. Each point's type sets how the sound moves on to the next
// point: 0, direct from its channel to the next point's channel (whole
// channels), crossfading straight between the two; 1, clockwise around the
// ring through the channels between; -1, anticlockwise. Positions are 1 to
// N (0 to 1 lies between channel N and channel 1). Focus below 1 gives
// (1 - Focus) / 2 of the signal to the channels beside those panned
// between.
//
// CDP's way through the table is kept: the first point's time is ignored
// (the first move runs from frame 0 to the second point); the gains are
// worked out at each point and every 128 frames after (a count that a point
// arriving mid-block adds 128 to); a direct move between channels that are
// not neighbours fades the start channel out and the goal in along the
// spill to the neighbours; where a direct move's position reaches the end
// of its pair before the move ends, CDP's pan puts the sound back on the
// start channel; each move lasts a frame longer than its points are apart
// (CDP counts down from the frame difference to -1), so later points come
// a frame later each; a rotation leaves the goal channel of direct moves
// where the last direct move left it. After the last point the sound stays
// where it is. No latency.
class TablePan
{
public:
    struct Point
    {
        double time = 0.0;
        double position = 1.0;
        int type = 0;
        bool operator==(const Point&) const = default;
    };

    // mchanpan.c's ranges.
    static constexpr int minimumChannels = 3;
    static constexpr int maximumChannels = 16;
    static constexpr double maximumPosition = 16.0;
    // CDP's BSIZE: the frames between gain updates.
    static constexpr std::int64_t blockFrames = 128;

    // False, with no change, for a sample rate that is not finite and
    // positive, output channels outside 3 to 16, a focus outside 0 to 1
    // (within 0.000002 of a range end is that end), an empty table, a time
    // that is negative or does not rise, two times in one frame (rounded),
    // a position outside 0 to 16 or above the channels, a type other than
    // -1, 0 or 1, or a direct (type 0) point between channels.
    [[nodiscard]] bool prepare(double sampleRate, int outputChannels, std::span<const Point> table,
        double focus = 1.0);

    // The table's start, frame 0 next.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The output is interleaved, outputChannels() samples a frame. False,
    // with no change, before prepare, for an output that is not
    // outputChannels() times the input, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const TablePan&) const = default;

private:
    [[nodiscard]] std::int64_t newposition() noexcept;
    void pancalc() noexcept;

    double sampleRate_ = 0.0;
    std::size_t channels_ = 0;
    double scalefact_ = 0.0;
    double focus_ = 1.0;
    std::vector<Point> table_;
    std::vector<std::int64_t> frames_;

    std::size_t brkindex_ = 1;
    int leftchan_ = 1;
    int rightchan_ = 0;
    int farleftchan_ = 0;
    int farrightchan_ = 0;
    int startbesideleft_ = 0;
    int startbesideright_ = 0;
    int goalbesideleft_ = 0;
    int goalbesideright_ = 0;
    int trueGoalchan_ = 0;
    int goalchan_ = 0;
    int startchan_ = 0;
    int pantype_ = 0;
    int step_ = 0;
    std::int64_t block_ = 0;
    std::int64_t sams_ = 0;
    std::int64_t totalSams_ = 0;
    double leftgain_ = 0.0;
    double rightgain_ = 0.0;
    double farleftgain_ = 0.0;
    double farrightgain_ = 0.0;
    double lcoef_ = 0.0;
    double rcoef_ = 0.0;
    double llcoef_ = 0.0;
    double rrcoef_ = 0.0;
    double maxllcoef_ = 0.0;
    double position_ = 0.0;
    double posincr_ = 0.0;
};
}
