/*
 * Derived from the CDP System, Release 8: dev/filter/filters0.c and
 * fltpcon.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/VariableFilter.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's PI and filtcon.h's BSIZE.
constexpr double pi = 3.141592654;
constexpr int blockSize = 128;
}

bool VariableFilter::prepare(
    double sampleRate, Mode mode, double acuity, double gain, double frequency)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || (mode != Mode::Notch && mode != Mode::BandPass && mode != Mode::LowPass
            && mode != Mode::HighPass)
        || !std::isfinite(acuity) || acuity < minimumAcuity || acuity > 1.0
        || !std::isfinite(gain) || gain < minimumGain || gain > maximumGain
        || !std::isfinite(frequency) || frequency < minimumFrequency
        || frequency > sampleRate / 2.0)
    {
        return false;
    }
    mode_ = mode;
    startAcuity_ = acuity;
    startGain_ = gain;
    startFrequency_ = frequency;
    sampleRate_ = sampleRate;
    // filter_pconsistency's FLT_INV_SR.
    inverseSampleRate_ = 1.0 / sampleRate;
    prepared_ = true;
    reset();
    return true;
}

void VariableFilter::reset() noexcept
{
    dls_ = dbs_ = dhs_ = dns_ = 0.0;
    gain_ = startGain_;
    acuity_ = nextAcuity_ = startAcuity_;
    frequency_ = nextFrequency_ = startFrequency_;
    coefficient_ = 0.0;
    blockCount_ = 0;
}

bool VariableFilter::setControls(double acuity, double frequency) noexcept
{
    if (!prepared_ || !std::isfinite(acuity) || acuity < minimumAcuity || acuity > 1.0
        || !std::isfinite(frequency) || frequency < minimumFrequency
        || frequency > sampleRate_ / 2.0)
    {
        return false;
    }
    nextAcuity_ = acuity;
    nextFrequency_ = frequency;
    return true;
}

bool VariableFilter::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared_ || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    // do_varifilter calls multifilter(dll, dbb, dnn, dhh, ...) with FLT_DBS,
    // FLT_DLS, FLT_DHS and FLT_DNS, and passes FLT_Q, the acuity, as its
    // qfac.
    auto* dll = &dbs_;
    auto* dbb = &dls_;
    auto* dnn = &dhs_;
    auto* dhh = &dns_;
    // Its modes 1 to 4 are FSW_HIGH, FSW_LOW, FSW_BAND and FSW_NOTCH,
    // pointing at FLT_DHS, FLT_DLS, FLT_DBS and FLT_DNS; with the order of
    // the arguments, they give the notch, band-pass, low-pass and high-pass
    // outputs as named.
    const double* dpp = mode_ == Mode::Notch ? &dhs_
        : mode_ == Mode::BandPass           ? &dls_
        : mode_ == Mode::LowPass            ? &dbs_
                                            : &dns_;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        if (--blockCount_ <= 0)
        {
            // do_varifilter's block: the frequency and acuity its
            // breakpoints give now.
            acuity_ = nextAcuity_;
            frequency_ = nextFrequency_;
            coefficient_ = (2.0 * pi * frequency_) * inverseSampleRate_;
            blockCount_ = blockSize;
        }
        const auto sample = static_cast<float>(input[frame]);
        const auto qfac = acuity_;
        const auto coeff = coefficient_;
        // multifilter.
        double dya, dsa;
        dya = static_cast<double>(sample);
        *dhh = (qfac * *dbb) - *dll - dya;
        *dbb = *dbb - (coeff * *dhh);
        *dll = *dll - (coeff * *dbb);
        *dnn = *dll + *dhh;
        dsa = (*dpp) * gain_;
        // check_float_limits, clipping float samples as CDP does by
        // default.
        if (dsa > 1.0)
        {
            gain_ *= 0.9999;
            dsa = 1.0;
        }
        if (dsa < -1.0)
        {
            gain_ *= 0.9999;
            dsa = -1.0;
        }
        output[frame] = static_cast<double>(static_cast<float>(dsa));
    }
    return true;
}
}
