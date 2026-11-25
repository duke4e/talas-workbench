/*
 * Derived from the CDP System, Release 8: dev/blur/blur.c and ap_blur.c,
 * dev/hilite/hilite.c and ap_hilite.c, and dev/cdp2k/tklib1.c and tklib3.c
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
#include <utility>
#include <vector>

namespace talas::cdp
{
class BlurBlur;
class HiliteBlurTrace;

// CDP's spec_blur_and_bltr, which `blur blur` and `hilite bltr` share:
// CDP keeps every blur-th analysis frame, reading the blur (a breakpoint
// file) at each kept frame's time, and replaces each frame from one kept
// frame up to the next with amplitudes and frequencies stepped linearly
// between the two (do_the_bltr, the step computed in float and added with
// one rounding, as CDP8's build fuses it). `hilite bltr` then keeps only
// the trace loudest channels of each such frame, reading the trace (a
// breakpoint file) at that frame's time.
//
// A frame so needs the next kept frame, up to the blur ahead. A stream
// bounds that with its maximum blur: analysis runs that many frames
// ahead of synthesis, each frame reaches synthesis as CDP writes it, and the
// output is CDP's, maximumBlur analysis frames (times their spacing D) later
// on top of the phase vocoder's latency. Analysis and synthesis are two
// phase vocoders, the second starting maximumBlur * D frames into the
// stream with CDP's first frame: `pvoc synth` drops what its first frames
// give before time 0 and turns each frame by its time, so one vocoder
// synthesising silent frames first would not give CDP's output. A blur or
// trace given before a process call reaches the frames analysed during it.
//
// CDP shortens the last blur to end at its file's last frame and writes
// that frame as it is, untraced (spec_blur_and_bltr after its loop); a
// stream has no end, and differs from CDP from the first frame after the
// last kept frame whose blur reaches past the file's end (for `hilite
// bltr`, at the latest from the file's last frame).
class BlurAndTrace
{
private:
    friend class BlurBlur;
    friend class HiliteBlurTrace;

    [[nodiscard]] static std::optional<std::size_t> latencyFor(int points, int overlap, int maximumBlur) noexcept;
    [[nodiscard]] bool prepare(
        double sampleRate, int points, int overlap, double blur, int maximumBlur, bool tracing, double trace);
    [[nodiscard]] bool validBlur(double blur) const noexcept;
    [[nodiscard]] bool validTrace(double trace) const noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return analysis_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept
    {
        return synthesis_.latencyFrames() + delay_;
    }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;
    void analysed(std::span<const float> frame) noexcept;
    void bltr(std::span<const float> frame) noexcept;
    [[nodiscard]] float* slot(std::size_t window) noexcept;

    PhaseVocoder analysis_;
    PhaseVocoder synthesis_;
    int clength_ = 0;
    int maximumBlur_ = 1;
    // The frames before synthesis starts (maximumBlur * D), and those played
    // of them.
    std::size_t delay_ = 0;
    std::size_t waited_ = 0;
    // The analysis vocoder's discarded output and the synthesis vocoder's
    // (discarded) input, a frame spacing each.
    std::vector<double> discard_;
    std::vector<double> silence_;
    // The frames written and waiting for synthesis, maximumBlur + 1 of them,
    // by window modulo that count; and each waiting window's trace.
    std::vector<float> ring_;
    std::vector<int> traces_;
    // CDP's amp and freq (the last kept frame), ampdif and freqdif.
    std::vector<float> amp_;
    std::vector<float> freq_;
    std::vector<float> ampdif_;
    std::vector<float> freqdif_;
    // BLUR_BLURF and BLTR_TRACE as read, and as prepared.
    int blur_ = 1;
    int trace_ = 1;
    int startBlur_ = 1;
    int startTrace_ = 1;
    bool tracing_ = false;
    // spec_blur_and_bltr's blurcnt, blurfactor, last_total_windows and
    // total_windows; the windows synthesised.
    int blurcnt_ = 0;
    int blurfactor_ = 0;
    std::size_t lastTotalWindows_ = 0;
    std::size_t totalWindows_ = 0;
    std::size_t synthesised_ = 0;
    // hilite bltr's loudest channels (chan, amp), as many as the channels.
    std::vector<std::pair<int, float>> loudest_;
};

// `blur blur`: the blur from 1 to maximumBlur. CDP's blur runs to its
// file's frame count; a stream's maximum blur, from 1 to maximumBlurLimit,
// fixes the latency: a static blur takes maximumBlur equal to it.
class BlurBlur
{
public:
    // CDP's DEFAULT_BLUR, and the most a stream's maximum blur can be.
    static constexpr int defaultBlur = 16;
    static constexpr int maximumBlurLimit = 1024;

    // The latency prepare gives those points, overlap and maximum blur: the
    // phase vocoder's, and maximumBlur times its frame spacing. Nothing for
    // what prepare refuses.
    [[nodiscard]] static std::optional<std::size_t> latencyFor(int points, int overlap, int maximumBlur) noexcept
    {
        return BlurAndTrace::latencyFor(points, overlap, maximumBlur);
    }

    // False, with no change, for a maximum blur outside 1 to
    // maximumBlurLimit, a blur that does not round to 1 to it or what the
    // phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double blur, int maximumBlur)
    {
        return core_.prepare(sampleRate, points, overlap, blur, maximumBlur, false, 1.0);
    }
    // False, with no change, before prepare or for a blur that does not
    // round to 1 to the maximum blur.
    [[nodiscard]] bool setBlur(double blur) noexcept;
    void reset() noexcept { core_.reset(); }
    [[nodiscard]] bool prepared() const noexcept { return core_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return core_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept
    {
        return core_.process(input, output);
    }

private:
    BlurAndTrace core_;
};

// `hilite bltr`: the blur as for BlurBlur, and the trace, the loudest
// channels kept in each frame, from 1 to the channel count. CDP keeps a
// list of the trace loudest channels, emptying only its amplitudes at each
// frame: a place no sounding channel fills keeps the channel it last held,
// which is then silenced after the louder channels are written, as CDP's
// create_trace writes it, even when it is one of them.
class HiliteBlurTrace
{
public:
    // CDP's DEFAULT_BLUR and DEFAULT_TRACE.
    static constexpr int defaultBlur = 16;
    static constexpr int defaultTrace = 16;
    static constexpr int maximumBlurLimit = BlurBlur::maximumBlurLimit;

    [[nodiscard]] static std::optional<std::size_t> latencyFor(int points, int overlap, int maximumBlur) noexcept
    {
        return BlurAndTrace::latencyFor(points, overlap, maximumBlur);
    }

    // False, with no change, for a maximum blur outside 1 to
    // maximumBlurLimit, a blur that does not round to 1 to it, a trace that
    // does not round to 1 to the channel count (points / 2 + 1) or what the
    // phase vocoder refuses.
    [[nodiscard]] bool prepare(
        double sampleRate, int points, int overlap, double blur, double trace, int maximumBlur)
    {
        return core_.prepare(sampleRate, points, overlap, blur, maximumBlur, true, trace);
    }
    // False, with no change, before prepare or outside those ranges.
    [[nodiscard]] bool setControls(double blur, double trace) noexcept;
    void reset() noexcept { core_.reset(); }
    [[nodiscard]] bool prepared() const noexcept { return core_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return core_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept
    {
        return core_.process(input, output);
    }

private:
    BlurAndTrace core_;
};
}
