/*
 * Derived from the CDP System, Release 8: dev/hilite/hilite.c and
 * ap_hilite.c, and dev/cdp2k/tklib1.c, tklib3.c and special.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralBands.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h.
constexpr double PI = 3.141592654;
constexpr double ONE_OVER_LN2 = 1.442695;
constexpr double FLTERR = 0.000002;
constexpr double OCTAVES_PER_SEMITONE = 0.08333333333;
// dev/include/speccon.h.
constexpr double SPEC_MINFRQ = 10.0;
constexpr int DO_AMPLITUDE_CHANGE = 3;
constexpr int DO_RAMPED_AMPLITUDE = 2;
constexpr int DO_TRANSPOSITION = 4;
constexpr int DO_ADD_TO_SPECTRUM = 8;

[[nodiscard]] double LOG2(double x) noexcept
{
    return (std::log(x) * ONE_OVER_LN2);
}

// tklib1.c's flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const double upperbnd = f2 + FLTERR;
    const double lowerbnd = f2 - FLTERR;
    return !((f1 > upperbnd) || (f1 < lowerbnd));
}

// isspace and isdigit in the C locale.
[[nodiscard]] bool space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] bool digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

// The pieces fgets(temp,200,fp) reads: up to 199 characters, through a
// newline, each a C string ending at its first NUL.
template <class Visit> void forEachPiece(std::string_view text, Visit&& visit)
{
    std::size_t begin = 0;
    while (begin < text.size())
    {
        std::size_t end = begin;
        while (end < text.size() && end - begin < 199)
        {
            if (text[end++] == '\n')
                break;
        }
        const auto piece = text.substr(begin, end - begin);
        if (!visit(piece.substr(0, piece.find('\0'))))
            return;
        begin = end;
    }
}

// tklib1.c's get_float_from_within_string: a word of a '-' or '.' or digit,
// then digits and at most one '.' in all, with a digit, read as sscanf's
// "%lf" reads it. Nothing, and no move, for any other word or none.
[[nodiscard]] std::optional<double> floatWithin(std::string_view str, std::size_t& at) noexcept
{
    std::size_t p = at;
    int decimal_point_cnt = 0;
    bool has_digits = false;
    while (p < str.size() && space(str[p]))
        p++;
    const auto valstart = p;
    if (p == str.size())
        return std::nullopt;
    switch (str[p])
    {
    case '-':
        break;
    case '.':
        decimal_point_cnt = 1;
        break;
    default:
        if (!digit(str[p]))
            return std::nullopt;
        has_digits = true;
        break;
    }
    p++;
    while (p < str.size() && !space(str[p]))
    {
        if (digit(str[p]))
            has_digits = true;
        else if (str[p] == '.')
        {
            if (++decimal_point_cnt > 1)
                return std::nullopt;
        }
        else
            return std::nullopt;
        p++;
    }
    if (!has_digits)
        return std::nullopt;
    // Under 200 characters a number is finite and normal.
    double val = 0.0;
    const auto* first = str.data() + valstart;
    const auto* last = str.data() + p;
    if (const auto [end, error] = std::from_chars(first, last, val); error != std::errc{} || end != last)
        return std::nullopt;
    at = p;
    return val;
}

// tklib1.c's get_word_from_string, which ends the word in place and steps
// past the character it overwrites.
[[nodiscard]] std::optional<std::string_view> wordWithin(std::string_view str, std::size_t& at) noexcept
{
    while (at < str.size() && space(str[at]))
        at++;
    if (at == str.size())
        return std::nullopt;
    const auto start = at;
    while (at < str.size() && !space(str[at]))
        at++;
    const auto word = str.substr(start, at - start);
    if (at < str.size())
        at++;
    return word;
}
}

// setup_special_data_ranges: FILTER_FRQS and FILTER_BWS_AND_FRQS.
double HiliteGraphicEq::maximumFrequency(double sampleRate) noexcept
{
    const double nyquist = sampleRate / 2.0;
    return (nyquist * 2.0) / 3.0;
}

double HiliteGraphicEq::maximumBandwidth(double sampleRate) noexcept
{
    const double nyquist = sampleRate / 2.0;
    return LOG2(nyquist - SPEC_MINFRQ);
}

// get_filt_params.
std::optional<std::vector<HiliteGraphicEq::Band>> readHiliteGraphicEq(
    std::string_view text, HiliteGraphicEq::Mode mode)
{
    if (mode != HiliteGraphicEq::Mode::OneBandwidth && mode != HiliteGraphicEq::Mode::Bandwidths)
        return std::nullopt;
    bool gotpair = true, firstime = true;
    double bwidth = 0.0, fpeak = 0.0;
    std::vector<HiliteGraphicEq::Band> bands;
    forEachPiece(text, [&](std::string_view temp) {
        std::size_t p = 0;
        while (const auto val = floatWithin(temp, p))
        {
            if (mode == HiliteGraphicEq::Mode::OneBandwidth)
            {
                if (firstime)
                {
                    bwidth = *val;
                    gotpair = false;
                    firstime = false;
                }
                else
                {
                    fpeak = *val;
                    gotpair = true;
                }
            }
            else
            {
                if (gotpair)
                    fpeak = *val;
                else
                    bwidth = *val;
                gotpair = !gotpair;
            }
            if (gotpair)
                bands.push_back({fpeak, bwidth});
        }
        return true;
    });
    if (!gotpair || bands.empty())
        return std::nullopt;
    return bands;
}

bool HiliteGraphicEq::prepare(
    double sampleRate, int points, int overlap, std::span<const Band> bands, bool notch)
{
    if (bands.empty())
        return false;
    // check_filtfrq and check_filtbw.
    for (const auto& band : bands)
    {
        if (!std::isfinite(band.frequency) || !std::isfinite(band.bandwidth)
            || band.frequency < SPEC_MINFRQ || band.frequency > maximumFrequency(sampleRate)
            || band.bandwidth < OCTAVES_PER_SEMITONE || band.bandwidth > maximumBandwidth(sampleRate))
        {
            return false;
        }
    }
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    const double nyquist = sampleRate / 2.0;
    const int itemcnt = static_cast<int>(bands.size());
    std::vector<double> filtpeak(bands.size());
    std::vector<double> filtwidth(bands.size());
    for (int n = 0; n < itemcnt; n++)
    {
        filtpeak[n] = bands[n].frequency;
        filtwidth[n] = bands[n].bandwidth;
    }
    // sort_peaks_by_frq, which puts the lower peak in a bandwidth's place,
    // so that out of order the peaks repeat and bandwidths take
    // frequencies.
    {
        int m, n;
        double k;
        for (n = 0; n < itemcnt - 1; n++)
        {
            for (m = n + 1; m < itemcnt; m++)
            {
                if (filtpeak[n] > filtpeak[m])
                {
                    k = filtpeak[m];
                    filtpeak[m] = filtpeak[n];
                    filtwidth[n] = k;
                    k = filtwidth[m];
                    filtwidth[m] = filtwidth[n];
                    filtwidth[n] = k;
                }
            }
        }
    }
    // generate_filter_params: the last band's scale serves them all. CDP's
    // build calls exp2 for its pow(2.0,halfbwidth).
    std::vector<double> fbandbot(bands.size());
    std::vector<double> fbandtop(bands.size());
    double scalefact = 0.0;
    {
        int n;
        double bratio_up, bratio_dn, halfbwidth;
        for (n = 0; n < itemcnt; n++)
        {
            halfbwidth = filtwidth[n] / 2.0;
            scalefact = PI / halfbwidth;
            bratio_up = std::exp2(halfbwidth);
            bratio_dn = 1.0 / bratio_up;
            fbandbot[n] = filtpeak[n] * bratio_dn;
            fbandtop[n] = std::min(filtpeak[n] * bratio_up, nyquist);
        }
    }
    vocoder_ = std::move(vocoder);
    notch_ = notch;
    scalefact_ = scalefact;
    filtpeak_ = std::move(filtpeak);
    fbandbot_ = std::move(fbandbot);
    fbandtop_ = std::move(fbandtop);
    fsampbuf_.assign(vocoder_.frameSize() / 2, 0.0F);
    reset();
    return true;
}

void HiliteGraphicEq::reset() noexcept
{
    vocoder_.reset();
    frames_ = 0;
}

bool HiliteGraphicEq::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        // skip_or_special_operation_on_window_zero leaves the first frame.
        if (frames_++ == 0)
            return;
        filter(frame);
    });
}

// specgreq: construct_filter_envelope, invert_greq_filter_envelope and
// do_greq_filter.
void HiliteGraphicEq::filter(std::span<float> frame) noexcept
{
    constexpr int OVERSHOOT = 8;
    const int clength = static_cast<int>(fsampbuf_.size());
    const int pkcnt_here = static_cast<int>(filtpeak_.size());
    const float* fbuf = frame.data();
    float* fsampbuf = fsampbuf_.data();
    double thisfrq, thisamp, filt_centre_frq;
    int n, m;
    int baktrak = -1, activate_baktrak = 0, checklogic = 0;
    int beyond_top, band_incremented;
    int cc, vc;
    for (cc = 0; cc < clength; cc++)
        fsampbuf[cc] = 0.0f;
    n = 0;
    m = 1;
    cc = 0;
    vc = 0;
    beyond_top = 0;
    // gen_amplitude_in_lo_half_filterband and
    // gen_amplitude_in_hi_half_filterband.
    const auto halfband = [this](double octaves) {
        double val = octaves;
        val *= scalefact_;
        val = std::cos(val);
        val += 1.0;
        val /= 2.0;
        return val;
    };
    while (cc < clength)
    {
        filt_centre_frq = filtpeak_[n];
        band_incremented = 0;
        thisfrq = std::fabs((double)fbuf[vc + 1]);
        if (baktrak < 0 && (m < pkcnt_here) && (thisfrq > fbandbot_[m]))
            baktrak = cc;
        if (thisfrq < fbandbot_[n])
        {
            if (beyond_top)
                beyond_top++;
        }
        else if (thisfrq < filt_centre_frq)
        {
            thisamp = halfband(-LOG2(thisfrq / filt_centre_frq));
            fsampbuf[cc] = (float)std::max((double)fsampbuf[cc], thisamp);
            if (beyond_top)
                beyond_top++;
        }
        else if (thisfrq < fbandtop_[n])
        {
            thisamp = halfband(LOG2(thisfrq / filt_centre_frq));
            fsampbuf[cc] = (float)std::max((double)fsampbuf[cc], thisamp);
            if (beyond_top)
                beyond_top++;
        }
        else
        {
            if (!beyond_top)
            {
                if (baktrak < 0)
                    baktrak = cc;
            }
            if (++beyond_top > OVERSHOOT)
            {
                beyond_top = 0;
                if (++n >= pkcnt_here)
                    break;
                m++;
                band_incremented = 1;
                activate_baktrak = 1;
            }
        }
        if (cc == clength - 1)
        {
            checklogic = 1;
            if (!band_incremented)
            {
                beyond_top = 0;
                if (++n >= pkcnt_here)
                    break;
                m++;
                activate_baktrak = 1;
            }
        }
        if (activate_baktrak && baktrak >= 0)
        {
            if (checklogic)
                checklogic++;
            cc = baktrak;
            vc = cc * 2;
            baktrak = -1;
            activate_baktrak = 0;
            continue;
        }
        // CDP stops with "Problem with peak-scanning logic" here; the stream
        // keeps the contour so far.
        if (checklogic == 1)
            break;
        checklogic = 0;
        cc++;
        vc += 2;
    }
    if (notch_)
    {
        for (cc = 0; cc < clength; cc++)
            fsampbuf[cc] = (float)(1.0 - fsampbuf[cc]);
    }
    for (cc = 0, vc = 0; cc < clength; cc++, vc += 2)
        frame[vc] = (float)(frame[vc] * fsampbuf[cc]);
}

// read_specsplit_data.
std::optional<std::vector<HiliteBand::Band>> readHiliteBand(std::string_view text)
{
    std::vector<HiliteBand::Band> bands;
    int flags_set = 0;
    bool ok = true;
    forEachPiece(text, [&](std::string_view temp) {
        std::size_t p = 0;
        while (p < temp.size() && space(temp[p]))
            p++;
        if (p == temp.size())
            return true;
        ok = false;
        HiliteBand::Band bb;
        const auto bfrqlo = floatWithin(temp, p);
        if (!bfrqlo || *bfrqlo < 0.0)
            return false;
        const auto bfrqhi = floatWithin(temp, p);
        if (!bfrqhi || *bfrqhi < 0.0)
            return false;
        bb.lowFrequency = *bfrqlo;
        bb.highFrequency = *bfrqhi;
        const auto q = wordWithin(temp, p);
        if (!q)
            return false;
        for (std::size_t n = 0; n < 4; n++)
        {
            const char flag = n < q->size() ? (*q)[n] : '\0';
            if (flag < '0' || flag > '1')
                return false;
            if (flag == '1')
            {
                flags_set++;
                switch (n)
                {
                case 0:
                {
                    const auto bamp = floatWithin(temp, p);
                    if (!bamp)
                        return false;
                    bb.changeAmplitude = true;
                    bb.amplitude = *bamp;
                    break;
                }
                case 1:
                {
                    // CDP's check for amp1 tests the bit just set.
                    const auto otheramp = floatWithin(temp, p);
                    if (!otheramp)
                        return false;
                    bb.rampAmplitude = true;
                    bb.endAmplitude = *otheramp;
                    break;
                }
                case 2:
                {
                    while (p < temp.size() && space(temp[p]))
                        p++;
                    if (p < temp.size() && temp[p] == '+')
                    {
                        bb.additive = true;
                        p++;
                    }
                    const auto btrans = floatWithin(temp, p);
                    if (!btrans)
                        return false;
                    bb.transpose = true;
                    bb.transposition = *btrans;
                    break;
                }
                default:
                    if (!bb.transpose)
                        return false;
                    bb.addToSpectrum = true;
                    break;
                }
            }
        }
        if (flags_set == 0)
            return false;
        bands.push_back(bb);
        ok = true;
        return true;
    });
    if (!ok)
        return std::nullopt;
    return bands;
}

bool HiliteBand::prepare(double sampleRate, int points, int overlap, std::span<const Band> bands)
{
    const double nyquist = sampleRate / 2.0;
    std::vector<Bandd> band;
    // read_specsplit_data's checks.
    for (std::size_t index = 0; index < bands.size(); index++)
    {
        const auto& line = bands[index];
        if (!std::isfinite(line.lowFrequency) || !std::isfinite(line.highFrequency)
            || !std::isfinite(line.amplitude) || !std::isfinite(line.endAmplitude)
            || !std::isfinite(line.transposition))
        {
            return false;
        }
        Bandd bb;
        bb.bfrqlo = line.lowFrequency;
        if (bb.bfrqlo < 0.0 || bb.bfrqlo > nyquist)
            return false;
        bb.bfrqhi = line.highFrequency;
        if (bb.bfrqhi < 0.0)
            return false;
        if (bb.bfrqhi > nyquist)
            bb.bfrqhi = nyquist;
        if (flteq(bb.bfrqlo, bb.bfrqhi))
            return false;
        if ((bb.bfrqdif = bb.bfrqhi - bb.bfrqlo) < 0.0)
        {
            const double d1 = bb.bfrqlo;
            bb.bfrqlo = bb.bfrqhi;
            bb.bfrqhi = d1;
            bb.bfrqdif = -(bb.bfrqdif);
        }
        if (line.addToSpectrum && !line.transpose)
            return false;
        bb.bdoflag = (line.changeAmplitude ? 1 : 0) | (line.rampAmplitude ? 2 : 0)
            | (line.transpose ? DO_TRANSPOSITION : 0) | (line.addToSpectrum ? DO_ADD_TO_SPECTRUM : 0);
        if (index == 0 && bb.bdoflag == 0)
            return false;
        // An unset amp1 reads as zero.
        if (line.changeAmplitude)
            bb.bamp = line.amplitude;
        if (line.rampAmplitude)
            bb.bampdif = line.endAmplitude - bb.bamp;
        if (line.transpose)
        {
            bb.badditive = line.additive ? 1 : 0;
            bb.btrans = line.transposition;
            // get_channel_corresponding_to_frq stops CDP at a frequency
            // below zero, as a negative ratio gives at once.
            if (!line.additive && bb.btrans < 0.0)
                return false;
        }
        band.push_back(bb);
    }
    // sort_frqbands_into_ascending_order, which keeps comparing with the
    // first lower frequency after a swap.
    {
        const int itemcnt = static_cast<int>(band.size());
        double f1, f2;
        int penult = itemcnt - 1, n, m;
        for (n = 0; n < penult; n++)
        {
            f1 = band[n].bfrqlo;
            for (m = n + 1; m < itemcnt; m++)
            {
                if ((f2 = band[m].bfrqlo) < f1)
                    std::swap(band[n], band[m]);
            }
        }
        for (n = 0; n < penult; n++)
        {
            if (band[n].bfrqhi > band[n + 1].bfrqlo)
                return false;
        }
    }
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    const auto clength = static_cast<int>(vocoder_.frameSize() / 2);
    nyquist_ = nyquist;
    chwidth_ = nyquist_ / (double)(clength - 1);
    halfchwidth_ = chwidth_ / 2.0;
    bands_ = std::move(band);
    amp_.assign(static_cast<std::size_t>(clength), 0.0F);
    freq_.assign(static_cast<std::size_t>(clength), 0.0F);
    reset();
    return true;
}

void HiliteBand::reset() noexcept
{
    vocoder_.reset();
}

bool HiliteBand::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) { split(frame); });
}

// specsplit, with rectify_window, get_amp_and_frq and put_amp_and_frq.
void HiliteBand::split(std::span<float> frame) noexcept
{
    const int clength = static_cast<int>(amp_.size());
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
    {
        if (frame[vc + 1] < 0.0)
            frame[vc + 1] = -frame[vc + 1];
    }
    for (int cc = 0; cc < clength; cc++)
    {
        amp_[cc] = frame[static_cast<std::size_t>(cc) * 2];
        freq_[cc] = frame[static_cast<std::size_t>(cc) * 2 + 1];
    }
    const int itemcnt = static_cast<int>(bands_.size());
    int cc, bno = 0, bottom, top, done = 0, k;
    double bandbot, bandtop;
    cc = 0;
    while (bno < itemcnt)
    {
        const Bandd& bb = bands_[bno];
        bandbot = bb.bfrqlo;
        bandtop = bb.bfrqhi;
        while (freq_[cc] < bandbot)
        {
            if (++cc >= clength)
            {
                done = 1;
                break;
            }
        }
        if (done)
            break;
        bottom = cc;
        while (freq_[cc] < bandtop)
        {
            if (++cc >= clength)
            {
                done = 1;
                break;
            }
        }
        top = std::min(cc, clength - 1);
        if ((bb.bdoflag & DO_TRANSPOSITION) && bb.btrans > 1.0)
        {
            for (k = top; k >= bottom; k--)
                channel(k, bb);
        }
        else
        {
            for (k = bottom; k <= top; k++)
                channel(k, bb);
        }
        if (done)
            break;
        bno++;
    }
    for (cc = 0; cc < clength; cc++)
    {
        frame[static_cast<std::size_t>(cc) * 2] = amp_[cc];
        frame[static_cast<std::size_t>(cc) * 2 + 1] = freq_[cc];
    }
}

// process_specsplit_channel, with get_channel_corresponding_to_frq.
void HiliteBand::channel(int cc, const Bandd& bb) noexcept
{
    const int clength = static_cast<int>(amp_.size());
    double thismult;
    double newamp = amp_[cc];
    double newfrq = freq_[cc];
    int newchan;
    if (bb.bdoflag & DO_AMPLITUDE_CHANGE)
    {
        if (bb.bdoflag & DO_RAMPED_AMPLITUDE)
        {
            thismult = (freq_[cc] - bb.bfrqlo) / (bb.bfrqdif);
            thismult *= bb.bampdif;
            thismult += bb.bamp;
            newamp = amp_[cc] * thismult;
        }
        else
            newamp = amp_[cc] * bb.bamp;
    }
    if (bb.bdoflag & DO_TRANSPOSITION)
    {
        if (bb.badditive)
            newfrq = freq_[cc] + bb.btrans;
        else
            newfrq = freq_[cc] * bb.btrans;
        // CDP stops with "-ve frequency" below zero; the stream leaves the
        // partial where it is, as at the Nyquist or above.
        if (newfrq < nyquist_ && !(newfrq < 0.0))
        {
            newchan = (int)((std::fabs(newfrq) + halfchwidth_) / chwidth_);
            if (newchan != cc)
            {
                freq_[newchan] = (float)newfrq;
                amp_[newchan] = (float)newamp;
            }
        }
        if (!(bb.bdoflag & DO_ADD_TO_SPECTRUM))
            newamp = (amp_[std::max(cc - 1, 0)] + amp_[std::min(cc + 1, (clength - 1))]) / 2.0;
    }
    freq_[cc] = (float)newfrq;
    amp_[cc] = (float)newamp;
}
}
