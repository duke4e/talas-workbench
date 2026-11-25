/*
 * Derived from the CDP System, Release 8: dev/formants/formants.c and
 * ap_formants.c, dev/hilite/hilite.c and ap_hilite.c, dev/include/vowels.h
 * and vowels2.h, and dev/cdp2k/formantsg.c, readfiles.c, tklib1.c and
 * tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralFormants.h"

#include "talas/core/FromChars.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace talas::cdp
{
namespace
{
// dev/include/globcon.h, speccon.h and tkglobals.h.
constexpr double FLTERR = 0.000002;
constexpr double VERY_TINY_VAL = 0.00000000000000000001;
constexpr double SPEC_MINFRQ = 10.0;
constexpr double PITCHZERO = SPEC_MINFRQ / 2.0;
constexpr double FORM_MAX_GAIN = 40.0;
constexpr int BUF_MULTIPLIER = 20;
constexpr int DESCRIPTOR_DATA_BLOKS = 2;

// tklib1.c's flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const double upperbnd = f2 + FLTERR;
    const double lowerbnd = f2 - FLTERR;
    return !((f1 > upperbnd) || (f1 < lowerbnd));
}

[[nodiscard]] bool within(double value, double low, double high) noexcept
{
    return std::isfinite(value) && value >= low && value <= high;
}

// rectify_window (formantsg.c).
void rectify_window(std::span<float> frame) noexcept
{
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
    {
        if (frame[vc + 1] < 0.0)
            frame[vc + 1] = -frame[vc + 1];
    }
}

// get_totalamp (tklib3.c).
[[nodiscard]] double get_totalamp(std::span<const float> frame) noexcept
{
    double totalamp = 0.0;
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        totalamp += frame[vc];
    return totalamp;
}

// normalise (tklib3.c): a frame summing to nothing stays as it is.
void normalise(std::span<float> frame, double pre_totalamp, double post_totalamp) noexcept
{
    if (post_totalamp < VERY_TINY_VAL)
        return;
    const double normaliser = pre_totalamp / post_totalamp;
    for (std::size_t vc = 0; vc < frame.size(); vc += 2)
        frame[vc] = (float)(frame[vc] * normaliser);
}
}

bool FormantsPut::prepare(double sampleRate, int points, int overlap, bool pitchwise, int bands,
    bool getQuickSearch, Mode mode, bool quickSearch, double lowFrequency, double highFrequency, double gain)
{
    if (mode != Mode::Replace && mode != Mode::Impose)
        return false;
    PhaseVocoder vocoder;
    PhaseVocoder sideVocoder;
    if (!vocoder.prepare(sampleRate, points, overlap) || !sideVocoder.prepare(sampleRate, points, overlap))
        return false;
    // get's -f/-p and -i set its envelope; put reads the same bands from
    // the formant file's header (specenvpch and specenvtop) and takes its
    // own -i.
    SpectralEnvelope getEnvelope;
    SpectralEnvelope putEnvelope;
    if (!getEnvelope.prepare(sampleRate, vocoder.frameSize(), pitchwise, bands, getQuickSearch)
        || !putEnvelope.prepare(sampleRate, vocoder.frameSize(), pitchwise, bands, quickSearch))
    {
        return false;
    }
    // tklib1.c's ranges, then handle_pitch_zeros (setup_pitchzero) and
    // check_compatibility_of_params_for_form.
    const double nyquist = getEnvelope.nyquist();
    if (!within(lowFrequency, PITCHZERO, nyquist) || !within(highFrequency, PITCHZERO, nyquist)
        || !within(gain, FLTERR, FORM_MAX_GAIN))
    {
        return false;
    }
    const double fbot = lowFrequency < SPEC_MINFRQ ? 0.0 : lowFrequency;
    const double ftop = highFrequency < SPEC_MINFRQ ? 0.0 : highFrequency;
    if (ftop <= fbot)
        return false;
    // calculate_analdata_plus_formantdata_buffer: the formant buffer
    // (buflen2) that setup_formant_params fills and specform then takes for
    // the input's first ssampsread / wanted frames.
    const int specenvcnt = static_cast<int>(getEnvelope.frequencies().size());
    const int wanted = static_cast<int>(vocoder.frameSize());
    const int descriptor_samps = specenvcnt * DESCRIPTOR_DATA_BLOKS;
    int buflen2 = 0;
    for (int cnt = 1; buflen2 < descriptor_samps; cnt++)
        buflen2 = specenvcnt * BUF_MULTIPLIER * cnt;
    vocoder_ = std::move(vocoder);
    sideVocoder_ = std::move(sideVocoder);
    getEnvelope_ = std::move(getEnvelope);
    putEnvelope_ = std::move(putEnvelope);
    mode_ = mode;
    lowFrequency_ = fbot;
    highFrequency_ = ftop;
    gain_ = gain;
    shift_ = buflen2 / wanted;
    side_.assign(static_cast<std::size_t>(wanted), 0.0F);
    ring_.assign(static_cast<std::size_t>(shift_) * static_cast<std::size_t>(wanted), 0.0F);
    work_.assign(static_cast<std::size_t>(wanted), 0.0F);
    reset();
    return true;
}

void FormantsPut::reset() noexcept
{
    vocoder_.reset();
    sideVocoder_.reset();
    std::ranges::fill(ring_, 0.0F);
    ringAt_ = 0;
    frames_ = 0;
}

bool FormantsPut::process(std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    return vocoder_.process(input, side, sideVocoder_, output,
        [this](std::span<float> frame, std::span<const float> sideFrame) { putFrame(frame, sideFrame); });
}

// specformants (get) and specform with inner_form_loop (put), a frame of
// each.
void FormantsPut::putFrame(std::span<float> frame, std::span<const float> sideFrame) noexcept
{
    const std::size_t wanted = frame.size();
    const bool windowZero = frames_++ == 0;
    // get: skip_or_special_operation_on_window_zero writes a zero formant
    // for the first frame (which put never reads), do_specformants the
    // side's rectified envelope for the others.
    if (!windowZero)
    {
        std::ranges::copy(sideFrame, side_.begin());
        rectify_window(side_);
        getEnvelope_.extract(side_);
    }
    // put: the input shift_ frames late, after that many of the zeroed
    // buffer.
    if (shift_ > 0)
    {
        float* oldest = ring_.data() + ringAt_ * wanted;
        std::copy(oldest, oldest + wanted, work_.begin());
        std::ranges::copy(frame, oldest);
        ringAt_ = (ringAt_ + 1) % static_cast<std::size_t>(shift_);
    }
    else
        std::ranges::copy(frame, work_.begin());
    // inner_form_loop: SKIP FIRST (ZERO-AMP) WINDOW.
    if (!windowZero)
    {
        float* flbufptr0 = work_.data();
        double pre_amptotal, post_amptotal;
        if (mode_ == Mode::Replace)
        {
            rectify_window(work_);
            putEnvelope_.extract(work_);
        }
        pre_amptotal = get_totalamp(work_);
        // do_specform: getspecenvamp's store 0 is put's envelope, store 1
        // the formant file's.
        rectify_window(work_);
        double thisspecamp0, thisspecamp1, thisvalue;
        switch (mode_)
        {
        case Mode::Replace:
            for (std::size_t vc = 0; vc < wanted; vc += 2)
            {
                thisspecamp0 = putEnvelope_.amplitudeAt((double)flbufptr0[vc + 1]);
                thisspecamp1 = getEnvelope_.amplitudeAt((double)flbufptr0[vc + 1]);
                if (thisspecamp0 < VERY_TINY_VAL)
                    flbufptr0[vc] = 0.0f;
                else
                {
                    if ((thisvalue = flbufptr0[vc] * thisspecamp1 / thisspecamp0) < VERY_TINY_VAL)
                        flbufptr0[vc] = 0.0f;
                    else
                        flbufptr0[vc] = (float)thisvalue;
                }
            }
            break;
        case Mode::Impose:
            for (std::size_t vc = 0; vc < wanted; vc += 2)
            {
                thisspecamp1 = getEnvelope_.amplitudeAt((double)flbufptr0[vc + 1]);
                if ((thisvalue = flbufptr0[vc] * thisspecamp1) < VERY_TINY_VAL)
                    flbufptr0[vc] = 0.0f;
                else
                    flbufptr0[vc] = (float)thisvalue;
            }
            break;
        }
        // form_filter.
        for (std::size_t vc = 0; vc < wanted; vc += 2)
        {
            if (flbufptr0[vc + 1] < lowFrequency_ || flbufptr0[vc + 1] > highFrequency_)
                flbufptr0[vc] = 0.0f;
        }
        post_amptotal = get_totalamp(work_);
        normalise(work_, pre_amptotal, post_amptotal);
        // form_gain.
        if (!flteq(gain_, 1.0))
        {
            for (std::size_t vc = 0; vc < wanted; vc += 2)
                flbufptr0[vc] = (float)(flbufptr0[vc] * gain_);
        }
    }
    std::ranges::copy(work_, frame.begin());
}

namespace
{
// get_formant_frqs with vowels.h: formant1, formant2, formant3, f2atten
// and f3atten for each vowel number from 1.
struct VowelFormants
{
    double formant1;
    double formant2;
    double formant3;
    double f2atten;
    double f3atten;
};

constexpr std::array<VowelFormants, 20> vowelFormants{{
    {300, 2600, 2800, 0.85, 0.85}, // VOWEL_EE
    {360, 2100, 2300, 1.0, 1.0},   // VOWEL_I
    {465, 2250, 2450, .92, 0.25},  // VOWEL_AI
    {570, 1970, 2450, 1.0, 0.75},  // VOWEL_E
    {750, 1550, 2450, 1.0, 0.85},  // VOWEL_A
    {680, 1100, 2450, 1.0, 0.92},  // VOWEL_AR
    {600, 900, 2450, 1.0, 0.5},    // VOWEL_O
    {450, 860, 2300, 1.0, 0.82},   // VOWEL_OR
    {380, 850, 2100, 1.0, 0.6},    // VOWEL_U
    {720, 1240, 2300, 0.85, 0.3},  // VOWEL_XX
    {580, 1380, 2450, 1.0, 0.5},   // VOWEL_X
    {300, 850, 2100, 1.0, 0.6},    // VOWEL_OO
    {410, 810, 2300, 1.0, 0.82},   // VOWEL_OA
    {500, 2250, 2450, .92, 0.25},  // VOWEL_AII
    {380, 850, 2800, 0.85, 0.85},  // VOWEL_UU
    {580, 1380, 2625, 0.0, 0.1},   // VOWEL_N
    {380, 1000, 2450, .92, 0.25},  // VOWEL_UI
    {580, 850, 2100, 0.0, 0.0},    // VOWEL_M
    {880, 2100, 2450, 0.2, 0.0},   // VOWEL_R
    {400, 1000, 2450, 0.3, 0.2},   // VOWEL_TH
}};

[[nodiscard]] bool validVowel(HiliteVowels::Vowel vowel) noexcept
{
    const auto number = static_cast<int>(vowel);
    return number >= 1 && number <= static_cast<int>(vowelFormants.size());
}

// tklib1.c's ranges for VFILT.
constexpr double minimumHalfWidth = .01;
constexpr double maximumHalfWidth = 10.0;
constexpr double minimumSteepness = .1;
constexpr double maximumSteepness = 10;

// define_sensitivity_curve: frequency and multiplier pairs.
constexpr double LOFRQ_BOOST = 2.511;
constexpr double HIFRQ_LOSS = 0.4;
constexpr double LOFRQ_FOOT = 250.0;
constexpr double MIDFRQSHELF_BOT = 2000.0;
constexpr double MIDFRQSHELF_TOP = 3000.0;
constexpr double HIFRQ_FOOT = 4000.0;
constexpr double TOP_OF_SPECTRUM = 96000.0;
constexpr int senslen = 12;
constexpr std::array<double, senslen> sensitivity{
    0.0,
    1.0,
    LOFRQ_FOOT,
    1.0,
    MIDFRQSHELF_BOT,
    1.0 / LOFRQ_BOOST,
    MIDFRQSHELF_TOP,
    1.0 / LOFRQ_BOOST,
    HIFRQ_FOOT,
    HIFRQ_LOSS / LOFRQ_BOOST,
    TOP_OF_SPECTRUM,
    HIFRQ_LOSS / LOFRQ_BOOST,
};

// adjust_for_sensitivity: CDP8's build fuses the multiplier's multiply and
// add. Above TOP_OF_SPECTRUM CDP fails; this holds the last value.
[[nodiscard]] double adjust_for_sensitivity(double amp, double frq) noexcept
{
    int n = 0;
    double multiplier, losensfrq, hisensfrq, losens, hisens, frqfrac, sensstep;
    while (frq > sensitivity[static_cast<std::size_t>(n)])
    {
        n += 2;
        if (n >= senslen)
            return amp * sensitivity[senslen - 1];
    }
    hisensfrq = sensitivity[static_cast<std::size_t>(n)];
    n -= 2;
    if (n < 0)
        return amp * sensitivity[1];
    losensfrq = sensitivity[static_cast<std::size_t>(n)];
    frqfrac = (frq - losensfrq) / (hisensfrq - losensfrq);
    n++;
    losens = sensitivity[static_cast<std::size_t>(n)];
    n += 2;
    hisens = sensitivity[static_cast<std::size_t>(n)];
    sensstep = hisens - losens;
    multiplier = std::fma(sensstep, frqfrac, losens);
    return amp * multiplier;
}

// CDP's max macro.
[[nodiscard]] double cdpmax(double a, double b) noexcept
{
    return (a > b) ? a : b;
}

// isspace in the C locale.
[[nodiscard]] bool space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// The pieces fgets(temp,200,fp) reads: up to 199 characters, through a
// newline, each a C string ending at its first NUL (as SpectralBands.cpp
// reads them).
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

// sscanf(p,"%lf",t) on a word: a sign, then a decimal or (0x) hexadecimal
// number, its longest leading part taken. Nothing where no number starts
// the word or it is out of double's range.
[[nodiscard]] std::optional<double> scanDouble(std::string_view word) noexcept
{
    std::size_t at = 0;
    bool negative = false;
    if (at < word.size() && (word[at] == '+' || word[at] == '-'))
        negative = word[at++] == '-';
    const auto rest = word.substr(at);
    if (!rest.empty() && (rest[0] == '+' || rest[0] == '-'))
        return std::nullopt;
    double value = 0.0;
    if (rest.size() >= 2 && rest[0] == '0' && (rest[1] == 'x' || rest[1] == 'X'))
    {
        // "0x" with no hex digit after it reads as its 0.
        const auto [end, error] = talas::fromChars(rest.data() + 2, rest.data() + rest.size(), value, std::chars_format::hex);
        if (error == std::errc::invalid_argument)
            value = 0.0;
        else if (error != std::errc{})
            return std::nullopt;
    }
    else
    {
        const auto [end, error] = talas::fromChars(rest.data(), rest.data() + rest.size(), value, std::chars_format::general);
        if (error != std::errc{})
            return std::nullopt;
    }
    return negative ? -value : value;
}
}

bool HiliteVowels::prepare(double sampleRate, int points, int overlap, std::span<const Point> vowels,
    double halfWidth, double steepness, double range, double threshold)
{
    if (!within(halfWidth, minimumHalfWidth, maximumHalfWidth) || !within(steepness, minimumSteepness, maximumSteepness)
        || !within(range, 0.0, 1.0) || !within(threshold, 0.0, 1.0) || vowels.empty())
    {
        return false;
    }
    // get_the_vowels: a later first vowel also runs from time zero; the
    // last is held to a time past the end, here never reached.
    std::vector<double> times;
    std::vector<Vowel> table;
    double lasttime = 0.0;
    for (std::size_t index = 0; index < vowels.size(); index++)
    {
        const auto& point = vowels[index];
        if (!std::isfinite(point.time) || !validVowel(point.vowel))
            return false;
        if (index == 0)
        {
            if (point.time < 0.0)
                return false;
            if (point.time > 0.0)
            {
                times.push_back(0.0);
                table.push_back(point.vowel);
            }
        }
        else if (point.time <= lasttime)
            return false;
        times.push_back(point.time);
        table.push_back(point.vowel);
        lasttime = point.time;
    }
    const bool only_one_vowel = times.size() == 1;
    times.push_back(std::numeric_limits<double>::infinity());
    table.push_back(table.back());
    PhaseVocoder vocoder;
    if (!vocoder.prepare(sampleRate, points, overlap))
        return false;
    vocoder_ = std::move(vocoder);
    // The .ana header's arate and CDP's frame time.
    const auto arate = (float)((float)sampleRate / (float)vocoder_.decimation());
    frametime_ = (float)(1.0 / arate);
    halfWidth_ = halfWidth;
    steepness_ = steepness;
    range_ = range;
    threshold_ = threshold;
    itemcnt_ = only_one_vowel ? 0 : static_cast<int>(times.size());
    times_ = std::move(times);
    vowels_ = std::move(table);
    vamp_.assign(vocoder_.frameSize() / 2, 0.0);
    reset();
    return true;
}

void HiliteVowels::reset() noexcept
{
    vocoder_.reset();
    frames_ = 0;
    if (times_.empty())
        return;
    // vowel_filter's start.
    const auto formantsOf = [this](int t) {
        const auto& v = vowelFormants[static_cast<std::size_t>(static_cast<int>(vowels_[static_cast<std::size_t>(t)]) - 1)];
        return Formants{v.formant1, v.formant2, v.formant3, v.f2atten, v.f3atten};
    };
    t_ = 0;
    start_ = formantsOf(t_);
    starttime_ = times_[static_cast<std::size_t>(t_++)];
    end_ = formantsOf(t_);
    endtime_ = times_[static_cast<std::size_t>(t_++)];
    step_ = {end_.formant1 - start_.formant1, end_.formant2 - start_.formant2, end_.formant3 - start_.formant3,
        end_.f2atten - start_.f2atten, end_.f3atten - start_.f3atten};
    timestep_ = endtime_ - starttime_;
    now_ = start_; /* works if only one vowel is entered (for time zero) */
}

bool HiliteVowels::process(std::span<const double> input, std::span<double> output) noexcept
{
    return vocoder_.process(input, output, [this](std::span<float> frame) {
        advance();
        filter(frame);
        frames_++;
    });
}

// vowel_filter's step to the frame's time: CDP8's build fuses each
// interpolation's multiply and add.
void HiliteVowels::advance() noexcept
{
    if (!itemcnt_)
        return;
    const double time = (float)frames_ * frametime_;
    while (time >= endtime_)
    { /* advance along vowels */
        start_ = end_;
        starttime_ = endtime_;
        if (t_ < itemcnt_)
        {
            const auto& v = vowelFormants[static_cast<std::size_t>(static_cast<int>(vowels_[static_cast<std::size_t>(t_)]) - 1)];
            end_ = Formants{v.formant1, v.formant2, v.formant3, v.f2atten, v.f3atten};
            endtime_ = times_[static_cast<std::size_t>(t_++)];
        }
        else
            break;
        step_ = {end_.formant1 - start_.formant1, end_.formant2 - start_.formant2, end_.formant3 - start_.formant3,
            end_.f2atten - start_.f2atten, end_.f3atten - start_.f3atten};
        timestep_ = endtime_ - starttime_;
    }
    if (!flteq(starttime_, endtime_))
    { /* interpolate between vowels : or retain last vowel */
        const double timefrac = (time - starttime_) / timestep_;
        now_.formant1 = std::fma(step_.formant1, timefrac, start_.formant1);
        now_.formant2 = std::fma(step_.formant2, timefrac, start_.formant2);
        now_.formant3 = std::fma(step_.formant3, timefrac, start_.formant3);
        now_.f2atten = std::fma(step_.f2atten, timefrac, start_.f2atten);
        now_.f3atten = std::fma(step_.f3atten, timefrac, start_.f3atten);
    }
}

// do_vowel_filter: CDP8's build leaves these statements unfused.
void HiliteVowels::filter(std::span<float> frame) noexcept
{
    float* flbufptr0 = frame.data();
    double* vamp = vamp_.data();
    const int clength = static_cast<int>(frame.size() / 2);
    const double formant1 = now_.formant1;
    const double formant2 = now_.formant2;
    const double formant3 = now_.formant3;
    const double f2atten = now_.f2atten;
    const double f3atten = now_.f3atten;
    double hfwidth1, hfwidth2, hfwidth3 = 0.0, lolim1, lolim2, lolim3 = 0.0, hilim1, hilim2, hilim3 = 0.0;
    double thisfrq, amp, amp2 = 0.0, amp3 = 0.0;
    int cc, vc;
    int overlapped_formants12 = 0, overlapped_formants23 = 0, overlapped_formants13 = 0;
    int is_overlap12, is_overlap23, is_overlap13;
    double pre_totamp, post_totamp, maxamp, maxvamp, ampscale;
    int is_third_formant = 0;
    const double signal_base = 1.0 - range_;

    if (formant3 > 0.0)
        is_third_formant = 1;
    hfwidth1 = formant1 * halfWidth_; /* set limits of formant bands */
    lolim1 = formant1 - hfwidth1;
    hilim1 = formant1 + hfwidth1;
    hfwidth2 = formant2 * halfWidth_;
    lolim2 = formant2 - hfwidth2;
    hilim2 = formant2 + hfwidth2;
    if (is_third_formant)
    {
        hfwidth3 = formant3 * halfWidth_;
        lolim3 = formant3 - hfwidth3;
        hilim3 = formant3 + hfwidth3;
    }
    if (hilim1 > lolim2) /* deal with overlapping formants */
        overlapped_formants12 = 1;
    if (is_third_formant)
    {
        if (hilim2 > lolim3)
            overlapped_formants23 = 1;
        if (hilim1 > lolim3)
            overlapped_formants13 = 1;
    }
    maxamp = 0.0;
    pre_totamp = 0.0;
    for (cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        pre_totamp += flbufptr0[vc];
        maxamp = cdpmax(flbufptr0[vc], maxamp);
    }
    maxvamp = 0.0;
    /* find the formant envelope amplitude at frequency of every window */
    if (!is_third_formant)
        hilim3 = hilim2;
    for (cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        thisfrq = flbufptr0[vc + 1];
        amp = 0.0;
        is_overlap12 = 0;
        is_overlap23 = 0;
        is_overlap13 = 0;
        if (thisfrq < lolim1 || thisfrq > hilim3)
            ;
        else if ((thisfrq > lolim1) && (thisfrq < hilim1))
        {
            if (overlapped_formants12 && (thisfrq > lolim2))
            {
                is_overlap12 = 1;
                if (thisfrq >= formant2)
                    amp2 = (hilim2 - thisfrq) / hfwidth2;
                else
                    amp2 = (thisfrq - lolim2) / hfwidth2;
                amp2 *= f2atten;
            }
            if (is_third_formant)
            {
                if (overlapped_formants13 && (thisfrq > lolim3))
                {
                    is_overlap13 = 1;
                    if (thisfrq >= formant3)
                        amp3 = (hilim3 - thisfrq) / hfwidth3;
                    else
                        amp3 = (thisfrq - lolim3) / hfwidth3;
                    amp3 *= f3atten;
                }
            }
            if (thisfrq >= formant1)
                amp = (hilim1 - thisfrq) / hfwidth1;
            else
                amp = (thisfrq - lolim1) / hfwidth1;
            if (is_overlap12)
                amp = cdpmax(amp, amp2);
            if (is_third_formant && is_overlap13)
                amp = cdpmax(amp, amp3);
        }
        else if ((thisfrq > lolim2) && (thisfrq < hilim2))
        {
            if (is_third_formant && overlapped_formants23 && (thisfrq > lolim3))
            {
                is_overlap23 = 1;
                if (thisfrq >= formant3)
                    amp3 = (hilim3 - thisfrq) / hfwidth3;
                else
                    amp3 = (thisfrq - lolim3) / hfwidth3;
                amp3 *= f3atten;
            }
            if (thisfrq >= formant2)
                amp = (hilim2 - thisfrq) / hfwidth2;
            else
                amp = (thisfrq - lolim2) / hfwidth2;
            amp *= f2atten;
            if (is_third_formant && is_overlap23)
                amp = cdpmax(amp, amp3);
        }
        else if (is_third_formant && (thisfrq > lolim3))
        {
            if (thisfrq >= formant3)
                amp = (hilim3 - thisfrq) / hfwidth3;
            else
                amp = (thisfrq - lolim3) / hfwidth3;
            amp *= f3atten;
        }
        amp = std::pow(amp, steepness_);
        amp *= range_;
        amp += signal_base;
        amp = adjust_for_sensitivity(amp, thisfrq);
        vamp[cc] = amp;
        maxvamp = cdpmax(maxvamp, vamp[cc]);
    }
    /* set scaling factor to allow for max level of input window */
    if (flteq(maxvamp, 0.0))
        ampscale = 0.0;
    else
        ampscale = (maxamp / maxvamp);
    post_totamp = 0.0;
    for (cc = 0, vc = 0; cc < clength; cc++, vc += 2)
    {
        vamp[cc] *= ampscale;
        /* force window to formant level ONLY if existing val > a given proportion of formant level */
        if ((double)flbufptr0[vc] > vamp[cc] * threshold_)
            flbufptr0[vc] = (float)vamp[cc];
        post_totamp += flbufptr0[vc];
    }
    /* normalise to overall level of original window */
    normalise(frame, pre_totamp, post_totamp);
}

std::optional<HiliteVowels::Vowel> hiliteVowel(std::string_view str) noexcept
{
    using V = HiliteVowels::Vowel;
    if (str == "ee")
        return V::Ee;
    if (str == "i")
        return V::I;
    if (str == "ai")
        return V::Ai;
    if (str == "aii")
        return V::Aii;
    if (str == "e")
        return V::E;
    if (str == "a")
        return V::A;
    if (str == "ar")
        return V::Ar;
    if (str == "o")
        return V::O;
    if (str == "oo")
        return V::Oo;
    if (str == "oa")
        return V::Oa;
    if (str == "or")
        return V::Or;
    if (str == "u")
        return V::U;
    if (str == "ui")
        return V::Ui;
    if (str == "uu")
        return V::Uu;
    if (str == "xx")
        return V::Xx;
    if (str == "x")
        return V::X;
    if (str == "n")
        return V::N;
    if (str == "m")
        return V::M;
    if (str == "r")
        return V::R;
    if (str == "th")
        return V::Th;
    return std::nullopt;
}

// get_the_vowels, from its file.
std::optional<std::vector<HiliteVowels::Point>> readHiliteVowels(std::string_view text)
{
    std::vector<HiliteVowels::Point> points;
    bool istime = true;
    bool ok = true;
    double time = 0.0, lasttime = 0.0;
    forEachPiece(text, [&](std::string_view temp) {
        if (!temp.empty() && temp[0] == ';') //  Allow comments in file
            return true;
        std::size_t q = 0;
        while (const auto p = wordWithin(temp, q))
        {
            if (istime)
            {
                const auto t = scanDouble(*p);
                if (!t || !std::isfinite(*t))
                    return ok = false;
                if (points.empty())
                {
                    if (*t < 0.0)
                        return ok = false;
                }
                else if (*t <= lasttime)
                    return ok = false;
                time = *t;
                lasttime = *t;
            }
            else
            {
                const auto v = hiliteVowel(*p);
                if (!v)
                    return ok = false;
                points.push_back({time, *v});
            }
            istime = !istime;
        }
        return true;
    });
    if (!ok || points.empty() || !istime)
        return std::nullopt;
    return points;
}
}
