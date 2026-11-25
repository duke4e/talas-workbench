/*
 * Derived from the CDP System, Release 8: dev/distort/distorte.c,
 * disprepro.c, ap_distort.c and distortion.c, and dev/cdp2k/tklib1.c
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
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// CDP's user envelope for `distort envel`, as get_envfile_data reads it:
// time and value pairs (values 0 to 1), times rising, the times scaled to
// end at 1 through CDP's float. Nothing for an odd count of numbers, no
// numbers, a value outside 0 to 1, times that do not rise, or a first time
// that is not zero (CDP8 then reads an unset value for time zero).
[[nodiscard]] std::optional<std::vector<double>> readDistortEnvelope(std::string_view text);

// CDP `distort envel`: an envelope over each group of Cycles wavecycles,
// rising, falling, troughed (falling to Trough at the group's middle and
// rising again), or the user's. An Exponent other than 1 curves it, as
// CDP's -e flag does; a rising or falling envelope's Trough lifts its low
// end, as CDP's -t flag does.
//
// CDP reads its whole file; the stream waits for each group to end, so the
// output comes latencyFrames late, and a longer group is cut where CDP's
// would run on.
class DistortEnvelope
{
public:
    enum class Shape
    {
        Rising,
        Falling,
        Troughed,
        User,
    };

    // The longest group the stream waits for, as Distort Reform waits for a
    // half-cycle.
    static constexpr std::size_t latencyFrames = 2047;
    static constexpr int maximumCycles = 1000;
    static constexpr double minimumExponent = 0.02;
    static constexpr double maximumExponent = 50.0;

    // False, with no change, for cycles outside 1 to 1000, a trough outside
    // 0 to 1, an exponent outside 0.02 to 50, or, for the user's shape, no
    // envelope from readDistortEnvelope.
    // A live trough or exponent, which CDP takes as a breakpoint file, gives
    // the troffed or curved shapes as CDP's -t and -e flags do.
    [[nodiscard]] bool prepare(
        Shape shape, int cycles, double trough, double exponent,
        std::span<const double> envelope = {}, bool liveTrough = false, bool liveExponent = false);

    // CDP takes Cycles, the trough and the exponent as breakpoint files and
    // reads them, Cycles rounded, where each group starts: the frames of the
    // next process call take these. False, with no change, before prepare
    // or for values outside prepare's ranges.
    [[nodiscard]] bool setControls(double cycles, double trough, double exponent) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    enum class Curve
    {
        Rising,
        Falling,
        Troffed,
        LinearRise,
        LinearFall,
        LinearTrough,
        User,
        RisingTroffed,
        FallingTroffed,
        LinearRiseTroffed,
        LinearFallTroffed,
    };

    void envelopeGroup(std::size_t end) noexcept;
    [[nodiscard]] double gain(int k, int cyclelen, std::size_t& point) const noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    // Takes the group starting at frame k its fields there.
    void startGroup(std::size_t k) noexcept;

    Curve curve_ = Curve::LinearRise;
    int cycles_ = 1;
    double trof_ = 0.0;
    double expon_ = 1.0;
    double oneLessTrof_ = 1.0;
    // The fields prepare gave, those the next frames take, and those at
    // each frame of the ring.
    int startCycles_ = 1;
    double startTrof_ = 0.0;
    double startExpon_ = 1.0;
    int nextCycles_ = 1;
    double nextTrof_ = 0.0;
    double nextExpon_ = 1.0;
    std::vector<std::uint16_t> counts_;
    std::vector<double> troughs_;
    std::vector<double> exponents_;
    std::vector<double> envelope_;
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    std::size_t start_ = 0;
    // get_initial_phase: 0 until the first sample that is not zero.
    int phase_ = 0;
    bool secondHalf_ = false;
    int done_ = 0;
};
}
