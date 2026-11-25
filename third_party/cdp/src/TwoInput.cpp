/*
 * Derived from the CDP System, Release 8: dev/modify/radical.c,
 * dev/new/subtract.c, dev/submix/mixmerge.c and mixprepro.c, and
 * dev/cdp2k/tklib1.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/TwoInput.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace talas::cdp
{
namespace
{
// globcon.h's PI, to ten digits.
constexpr double pi = 3.141592654;
// globcon.h's FLTERR, flteq's tolerance.
constexpr double flterr = 0.000002;

using CosineTable = std::array<double, MixCrossfade::tableSize + 1>;

// gen_mcr_table: a raised cosine rising from 0 to 1 over the table, closed
// by 1.
const CosineTable& crossfadeCosineTable() noexcept
{
    static const auto table = [] {
        CosineTable values{};
        std::size_t n = 0;
        for (; n < MixCrossfade::tableSize; n++)
        {
            values[n] = (static_cast<double>(n) / static_cast<double>(MixCrossfade::tableSize)) * pi;
            values[n] = std::cos(values[n]);
            values[n] += 1.0;
            values[n] /= 2.0;
            values[n] = 1.0 - values[n];
            values[n] = std::max(0.0, values[n]);
            values[n] = std::min(values[n], 1.0);
        }
        values[n] = 1.0;
        return values;
    }();
    return table;
}

// read_cos, statement for statement (CDP8's build fuses none of it).
[[nodiscard]] double readCos(double d) noexcept
{
    const auto& costable = crossfadeCosineTable();
    d *= static_cast<double>(MixCrossfade::tableSize);
    const int j = static_cast<int>(d);
    const int k = j + 1;
    if (k > static_cast<int>(MixCrossfade::tableSize))
        return 1.0;
    double frac = d - static_cast<double>(j);
    const double diff = costable[static_cast<std::size_t>(k)] - costable[static_cast<std::size_t>(j)];
    frac *= diff;
    return costable[static_cast<std::size_t>(j)] + frac;
}

[[nodiscard]] bool inRange(double value, double minimum, double maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

[[nodiscard]] bool finite(std::span<const double> samples) noexcept
{
    return std::ranges::all_of(samples, [](double sample) {
        return std::isfinite(sample);
    });
}

[[nodiscard]] bool validSpans(
    std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    return input.size() == output.size() && side.size() == output.size() && finite(input) && finite(side);
}
}

bool CrossModulate::prepare() noexcept
{
    prepared_ = true;
    return true;
}

bool CrossModulate::process(
    std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    if (!prepared_ || !validSpans(input, side, output))
        return false;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        // cross_modulate's mono case: obuf[m] = (float)(ibuf1[m] * ibuf2[m]),
        // a float product.
        const auto product = static_cast<float>(input[n]) * static_cast<float>(side[n]);
        output[n] = static_cast<double>(product);
    }
    return true;
}

bool Subtract::prepare() noexcept
{
    prepared_ = true;
    return true;
}

bool Subtract::process(
    std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    if (!prepared_ || !validSpans(input, side, output))
        return false;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        // subtract: ibuf1[n] = (float)(ibuf1[n] - ibuf2[m]), a float
        // difference.
        const auto difference = static_cast<float>(input[n]) - static_cast<float>(side[n]);
        output[n] = static_cast<double>(difference);
    }
    return true;
}

bool MixBalance::prepare(double balance) noexcept
{
    if (!inRange(balance, 0.0, 1.0))
        return false;
    prepared_ = true;
    startBalance_ = balance;
    reset();
    return true;
}

bool MixBalance::setBalance(double balance) noexcept
{
    if (!prepared_ || !inRange(balance, 0.0, 1.0))
        return false;
    balance_ = balance;
    return true;
}

void MixBalance::reset() noexcept
{
    balance_ = startBalance_;
}

bool MixBalance::process(
    std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    if (!prepared_ || !validSpans(input, side, output))
        return false;
    const double atten = balance_;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        // do_stitch's mono case: buf0 * atten + buf1 * (1.0 - atten), whose
        // first product CDP8's build fuses into the sum.
        const auto first = static_cast<double>(static_cast<float>(input[n]));
        const auto second = static_cast<double>(static_cast<float>(side[n]));
        output[n] = static_cast<double>(static_cast<float>(std::fma(first, atten, second * (1.0 - atten))));
    }
    return true;
}

bool MixCrossfade::prepare(
    double sampleRate, Mode mode, double begin, double end, double stagger, double powerFactor)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || mode < Mode::Linear || mode > Mode::Cosine
        || !inRange(begin, 0.0, maximumTime) || !inRange(end, 0.0, maximumTime)
        || !inRange(stagger, 0.0, maximumStaggerSeconds)
        || !inRange(powerFactor, minimumPowerFactor, maximumPowerFactor))
    {
        return false;
    }
    // CDP holds each time in frames in an int.
    constexpr auto maximumFrames = static_cast<double>(std::numeric_limits<int>::max());
    if (begin * sampleRate > maximumFrames || end * sampleRate > maximumFrames
        || stagger * sampleRate > maximumFrames)
    {
        return false;
    }
    // mixcross_preprocess: each time rounded to frames (CDP's round is
    // lround).
    const auto beginFrames = std::llround(begin * sampleRate);
    const auto staggerFrames = std::llround(stagger * sampleRate);
    const auto endFrames = std::llround(end * sampleRate);
    if (beginFrames < staggerFrames || endFrames - beginFrames <= 0)
        return false;
    std::vector<float> delay(static_cast<std::size_t>(staggerFrames), 0.0F);
    // Builds the shared table here, before any rendering thread reads it.
    static_cast<void>(crossfadeCosineTable());
    if (mode == Mode::Linear)
        contour_ = Contour::Linear;
    else if (!(powerFactor > 1.0 + flterr || powerFactor < 1.0 - flterr))
        contour_ = Contour::Cosine; // flteq(powfac, 1.0)
    else
        contour_ = Contour::Skewed;
    powerFactor_ = powerFactor;
    begin_ = beginFrames;
    end_ = endFrames;
    stagger_ = static_cast<std::size_t>(staggerFrames);
    crossfadeFactor_ = 1.0 / static_cast<double>(endFrames - beginFrames);
    delay_ = std::move(delay);
    reset();
    return true;
}

void MixCrossfade::reset() noexcept
{
    std::ranges::fill(delay_, 0.0F);
    delayPosition_ = 0;
    frame_ = 0;
}

double MixCrossfade::sideLevel(long long index) const noexcept
{
    // cross: xxx = (double)crosindex * crosfact, then the contour.
    double xxx = static_cast<double>(index) * crossfadeFactor_;
    switch (contour_)
    {
    case Contour::Linear:
        break;
    case Contour::Cosine:
        xxx = readCos(xxx);
        break;
    case Contour::Skewed:
        xxx = std::pow(xxx, powerFactor_);
        xxx = readCos(xxx);
        break;
    }
    return xxx;
}

bool MixCrossfade::process(
    std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    if (!prepared() || !validSpans(input, side, output))
        return false;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        const auto first = static_cast<float>(input[n]);
        auto second = static_cast<float>(side[n]);
        if (stagger_ > 0)
        {
            // cross_stagger: CDP's second file starts Stagger frames into the
            // first.
            const float delayed = delay_[delayPosition_];
            delay_[delayPosition_] = second;
            delayPosition_ = delayPosition_ + 1 == stagger_ ? 0 : delayPosition_ + 1;
            second = delayed;
        }
        float sample = first;
        if (frame_ >= end_)
        {
            // mix_cross: after the fade, the second file as it is.
            sample = second;
        }
        else if (frame_ >= begin_)
        {
            // cross: obuf = inbuf1 * yyy + inbuf2 * xxx, whose first product
            // CDP8's build fuses into the sum.
            const double xxx = sideLevel(frame_ - begin_);
            const double yyy = 1.0 - xxx;
            sample = static_cast<float>(
                std::fma(static_cast<double>(first), yyy, static_cast<double>(second) * xxx));
        }
        output[n] = static_cast<double>(sample);
        if (frame_ < end_)
            frame_++;
    }
    return true;
}
}
