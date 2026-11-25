/*
 * Derived from the CDP System, Release 8: dev/filter/filters0.c and
 * fltpcon.c (after Andy Moorer's presence and shelving filter designs)
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/FixedFilter.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// fltpcon.c's constants, ROOT2OF2's misprinted digit included.
constexpr double pii = 3.141592653589793238462643;
constexpr double root2Of2 = 0.7071067811965475244;
constexpr double spn = 1.65436e-24;

void establish(double cf, double boost, double& a, double& asq, double& A, double& F,
    double& F2, double& tmp) noexcept
{
    const double sqrt2 = std::sqrt(2.0);
    const double cf_less_quarter = cf - 0.25;
    const double b_over20 = boost / 20.0;
    a = std::tan(pii * cf_less_quarter);
    asq = a * a;
    A = std::pow(10.0, b_over20);
    if (boost < 6.0 && boost > -6.0)
        F = std::sqrt(A);
    else if (A > 1.0)
        F = A / sqrt2;
    else
        F = A * sqrt2;
    F2 = F * F;
    tmp = (A * A) - F2;
}

void setTheTvars(double gamma, double& t0, double& t1, double& t2) noexcept
{
    double gamma2, gam2p1, siggam2;
    gamma2 = gamma * gamma;
    gam2p1 = 1.0 + gamma2;
    siggam2 = 2.0 * root2Of2 * gamma;
    t0 = gam2p1 + siggam2;
    t1 = -2.0 * (1.0 - gamma2);
    t2 = gam2p1 - siggam2;
}

void bilinearTransform(double a, double t0, double t1, double t2, double asq,
    double& x0, double& x1, double& x2) noexcept
{
    double aa1;
    aa1 = a * t1;
    x0 = t0 + aa1 + (asq * t2);
    x1 = (2 * a * (t0 + t2)) + ((1 + asq) * t1);
    x2 = (asq * t0) + aa1 + t2;
}

void normaliseB0ToOne(double b0, double& a0, double& a1, double& a2, double& b1, double& b2) noexcept
{
    const double recipb0 = 1.0 / b0;
    a0 *= recipb0;
    a1 *= recipb0;
    a2 *= recipb0;
    b1 *= recipb0;
    b2 *= recipb0;
}

double bw2angle(double a, double bw) noexcept
{
    double T, d, sn, cs, mag, delta, theta, tmp, a2, a4, asnd;
    T = std::tan(2 * pii * bw);
    a2 = a * a;
    a4 = a2 * a2;
    d = 2 * a2 * T;
    sn = (1 + a4) * T;
    cs = (1 - a4);
    mag = std::sqrt((sn * sn) + (cs * cs));
    d /= mag;
    delta = std::atan2(sn, cs);
    asnd = std::asin(d);
    theta = 0.5 * (pii - asnd - delta);
    tmp = 0.5 * (asnd - delta);
    if (tmp > 0 && tmp < theta)
        theta = tmp;
    return (theta / (2 * pii));
}
}

bool FixedFilter::prepare(
    double sampleRate, Mode mode, double boostDb, double frequency, double bandwidth,
    double prescale)
{
    const auto nyquist = sampleRate / 2.0;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || (mode != Mode::BelowFrequency && mode != Mode::AboveFrequency && mode != Mode::Band)
        || !std::isfinite(boostDb) || boostDb < minimumBoost || boostDb > maximumBoost
        || !std::isfinite(frequency) || frequency < minimumFrequency || frequency > nyquist
        || (mode == Mode::Band
            && (!std::isfinite(bandwidth) || bandwidth < minimumBandwidth || bandwidth > nyquist))
        || !std::isfinite(prescale) || prescale < minimumPrescale || prescale > maximumPrescale)
    {
        return false;
    }
    // setup_internal_eq_params: normalised frequency and bandwidth.
    const double inverseSampleRate = 1.0 / sampleRate;
    const double cf = frequency * inverseSampleRate;
    const double bw = bandwidth * inverseSampleRate;
    double a0 = 0.0, a1 = 0.0, a2 = 0.0, b1 = 0.0, b2 = 0.0;
    if (mode == Mode::Band)
    {
        // presence.
        double a, A, F, xfmbw, C, tmp, alphan, alphad, b0, recipb0, asq, F2, a2plus1, ma2plus1;
        establish(cf, boostDb, a, asq, A, F, F2, tmp);
        xfmbw = bw2angle(a, bw);
        C = 1.0 / std::tan(2 * pii * xfmbw);
        if (std::fabs(tmp) <= spn)
        {
            alphad = C;
        }
        else
        {
            alphad = (C * C * (F2 - 1)) / tmp;
            alphad = std::sqrt(alphad);
        }
        alphan = A * alphad;
        a2plus1 = 1 + asq;
        ma2plus1 = 1 - asq;
        a0 = a2plus1 + alphan * ma2plus1;
        a1 = 4 * a;
        a2 = a2plus1 - alphan * ma2plus1;
        b0 = a2plus1 + alphad * ma2plus1;
        b2 = a2plus1 - alphad * ma2plus1;
        recipb0 = 1.0 / b0;
        a0 *= recipb0;
        a1 *= recipb0;
        a2 *= recipb0;
        b1 = a1;
        b2 *= recipb0;
    }
    else
    {
        // shelve.
        double a, A, F, tmp, b0, asq, F2, gamman, gammad, ta0, ta1, ta2, tb0, tb1, tb2;
        establish(cf, boostDb, a, asq, A, F, F2, tmp);
        if (std::fabs(tmp) <= spn)
        {
            gammad = 1.0;
        }
        else
        {
            gammad = (F2 - 1.0) / tmp;
            gammad = std::pow(gammad, 0.25);
        }
        gamman = std::sqrt(A) * gammad;
        setTheTvars(gamman, ta0, ta1, ta2);
        if (mode == Mode::AboveFrequency)
            ta1 = -ta1;
        setTheTvars(gammad, tb0, tb1, tb2);
        if (mode == Mode::AboveFrequency)
            tb1 = -tb1;
        bilinearTransform(a, ta0, ta1, ta2, asq, a0, a1, a2);
        bilinearTransform(a, tb0, tb1, tb2, asq, b0, b1, b2);
        normaliseB0ToOne(b0, a0, a1, a2, b1, b2);
    }
    if (!std::isfinite(a0) || !std::isfinite(a1) || !std::isfinite(a2)
        || !std::isfinite(b1) || !std::isfinite(b2))
    {
        return false;
    }
    a0_ = a0;
    a1_ = a1;
    a2_ = a2;
    b1_ = b1;
    b2_ = b2;
    prescale_ = prescale;
    prepared_ = true;
    reset();
    return true;
}

void FixedFilter::reset() noexcept
{
    xx1_ = xx2_ = yy1_ = yy2_ = 0.0;
}

bool FixedFilter::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared_ || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    // do_eq_filter for one channel, keeping CDP's float samples.
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        double ip, op;
        ip = static_cast<double>(static_cast<float>(input[frame]));
        ip *= prescale_;
        op = (a0_ * ip) + (a1_ * xx1_) + (a2_ * xx2_) - (b1_ * yy1_) - (b2_ * yy2_);
        xx2_ = xx1_;
        xx1_ = ip;
        yy2_ = yy1_;
        yy1_ = op;
        output[frame] = static_cast<double>(static_cast<float>(op));
    }
    return true;
}
}
