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
#include "talas/cdp/SweepingFilter.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's PI, TWOPI and FLTERR, and filtcon.h's BSIZE and FLT_MINSWEEP.
constexpr double pi = 3.141592654;
constexpr double twoPi = 2.0 * pi;
constexpr double floatError = 0.000002;
constexpr int blockSize = 128;
constexpr double minimumSweep = 0.0001;

// tklib1.c's flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const auto upperbnd = f2 + floatError;
    const auto lowerbnd = f2 - floatError;
    return !((f1 > upperbnd) || (f1 < lowerbnd));
}
}

bool SweepingFilter::prepare(
    double sampleRate, Mode mode, double acuity, double gain, double lowFrequency,
    double highFrequency, double sweepFrequency, double phase)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || (mode != Mode::Notch && mode != Mode::BandPass && mode != Mode::LowPass
            && mode != Mode::HighPass)
        || !std::isfinite(acuity) || acuity < minimumAcuity || acuity > 1.0
        || !std::isfinite(gain) || gain < minimumGain || gain > maximumGain
        || !std::isfinite(lowFrequency) || lowFrequency < minimumFrequency
        || lowFrequency > sampleRate / 2.0
        || !std::isfinite(highFrequency) || highFrequency < minimumFrequency
        || highFrequency > sampleRate / 2.0
        || !std::isfinite(sweepFrequency) || sweepFrequency < 0.0
        || sweepFrequency > maximumSweepFrequency
        || !std::isfinite(phase) || phase < 0.0 || phase > 1.0)
    {
        return false;
    }
    // filter_pconsistency's FLT_INV_SR, then setup_internal_fltsweep_params:
    // convert_phase_from_input_range_to_actual_range and convert_sfrq.
    const auto inverseSampleRate = 1.0 / sampleRate;
    auto swpphase = phase;
    swpphase += .5;
    if (swpphase >= 1.0)
        swpphase -= 1.0;
    swpphase *= twoPi;
    mode_ = mode;
    startAcuity_ = acuity;
    startGain_ = gain;
    startLowFrequency_ = lowFrequency;
    startHighFrequency_ = highFrequency;
    inverseSampleRate_ = inverseSampleRate;
    sampleRate_ = sampleRate;
    startSweepStep_ = sweepStepFor(sweepFrequency);
    startPosition_ = swpphase;
    prepared_ = true;
    reset();
    return true;
}

double SweepingFilter::sweepStepFor(double sweepFrequency) const noexcept
{
    // convert_sfrq, which converts a breakpoint file's values alike.
    auto swpfrq = sweepFrequency;
    if (flteq(swpfrq, 0.0))
        swpfrq = minimumSweep;
    return swpfrq * inverseSampleRate_ * blockSize * twoPi;
}

bool SweepingFilter::setControls(
    double acuity, double lowFrequency, double highFrequency, double sweepFrequency) noexcept
{
    if (!prepared_ || !std::isfinite(acuity) || acuity < minimumAcuity || acuity > 1.0
        || !std::isfinite(lowFrequency) || lowFrequency < minimumFrequency
        || lowFrequency > sampleRate_ / 2.0
        || !std::isfinite(highFrequency) || highFrequency < minimumFrequency
        || highFrequency > sampleRate_ / 2.0
        || !std::isfinite(sweepFrequency) || sweepFrequency < 0.0
        || sweepFrequency > maximumSweepFrequency)
    {
        return false;
    }
    nextAcuity_ = acuity;
    nextLowFrequency_ = lowFrequency;
    nextHighFrequency_ = highFrequency;
    nextSweepStep_ = sweepStepFor(sweepFrequency);
    return true;
}

void SweepingFilter::reset() noexcept
{
    dls_ = dbs_ = dhs_ = dns_ = 0.0;
    gain_ = startGain_;
    acuity_ = nextAcuity_ = startAcuity_;
    lowFrequency_ = nextLowFrequency_ = startLowFrequency_;
    highFrequency_ = nextHighFrequency_ = startHighFrequency_;
    sweepStep_ = nextSweepStep_ = startSweepStep_;
    position_ = startPosition_;
    coefficient_ = 0.0;
    blockCount_ = 0;
}

bool SweepingFilter::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared_ || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    // do_sweep_filter calls multifilter as do_varifilter does, so its modes
    // give the notch, band-pass, low-pass and high-pass outputs as named.
    auto* dll = &dbs_;
    auto* dbb = &dls_;
    auto* dnn = &dhs_;
    auto* dhh = &dns_;
    const double* dpp = mode_ == Mode::Notch ? &dhs_
        : mode_ == Mode::BandPass           ? &dls_
        : mode_ == Mode::LowPass            ? &dbs_
                                            : &dns_;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        if (--blockCount_ <= 0)
        {
            // do_sweep_filter's block: the acuity and frequencies its
            // breakpoints give now.
            acuity_ = nextAcuity_;
            lowFrequency_ = nextLowFrequency_;
            highFrequency_ = nextHighFrequency_;
            sweepStep_ = nextSweepStep_;
            // getfrq.
            double thispos;
            const auto range = highFrequency_ - lowFrequency_;
            thispos = (std::cos(position_) + 1.0) * 0.5;
            position_ = std::fmod(position_ + sweepStep_, twoPi);
            const auto frq = (thispos * range) + lowFrequency_;
            coefficient_ = (2.0 * pi * frq) * inverseSampleRate_;
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
