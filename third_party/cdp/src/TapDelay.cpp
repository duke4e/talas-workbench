/*
 * Derived from the CDP System, Release 8: dev/externals/reverb/tdelaymain.cpp
 * and reverberator.cpp
 * Copyright (c) 1983-2013 Richard Dobson and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/TapDelay.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace talas::cdp
{
namespace
{
// tdelaymain.cpp's ROOT2.
constexpr double root2 = 1.4142136;
// reverberator.cpp's SAFETY: the line is two frames longer than its
// longest tap.
constexpr double safetyFrames = 2.0;

// pancalc, statement for statement.
void panGains(double position, double& leftGain, double& rightGain) noexcept
{
    const auto signalToLeft = position < 0.0;
    const auto relativePosition = position < 0 ? -position : position;
    if (relativePosition <= 1.0)
    {
        auto temp = 1.0 + (relativePosition * relativePosition);
        const auto relativeDistance = root2 / std::sqrt(temp);
        temp = (position + 1.0) / 2.0;
        rightGain = temp * relativeDistance;
        leftGain = (1.0 - temp) * relativeDistance;
        return;
    }
    const auto temp = (relativePosition * relativePosition) + 1.0;
    const auto relativeDistance = std::sqrt(temp) / root2;
    const auto inverseSquare = 1.0 / (relativeDistance * relativeDistance);
    leftGain = signalToLeft ? inverseSquare : 0.0;
    rightGain = signalToLeft ? 0.0 : inverseSquare;
}

[[nodiscard]] bool space(char character) noexcept
{
    return character == ' ' || character == '\t' || character == '\r'
        || character == '\v' || character == '\f';
}

// Up to three numbers as sscanf's "%lf%lf%lf" reads them, stopping at the
// first that does not parse. Returns how many it read.
int scanNumbers(std::string_view line, double (&values)[3]) noexcept
{
    int count = 0;
    std::size_t position = 0;
    while (count < 3)
    {
        while (position < line.size() && space(line[position]))
            ++position;
        if (position < line.size() && line[position] == '+')
            ++position;
        double value = 0.0;
        const auto* first = line.data() + position;
        const auto* last = line.data() + line.size();
        const auto [end, error] = std::from_chars(first, last, value);
        if (error != std::errc{} || end == first)
            break;
        values[count++] = value;
        position = static_cast<std::size_t>(end - line.data());
    }
    return count;
}
}

std::optional<DelayTapList> readDelayTaps(std::string_view text, double sampleRate)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return std::nullopt;
    const auto sampleDuration = 1.0 / sampleRate;
    DelayTapList list;
    auto currentTime = 0.0;
    std::size_t begin = 0;
    while (begin <= text.size())
    {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        const auto line = text.substr(begin, end - begin);
        begin = end + 1;
        // tdelaymain.cpp skips empty lines and lines starting with a
        // semicolon; a line of spaces is skipped too.
        if (line.empty() || line.front() == ';'
            || std::ranges::all_of(line, space))
        {
            if (newline == std::string_view::npos)
                break;
            continue;
        }
        double values[3] = {0.0, 0.0, 0.0};
        const auto got = scanNumbers(line, values);
        if (got < 2)
            return std::nullopt;
        auto time = values[0];
        const auto amplitude = values[1];
        if (!std::isfinite(time) || time < 0.0)
            return std::nullopt;
        // A time under one sample is the direct sound, which takes the
        // place of Mix.
        if (time < sampleDuration)
        {
            time = 0.0;
            list.directTap = true;
        }
        if (currentTime != 0.0 && time == currentTime)
        {
            if (newline == std::string_view::npos)
                break;
            continue;
        }
        if (time < currentTime)
            return std::nullopt;
        currentTime = time;
        if (!(amplitude >= 0.0 && amplitude <= 1.0))
            return std::nullopt;
        DelayTap tap{time, amplitude, 0.0};
        if (got == 3)
        {
            if (!std::isfinite(values[2]))
                return std::nullopt;
            tap.pan = values[2];
            list.stereo = true;
        }
        if (list.taps.size() == TapDelay::maximumTaps)
            return std::nullopt;
        list.taps.push_back(tap);
        if (newline == std::string_view::npos)
            break;
    }
    if (list.taps.empty())
        return std::nullopt;
    return list;
}

bool TapDelay::prepare(
    double sampleRate,
    const DelayTapList& taps,
    double tapGain,
    double feedback,
    double mix)
{
    const auto feedbackFloat = static_cast<float>(feedback);
    const auto dryFactor = static_cast<float>(mix);
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || taps.taps.empty() || taps.taps.size() > maximumTaps
        || !std::isfinite(tapGain) || tapGain <= 0.0
        || !std::isfinite(feedback) || feedbackFloat < -1.0F || feedbackFloat > 1.0F
        || !std::isfinite(mix) || dryFactor < 0.0F || dryFactor >= 1.0F
        || !std::ranges::all_of(taps.taps, [](const DelayTap& tap) {
               return std::isfinite(tap.time) && tap.time >= 0.0
                   && tap.time <= maximumTapSeconds && std::isfinite(tap.amplitude)
                   && std::isfinite(tap.pan);
           })
        || taps.taps.back().time * sampleRate + safetyFrames
            >= static_cast<double>(std::numeric_limits<unsigned int>::max()))
    {
        return false;
    }

    // tapdelay::create for one channel's taps.
    const auto create = [&](Line& line, auto value) {
        const auto count = taps.taps.size();
        const auto length = static_cast<unsigned int>(
            (taps.taps.back().time * sampleRate) + safetyFrames);
        line.buffer.assign(length, 0.0);
        line.pointers.assign(count, 0);
        line.coefficients.assign(count, 0.0);
        double sum = 0.0;
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto& tap = taps.taps[index];
            line.pointers[index] = tap.time == 0.0
                ? 0U
                : length - static_cast<unsigned int>(static_cast<int>(tap.time * sampleRate));
            line.coefficients[index] = value(tap);
            sum += std::fabs(line.coefficients[index]);
        }
        if (sum == 0.0)
            return std::optional<double>{};
        line.startPointers = line.pointers;
        line.reader = 0;
        line.oneOverTaps = 1.0 / static_cast<double>(count);
        return std::optional<double>{static_cast<double>(count) / sum};
    };

    Line left;
    Line right;
    std::optional<double> leftGain;
    std::optional<double> rightGain;
    if (taps.stereo)
    {
        leftGain = create(left, [](const DelayTap& tap) {
            double leftPan = 0.0;
            double rightPan = 0.0;
            panGains(tap.pan, leftPan, rightPan);
            return tap.amplitude * leftPan;
        });
        rightGain = create(right, [](const DelayTap& tap) {
            double leftPan = 0.0;
            double rightPan = 0.0;
            panGains(tap.pan, leftPan, rightPan);
            return tap.amplitude * rightPan;
        });
        if (!leftGain.has_value() || !rightGain.has_value())
            return false;
    }
    else
    {
        leftGain = create(left, [](const DelayTap& tap) { return tap.amplitude; });
        if (!leftGain.has_value())
            return false;
    }
    auto maximumGain = taps.stereo
        ? static_cast<float>(std::min(*leftGain, *rightGain))
        : static_cast<float>(*leftGain);
    maximumGain *= static_cast<float>(tapGain);
    left.outputGain = static_cast<double>(maximumGain);
    right.outputGain = static_cast<double>(maximumGain);

    left_ = std::move(left);
    right_ = std::move(right);
    stereo_ = taps.stereo;
    mixInput_ = !taps.directTap;
    feedback_ = feedbackFloat;
    dryFactor_ = dryFactor;
    wetFactor_ = 1.0F - dryFactor;
    maximumGain_ = maximumGain;
    prepared_ = true;
    reset();
    return true;
}

void TapDelay::reset() noexcept
{
    for (auto* line : {&left_, &right_})
    {
        std::ranges::fill(line->buffer, 0.0);
        std::ranges::copy(line->startPointers, line->pointers.begin());
        line->reader = 0;
    }
    leftFedBack_ = 0.0F;
    rightFedBack_ = 0.0F;
}

// tapdelay::tick, statement for statement.
double TapDelay::tick(Line& line, double sample) noexcept
{
    double val = 0.0;
    line.buffer[line.reader++] = sample;
    if (line.reader == line.buffer.size())
        line.reader = 0;
    for (std::size_t index = 0; index < line.pointers.size(); ++index)
    {
        val += ((line.buffer[line.pointers[index]++]) * (line.coefficients[index]));
        if (line.pointers[index] == line.buffer.size())
            line.pointers[index] = 0;
    }
    val *= line.oneOverTaps;
    return val * line.outputGain;
}

bool TapDelay::process(
    std::span<const double> input,
    std::span<double> left,
    std::span<double> right) noexcept
{
    if (!prepared_ || input.size() != left.size() || input.size() != right.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    // tdelaymain.cpp's loops for a mono input, keeping its float samples.
    auto l_op = leftFedBack_;
    auto r_op = rightFedBack_;
    const auto feedback = feedback_;
    const auto wetfac = wetFactor_;
    const auto dryfac = dryFactor_;
    const auto max_gain = maximumGain_;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto l_ip = static_cast<float>(input[frame]);
        const auto ip = l_ip;
        float l_out = 0.0F;
        float r_out = 0.0F;
        if (stereo_)
        {
            if (feedback != 0.0F)
            {
                l_op = static_cast<float>(tick(left_, ip + (l_op * feedback)));
                r_op = static_cast<float>(tick(right_, ip + (r_op * feedback)));
            }
            else
            {
                l_op = static_cast<float>(tick(left_, ip));
                r_op = static_cast<float>(tick(right_, ip));
            }
            if (mixInput_)
            {
                l_out = l_op * wetfac + l_ip * dryfac;
                r_out = r_op * wetfac;
                r_out += l_ip * dryfac;
            }
            else
            {
                l_out = l_op;
                r_out = r_op;
            }
            l_out *= max_gain;
            r_out *= max_gain;
        }
        else
        {
            if (feedback != 0.0F)
                l_op = static_cast<float>(tick(left_, ip + l_op * feedback));
            else
                l_op = static_cast<float>(tick(left_, ip));
            if (mixInput_)
                l_out = l_op * wetfac + l_ip * dryfac;
            else
                l_out = l_op;
            l_out *= max_gain;
            r_out = l_out;
        }
        left[frame] = static_cast<double>(l_out);
        right[frame] = static_cast<double>(r_out);
    }
    leftFedBack_ = l_op;
    rightFedBack_ = r_op;
    return true;
}
}
