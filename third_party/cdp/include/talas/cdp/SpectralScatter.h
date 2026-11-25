/*
 * Derived from the CDP System, Release 8: dev/blur/blur.c and ap_blur.c,
 * dev/cdp2k/tklib1.c and tklib3.c, and dev/sfsys/osbind.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
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
#include "talas/cdp/PhaseVocoder.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `blur scatter` on each frame between `pvoc anal` and `pvoc synth`.
// CDP takes keep and the block size as breakpoint files and reads them at
// each analysis frame's time: a value given before a process call reaches
// the frames analysed during it.
//
// Each frame's channels fall into blocks of the block size, rounded to
// whole channels (at each frame, for a breakpoint block size), and CDP
// draws keep different blocks at random (with -r, first a count from 1 to
// keep), silencing the rest, then normalises the frame to its first total
// amplitude unless told not to (-n). CDP moves on only past the blocks it
// keeps, so each dropped block silences the place after the kept blocks so
// far: the frame keeps its lowest blocks, as many as the kept blocks that
// directly follow another kept one (or start the frame), and the port
// keeps that. CDP never seeds its generator, so the draws are the same at
// each run and after reset.
class BlurScatter
{
public:
    // CDP's channel width, the least block size and the default for -b,
    // for that sample rate and those points; nothing for what the phase
    // vocoder refuses.
    [[nodiscard]] static std::optional<double> channelWidthFor(double sampleRate, int points) noexcept;

    // Keep from 1 to the channel count, rounded, below the frame's block
    // count (the channel count over the rounded block size, rounded up), as
    // CDP checks at each frame, and the block size in Hz from one channel
    // width to a quarter of the sample rate. False, with no change, outside
    // those ranges or for what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double keep, double blockSize,
        bool randomiseCount, bool normalise);
    // False, with no change, before prepare or outside those ranges.
    [[nodiscard]] bool setControls(double keep, double blockSize) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // CDP's channel width after prepare.
    [[nodiscard]] double channelWidth() const noexcept { return chwidth_; }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    [[nodiscard]] bool valid(double keep, double blockSize) const noexcept;
    void scatter(std::span<float> frame) noexcept;

    PhaseVocoder vocoder_;
    int clength_ = 0;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    // SCAT_CNT and SCAT_BLOKSIZE, in channels.
    int keep_ = 1;
    int bloksize_ = 1;
    int startKeep_ = 1;
    int startBloksize_ = 1;
    bool randomiseCount_ = false;
    bool normalise_ = true;
    Drand48 random_;
    // SCAT_KEEP, the kept blocks in ascending order.
    std::vector<int> keepTable_;
};
}
