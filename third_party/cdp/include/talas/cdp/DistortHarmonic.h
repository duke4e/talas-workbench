/*
 * Derived from the CDP System, Release 8: dev/distort/distorth.c,
 * distortf.c, distortion.c and ap_distort.c
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
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// One line of CDP's harmonics file: a harmonic number and its amplitude,
// the wavecycle's own being 1.
struct DistortHarmonicPartial
{
    int number = 2;
    double amplitude = 0.0;

    bool operator==(const DistortHarmonicPartial&) const = default;
};

// The harmonics a file gives, as get_harmonic_data reads it: each line that
// is not blank read as sscanf's "%d%lf" reads it. Nothing for a line it
// cannot read, a number outside 2 to 1024, an amplitude outside 0 to 32 (CDP
// compares it to its range's whole numbers) or not finite, or no lines.
[[nodiscard]] std::optional<std::vector<DistortHarmonicPartial>> readDistortHarmonics(
    std::string_view text);

// CDP `distort harmonic`: each wavecycle has its harmonics added, the cycle
// itself read faster, starting a harmonic's length in; a harmonic with a
// quarter of the cycle or more is left out. Prescale scales the input first.
//
// CDP reads its whole file; the stream waits for each cycle to end, so the
// output comes latencyFrames late, and a longer cycle is cut where CDP's
// would run on.
class DistortHarmonic
{
public:
    // The longest cycle the stream waits for, as Distort Reform waits for a
    // half-cycle.
    static constexpr std::size_t latencyFrames = 2047;
    static constexpr double minimumPrescale = 0.000002;
    static constexpr double maximumPrescale = 200.0;

    // False, with no change, for no harmonics, harmonics outside CDP's
    // ranges, or a prescale outside 0.000002 to 200.
    [[nodiscard]] bool prepare(
        std::span<const DistortHarmonicPartial> harmonics, double prescale);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    void harmonize(std::size_t end) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    std::vector<DistortHarmonicPartial> harmonics_;
    double prescale_ = 1.0;
    std::vector<float> ring_;
    std::vector<float> cycle_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    std::size_t start_ = 0;
    // get_initial_phase: 0 until the first sample that is not zero.
    int phase_ = 0;
    bool secondHalf_ = false;
};

// CDP `distort fractal`: each group of Scale wavecycles, kept as every
// Scale-th sample, is laid in miniature over each of its cycles at
// Loudness. Prescale scales the input first.
//
// CDP reads its whole file; the stream waits for each group to end, so the
// output comes latencyFrames late, and a longer group is cut where CDP's
// would run on.
class DistortFractal
{
public:
    // The longest group the stream waits for.
    static constexpr std::size_t latencyFrames = 2047;
    static constexpr int minimumScale = 2;
    static constexpr double minimumLoudness = 1.0 / 32767.0;
    static constexpr double maximumLoudness = 32767.0;
    static constexpr double minimumPrescale = 1.0 / 32767.0;

    // False, with no change, for a sample rate that is not positive, a scale
    // outside 2 to half a second of samples, a loudness outside 1/32767 to
    // 32767, or a prescale under 1/32767 or above 32767.
    // A live scale, which CDP takes as a breakpoint file, keeps room for
    // the most cycles a group can hold within the latency.
    [[nodiscard]] bool prepare(
        double sampleRate, int scale, double loudness, double prescale, bool liveScale = false);

    // CDP takes the scale and loudness as breakpoint files and reads them,
    // the scale rounded, where each group starts: a group starting in the
    // next process call takes these. False, with no change, before prepare
    // or for values outside prepare's ranges.
    [[nodiscard]] bool setControls(double scale, double loudness) noexcept;

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    void layMiniature(std::size_t end) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    int scale_ = 2;
    double loudness_ = 1.0;
    double prescale_ = 1.0;
    // What prepare gave, what the next group takes, and the largest scale.
    int startScale_ = 2;
    double startLoudness_ = 1.0;
    int nextScale_ = 2;
    double nextLoudness_ = 1.0;
    double maximumScale_ = 2.0;
    // The first frame of the stream starts a group.
    bool groupStarting_ = true;
    std::vector<float> ring_;
    // CDP's cycle buffer keeps what earlier groups left past this one's
    // samples, where a miniature's last index can reach.
    std::vector<float> miniature_;
    std::vector<std::size_t> cycleStarts_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    std::size_t start_ = 0;
    std::size_t kept_ = 0;
    int phase_ = 0;
    bool secondHalf_ = false;
    int cycles_ = 0;
    int count_ = 0;
};
}
