/*
 * Derived from the CDP System, Release 8: dev/standalone/mchstereo.c and
 * dev/cdp2k/readdata.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/StereoPlacement.h"

#include <cmath>

// CDP8's build fuses mchstereo's sums of a product; the std::fma calls below
// stand for them, and contraction is off elsewhere.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR.
constexpr double FLTERR = 0.000002;

[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const auto upperbnd = f2 + FLTERR;
    const auto lowerbnd = f2 - FLTERR;
    return !(f1 > upperbnd || f1 < lowerbnd);
}

// obuf[x] = (float)(obuf[x] + sample * gain).
[[nodiscard]] float accumulate(float into, double sample, double gain) noexcept
{
    return static_cast<float>(std::fma(sample, gain, static_cast<double>(into)));
}
}

bool StereoPlacement::prepare(
    int inputs, int outputChannels, std::span<const int> centres, double pregain, bool shifted) noexcept
{
    if (inputs < 1 || inputs > maximumInputs || outputChannels < minimumChannels
        || outputChannels > maximumChannels || centres.size() != static_cast<std::size_t>(inputs))
        return false;
    if (!std::isfinite(pregain))
        return false;
    // readdata.c's range check, then check_the_param_validity_and_consistency.
    if (flteq(pregain, 0.0))
        pregain = 0.0;
    if (flteq(pregain, 1.0))
        pregain = 1.0;
    if (pregain > 1.0 || pregain <= 0.0)
        return false;
    StereoPlacement fresh;
    for (std::size_t n = 0; n < centres.size(); n++)
    {
        if (centres[n] < 1 || centres[n] > outputChannels)
            return false;
        fresh.centres_[n] = static_cast<std::size_t>(centres[n] - 1);
    }
    fresh.inputs_ = static_cast<std::size_t>(inputs);
    fresh.channels_ = static_cast<std::size_t>(outputChannels);
    fresh.pregain_ = pregain;
    fresh.halfPregain_ = pregain / 2.0;
    fresh.shifted_ = shifted;
    *this = fresh;
    return true;
}

bool StereoPlacement::process(std::span<const std::span<const double>> inputs, std::span<double> output) noexcept
{
    if (!prepared() || inputs.size() != inputs_ || output.size() % channels_ != 0)
        return false;
    const std::size_t frames = output.size() / channels_;
    for (const auto input : inputs)
    {
        if (input.size() != 2 * frames)
            return false;
        for (const double sample : input)
        {
            if (!std::isfinite(sample))
                return false;
        }
    }
    std::array<float, maximumChannels> obuf{};
    for (std::size_t frame = 0; frame < frames; frame++)
    {
        obuf.fill(0.0F);
        for (std::size_t n = 0; n < inputs_; n++)
        {
            const float left = static_cast<float>(inputs[n][2 * frame]);
            const float right = static_cast<float>(inputs[n][2 * frame + 1]);
            const std::size_t centre = centres_[n];
            if (shifted_)
            {
                // Stereo centred between output loudspeakers.
                const std::size_t llout = centre;
                const std::size_t rrout = llout + 1 >= channels_ ? 0 : llout + 1;
                obuf[llout] = accumulate(obuf[llout], static_cast<double>(left), pregain_);
                obuf[rrout] = accumulate(obuf[rrout], static_cast<double>(right), pregain_);
            }
            else
            {
                // Stereo centred at a loudspeaker: the float sum to the centre.
                const float both = left + right;
                obuf[centre] = accumulate(obuf[centre], static_cast<double>(both), halfPregain_);
                const std::size_t llout = centre == 0 ? channels_ - 1 : centre - 1;
                obuf[llout] = accumulate(obuf[llout], static_cast<double>(left), halfPregain_);
                const std::size_t rrout = centre + 1 >= channels_ ? 0 : centre + 1;
                obuf[rrout] = accumulate(obuf[rrout], static_cast<double>(right), halfPregain_);
            }
        }
        for (std::size_t c = 0; c < channels_; c++)
            output[frame * channels_ + c] = static_cast<double>(obuf[c]);
    }
    return true;
}
}
