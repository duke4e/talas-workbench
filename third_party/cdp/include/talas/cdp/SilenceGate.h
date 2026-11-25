/*
 * Derived from the CDP System, Release 8: dev/standalone/gate.c,
 * dev/cdp2k/tklib1.c and dev/cdp2k/tklib3.c
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
#include <vector>

namespace talas::cdp
{
// CDP's standalone `gate gate` mode 1: a run of samples below the gate level
// longer than two 1 ms splices is silenced, faded out over the splice at its
// start and back in over the splice at its end; a run at the very start of
// the stream needs only to be longer than one splice, and is silenced from
// its first sample.
//
// CDP first finds every run in its whole file, then gates the file. Its
// search has a fault the port keeps: after a run that reaches the end of
// one of its sound buffers, it skips on by as many samples as the run had
// before that buffer, to at most that buffer's end, missing any run that
// starts there (or finding only the part of it after the skip). So the
// stream emulates CDP's buffers: bufferFrames, counted from the stream's
// start, is CDP's buffer length (CDP_MEMORY_BBSIZE sets other sizes). A
// start run of one to two splices puts CDP's fade back in that many frames
// late, and each later gating starts that much later too, until a long
// enough run takes the delay up; the port does the same.
//
// The stream waits two splices to know whether a run is long enough, so
// the output comes latencyFrames() late. CDP refuses a file where nothing,
// or everything, would be gated, a whole-file test a stream cannot make, so
// the stream plays on; at its file's end CDP gates a final run of any
// length over a splice, and drops the rest of its file when its last gating
// ends exactly at a buffer's end, neither of which a stream has.
class SilenceGate
{
public:
    static constexpr double minimumDecibels = -96.0;
    static constexpr double maximumDecibels = 0.0;
    // CDP's two sound buffers for a mono input: its default 1 MB of memory
    // shared between them.
    static constexpr std::size_t defaultBufferFrames = 131072;

    // False, with no change, for a sample rate that is not finite or gives a
    // splice under one frame (round(0.001 * sampleRate)), a gate level
    // outside -96 to 0 dB or no more than one over the sample rate as a gain
    // (CDP's "effectively zero"), or no buffer frames.
    [[nodiscard]] bool prepare(
        double sampleRate, double gateDecibels, std::size_t bufferFrames = defaultBufferFrames);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    // Two splices.
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 2 * splicelen_; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const SilenceGate&) const = default;

private:
    enum class Stage
    {
        Copy,
        ZeroStart,
        DownSplice,
        Zeros,
        UpSplice,
    };

    struct Cut
    {
        std::size_t start = 0;
        std::size_t end = 0;
        bool open = false;
        bool operator==(const Cut&) const = default;
    };

    // At most two gatings lie between the search and the output, which
    // waits two splices; the queue holds twice that.
    static constexpr std::size_t maximumCuts = 4;

    void search(std::size_t frame, float sample) noexcept;
    [[nodiscard]] float gateFrame(std::size_t frame) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }
    [[nodiscard]] Cut& cut(std::size_t index) noexcept { return cuts_[(cutHead_ + index) % maximumCuts]; }

    double level_ = 1.0;
    std::size_t splicelen_ = 0;
    double incr_ = 0.0;
    std::size_t bufferFrames_ = defaultBufferFrames;
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    // CDP's first pass: the next sample its search tests, and the run under
    // way.
    std::size_t nextScan_ = 0;
    bool inRun_ = false;
    std::size_t runStart_ = 0;
    bool runQueued_ = false;
    std::array<Cut, maximumCuts> cuts_{};
    std::size_t cutHead_ = 0;
    std::size_t cutCount_ = 0;
    // CDP's second pass: how far its read position runs ahead of its count
    // of ungated samples, and the splice under way.
    Stage stage_ = Stage::Copy;
    std::size_t drift_ = 0;
    std::size_t count_ = 0;
    double splice_ = 0.0;
};
}
