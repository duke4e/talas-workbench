/*
 * Derived from the CDP System, Release 8: dev/pitch/pitch.c and ap_pitch.c,
 * dev/spec/ap_simple.c, dev/cdp2k/formantsg.c, tklib1.c, tklib3.c and
 * readdata.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralPitchFile.h"

#include "talas/core/FromChars.h"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h.
constexpr double VERY_TINY_VAL = 0.00000000000000000001;
constexpr double FLTERR = 0.000002;
constexpr double OCTAVES_PER_SEMITONE = 0.08333333333;
constexpr double SEMITONE_INTERVAL = 1.05946309436;
constexpr double SPAN_FACTOR = 1.5;
constexpr double LOW_A = 6.875;
constexpr double MINPITCH = 9.0;
// dev/include/speccon.h.
constexpr double SPEC_MINFRQ = 10.0;
constexpr double PITCHZERO = SPEC_MINFRQ / 2.0;
constexpr double MIN_TRANSPOS = 0.00383;
constexpr double MAX_TRANSPOS = 256.0;
// dev/pitch/ap_pitch.c (macOS's math.h gives the same MAXFLOAT).
constexpr float HUGE_FRQ = 3.40282347e+38F;
constexpr double SCALEFUDGE = 3.0;
// dev/include/speccon.h's CHARBITSIZE times sizeof(int).
constexpr int INT_BITS = 32;

// tklib1.c's flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const double upperbnd = f2 + FLTERR;
    const double lowerbnd = f2 - FLTERR;
    return !((f1 > upperbnd) || (f1 < lowerbnd));
}

// read_param_as_value_or_brkfile_and_check_range (and, alike, each value of
// a breakpoint file in get_brkpnt_data_from_file_and_test_it): a value
// within FLTERR of a limit is taken as it, then checked.
[[nodiscard]] std::optional<double> inRange(double value, double lo, double hi) noexcept
{
    if (!std::isfinite(value))
        return std::nullopt;
    if (flteq(value, lo))
        value = lo;
    else if (flteq(value, hi))
        value = hi;
    if (value > hi || value < lo)
        return std::nullopt;
    return value;
}

// miditohz: CDP8's build calls exp2 for pow(2.0,...).
[[nodiscard]] double miditohz(double val) noexcept
{
    val += 3.0;
    val /= 12.0;
    val = std::exp2(val);
    val *= LOW_A;
    return val;
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

// As SpectralBands.cpp's: the pieces fgets(temp,200,fp) reads: up to 199
// characters, through a newline, each a C string ending at its first NUL.
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

// As SpectralBands.cpp's: tklib1.c's get_float_from_within_string, a word
// of a '-' or '.' or digit, then digits and at most one '.' in all, with a
// digit, read as sscanf's "%lf" reads it. Nothing, and no move, for any
// other word or none.
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
    if (const auto [end, error] = talas::fromChars(first, last, val); error != std::errc{} || end != last)
        return std::nullopt;
    at = p;
    return val;
}

// set_chunklens_and_establish_windowbufs's nyquist, chwidth and halfchwidth.
struct Geometry
{
    int clength = 0;
    double nyquist = 0.0;
    double chwidth = 0.0;
    double halfchwidth = 0.0;
};

[[nodiscard]] Geometry geometry(double sampleRate, std::size_t frameSize) noexcept
{
    Geometry g;
    g.clength = static_cast<int>(frameSize / 2);
    g.nyquist = sampleRate / 2.0;
    g.chwidth = g.nyquist / (double)(g.clength - 1);
    g.halfchwidth = g.chwidth / 2.0;
    return g;
}

// get_totalamp.
[[nodiscard]] double get_totalamp(std::span<const float> frame) noexcept
{
    double totalamp = 0.0;
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        totalamp += frame[vc];
    return totalamp;
}

// normalise.
void normalise(std::span<float> frame, double pre_totalamp, double post_totalamp) noexcept
{
    if (post_totalamp < VERY_TINY_VAL)
        return;
    const double normaliser = pre_totalamp / post_totalamp;
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        frame[vc] = (float)(frame[vc] * normaliser);
}

// move_data_into_appropriate_channel: the louder in the source frame wins.
void move_data_into_appropriate_channel(
    const float* flbuf, float* windowbuf, int vc, int truevc, float thisamp, float thisfrq) noexcept
{
    if (flbuf[vc] > windowbuf[truevc])
    {
        windowbuf[truevc++] = thisamp;
        windowbuf[truevc] = thisfrq;
    }
}

// move_data_into_some_appropriate_channel, whose scan limits mix a channel
// count with offsets and whose last resort compares the amplitude with the
// source frame's.
void move_data_into_some_appropriate_channel(
    const float* flbuf, float* windowbuf, int clength, int truevc, float thisamp, float thisfrq) noexcept
{
    constexpr int THISCHANSCAN = 4;
    int minscan, maxscan, n;
    if (windowbuf[truevc] == 0.0f)
    { /* 1 */
        windowbuf[truevc++] = thisamp;
        windowbuf[truevc] = thisfrq;
        return;
    }
    minscan = std::max(0, truevc - (THISCHANSCAN * 2)); /* 2 */
    maxscan = std::min(clength - 1, truevc + (THISCHANSCAN * 2)); /* 3 */
    for (n = truevc - 2; n > minscan; n -= 2)
    {
        if (windowbuf[n] == 0.0f)
        {
            windowbuf[n++] = thisamp;
            windowbuf[n] = thisfrq;
            return;
        }
    }
    for (n = truevc + 2; n < maxscan; n += 2)
    { /* 4 */
        if (windowbuf[n] == 0.0f)
        {
            windowbuf[n++] = thisamp;
            windowbuf[n] = thisfrq;
            return;
        }
    }
    if (thisamp > flbuf[truevc])
    { /* 5 */
        windowbuf[truevc++] = thisamp;
        windowbuf[truevc] = thisfrq;
        return;
    }
    for (n = truevc - 2; n > minscan; n -= 2)
    { /* 6 */
        if (thisamp > windowbuf[n])
        { /* 7 */
            windowbuf[n++] = thisamp;
            windowbuf[n] = thisfrq;
            return;
        }
    }
    for (n = truevc + 2; n < maxscan; n += 2)
    { /* 8 */
        if (thisamp > windowbuf[n])
        {
            windowbuf[n++] = thisamp;
            windowbuf[n] = thisfrq;
            return;
        }
    }
}

// remove_unwanted_frq_areas: CDP8 names vflag[0] CHORD_FTOP and vflag[1]
// CHORD_FBOT, though -b sets the first and -t the second.
void remove_unwanted_frq_areas(
    std::span<float> frame, bool bottomFlag, bool topFlag, double lofrq_limit, double hifrq_limit) noexcept
{
    float* flbuf = frame.data();
    const int wanted = static_cast<int>(frame.size());
    int vc;
    if (topFlag)
    { /* vflag[CHORD_FBOT] */
        if (bottomFlag)
        { /* vflag[CHORD_FTOP] */
            for (vc = 0; vc < wanted; vc += 2)
            {
                if (flbuf[vc + 1] < lofrq_limit || flbuf[vc + 1] > hifrq_limit)
                    flbuf[vc] = 0.0f;
            }
        }
        else
        {
            for (vc = 0; vc < wanted; vc += 2)
            {
                if (flbuf[vc + 1] < lofrq_limit)
                    flbuf[vc] = 0.0f;
            }
        }
    }
    else if (bottomFlag)
    {
        for (vc = 0; vc < wanted; vc += 2)
        {
            if (flbuf[vc + 1] > hifrq_limit)
                flbuf[vc] = 0.0f;
        }
    }
}

// The chords' -b and -t: get_variants_and_flags sets a variant's flag for a
// breakpoint file or a value other than its default (PITCHZERO and the
// Nyquist frequency).
struct Band
{
    double bottom = 0.0;
    double top = 0.0;
};

[[nodiscard]] std::optional<Band> chordBand(double nyquist, double bottom, double top) noexcept
{
    const auto lo = inRange(bottom, PITCHZERO, nyquist);
    const auto hi = inRange(top, PITCHZERO, nyquist);
    if (!lo.has_value() || !hi.has_value())
        return std::nullopt;
    return Band{*lo, *hi};
}

[[nodiscard]] bool validTranspositions(std::span<const float> transpositions) noexcept
{
    if (transpositions.empty())
        return false;
    // chordget's range.
    return std::ranges::all_of(transpositions, [](float transpos) {
        const auto val = (double)transpos;
        return std::isfinite(val) && !(val <= MIN_TRANSPOS || val > MAX_TRANSPOS);
    });
}

// CDP's ring of the size loudest channels, which initialise_ring_vals fills
// with the initial amplitude: an amplitude enters before the first entry it
// beats, ties staying behind, and the last entry falls out
// (if_one_of_loudest_chans_store_in_ring and
// rearrange_ring_to_allow_new_entry_and_return_entry_address; as
// SpectralBlur.cpp's).
void storeInRing(std::span<std::pair<int, float>> ring, int loc, float val) noexcept
{
    const auto place = std::partition_point(
        ring.begin(), ring.end(), [&](const std::pair<int, float>& entry) { return !(val > entry.second); });
    if (place == ring.end())
        return;
    std::move_backward(place, ring.end() - 1, ring.end());
    *place = {loc, val};
}
}

// ---------------------------------------------------------------- tune --

std::optional<std::vector<double>> readPitchTemplate(std::string_view text, PitchTune::Mode mode)
{
    if (mode != PitchTune::Mode::Frequencies && mode != PitchTune::Mode::Midi)
        return std::nullopt;
    // get_input_frqs.
    std::vector<double> fundamentals;
    forEachPiece(text, [&](std::string_view temp) {
        std::size_t p = 0;
        while (const auto value = floatWithin(temp, p))
        {
            double dummy = *value;
            if (mode == PitchTune::Mode::Midi)
                dummy = miditohz(dummy);
            fundamentals.push_back((float)dummy);
        }
        return true;
    });
    if (fundamentals.empty())
        return std::nullopt;
    return fundamentals;
}

// The trace as TUNE_INDX holds it, rounded as read_value_from_brktable
// rounds an integer field.
std::optional<double> PitchTune::traceValue(double trace) const noexcept
{
    return inRange(trace, 1.0, (double)clength_);
}

// The bass cut as TUNE_BFRQ holds it, after setup_pitchzero.
std::optional<double> PitchTune::bassCutValue(double bassCut) const noexcept
{
    const auto value = inRange(bassCut, MINPITCH, nyquist_);
    if (!value.has_value())
        return std::nullopt;
    if (*value < SPEC_MINFRQ)
        return liveBassCut_ ? SPEC_MINFRQ : 0.0;
    return value;
}

bool PitchTune::prepare(double sampleRate, int points, int overlap, std::span<const double> pitches, double focus,
    double clarity, double trace, bool liveTrace, double bassCut, bool liveBassCut)
{
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap) || pitches.empty())
        return false;
    const auto g = geometry(sampleRate, vocoder.frameSize());
    const auto foc = inRange(focus, 0.0, 1.0);
    const auto clar = inRange(clarity, 0.0, 1.0);
    if (!foc.has_value() || !clar.has_value())
        return false;
    // get_input_frqs's range, then the float generate_template takes, which
    // must stay under the Nyquist frequency; getnextfrq steps through every
    // harmonic of a pitch, and stops growing on the tiniest.
    std::vector<double> fundamentals;
    for (const double pitch : pitches)
    {
        if (!std::isfinite(pitch) || pitch <= 0.0 || pitch >= g.nyquist)
            return false;
        const double stored = (float)pitch;
        if (stored >= g.nyquist || stored < g.nyquist / 16777216.0)
            return false;
        fundamentals.push_back(stored);
    }
    PitchTune tune;
    tune.clength_ = g.clength;
    tune.nyquist_ = g.nyquist;
    tune.liveBassCut_ = liveBassCut;
    // get_variants_and_flags: -t and -b are on for a breakpoint file or a
    // value other than their defaults.
    if (liveTrace || trace != 0.0)
    {
        const auto value = tune.traceValue(trace);
        if (!value.has_value())
            return false;
        tune.tracing_ = liveTrace || !flteq(*value, defaultTrace);
        tune.startTrace_ = (int)std::lround(*value);
    }
    if (liveBassCut || bassCut != 0.0)
    {
        if (!inRange(bassCut, MINPITCH, g.nyquist).has_value())
            return false;
        tune.bassCutting_ = liveBassCut || !flteq(*inRange(bassCut, MINPITCH, g.nyquist), defaultBassCut);
        tune.startBassCut_ = *tune.bassCutValue(bassCut);
    }
    // generate_template, with getnextfrq.
    std::vector<double> harmonics = fundamentals;
    const auto getnextfrq = [&]() {
        std::size_t minptr = 0;
        double minfrq = HUGE_FRQ;
        for (std::size_t n = 0; n < harmonics.size(); n++)
        {
            if (harmonics[n] < minfrq)
            { /* FIND CURRENT MIN FRQ IN ARRAY harmonics[] */
                minfrq = harmonics[n];
                minptr = n;
            }
        }
        harmonics[minptr] += fundamentals[minptr]; /* INCREMENT MIN VALUE TO NEXT APPROPRIATE HARMONIC */
        return minfrq;
    };
    tune.template_.assign(static_cast<std::size_t>(g.clength), 0.0F);
    double lastchtop = 0.0;
    double thischtop = g.chwidth / 2.0;
    double thisfrq = getnextfrq();
    for (int n = 0; n < g.clength; n++)
    {
        while (thisfrq < lastchtop)
            thisfrq = getnextfrq();
        if (thisfrq > thischtop)
            tune.template_[static_cast<std::size_t>(n)] = (float)(-1.0);
        else
            tune.template_[static_cast<std::size_t>(n)] = (float)thisfrq;
        lastchtop = thischtop;
        if ((thischtop += g.chwidth) > g.nyquist)
            thischtop = g.nyquist;
    }
    // Every pitch under the Nyquist frequency reaches a channel, which
    // replace_partial_by_tuned_partial's search needs.
    if (std::ranges::none_of(tune.template_, [](float frq) { return frq > 0.0; }))
        return false;
    tune.vocoder_ = std::move(vocoder);
    // tune_preprocess: TUNE_CLAR as 1 - clarity (convert_params_for_tune).
    tune.startFocus_ = *foc;
    tune.startClarity_ = 1.0 - *clar;
    tune.ring_.assign(static_cast<std::size_t>(g.clength), {0, 0.0F});
    tune.loud_.assign(tune.vocoder_.frameSize(), 0);
    *this = std::move(tune);
    reset();
    return true;
}

bool PitchTune::setControls(double focus, double clarity, double trace, double bassCut) noexcept
{
    if (!prepared())
        return false;
    const auto foc = inRange(focus, 0.0, 1.0);
    const auto clar = inRange(clarity, 0.0, 1.0);
    if (!foc.has_value() || !clar.has_value())
        return false;
    std::optional<double> traceNow;
    std::optional<double> bassCutNow;
    if (tracing_ && !(traceNow = traceValue(trace)).has_value())
        return false;
    if (bassCutting_ && !(bassCutNow = bassCutValue(bassCut)).has_value())
        return false;
    focus_ = *foc;
    clarity_ = 1.0 - *clar;
    if (traceNow.has_value())
        trace_ = (int)std::lround(*traceNow);
    if (bassCutNow.has_value())
        bassCut_ = *bassCutNow;
    return true;
}

void PitchTune::reset() noexcept
{
    vocoder_.reset();
    focus_ = startFocus_;
    clarity_ = startClarity_;
    trace_ = startTrace_;
    bassCut_ = startBassCut_;
    std::ranges::fill(loud_, 0);
}

bool PitchTune::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) { tuneFrame(frame); });
}

// spectune.
void PitchTune::tuneFrame(std::span<float> frame) noexcept
{
    float* flbuf = frame.data();
    const int wanted = static_cast<int>(frame.size());
    int cc, vc;
    if (bassCutting_)
    {
        for (cc = 0, vc = 0; vc < wanted; cc++, vc += 2)
        {
            if (flbuf[vc + 1] < bassCut_)
                flbuf[vc] = 0.0f;
        }
    }
    if (tracing_)
    {
        // initialise_ring_vals(TUNE_INDX,-1.0), then each channel's
        // amplitude-buffer offset (vc) into the ring.
        const auto ring = std::span{ring_}.first(static_cast<std::size_t>(trace_));
        std::ranges::fill(ring, std::pair{0, (float)(-1.0)});
        for (cc = 0, vc = 0; vc < wanted; cc++, vc += 2)
            storeInRing(ring, vc, flbuf[vc]);
        // CDP clears clength flags, sets those at the ring's offsets and
        // reads them by channel.
        std::fill(loud_.begin(), loud_.begin() + clength_, 0);
        for (const auto& loudest : ring)
            loud_[static_cast<std::size_t>(loudest.first)] = 1;
        for (cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
        {
            if (loud_[static_cast<std::size_t>(cc)])
                replacePartialByTunedPartial(cc, vc, flbuf);
            else
                flbuf[vc] = (float)(flbuf[vc] * clarity_);
        }
    }
    else
    {
        for (cc = 0, vc = 0; vc < wanted; cc++, vc += 2)
        {
            if (template_[static_cast<std::size_t>(cc)] > 0.0)
                focusPartialTowardsTunedValue(vc + 1, cc, flbuf);
            else
                flbuf[vc] = (float)(flbuf[vc] * clarity_);
        }
    }
}

// replace_partial_by_tuned_partial.
void PitchTune::replacePartialByTunedPartial(int cc, int vc, float* flbuf) const noexcept
{
    const float* frq_template = template_.data();
    int aa = cc, bb = cc, here, there;
    double upratio, dnratio;
    if (frq_template[cc] > 0.0)
    { /* if loudchan is a validfrq chan */
        focusPartialTowardsTunedValue(vc + 1, cc, flbuf);
        return;
    }
    /* when loudchan NOT validfrq chan */
    while (aa >= 0 && frq_template[aa] <= 0.0) /* search downwards */
        aa--;
    while (bb < clength_ && frq_template[bb] <= 0.0) /* and upwards */
        bb++; /* for closest validfrq chans */
    if (aa < 0 && bb < clength_)
    {
        here = bb * 2; /* if none below, use one above */
        aa = bb;
    }
    else if (bb >= clength_ && aa >= 0)
    { /* if none above, use one below */
        here = aa * 2;
    }
    else
    {
        here = aa * 2; /* otherwise, choose nearest validfrq */
        there = bb * 2;
        // Float divisions.
        upratio = flbuf[there + 1] / flbuf[vc + 1];
        dnratio = flbuf[vc + 1] / flbuf[here + 1];
        if (upratio < dnratio)
        {
            here = there;
            aa = bb;
        }
    }
    focusPartialTowardsTunedValue(here + 1, aa, flbuf);
    flbuf[here] = flbuf[vc]; /* and gets ampl of nearest loudchan,    */
    flbuf[vc] = 0.0F; /* while actual loudchan is zeroed out.  */
}

// focus_partial_towards_tuned_value: the difference in float, the step not
// fused.
void PitchTune::focusPartialTowardsTunedValue(int vc, int cc, float* flbuf) const noexcept
{
    double frqshift;
    frqshift = template_[static_cast<std::size_t>(cc)] - flbuf[vc];
    frqshift *= focus_;
    flbuf[vc] = (float)(flbuf[vc] + frqshift);
}

// ---------------------------------------------------------------- pick --

bool PitchPick::prepare(
    double sampleRate, int points, int overlap, Mode mode, double fundamental, double step, double clarity)
{
    if (mode < Mode::Harmonics || mode > Mode::DisplacedHarmonics)
        return false;
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    const auto g = geometry(sampleRate, vocoder.frameSize());
    const auto fund = inRange(fundamental, SPEC_MINFRQ, g.nyquist);
    const auto clar = inRange(clarity, 0.0, 1.0);
    if (!fund.has_value() || !clar.has_value())
        return false;
    double lin = 0.0;
    if (mode == Mode::Linear || mode == Mode::DisplacedHarmonics)
    {
        const auto value
            = inRange(step, mode == Mode::DisplacedHarmonics ? -g.nyquist : SPEC_MINFRQ, g.nyquist);
        if (!value.has_value())
            return false;
        lin = *value;
    }
    // setup_internal_bitflags: PICK_LONGPOW2 and PICK_DIVMASK for an int's
    // bits, and init_bitflags_to_zero.
    PitchPick pick;
    pick.longpow2_ = 5;
    pick.divmask_ = (1 << pick.longpow2_) - 1;
    static_assert(1 << 5 == INT_BITS);
    pick.bitflags_.assign(static_cast<std::size_t>(g.clength / INT_BITS) + 1, 0U);
    // mark_chosen_channels_in_bitflags, with chan_containing_partial.
    int n = 1, chan = 0;
    int botpchan = 0;
    double lastchtop = g.chwidth / 2.0;
    double partial = *fund;
    do
    {
        bool found = false;
        double chtop = lastchtop;
        for (int k = botpchan; k <= g.clength; k++)
        {
            if (partial < chtop)
            {
                botpchan = k;
                lastchtop = chtop;
                chan = k;
                found = true;
                break;
            }
            chtop += g.chwidth;
        }
        if (!found)
            return false;
        pick.bitflags_[static_cast<std::size_t>(chan >> pick.longpow2_)] |= (1U << (chan & pick.divmask_));
        switch (mode)
        {
        case Mode::Harmonics:
            n++;
            partial = *fund * (double)n;
            break;
        case Mode::Octaves:
            partial = partial * 2.0;
            break;
        case Mode::OddHarmonics:
            n += 2;
            partial = *fund * (double)n;
            break;
        case Mode::Linear:
            partial += lin;
            break;
        case Mode::DisplacedHarmonics:
            n++;
            // CDP8's build fuses the multiply and add.
            partial = std::fma(*fund, (double)n, lin);
            break;
        }
    } while (partial < g.nyquist);
    pick.vocoder_ = std::move(vocoder);
    // adjust_parameters_for_specpick: PICK_CLAR as (1 - clarity) cubed.
    pick.startClarity_ = std::pow(1.0 - *clar, SCALEFUDGE);
    *this = std::move(pick);
    reset();
    return true;
}

bool PitchPick::setClarity(double clarity) noexcept
{
    const auto clar = inRange(clarity, 0.0, 1.0);
    if (!prepared() || !clar.has_value())
        return false;
    clarity_ = std::pow(1.0 - *clar, SCALEFUDGE);
    return true;
}

void PitchPick::reset() noexcept
{
    vocoder_.reset();
    clarity_ = startClarity_;
}

bool PitchPick::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) { pickFrame(frame); });
}

// specpick, with choose_bflagno_and_reset_mask_if_ness.
void PitchPick::pickFrame(std::span<float> frame) const noexcept
{
    float* flbuf = frame.data();
    const int clength = static_cast<int>(frame.size() / 2);
    std::uint32_t mask = 1;
    int cc, vc, bflagno;
    for (cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        bflagno = cc >> longpow2_; /* bflagno chooses which bitflag */
        if (!(cc & divmask_)) /* If cc divisible by bitflag length */
            mask = 1; /* reset bitmask to 1, for next bitflag */
        if (!(bitflags_[static_cast<std::size_t>(bflagno)] & mask)) /* if bit NOT set, modify amplitude */
            flbuf[vc] = (float)(flbuf[vc] * clarity_);
        mask <<= 1; /* move bitmask upwards */
    }
}

// --------------------------------------------------------------- chords --

std::optional<std::vector<float>> readChordTranspositions(std::string_view text)
{
    // chordget: CDP8's build calls exp2 for pow(2.0,...).
    std::vector<float> transpos;
    bool ok = true;
    forEachPiece(text, [&](std::string_view temp) {
        std::size_t q = 0;
        while (const auto value = floatWithin(temp, q))
        {
            double val = *value;
            val *= OCTAVES_PER_SEMITONE; /* CONVERT SEMITONES TO OCTAVES */
            val = std::exp2(val); /* CONVERT OCTAVES TO FRQ RATIO */
            if (val <= MIN_TRANSPOS || val > MAX_TRANSPOS)
            {
                ok = false;
                return false;
            }
            transpos.push_back((float)val);
        }
        return true;
    });
    if (!ok || transpos.empty())
        return std::nullopt;
    return transpos;
}

bool PitchChord::prepare(double sampleRate, int points, int overlap, std::span<const float> transpositions,
    double bottom, bool liveBottom, double top, bool liveTop, bool lessBody)
{
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap) || !validTranspositions(transpositions))
        return false;
    const auto g = geometry(sampleRate, vocoder.frameSize());
    const auto band = chordBand(g.nyquist, bottom, top);
    if (!band.has_value())
        return false;
    vocoder_ = std::move(vocoder);
    clength_ = g.clength;
    nyquist_ = g.nyquist;
    chwidth_ = g.chwidth;
    halfchwidth_ = g.halfchwidth;
    transpos_.assign(transpositions.begin(), transpositions.end());
    lessBody_ = lessBody;
    bottomFlag_ = liveBottom || !flteq(band->bottom, PITCHZERO);
    topFlag_ = liveTop || !flteq(band->top, g.nyquist);
    liveBand_ = liveBottom || liveTop;
    // A flag left out keeps CDP's default.
    startLofrq_ = bottomFlag_ ? band->bottom : PITCHZERO;
    startHifrq_ = topFlag_ ? band->top : g.nyquist;
    windowbuf_.assign(vocoder_.frameSize(), 0.0F);
    reset();
    return true;
}

bool PitchChord::setBand(double bottom, double top) noexcept
{
    if (!prepared())
        return false;
    const auto band = chordBand(nyquist_, bottom, top);
    if (!band.has_value())
        return false;
    if (bottomFlag_)
        lofrq_ = band->bottom;
    if (topFlag_)
        hifrq_ = band->top;
    return true;
}

void PitchChord::reset() noexcept
{
    vocoder_.reset();
    lofrq_ = startLofrq_;
    hifrq_ = startHifrq_;
    std::ranges::fill(windowbuf_, 0.0F);
    frames_ = 0;
}

bool PitchChord::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        if (frames_++ == 0)
        {
            // skip_or_special_operation_on_window_zero.
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
                windowbuf_[vc + 1] = frame[vc + 1];
            return;
        }
        chordFrame(frame);
    });
}

// specchord2, with gotnewfrq2: only the amplitudes go back to the frame.
void PitchChord::chordFrame(std::span<float> frame) noexcept
{
    float* flbuf = frame.data();
    float* windowbuf = windowbuf_.data();
    const int wanted = static_cast<int>(frame.size());
    const int itemcnt = static_cast<int>(transpos_.size());
    int newvc, newcc, cc, vc, n;
    double pre_amptotal, post_amptotal;
    double thisamp, thisfrq;
    double hifrq = hifrq_;
    double lofrq = lofrq_;
    if (liveBand_)
    {
        if (hifrq < lofrq)
            std::swap(hifrq, lofrq);
    }
    pre_amptotal = get_totalamp(frame);
    for (vc = 0; vc < wanted; vc += 2)
        windowbuf[vc] = 0.0F;
    for (cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
    {
        for (n = 0; n < itemcnt; n++)
        { /* for each note of chord */
            // The product in float.
            if ((thisfrq = std::fabs(flbuf[vc + 1] * transpos_[static_cast<std::size_t>(n)])) > nyquist_)
                continue;
            thisamp = flbuf[vc];
            if ((newcc = (int)((thisfrq + halfchwidth_) / chwidth_)) >= clength_) /* TRUNCATE */
                continue;
            newvc = newcc * 2;
            if (lessBody_)
                move_data_into_appropriate_channel(flbuf, windowbuf, vc, newvc, (float)thisamp, (float)thisfrq);
            else
                move_data_into_some_appropriate_channel(
                    flbuf, windowbuf, clength_, newvc, (float)thisamp, (float)thisfrq);
        }
    }
    for (vc = 0; vc < wanted; vc += 2)
        flbuf[vc] = windowbuf[vc];
    if (bottomFlag_ || topFlag_)
        remove_unwanted_frq_areas(frame, bottomFlag_, topFlag_, lofrq, hifrq);
    post_amptotal = get_totalamp(frame);
    normalise(frame, pre_amptotal, post_amptotal);
}

// setup_pitchzero.
double PitchChordFormants::limit(double frequency, bool live) const noexcept
{
    if (frequency < SPEC_MINFRQ)
        return live ? SPEC_MINFRQ : 0.0;
    return frequency;
}

bool PitchChordFormants::prepare(double sampleRate, int points, int overlap, bool pitchwise, int bands,
    bool quickSearch, std::span<const float> transpositions, double bottom, bool liveBottom, double top,
    bool liveTop, bool lessBody)
{
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap) || !validTranspositions(transpositions))
        return false;
    SpectralEnvelope envelope;
    if (!envelope.prepare(sampleRate, vocoder.frameSize(), pitchwise, bands, quickSearch))
        return false;
    const auto g = geometry(sampleRate, vocoder.frameSize());
    const auto band = chordBand(g.nyquist, bottom, top);
    if (!band.has_value())
        return false;
    vocoder_ = std::move(vocoder);
    envelope_ = std::move(envelope);
    clength_ = g.clength;
    nyquist_ = g.nyquist;
    chwidth_ = g.chwidth;
    halfchwidth_ = g.halfchwidth;
    transpos_.assign(transpositions.begin(), transpositions.end());
    lessBody_ = lessBody;
    // The flags from the values as given, then handle_pitch_zeros.
    bottomFlag_ = liveBottom || !flteq(band->bottom, PITCHZERO);
    topFlag_ = liveTop || !flteq(band->top, g.nyquist);
    liveBottom_ = liveBottom;
    liveTop_ = liveTop;
    startLofrq_ = limit(bottomFlag_ ? band->bottom : PITCHZERO, liveBottom);
    startHifrq_ = limit(topFlag_ ? band->top : g.nyquist, liveTop);
    windowbuf_.assign(vocoder_.frameSize(), 0.0F);
    reset();
    return true;
}

bool PitchChordFormants::setBand(double bottom, double top) noexcept
{
    if (!prepared())
        return false;
    const auto band = chordBand(nyquist_, bottom, top);
    if (!band.has_value())
        return false;
    if (bottomFlag_)
        lofrq_ = limit(band->bottom, liveBottom_);
    if (topFlag_)
        hifrq_ = limit(band->top, liveTop_);
    return true;
}

void PitchChordFormants::reset() noexcept
{
    vocoder_.reset();
    lofrq_ = startLofrq_;
    hifrq_ = startHifrq_;
    std::ranges::fill(windowbuf_, 0.0F);
    frames_ = 0;
}

bool PitchChordFormants::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        if (frames_++ == 0)
        {
            // skip_or_special_operation_on_window_zero.
            for (std::size_t vc = 0; vc < frame.size(); vc += 2)
                windowbuf_[vc + 1] = frame[vc + 1];
            return;
        }
        chordFrame(frame);
    });
}

// specchord, with gotnewfrq: the envelope of the frame as it is (CDP does
// not rectify it first); only the amplitudes go back to the frame.
void PitchChordFormants::chordFrame(std::span<float> frame) noexcept
{
    float* flbuf = frame.data();
    float* windowbuf = windowbuf_.data();
    const int wanted = static_cast<int>(frame.size());
    const int itemcnt = static_cast<int>(transpos_.size());
    int newvc, newcc, cc, vc, n;
    double pre_amptotal, post_amptotal;
    double thisamp, thisfrq, thisspecamp, newspecamp;
    double hifrq = hifrq_;
    double lofrq = lofrq_;
    if (liveBottom_ || liveTop_)
    {
        if (hifrq < lofrq)
            std::swap(hifrq, lofrq);
    }
    envelope_.extract(frame);
    pre_amptotal = get_totalamp(frame);
    for (vc = 0; vc < wanted; vc += 2)
        windowbuf[vc] = 0.0F;
    for (cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
    {
        thisspecamp = envelope_.amplitudeAt((double)flbuf[vc + 1]);
        if (thisspecamp < VERY_TINY_VAL)
            continue;
        for (n = 0; n < itemcnt; n++)
        { /* for each note of chord */
            // The product in float.
            if ((thisfrq = std::fabs(flbuf[vc + 1] * transpos_[static_cast<std::size_t>(n)])) > nyquist_)
                continue;
            newspecamp = envelope_.amplitudeAt(thisfrq);
            if (newspecamp < VERY_TINY_VAL)
                continue;
            if ((thisamp = flbuf[vc] * newspecamp / thisspecamp) < VERY_TINY_VAL)
                continue;
            if ((newcc = (int)((thisfrq + halfchwidth_) / chwidth_)) >= clength_) /* TRUNCATE */
                continue;
            newvc = newcc * 2;
            if (lessBody_)
                move_data_into_appropriate_channel(flbuf, windowbuf, vc, newvc, (float)thisamp, (float)thisfrq);
            else
                move_data_into_some_appropriate_channel(
                    flbuf, windowbuf, clength_, newvc, (float)thisamp, (float)thisfrq);
        }
    }
    for (vc = 0; vc < wanted; vc += 2)
        flbuf[vc] = windowbuf[vc];
    if (bottomFlag_ || topFlag_)
        remove_unwanted_frq_areas(frame, bottomFlag_, topFlag_, lofrq, hifrq);
    post_amptotal = get_totalamp(frame);
    normalise(frame, pre_amptotal, post_amptotal);
}

// ---------------------------------------------------------------- bare --

bool SpecBare::prepare(double sampleRate, int points, int overlap, std::span<const float> pitches, bool lessBody)
{
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    // A pitch of 0 Hz loops forever in specbare; `repitch getpitch` writes
    // NOT_PITCH, NOT_SOUND or pitches from SPEC_MINFRQ up.
    if (!std::ranges::all_of(pitches, [](float pitch) {
            return std::isfinite(pitch) && (pitch < 0.0 || pitch >= MINPITCH);
        }))
    {
        return false;
    }
    vocoder_ = std::move(vocoder);
    nyquist_ = sampleRate / 2.0;
    lessBody_ = lessBody;
    pitches_.assign(pitches.begin(), pitches.end());
    reset();
    return true;
}

void SpecBare::reset() noexcept
{
    vocoder_.reset();
    frames_ = 0;
}

bool SpecBare::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        // skip_or_special_operation_on_window_zero: CDP's pitchcnt starts
        // at the first frame it processes.
        const auto window = frames_++;
        if (window == 0)
            return;
        const auto pitchcnt = window - 1;
        if (pitchcnt >= pitches_.size())
            return;
        bareFrame(frame, pitches_[pitchcnt]);
    });
}

// specbare, with zero_channels_between_partials,
// eliminate_all_but_loudest_channel_near_this_partial_frq and
// keep_all_channels_near_partial_frq.
void SpecBare::bareFrame(std::span<float> frame, double pitch) const noexcept
{
    float* flbuf = frame.data();
    const int wanted = static_cast<int>(frame.size());
    constexpr double error_range = SPAN_FACTOR * SEMITONE_INTERVAL; /* +or- error in finding pitch of partial */
    double thisfrq, frq_close_above_next_partial, frq_close_below_next_partial;
    int n = 0;
    int vc = 0;
    constexpr double one_over_error_range = 1.0 / error_range;
    double upper_freqdiff, lower_freqdiff;
    if (pitch < 0.0) /*   UNPITCHED WINDOW : marked in specpich */
        return;
    // CDP8's build fuses both.
    upper_freqdiff = std::fma(pitch, error_range, -pitch);
    lower_freqdiff = std::fma(pitch, -one_over_error_range, pitch);
    while (vc < wanted && (thisfrq = pitch * (double)++n) < nyquist_)
    {
        frq_close_above_next_partial = thisfrq + upper_freqdiff;
        frq_close_below_next_partial = thisfrq - lower_freqdiff;
        // zero_channels_between_partials.
        while (flbuf[vc + 1] < frq_close_below_next_partial)
        {
            flbuf[vc] = 0.0f;
            if ((vc += 2) >= wanted)
                break;
        }
        if (vc >= wanted)
            break;
        if (flbuf[vc + 1] > frq_close_above_next_partial)
            continue;
        else if (lessBody_)
        {
            // eliminate_all_but_loudest_channel_near_this_partial_frq.
            double maxamp = flbuf[vc]; /* SET MAXAMP TO 1ST IN-RANGE CH */
            int k, maxloc = vc; /* NOTE NO. OF THE CH THUS MARKD */
            const int firstloc = vc; /* MARK FIRST CH IN SET          */
            while ((vc += 2) < wanted)
            {
                if (flbuf[vc + 1] > frq_close_above_next_partial) /* IF BEYOND CURRENT RANGE, STOP */
                    break;
                if (flbuf[vc] > maxamp)
                { /* IF LOUDER THAN MAX,RESET MAX  */
                    maxamp = flbuf[vc]; /* AND RESET MAXAMP CHANNEL NO.  */
                    maxloc = vc;
                }
            }
            for (k = firstloc; k < vc; k += 2)
            { /* FOR THE WHOLE SET THUS DONE  */
                if (k != maxloc) /* ZERO ALL CHS OTHER THAN MAX  */
                    flbuf[k] = 0.0f;
            }
        }
        else
        {
            // keep_all_channels_near_partial_frq.
            while ((vc += 2) < wanted)
            {
                if (flbuf[vc + 1] > frq_close_above_next_partial) /* IF BEYOND CURRENT RANGE, STOP */
                    break;
            }
        }
    }
}
}
