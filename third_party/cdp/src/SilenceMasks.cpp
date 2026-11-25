/*
 * Derived from the CDP System, Release 8: dev/editsf/cut.c,
 * dev/editsf/ap_edit.c, dev/cdp2k/tklib1.c and dev/cdp2k/readdata.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SilenceMasks.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

// CDP8's build fuses do_other_splice's restart, a1 += aincr * start, into a
// multiply-add; the std::fma below stands for it, and contraction is off
// elsewhere.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR and MS_TO_SECS.
constexpr double FLTERR = 0.000002;
constexpr double MS_TO_SECS = 0.001;
// CDP's ints.
constexpr double largest = 2147483647.0;

[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const auto upperbnd = f2 + FLTERR;
    const auto lowerbnd = f2 - FLTERR;
    return !(f1 > upperbnd || f1 < lowerbnd);
}

// readdata.c's range check, the ends taken within FLTERR.
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

// edit_pconsistency's splice: round(ms * MS_TO_SECS * sr) * chans samples.
[[nodiscard]] bool spliceSamples(double sampleRate, double& spliceMs, long long chans, double maximum,
    long long& samples) noexcept
{
    if (!inRange(spliceMs, 0.0, maximum))
        return false;
    const double frames = std::round(spliceMs * MS_TO_SECS * sampleRate);
    if (!(frames * static_cast<double>(chans) <= largest))
        return false;
    samples = static_cast<long long>(frames) * chans;
    return true;
}

[[nodiscard]] bool wholeFrames(std::span<const double> input, std::span<const double> output, std::size_t channels)
{
    return input.size() == output.size() && input.size() % channels == 0
           && std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); });
}
}

// ---------------------------------------------------------------------------
// SilenceMasks

std::size_t SilenceMasks::defaultBufferFrames(int channels) noexcept
{
    if (channels < 1)
        return 0;
    // create_insert_buffers: Malloc(-1)'s 1048576, taken as samples, in
    // whole frames of chans * F_SECSIZE * 2.
    const auto framesize = static_cast<std::size_t>(channels) * 512;
    return std::size_t{1048576} / framesize * framesize / static_cast<std::size_t>(channels);
}

bool SilenceMasks::prepare(double sampleRate, int channels, EditUnits units, std::span<const double> times,
    double spliceMs, std::size_t bufferFrames)
{
    if (!std::isfinite(sampleRate) || !(sampleRate > 0.0) || channels < 1 || channels > maximumChannels)
        return false;
    if (times.empty() || times.size() % 2 != 0 || units < EditUnits::Seconds || units > EditUnits::Frames)
        return false;
    const long long chans = channels;
    long long splen = 0;
    if (!spliceSamples(sampleRate, spliceMs, chans, maximumSpliceMs, splen))
        return false;
    // edit_pconsistency, INSERTSIL_MANY: the splice up to whole pairs of
    // frames.
    const long long zz = chans * 2;
    long long zsecs = splen / zz;
    if (zsecs * zz != splen)
        zsecs++;
    splen = zsecs * zz;
    const long long hlfsplen = splen / 2;

    // get_excise_times_from_file.
    const std::size_t count = times.size() / 2;
    std::vector<long long> sttsamp(count), endsamp(count), sttspli(count), endspli(count);
    for (std::size_t k = 0; k < count; k++)
    {
        for (int which = 0; which < 2; which++)
        {
            const double value = times[2 * k + static_cast<std::size_t>(which)];
            if (!std::isfinite(value))
                return false;
            double samples = 0.0;
            switch (units)
            {
            case EditUnits::Samples:
                samples = std::trunc(value);
                break;
            case EditUnits::Frames:
                samples = std::trunc(value) * static_cast<double>(chans);
                break;
            case EditUnits::Seconds:
                samples = std::round(value * sampleRate) * static_cast<double>(chans);
                break;
            }
            if (!(std::fabs(samples) <= largest))
                return false;
            (which == 0 ? sttsamp : endsamp)[k] = static_cast<long long>(samples);
        }
        if (sttsamp[k] < 0)
            return false;
        if (k > 0 && sttsamp[k] <= endsamp[k - 1])
            return false;
        if (sttsamp[k] >= endsamp[k])
            return false;
    }
    // edit_pconsistency: aligned to frames.
    if (chans > 1)
    {
        for (std::size_t k = 0; k < count; k++)
        {
            sttsamp[k] = sttsamp[k] / chans * chans;
            endsamp[k] = endsamp[k] / chans * chans;
        }
    }
    // check_excise_splices.
    if (sttsamp[0] <= 0)
        sttspli[0] = 0;
    else
    {
        if ((sttspli[0] = sttsamp[0] - hlfsplen) < 0)
            return false;
        sttsamp[0] += hlfsplen;
    }
    for (std::size_t k = 1; k < count; k++)
    {
        sttspli[k] = sttsamp[k] - hlfsplen;
        sttsamp[k] += hlfsplen;
    }
    for (std::size_t k = 0; k < count; k++)
    {
        endspli[k] = endsamp[k] + hlfsplen;
        endsamp[k] -= hlfsplen;
        if (endsamp[k] < sttsamp[k])
            return false;
        if (k > 0 && sttspli[k] < endspli[k - 1])
            return false;
    }
    const std::size_t splice = static_cast<std::size_t>(splen / chans);
    const std::size_t buffer = bufferFrames == 0 ? defaultBufferFrames(channels) : bufferFrames;
    if (splice > buffer)
        return false;

    SilenceMasks fresh;
    fresh.channels_ = static_cast<std::size_t>(channels);
    fresh.splice_ = splice;
    fresh.bufferFrames_ = buffer;
    // do_other_splice: aincr = 1.0 / sampgrps.
    fresh.step_ = splice > 0 ? 1.0 / static_cast<double>(splice) : 0.0;
    fresh.masks_.reserve(count);
    for (std::size_t k = 0; k < count; k++)
    {
        fresh.masks_.push_back(Mask{static_cast<std::size_t>(sttspli[k] / chans),
            static_cast<std::size_t>(sttsamp[k] / chans), static_cast<std::size_t>(endsamp[k] / chans)});
    }
    *this = std::move(fresh);
    return true;
}

void SilenceMasks::reset() noexcept
{
    frame_ = 0;
    next_ = 0;
    out_ = Fade{};
    in_ = Fade{};
}

double SilenceMasks::gain(Fade& fade, std::size_t start, std::size_t frame, bool down) const noexcept
{
    const double aincr = down ? -step_ : step_;
    double a1 = 0.0;
    if (frame == start || frame % bufferFrames_ == 0)
    {
        // do_other_splice from start_in_splice, counted in samples.
        a1 = down ? 1.0 + aincr : 0.0;
        const auto startInSplice = static_cast<double>((frame - start) * channels_);
        a1 = std::fma(aincr, startInSplice, a1);
    }
    else
    {
        a1 = fade.next;
    }
    fade.next = a1 + aincr;
    return a1;
}

bool SilenceMasks::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || !wholeFrames(input, output, channels_))
        return false;
    const std::size_t frames = input.size() / channels_;
    for (std::size_t n = 0; n < frames; n++, frame_++)
    {
        while (next_ < masks_.size() && masks_[next_].fadeIn + splice_ <= frame_)
        {
            next_++;
            out_ = Fade{};
            in_ = Fade{};
        }
        const Mask* mask = next_ < masks_.size() && masks_[next_].fadeOut <= frame_ ? &masks_[next_] : nullptr;
        const bool fadingOut = mask != nullptr && frame_ < mask->fadeOut + splice_;
        const bool silent = mask != nullptr && frame_ >= mask->silence && frame_ < mask->fadeIn;
        const bool fadingIn = mask != nullptr && frame_ >= mask->fadeIn;
        const double down = fadingOut ? gain(out_, mask->fadeOut, frame_, true) : 1.0;
        const double up = fadingIn ? gain(in_, mask->fadeIn, frame_, false) : 1.0;
        for (std::size_t c = 0; c < channels_; c++)
        {
            float sample = static_cast<float>(input[n * channels_ + c]);
            if (fadingOut)
                sample = static_cast<float>(static_cast<double>(sample) * down);
            if (silent)
                sample = 0.0F;
            if (fadingIn)
                sample = static_cast<float>(static_cast<double>(sample) * up);
            output[n * channels_ + c] = static_cast<double>(sample);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// SilenceOverwrite

bool SilenceOverwrite::prepare(
    double sampleRate, int channels, EditUnits units, double time, double duration, double spliceMs)
{
    if (!std::isfinite(sampleRate) || !(sampleRate > 0.0) || channels < 1 || channels > maximumChannels)
        return false;
    if (!std::isfinite(time) || !std::isfinite(duration) || time < 0.0 || duration < 0.0
        || units < EditUnits::Seconds || units > EditUnits::Frames)
        return false;
    const long long chans = channels;
    long long splen = 0;
    if (!spliceSamples(sampleRate, spliceMs, chans, maximumSpliceMs, splen))
        return false;
    // edit_pconsistency: CUT_CUT and CUT_END (the duration) in samples.
    const auto toSamples = [&](double value, long long& samples) {
        double result = 0.0;
        switch (units)
        {
        case EditUnits::Seconds:
            result = std::round(value * sampleRate) * static_cast<double>(chans);
            break;
        case EditUnits::Samples:
            // An int parameter, rounded, then down to a whole frame.
            result = std::round(value);
            result = std::trunc(result / static_cast<double>(chans)) * static_cast<double>(chans);
            break;
        case EditUnits::Frames:
            result = std::round(value) * static_cast<double>(chans);
            break;
        }
        if (!(result <= largest))
            return false;
        samples = static_cast<long long>(result);
        return true;
    };
    long long cut = 0;
    long long len = 0;
    if (!toSamples(time, cut) || !toSamples(duration, len))
        return false;
    if (splen * 2 > len)
        return false;
    if (cut < splen)
        return false;
    SilenceOverwrite fresh;
    fresh.channels_ = static_cast<std::size_t>(channels);
    const auto s = static_cast<std::size_t>(splen / chans);
    const auto t = static_cast<std::size_t>(cut / chans);
    const auto l = static_cast<std::size_t>(len / chans);
    fresh.splice_ = s;
    fresh.fadeOut_ = t - s;
    fresh.silence_ = t;
    fresh.fadeIn_ = t + l - 2 * s;
    fresh.back_ = t + l - s;
    // dosplice: aincr = 1.0 / sampgrps.
    fresh.step_ = s > 0 ? 1.0 / static_cast<double>(s) : 0.0;
    *this = fresh;
    return true;
}

void SilenceOverwrite::reset() noexcept
{
    frame_ = 0;
    running_ = 0.0;
}

bool SilenceOverwrite::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || !wholeFrames(input, output, channels_))
        return false;
    const std::size_t frames = input.size() / channels_;
    for (std::size_t n = 0; n < frames; n++, frame_++)
    {
        const bool fadingOut = frame_ >= fadeOut_ && frame_ < silence_;
        const bool silent = frame_ >= silence_ && frame_ < fadeIn_;
        const bool fadingIn = frame_ >= fadeIn_ && frame_ < back_;
        double a1 = 0.0;
        if (fadingOut || fadingIn)
        {
            // dosplice's running gain: down from 1 + aincr, up from 0.
            if (frame_ == fadeOut_)
                running_ = 1.0 - step_;
            else if (frame_ == fadeIn_)
                running_ = 0.0;
            a1 = running_;
            running_ = fadingOut ? running_ - step_ : running_ + step_;
        }
        for (std::size_t c = 0; c < channels_; c++)
        {
            const float sample = static_cast<float>(input[n * channels_ + c]);
            float value = sample;
            if (silent)
                value = 0.0F;
            else if (fadingOut || fadingIn)
            {
                // The faded sample added to the silence in the buffer.
                const float faded = static_cast<float>(static_cast<double>(sample) * a1);
                value = 0.0F + faded;
            }
            output[n * channels_ + c] = static_cast<double>(value);
        }
    }
    return true;
}
}
