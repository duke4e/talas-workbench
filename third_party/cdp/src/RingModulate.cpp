/*
 * Derived from the CDP System, Release 8: dev/modify/radical.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/RingModulate.h"

#include <array>
#include <cmath>

namespace talas::cdp
{
namespace
{
// CDP's TWOPI (include/globcon.h), from a ten-digit PI.
constexpr double twoPi = 2.0 * 3.141592654;

using SineTable = std::array<double, RingModulate::tableSize + 1>;

// create_rm_sintab: one cycle over the table, then a closing zero that
// read_rmod_tab interpolates towards.
const SineTable& sineTable() noexcept
{
    static const auto table = [] {
        SineTable values{};
        for (std::size_t index = 0; index < RingModulate::tableSize; ++index)
        {
            values[index] = std::sin(twoPi * static_cast<double>(index)
                / static_cast<double>(RingModulate::tableSize));
        }
        values[RingModulate::tableSize] = 0.0;
        return values;
    }();
    return table;
}
}

bool RingModulate::prepare(double sampleRate) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
    {
        *this = {};
        return false;
    }
    // Builds the shared table here, before any rendering thread reads it.
    static_cast<void>(sineTable());
    sampleRate_ = sampleRate;
    reset();
    return true;
}

bool RingModulate::prepared() const noexcept
{
    return sampleRate_ > 0.0;
}

void RingModulate::reset() noexcept
{
    tableIndex_ = 0.0;
    pitch_ = 0.0;
    nextPitch_ = 0.0;
    pitchStep_ = 0.0;
    framesLeftInBlock_ = 0;
    started_ = false;
}

bool RingModulate::validFrequency(double frequency) const noexcept
{
    return std::isfinite(frequency) && frequency >= minimumFrequency
        && frequency <= sampleRate_ / 2.0;
}

bool RingModulate::process(
    std::span<const double> input,
    std::optional<std::span<const double>> connectedFrequency,
    double fallbackFrequency,
    std::span<double> output) noexcept
{
    if (sampleRate_ <= 0.0 || input.size() != output.size()
        || (connectedFrequency.has_value()
            && connectedFrequency->size() != input.size()))
    {
        return false;
    }
    const auto frequencyAt = [&](std::size_t frame) {
        return connectedFrequency.has_value()
            ? (*connectedFrequency)[frame]
            : fallbackFrequency;
    };
    // Checks the whole block first, so a rejected block changes nothing.
    auto framesLeft = framesLeftInBlock_;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        if (!std::isfinite(input[frame]))
            return false;
        if (framesLeft == 0)
        {
            if (!validFrequency(frequencyAt(frame)))
                return false;
            framesLeft = controlBlockFrames;
        }
        --framesLeft;
    }

    const auto& table = sineTable();
    const auto tableConvertor =
        static_cast<double>(tableSize) / sampleRate_;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        // ring_modulate reads the frequency one block ahead; a stream reads
        // it at the block start and ramps from the previous reading.
        if (framesLeftInBlock_ == 0)
        {
            const auto reading = std::log10(frequencyAt(frame));
            if (!started_)
            {
                nextPitch_ = reading;
                started_ = true;
            }
            pitch_ = nextPitch_;
            nextPitch_ = reading;
            pitchStep_ = (nextPitch_ - pitch_)
                / static_cast<double>(controlBlockFrames);
            framesLeftInBlock_ = controlBlockFrames;
        }
        --framesLeftInBlock_;
        const auto tableStep = std::pow(10.0, pitch_) * tableConvertor;
        // read_rmod_tab: truncate, then interpolate linearly. The increment
        // is its own statement, as in CDP, so it is not fused into the sum.
        const auto whole = static_cast<std::size_t>(tableIndex_);
        const auto fraction = tableIndex_ - static_cast<double>(whole);
        const auto increment = (table[whole + 1] - table[whole]) * fraction;
        const auto sine = table[whole] + increment;
        // CDP stores each product in its float sample buffer.
        output[frame] = static_cast<float>(input[frame] * sine);
        tableIndex_ += tableStep;
        while (tableIndex_ >= static_cast<double>(tableSize))
            tableIndex_ -= static_cast<double>(tableSize);
        pitch_ += pitchStep_;
    }
    return true;
}
}
