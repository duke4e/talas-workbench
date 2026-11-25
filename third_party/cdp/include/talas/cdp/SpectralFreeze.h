/*
 * Derived from the CDP System, Release 8: dev/focus/focus.c and
 * ap_focus.c, and dev/cdp2k/tklib3.c
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

#include "talas/cdp/PhaseVocoder.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// CDP `focus freeze 1-3 infile outfile datafile` between `pvoc anal` and
// `pvoc synth`: the data file's times (seconds, in order) cut the sound into
// segments, and a time marked `a` freezes the segment after it on the window
// at that time, one marked `b` the segment before it on the window at that
// time; mode 1 freezes the channels' amplitudes, mode 2 their frequencies,
// mode 3 both. A segment between unmarked times, or after a `b`, passes as
// it is. The data's times are read as CDP reads them (read_specfreeze_data,
// get_frztimes and rationalise_frztimes, whose loop moves only one of the
// later times when it drops a time between two unfrozen segments, so some
// data loses a freeze in CDP8, as here).
//
// A segment frozen on a later window needs that window: the stream waits
// for the longest such segment of its data, at most maximumLookahead
// windows (times their spacing D) on top of the phase vocoder's latency,
// with two phase vocoders as `blur blur` has them. A `b` segment longer than
// that is cut: its windows more than maximumLookahead windows before the
// time pass as they are. Otherwise the output is CDP's, except that CDP
// reads a frozen window, or starts a frozen segment, on a multiple of its
// 20-window buffer from past the buffer (memory the stream does not have),
// and that the stream has no end: CDP stops reading at its file's end and
// freezes a last `b` time past it on whatever its buffer then holds.
class FocusFreeze
{
public:
    enum class Mode
    {
        Amplitudes = 1,
        Frequencies,
        Both,
    };

    // The most windows a stream waits for a `b` time.
    static constexpr int maximumLookahead = 1024;

    // The latency prepare gives those points, overlap and data at that
    // sample rate: the phase vocoder's, and the windows the data needs, at
    // most maximumLookahead, times the frame spacing. Nothing for what
    // prepare refuses.
    [[nodiscard]] static std::optional<std::size_t> latencyFor(
        double sampleRate, int points, int overlap, std::string_view data);

    // False, with no change, for a mode outside 1 to 3, data CDP refuses (no
    // time, a time without `a` or `b` throughout, an unknown flag, times out
    // of order or within a window of each other, `a` followed by `b`), or
    // what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, std::string_view data);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return analysis_.prepared(); }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return synthesis_.latencyFrames() + delay_; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    struct Segment
    {
        // The segment's first window and the next segment's.
        long long start = 0;
        long long end = 0;
        // FRZ_FRZTIME: the window it freezes on, or -1.
        long long freeze = -1;
        // The template slot the window is kept in, or -1 when the stream
        // never reaches it in time.
        int slot = -1;
    };

    // The segments CDP's data gives, and the windows the stream waits for.
    struct Plan
    {
        std::vector<Segment> segments;
        int lookahead = 0;
        int slots = 0;
    };

    [[nodiscard]] static std::optional<Plan> plan(double sampleRate, int points, int overlap, std::string_view data);

    void analysed(std::span<const float> frame) noexcept;
    void synthesised(std::span<float> frame) noexcept;
    [[nodiscard]] float* ring(std::size_t window) noexcept;

    PhaseVocoder analysis_;
    PhaseVocoder synthesis_;
    Mode mode_ = Mode::Both;
    std::vector<Segment> segments_;
    // The segments by the window they freeze on, for capturing it.
    std::vector<std::size_t> byFreeze_;
    int lookahead_ = 0;
    std::size_t delay_ = 0;
    std::size_t waited_ = 0;
    std::vector<double> discard_;
    std::vector<double> silence_;
    // The analysed windows waiting for synthesis, lookahead + 1 of them, and
    // the windows frozen on, a slot each.
    std::vector<float> ring_;
    std::vector<float> templates_;
    std::size_t analysed_ = 0;
    std::size_t synthesised_ = 0;
    std::size_t nextCapture_ = 0;
    std::size_t segment_ = 0;
};
}
