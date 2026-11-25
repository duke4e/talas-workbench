/*
 * Derived from the CDP System, Release 8: dev/externals/reverb/rmverb.cpp,
 * reverberator.cpp and reflect.cpp
 * Copyright (c) 1983-2013 Richard Dobson and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/RoomReverb.h"

#include "talas/core/FromChars.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <limits>

// CDP8's build (Apple clang, -O2, contraction on) fuses each multiply-add
// within one expression; the std::fma calls below stand where its rmverb
// fuses, and nowhere else, so the port does not depend on this compiler's
// contraction.
namespace talas::cdp
{
namespace
{
// reverberator.cpp's constants, ROOT2O2's misprinted digit included.
constexpr double pi = 3.141592653589793238462643;
constexpr double twoPi = 2.0 * pi;
constexpr double root2Over2 = 0.7071067811965475244;
constexpr double spn = 1.65436e-24;
// tapdelay's SAFETY and the allpass's VMODMAX.
constexpr double safetyFrames = 2.0;
constexpr double modulationMaximum = 0.5;

struct ReflectionTap
{
    double pos;
    double val;
};

// reflect.cpp's rooms, as rmverb.cpp includes them.
constexpr ReflectionTap smallTaps[] = {
    {0.0082, 0.500000}, {0.0121, 0.204198}, {0.0134, 0.169094}, {0.0145, 0.143638},
    {0.0170, 0.093391}, {0.0180, 0.084478}, {0.0200, 0.067530}, {0.0219, 0.063096},
    {0.0226, 0.053176}, {0.0233, 0.044833}, {0.0237, 0.048586}, {0.0243, 0.046058},
    {0.0250, 0.043713}, {0.0256, 0.037388}, {0.0261, 0.044475}, {0.0265, 0.034818},
    {0.0271, 0.033360}, {0.0276, 0.035772}, {0.0281, 0.034382}, {0.0285, 0.030051},
    {0.0287, 0.033059}, {0.0301, 0.027091}, {0.0304, 0.026513}, {0.0306, 0.026200},
    {0.0309, 0.023001}, {0.0319, 0.024114}, {0.0326, 0.020657}, {0.0335, 0.021782},
    {0.0340, 0.019019}, {0.0356, 0.017387}, {0.0398, 0.017155}, {0.0408, 0.014690},
    {0.0412, 0.014424}, {0.0416, 0.014160}, {0.0425, 0.012175}, {0.0429, 0.011971},
    {0.0439, 0.011468}, {0.0451, 0.010854}};
constexpr ReflectionTap mediumTaps[] = {
    {0.0332, 0.993303}, {0.0353, 0.834879}, {0.0377, 0.729745}, {0.0447, 0.494138},
    {0.0548, 0.345328}, {0.0561, 0.313134}, {0.0570, 0.319440}, {0.0577, 0.296282},
    {0.0583, 0.290651}, {0.0598, 0.276075}, {0.0615, 0.274630}, {0.0625, 0.240116},
    {0.0627, 0.251368}, {0.0641, 0.240392}, {0.0644, 0.226004}, {0.0684, 0.200374},
    {0.0690, 0.218340}, {0.0700, 0.201354}, {0.0713, 0.194250}, {0.0718, 0.191280},
    {0.0728, 0.176802}, {0.0740, 0.171020}, {0.0752, 0.165815}, {0.0770, 0.166316},
    {0.0778, 0.147079}, {0.0780, 0.154272}, {0.0791, 0.149852}, {0.0816, 0.148066},
    {0.0825, 0.137700}, {0.0826, 0.130403}, {0.0836, 0.134167}, {0.0862, 0.132653},
    {0.0870, 0.117789}, {0.0871, 0.123637}, {0.0881, 0.120782}, {0.0913, 0.106847},
    {0.0929, 0.114356}, {0.0937, 0.106863}, {0.0946, 0.104723}, {0.0975, 0.103831}};
constexpr ReflectionTap largeTaps[] = {
    {0.0729, 0.500000}, {0.0758, 0.439456}, {0.0975, 0.265770}, {0.0997, 0.241550},
    {0.1151, 0.190794}, {0.1162, 0.187246}, {0.1180, 0.172387}, {0.1247, 0.154486},
    {0.1320, 0.137708}, {0.1330, 0.135754}, {0.1344, 0.139864}, {0.1346, 0.125902},
    {0.1360, 0.129780}, {0.1404, 0.115604}, {0.1449, 0.114371}, {0.1464, 0.112074},
    {0.1477, 0.115801}, {0.1492, 0.107884}, {0.1540, 0.096167}};

// rmverb.cpp's NESTDATA presets: each nested allpass's outer time and gain,
// then its two inner allpasses' times and gains (a zero gain leaves the
// allpass out).
struct NestedPreset
{
    double time1;
    double time2;
    double time3;
    double gain1;
    double gain2;
    double gain3;
};
constexpr NestedPreset smallPresets[] = {
    {0.0047, 0.022, 0.0083, 0.3, 0.4, 0.6},
    {0.036, 0.03, 0.0, 0.1, 0.4, 0.0}};
constexpr NestedPreset mediumPresets[] = {
    {0.035, 0.0083, 0.022, 0.3, 0.7, 0.5},
    {0.039, 0.0098, 0.0, 0.3, 0.6, 0.0}};
constexpr NestedPreset largePresets[] = {
    {0.03, 0.062, 0.022, 0.5, 0.25, 0.0},
    {0.018, 0.076, 0.03, 0.5, 0.25, 0.25}};

// prime, from Csound's nreverb.
bool prime(int val) noexcept
{
    const auto last = static_cast<int>(std::sqrt(static_cast<double>(val)));
    for (int i = 3; i <= last; i += 2)
    {
        if ((val % i) == 0)
            return false;
    }
    return true;
}

unsigned int toPrime(double dur, double sr) noexcept
{
    auto time = static_cast<unsigned int>(dur * sr);
    if (time % 2 == 0)
        time += 1;
    while (!prime(static_cast<int>(time)))
        time += 2;
    return time;
}

[[nodiscard]] bool space(char character) noexcept
{
    return character == ' ' || character == '\t' || character == '\n' || character == '\r'
        || character == '\v' || character == '\f';
}

// Up to two numbers as sscanf's "%lf%lf" reads them, stopping at the first
// that does not parse. Returns how many it read.
int scanNumbers(std::string_view line, double (&values)[2]) noexcept
{
    int count = 0;
    std::size_t position = 0;
    while (count < 2)
    {
        while (position < line.size() && space(line[position]))
            ++position;
        auto negative = false;
        if (position < line.size() && (line[position] == '+' || line[position] == '-'))
        {
            negative = line[position] == '-';
            ++position;
        }
        auto format = std::chars_format::general;
        if (position + 1 < line.size() && line[position] == '0'
            && (line[position + 1] == 'x' || line[position + 1] == 'X'))
        {
            format = std::chars_format::hex;
            position += 2;
        }
        if (position < line.size() && (line[position] == '+' || line[position] == '-'))
            break;
        double value = 0.0;
        const auto* first = line.data() + position;
        const auto* last = line.data() + line.size();
        const auto [end, error] = talas::fromChars(first, last, value, format);
        if (error != std::errc{} || end == first)
        {
            // strtod reads "0x" without hex digits as the zero before it.
            if (format == std::chars_format::hex)
            {
                values[count++] = negative ? -0.0 : 0.0;
                position -= 1;
                continue;
            }
            break;
        }
        values[count++] = negative ? -value : value;
        position = static_cast<std::size_t>(end - line.data());
    }
    return count;
}

// tonecontrol::lshelve and hshelve: a shelf of boost dB at the normalised
// frequency freq.
void shelve(
    double freq, double boost, bool high,
    double& a0, double& a1, double& a2, double& b1, double& b2) noexcept
{
    double a, A, F, tmp, b0, recipb0, asq, F2, gamma2, siggam2, gam2p1;
    double gamman, gammad, ta0, ta1, ta2, tb0, tb1, tb2, aa1, ab1;

    a = std::tan(pi * (freq - 0.25));
    asq = a * a;
    A = std::pow(10.0, boost / 20.0);
    if ((boost < 6.0) && (boost > -6.0))
        F = std::sqrt(A);
    else if (A > 1.0)
        F = A / std::sqrt(2.0);
    else
        F = A * std::sqrt(2.0);

    F2 = F * F;
    tmp = std::fma(A, A, -F2);
    if (std::fabs(tmp) <= spn)
        gammad = 1;
    else
        gammad = std::pow((F2 - 1) / tmp, 0.25);
    gamman = std::sqrt(A) * gammad;

    gamma2 = gamman * gamman;
    gam2p1 = 1 + gamma2;
    siggam2 = 2 * root2Over2 * gamman;
    ta0 = gam2p1 + siggam2;
    ta1 = -2 * (1 - gamma2);
    ta2 = gam2p1 - siggam2;
    if (high)
        ta1 = -ta1;

    gamma2 = gammad * gammad;
    gam2p1 = 1 + gamma2;
    siggam2 = 2 * root2Over2 * gammad;
    tb0 = gam2p1 + siggam2;
    tb1 = -2 * (1 - gamma2);
    tb2 = gam2p1 - siggam2;
    if (high)
        tb1 = -tb1;

    aa1 = a * ta1;
    a0 = std::fma(asq, ta2, ta0 + aa1);
    a1 = std::fma(2 * a, ta0 + ta2, (1 + asq) * ta1);
    a2 = std::fma(asq, ta0, aa1) + ta2;

    ab1 = a * tb1;
    b0 = std::fma(asq, tb2, tb0 + ab1);
    b1 = std::fma(2 * a, tb0 + tb2, (1 + asq) * tb1);
    b2 = std::fma(asq, tb0, ab1) + tb2;

    recipb0 = 1 / b0;
    a0 *= recipb0;
    a1 *= recipb0;
    a2 *= recipb0;
    b1 *= recipb0;
    b2 *= recipb0;
}
}

std::optional<std::vector<RoomReflection>> readRoomReflections(std::string_view text, double sampleRate)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return std::nullopt;
    std::vector<RoomReflection> taps;
    double time = 0.0, current_time = 0.0, val = 0.0;
    const auto sample_duration = 1.0 / sampleRate;
    std::size_t begin = 0;
    while (begin < text.size())
    {
        // fgets(line, 256, fp): up to 255 characters, through a newline.
        auto end = begin;
        while (end < text.size() && end - begin < 255)
        {
            if (text[end++] == '\n')
                break;
        }
        const auto line = text.substr(begin, end - begin);
        begin = end;
        if (line.front() == '\n' || line.front() == ';')
            continue;
        double values[2] = {time, val};
        if (scanNumbers(line, values) < 2)
            return std::nullopt;
        time = values[0];
        val = values[1];
        if (!std::isfinite(time) || time < 0.0)
            return std::nullopt;
        if (time < sample_duration)
            time = 0.0;
        if (current_time != 0.0 && time == current_time)
            continue;
        if (time < current_time)
            return std::nullopt;
        current_time = time;
        if (!(val > 0.0 && val <= 1.0))
            return std::nullopt;
        if (taps.size() == RoomReverb::maximumReflections)
            return std::nullopt;
        taps.push_back({time, val});
    }
    if (taps.empty())
        return std::nullopt;
    return taps;
}

bool RoomReverb::prepare(double sampleRate, const RoomReverbSettings& settings)
{
    const auto& s = settings;
    const auto roomNumber = static_cast<int>(s.room);
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || 0.2 * sampleRate >= static_cast<double>(INT_MAX)
        || roomNumber < 1 || roomNumber > 3)
    {
        return false;
    }
    // rmverb.cpp's argument checks.
    const auto nyquist = sampleRate / 2.0;
    const auto pseudo_nyquist = nyquist * 0.7;
    const auto& reflections = s.reflections;
    if (!std::isfinite(s.reverbGain) || s.reverbGain <= 0.0 || s.reverbGain > 1.0
        || !std::isfinite(s.mix) || s.mix < 0.0 || s.mix > 1.0
        || !std::isfinite(s.feedback) || s.feedback < 0.0 || s.feedback > 1.0
        || !std::isfinite(s.absorbHz) || s.absorbHz < 0.0 || s.absorbHz > pseudo_nyquist
        || !std::isfinite(s.lowpassHz) || s.lowpassHz < 0.0 || s.lowpassHz > pseudo_nyquist
        || std::isnan(s.predelayMilliseconds) || s.predelayMilliseconds > maximumPredelayMilliseconds
        || !std::isfinite(s.lowCutHz) || s.lowCutHz < 0.0
        || !std::isfinite(s.highCutHz) || s.highCutHz < 0.0
        || (s.lowCutHz > 0.0 && s.highCutHz > 0.0 && s.highCutHz <= s.lowCutHz)
        || reflections.size() > maximumReflections
        || !std::ranges::all_of(reflections, [](const RoomReflection& tap) {
               return std::isfinite(tap.time) && tap.time >= 0.0
                   && tap.time <= maximumReflectionSeconds
                   && std::isfinite(tap.amplitude) && tap.amplitude > 0.0 && tap.amplitude <= 1.0;
           })
        || !std::ranges::is_sorted(reflections, {}, &RoomReflection::time))
    {
        return false;
    }

    RoomReverb next;
    const auto sr = sampleRate;

    // tonecontrol(freq, -12.0, LOW_SHELVE or HIGH_SHELVE, sr).
    const auto toneControl = [&](ToneControl& tc, double frq, bool high) {
        tc.active = true;
        tc.boost = -12.0;
        shelve(frq / sr, tc.boost, high, tc.a0, tc.a1, tc.a2, tc.b1, tc.b2);
        return std::isfinite(tc.a0) && std::isfinite(tc.a1) && std::isfinite(tc.a2)
            && std::isfinite(tc.b1) && std::isfinite(tc.b2);
    };
    if (s.lowCutHz > 0.0 && !toneControl(next.lowCut_, s.lowCutHz, false))
        return false;
    if (s.highCutHz > 0.0 && !toneControl(next.highCut_, s.highCutHz, true))
        return false;

    // onepole(freq, srate, LOW_PASS); a zero frequency passes the input.
    const auto onePole = [&](OnePole& filter, double freq) {
        if (freq == 0.0)
        {
            filter.a1 = 1.0;
            filter.a2 = 0.0;
        }
        else
        {
            const auto b = 2.0 - std::cos(twoPi * (freq / sr));
            filter.a2 = std::sqrt(std::fma(b, b, -1.0)) - b;
            filter.a1 = 1.0 + filter.a2;
        }
        filter.lastOut = 0.0;
    };
    // delay::create.
    const auto delay = [](Delay& line, unsigned int length) {
        line.buffer.assign(length, 0.0);
        line.ptr = 0;
        return length > 0;
    };
    // allpassfilter(sr, time, ingain, predelay) and its create.
    const auto allpass = [&](Allpass& filter, unsigned int time, double gain, unsigned int predelay) {
        filter.gain = gain;
        filter.buffer.assign(time + static_cast<unsigned int>(time * modulationMaximum), 0.0);
        filter.preDelayed = predelay > 0;
        if (filter.preDelayed && !delay(filter.preDelay, predelay))
            return false;
        return gain >= -1.0 && gain <= 1.0 && !filter.buffer.empty();
    };
    // nested_allpass(sr, lpfreq, time1, time2, time3, gain1, gain2, gain3)
    // and its create, which leaves damping off.
    const auto absorb = s.absorbHz;
    const auto nested = [&](NestedAllpass& filter, const NestedPreset& p) {
        const auto outer_time = toPrime(p.time1, sr);
        auto ap1_length = toPrime(p.time2, sr);
        auto ap2_length = toPrime(p.time3, sr);
        if (p.gain3 == 0.0)
            ap2_length = 0;
        if (p.gain1 < -1.0 || p.gain1 > 1.0 || outer_time <= 0)
            return false;
        filter.outerGain = p.gain1;
        filter.buffer.assign(outer_time, 0.0);
        filter.hasFirst = ap1_length > 0 && p.gain2 > 0.0;
        if (filter.hasFirst && !allpass(filter.first, ap1_length, p.gain2, 0))
            return false;
        filter.hasSecond = ap2_length > 0 && p.gain3 > 0.0;
        if (filter.hasSecond && !allpass(filter.second, ap2_length, p.gain3, 0))
            return false;
        // nested_allpass::tick runs the second only after the first.
        if (filter.hasSecond && !filter.hasFirst)
            return false;
        onePole(filter.lowpass, absorb);
        filter.damping = false;
        return true;
    };

    // The room's diffuser, with set_damping(double_damping) before create.
    next.room_ = s.room;
    auto damping = s.doubleDamping;
    std::span<const ReflectionTap> room;
    switch (s.room)
    {
    case RoomSize::small:
        // small_diffuser((unsigned int)(0.024 * sr), ...): it keeps the
        // damping even with no absorption.
        if (!nested(next.nested_[0], smallPresets[0]))
            return false;
        next.secondNested_ = smallPresets[1].gain1 != 0.0;
        if (next.secondNested_ && !nested(next.nested_[1], smallPresets[1]))
            return false;
        next.preDelayed_ = static_cast<unsigned int>(0.024 * sr) > 0;
        if (next.preDelayed_ && !delay(next.preDelay_, static_cast<unsigned int>(0.024 * sr)))
            return false;
        room = smallTaps;
        break;
    case RoomSize::medium:
        // medium_diffuser(0.108, ...).
        if (!nested(next.nested_[0], mediumPresets[0]) || !nested(next.nested_[1], mediumPresets[1]))
            return false;
        next.secondNested_ = true;
        if (!allpass(next.innerAllpass1_, toPrime(0.03, sr), 0.5, toPrime(0.005, sr))
            || !delay(next.postDelay_, toPrime(0.108, sr))
            || !delay(next.delay1_, toPrime(0.067, sr))
            || !delay(next.delay2_, toPrime(0.015, sr)))
        {
            return false;
        }
        if (absorb == 0.0)
            damping = false;
        room = mediumTaps;
        break;
    case RoomSize::large:
        if (!nested(next.nested_[0], largePresets[0]) || !nested(next.nested_[1], largePresets[1]))
            return false;
        next.secondNested_ = true;
        if (!allpass(next.innerAllpass1_, toPrime(0.008, sr), 0.3, 0)
            || !allpass(next.innerAllpass2_, toPrime(0.012, sr), 0.3, 0)
            || !delay(next.delay1_, static_cast<unsigned int>(0.004 * sr))
            || !delay(next.delay2_, static_cast<unsigned int>(0.017 * sr))
            || !delay(next.delay3_, static_cast<unsigned int>(0.031 * sr))
            || !delay(next.delay4_, static_cast<unsigned int>(0.003 * sr)))
        {
            return false;
        }
        if (absorb == 0.0)
            damping = false;
        room = largeTaps;
        break;
    }
    next.nested_[0].damping = damping;
    next.nested_[1].damping = damping;
    onePole(next.feedbackLowpass_, absorb);
    next.feedbackGain_ = s.feedback;

    if (s.lowpassHz > 0.0)
    {
        next.lowpassActive_ = true;
        onePole(next.lowpass_, s.lowpassHz);
    }

    // The early reflections: the room's, or the user's, all moved so the
    // first falls on a given predelay.
    std::vector<ReflectionTap> taps;
    if (reflections.empty())
        taps.assign(room.begin(), room.end());
    else
    {
        for (const auto& reflection : reflections)
            taps.push_back({reflection.time, reflection.amplitude});
    }
    if (s.predelayMilliseconds >= 0.0)
    {
        const auto predelay = s.predelayMilliseconds * 0.001;
        const auto current_delay = taps[0].pos;
        const auto adjust = predelay - current_delay;
        for (auto& tap : taps)
            tap.pos += adjust;
    }
    // tapdelay::create.
    if (std::fma(taps.back().pos, sr, safetyFrames) >= static_cast<double>(INT_MAX))
        return false;
    auto& line = next.earlies_;
    const auto buflen = static_cast<unsigned int>(std::fma(taps.back().pos, sr, safetyFrames));
    line.buffer.assign(buflen, 0.0);
    line.pointers.resize(taps.size());
    line.coefficients.resize(taps.size());
    double sum = 0.0;
    for (std::size_t index = 0; index < taps.size(); ++index)
    {
        if (taps[index].pos == 0.0)
            line.pointers[index] = 0;
        else
            line.pointers[index] = buflen - static_cast<unsigned int>(static_cast<int>(taps[index].pos * sr));
        // A reflection within a sample of the direct sound but not on it
        // would start CDP's tap past its line.
        if (line.pointers[index] >= buflen)
            return false;
        line.coefficients[index] = taps[index].val;
        sum += std::fabs(line.coefficients[index]);
    }
    if (sum == 0.0)
        return false;
    line.startPointers = line.pointers;
    line.reader = 0;
    line.oneOverTaps = 1.0 / static_cast<double>(taps.size());

    // The final allpass pairs: 0.02 and 0.014 s on the left, 0.025 and
    // 0.0145 s on the right.
    if (!allpass(next.leftAllpasses_[0], toPrime(0.02, sr), -0.6, 0)
        || !allpass(next.leftAllpasses_[1], toPrime(0.014, sr), 0.4, 0)
        || !allpass(next.rightAllpasses_[0], toPrime(0.025, sr), -0.6, 0)
        || !allpass(next.rightAllpasses_[1], toPrime(0.0145, sr), 0.4, 0))
    {
        return false;
    }

    next.diffGain_ = s.reverbGain;
    next.dryGain_ = s.mix;
    // wet_gain = 1.0f - dry_gain, doubled.
    auto wet_gain = 1.0F - s.mix;
    wet_gain *= 2.0;
    next.wetGain_ = wet_gain;
    next.mono_ = s.monoOutput;
    next.prepared_ = true;
    *this = std::move(next);
    return true;
}

void RoomReverb::reset() noexcept
{
    const auto clear = [](Allpass& filter) {
        std::ranges::fill(filter.buffer, 0.0);
        filter.reader = filter.writer = 0;
        std::ranges::fill(filter.preDelay.buffer, 0.0);
        filter.preDelay.ptr = 0;
    };
    for (auto& filter : nested_)
    {
        std::ranges::fill(filter.buffer, 0.0);
        filter.reader = filter.writer = 0;
        clear(filter.first);
        clear(filter.second);
        filter.lowpass.lastOut = 0.0;
    }
    for (auto* filter : {&innerAllpass1_, &innerAllpass2_, &leftAllpasses_[0], &leftAllpasses_[1],
             &rightAllpasses_[0], &rightAllpasses_[1]})
    {
        clear(*filter);
    }
    for (auto* line : {&preDelay_, &postDelay_, &delay1_, &delay2_, &delay3_, &delay4_})
    {
        std::ranges::fill(line->buffer, 0.0);
        line->ptr = 0;
    }
    feedbackLowpass_.lastOut = 0.0;
    out1_ = out2_ = out3_ = 0.0;
    std::ranges::fill(earlies_.buffer, 0.0);
    std::ranges::copy(earlies_.startPointers, earlies_.pointers.begin());
    earlies_.reader = 0;
    lowCut_.x1 = lowCut_.x2 = lowCut_.y1 = lowCut_.y2 = 0.0;
    highCut_.x1 = highCut_.x2 = highCut_.y1 = highCut_.y2 = 0.0;
    lowpass_.lastOut = 0.0;
}

// delay::tick.
double RoomReverb::tick(Delay& line, double insam) noexcept
{
    double val;
    val = line.buffer[line.ptr];
    line.buffer[line.ptr++] = insam;
    if (line.ptr == line.buffer.size())
        line.ptr = 0;
    return val * 1.0;
}

// allpassfilter::tick.
double RoomReverb::tick(Allpass& filter, double insamp) noexcept
{
    double output, input;
    input = insamp;
    output = (-filter.gain) * input;
    output += filter.buffer[filter.reader++];
    input = std::fma(filter.gain, output, input);
    if (filter.preDelayed)
        filter.buffer[filter.writer++] = tick(filter.preDelay, input);
    else
        filter.buffer[filter.writer++] = input;
    if (filter.reader == filter.buffer.size())
        filter.reader = 0;
    if (filter.writer == filter.buffer.size())
        filter.writer = 0;
    return output;
}

// onepole::tick.
double RoomReverb::tick(OnePole& filter, double input) noexcept
{
    double output;
    output = std::fma(filter.a1, input, -(filter.a2 * filter.lastOut));
    filter.lastOut = output;
    return output;
}

// nested_allpass::tick.
double RoomReverb::tick(NestedAllpass& filter, double insam) noexcept
{
    double output, input, nest_out;
    input = insam;
    output = (-filter.outerGain) * input;
    output += filter.buffer[filter.reader++];
    input = std::fma(filter.outerGain, output, input);
    if (!filter.hasSecond)
    {
        if (filter.hasFirst)
            nest_out = tick(filter.first, input);
        else
            nest_out = input;
    }
    else
        nest_out = tick(filter.second, tick(filter.first, input));
    if (filter.damping)
        filter.buffer[filter.writer++] = tick(filter.lowpass, nest_out);
    else
        filter.buffer[filter.writer++] = nest_out;
    if (filter.writer == filter.buffer.size())
        filter.writer = 0;
    if (filter.reader == filter.buffer.size())
        filter.reader = 0;
    return output;
}

// tonecontrol::tick.
double RoomReverb::tick(ToneControl& tc, double input) noexcept
{
    double out;
    if (tc.boost == 0.0)
        return input;
    out = std::fma(-tc.b2, tc.y2,
        std::fma(-tc.b1, tc.y1, std::fma(tc.a2, tc.x2, std::fma(tc.a0, input, tc.a1 * tc.x1))));
    tc.x2 = tc.x1;
    tc.x1 = input;
    tc.y2 = tc.y1;
    tc.y1 = out;
    return out;
}

// tapdelay::tick.
double RoomReverb::tick(TapLine& line, double insam) noexcept
{
    double val = 0.0;
    line.buffer[line.reader++] = insam;
    if (line.reader == line.buffer.size())
        line.reader = 0;
    for (std::size_t i = 0; i < line.pointers.size(); ++i)
    {
        val = std::fma(line.buffer[line.pointers[i]++], line.coefficients[i], val);
        if (line.pointers[i] == line.buffer.size())
            line.pointers[i] = 0;
    }
    val *= line.oneOverTaps;
    return val * 1.0;
}

// small_diffuser::tick, medium_diffuser::tick and large_diffuser::tick.
double RoomReverb::diffuse(double insam) noexcept
{
    double filter_out;
    double output;
    switch (room_)
    {
    case RoomSize::small:
    {
        double lp_in;
        double ip;
        if (secondNested_)
            lp_in = out2_;
        else
            lp_in = out1_;
        filter_out = tick(feedbackLowpass_, lp_in);
        ip = std::fma(feedbackGain_, filter_out, insam);
        if (preDelayed_)
            ip = tick(preDelay_, ip);
        out1_ = tick(nested_[0], ip);
        if (secondNested_)
        {
            out2_ = tick(nested_[1], out1_);
            output = (out1_ + out2_) * 0.5;
        }
        else
            output = out1_;
        return output;
    }
    case RoomSize::medium:
        filter_out = feedbackGain_ * tick(feedbackLowpass_, tick(postDelay_, out3_));
        out1_ = tick(nested_[0], insam + filter_out);
        out2_ = tick(delay1_, tick(innerAllpass1_, out1_));
        out3_ = tick(nested_[1], std::fma(feedbackGain_, tick(delay2_, out2_), insam));
        output = 0.5 * (out1_ + out2_ + out3_);
        return output;
    case RoomSize::large:
        filter_out = feedbackGain_ * tick(feedbackLowpass_, out3_);
        out1_ = tick(delay1_, tick(innerAllpass2_, tick(innerAllpass1_, insam + filter_out)));
        out2_ = tick(delay3_, tick(nested_[0], tick(delay2_, out1_)));
        out3_ = tick(nested_[1], tick(delay4_, out2_));
        output = std::fma(out3_, 0.14, std::fma(out1_, 0.34, out2_ * 0.14));
        return output;
    }
    return 0.0;
}

bool RoomReverb::process(
    std::span<const double> input,
    std::span<double> left,
    std::span<double> right) noexcept
{
    if (!prepared_ || input.size() != left.size() || input.size() != right.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    const auto diff_gain = diffGain_;
    const auto dry_gain = dryGain_;
    const auto wet_gain = wetGain_;
    // rmverb.cpp's main loop for a mono input, reading and writing floats.
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        double l_ip, out, l_direct, r_direct, mono_direct, earlies, l_out, r_out;
        l_ip = static_cast<double>(static_cast<float>(input[frame]));
        if (lowCut_.active)
            l_ip = tick(lowCut_, l_ip);
        if (highCut_.active)
            l_ip = tick(highCut_, l_ip);
        l_direct = l_ip;
        mono_direct = l_direct;
        r_direct = l_direct;
        if (lowpassActive_)
            mono_direct = tick(lowpass_, mono_direct);
        earlies = tick(earlies_, mono_direct);
        out = std::fma(diffuse(earlies + mono_direct) * diff_gain, 0.707, earlies);
        l_out = wet_gain * tick(leftAllpasses_[1], tick(leftAllpasses_[0], out));
        if (mono_)
            l_out = std::fma(mono_direct, dry_gain, l_out);
        else
            l_out = std::fma(l_direct, dry_gain, l_out);
        left[frame] = static_cast<double>(static_cast<float>(l_out));
        if (mono_)
        {
            right[frame] = left[frame];
            continue;
        }
        r_out = wet_gain * tick(rightAllpasses_[1], tick(rightAllpasses_[0], out));
        r_out = std::fma(r_direct, dry_gain, r_out);
        right[frame] = static_cast<double>(static_cast<float>(r_out));
    }
    return true;
}
}
