/*
 * Derived from the CDP System, Release 8: dev/env/envxtract.c,
 * dev/env/envimpos.c, dev/env/envfuncs.c, dev/env/envprocess.c,
 * dev/env/envprepro.c, dev/env/ap_envel.c, dev/cdp2k/tklib1.c and
 * dev/cdp2k/tklib3.c
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
// The windowed envelope CDP's `envel warp`, `impose` and `replace` share.
// CDP takes the peak |x| of each window of W frames (getmaxsamp) and
// imposes a new envelope on the sound, the gain ramping across window k
// from the new envelope's value k to its value k + 1 (step_gen: linearly,
// or along a power of ten when the linear step per frame would pass 1,
// after which every later window of the same CDP buffer interpolates so),
// each product clipped to -1 to 1 (sfsys.h's min and max, through which a
// NaN, from interpolating towards or away from 0 along powers of ten,
// comes out as -1, as in CDP).
//
// W is the window size in ms rounded to a multiple of 256 frames, or, under
// 256 frames, to the nearer power-of-two fraction of 256 (CDP's
// generate_samp_windowsize). The gain over window k needs window k + 1's
// envelope, and a mode may need D windows more, so the output comes
// latencyFrames() = (D + 2) W - 1 frames late. bufferFrames is CDP's
// buffer, which sets where the power-of-ten interpolation stops (CDP rounds
// it down to whole windows of at least 256 frames; CDP_MEMORY_BBSIZE sets
// other sizes).
//
// Before imposing, CDP runs the whole file to find the output's peak and,
// if that passes 1, scales the whole new envelope down to keep it at 1
// (test_impose_envel_on_sndfile, adjust_envelope). A stream cannot know its
// peak in advance: it matches CDP exactly whenever CDP's test pass finds no
// overload, and otherwise clips at -1 and 1 where CDP would have scaled.
//
// This class is the shared engine; EnvelopeWarp and EnvelopeImpose drive
// it.
class EnvelopeWindows
{
public:
    // envel.h's ENV_FSECSIZE.
    static constexpr std::size_t sectorFrames = 256;
    // CDP's default buffer: Malloc(-1)'s 1 MB of floats.
    static constexpr std::size_t defaultBufferFrames = 262144;
    // envlcon.h's ENV_MIN_WSIZE and ENV_MAX_WSIZE (CDP: up to its file's
    // duration).
    static constexpr double minimumWindowMs = 5.0;
    static constexpr double maximumWindowMs = 10000.0;
    // The longest latency any envel stream accepts.
    static constexpr std::size_t maximumLatencyFrames = std::size_t{1} << 20;

    // CDP's window in frames for a mono input at this rate, or 0 for a
    // sample rate that is not finite and positive or a window outside 5 to
    // 10000 ms.
    [[nodiscard]] static std::size_t windowFrames(double sampleRate, double windowMs) noexcept;

    // False, with no change, for no window, no buffer or a latency over
    // maximumLatencyFrames.
    [[nodiscard]] bool prepare(std::size_t windowFrames, std::size_t lookaheadWindows, std::size_t bufferFrames);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return window_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return latency_; }

    [[nodiscard]] std::size_t frames() const noexcept { return window_; }

    // True when the next frame pushed starts window windowIndex().
    [[nodiscard]] bool atWindowStart() const noexcept { return position_ == 0; }

    [[nodiscard]] std::size_t windowIndex() const noexcept { return windowIndex_; }

    // Takes a frame of the input (and of the side, whose envelope only is
    // kept); true when it completes a window, whose peaks peak() and
    // sidePeak() then give and whose index is windowIndex() - 1.
    [[nodiscard]] bool push(float sample, float side) noexcept;

    [[nodiscard]] float peak() const noexcept { return peak_; }
    [[nodiscard]] float sidePeak() const noexcept { return sidePeak_; }

    // Imposes the gain ramp from thisValue to nextValue on window k of the
    // input, which push has taken, in place (apply_env_to_window).
    void render(std::size_t k, float thisValue, float nextValue) noexcept;

    // The output frame for the frame pushed last: the input of
    // latencyFrames() earlier as rendered, or 0 before then.
    [[nodiscard]] double pull() const noexcept;

    bool operator==(const EnvelopeWindows&) const = default;

private:
    std::size_t window_ = 0;
    std::size_t latency_ = 0;
    std::size_t windowsPerBuffer_ = 1;
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    std::size_t position_ = 0;
    std::size_t windowIndex_ = 0;
    float running_ = 0.0F;
    float sideRunning_ = 0.0F;
    float peak_ = 0.0F;
    float sidePeak_ = 0.0F;
    bool isLog_ = false;
};

// CDP `envel warp`: the input's own envelope, warped, replaces it. CDP
// divides each warped value by the original (envreplace), holding the last
// ratio where the original is at or below 1/32767 (0 at the start, or where
// the warped value is that low too); Corrugate imposes its warped envelope
// directly.
//
// Each mode takes CDP's parameters 1 and 2 as first and second (CDP's
// parameter 3, Smoothing, is smoothing):
//
// | Mode        | CDP | first                | second                 |
// | Exaggerate  | 3   | Exaggerate 0.01-100  | -                      |
// | Attenuate   | 4   | Attenuate 0-1        | -                      |
// | Lift        | 5   | Lift 0-1             | -                      |
// | Flatten     | 7   | Flatten 2-5000 wins  | -                      |
// | Gate        | 8   | Gate 0-1             | -                      |
// | Invert      | 9   | Gate 0-1             | Mirror 0-1, > Gate     |
// | Limit       | 10  | Limit 0-1            | Threshold 0-Limit      |
// | Corrugate   | 11  | Trofdel 1-32767 wins | Peak separation 2-32767|
// | Expand      | 12  | Gate 0-1             | Threshold Gate-1       |
// | Ducked      | 15  | Gate 0-1             | Threshold 0-1          |
//
// Invert needs Gate at most Mirror - 0.000002 (FLTERR), Limit a Threshold
// at most Limit, Expand a Gate at most Threshold, as CDP's
// envel_preprocess. Gate and Expand take Smoothing 0 to 32767 windows.
//
// D, the windows a mode reads ahead: Flatten ceil(N / 2) - 1 for its
// largest N, smoothed Gate and Expand Smoothing - 1, Corrugate
// max(1, S / 2) + T - 1 for its largest Peak separation S and Trofdel T,
// the others 0. CDP's flatten pads the envelope at its file's end with its
// last window, which a stream, having no end, plays on past; Gate and
// Expand's smoothing averages fewer windows near CDP's file end; CDP holds
// its last window's value over that window, where the stream ramps on to
// the next. CDP's Corrugate starts its trough pass in the direction its
// envelope took into its file's last window; the stream starts as if it
// fell (as for any file whose envelope does not rise into its last
// window). CDP refuses a Flatten of as many windows as its file has, and a
// warped envelope that is everywhere at or below 1/32767, which a stream
// cannot know.
//
// CDP's modes 1, 2, 13, 14 and 16 (normalise, reverse, trigger, ceiling,
// peakcnt) need the whole file, and mode 6 (timestretch) reads window k's
// value from window k / Timestretch, which a stream would have to keep
// without bound (or, below 1, wait for without bound): none is streamable.
class EnvelopeWarp
{
public:
    enum class Mode
    {
        Exaggerate,
        Attenuate,
        Lift,
        Flatten,
        Gate,
        Invert,
        Limit,
        Corrugate,
        Expand,
        Ducked,
    };

    // envlcon.h's MIN_ENV_EXAG, MAX_ENV_EXAG, MAX_ENV_FLATN and
    // MAX_ENV_SMOOTH; globcon.h's BIG_VALUE (CDP's limit for Trofdel and
    // Peak separation). The latency limit holds all of them lower in
    // practice: (D + 2) W - 1 at most 2^20 frames.
    static constexpr double minimumExaggerate = 0.01;
    static constexpr double maximumExaggerate = 100.0;
    static constexpr int minimumFlatten = 2;
    static constexpr int maximumFlatten = 5000;
    static constexpr int maximumSmoothing = 32767;
    static constexpr int maximumCorrugate = 32767;

    // False, with no change, for a sample rate that is not finite and
    // positive, a window outside 5 to 10000 ms, values outside the table's
    // ranges (Flatten, Trofdel and Peak separation are rounded, as CDP
    // rounds them), a smoothing outside 0 to 32767 (ignored but by Gate and
    // Expand), a zero buffer, or a latency over
    // EnvelopeWindows::maximumLatencyFrames. maximumFirst and maximumSecond
    // are the largest live Flatten, Trofdel and Peak separation setControls
    // may set (CDP's flatten pads by its breakpoint file's largest value),
    // which set D; 0, or anything under the given value, means the given
    // value.
    [[nodiscard]] bool prepare(
        double sampleRate, Mode mode, double windowMs, double first, double second = 0.0, int smoothing = 0,
        int maximumFirst = 0, int maximumSecond = 0,
        std::size_t bufferFrames = EnvelopeWindows::defaultBufferFrames);

    // CDP takes these parameters as breakpoint files and reads them at each
    // window's start: windows starting in the next process call take these.
    // Modes with one parameter ignore second. False, with no change, before
    // prepare, for values
    // outside prepare's ranges or pairs CDP refuses, or a Flatten, Trofdel
    // or Peak separation over prepare's maximum.
    [[nodiscard]] bool setControls(double first, double second = 0.0) noexcept;

    // Back to prepare's parameters.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return engine_.prepared(); }

    // (D + 2) W - 1 frames, W the window.
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return engine_.latencyFrames(); }

    // The window in frames.
    [[nodiscard]] std::size_t windowFrames() const noexcept { return engine_.frames(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const EnvelopeWarp&) const = default;

private:
    struct Controls
    {
        double first = 0.0;
        double second = 0.0;
        bool operator==(const Controls&) const = default;
    };

    [[nodiscard]] bool valid(double first, double second) const noexcept;
    void completeWindow(std::size_t m) noexcept;
    void finishWindow(std::size_t j, float value) noexcept;
    [[nodiscard]] float warp(std::size_t j) noexcept;
    [[nodiscard]] bool gateFilter(std::size_t j, double gate) const noexcept;
    void corrugate(std::size_t m) noexcept;
    [[nodiscard]] bool shadowPeak(std::size_t r) const noexcept;
    void troughStep(std::size_t k) noexcept;
    [[nodiscard]] float& raw(std::size_t k) noexcept { return raw_[k & envelopeMask_]; }
    [[nodiscard]] float raw(std::size_t k) const noexcept { return raw_[k & envelopeMask_]; }

    EnvelopeWindows engine_;
    Mode mode_ = Mode::Exaggerate;
    Controls start_;
    Controls controls_;
    int smoothing_ = 0;
    int maximumFirst_ = 0;
    int maximumSecond_ = 0;
    std::size_t lookahead_ = 0;
    // Per window: the input's envelope, the parameters at its start, the
    // envelope imposed, and Corrugate's peaks and trough-dug envelope.
    std::vector<float> raw_;
    std::vector<Controls> windowControls_;
    std::vector<float> imposed_;
    std::vector<unsigned char> shadow_;
    std::vector<float> dug_;
    std::size_t envelopeMask_ = 0;
    // do_smoothed_gating's and do_expanding's gating, and envkorrug's
    // upwards in its trough pass.
    bool gating_ = true;
    bool upwards_ = false;
};

// CDP `envel impose` mode 1 and `envel replace` mode 1, the second sound's
// envelope taken from the Side Chain: Impose multiplies the input by the
// side's envelope; Replace divides the side's envelope by the input's
// (envreplace: holding the last ratio where the input's is at or below
// 1/32767, 0 at the start or where the side's is that low too), so the
// input takes the side's envelope. The output comes latencyFrames() = 2W - 1
// frames late. CDP holds the side's last envelope value past its file's
// end; the stream's side runs on. CDP refuses a replacement that leaves the
// whole envelope at or below 1/32767, which a stream cannot know.
class EnvelopeImpose
{
public:
    enum class Mode
    {
        Impose,
        Replace,
    };

    // False, with no change, for a sample rate that is not finite and
    // positive, a window outside 5 to 10000 ms, a zero buffer or a latency
    // over EnvelopeWindows::maximumLatencyFrames.
    [[nodiscard]] bool prepare(
        double sampleRate, Mode mode, double windowMs,
        std::size_t bufferFrames = EnvelopeWindows::defaultBufferFrames);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return engine_.prepared(); }

    // 2W - 1 frames, W the window.
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return engine_.latencyFrames(); }

    [[nodiscard]] std::size_t windowFrames() const noexcept { return engine_.frames(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

    bool operator==(const EnvelopeImpose&) const = default;

private:
    EnvelopeWindows engine_;
    Mode mode_ = Mode::Impose;
    float previous_ = 0.0F;
};

// CDP `envel replace` modes 3 and 4: the input's envelope is replaced by a
// target given as a breakpoint file of levels 0 to 1 (Level, mode 3) or of
// decibels -96 to 0 (Decibels, mode 4, each converted to a gain as CDP reads
// the file: -96 and below silent, within 0.000002 of 0 unity, otherwise
// 1 / 10^(-dB / 20)). CDP samples the file at each window's start
// (convert_brktable_to_envtable) and divides by the input's envelope
// (envreplace, as EnvelopeImpose's Replace); the stream takes Target as a
// live field, read at each window's start. The output comes
// latencyFrames() = 2W - 1 frames late. CDP holds its table's last value
// past the table's end; a stream has the field. Mode 2, a binary envelope
// file, is not ported.
class EnvelopeTarget
{
public:
    enum class Mode
    {
        Level,
        Decibels,
    };

    // globcon.h's MIN_DB_ON_16_BIT.
    static constexpr double minimumDecibels = -96.0;

    // False, with no change, for a sample rate that is not finite and
    // positive, a window outside 5 to 10000 ms, a target outside 0 to 1
    // (Level) or -96 to 0 dB (Decibels), a zero buffer or a latency over
    // EnvelopeWindows::maximumLatencyFrames.
    [[nodiscard]] bool prepare(
        double sampleRate, Mode mode, double windowMs, double target,
        std::size_t bufferFrames = EnvelopeWindows::defaultBufferFrames);

    // Windows starting in the next process call take this target. False,
    // with no change, before prepare or for a target outside prepare's
    // range.
    [[nodiscard]] bool setTarget(double target) noexcept;

    // Back to prepare's target.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return engine_.prepared(); }

    // 2W - 1 frames, W the window.
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return engine_.latencyFrames(); }

    [[nodiscard]] std::size_t windowFrames() const noexcept { return engine_.frames(); }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const EnvelopeTarget&) const = default;

private:
    [[nodiscard]] bool valid(double target) const noexcept;
    [[nodiscard]] float gain(double target) const noexcept;

    EnvelopeWindows engine_;
    Mode mode_ = Mode::Level;
    float start_ = 0.0F;
    // The target as CDP's envelope table holds it, and as latched at the
    // start of the window under way.
    float target_ = 0.0F;
    float latched_ = 0.0F;
    float previous_ = 0.0F;
};
}
