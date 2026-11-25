/*
 * Derived from the CDP System, Release 8: dev/env/envfuncs.c, dev/env/envprepro.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Tremolo.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace talas::cdp
{
namespace
{
// CDP's PI (include/globcon.h), to ten digits.
constexpr double pi = 3.141592654;

using SineTable = std::array<double, Tremolo::tableSize + 1>;

// create_tremol_sintable: a sine raised to run from 0 to 1, closed by its
// midpoint for interpolation across the wrap.
const SineTable& raisedSineTable() noexcept
{
    static const auto table = [] {
        SineTable values{};
        for (std::size_t index = 0; index < Tremolo::tableSize; ++index)
        {
            values[index] = std::sin(pi * 2.0
                * (static_cast<double>(index) / static_cast<double>(Tremolo::tableSize)));
            values[index] += 1.0;
            values[index] /= 2.0;
        }
        values[Tremolo::tableSize] = 0.5;
        return values;
    }();
    return table;
}
}

bool Tremolo::prepare(double sampleRate) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
    {
        *this = {};
        return false;
    }
    // Builds the shared table here, before any rendering thread reads it.
    static_cast<void>(raisedSineTable());
    sampleRate_ = sampleRate;
    reset();
    return true;
}

bool Tremolo::prepared() const noexcept
{
    return sampleRate_ > 0.0;
}

void Tremolo::reset() noexcept
{
    tablePosition_ = 0.0;
}

bool Tremolo::process(
    std::span<const double> input,
    double frequency,
    double depth,
    double gain,
    std::span<double> output) noexcept
{
    const auto inRange = [](double value, double maximum) {
        return std::isfinite(value) && value >= 0.0 && value <= maximum;
    };
    if (!prepared() || input.size() != output.size()
        || !inRange(frequency, maximumFrequency) || !inRange(depth, 1.0)
        || !inRange(gain, 1.0)
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    const auto& table = raisedSineTable();
    const auto tableSizeOverRate =
        static_cast<double>(tableSize) / sampleRate_;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        // envelope_tremol, statement for statement.
        const auto whole = static_cast<std::size_t>(tablePosition_);
        const auto fraction = tablePosition_ - static_cast<double>(whole);
        const auto low = table[whole];
        const auto high = table[whole + 1];
        auto value = low + ((high - low) * fraction);
        value *= depth;
        value += (1.0 - depth);
        value *= gain;
        tablePosition_ += (frequency * tableSizeOverRate);
        tablePosition_ = std::fmod(tablePosition_, static_cast<double>(tableSize));
        // CDP stores each product in its float sample buffer.
        output[frame] = static_cast<float>(input[frame] * value);
    }
    return true;
}
}
