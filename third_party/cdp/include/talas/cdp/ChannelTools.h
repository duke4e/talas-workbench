/*
 * Derived from the CDP System, Release 8: dev/houskeep/channels.c,
 * dev/houskeep/ap_house.c, dev/new/chanphase.c, dev/standalone/mton.c,
 * dev/standnew/pairex.c and dev/cdp2k/tklib1.c and tklib3.c
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

namespace talas::cdp
{
// CDP's channel housekeeping, each frame on its own (no latency): the
// input and output are interleaved, a frame of each per frame. Each
// process call is false, with no change, before prepare, for spans that are
// not the same number of whole frames, or a non-finite input. The ports
// take 1 to 8 channels where CDP takes more.

// The most channels these ports take.
inline constexpr int channelToolsMaximumChannels = 8;

// CDP `housekeep chans 3`, ZERO ONE CHANNEL: Channel (1 to N) of an N-channel
// input set to 0. A mono input (Channel 1, CDP's only choice) becomes
// stereo, silent on the left and the input on the right; there CDP builds
// each buffer's stereo frames in place from the last back, so the first
// frame of each of its buffers copies the left's 0 to the right as well:
// the stream does the same every bufferFrames frames from its start (CDP's
// 16384 frames, CDP_MEMORY_BBSIZE giving other sizes). CDP writes a file of
// more than two channels as 16-bit unless its output is named with -f; the
// stream plays the float output.
class ChannelZero
{
public:
    // CDP's mono buffer: 1 MB over MAX_SNDFILE_OUTCHANS (16) buffers of
    // floats, in 256-frame sectors.
    static constexpr std::size_t defaultMonoBufferFrames = 16384;

    // False, with no change, for channels outside 1 to 8 or a channel
    // outside 1 to channels. bufferFrames 0 is defaultMonoBufferFrames.
    [[nodiscard]] bool prepare(int channels, int channel, std::size_t bufferFrames = 0) noexcept;

    // Frame 0 (the start of CDP's first buffer) next.
    void reset() noexcept { frame_ = 0; }

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t inputChannels() const noexcept { return channels_; }

    // Two for a mono input, otherwise the input's channels.
    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_ == 1 ? 2 : channels_; }

    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const ChannelZero&) const = default;

private:
    std::size_t channels_ = 0;
    std::size_t channel_ = 0;
    std::size_t bufferFrames_ = 0;
    std::size_t frame_ = 0;
};

// CDP `housekeep chans 4`, MIX DOWN TO MONO, of a stereo input: half the
// float sum of the two channels, or, with -p (invertSecond), half the float
// difference. CDP mixes more than two channels by a gain it sets from the
// whole file's peaks (maxinval / maxoutsum), which a stream cannot know:
// only stereo is ported. CDP refuses a mono input.
class ChannelMixdown
{
public:
    [[nodiscard]] bool prepare(bool invertSecond = false) noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t inputChannels() const noexcept { return 2; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return 1; }

    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const ChannelMixdown&) const = default;

private:
    bool prepared_ = false;
    bool invert_ = false;
};

// CDP `housekeep chans 5`, MONO TO STEREO: the mono input on both channels.
class MonoToStereo
{
public:
    [[nodiscard]] bool prepare() noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t inputChannels() const noexcept { return 1; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return 2; }

    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const MonoToStereo&) const = default;

private:
    bool prepared_ = false;
};

// CDP `chanphase chanphase`: Channel (1 to N) of an N-channel input negated
// (0 becomes -0), the others as they are. CDP pads its output to a whole
// buffer; the stream keeps the input's length.
class ChannelPhase
{
public:
    // False, with no change, for channels outside 1 to 8 or a channel
    // outside 1 to channels (chanphase.c: 1 to 16, and no more than the
    // input has).
    [[nodiscard]] bool prepare(int channels, int channel) noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t inputChannels() const noexcept { return channels_; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const ChannelPhase&) const = default;

private:
    std::size_t channels_ = 0;
    std::size_t channel_ = 0;
};

// CDP `mton mton`: the mono input on each of N output channels.
class MonoToMany
{
public:
    // mton.c's range for N: 2 to 16 (here to 8). False, with no change,
    // outside it.
    [[nodiscard]] bool prepare(int outputChannels) noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t inputChannels() const noexcept { return 1; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return channels_; }

    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const MonoToMany&) const = default;

private:
    std::size_t channels_ = 0;
};

// CDP `pairex pairex`: two channels of an input of 3 to 8 channels as a
// stereo pair, First on the left and Second on the right. CDP takes the
// pair as a two-digit number (12 to 87, both digits 1 to 8 and different)
// and checks only the first against the input's channels: a second past
// them reads the next frame's channels, which the port refuses.
class ChannelPair
{
public:
    // pairex.c: an input of more than two channels.
    static constexpr int minimumChannels = 3;

    // False, with no change, for channels outside 3 to 8, a first or second
    // outside 1 to channels, or the same channel twice.
    [[nodiscard]] bool prepare(int channels, int first, int second) noexcept;

    void reset() noexcept {}

    [[nodiscard]] bool prepared() const noexcept { return channels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    [[nodiscard]] std::size_t inputChannels() const noexcept { return channels_; }

    [[nodiscard]] std::size_t outputChannels() const noexcept { return 2; }

    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const ChannelPair&) const = default;

private:
    std::size_t channels_ = 0;
    std::size_t first_ = 0;
    std::size_t second_ = 0;
};
}
