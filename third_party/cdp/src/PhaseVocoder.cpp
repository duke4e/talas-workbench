/*
 * Derived from the CDP System, Release 8: dev/pv/pvoc.c and mxfft.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/PhaseVocoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h, which pvoc.c reads.
constexpr double PI = 3.141592654;
constexpr double TWOPI = 2.0 * PI;

// mxfft.c's factors of n, as fft_ finds them: nfac from 1, m of them and kt
// square factors, with the work space sizes fft_ allocates.
struct Factors
{
    int nfac[16] = {};
    int m = 0;
    int kt = 0;
    int maxf = 0;
    int maxp = 0;
};

[[nodiscard]] std::optional<Factors> factor(int n) noexcept
{
    Factors result;
    auto& nfac = result.nfac;
    int m = 0;
    int kt = 0;
    int maxp = 0;
    int j = 0;
    int jj = 0;
    int k = std::abs(n);
    for (m = 0; !(k % 16); nfac[++m] = 4, k /= 16)
    {
    }
    for (j = 3, jj = 9; jj <= k; j += 2, jj = j * j)
        for (; !(k % jj); nfac[++m] = j, k /= jj)
        {
        }
    if (k <= 4)
    {
        kt = m;
        nfac[m + 1] = k;
        if (k != 1)
            m++;
    }
    else
    {
        if (k % 4 == 0)
        {
            nfac[++m] = 2;
            k /= 4;
        }
        kt = m;
        maxp = std::max((kt + kt + 2), (k - 1));
        for (j = 2; j <= k; j = 1 + ((j + 1) / 2) * 2)
            if (k % j == 0)
            {
                // CDP checks its fifteen factors only afterwards.
                if (m >= 15)
                    return std::nullopt;
                nfac[++m] = j;
                k /= j;
            }
    }
    if (m <= kt + 1)
        maxp = m + kt + 1;
    if (m + kt > 15)
        return std::nullopt;
    if (kt != 0)
    {
        j = kt;
        while (j)
            nfac[++m] = nfac[j--];
    }
    int maxf = nfac[m - kt];
    if (kt > 0)
        maxf = std::max(nfac[kt], maxf);
    result.m = m;
    result.kt = kt;
    result.maxf = maxf;
    result.maxp = maxp;
    return result;
}

// fftmx, with FORTRAN's indices from 1: a(j) is a0[j - 1], and so on.
void fftmx(float* a0, float* b0, int ntot, int n, int nspan, int isn, int m, int* kt,
    float* at0, float* ck0, float* bt0, float* sk0, int* np0, int nfac[]) noexcept
{
    const auto a = [a0](int j) -> float& { return a0[j - 1]; };
    const auto b = [b0](int j) -> float& { return b0[j - 1]; };
    const auto at = [at0](int j) -> float& { return at0[j - 1]; };
    const auto bt = [bt0](int j) -> float& { return bt0[j - 1]; };
    const auto ck = [ck0](int j) -> float& { return ck0[j - 1]; };
    const auto sk = [sk0](int j) -> float& { return sk0[j - 1]; };
    const auto np = [np0](int j) -> int& { return np0[j - 1]; };
    int i, inc,
        j, jc, jf, jj,
        k, k1, k2, k3 = 0, k4,
        kk, klim, ks, kspan, kspnn,
        lim,
        maxf, mm,
        nn, nt;
    double aa, aj, ajm, ajp, ak, akm, akp,
        bb, bj, bjm, bjp, bk, bkm, bkp,
        c1, c2 = 0.0, c3 = 0.0, c72, cd,
        dr,
        rad,
        sd, s1, s2 = 0.0, s3 = 0.0, s72, s120;

    double xx;
    inc = std::abs(isn);
    nt = inc * ntot;
    ks = inc * nspan;
    rad = 0.785398163397448278900;
    c72 = 0.309016994374947451270;
    s72 = 0.951056516295153531190;
    s120 = 0.866025403784438707600;

    // scale by 1/n for isn > 0 ( reverse transform )
    if (isn < 0)
    {
        s72 = -s72;
        s120 = -s120;
        rad = -rad;
    }
    else
    {
        ak = 1.0 / (double)n;
        for (j = 1; j <= nt; j += inc)
        {
            a(j) = (float)(a(j) * ak);
            b(j) = (float)(b(j) * ak);
        }
    }
    kspan = ks;
    nn = nt - inc;
    jc = ks / n;

    // sin, cos values are re-initialized each lim steps
    lim = 32;
    klim = lim * jc;
    i = 0;
    jf = 0;
    maxf = m - (*kt);
    maxf = nfac[maxf];
    if ((*kt) > 0)
        maxf = std::max(nfac[*kt], maxf);

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
            a(k2) = (float)((a(kk)) - ak);
            b(k2) = (float)((b(kk)) - bk);
            a(kk) = (float)((a(kk)) + ak);
            b(kk) = (float)((b(kk)) + bk);
            kk = k2 + kspan;
        } while (kk <= nn);
        kk -= nn;
    } while (kk <= jc);
    if (kk > kspan)
        goto lbl350;
lbl60:
    c1 = 1.0 - cd;
    s1 = sd;
    mm = std::min((k1 / 2), klim);
    goto lbl80;
lbl70:
    ak = c1 - ((cd * c1) + (sd * s1));
    s1 = ((sd * c1) - (cd * s1)) + s1;
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
            a(k2) = (float)((c1 * ak) - (s1 * bk));
            b(k2) = (float)((s1 * ak) + (c1 * bk));
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
    c1 = std::cos(s1);
    s1 = std::sin(s1);
    mm = std::min(k1 / 2, mm + klim);
    goto lbl80;

    // transform for factor of 3 (optional code)
lbl100:
    k1 = kk + kspan;
    k2 = k1 + kspan;
    ak = a(kk);
    bk = b(kk);
    aj = a(k1) + a(k2);
    bj = b(k1) + b(k2);
    a(kk) = (float)(ak + aj);
    b(kk) = (float)(bk + bj);
    ak += (-0.5 * aj);
    bk += (-0.5 * bj);
    aj = (a(k1) - a(k2)) * s120;
    bj = (b(k1) - b(k2)) * s120;
    a(k1) = (float)(ak - bj);
    b(k1) = (float)(bk + aj);
    a(k2) = (float)(ak + bj);
    b(k2) = (float)(bk - aj);
    kk = k2 + kspan;
    if (kk < nn)
        goto lbl100;
    kk -= nn;
    if (kk <= kspan)
        goto lbl100;
    goto lbl290;

    // transform for factor of 4
lbl110:
    if (nfac[i] != 4)
        goto lbl230;
    kspnn = kspan;
    kspan = kspan / 4;
lbl120:
    c1 = 1.0;
    s1 = 0;
    mm = std::min(kspan, klim);
    goto lbl150;
lbl130:
    c2 = c1 - ((cd * c1) + (sd * s1));
    s1 = ((sd * c1) - (cd * s1)) + s1;
    c1 = c2;
lbl140:
    c2 = (c1 * c1) - (s1 * s1);
    s2 = c1 * s1 * 2.0;
    c3 = (c2 * c1) - (s2 * s1);
    s3 = (c2 * s1) + (s2 * c1);
lbl150:
    k1 = kk + kspan;
    k2 = k1 + kspan;
    k3 = k2 + kspan;
    akp = a(kk) + a(k2);
    akm = a(kk) - a(k2);
    ajp = a(k1) + a(k3);
    ajm = a(k1) - a(k3);
    a(kk) = (float)(akp + ajp);
    ajp = akp - ajp;
    bkp = b(kk) + b(k2);
    bkm = b(kk) - b(k2);
    bjp = b(k1) + b(k3);
    bjm = b(k1) - b(k3);
    b(kk) = (float)(bkp + bjp);
    bjp = (float)(bkp - bjp);
    if (isn < 0)
        goto lbl180;
    akp = akm - bjm;
    akm = akm + bjm;
    bkp = bkm + ajm;
    bkm = bkm - ajm;
    if (s1 == 0.0)
        goto lbl190;
lbl160:
    a(k1) = (float)((akp * c1) - (bkp * s1));
    b(k1) = (float)((akp * s1) + (bkp * c1));
    a(k2) = (float)((ajp * c2) - (bjp * s2));
    b(k2) = (float)((ajp * s2) + (bjp * c2));
    a(k3) = (float)((akm * c3) - (bkm * s3));
    b(k3) = (float)((akm * s3) + (bkm * c3));
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
    a(k1) = (float)akp;
    b(k1) = (float)bkp;
    a(k2) = (float)ajp;
    b(k2) = (float)bjp;
    a(k3) = (float)akm;
    b(k3) = (float)bkm;
    kk = k3 + kspan;
    if (kk <= nt)
        goto lbl150;
    goto lbl170;
lbl200:
    s1 = ((double)((kk - 1) / jc)) * dr * rad;
    c1 = std::cos(s1);
    s1 = std::sin(s1);
    mm = std::min(kspan, mm + klim);
    goto lbl140;

    // transform for factor of 5 (optional code)
lbl210:
    c2 = (c72 * c72) - (s72 * s72);
    s2 = 2.0 * c72 * s72;
lbl220:
    k1 = kk + kspan;
    k2 = k1 + kspan;
    k3 = k2 + kspan;
    k4 = k3 + kspan;
    akp = a(k1) + a(k4);
    akm = a(k1) - a(k4);
    bkp = b(k1) + b(k4);
    bkm = b(k1) - b(k4);
    ajp = a(k2) + a(k3);
    ajm = a(k2) - a(k3);
    bjp = b(k2) + b(k3);
    bjm = b(k2) - b(k3);
    aa = a(kk);
    bb = b(kk);
    a(kk) = (float)(aa + akp + ajp);
    b(kk) = (float)(bb + bkp + bjp);
    ak = (akp * c72) + (ajp * c2) + aa;
    bk = (bkp * c72) + (bjp * c2) + bb;
    aj = (akm * s72) + (ajm * s2);
    bj = (bkm * s72) + (bjm * s2);
    a(k1) = (float)(ak - bj);
    a(k4) = (float)(ak + bj);
    b(k1) = (float)(bk + aj);
    b(k4) = (float)(bk - aj);
    ak = (akp * c2) + (ajp * c72) + aa;
    bk = (bkp * c2) + (bjp * c72) + bb;
    aj = (akm * s2) - (ajm * s72);
    bj = (bkm * s2) - (bjm * s72);
    a(k2) = (float)(ak - bj);
    a(k3) = (float)(ak + bj);
    b(k2) = (float)(bk + aj);
    b(k3) = (float)(bk - aj);
    kk = k4 + kspan;
    if (kk < nn)
        goto lbl220;
    kk -= nn;
    if (kk <= kspan)
        goto lbl220;
    goto lbl290;

    // transform for odd factors
lbl230:
    k = nfac[i];
    kspnn = kspan;
    kspan /= k;
    if (k == 3)
        goto lbl100;
    if (k == 5)
        goto lbl210;
    if (k == jf)
        goto lbl250;
    jf = k;
    s1 = rad / (((double)(k)) / 8.0);
    c1 = std::cos(s1);
    s1 = std::sin(s1);
    ck(jf) = 1.0f;
    sk(jf) = 0.0f;
    for (j = 1; j < k; j++)
    {
        ck(j) = (float)((ck(k)) * c1 + (sk(k)) * s1);
        sk(j) = (float)((ck(k)) * s1 - (sk(k)) * c1);
        k--;
        ck(k) = ck(j);
        sk(k) = -(sk(j));
    }
lbl250:
    k1 = kk;
    k2 = kk + kspnn;
    aa = a(kk);
    bb = b(kk);
    ak = aa;
    bk = bb;
    j = 1;
    k1 += kspan;
    do
    {
        k2 -= kspan;
        j++;
        at(j) = a(k1) + a(k2);
        ak = at(j) + ak;
        bt(j) = b(k1) + b(k2);
        bk = bt(j) + bk;
        j++;
        at(j) = a(k1) - a(k2);
        bt(j) = b(k1) - b(k2);
        k1 += kspan;
    } while (k1 < k2);
    a(kk) = (float)ak;
    b(kk) = (float)bk;
    k1 = kk;
    k2 = kk + kspnn;
    j = 1;
lbl270:
    k1 += kspan;
    k2 -= kspan;
    jj = j;
    ak = aa;
    bk = bb;
    aj = 0.0;
    bj = 0.0;
    k = 1;
    do
    {
        k++;
        ak = (at(k) * ck(jj)) + ak;
        bk = (bt(k) * ck(jj)) + bk;
        k++;
        aj = (at(k) * sk(jj)) + aj;
        bj = (bt(k) * sk(jj)) + bj;
        jj += j;
        if (jj > jf)
            jj -= jf;
    } while (k < jf);
    k = jf - j;
    a(k1) = (float)(ak - bj);
    b(k1) = (float)(bk + aj);
    a(k2) = (float)(ak + bj);
    b(k2) = (float)(bk - aj);
    j++;
    if (j < k)
        goto lbl270;
    kk += kspnn;
    if (kk <= nn)
        goto lbl250;
    kk -= nn;
    if (kk <= kspan)
        goto lbl250;

    // multiply by rotation factor (except for factors of 2 and 4)
lbl290:
    if (i == m)
        goto lbl350;
    kk = jc + 1;
lbl300:
    c2 = 1.0 - cd;
    s1 = sd;
    mm = std::min(kspan, klim);
    goto lbl320;
lbl310:
    c2 = c1 - ((cd * c1) + (sd * s1));
    s1 = s1 + ((sd * c1) - (cd * s1));
lbl320:
    c1 = c2;
    s2 = s1;
    kk += kspan;
lbl330:
    ak = a(kk);
    a(kk) = (float)((c2 * ak) - (s2 * b(kk)));
    b(kk) = (float)((s2 * ak) + (c2 * b(kk)));
    kk += kspnn;
    if (kk <= nt)
        goto lbl330;
    ak = s1 * s2;
    s2 = (s1 * c2) + (c1 * s2);
    c2 = (c1 * c2) - ak;
    kk -= (nt - kspan);
    if (kk <= kspnn)
        goto lbl330;
    kk -= (kspnn - jc);
    if (kk <= mm)
        goto lbl310;
    if (kk < kspan)
        goto lbl340;
    kk -= (kspan - jc - inc);
    if (kk <= (jc + jc))
        goto lbl300;
    goto lbl40;
lbl340:
    s1 = ((double)((kk - 1) / jc)) * dr * rad;
    c2 = std::cos(s1);
    s1 = std::sin(s1);
    mm = std::min(kspan, mm + klim);
    goto lbl320;

    // permute the results to normal order---done in two stages
    // permutation for square factors of n
lbl350:
    np(1) = ks;
    if (!(*kt))
        goto lbl440;
    k = *kt + *kt + 1;
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
    if (n != ntot)
        goto lbl400;
    // permutation for single-variate transform (optional code)
lbl370:
    do
    {
        ak = a(kk);
        a(kk) = a(k2);
        a(k2) = (float)ak;
        bk = b(kk);
        b(kk) = b(k2);
        b(k2) = (float)bk;
        kk += inc;
        k2 += kspan;
    } while (k2 < ks);
lbl380:
    do
    {
        k2 -= np(j++);
        k2 += np(j + 1);
    } while (k2 > np(j));
    j = 1;
lbl390:
    if (kk < k2)
    {
        goto lbl370;
    }
    kk += inc;
    k2 += kspan;
    if (k2 < ks)
        goto lbl390;
    if (kk < ks)
        goto lbl380;
    jc = k3;
    goto lbl440;
    // permutation for multivariate transform
lbl400:
    do
    {
        do
        {
            k = kk + jc;
            do
            {
                ak = a(kk);
                a(kk) = a(k2);
                a(k2) = (float)ak;
                bk = b(kk);
                b(kk) = b(k2);
                b(k2) = (float)bk;
                kk += inc;
                k2 += inc;
            } while (kk < k);
            kk += (ks - jc);
            k2 += (ks - jc);
        } while (kk < nt);
        k2 -= (nt - kspan);
        kk -= (nt - jc);
    } while (k2 < ks);
lbl420:
    do
    {
        k2 -= np(j++);
        k2 += np(j + 1);
    } while (k2 > np(j));
    j = 1;
lbl430:
    if (kk < k2)
        goto lbl400;
    kk += jc;
    k2 += kspan;
    if (k2 < ks)
        goto lbl430;
    if (kk < ks)
        goto lbl420;
    jc = k3;
lbl440:
    if ((2 * (*kt)) + 1 >= m)
        return;

    kspnn = np(*(kt) + 1);
    j = m - *kt;
    nfac[j + 1] = 1;
lbl450:
    nfac[j] = nfac[j] * nfac[j + 1];
    j--;
    if (j != *kt)
        goto lbl450;
    *kt = *(kt) + 1;
    nn = nfac[*kt] - 1;
    jj = 0;
    j = 0;
    goto lbl480;
lbl460:
    jj -= k2;
    k2 = kk;
    kk = nfac[++k];
lbl470:
    jj += kk;
    if (jj >= k2)
        goto lbl460;
    np(j) = jj;
lbl480:
    k2 = nfac[*kt];
    k = *kt + 1;
    kk = nfac[k];
    j++;
    if (j <= nn)
        goto lbl470;
    // Determine permutation cycles of length greater than 1
    j = 0;
    goto lbl500;
lbl490:
    k = kk;
    kk = np(k);
    np(k) = -kk;
    if (kk != j)
        goto lbl490;
    k3 = kk;
lbl500:
    kk = np(++j);
    if (kk < 0)
        goto lbl500;
    if (kk != j)
        goto lbl490;
    np(j) = -j;
    if (j != nn)
        goto lbl500;
    maxf *= inc;
    // Perform reordering following permutation cycles
    goto lbl570;
lbl510:
    j--;
    if (np(j) < 0)
        goto lbl510;
    jj = jc;
lbl520:
    kspan = jj;
    if (jj > maxf)
        kspan = maxf;
    jj -= kspan;
    k = np(j);
    kk = (jc * k) + i + jj;
    k1 = kk + kspan;
    k2 = 0;
lbl530:
    k2++;
    at(k2) = a(k1);
    bt(k2) = b(k1);
    k1 -= inc;
    if (k1 != kk)
        goto lbl530;
lbl540:
    k1 = kk + kspan;
    k2 = k1 - (jc * (k + np(k)));
    k = -(np(k));
lbl550:
    a(k1) = a(k2);
    b(k1) = b(k2);
    k1 -= inc;
    k2 -= inc;
    if (k1 != kk)
        goto lbl550;
    kk = k2;
    if (k != j)
        goto lbl540;
    k1 = kk + kspan;
    k2 = 0;
lbl560:
    k2++;
    a(k1) = at(k2);
    b(k1) = bt(k2);
    k1 -= inc;
    if (k1 != kk)
        goto lbl560;
    if (jj)
        goto lbl520;
    if (j != 1)
        goto lbl510;
lbl570:
    j = k3 + 1;
    nt -= kspnn;
    i = nt - inc + 1;
    if (nt >= 0)
        goto lbl510;
}

// reals_, with FORTRAN's indices from 1.
void reals(float* a0, float* b0, int n, int isn) noexcept
{
    const auto a = [a0](int j) -> float& { return a0[j - 1]; };
    const auto b = [b0](int j) -> float& { return b0[j - 1]; };
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
    // sin,cos values are re-initialized each lim steps
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
        re = (cn * ba) + (sn * ab);
        em = (sn * ba) - (cn * ab);
        b(k) = (float)((em - bb) * 0.5);
        b(j) = (float)((em + bb) * 0.5);
        a(k) = (float)((aa - re) * 0.5);
        a(j) = (float)((aa + re) * 0.5);
        ml++;
        if (ml != mm)
        {
            aa = cn - ((cd * cn) + (sd * sn));
            sn = ((sd * cn) - (cd * sn)) + sn;
            cn = aa;
        }
        else
        {
            mm += lim;
            sn = ((float)ml) * dr * rad;
            cn = std::cos(sn);
            if (isn > 0)
                cn = -cn;
            sn = std::sin(sn);
        }
    }
}

// pvoc.c's hamming, from the window's centre.
void hamming(float* win, int winLen, int even) noexcept
{
    float Pi, ftmp;
    int i;
    Pi = (float)PI;
    ftmp = Pi / winLen;
    if (even)
    {
        for (i = 0; i < winLen; i++)
            *(win + i) = (float)((double).54 + (double).46 * std::cos((double)(ftmp * ((float)i + .5))));
        *(win + winLen) = 0.0f;
    }
    else
    {
        *(win) = 1.0f;
        for (i = 1; i <= winLen; i++)
            *(win + i) = (float)((double).54 + (double).46 * std::cos((double)(ftmp * (float)i)));
    }
}
}

namespace
{
// pvoc_preprocess's points and pvoc_process's window length M and
// decimation D.
struct Sizes
{
    int chans = 0;
    int n2 = 0;
    int m = 0;
    int d = 0;
};

[[nodiscard]] std::optional<Sizes> sizesFor(int points, int overlap) noexcept
{
    if (points < PhaseVocoder::minimumPoints || points > PhaseVocoder::maximumPoints || overlap < 1
        || overlap > 4)
    {
        return std::nullopt;
    }
    Sizes sizes;
    sizes.chans = points + (points % 2);
    sizes.n2 = sizes.chans / 2;
    if (!factor(sizes.n2).has_value())
        return std::nullopt;
    switch (overlap - 1)
    {
    case 0: sizes.m = 4 * sizes.chans; break;
    case 1: sizes.m = 2 * sizes.chans; break;
    case 2: sizes.m = sizes.chans; break;
    default: sizes.m = sizes.n2; break;
    }
    sizes.d = static_cast<int>(sizes.m / 8.0);
    if (sizes.d == 0)
        sizes.d = 1;
    return sizes;
}
}

std::optional<std::size_t> PhaseVocoder::latencyFor(int points, int overlap) noexcept
{
    const auto sizes = sizesFor(points, overlap);
    if (!sizes.has_value())
        return std::nullopt;
    // The most input frames read, less one, before an output frame is
    // shifted out, counting as pvoc_process counts.
    const auto d = sizes->d;
    const auto analWinLen = sizes->m / 2;
    const auto synWinLen = sizes->m / 2;
    const auto nI0 = -(analWinLen / d) * d;
    auto consumed = static_cast<long long>(analWinLen + nI0 + 1);
    long long produced = 0;
    long long latency = 0;
    auto nO = nI0;
    auto iOi = 0;
    for (int iteration = 0; iteration < (analWinLen + synWinLen) / d + 8; iteration++)
    {
        if (iOi > 0)
            latency = std::max(latency, consumed - 1 - produced);
        produced += iOi;
        nO += d;
        iOi = nO > (synWinLen + d) ? d : nO > synWinLen ? nO - synWinLen : 0;
        consumed += d;
    }
    return static_cast<std::size_t>(latency);
}

std::optional<int> PhaseVocoder::decimationFor(int points, int overlap) noexcept
{
    const auto sizes = sizesFor(points, overlap);
    if (!sizes.has_value())
        return std::nullopt;
    return sizes->d;
}

bool PhaseVocoder::prepare(double sampleRate, int points, int overlap)
{
    const auto sizes = sizesFor(points, overlap);
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || !sizes.has_value())
        return false;
    const auto chans = sizes->chans;
    const auto n2 = sizes->n2;
    const auto factors = factor(n2);
    const auto m = sizes->m;
    const auto mf = 1 - m % 2;
    const auto d = sizes->d;
    const auto i = d;
    const auto io = i;
    const auto R = static_cast<float>(sampleRate);
    n_ = chans;
    n2_ = n2;
    m_ = m;
    d_ = d;
    ibuflen_ = 4 * m;
    obuflen_ = 4 * m;
    f_ = (float)(R / (float)chans);

    analWindow_.assign(static_cast<std::size_t>(m + mf), 0.0F);
    analWinLen_ = m / 2;
    auto* analWindow = analWindow_.data() + analWinLen_;
    hamming(analWindow, analWinLen_, mf);
    for (int k = 1; k <= analWinLen_; k++)
        *(analWindow - k) = *(analWindow + k - mf);
    if (m > chans)
    {
        if (mf)
            *analWindow *= (float)((double)chans * std::sin((double)PI * .5 / chans) / (double)(PI * .5));
        for (int k = 1; k <= analWinLen_; k++)
            *(analWindow + k) *= (float)((double)chans * std::sin((double)(PI * (k + .5 * mf) / chans))
                / (PI * (k + .5 * mf)));
        for (int k = 1; k <= analWinLen_; k++)
            *(analWindow - k) = *(analWindow + k - mf);
    }
    float sum = 0.0f;
    for (int k = -analWinLen_; k <= analWinLen_; k++)
        sum += *(analWindow + k);
    sum = (float)(2.0 / sum);
    for (int k = -analWinLen_; k <= analWinLen_; k++)
        *(analWindow + k) *= sum;

    synWindow_.assign(static_cast<std::size_t>(m + mf), 0.0F);
    synWinLen_ = m / 2;
    auto* synWindow = synWindow_.data() + synWinLen_;
    if (m <= chans)
    {
        hamming(synWindow, synWinLen_, mf);
        for (int k = 1; k <= synWinLen_; k++)
            *(synWindow - k) = *(synWindow + k - mf);
        for (int k = -synWinLen_; k <= synWinLen_; k++)
            *(synWindow + k) *= sum;
        sum = 0.0f;
        // CDP8's build fuses this multiply and add, except in the loop it
        // vectorises for a step of one over four terms or more; the compiler
        // here might do either.
        for (int k = -synWinLen_; k <= synWinLen_; k += i)
        {
            if (i == 1 && synWinLen_ >= 2)
            {
                const float square = *(synWindow + k) * *(synWindow + k);
                sum += square;
            }
            else
                sum = std::fma(*(synWindow + k), *(synWindow + k), sum);
        }
        sum = (float)(1.0 / sum);
        for (int k = -synWinLen_; k <= synWinLen_; k++)
            *(synWindow + k) *= sum;
    }
    else
    {
        hamming(synWindow, synWinLen_, mf);
        for (int k = 1; k <= synWinLen_; k++)
            *(synWindow - k) = *(synWindow + k - mf);
        if (mf)
            *synWindow *= (float)((double)io * std::sin((double)(PI * .5 / io)) / (double)(PI * .5));
        for (int k = 1; k <= synWinLen_; k++)
            *(synWindow + k) *= (float)((double)io * std::sin((double)(PI * (k + .5 * mf) / io))
                / (double)(PI * (k + .5 * mf)));
        for (int k = 1; k <= synWinLen_; k++)
            *(synWindow - k) = *(synWindow + k - mf);
        sum = (float)(1.0 / sum);
        for (int k = -synWinLen_; k <= synWinLen_; k++)
            *(synWindow + k) *= sum;
    }

    const auto rIn = (float)(R / (float)d);
    const auto rOut = (float)(R / (float)i);
    roverTwoPi_ = (float)(rIn / TWOPI);
    twoPioverR_ = (float)(TWOPI / rOut);

    input_.assign(static_cast<std::size_t>(ibuflen_), 0.0F);
    output_.assign(static_cast<std::size_t>(obuflen_), 0.0F);
    anal_.assign(static_cast<std::size_t>(chans + 2), 0.0F);
    syn_.assign(static_cast<std::size_t>(chans + 2), 0.0F);
    oldInPhase_.assign(static_cast<std::size_t>(n2 + 1), 0.0F);
    oldOutPhase_.assign(static_cast<std::size_t>(n2 + 1), 0.0F);
    at_.assign(static_cast<std::size_t>(std::max(factors->maxf, 1)), 0.0F);
    ck_.assign(at_.size(), 0.0F);
    bt_.assign(at_.size(), 0.0F);
    sk_.assign(at_.size(), 0.0F);
    // fft_ allocates maxp, which CDP8 leaves at zero for some sizes (512
    // among them) and then writes past; the square factors' permutation
    // reaches 2 kt + 2.
    np_.assign(static_cast<std::size_t>(std::max(factors->maxp, 2 * factors->m + 2)) + 1, 0);

    latency_ = *latencyFor(points, overlap);
    pending_.assign(latency_ + static_cast<std::size_t>(i) + 1, 0.0F);
    reset();
    return true;
}

void PhaseVocoder::reset() noexcept
{
    std::ranges::fill(input_, 0.0F);
    std::ranges::fill(output_, 0.0F);
    std::ranges::fill(anal_, 0.0F);
    std::ranges::fill(syn_, 0.0F);
    std::ranges::fill(oldInPhase_, 0.0F);
    std::ranges::fill(oldOutPhase_, 0.0F);
    nI_ = -(analWinLen_ / std::max(d_, 1)) * d_;
    nO_ = nI_;
    dd_ = analWinLen_ + nI_ + 1;
    iOi_ = 0;
    nextIn_ = 0;
    nextOut_ = 0;
    read_ = 0;
    std::ranges::fill(pending_, 0.0F);
    pendingRead_ = 0;
    pendingCount_ = 0;
    frame_ = 0;
}

bool PhaseVocoder::consume(float sample) noexcept
{
    input_[static_cast<std::size_t>(nextIn_)] = sample;
    if (++nextIn_ >= ibuflen_)
        nextIn_ -= ibuflen_;
    if (++read_ < dd_)
        return false;
    read_ = 0;
    analyse();
    return true;
}

void PhaseVocoder::fft(float* a, float* b, int n, int isn) noexcept
{
    // fft_(a, b, 1, n, 1, isn).
    const auto nf = std::abs(n);
    if (nf == 1)
        return;
    auto factors = factor(nf);
    const auto nspn = std::abs(nf * 1);
    const auto ntot = std::abs(nspn * 1);
    fftmx(a, b, ntot, nf, nspn, isn, factors->m, &factors->kt, at_.data(), ck_.data(), bt_.data(),
        sk_.data(), np_.data(), factors->nfac);
}

void PhaseVocoder::analyse() noexcept
{
    float* anal = anal_.data();
    const float* analWindow = analWindow_.data() + analWinLen_;
    const float* input = input_.data();
    int i, j, k;
    for (i = 0; i < n_ + 2; i++)
        *(anal + i) = 0.0f;
    j = (nI_ - analWinLen_ - 1 + ibuflen_) % ibuflen_;
    k = nI_ - analWinLen_ - 1;
    while (k < 0)
        k += n_;
    k = k % n_;
    for (i = -analWinLen_; i <= analWinLen_; i++)
    {
        if (++j >= ibuflen_)
            j -= ibuflen_;
        if (++k >= n_)
            k -= n_;
        *(anal + k) += *(analWindow + i) * *(input + j);
    }
    fft(anal, anal + 1, n2_, -2);
    reals(anal, anal + 1, n2_, -2);

    float real, imag, phase, angleDif;
    double rratio;
    float* i0 = anal;
    float* i1 = anal + 1;
    float* oi = oldInPhase_.data();
    for (i = 0; i <= n2_; i++, i0 += 2, i1 += 2, oi++)
    {
        real = *i0;
        imag = *i1;
        *i0 = (float)std::sqrt((double)(real * real + imag * imag));
        // phase unwrapping
        if (*i0 == 0.)
            angleDif = 0.0f;
        else
        {
            rratio = std::atan((double)imag / (double)real);
            if (real < 0.0)
            {
                if (imag < 0.0)
                    rratio -= PI;
                else
                    rratio += PI;
            }
            angleDif = (phase = (float)rratio) - *oi;
            *oi = phase;
        }
        if (angleDif > PI)
            angleDif = (float)(angleDif - TWOPI);
        if (angleDif < -PI)
            angleDif = (float)(angleDif + TWOPI);
        // add in filter center freq.
        *i1 = angleDif * roverTwoPi_ + ((float)i * f_);
    }
}

void PhaseVocoder::synthesise() noexcept
{
    float* syn = syn_.data();
    float* output = output_.data();
    const float* synWindow = synWindow_.data() + synWinLen_;
    const auto no = n_;
    const auto no2 = n2_;
    int i, j, k;
    for (i = 0; i < no + 2; i++)
        *(syn + i) = anal_[static_cast<std::size_t>(i)];
    float mag, phase;
    float* i0 = syn;
    float* i1 = syn + 1;
    for (i = 0; i <= no2; i++, i0 += 2, i1 += 2)
    {
        mag = *i0;
        // A sample beyond float's range (finite as a double, infinite once
        // consumed) makes the frequencies of the frames it reaches NaN. CDP
        // adds them to the running phase, where they stay, and every later
        // frame of the channel is NaN; keep the phase instead, so only the
        // frames that sample reaches are lost.
        auto advanced = oldOutPhase_[static_cast<std::size_t>(i)];
        advanced += *i1 - ((float)i * f_);
        if (std::isfinite(advanced))
            oldOutPhase_[static_cast<std::size_t>(i)] = advanced;
        phase = oldOutPhase_[static_cast<std::size_t>(i)] * twoPioverR_;
        *i0 = (float)((double)mag * std::cos((double)phase));
        *i1 = (float)((double)mag * std::sin((double)phase));
    }
    reals(syn, syn + 1, no2, 2);
    fft(syn, syn + 1, no2, 2);

    j = nO_ - synWinLen_ - 1;
    while (j < 0)
        j += obuflen_;
    j = j % obuflen_;
    k = nO_ - synWinLen_ - 1;
    while (k < 0)
        k += no;
    k = k % no;
    for (i = -synWinLen_; i <= synWinLen_; i++)
    {
        if (++j >= obuflen_)
            j -= obuflen_;
        if (++k >= no)
            k -= no;
        *(output + j) += *(syn + k) * *(synWindow + i);
    }
    // Shift out the next IOi values, clipped as outfloats clips them.
    for (i = 0; i < iOi_; i++)
    {
        auto val = output[nextOut_];
        if (val >= 1.0 || val <= -1.0)
        {
            if (val > 0.0f)
                val = 1.0f;
            if (val < 0.0f)
                val = -1.0f;
        }
        pending_[(pendingRead_ + pendingCount_) % pending_.size()] = val;
        ++pendingCount_;
        output[nextOut_] = 0.0f;
        if (++nextOut_ >= obuflen_)
            nextOut_ -= obuflen_;
    }

    const auto D = d_;
    const auto I = d_;
    nI_ += D;
    nO_ += I;
    dd_ = D;
    int ii = 0;
    if (nO_ > (synWinLen_ + I))
        ii = I;
    else if (nO_ > synWinLen_)
        ii = nO_ - synWinLen_;
    else
    {
        ii = 0;
        for (i = nO_ + synWinLen_; i < obuflen_; i++)
        {
            if (i > 0)
                *(output + i) = 0.0f;
        }
    }
    iOi_ = ii;
}

double PhaseVocoder::emit() noexcept
{
    const auto frame = frame_++;
    if (frame < latency_ || pendingCount_ == 0)
        return 0.0;
    const auto value = pending_[pendingRead_];
    pendingRead_ = (pendingRead_ + 1) % pending_.size();
    --pendingCount_;
    return static_cast<double>(value);
}
}
