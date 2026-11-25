/*
 * Derived from the CDP System, Release 8: dev/cdp2k/formantsg.c, tklib1.c,
 * mainfuncs.c and dzsetup.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralEnvelope.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h and formants.h.
constexpr int LOW_OCTAVE_BANDS = 4;
constexpr int TOP_OF_LOW_OCTAVE_BANDS = 4;
constexpr int CHAN_ABOVE_LOW_OCTAVES = 8;
// dev/cdp2k/formantsg.c.
constexpr int CHAN_SRCHRANGE_F = 4;

struct Points
{
    std::vector<float> frq;
    std::vector<float> pch;
    std::vector<float> top;
    std::vector<float> amp;
};

// set_specenv_frqs, with setup_octaveband_steps and setup_low_octave_bands,
// into initialise_specenv's arrays of clength + 1; nothing where CDP finds
// them too small, or where the bands fill them and CDP writes its Nyquist
// point past them.
[[nodiscard]] std::optional<Points> setSpecenvFrqs(
    double nyquist, double chwidth, double halfchwidth, int clength, bool pitchwise, int formant_bands)
{
    const int arraycnt = clength + 1;
    Points points;
    for (auto* array : {&points.frq, &points.pch, &points.top, &points.amp})
        array->assign(static_cast<std::size_t>(arraycnt), 0.0F);
    float* specenvfrq = points.frq.data();
    float* specenvpch = points.pch.data();
    float* specenvtop = points.top.data();
    float* specenvamp = points.amp.data();
    int m, n, k = 0, cc;
    if (!pitchwise)
    {
        // FREQWISE_FORMANTS.
        double frqstep, thisfrq, nextfrq;
        specenvamp[0] = (float)0.0;
        specenvfrq[0] = (float)1.0;
        specenvpch[0] = (float)std::log10(specenvfrq[0]);
        specenvtop[0] = (float)halfchwidth;
        frqstep = halfchwidth * (double)formant_bands;
        thisfrq = specenvtop[0];
        k = 1;
        while ((nextfrq = thisfrq + frqstep) < nyquist)
        {
            if (k >= arraycnt)
                return std::nullopt;
            specenvfrq[k] = (float)nextfrq;
            specenvpch[k] = (float)std::log10(specenvfrq[k]);
            nextfrq += frqstep;
            specenvtop[k] = (float)std::min(nyquist, nextfrq);
            thisfrq = nextfrq;
            k++;
        }
    }
    else
    {
        // setup_octaveband_steps: CDP8's build calls exp2 for pow(2.0,...).
        // With no bands CDP still writes the top step.
        const int halfbands_per_octave = formant_bands * 2;
        std::vector<double> interval(static_cast<std::size_t>(std::max(halfbands_per_octave, 1)));
        n = 1;
        m = 0;
        while (n < halfbands_per_octave)
        {
            const double octave_step = (double)n++ / (double)halfbands_per_octave;
            interval[static_cast<std::size_t>(m++)] = std::exp2(octave_step);
        }
        interval[static_cast<std::size_t>(m)] = 2.0;
        // setup_low_octave_bands: the lowest channels in octave bands.
        if (arraycnt < LOW_OCTAVE_BANDS)
            return std::nullopt;
        specenvfrq[0] = (float)1.0;
        specenvpch[0] = (float)0.0;
        specenvtop[0] = (float)chwidth;
        specenvfrq[1] = (float)(chwidth * 1.5);
        specenvpch[1] = (float)std::log10(specenvfrq[1]);
        specenvtop[1] = (float)(chwidth * 2.0);
        specenvfrq[2] = (float)(chwidth * 3.0);
        specenvpch[2] = (float)std::log10(specenvfrq[2]);
        specenvtop[2] = (float)(chwidth * 4.0);
        specenvfrq[3] = (float)(chwidth * 6.0);
        specenvpch[3] = (float)std::log10(specenvfrq[3]);
        specenvtop[3] = (float)(chwidth * 8.0);
        k = TOP_OF_LOW_OCTAVE_BANDS;
        cc = CHAN_ABOVE_LOW_OCTAVES;
        while (cc <= clength)
        {
            m = 0;
            double bandbot;
            if ((bandbot = chwidth * (double)cc) >= nyquist)
                break;
            for (n = 0; n < formant_bands; n++)
            {
                if (k >= arraycnt)
                    return std::nullopt;
                specenvfrq[k] = (float)(bandbot * interval[static_cast<std::size_t>(m++)]);
                specenvpch[k] = (float)std::log10(specenvfrq[k]);
                specenvtop[k++] = (float)(bandbot * interval[static_cast<std::size_t>(m++)]);
            }
            cc *= 2; /* 8-16: 16-32: 32-64 etc as 8vas, then split into bands */
        }
    }
    if (k >= arraycnt)
        return std::nullopt;
    specenvfrq[k] = (float)nyquist;
    specenvpch[k] = (float)std::log10(nyquist);
    specenvtop[k] = (float)nyquist;
    specenvamp[k] = (float)0.0;
    k++;
    for (auto* array : {&points.frq, &points.pch, &points.top, &points.amp})
        array->resize(static_cast<std::size_t>(k));
    return points;
}
}

std::optional<SpectralEnvelope::BandRange> SpectralEnvelope::bandRange(std::size_t frameSize, bool pitchwise) noexcept
{
    if (frameSize % 2 != 0 || frameSize < 4 || frameSize > (std::size_t{1} << 24))
        return std::nullopt;
    // establish_formant_band_ranges.
    const int clength = static_cast<int>(frameSize / 2);
    const int clength_less_one = clength - 1;
    BandRange range;
    if (!pitchwise)
    {
        // No bands would never reach the Nyquist frequency.
        range.minimum = 1;
        range.maximum = clength_less_one / 2;
    }
    else if (clength_less_one < LOW_OCTAVE_BANDS)
        return std::nullopt;
    else
    {
        // set_specenv_frqs fills octaves from channel 8 while their bottom
        // (chwidth * cc) is under the Nyquist frequency, i.e. cc under
        // clength - 1, after the four low bands, into arrays of clength + 1
        // that must keep the Nyquist point.
        int octaves = 0;
        for (int cc = CHAN_ABOVE_LOW_OCTAVES; cc <= clength && cc < clength_less_one; cc *= 2)
            octaves++;
        range.minimum = 0;
        range.maximum = maximumPitchwiseBands;
        while (range.maximum >= range.minimum
            && TOP_OF_LOW_OCTAVE_BANDS + range.maximum * octaves + 1 > clength + 1)
        {
            range.maximum--;
        }
    }
    if (range.maximum < range.minimum)
        return std::nullopt;
    return range;
}

bool SpectralEnvelope::prepare(double sampleRate, std::size_t frameSize, bool pitchwise, int bands, bool quickSearch)
{
    const auto range = bandRange(frameSize, pitchwise);
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || !range.has_value() || bands < range->minimum
        || bands > range->maximum)
    {
        return false;
    }
    // set_chunklens_and_establish_windowbufs.
    const int clength = static_cast<int>(frameSize / 2);
    const double nyquist = sampleRate / 2.0;
    const double chwidth = nyquist / (double)(clength - 1);
    const double halfchwidth = chwidth / 2.0;
    auto points = setSpecenvFrqs(nyquist, chwidth, halfchwidth, clength, pitchwise, bands);
    if (!points.has_value())
        return false;
    clength_ = clength;
    // read_formant_qksrch_flag: -i sets IGNORE_ACTUAL_CHAN_FRQS over
    // dzsetup.c's RECTIFY_CHANNEL_FRQ_ORDER.
    quickSearch_ = quickSearch;
    nyquist_ = nyquist;
    chwidth_ = chwidth;
    halfchwidth_ = halfchwidth;
    frq_ = std::move(points->frq);
    pch_ = std::move(points->pch);
    top_ = std::move(points->top);
    amp_ = std::move(points->amp);
    return true;
}

// extract_specenv, into store 0.
void SpectralEnvelope::extract(std::span<const float> frame) noexcept
{
    if (!prepared() || frame.size() != static_cast<std::size_t>(clength_) * 2)
        return;
    const float* flbufptr = frame.data();
    float* ampstore = amp_.data();
    const float* specenvtop = top_.data();
    const int specenvcnt = static_cast<int>(amp_.size());
    const int specenvcnt_less_one = specenvcnt - 1;
    int n, cc, vc;
    int botchan, topchan;
    double botfreq, topfreq;
    double bwidth_in_chans;
    if (!quickSearch_)
    {
        // RECTIFY_CHANNEL_FRQ_ORDER: each band's channels by frequency,
        // searched to CHAN_SRCHRANGE_F channels beyond its edges.
        for (n = 0; n < specenvcnt; n++)
            ampstore[n] = (float)0.0;
        topfreq = 0.0F;
        n = 0;
        while (n < specenvcnt_less_one)
        {
            botfreq = topfreq;
            botchan = (int)((botfreq + halfchwidth_) / chwidth_); /* TRUNCATE */
            botchan -= CHAN_SRCHRANGE_F;
            botchan = std::max(botchan, 0);
            topfreq = specenvtop[n];
            topchan = (int)((topfreq + halfchwidth_) / chwidth_); /* TRUNCATE */
            topchan += CHAN_SRCHRANGE_F;
            topchan = std::min(topchan, clength_);
            for (cc = botchan, vc = botchan * 2; cc < topchan; cc++, vc += 2)
            {
                if (flbufptr[vc + 1] >= botfreq && flbufptr[vc + 1] < topfreq)
                    ampstore[n] = (float)(ampstore[n] + flbufptr[vc]);
            }
            bwidth_in_chans = (double)(topfreq - botfreq) / chwidth_;
            ampstore[n] = (float)(ampstore[n] / bwidth_in_chans);
            n++;
        }
        return;
    }
    // IGNORE_ACTUAL_CHAN_FRQS: channels by position. Each band boundary
    // passes one channel width without taking a channel, so later channels
    // meet later bands, and the band the channels run out in keeps its sum.
    for (n = 1; n < specenvcnt_less_one; n++)
        ampstore[n] = (float)0.0;
    ampstore[0] = (float)(flbufptr[0] / 2.0); /* dz->halfchwidth WIDE CHANNEL */
    cc = 1;
    vc = 2;
    n = 1; /* OMIT LOWEST CHANNEL */
    topfreq = halfchwidth_;
    while (cc < clength_ && n < specenvcnt_less_one)
    {
        botfreq = topfreq;
        topfreq += chwidth_;
        if (topfreq < specenvtop[n])
        {
            if (botfreq >= specenvtop[n - 1]) /* WHOLLY IN SPECENVBAND  */
                ampstore[n] = (float)(ampstore[n] + flbufptr[vc]);
            else /* PARTLY IN SPECENVBAND  */
                ampstore[n] = (float)(ampstore[n] + (flbufptr[vc] / 2.0));
            cc++;
            vc += 2;
        }
        else if (botfreq < specenvtop[n])
        { /* PARTLY IN SPECENVBAND  */
            ampstore[n] = (float)(ampstore[n] + (flbufptr[vc] / 2.0));
            cc++;
            vc += 2;
        }
        else
        { /* NOT IN SPECENVBAND     */
            bwidth_in_chans = (specenvtop[n] - specenvtop[n - 1]) / chwidth_;
            ampstore[n] = (float)(ampstore[n] / bwidth_in_chans);
            n++; /* GO TO NEXT SPECENVBAND */
        }
    }
}

// getspecenvamp, from store 0.
double SpectralEnvelope::amplitudeAt(double thisfrq) const noexcept
{
    if (!prepared())
        return 0.0;
    double pp, ratio, ampdiff, thisamp;
    int z = 1;
    if (thisfrq < 0.0) /* NOT SURE THIS IS CORRECT */
        return 0.0;
    if (thisfrq <= 1.0)
        pp = 0.0;
    else
        pp = std::log10(thisfrq);
    const float* specenvpch = pch_.data();
    const float* ampstore = amp_.data();
    const int specenvcnt = static_cast<int>(pch_.size());
    while (specenvpch[z] < pp)
    {
        z++;
        if (z == specenvcnt - 1)
            break;
    }
    ratio = (pp - specenvpch[z - 1]) / (specenvpch[z] - specenvpch[z - 1]);
    ampdiff = ampstore[z] - ampstore[z - 1];
    // CDP8's build fuses the multiply and add.
    thisamp = std::fma(ampdiff, ratio, (double)ampstore[z - 1]);
    thisamp = (0.0 > thisamp) ? 0.0 : thisamp; /* max(0.0,*thisamp) */
    return thisamp;
}
}
