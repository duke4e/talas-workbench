/*
 * Derived from the CDP System, Release 8: dev/new/tremolo.c and
 * dev/cdp2k/tklib3.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/TremoloFine.h"

#include <algorithm>
#include <array>
#include <cmath>

#if defined(__APPLE__)
// Apple's libm, where CDP8's build turns pow(10.0, x) into __exp10(x).
extern "C" double __exp10(double);
#endif

namespace talas::cdp
{
namespace
{
// globcon.h's PI, to ten digits.
constexpr double pi = 3.141592654;

using SineTable = std::array<double, TremoloFine::tableSize + 1>;

// create_tremolo_sintable: a sine raised to run from 0 to 1, closed by its
// midpoint for interpolation across the wrap.
const SineTable& tremoloSineTable() noexcept
{
    static const auto table = [] {
        SineTable values{};
        std::size_t n = 0;
        for (; n < TremoloFine::tableSize; n++)
        {
            values[n] = std::sin(pi * 2.0
                * (static_cast<double>(n) / static_cast<double>(TremoloFine::tableSize)));
            values[n] += 1.0;
            values[n] /= 2.0;
        }
        values[n] = 0.5;
        return values;
    }();
    return table;
}

// pow(10.0, x) as CDP8's build calls it.
[[nodiscard]] double cdpExp10(double x) noexcept
{
#if defined(__APPLE__)
    return __exp10(x);
#else
    return std::pow(10.0, x);
#endif
}

[[nodiscard]] bool inRange(double value, double minimum, double maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}
}

bool TremoloFine::valid(double frequency, double depth, double gain) const noexcept
{
    // A live Logarithmic frequency is refused at zero, whose log10 CDP never
    // takes (convert_tremolo_frq_to_log10 raises it to one over the file's
    // duration first).
    const auto logarithmic = mode_ == Mode::Logarithmic && liveFrequency_;
    return inRange(frequency, 0.0, maximumFrequency) && !(logarithmic && frequency <= 0.0)
        && inRange(depth, 0.0, 1.0) && inRange(gain, 0.0, 1.0);
}

double TremoloFine::readFrequency(double frequency) const noexcept
{
    // convert_tremolo_frq_to_log10 stores a frequency breakpoint file's
    // values as log10s, and tremolo raises each value read back with
    // pow(10, ...); a fixed frequency is used as it is.
    if (mode_ == Mode::Logarithmic && liveFrequency_)
        return cdpExp10(std::log10(frequency));
    return frequency;
}

bool TremoloFine::prepare(
    double sampleRate, Mode mode, double frequency, double depth, double gain, int fineness,
    bool liveFrequency)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || fineness < 1 || fineness > maximumFineness
        || mode < Mode::Linear || mode > Mode::Logarithmic)
    {
        return false;
    }
    TremoloFine candidate;
    candidate.mode_ = mode;
    candidate.liveFrequency_ = liveFrequency;
    if (!candidate.valid(frequency, depth, gain))
        return false;
    // Builds the shared table here, before any rendering thread reads it.
    static_cast<void>(tremoloSineTable());
    sampleRate_ = sampleRate;
    // tremolo's tabsize_over_srate.
    tabsizeOverSrate_ = static_cast<double>(tableSize) / sampleRate;
    mode_ = mode;
    liveFrequency_ = liveFrequency;
    fineness_ = fineness;
    startFrequency_ = frequency;
    startDepth_ = depth;
    startGain_ = gain;
    reset();
    return true;
}

bool TremoloFine::setControls(double frequency, double depth, double gain) noexcept
{
    if (!prepared() || !valid(frequency, depth, gain))
        return false;
    frequency_ = readFrequency(frequency);
    depth_ = depth;
    gain_ = gain;
    return true;
}

void TremoloFine::reset() noexcept
{
    frequency_ = readFrequency(startFrequency_);
    depth_ = startDepth_;
    gain_ = startGain_;
    fsinpos_ = 0.0;
}

bool TremoloFine::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    const auto& sintab = tremoloSineTable();
    for (std::size_t n = 0; n < input.size(); n++)
    {
        // tremolo, statement for statement.
        auto sample = static_cast<float>(input[n]);
        const int sinpos = static_cast<int>(fsinpos_);
        const double frac = fsinpos_ - static_cast<double>(sinpos);
        const double losin = sintab[static_cast<std::size_t>(sinpos)];
        const double hisin = sintab[static_cast<std::size_t>(sinpos) + 1];
        // CDP8's build fuses the interpolation's multiply and add, and the
        // table pointer's advance, but not the depth's two statements.
        double val = std::fma(hisin - losin, frac, losin);
        val *= depth_;
        val += (1.0 - depth_);
        val *= gain_;
        fsinpos_ = std::fma(frequency_, tabsizeOverSrate_, fsinpos_);
        fsinpos_ = std::fmod(fsinpos_, static_cast<double>(tableSize));
        // The fineness: the sample multiplied by the value that many times,
        // stored in CDP's float buffer each time.
        for (int j = 0; j < fineness_; j++)
            sample = static_cast<float>(static_cast<double>(sample) * val);
        output[n] = static_cast<double>(sample);
    }
    return true;
}
}
