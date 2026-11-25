/*
 * Derived from the CDP System, Release 8: dev/filter/filters0.c and
 * fltprepro.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/LohiFilter.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
bool LohiFilter::prepare(
    double sampleRate, double attenuationDb, double passBand, double stopBand,
    double prescale)
{
    const auto nyquist = sampleRate / 2.0;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(attenuationDb) || attenuationDb < minimumAttenuation
        || attenuationDb > 0.0
        || !std::isfinite(passBand) || passBand < minimumFrequency || passBand > nyquist
        || !std::isfinite(stopBand) || stopBand < minimumFrequency || stopBand > nyquist
        || passBand == stopBand
        || !std::isfinite(prescale) || prescale < minimumPrescale || prescale > maximumPrescale)
    {
        return false;
    }
    // setup_lphp_filter and establish_order_of_filter.
    auto signd = 1.0;
    if (passBand < stopBand)
        signd = -1.0;
    double mul;
    auto passfrq = passBand;
    auto stopfrq = stopBand;
    if (passfrq < stopfrq)
        mul = 2.0;
    else
    {
        mul = -2.0;
        passfrq = nyquist - passfrq;
        stopfrq = nyquist - stopfrq;
    }
    const auto pii = 4.0 * std::atan(1.0);
    passfrq = pii * passfrq / sampleRate;
    const auto tp = std::tan(passfrq);
    stopfrq = pii * stopfrq / sampleRate;
    const auto tc = std::tan(stopfrq);
    auto tt = tc / tp;
    tt = (tt * tt);
    auto gain = std::fabs(attenuationDb);
    gain = gain * std::log(10.0) / 10.0;
    gain = std::exp(gain) - 1.0;
    const auto xx = std::log(gain) / std::log(tt);
    auto yy = std::floor(xx);
    if ((xx - yy) == 0.0)
        yy = yy - 1.0;
    // C leaves `((int)yy) + 1` undefined beyond int's range, which an
    // attenuation of 0 dB (an order of minus infinity) or nearly equal bands
    // reach; the machines CDP8 runs on then give an order under 2.
    auto filterOrder = std::isfinite(yy) && yy > -2147483648.0 && yy < 2147483647.0
        ? static_cast<int>(yy) + 1
        : 1;
    if (filterOrder <= 1)
        filterOrder = 2;
    auto count = filterOrder / 2;
    count = std::min(count, maximumSections);
    filterOrder = 2 * count;

    // calculate_filter_poles_lphp, for the last section, the only one the
    // loop's output keeps.
    const auto ss = pii / static_cast<double>(2 * filterOrder);
    const auto k = count - 1;
    const auto sectionX = static_cast<double>((2.0 * (k + 1)) - 1.0);
    const auto aa = -std::sin(sectionX * ss);
    const auto tppwr = std::pow(tp, 2.0);
    const auto cc = 1.0 - (2.0 * aa * tp) + tppwr;
    const auto x1 = 2.0 * (tppwr - 1.0) / cc;
    const auto x2 = (1.0 + (2.0 * aa * tp) + tppwr) / cc;
    multiplier_ = mul;
    den1_ = signd * x1;
    den2_ = -x2;
    cn_ = std::pow(tp, 2.0) / cc;
    prescale_ = prescale;
    prepared_ = true;
    reset();
    return true;
}

void LohiFilter::reset() noexcept
{
    s1_ = 0.0;
    s2_ = 0.0;
    e1_ = 0.0;
    e2_ = 0.0;
}

bool LohiFilter::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared_ || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    // lphp_filt_chan for its last section, statement for statement, then
    // check_float_limits, clipping float samples as CDP does by default.
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto ip = static_cast<double>(static_cast<float>(input[frame]));
        const auto b1 = multiplier_ * cn_;
        auto op = (cn_ * ip) + (den1_ * s1_) + (den2_ * s2_) + (b1 * e1_) + (cn_ * e2_);
        s2_ = s1_;
        s1_ = op;
        e2_ = e1_;
        e1_ = ip;
        op *= prescale_;
        if (op > 1.0)
            op = 1.0;
        if (op < -1.0)
            op = -1.0;
        output[frame] = static_cast<double>(static_cast<float>(op));
    }
    return true;
}
}
