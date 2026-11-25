/*
 * Derived from the CDP System, Release 8: dev/spec/simple.c and
 * dev/cdp2k/tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralSimple.h"

#include <cmath>

namespace talas::cdp
{
namespace
{
[[nodiscard]] bool validGain(double gain) noexcept
{
    return std::isfinite(gain) && gain >= 0.0 && gain <= SpectralGain::maximumGain;
}

[[nodiscard]] bool validThreshold(double threshold) noexcept
{
    return std::isfinite(threshold) && threshold >= 0.0 && threshold <= 1.0;
}
}

bool SpectralGain::prepare(double sampleRate, int points, int overlap, double gain)
{
    if (!validGain(gain))
        return false;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    startGain_ = gain;
    reset();
    return true;
}

bool SpectralGain::setGain(double gain) noexcept
{
    if (!prepared() || !validGain(gain))
        return false;
    gain_ = gain;
    return true;
}

void SpectralGain::reset() noexcept
{
    vocoder_.reset();
    gain_ = startGain_;
}

bool SpectralGain::process(std::span<const double> input, std::span<double> output) noexcept
{
    const auto gain = gain_;
    // specgain, on each window outer_loop reads.
    return vocoder_.process(input, output, [gain](std::span<float> frame) {
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            frame[vc] = (float)(frame[vc] * gain);
    });
}

bool SpectralGate::prepare(double sampleRate, int points, int overlap, double threshold)
{
    if (!validThreshold(threshold))
        return false;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    startThreshold_ = threshold;
    reset();
    return true;
}

bool SpectralGate::setThreshold(double threshold) noexcept
{
    if (!prepared() || !validThreshold(threshold))
        return false;
    threshold_ = threshold;
    return true;
}

void SpectralGate::reset() noexcept
{
    vocoder_.reset();
    threshold_ = startThreshold_;
}

bool SpectralGate::process(std::span<const double> input, std::span<double> output) noexcept
{
    const auto threshold = threshold_;
    // speclimit, CDP's `spec gate`.
    return vocoder_.process(input, output, [threshold](std::span<float> frame) {
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        {
            if (frame[vc] < threshold)
                frame[vc] = 0.0f;
        }
    });
}
}
