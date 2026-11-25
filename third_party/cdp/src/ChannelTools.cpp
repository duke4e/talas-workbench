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
#include "talas/cdp/ChannelTools.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// Whole frames of finite input, and an output of the same frames.
[[nodiscard]] bool fits(std::span<const double> input, std::span<const double> output, std::size_t in,
    std::size_t out) noexcept
{
    if (in == 0 || input.size() % in != 0 || output.size() != input.size() / in * out)
        return false;
    return std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); });
}

[[nodiscard]] double asFloat(double sample) noexcept
{
    return static_cast<double>(static_cast<float>(sample));
}
}

// ---------------------------------------------------------------------------
// ChannelZero

bool ChannelZero::prepare(int channels, int channel, std::size_t bufferFrames) noexcept
{
    if (channels < 1 || channels > channelToolsMaximumChannels || channel < 1 || channel > channels)
        return false;
    channels_ = static_cast<std::size_t>(channels);
    channel_ = static_cast<std::size_t>(channel - 1);
    bufferFrames_ = bufferFrames == 0 ? defaultMonoBufferFrames : bufferFrames;
    frame_ = 0;
    return true;
}

bool ChannelZero::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || !fits(input, output, channels_, outputChannels()))
        return false;
    const std::size_t frames = input.size() / channels_;
    if (channels_ == 1)
    {
        // do_channels, HOUSE_ZCHANNEL of a mono buffer, built from its end:
        // frame 0's right copies the 0 just written to its left.
        for (std::size_t n = 0; n < frames; n++)
        {
            output[2 * n] = 0.0;
            output[2 * n + 1] = frame_ % bufferFrames_ == 0 ? 0.0 : asFloat(input[n]);
            frame_++;
        }
        return true;
    }
    for (std::size_t n = 0; n < frames; n++)
    {
        for (std::size_t c = 0; c < channels_; c++)
            output[n * channels_ + c] = c == channel_ ? 0.0 : asFloat(input[n * channels_ + c]);
    }
    frame_ += frames;
    return true;
}

// ---------------------------------------------------------------------------
// ChannelMixdown

bool ChannelMixdown::prepare(bool invertSecond) noexcept
{
    prepared_ = true;
    invert_ = invertSecond;
    return true;
}

bool ChannelMixdown::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared_ || !fits(input, output, 2, 1))
        return false;
    for (std::size_t m = 0; m < output.size(); m++)
    {
        // do_channels, STOM of a stereo buffer: a float sum or difference,
        // halved in double.
        const float left = static_cast<float>(input[2 * m]);
        const float right = static_cast<float>(input[2 * m + 1]);
        const float sum = invert_ ? left - right : left + right;
        output[m] = static_cast<double>(static_cast<float>(static_cast<double>(sum) / 2.0));
    }
    return true;
}

// ---------------------------------------------------------------------------
// MonoToStereo

bool MonoToStereo::prepare() noexcept
{
    prepared_ = true;
    return true;
}

bool MonoToStereo::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared_ || !fits(input, output, 1, 2))
        return false;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        const double sample = asFloat(input[n]);
        output[2 * n] = sample;
        output[2 * n + 1] = sample;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ChannelPhase

bool ChannelPhase::prepare(int channels, int channel) noexcept
{
    if (channels < 1 || channels > channelToolsMaximumChannels || channel < 1 || channel > channels)
        return false;
    channels_ = static_cast<std::size_t>(channels);
    channel_ = static_cast<std::size_t>(channel - 1);
    return true;
}

bool ChannelPhase::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || !fits(input, output, channels_, channels_))
        return false;
    for (std::size_t i = 0; i < input.size(); i++)
    {
        // invert_chan_phase: buf[i] = -buf[i].
        const float sample = static_cast<float>(input[i]);
        output[i] = static_cast<double>(i % channels_ == channel_ ? -sample : sample);
    }
    return true;
}

// ---------------------------------------------------------------------------
// MonoToMany

bool MonoToMany::prepare(int outputChannels) noexcept
{
    if (outputChannels < 2 || outputChannels > channelToolsMaximumChannels)
        return false;
    channels_ = static_cast<std::size_t>(outputChannels);
    return true;
}

bool MonoToMany::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || !fits(input, output, 1, channels_))
        return false;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        const double sample = asFloat(input[n]);
        for (std::size_t c = 0; c < channels_; c++)
            output[n * channels_ + c] = sample;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ChannelPair

bool ChannelPair::prepare(int channels, int first, int second) noexcept
{
    if (channels < minimumChannels || channels > channelToolsMaximumChannels || first < 1 || first > channels
        || second < 1 || second > channels || first == second)
        return false;
    channels_ = static_cast<std::size_t>(channels);
    first_ = static_cast<std::size_t>(first - 1);
    second_ = static_cast<std::size_t>(second - 1);
    return true;
}

bool ChannelPair::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || !fits(input, output, channels_, 2))
        return false;
    const std::size_t frames = input.size() / channels_;
    for (std::size_t n = 0; n < frames; n++)
    {
        output[2 * n] = asFloat(input[n * channels_ + first_]);
        output[2 * n + 1] = asFloat(input[n * channels_ + second_]);
    }
    return true;
}
}
