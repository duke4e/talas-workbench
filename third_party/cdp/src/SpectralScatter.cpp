/*
 * Derived from the CDP System, Release 8: dev/blur/blur.c and ap_blur.c,
 * dev/cdp2k/tklib1.c and tklib3.c, and dev/sfsys/osbind.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralScatter.h"

#include <cmath>
#include <utility>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h.
constexpr double VERY_TINY_VAL = 0.00000000000000000001;

// get_totalamp.
[[nodiscard]] double get_totalamp(std::span<const float> frame) noexcept
{
    double totalamp = 0.0;
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        totalamp += frame[vc];
    return totalamp;
}

// normalise, after a process that changes a frame's total amplitude.
void normalise(std::span<float> frame, double pre_totalamp, double post_totalamp) noexcept
{
    if (post_totalamp < VERY_TINY_VAL)
        return;
    const auto normaliser = pre_totalamp / post_totalamp;
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        frame[vc] = (float)(frame[vc] * normaliser);
}

// convert_params_for_scat and read_value_from_brktable: the block size in
// channels, rounded (CDP's round is lround).
[[nodiscard]] int bloksizeFor(double blockSize, double chwidth) noexcept
{
    return (int)std::lround(blockSize / chwidth);
}

// setup_internal_arrays_and_preset_internal_variables_for_scat and specscat:
// SCAT_BLOKS_PER_WINDOW.
[[nodiscard]] int bloksPerWindow(int clength, int bloksize) noexcept
{
    auto bpw = clength / bloksize;
    if ((bpw * bloksize) != clength)
        bpw++;
    return bpw;
}

[[nodiscard]] bool validControls(
    int clength, double nyquist, double chwidth, double keep, double blockSize) noexcept
{
    // tklib1.c's ranges: keep 1 to the channel count, the block size one
    // channel width to half the Nyquist frequency.
    if (!std::isfinite(keep) || !std::isfinite(blockSize) || keep < 1.0 || keep > (double)clength
        || blockSize < chwidth || blockSize > nyquist / 2.0)
    {
        return false;
    }
    // check_consistency_of_scat_params and specscat: keep below the blocks
    // per window.
    return (int)std::lround(keep) < bloksPerWindow(clength, bloksizeFor(blockSize, chwidth));
}
}

std::optional<double> BlurScatter::channelWidthFor(double sampleRate, int points) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || !PhaseVocoder::decimationFor(points, 1).has_value())
        return std::nullopt;
    // pvoc anal's points made even, and its channels.
    const auto clength = (points + (points % 2)) / 2 + 1;
    const auto nyquist = sampleRate / 2.0;
    return nyquist / (double)(clength - 1);
}

bool BlurScatter::valid(double keep, double blockSize) const noexcept
{
    return validControls(clength_, nyquist_, chwidth_, keep, blockSize);
}

bool BlurScatter::prepare(double sampleRate, int points, int overlap, double keep, double blockSize,
    bool randomiseCount, bool normalise)
{
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    const auto clength = static_cast<int>(vocoder.frameSize() / 2);
    const auto nyquist = sampleRate / 2.0;
    const auto chwidth = nyquist / (double)(clength - 1);
    if (!validControls(clength, nyquist, chwidth, keep, blockSize))
        return false;
    vocoder_ = std::move(vocoder);
    clength_ = clength;
    nyquist_ = nyquist;
    chwidth_ = chwidth;
    startKeep_ = (int)std::lround(keep);
    startBloksize_ = bloksizeFor(blockSize, chwidth);
    randomiseCount_ = randomiseCount;
    normalise_ = normalise;
    // setup_internal_arrays_and_preset_internal_variables_for_scat.
    keepTable_.assign(static_cast<std::size_t>(clength + 1), 0);
    reset();
    return true;
}

bool BlurScatter::setControls(double keep, double blockSize) noexcept
{
    if (!prepared() || !valid(keep, blockSize))
        return false;
    keep_ = (int)std::lround(keep);
    bloksize_ = bloksizeFor(blockSize, chwidth_);
    return true;
}

void BlurScatter::reset() noexcept
{
    vocoder_.reset();
    random_ = Drand48{};
    keep_ = startKeep_;
    bloksize_ = startBloksize_;
}

bool BlurScatter::process(std::span<const double> input, std::span<double> output) noexcept
{
    // skip_or_special_operation_on_window_zero leaves no frame out.
    return vocoder_.process(input, output, [this](std::span<float> frame) { scatter(frame); });
}

// specscat.
void BlurScatter::scatter(std::span<float> frame) noexcept
{
    const auto wanted = static_cast<int>(frame.size());
    const auto clength = clength_;
    float* flbufptr = frame.data();
    int* keep = keepTable_.data();
    const auto cnt = keep_;
    const auto bloksize = bloksize_;
    const auto bpw = bloksPerWindow(clength, bloksize);
    auto thiscnt = cnt;
    double pre_totalamp = 0.0;
    if (normalise_)
        pre_totalamp = get_totalamp(frame);
    // CDP8's build fuses the multiply and add.
    if (randomiseCount_)
        thiscnt = (int)std::fma(random_.next(), (double)cnt, 1.0); /* TRUNCATE */

    // randomly_select_blokstokeep_and_sort_in_ascending_order, with insert
    // and shuflup: thiscnt different blocks, drawn again on a repeat, in
    // ascending order.
    int n, m, k, keepcnt, bad_value, inserted, z;
    for (n = 0; n < clength + 1; n++)
        keep[n] = clength + 1;
    k = (int)(random_.next() * bpw); /* TRUNC */
    keep[0] = k;
    keepcnt = 1;
    for (n = 1; n < thiscnt; n++)
    {
        k = (int)(random_.next() * bpw); /* TRUNC */
        bad_value = 0;
        inserted = 0;
        for (m = 0; m < keepcnt; m++)
        {
            if (k == keep[m])
            {
                bad_value = 1;
                break;
            }
        }
        if (bad_value)
        {
            n--;
            continue;
        }
        for (m = 0; m < keepcnt; m++)
        {
            if (k < keep[m])
            {
                for (z = keepcnt; z > m; z--)
                    keep[z] = keep[z - 1];
                keep[m] = k;
                inserted = 1;
                break;
            }
        }
        if (!inserted)
            keep[keepcnt] = k;
        keepcnt++;
    }

    // delete_unwanted_bloks: cc moves on only past a kept block, so a
    // dropped one silences the place after the kept blocks so far.
    int this_blok = 0;
    int c_end, vc_end;
    int cc = 0, vc;
    for (n = 0; n < bpw; n++)
    {
        if (n == keep[this_blok])
        {
            if ((cc += bloksize) >= clength)
                break;
            if (++this_blok >= thiscnt)
            {
                vc = cc * 2;
                while (vc < wanted)
                {
                    flbufptr[vc] = 0.0F;
                    vc += 2;
                }
                break;
            }
        }
        else
        {
            if ((c_end = cc + bloksize) >= clength)
                c_end = clength;
            vc = cc * 2;
            vc_end = c_end * 2;
            while (vc < vc_end)
            {
                flbufptr[vc] = 0.0F;
                vc += 2;
            }
            if (vc >= wanted)
                break;
        }
    }
    if (normalise_)
        normalise(frame, pre_totalamp, get_totalamp(frame));
}
}
