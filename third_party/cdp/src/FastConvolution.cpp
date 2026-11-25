/*
 * Derived from the CDP System, Release 8: dev/externals/fastconv/fconv.cpp,
 * genrespframe2.cpp and mxfftd.c
 * Copyright (c) 1983-2013 Richard Dobson and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/FastConvolution.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <math.h>

#if defined(__APPLE__)
// Apple's libm, where CDP8's build turns pow(10.0, x) into __exp10(x).
extern "C" double __exp10(double);
#endif

// CDP8's build (Apple clang, -O2, contraction on) fuses the first product
// of each sum or difference of products, and a product added to a plain
// value, within one expression; the std::fma calls below stand where its
// fastconv fuses, and nowhere else, so the port does not depend on this
// compiler's contraction. It also computes each cosine and sine of one
// angle with __sincos_stret, which can differ from cos and sin by an ulp.
namespace talas::cdp
{
namespace
{
// fconv.cpp's PI2, to ten digits.
constexpr double pi2 = 1.570796327;

struct SineCosine
{
    double sine;
    double cosine;
};

[[nodiscard]] SineCosine sineCosine(double angle) noexcept
{
#if defined(__APPLE__)
    const auto both = __sincos_stret(angle);
    return {both.__sinval, both.__cosval};
#else
    return {std::sin(angle), std::cos(angle)};
#endif
}

// pow(10, x) as CDP8's build calls it.
[[nodiscard]] double cdpExp10(double x) noexcept
{
#if defined(__APPLE__)
    return __exp10(x);
#else
    return std::pow(10.0, x);
#endif
}

// fftmx (mxfftd.c) as it runs for the powers of two fastconv gives it:
// factors of 4, with at most three of 2 among them, so the code for odd
// factors, the rotation after them, the multivariate permutation and the
// permutation for non-square factors never runs, and their workspace is not
// needed. FORTRAN's indices from 1: a(j) is a0[j - 1].
void fftmx(double* a0, double* b0, int ntot, int n, int nspan, int isn, int m, int kt, const int nfac[],
    int* np0) noexcept
{
    const auto a = [a0](int j) -> double& { return a0[j - 1]; };
    const auto b = [b0](int j) -> double& { return b0[j - 1]; };
    const auto np = [np0](int j) -> int& { return np0[j - 1]; };
    int i, inc, j, jc, k, k1, k2, k3 = 0, kk, klim, ks, kspan, lim, mm, nn, nt;
    double ak, ajm, ajp, akm, akp, bk, bjm, bjp, bkm, bkp, c1, c2 = 0.0, c3 = 0.0, cd, dr, rad, sd, s1,
        s2 = 0.0, s3 = 0.0;
    double xx;
    inc = std::abs(isn);
    nt = inc * ntot;
    ks = inc * nspan;
    rad = 0.785398163397448278900;

    // scale by 1/n for isn > 0 ( reverse transform )
    if (isn < 0)
    {
        rad = -rad;
    }
    else
    {
        ak = 1.0 / (double)n;
        for (j = 1; j <= nt; j += inc)
        {
            a(j) *= ak;
            b(j) *= ak;
        }
    }
    kspan = ks;
    nn = nt - inc;
    jc = ks / n;

    // sin, cos values are re-initialised each lim steps
    lim = 32;
    klim = lim * jc;
    i = 0;

    // compute fourier transform
lbl40:
    dr = (8.0 * (double)jc) / ((double)kspan);
    xx = std::sin((double)0.5 * dr * rad);
    cd = 2.0 * xx * xx;
    sd = std::sin(dr * rad);
    kk = 1;
    if (nfac[++i] != 2)
        goto lbl110;
    // transform for factor of 2 (including rotation factor)
    kspan /= 2;
    k1 = kspan + 2;
    do
    {
        do
        {
            k2 = kk + kspan;
            ak = a(k2);
            bk = b(k2);
            a(k2) = (a(kk)) - ak;
            b(k2) = (b(kk)) - bk;
            a(kk) = (a(kk)) + ak;
            b(kk) = (b(kk)) + bk;
            kk = k2 + kspan;
        } while (kk <= nn);
        kk -= nn;
    } while (kk <= jc);
    if (kk > kspan)
        goto lbl350;
lbl60:
    c1 = 1.0 - cd;
    s1 = sd;
    mm = std::min(k1 / 2, klim);
    goto lbl80;
lbl70:
    ak = c1 - std::fma(cd, c1, sd * s1);
    s1 = std::fma(sd, c1, -(cd * s1)) + s1;
    c1 = ak;
lbl80:
    do
    {
        do
        {
            k2 = kk + kspan;
            ak = a(kk) - a(k2);
            bk = b(kk) - b(k2);
            a(kk) = a(kk) + a(k2);
            b(kk) = b(kk) + b(k2);
            a(k2) = std::fma(c1, ak, -(s1 * bk));
            b(k2) = std::fma(s1, ak, c1 * bk);
            kk = k2 + kspan;
        } while (kk < nt);
        k2 = kk - nt;
        c1 = -c1;
        kk = k1 - k2;
    } while (kk > k2);
    kk += jc;
    if (kk <= mm)
        goto lbl70;
    if (kk < k2)
        goto lbl90;
    k1 += (inc + inc);
    kk = ((k1 - kspan) / 2) + jc;
    if (kk <= (jc + jc))
        goto lbl60;
    goto lbl40;
lbl90:
    s1 = ((double)((kk - 1) / jc)) * dr * rad;
    {
        const auto both = sineCosine(s1);
        c1 = both.cosine;
        s1 = both.sine;
    }
    mm = std::min(k1 / 2, mm + klim);
    goto lbl80;

    // transform for factor of 4, the only other factor of a power of two
lbl110:
    kspan = kspan / 4;
lbl120:
    c1 = 1.0;
    s1 = 0;
    mm = std::min(kspan, klim);
    goto lbl150;
lbl130:
    c2 = c1 - std::fma(cd, c1, sd * s1);
    s1 = std::fma(sd, c1, -(cd * s1)) + s1;
    c1 = c2;
lbl140:
    c2 = std::fma(c1, c1, -(s1 * s1));
    s2 = c1 * s1 * 2.0;
    c3 = std::fma(c2, c1, -(s2 * s1));
    s3 = std::fma(c2, s1, s2 * c1);
lbl150:
    k1 = kk + kspan;
    k2 = k1 + kspan;
    k3 = k2 + kspan;
    akp = a(kk) + a(k2);
    akm = a(kk) - a(k2);
    ajp = a(k1) + a(k3);
    ajm = a(k1) - a(k3);
    a(kk) = akp + ajp;
    ajp = akp - ajp;
    bkp = b(kk) + b(k2);
    bkm = b(kk) - b(k2);
    bjp = b(k1) + b(k3);
    bjm = b(k1) - b(k3);
    b(kk) = bkp + bjp;
    bjp = bkp - bjp;
    if (isn < 0)
        goto lbl180;
    akp = akm - bjm;
    akm = akm + bjm;
    bkp = bkm + ajm;
    bkm = bkm - ajm;
    if (s1 == 0.0)
        goto lbl190;
lbl160:
    a(k1) = std::fma(akp, c1, -(bkp * s1));
    b(k1) = std::fma(akp, s1, bkp * c1);
    a(k2) = std::fma(ajp, c2, -(bjp * s2));
    b(k2) = std::fma(ajp, s2, bjp * c2);
    a(k3) = std::fma(akm, c3, -(bkm * s3));
    b(k3) = std::fma(akm, s3, bkm * c3);
    kk = k3 + kspan;
    if (kk <= nt)
        goto lbl150;
lbl170:
    kk -= (nt - jc);
    if (kk <= mm)
        goto lbl130;
    if (kk < kspan)
        goto lbl200;
    kk -= (kspan - inc);
    if (kk <= jc)
        goto lbl120;
    if (kspan == jc)
        goto lbl350;
    goto lbl40;
lbl180:
    akp = akm + bjm;
    akm = akm - bjm;
    bkp = bkm - ajm;
    bkm = bkm + ajm;
    if (s1 != 0.0)
        goto lbl160;
lbl190:
    a(k1) = akp;
    b(k1) = bkp;
    a(k2) = ajp;
    b(k2) = bjp;
    a(k3) = akm;
    b(k3) = bkm;
    kk = k3 + kspan;
    if (kk <= nt)
        goto lbl150;
    goto lbl170;
lbl200:
    s1 = ((double)((kk - 1) / jc)) * dr * rad;
    {
        const auto both = sineCosine(s1);
        c1 = both.cosine;
        s1 = both.sine;
    }
    mm = std::min(kspan, mm + klim);
    goto lbl140;

    // permute the results to normal order: the permutation for square
    // factors, single-variate as n is ntot; a power of two has no others
lbl350:
    np(1) = ks;
    if (!kt)
        return;
    k = kt + kt + 1;
    if (m < k)
        k--;
    np(k + 1) = jc;
    for (j = 1; j < k; j++, k--)
    {
        np(j + 1) = np(j) / nfac[j];
        np(k) = np(k + 1) * nfac[j];
    }
    k3 = np(k + 1);
    kspan = np(2);
    kk = jc + 1;
    k2 = kspan + 1;
    j = 1;
lbl370:
    do
    {
        ak = a(kk);
        a(kk) = a(k2);
        a(k2) = ak;
        bk = b(kk);
        b(kk) = b(k2);
        b(k2) = bk;
        kk += inc;
        k2 += kspan;
    } while (k2 < ks);
lbl380:
    do
    {
        k2 -= np(j);
        j++;
        k2 += np(j + 1);
    } while (k2 > np(j));
    j = 1;
lbl390:
    if (kk < k2)
        goto lbl370;
    kk += inc;
    k2 += kspan;
    if (k2 < ks)
        goto lbl390;
    if (kk < ks)
        goto lbl380;
}

// reals_ (mxfftd.c), with FORTRAN's indices from 1.
void reals(double* a0, double* b0, int n, int isn) noexcept
{
    const auto a = [a0](int j) -> double& { return a0[j - 1]; };
    const auto b = [b0](int j) -> double& { return b0[j - 1]; };
    int inc, j, k, lim, mm, ml, nf, nk, nh;
    double aa, ab, ba, bb, cd, cn, dr, em, rad, re, sd, sn;
    double xx;
    inc = std::abs(isn);
    nf = std::abs(n);
    nk = (nf * inc) + 2;
    nh = nk / 2;
    rad = 0.785398163397448278900;
    dr = -4.0 / (double)(nf);
    xx = std::sin((double)0.5 * dr * rad);
    cd = 2.0 * xx * xx;
    sd = std::sin(dr * rad);
    // sin,cos values are re-initialised each lim steps
    lim = 32;
    mm = lim;
    ml = 0;
    sn = 0.0;
    if (isn < 0)
    {
        cn = 1.0;
        a(nk - 1) = a(1);
        b(nk - 1) = b(1);
    }
    else
    {
        cn = -1.0;
        sd = -sd;
    }
    for (j = 1; j <= nh; j += inc)
    {
        k = nk - j;
        aa = a(j) + a(k);
        ab = a(j) - a(k);
        ba = b(j) + b(k);
        bb = b(j) - b(k);
        re = std::fma(cn, ba, sn * ab);
        em = std::fma(sn, ba, -(cn * ab));
        b(k) = (em - bb) * 0.5;
        b(j) = (em + bb) * 0.5;
        a(k) = (aa - re) * 0.5;
        a(j) = (aa + re) * 0.5;
        ml++;
        if (ml != mm)
        {
            aa = cn - std::fma(cd, cn, sd * sn);
            sn = std::fma(sd, cn, -(cd * sn)) + sn;
            cn = aa;
        }
        else
        {
            mm += lim;
            sn = ((double)ml) * dr * rad;
            const auto both = sineCosine(sn);
            cn = both.cosine;
            if (isn > 0)
                cn = -cn;
            sn = both.sine;
        }
    }
}

// fft_'s factors of n, from nfac[1], m of them and kt square factors, as it
// finds them at each call. False for more than fifteen, which CDP refuses.
[[nodiscard]] bool factor(int n, std::array<int, 32>& nfac, int& m, int& kt) noexcept
{
    int k = n;
    int j = 0;
    int jj = 0;
    for (m = 0; !(k % 16); nfac[static_cast<std::size_t>(++m)] = 4, k /= 16)
    {
    }
    for (j = 3, jj = 9; jj <= k; j += 2, jj = j * j)
        for (; !(k % jj); nfac[static_cast<std::size_t>(++m)] = j, k /= jj)
        {
        }
    if (k <= 4)
    {
        kt = m;
        nfac[static_cast<std::size_t>(m + 1)] = k;
        if (k != 1)
            m++;
    }
    else
    {
        if (k % 4 == 0)
        {
            nfac[static_cast<std::size_t>(++m)] = 2;
            k /= 4;
        }
        kt = m;
        for (j = 2; j <= k; j = 1 + ((j + 1) / 2) * 2)
            if (k % j == 0)
            {
                nfac[static_cast<std::size_t>(++m)] = j;
                k /= j;
            }
    }
    if (m + kt > 15)
        return false;
    if (kt != 0)
    {
        j = kt;
        while (j)
            nfac[static_cast<std::size_t>(++m)] = nfac[static_cast<std::size_t>(j--)];
    }
    return true;
}
}

bool FastConvolution::prepare(
    double sampleRate, std::span<const double> impulse, double impulseSampleRate, const FastConvolutionSettings& settings)
{
    const auto channels = settings.impulseChannels;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || impulseSampleRate != sampleRate || channels == 0
        || impulse.empty() || impulse.size() % channels != 0 || impulse.size() / channels > maximumImpulseFrames
        || settings.channel >= channels || settings.inputChannels == 0
        || (channels > 1 && settings.inputChannels != 1 && settings.inputChannels != channels)
        || !(settings.dry >= 0.0 && settings.dry <= 1.0) || !std::isfinite(settings.scale)
        || !(settings.scale > 0.0)
        || !std::ranges::all_of(impulse, [](double sample) { return std::isfinite(sample); }))
    {
        return false;
    }
    const auto inframes = static_cast<int>(impulse.size() / channels);
    const auto imchans = static_cast<int>(channels);
    // main's and genimpframe2's fftlen: convolution style, double-length.
    int fftlen = 1;
    while (fftlen < inframes * 2)
        fftlen <<= 1;
    std::array<int, 32> nfac{};
    int m = 0;
    int kt = 0;
    if (!factor(fftlen / 2, nfac, m, kt))
        return false;

    // genimpframe2: the user's scaling first, then each channel's RMS level,
    // its sum of squares fused as CDP8's build fuses it; the loudest sets
    // the gain.
    std::vector<double> scaled(impulse.begin(), impulse.end());
    for (auto& sample : scaled)
        sample *= settings.scale;
    double maxrmsfac = 0.0;
    for (int j = 0; j < imchans; j++)
    {
        double rmsfac = 0.0;
        for (int i = 0; i < inframes; i++)
        {
            const double val = scaled[static_cast<std::size_t>(i * imchans + j)];
            rmsfac = std::fma(val, val, rmsfac);
        }
        rmsfac /= inframes;
        rmsfac = std::sqrt(rmsfac);
        if (rmsfac > maxrmsfac)
            maxrmsfac = rmsfac;
    }
    const double rms = maxrmsfac;

    // main's gain ("most of this sheer guesswork!"). A silent impulse
    // response measures -inf dB, and its gain is 0.
    const double norm = std::sqrt(2.0);
    double Ninv = fftlen;
    Ninv /= std::sqrt(static_cast<double>(inframes * 2));
    Ninv *= norm;
    Ninv /= inframes;
    const double rmsdif2 = (-20.0 * std::log10(rms)) * 0.5;
    const double rmsadjust = cdpExp10(rmsdif2 / 20.0);
    Ninv /= rmsadjust;

    // main's dry and wet: a dry of 1 leaves dry at 0.
    double dry = 0.0;
    double wet = 1.0;
    if (settings.dry == 1.0)
    {
        wet = 0.0;
    }
    else
    {
        const auto both = sineCosine(pi2 * settings.dry);
        dry = both.sine;
        wet = both.cosine;
    }

    // genimpframe2's spectrum of this output's channel.
    const auto size = static_cast<std::size_t>(fftlen) + 2;
    std::vector<double> spectrum(size, 0.0);
    for (int i = 0; i < inframes; i++)
        spectrum[static_cast<std::size_t>(i)] = scaled[static_cast<std::size_t>(i * imchans) + settings.channel];
    std::array<int, 34> np{};
    transform(spectrum, -2, nfac, m, kt, np);
    std::vector<double> work(size, 0.0);
    std::vector<double> overlap(static_cast<std::size_t>(inframes), 0.0);
    std::vector<double> inputBlock(static_cast<std::size_t>(inframes), 0.0);
    std::vector<double> outputBlock(static_cast<std::size_t>(inframes), 0.0);

    frames_ = static_cast<std::size_t>(inframes);
    fftLength_ = static_cast<std::size_t>(fftlen);
    nfac_ = nfac;
    factorCount_ = m;
    squareFactors_ = kt;
    gain_ = Ninv;
    dryGain_ = dry;
    wetGain_ = wet;
    // main's mono-input loop rounds the dry part as a statement of its own.
    fusedDry_ = !(channels > 1 && settings.inputChannels == 1);
    impulseSpectrum_ = std::move(spectrum);
    work_ = std::move(work);
    overlap_ = std::move(overlap);
    inputBlock_ = std::move(inputBlock);
    outputBlock_ = std::move(outputBlock);
    reset();
    return true;
}

void FastConvolution::reset() noexcept
{
    std::ranges::fill(work_, 0.0);
    std::ranges::fill(overlap_, 0.0);
    std::ranges::fill(inputBlock_, 0.0);
    std::ranges::fill(outputBlock_, 0.0);
    position_ = 0;
    started_ = false;
}

// fft_ then reals_ forwards (isn -2), or reals_ then fft_ backwards (2), on
// interleaved complex points.
void FastConvolution::transform(std::span<double> data, int isn, const std::array<int, 32>& nfac, int m, int kt,
    std::array<int, 34>& np) noexcept
{
    auto* anal = data.data();
    const int n = static_cast<int>((data.size() - 2) / 2);
    const auto fft = [&] {
        // fft_ returns at once for a single point.
        if (n != 1)
            fftmx(anal, anal + 1, n, n, n, isn, m, kt, nfac.data(), np.data());
    };
    if (isn < 0)
    {
        fft();
        reals(anal, anal + 1, n, isn);
    }
    else
    {
        reals(anal, anal + 1, n, isn);
        fft();
    }
}

// One pass of main's loop for a block of frames_ input frames.
void FastConvolution::convolveBlock() noexcept
{
    const auto imlen = frames_;
    std::ranges::fill(work_, 0.0);
    std::copy(inputBlock_.begin(), inputBlock_.end(), work_.begin());
    transform(work_, -2, nfac_, factorCount_, squareFactors_, np_);
    // complexmult, fused as CDP8's build fuses it.
    for (std::size_t i = 0, j = 1; i < work_.size(); i += 2, j += 2)
    {
        const double re = std::fma(work_[i], impulseSpectrum_[i], -(work_[j] * impulseSpectrum_[j]));
        const double im = std::fma(work_[i], impulseSpectrum_[j], work_[j] * impulseSpectrum_[i]);
        work_[i] = re;
        work_[j] = im;
    }
    transform(work_, 2, nfac_, factorCount_, squareFactors_, np_);
    for (std::size_t i = 0; i < imlen; i++)
    {
        double out = work_[i] + overlap_[i];
        overlap_[i] = work_[i + imlen];
        out *= gain_;
        out *= wetGain_;
        if (fusedDry_)
        {
            out = std::fma(dryGain_, inputBlock_[i], out);
        }
        else
        {
            const double inval = dryGain_ * inputBlock_[i];
            out += inval;
        }
        outputBlock_[i] = static_cast<double>(static_cast<float>(out));
    }
}

bool FastConvolution::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); }))
    {
        return false;
    }
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        inputBlock_[position_] = input[index];
        if (position_ + 1 == frames_)
        {
            convolveBlock();
            position_ = 0;
            started_ = true;
            output[index] = outputBlock_[0];
        }
        else
        {
            ++position_;
            output[index] = started_ ? outputBlock_[position_] : 0.0;
        }
    }
    return true;
}
}
