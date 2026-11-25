/*
 * Derived from the CDP System, Release 8: dev/submix/mixmerge.c, inbetwee.c,
 * mixprepro.c and ap_mix.c, dev/standnew/repair.c, and dev/cdp2k/tklib1.c,
 * tklib3.c and readdata.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SubmixPairs.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR, flteq's tolerance.
constexpr double flterr = 0.000002;

[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    return !(f1 > f2 + flterr || f1 < f2 - flterr);
}

// A value as CDP's readers take it (read_param_as_value_or_brkfile_and_check_range
// for a number, get_brkpnt_data_from_file_and_test_it for a breakpoint):
// within FLTERR of either end, that end; nothing outside the range.
[[nodiscard]] std::optional<double> inCdpRange(double value, double minimum, double maximum) noexcept
{
    if (!std::isfinite(value))
        return std::nullopt;
    if (flteq(value, minimum))
        value = minimum;
    else if (flteq(value, maximum))
        value = maximum;
    if (value < minimum || value > maximum)
        return std::nullopt;
    return value;
}

[[nodiscard]] bool finite(std::span<const double> samples) noexcept
{
    return std::ranges::all_of(samples, [](double sample) {
        return std::isfinite(sample);
    });
}

[[nodiscard]] bool validSpans(
    std::span<const double> input, std::span<const double> side, std::size_t size) noexcept
{
    return input.size() == size && side.size() == size && finite(input) && finite(side);
}
}

bool MixMerge::prepare(double sampleRate, double stagger, double skew, double skip)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return false;
    const auto snappedStagger = inCdpRange(stagger, 0.0, maximumStaggerSeconds);
    const auto snappedSkew = inCdpRange(skew, minimumSkew, maximumSkew);
    // tklib1.c's MIXTWO range for Skip runs to BIG_TIME.
    const auto snappedSkip = inCdpRange(skip, 0.0, 32767.0);
    if (!snappedStagger || !snappedSkew || !snappedSkip)
        return false;
    // CDP holds each count of samples in an int.
    constexpr auto maximumFrames = static_cast<double>(std::numeric_limits<int>::max());
    if (*snappedStagger * sampleRate > maximumFrames || *snappedSkip * sampleRate > maximumFrames)
        return false;
    // mixtwo_preprocess: each time rounded to frames (CDP's round is
    // lround), then down to a whole number of sectors.
    constexpr auto sector = static_cast<long long>(sectorFrames);
    const auto staggerFrames = (std::llround(*snappedStagger * sampleRate) / sector) * sector;
    const auto skipFrames = (std::llround(*snappedSkip * sampleRate) / sector) * sector;
    // CDP's second file would be read ahead of its first.
    if (skipFrames > staggerFrames)
        return false;
    std::vector<float> delay(static_cast<std::size_t>(staggerFrames - skipFrames), 0.0F);
    // mixtwo_preprocess: MIX2_GAIN1 = skew / (skew + 1), MIX2_GAIN2 = 1 less.
    inputGain_ = *snappedSkew / (*snappedSkew + 1.0);
    sideGain_ = 1.0 - inputGain_;
    stagger_ = static_cast<std::size_t>(staggerFrames);
    delay_ = std::move(delay);
    prepared_ = true;
    reset();
    return true;
}

void MixMerge::reset() noexcept
{
    std::ranges::fill(delay_, 0.0F);
    delayPosition_ = 0;
    frame_ = 0;
}

bool MixMerge::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    if (!prepared_ || !validSpans(input, side, output.size()))
        return false;
    const std::size_t delayFrames = delay_.size();
    for (std::size_t n = 0; n < input.size(); n++)
    {
        const auto first = static_cast<double>(static_cast<float>(input[n]));
        auto second = static_cast<float>(side[n]);
        if (delayFrames > 0)
        {
            // CDP's second file enters Stagger frames in, Skip frames into
            // itself: the side Stagger - Skip frames late.
            const float delayed = delay_[delayPosition_];
            delay_[delayPosition_] = second;
            delayPosition_ = delayPosition_ + 1 == delayFrames ? 0 : delayPosition_ + 1;
            second = delayed;
        }
        float sample = 0.0F;
        if (frame_ < stagger_)
        {
            // do_mix2_stagger: obuf = (float)(inbuf1 * MIX2_GAIN1).
            sample = static_cast<float>(first * inputGain_);
            frame_++;
        }
        else
        {
            // do_mix2: dblbuf = inbuf1 * MIX2_GAIN1, then dblbuf += inbuf2 *
            // MIX2_GAIN2, which CDP8's build fuses; obuf = (float)dblbuf.
            sample = static_cast<float>(std::fma(static_cast<double>(second), sideGain_, first * inputGain_));
        }
        output[n] = static_cast<double>(sample);
    }
    return true;
}

bool MixFaders::prepare(double sampleRate, std::span<const Line> balance, double gain)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || balance.empty() || balance.front().time != 0.0)
        return false;
    const auto snappedGain = inCdpRange(gain, 0.0, maximumGain);
    if (!snappedGain)
        return false;
    // read_and_scale_balance_data's errtime: every time after the first must
    // be at least a frame past the first's (lasttime, which CDP never moves
    // on from the first line).
    const double errtime = 1.0 / sampleRate;
    constexpr auto maximumFrames = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    std::vector<ScaledLine> lines;
    lines.reserve(balance.size());
    for (std::size_t n = 0; n < balance.size(); n++)
    {
        const auto& line = balance[n];
        if (!std::isfinite(line.time) || !std::isfinite(line.input) || !std::isfinite(line.side))
            return false;
        if (n > 0 && line.time < 0.0 + errtime)
            return false;
        if (line.time * sampleRate > maximumFrames)
            return false;
        if (line.input < 0.0 || line.side < 0.0)
            return false;
        // read_and_scale_balance_data: sum the line's levels from its last
        // file back, and scale each by one over the sum.
        double sum = 0.0;
        sum += line.side;
        sum += line.input;
        if (sum <= 0.0)
            return false;
        sum = 1.0 / sum;
        ScaledLine scaled;
        // do_automix: nexttime = (unsigned int)round(time * srate * chans).
        scaled.frame = static_cast<std::uint32_t>(std::lround(line.time * sampleRate));
        scaled.level[0] = line.input * sum;
        scaled.level[1] = line.side * sum;
        lines.push_back(scaled);
    }
    lines_ = std::move(lines);
    startGain_ = *snappedGain;
    reset();
    return true;
}

bool MixFaders::setGain(double gain) noexcept
{
    if (!prepared())
        return false;
    const auto snapped = inCdpRange(gain, 0.0, maximumGain);
    if (!snapped)
        return false;
    gain_ = *snapped;
    return true;
}

void MixFaders::reset() noexcept
{
    gain_ = startGain_;
    // do_automix's start: the first line's time and levels are next.
    nextLine_ = 0;
    nextFrame_ = lines_.empty() ? 0 : lines_.front().frame;
    lastLine_ = false;
    level_ = {};
    increment_ = {};
    frame_ = 0;
}

void MixFaders::startSegment() noexcept
{
    // do_automix's outer loop: thistime = nexttime, thislevel = nextlevel;
    // then the next line's time and levels and the increments that ramp to
    // them over the frames between (an unsigned difference, as CDP's), or,
    // past the last line, none.
    const std::uint32_t thisFrame = nextFrame_;
    level_ = lines_[nextLine_].level;
    nextLine_++;
    if (nextLine_ < lines_.size())
    {
        nextFrame_ = lines_[nextLine_].frame;
        const auto frames = static_cast<double>(static_cast<std::uint32_t>(nextFrame_ - thisFrame));
        for (std::size_t k = 0; k < level_.size(); k++)
            increment_[k] = (lines_[nextLine_].level[k] - level_[k]) / frames;
    }
    else
    {
        increment_ = {};
        lastLine_ = true;
    }
}

bool MixFaders::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    if (!prepared() || !validSpans(input, side, output.size()))
        return false;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        // do_automix: while(outcnt < nexttime) runs each ramp; a line CDP's
        // count has reached or passed starts the next.
        while (!lastLine_ && !(frame_ < nextFrame_))
            startSegment();
        // sum += sample * thislevel, which CDP8's build fuses, then
        // thislevel += incr, file by file.
        double sum = 0.0;
        sum = std::fma(static_cast<double>(static_cast<float>(input[n])), level_[0], sum);
        level_[0] += increment_[0];
        sum = std::fma(static_cast<double>(static_cast<float>(side[n])), level_[1], sum);
        level_[1] += increment_[1];
        output[n] = static_cast<double>(static_cast<float>(sum * gain_));
        if (!lastLine_)
            frame_++;
    }
    return true;
}

bool MixInterleave::prepare() noexcept
{
    prepared_ = true;
    return true;
}

bool MixInterleave::process(std::span<const double> input, std::span<const double> side, std::span<double> left,
    std::span<double> right) noexcept
{
    if (!prepared_ || right.size() != left.size() || !validSpans(input, side, left.size()))
        return false;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        // copy_to_channel (repair's loop alike): each file's float sample to
        // its channel.
        left[n] = static_cast<double>(static_cast<float>(input[n]));
        right[n] = static_cast<double>(static_cast<float>(side[n]));
    }
    return true;
}

bool MixInbetween::prepare(double ratio) noexcept
{
    // read_inbetween_ratios: 0 to 1, with no FLTERR.
    if (!std::isfinite(ratio) || ratio < 0.0 || ratio > 1.0)
        return false;
    ratio_ = ratio;
    prepared_ = true;
    return true;
}

bool MixInbetween::prepareCount(int count, int file) noexcept
{
    if (count < 1 || count > maximumCount || file < 1 || file > count)
        return false;
    // get_inbetween_ratios: scaler = 1/(count + 1), the nth ratio n * scaler.
    const double scaler = 1.0 / static_cast<double>(count + 1);
    ratio_ = static_cast<double>(file) * scaler;
    prepared_ = true;
    return true;
}

bool MixInbetween::process(
    std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    if (!prepared_ || !validSpans(input, side, output.size()))
        return false;
    const double ratio = ratio_;
    const double invratio = 1.0 - ratio;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        // do_between_mix: (float)((buf1 * invratio) + (buf2 * ratio)), whose
        // first product CDP8's build fuses into the sum.
        const auto first = static_cast<double>(static_cast<float>(input[n]));
        const auto second = static_cast<double>(static_cast<float>(side[n]));
        output[n] = static_cast<double>(static_cast<float>(std::fma(first, invratio, second * ratio)));
    }
    return true;
}
}
