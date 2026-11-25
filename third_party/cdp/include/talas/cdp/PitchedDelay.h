/*
 * Derived from the CDP System, Release 8: dev/new/newdelay.c,
 * dev/cdp2k/tklib1.c and dev/cdp2k/readdata.c
 * Copyright (c) 1983-2013 Trevor Wishart, Richard Dobson and Composers
 * Desktop Project Ltd
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
#include <limits>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `newdelay newdelay`: a comb tuned to a MIDI pitch. The delay is one
// period of the pitch, rounded to frames (lround(sr / miditohz(pitch))); the
// line feeds back on itself by Feedback, the output takes Mix of the line
// and 1 - Mix of the input, raised by one more than Feedback, as revecho's
// standard delay (domulti) does.
//
// CDP reads its Midipitch breakpoint file at every frame and recomputes the
// delay there; its read and write pointer wraps once by the new delay after
// each frame, so a shorter delay walks the pointer down over a few frames
// and a longer one plays on into what the line last held there. A live
// pitch set before a process call takes effect from its first frame, as
// CDP's breakpoint step there would.
//
// Before writing, CDP runs the whole file, tail included, and, if its
// output would peak above 1, divides the input by that peak
// (domulti_test). A stream cannot know its peak in advance: it matches CDP
// bit for bit whenever CDP's test pass finds no overload, and otherwise
// clips at -1 and 1 where CDP would have scaled. CDP's tail, which rings
// on past its input's end, is not played (the stream goes on instead).
//
// Mono: CDP delays each channel of a multichannel file alike, so one
// processor per channel plays it. No latency.
class PitchedDelay
{
public:
    // newdelay.c's ranges: Mix 0.001 to 1, Feedback -1 to 1, Midipitch from
    // hztomidi(0.1 Hz) to hztomidi(the Nyquist frequency).
    static constexpr double minimumMix = 0.001;
    static constexpr double maximumMix = 1.0;
    static constexpr double minimumFeedback = -1.0;
    static constexpr double maximumFeedback = 1.0;
    static constexpr double lowestHz = 0.1;

    // hztomidi(0.1), about -76.24.
    [[nodiscard]] static double lowestPitch() noexcept;

    // hztomidi(sampleRate / 2), or NaN for a sample rate that is not finite
    // and positive.
    [[nodiscard]] static double highestPitch(double sampleRate) noexcept;

    // The delay CDP plays for this pitch, in frames.
    [[nodiscard]] static long delayForPitch(double sampleRate, double midiPitch) noexcept;

    // False, with no change, for a sample rate that is not finite and
    // positive, or a pitch, Mix or Feedback outside CDP's ranges (a value
    // within 0.000002 of a range end is taken as that end, as CDP's
    // readdata does). lowestLivePitch is the lowest pitch setPitch may set
    // (CDP sizes its line by its breakpoint file's lowest pitch); NaN, or a
    // value above midiPitch, means midiPitch.
    [[nodiscard]] bool prepare(double sampleRate, double midiPitch, double mix, double feedback,
        double lowestLivePitch = std::numeric_limits<double>::quiet_NaN());

    // The pitch from the next process call's first frame. False, with no
    // change, before prepare or for a pitch under prepare's lowest live
    // pitch or above highestPitch (snapped to these ends as prepare
    // snaps).
    [[nodiscard]] bool setPitch(double midiPitch) noexcept;

    // Silence in the line, prepare's pitch.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !line_.empty(); }

    [[nodiscard]] static constexpr std::size_t latencyFrames() noexcept { return 0; }

    // The delay now playing, in frames.
    [[nodiscard]] std::size_t delayFrames() const noexcept { return delay_; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const PitchedDelay&) const = default;

private:
    [[nodiscard]] bool snap(double& pitch, double lowest) const noexcept;

    std::vector<double> line_;
    double sampleRate_ = 0.0;
    double startPitch_ = 0.0;
    double lowest_ = 0.0;
    double highest_ = 0.0;
    double gain_ = 0.0;
    double feedback_ = 0.0;
    double feedForward_ = 0.0;
    std::size_t startDelay_ = 0;
    std::size_t delay_ = 0;
    std::size_t pointer_ = 0;
};
}
