/*
 * Derived from the CDP System, Release 8: dev/externals/reverb/reverb.cpp,
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
#include "talas/cdp/Reverb.h"

#include <algorithm>
#include <cmath>
#include <limits>

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

// reflect.cpp's rooms.
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

// reverb.cpp's MOORERDATA: comb and allpass loop times.
constexpr double combTimes[] = {0.050, 0.056, 0.061, 0.068, 0.072, 0.078};
constexpr double allpassTimes[] = {0.02, 0.014, 0.009, 0.006};
constexpr double combFilterFactors[] = {0.42, 0.46, 0.48, 0.49, 0.50, 0.52};
constexpr double allpassGains[] = {-0.6, 0.4, -0.61, 0.39};

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
}

bool Reverb::prepare(double sampleRate, const ReverbSettings& settings)
{
    const auto& s = settings;
    const auto diffGain = static_cast<float>(s.reverbGain);
    const auto dryGain = static_cast<float>(s.mix);
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(s.reverbGain) || diffGain < 0.0 || diffGain > 1.0
        || !std::isfinite(s.mix) || dryGain < 0.0 || dryGain >= 1.0
        || !std::isfinite(s.reverbTime) || s.reverbTime <= 0.0
        || !std::isfinite(s.damping) || s.damping < 0.0 || s.damping > 1.0
        || !std::isfinite(s.lowpassHz) || s.lowpassHz < 0.0 || s.lowpassHz > sampleRate / 3.0
        || !std::isfinite(s.predelayMilliseconds) || s.predelayMilliseconds < 0.0
        || s.predelayMilliseconds > 10000.0
        || !std::isfinite(s.lowCutHz) || s.lowCutHz < 0.0
        || !std::isfinite(s.highCutHz) || s.highCutHz < 0.0
        || (s.lowCutHz > 0.0 && s.highCutHz > 0.0 && s.highCutHz <= s.lowCutHz))
    {
        return false;
    }
    Reverb next;
    // tonecontrol(lowcut_freq, -12.0, LOW_SHELVE, sr) and its lshelve.
    if (s.lowCutHz > 0.0)
    {
        auto& tc = next.lowCut_;
        tc.active = true;
        tc.boost = -12.0;
        const auto freq = s.lowCutHz / sampleRate;
        double a, A, F, tmp, b0, recipb0, asq, F2, gamma2, siggam2, gam2p1;
        double gamman, gammad, ta0, ta1, ta2, tb0, tb1, tb2, aa1, ab1;
        a = std::tan(pi * (freq - 0.25));
        asq = a * a;
        A = std::pow(10.0, tc.boost / 20.0);
        if ((tc.boost < 6.0) && (tc.boost > -6.0))
            F = std::sqrt(A);
        else if (A > 1.0)
            F = A / std::sqrt(2.0);
        else
            F = A * std::sqrt(2.0);
        F2 = F * F;
        tmp = A * A - F2;
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
        gamma2 = gammad * gammad;
        gam2p1 = 1 + gamma2;
        siggam2 = 2 * root2Over2 * gammad;
        tb0 = gam2p1 + siggam2;
        tb1 = -2 * (1 - gamma2);
        tb2 = gam2p1 - siggam2;
        aa1 = a * ta1;
        tc.a0 = ta0 + aa1 + asq * ta2;
        tc.a1 = 2 * a * (ta0 + ta2) + (1 + asq) * ta1;
        tc.a2 = asq * ta0 + aa1 + ta2;
        ab1 = a * tb1;
        b0 = tb0 + ab1 + asq * tb2;
        tc.b1 = 2 * a * (tb0 + tb2) + (1 + asq) * tb1;
        tc.b2 = asq * tb0 + ab1 + tb2;
        recipb0 = 1 / b0;
        tc.a0 *= recipb0;
        tc.a1 *= recipb0;
        tc.a2 *= recipb0;
        tc.b1 *= recipb0;
        tc.b2 *= recipb0;
    }
    // onepole(freq, sr, LOW_PASS).
    const auto onePole = [&](OnePole& filter, double freq) {
        filter.active = true;
        if (freq == 0.0)
        {
            filter.a1 = 1.0;
            filter.a2 = 0.0;
        }
        else
        {
            const auto b = 2.0 - std::cos(twoPi * (freq / sampleRate));
            filter.a2 = std::sqrt((b * b) - 1.0) - b;
            filter.a1 = 1.0 + filter.a2;
        }
    };
    if (s.highCutHz > 0.0)
        onePole(next.highCut_, s.highCutHz);
    if (s.lowpassHz > 0.0)
        onePole(next.lowpass_, s.lowpassHz);
    // moorer::create.
    for (std::size_t index = 0; index < 6; ++index)
    {
        const auto fgain = combFilterFactors[index] * s.damping;
        auto& comb = next.combs_[index];
        comb.gain = std::exp(std::log(0.001) * (combTimes[index] / s.reverbTime)) * (1. - fgain);
        comb.lowpassGain = fgain;
        comb.prescale = 1.0;
        comb.buffer.assign(toPrime(combTimes[index], sampleRate), 0.0);
        if (comb.buffer.empty() || comb.gain < -1.0 || comb.gain > 1.0)
            return false;
    }
    const auto allpass = [&](Allpass& filter, unsigned int length, double gain) {
        filter.gain = gain;
        filter.buffer.assign(length + static_cast<unsigned int>(length * modulationMaximum), 0.0);
        return !filter.buffer.empty();
    };
    for (std::size_t index = 0; index < 4; ++index)
    {
        if (!allpass(next.allpasses_[index], toPrime(allpassTimes[index], sampleRate), allpassGains[index]))
            return false;
    }
    for (std::size_t index = 0; index < 2; ++index)
    {
        if (!allpass(next.diffusers_[index],
                toPrime(0.005 + 0.00025 * static_cast<double>(index), sampleRate), 0.4))
            return false;
    }
    // The room the reverb time chooses, its first reflection moved to the
    // predelay, as reverb.cpp moves them.
    std::span<const ReflectionTap> room = largeTaps;
    if (s.reverbTime <= 0.6)
        room = smallTaps;
    else if (s.reverbTime > 0.6 && s.reverbTime <= 1.3)
        room = mediumTaps;
    std::vector<Tap> taps(room.size());
    for (std::size_t index = 0; index < room.size(); ++index)
        taps[index] = {room[index].pos, room[index].val};
    const auto predelay = s.predelayMilliseconds * 0.001;
    if (predelay > 0.0)
    {
        const auto adjust = predelay - taps[0].pos;
        for (auto& tap : taps)
            tap.pos += adjust;
    }
    // tapdelay::create.
    auto& line = next.earlies_;
    const auto length = static_cast<unsigned int>((taps.back().pos * sampleRate) + safetyFrames);
    line.buffer.assign(length, 0.0);
    line.pointers.resize(taps.size());
    line.coefficients.resize(taps.size());
    for (std::size_t index = 0; index < taps.size(); ++index)
    {
        if (taps[index].pos == 0.0)
            line.pointers[index] = 0;
        else
            line.pointers[index] = length - static_cast<unsigned int>(static_cast<int>(taps[index].pos * sampleRate));
        if (line.pointers[index] >= length)
            return false;
        line.coefficients[index] = taps[index].val;
    }
    line.startPointers = line.pointers;
    line.oneOverTaps = 1.0 / static_cast<double>(taps.size());

    next.reverbGain_ = diffGain;
    next.dryGain_ = dryGain;
    next.wetGain_ = 1.0F - dryGain;
    next.prepared_ = true;
    *this = std::move(next);
    return true;
}

void Reverb::reset() noexcept
{
    std::ranges::fill(earlies_.buffer, 0.0);
    std::ranges::copy(earlies_.startPointers, earlies_.pointers.begin());
    earlies_.reader = 0;
    for (auto& comb : combs_)
    {
        std::ranges::fill(comb.buffer, 0.0);
        comb.reader = comb.writer = 0;
        comb.lowpassTemp = comb.lowpassOutput = 0.0;
    }
    for (auto* group : {&allpasses_[0], &allpasses_[1], &allpasses_[2], &allpasses_[3],
             &diffusers_[0], &diffusers_[1]})
    {
        std::ranges::fill(group->buffer, 0.0);
        group->reader = group->writer = 0;
    }
    highCut_.lastOut = 0.0;
    lowpass_.lastOut = 0.0;
    lowCut_.x1 = lowCut_.x2 = lowCut_.y1 = lowCut_.y2 = 0.0;
}

double Reverb::tick(TapLine& line, double sample) noexcept
{
    double val = 0.0;
    line.buffer[line.reader++] = sample;
    if (line.reader == line.buffer.size())
        line.reader = 0;
    for (std::size_t index = 0; index < line.pointers.size(); ++index)
    {
        val += ((line.buffer[line.pointers[index]++]) * (line.coefficients[index]));
        if (line.pointers[index] == line.buffer.size())
            line.pointers[index] = 0;
    }
    val *= line.oneOverTaps;
    return val * 1.0;
}

// allpassfilter::tick, without its unused pre-delay.
double Reverb::tick(Allpass& filter, double insamp) noexcept
{
    double output, input;
    input = insamp;
    output = (-filter.gain) * input;
    output += filter.buffer[filter.reader++];
    input += filter.gain * output;
    filter.buffer[filter.writer++] = input;
    if (filter.reader == filter.buffer.size())
        filter.reader = 0;
    if (filter.writer == filter.buffer.size())
        filter.writer = 0;
    return output;
}

// lpcomb::tick with lowpass1::tick.
double Reverb::tick(LowpassComb& comb, double insam) noexcept
{
    double input, output, lpsig;
    input = insam * comb.prescale;
    output = comb.buffer[comb.reader++];
    comb.lowpassOutput = comb.lowpassTemp;
    comb.lowpassTemp = output + (comb.lowpassOutput * comb.lowpassGain);
    lpsig = comb.lowpassOutput;
    comb.buffer[comb.writer++] = input + (comb.gain * lpsig);
    if (comb.writer == comb.buffer.size())
        comb.writer = 0;
    if (comb.reader == comb.buffer.size())
        comb.reader = 0;
    return output;
}

double Reverb::tick(OnePole& filter, double input) noexcept
{
    double output;
    output = filter.a1 * input - filter.a2 * filter.lastOut;
    filter.lastOut = output;
    return output;
}

double Reverb::tick(ToneControl& tc, double input) noexcept
{
    double out;
    if (tc.boost == 0.0)
        return input;
    out = ((tc.a0 * input) + (tc.a1 * tc.x1) + (tc.a2 * tc.x2) - (tc.b1 * tc.y1) - (tc.b2 * tc.y2));
    tc.x2 = tc.x1;
    tc.x1 = input;
    tc.y2 = tc.y1;
    tc.y1 = out;
    return out;
}

bool Reverb::process(
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
    const auto diff_gain = reverbGain_;
    const auto dry_gain = dryGain_;
    const auto wet_gain = wetGain_;
    // reverb.cpp's main loop for a mono input and two outputs.
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        auto l_ip = static_cast<float>(input[frame]);
        if (lowCut_.active)
            l_ip = static_cast<float>(tick(lowCut_, static_cast<double>(l_ip)));
        if (highCut_.active)
            l_ip = static_cast<float>(tick(highCut_, static_cast<double>(l_ip)));
        const auto l_direct = l_ip;
        auto mono_direct = l_direct;
        const auto r_direct = l_direct;
        if (lowpass_.active)
            mono_direct = static_cast<float>(tick(lowpass_, mono_direct));
        const auto earlies = static_cast<float>(tick(earlies_, mono_direct));
        // moorer::tick.
        const auto reverbInput = static_cast<double>(earlies + mono_direct);
        auto moorerOut = tick(combs_[0], reverbInput)
            + tick(combs_[1], reverbInput)
            + tick(combs_[2], reverbInput)
            + tick(combs_[3], reverbInput)
            + tick(combs_[4], reverbInput)
            + tick(combs_[5], reverbInput);
        moorerOut *= scale_;
        moorerOut = tick(allpasses_[3], tick(allpasses_[2], tick(allpasses_[1], tick(allpasses_[0], moorerOut))));
        const auto out = static_cast<float>(earlies + moorerOut * diff_gain);
        auto l_out = static_cast<float>(tick(diffusers_[0], out) * wet_gain);
        l_out += l_direct * dry_gain;
        auto r_out = r_direct * dry_gain;
        r_out += tick(diffusers_[1], out) * wet_gain;
        left[frame] = static_cast<double>(l_out);
        right[frame] = static_cast<double>(r_out);
    }
    return true;
}
}
