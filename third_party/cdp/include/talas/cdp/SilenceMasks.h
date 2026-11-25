/*
 * Derived from the CDP System, Release 8: dev/editsf/cut.c,
 * dev/editsf/ap_edit.c, dev/cdp2k/tklib1.c and dev/cdp2k/readdata.c
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
// How CDP's sfedit modes give times: mode 1 in seconds, mode 2 in samples
// (of all channels, taken down to a whole frame), mode 3 in frames
// ("stereo samples").
enum class EditUnits
{
    Seconds,
    Samples,
    Frames,
};

// CDP `sfedit masks` (INSERTSIL_MANY, do_insertsil_many): silence over each
// of a list of start and end times, with splices. The splice, Splice ms
// rounded to frames and then up to an even number L, is centred on each
// time: the sound fades out over the L frames centred on a start (the gain
// stepping from 1 - 1/L down by 1/L a frame, as a running sum), is 0 to the
// splice centred on the end, and fades back in over it (from 0 up by 1/L).
// A first mask starting at 0 is silent from frame 0 (CDP fades its first L
// frames out first, which the silence then covers as far as it reaches; a
// fade in starting sooner fades those frames again).
//
// CDP fades in buffers of 1 MB of samples (1048576 / N frames for N
// channels, down to whole 512-frame sectors; CDP_MEMORY_BBSIZE gives other
// sizes): a splice that runs into the next buffer restarts its gain there
// from its position counted in samples rather than frames, so, past more
// than one channel, the gain jumps at the buffer's start; the stream does
// the same every bufferFrames frames from its start. The times are known
// in advance, so the stream needs no lookahead; CDP refuses times past its
// file's end, which a stream cannot know. Any channels, interleaved. No
// latency.
class SilenceMasks
{
public:
    static constexpr int maximumChannels = 8;
    // edit.h's EDIT_SPLICELEN and EDIT_SPLICEMAX.
    static constexpr double defaultSpliceMs = 15.0;
    static constexpr double maximumSpliceMs = 5000.0;

    // CDP's buffer for N channels.
    [[nodiscard]] static std::size_t defaultBufferFrames(int channels) noexcept;

    // times holds start, end pairs, as CDP's text file does. False, with no
    // change, for a sample rate that is not finite and positive, channels
    // outside 1 to 8, no pairs or an odd count, a time that is not finite,
    // a Splice outside 0 to 5000 ms (within 0.000002 of either end is that
    // end), or the times CDP refuses: a start that is negative, an end not
    // after its start, a start not after the end before it, a start other
    // than 0 within half a splice of 0, a mask too short for its two half
    // splices, or masks too close together for their splices. Samples and
    // Frames are whole numbers, as CDP reads them (anything after the point
    // ignored). bufferFrames 0 is defaultBufferFrames(channels); a splice
    // longer than the buffer is refused.
    [[nodiscard]] bool prepare(double sampleRate, int channels, EditUnits units, std::span<const double> times,
        double spliceMs = defaultSpliceMs, std::size_t bufferFrames = 0);

    // Frame 0 (the start of CDP's first buffer) next.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The splice in frames.
    [[nodiscard]] std::size_t spliceFrames() const noexcept { return splice_; }

    // False, with no change, before prepare, for spans of different sizes or
    // not whole frames, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const SilenceMasks&) const = default;

private:
    // One mask in frames: the fade out from fadeOut (L frames), silence from
    // silence to fadeIn, and the fade in from fadeIn (L frames).
    struct Mask
    {
        std::size_t fadeOut = 0;
        std::size_t silence = 0;
        std::size_t fadeIn = 0;
        bool operator==(const Mask&) const = default;
    };

    // A fade under way: do_other_splice's running gain for the frame after
    // the last one faded.
    struct Fade
    {
        double next = 0.0;
        bool operator==(const Fade&) const = default;
    };

    // do_other_splice's gain on frame `frame` of the fade from `start`,
    // restarted at the fade's start and at each CDP buffer's.
    [[nodiscard]] double gain(Fade& fade, std::size_t start, std::size_t frame, bool down) const noexcept;

    std::size_t channels_ = 0;
    std::size_t splice_ = 0;
    std::size_t bufferFrames_ = 0;
    // do_other_splice's aincr, 1 / L.
    double step_ = 0.0;
    std::vector<Mask> masks_;
    std::size_t frame_ = 0;
    // The first mask not yet over, and its fades.
    std::size_t next_ = 0;
    Fade out_;
    Fade in_;
};

// CDP `sfedit insil` with -o (EDIT_INSERTSIL, overwriting): silence of
// Duration from Time, with splices of Splice ms (rounded to frames, L),
// written over the sound: the sound fades out over the L frames before
// Time (dosplice: from 1 - 1/L down by 1/L a frame, as a running sum), is 0
// from Time, fades back in over the L frames from Time + Duration - 2L (from
// 0 up by 1/L) and is itself again from Time + Duration - L, the length
// unchanged. Each faded frame is CDP's 0 plus the faded sample, in float
// (so a faded -0 is 0).
//
// CDP refuses a Time within L frames of its file's end, and, without -s, a
// silence running past it; with -s it leaves the end silent, as the stream
// does whenever its input ends before the fade in. Without -o insil
// inserts the silence, lengthening the sound: not streamable. Any channels,
// interleaved. No latency.
class SilenceOverwrite
{
public:
    static constexpr int maximumChannels = 8;
    static constexpr double defaultSpliceMs = 15.0;
    static constexpr double maximumSpliceMs = 5000.0;

    // False, with no change, for a sample rate that is not finite and
    // positive, channels outside 1 to 8, a Time or Duration that is not
    // finite or is negative, a Splice outside 0 to 5000 ms (within 0.000002
    // of either end is that end), or what CDP refuses: a Duration shorter
    // than two splices, or a Time within a splice of the start. Samples and
    // Frames are rounded to whole numbers, as CDP reads them.
    [[nodiscard]] bool prepare(double sampleRate, int channels, EditUnits units, double time, double duration,
        double spliceMs = defaultSpliceMs);

    // Frame 0 next.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    [[nodiscard]] std::size_t spliceFrames() const noexcept { return splice_; }

    // False, with no change, before prepare, for spans of different sizes or
    // not whole frames, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const SilenceOverwrite&) const = default;

private:
    std::size_t channels_ = 0;
    std::size_t splice_ = 0;
    // Frames: the fade out's start, the silence's start, the fade in's start
    // and the sound's return.
    std::size_t fadeOut_ = 0;
    std::size_t silence_ = 0;
    std::size_t fadeIn_ = 0;
    std::size_t back_ = 0;
    double step_ = 0.0;
    std::size_t frame_ = 0;
    // dosplice's running gain, for the frame frame_ when in a fade.
    double running_ = 0.0;
};
}
