/*
 * Derived from the CDP System, Release 8: dev/standnew/phasor.c and
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

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `phasor phasor`: copies of a mono input that drift out of phase and
// back, spread over the output channels. CDP cuts the input into packets of
// P frames, P = floor(sr / Phasfrq) raised to a multiple of 4. Stream 0
// plays the packet as it is, on channel 0; each other stream n reads it at
// a rate that rises along a semitone curve to Shift n / (Streams - 1)
// semitones and back (the second half of the packet the mirror image, so
// that the stream comes back in step), interpolating linearly, and sits
// between two channels (equal-power pancalc gains, along a line or, with
// surround, around a circle). Stream n also starts Offset n / (Streams - 1)
// ms late.
//
// CDP's quirks are kept: a stream reads one frame ahead of stream 0; it
// reads no further than frame P + 1 of CDP's input buffer, a buffer of
// ceil(sr / the lowest Phasfrq) frames raised to a multiple of 4, plus 16,
// rounded up to 256, which CDP reloads from the packet's start only when
// the packet would pass its end (so with a buffer longer than two packets,
// at low sample rates or with a varying Phasfrq, every other packet's
// streams read past frame P of the buffer as frame P, extrapolating).
//
// CDP's output buffer is two of its input buffers (times the channels)
// plus, with an Offset, an extension of ceil(Offset) frames (times the
// channels) plus 16, up to 256 samples: as it moves on by one input buffer,
// it moves the extension down over itself and then clears where the
// extension was, so where the extension is longer than an input buffer it
// loses the later part of the offset streams. The stream keeps that loss.
//
// A packet needs the input up to frame P + 1 after its start: the output
// comes latencyFrames() = Pmax + 1 frames late, Pmax the packet for the
// lowest Phasfrq prepare allows, and each packet is computed in the call
// that takes its last input frame. CDP reads its Phasfrq and Shift
// breakpoint files once per packet, at a time that it advances by 1 /
// Phasfrq per packet (where the packet advances P frames); the stream takes
// the controls set when a packet's first frame comes in, so a host that
// steps them where CDP's packet times fall plays alike.
//
// CDP runs the whole process once to find its output's peak and, if that
// passes 0.95, scales the whole output to peak at 0.95. A stream cannot know
// its peak in advance: it matches CDP bit for bit whenever CDP's first pass
// finds a peak at or below 0.95, and otherwise plays unscaled, clipped at
// -1 and 1 where CDP would have scaled. CDP's output stops after the packet
// that its time passes the input's duration in (its length then differs
// from the input's when P is not sr / Phasfrq); the stream goes on.
class Phasor
{
public:
    // phasor.c's ranges.
    static constexpr int minimumStreams = 2;
    static constexpr int maximumStreams = 8;
    static constexpr double minimumFrequency = 0.01;
    static constexpr double maximumFrequency = 100.0;
    static constexpr double minimumShift = 0.0;
    static constexpr double maximumShift = 12.0;
    static constexpr double maximumOffsetMs = 500.0;
    // The largest packet the stream takes (CDP: 100 s at its rate).
    static constexpr std::size_t maximumPacketFrames = std::size_t{1} << 20;

    // CDP's packet for this Phasfrq: floor(sr / Phasfrq) raised to a
    // multiple of 4.
    [[nodiscard]] static std::size_t packetFrames(double sampleRate, double frequency) noexcept;

    // False, with no change, for a sample rate that is not finite and
    // positive, values outside the ranges (within 0.000002 of a range end
    // is that end, as CDP's readdata takes it), more output channels than
    // streams, surround with fewer than 3 output channels, or a packet over
    // maximumPacketFrames. lowestFrequency is the lowest Phasfrq setControls
    // may set; it sets the latency and CDP's input buffer (CDP's is set by
    // the lowest Phasfrq it reads); NaN, or a value above frequency, means
    // frequency.
    [[nodiscard]] bool prepare(double sampleRate, int streams, double frequency, double shift, int outputChannels,
        double offsetMs = 0.0, bool surround = false,
        double lowestFrequency = std::numeric_limits<double>::quiet_NaN());

    // Phasfrq and Shift for the packets starting from the next process call
    // on. False, with no change, before prepare, for values outside the
    // ranges, or a Phasfrq under prepare's lowest.
    [[nodiscard]] bool setControls(double frequency, double shift) noexcept;

    // Silence, prepare's controls, the first packet starting with the next
    // frame.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    // Pmax + 1 frames.
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return latency_; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    // The output is interleaved, outputChannels() samples a frame. False,
    // with no change, before prepare, for an output that is not
    // outputChannels() times the input, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const Phasor&) const = default;

private:
    struct Packet
    {
        std::size_t start = 0;
        std::size_t frames = 0;
        double shift = 0.0;
        bool operator==(const Packet&) const = default;
    };
    struct Placement
    {
        std::size_t first = 0;
        double firstLevel = 0.0;
        std::size_t second = 0;
        double secondLevel = 0.0;
        bool operator==(const Placement&) const = default;
    };

    [[nodiscard]] bool valid(double& frequency, double& shift) const noexcept;
    void computePacket(const Packet& packet) noexcept;
    [[nodiscard]] float input(std::size_t frame) const noexcept { return ring_[frame & ringMask_]; }

    double sampleRate_ = 0.0;
    std::size_t streams_ = 0;
    std::size_t channels_ = 0;
    double lowest_ = 0.0;
    double startFrequency_ = 0.0;
    double startShift_ = 0.0;
    double frequency_ = 0.0;
    double shift_ = 0.0;
    std::size_t latency_ = 0;
    std::size_t bufferFrames_ = 0;
    std::size_t outputBufferSamples_ = 0;
    std::size_t offsetWrapSamples_ = 0;
    std::array<std::size_t, maximumStreams> offsets_{};
    std::size_t maximumOffset_ = 0;
    std::array<Placement, maximumStreams> placements_{};

    std::vector<float> ring_;
    std::size_t ringMask_ = 0;
    std::vector<float> out_;
    std::size_t outMask_ = 0;
    std::vector<double> increments_;
    std::vector<float> packetBuffer_;

    std::size_t frame_ = 0;
    std::size_t nextStart_ = 0;
    std::array<Packet, 4> pending_{};
    std::size_t pendingFirst_ = 0;
    std::size_t pendingCount_ = 0;
    std::size_t base_ = 0;
    std::size_t bufferPosition_ = 0;
    std::size_t outputBase_ = 0;
};
}
