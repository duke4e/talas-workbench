/*
 * Derived from the CDP System, Release 8: dev/distort/distort.c and
 * disprepro.c, and dev/sfsys/osbind.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/DistortOverload.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's PI and TWOPI, distort1.h's DISTORTER_TABLEN and sfsys's
// F_MAXSAMP.
constexpr double pi = 3.141592654;
constexpr double twoPi = 2.0 * pi;
constexpr int tableLength = 1028;
constexpr double maximumSample = 1.0;

// make_distorter_tab: a cosine shifted to lie between 0 and -1.
const std::array<double, tableLength>& distorterTable()
{
    static const auto table = [] {
        std::array<double, tableLength> tab{};
        const auto convertor = static_cast<double>(twoPi) / static_cast<double>(tableLength);
        for (int n = 0; n < tableLength; n++)
            tab[static_cast<std::size_t>(n)] = (std::cos(static_cast<double>(n) * convertor) - 1.0) / 2.0;
        return tab;
    }();
    return table;
}
}

bool DistortOverload::prepare(
    double sampleRate, Mode mode, double clipLevel, double depth, double frequency, int seed)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || (mode != Mode::Noise && mode != Mode::Waveform)
        || !std::isfinite(clipLevel) || clipLevel < 0.0 || clipLevel > 1.0
        || !std::isfinite(depth) || depth < 0.0 || depth > 1.0
        || (mode == Mode::Waveform
            && (!std::isfinite(frequency) || frequency < minimumFrequency
                || frequency > sampleRate / 2.0))
        || seed < 1 || seed > 256)
    {
        return false;
    }
    (void)distorterTable();
    // distort_overload's setup.
    mode_ = mode;
    maxgate_ = clipLevel;
    startDepth_ = depth;
    incRatio_ = static_cast<double>(tableLength) / sampleRate;
    startFrequency_ = frequency;
    sampleRate_ = sampleRate;
    stepper_ = static_cast<int>(std::lround(sampleRate * 0.01));
    if (stepper_ < 1)
        stepper_ = 1;
    normaliser_ = (maximumSample / (clipLevel + 1.0));
    startRandom_.seed(static_cast<unsigned long>(seed));
    prepared_ = true;
    reset();
    return true;
}

bool DistortOverload::setControls(double depth, double frequency) noexcept
{
    if (!prepared_ || !std::isfinite(depth) || depth < 0.0 || depth > 1.0
        || (mode_ == Mode::Waveform
            && (!std::isfinite(frequency) || frequency < minimumFrequency || frequency > sampleRate_ / 2.0)))
    {
        return false;
    }
    nextDepth_ = depth;
    nextFrequency_ = mode_ == Mode::Waveform ? frequency : startFrequency_;
    return true;
}

void DistortOverload::reset() noexcept
{
    depth_ = nextDepth_ = startDepth_;
    frequency_ = nextFrequency_ = startFrequency_;
    toRead_ = 0;
    random_ = startRandom_;
    tabpos_ = 0;
    tabposd_ = 0.0;
    inMaxgate_ = false;
    inMingate_ = false;
}

bool DistortOverload::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared_ || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    // distort_overload's loops, statement for statement, keeping CDP's float
    // samples.
    const auto maxgate = maxgate_;
    const auto mingate = -maxgate;
    const auto& tab = distorterTable();
    for (std::size_t n = 0; n < input.size(); ++n)
    {
        // read_values_from_all_existing_brktables every stepper frames.
        if (toRead_ == 0)
        {
            depth_ = nextDepth_;
            frequency_ = nextFrequency_;
            toRead_ = stepper_;
        }
        --toRead_;
        auto buf = static_cast<float>(input[n]);
        double k;
        if (mode_ == Mode::Noise)
        {
            if (static_cast<double>(buf) >= maxgate)
            {
                k = 1.0 - (random_.next() * depth_);
                buf = static_cast<float>(k * maxgate);
            }
            else if (static_cast<double>(buf) <= mingate)
            {
                k = 1.0 - (random_.next() * depth_);
                buf = static_cast<float>(k * mingate);
            }
            buf = static_cast<float>(buf * normaliser_);
        }
        else
        {
            if (static_cast<double>(buf) >= maxgate)
            {
                if (!inMaxgate_)
                {
                    tabpos_ = 0;
                    tabposd_ = 0.0;
                    inMingate_ = false;
                    inMaxgate_ = true;
                }
                k = 1.0 + (tab[static_cast<std::size_t>(tabpos_)] * depth_);
                buf = static_cast<float>(k * maxgate);
            }
            else if (static_cast<double>(buf) <= mingate)
            {
                if (!inMingate_)
                {
                    tabpos_ = 0;
                    tabposd_ = 0.0;
                    inMaxgate_ = false;
                    inMingate_ = true;
                }
                k = 1.0 + (tab[static_cast<std::size_t>(tabpos_)] * depth_);
                buf = static_cast<float>(k * mingate);
            }
            else
            {
                inMaxgate_ = false;
                inMingate_ = false;
            }
            buf = static_cast<float>(buf * normaliser_);
            tabposd_ = std::fmod(tabposd_ + (frequency_ * incRatio_), static_cast<double>(tableLength));
            tabpos_ = static_cast<int>(std::lround(tabposd_) % tableLength);
        }
        output[n] = static_cast<double>(buf);
    }
    return true;
}
}
