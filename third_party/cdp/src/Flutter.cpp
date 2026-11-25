/*
 * Derived from the CDP System, Release 8: dev/standalone/flutter.c,
 * dev/cdp2k/readdata.c and dev/sfsys/osbind.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Flutter.h"

#include <array>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR and PI; flutter.c's FLUTTER_TABSIZE.
constexpr double FLTERR = 0.000002;
constexpr double PI = 3.141592654;
constexpr std::size_t FLUTTER_TABSIZE = 1024;
// Malloc(-1)'s 1 MB, in float samples.
constexpr std::size_t cdpBufferSamples = 1024 * 1024 / sizeof(float);

// tklib1.c's flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const auto upperbnd = f2 + FLTERR;
    const auto lowerbnd = f2 - FLTERR;
    return !(f1 > upperbnd || f1 < lowerbnd);
}

// readdata.c: a value within FLTERR of a range end is that end.
[[nodiscard]] bool inRange(double& value, double lo, double hi) noexcept
{
    if (!std::isfinite(value))
        return false;
    if (flteq(value, lo))
        value = lo;
    if (flteq(value, hi))
        value = hi;
    return !(value > hi || value < lo);
}

// cyclicperm's hshuflup.
void hshuflup(std::size_t k, std::vector<std::size_t>& perm) noexcept
{
    for (auto n = perm.size() - 1; n > k; n--)
        perm[n] = perm[n - 1];
}
}

std::size_t Flutter::defaultBufferFrames(int channels) noexcept
{
    if (channels < 1)
        return 0;
    return cdpBufferSamples / static_cast<std::size_t>(channels);
}

bool Flutter::prepare(double sampleRate, int channels, std::span<const std::vector<int>> channelSets,
    double frequency, double depth, double gain, bool randomOrder, std::size_t bufferFrames)
{
    if (!std::isfinite(sampleRate) || !(sampleRate > 0.0))
        return false;
    if (channels < 1 || channels > maximumChannels)
        return false;
    if (channelSets.empty() || channelSets.size() > maximumSets || (randomOrder && channelSets.size() < 2))
        return false;
    for (const auto& set : channelSets)
    {
        for (const auto c : set)
        {
            if (c < 1 || c > channels)
                return false;
        }
    }
    if (!inRange(frequency, 0.0, maximumFrequency) || !inRange(depth, 0.0, maximumDepth)
        || !inRange(gain, 0.0, maximumGain) || gain < FLTERR)
        return false;
    if (bufferFrames == 0)
        bufferFrames = defaultBufferFrames(channels);

    setChannels_.clear();
    setStarts_.clear();
    setLengths_.clear();
    for (const auto& set : channelSets)
    {
        setStarts_.push_back(setChannels_.size());
        setLengths_.push_back(set.size());
        for (const auto c : set)
            setChannels_.push_back(static_cast<std::size_t>(c - 1));
    }
    // check_flutter_param_validity_and_consistency's table.
    costab_.assign(FLUTTER_TABSIZE + 1, 0.0);
    for (std::size_t n = 0; n < FLUTTER_TABSIZE; n++)
    {
        costab_[n] = std::cos(PI * 2.0 * (static_cast<double>(n) / static_cast<double>(FLUTTER_TABSIZE)));
        costab_[n] += 1.0;
        costab_[n] /= 2.0;
        costab_[n] = 1.0 - costab_[n];
    }
    costab_[FLUTTER_TABSIZE] = 0.0;
    channels_ = static_cast<std::size_t>(channels);
    tabsizeOverSrate_ = static_cast<double>(FLUTTER_TABSIZE) / sampleRate;
    gain_ = gain;
    startFrequency_ = frequency;
    startDepth_ = depth;
    random_ = randomOrder;
    bufferFrames_ = bufferFrames;
    perm_.assign(channelSets.size(), 0);
    reset();
    return true;
}

bool Flutter::setFrequency(double frequency) noexcept
{
    if (!prepared() || !inRange(frequency, 0.0, maximumFrequency))
        return false;
    frequency_ = frequency;
    return true;
}

bool Flutter::setDepth(double depth) noexcept
{
    if (!prepared() || !inRange(depth, 0.0, maximumDepth))
        return false;
    depth_ = depth;
    return true;
}

void Flutter::reset() noexcept
{
    frequency_ = startFrequency_;
    depth_ = startDepth_;
    fcospos_ = 0.0;
    chansetcnt_ = 0;
    for (std::size_t n = 0; n < perm_.size(); n++)
        perm_[n] = n;
    lastchanset_ = perm_.size() - 1;
    setStart_ = setStarts_.empty() ? 0 : setStarts_[0];
    setLength_ = setLengths_.empty() ? 0 : setLengths_[0];
    randomState_ = Drand48{}.state;
    bufferPosition_ = 0;
    frozen_ = false;
}

// flutter.c's cyclicperm. A draw of exactly 1 (rand() at RAND_MAX) would
// make CDP write past its array; the stream keeps the index within it.
void Flutter::cyclicperm() noexcept
{
    const auto chansetcnt = perm_.size();
    Drand48 random48;
    random48.state = randomState_;
    do
    {
        for (std::size_t n = 0; n < chansetcnt; n++)
        {
            auto t = static_cast<std::size_t>(std::floor(random48.next() * static_cast<double>(n + 1)));
            if (t > n)
                t = n;
            if (t == n)
            {
                hshuflup(0, perm_);
                perm_[0] = n;
            }
            else
            {
                hshuflup(t + 1, perm_);
                perm_[t + 1] = n;
            }
        }
    } while (perm_[0] == lastchanset_);
    randomState_ = random48.state;
}

bool Flutter::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || output.size() != input.size() || input.size() % channels_ != 0)
        return false;
    for (const auto x : input)
    {
        if (!std::isfinite(x))
            return false;
    }
    const auto frames = input.size() / channels_;
    const auto itemcnt = setStarts_.size();
    for (std::size_t i = 0; i < frames; i++)
    {
        const auto* in = input.data() + i * channels_;
        auto* out = output.data() + i * channels_;
        std::array<float, maximumChannels> buf{};
        for (std::size_t m = 0; m < channels_; m++)
            buf[m] = static_cast<float>(in[m]);
        // flutter: where Depth is 0 the frame is left alone and the clock
        // stops for the rest of the buffer.
        const auto depthParam = depth_;
        const auto depth = 1.0 > depthParam ? depthParam : 1.0;
        if (!frozen_ && depth <= 0)
            frozen_ = true;
        if (!frozen_)
        {
            const auto lastfcospos = fcospos_;
            auto baslevel = 1.0 - depth;
            if (baslevel < 0.0)
                baslevel = 0.0;
            const auto cospos = static_cast<std::size_t>(fcospos_);
            const auto frac = fcospos_ - static_cast<double>(cospos);
            const auto locos = costab_[cospos];
            const auto hicos = costab_[cospos + 1];
            auto val = std::fma(hicos - locos, frac, locos);
            if (depthParam > 1.0)
                val = std::pow(val, depthParam);
            val *= depth;
            fcospos_ = std::fma(frequency_, tabsizeOverSrate_, fcospos_);
            fcospos_ = std::fmod(fcospos_, static_cast<double>(FLUTTER_TABSIZE));
            if (fcospos_ < lastfcospos)
            {
                if (++chansetcnt_ >= itemcnt)
                {
                    if (random_)
                    {
                        cyclicperm();
                        lastchanset_ = perm_[itemcnt - 1];
                    }
                    chansetcnt_ = 0;
                }
                const auto set = random_ ? perm_[chansetcnt_] : chansetcnt_;
                setLength_ = setLengths_[set];
                setStart_ = setStarts_[set];
            }
            std::array<float, maximumChannels> store{};
            for (std::size_t m = 0; m < channels_; m++)
            {
                buf[m] = static_cast<float>(static_cast<double>(buf[m]) * gain_);
                store[m] = buf[m];
                buf[m] = static_cast<float>(static_cast<double>(buf[m]) * baslevel);
            }
            for (std::size_t j = 0; j < setLength_; j++)
            {
                const auto m = setChannels_[setStart_ + j];
                buf[m] = static_cast<float>(std::fma(static_cast<double>(store[m]), val, static_cast<double>(buf[m])));
            }
        }
        for (std::size_t m = 0; m < channels_; m++)
            out[m] = static_cast<double>(buf[m]);
        if (++bufferPosition_ == bufferFrames_)
        {
            bufferPosition_ = 0;
            frozen_ = false;
        }
    }
    return true;
}
}
