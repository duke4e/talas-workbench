/*
 * Derived from the CDP System, Release 8: dev/env/envxtract.c,
 * dev/env/envimpos.c, dev/env/envfuncs.c, dev/env/envprocess.c,
 * dev/env/envprepro.c, dev/env/ap_envel.c, dev/cdp2k/tklib1.c and
 * dev/cdp2k/tklib3.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/EnvelopeWarp.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#if defined(__APPLE__)
// Apple's libm, where CDP8's build turns pow(10.0, x) into __exp10(x).
extern "C" double __exp10(double);
#endif

// CDP8's build (Apple clang, -O2, contraction on) fuses a product added to
// or taken from a plain value within one expression: in step interpolation
// (apply_env_to_window), envinvert and do_expanding. The std::fma calls
// below stand there, and contraction is off elsewhere, as in envlimit,
// whose product and sum are separate statements.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace talas::cdp
{
namespace
{
// globcon.h's MIN_FRACTION_OF_LEVEL and FLTERR.
constexpr double minimumLevel = 1.0 / 32767.0;
constexpr double flterr = 0.000002;

// pow(10.0, x) as CDP8's build calls it.
[[nodiscard]] double cdpExp10(double x) noexcept
{
#if defined(__APPLE__)
    return __exp10(x);
#else
    return std::pow(10.0, x);
#endif
}

[[nodiscard]] std::size_t powerOfTwoAtLeast(std::size_t n) noexcept
{
    std::size_t size = 1;
    while (size < n)
        size <<= 1;
    return size;
}

[[nodiscard]] bool within(double value, double lo, double hi) noexcept
{
    return value >= lo && value <= hi;
}

// envreplace for one window: the new value over the original, the last
// ratio held where the original is at or below MIN_FRACTION_OF_LEVEL (0 at
// the start, or where the new value is that low too).
[[nodiscard]] float replaceWindow(float value, float original, bool first, float previous) noexcept
{
    if (static_cast<double>(original) <= minimumLevel)
    {
        if (static_cast<double>(value) <= minimumLevel || first)
            return 0.0F;
        return previous;
    }
    return value / original;
}

// istrough's and ispeak's search span, and digout_trough's: width / 2 above
// and, for an even width, one fewer below.
[[nodiscard]] long long spanBelow(long long width) noexcept
{
    return width % 2 == 0 ? width / 2 - 1 : width / 2;
}
}

// ---------------------------------------------------------------------------
// EnvelopeWindows

std::size_t EnvelopeWindows::windowFrames(double sampleRate, double windowMs) noexcept
{
    if (!(std::isfinite(sampleRate) && sampleRate > 0.0) || !within(windowMs, minimumWindowMs, maximumWindowMs))
        return 0;
    // generate_samp_windowsize, for one channel.
    const double frames = windowMs * 0.001 * sampleRate;
    if (!(frames < 1.0e9))
        return 0;
    const long long unadjusted = std::lround(frames);
    const long long sector = static_cast<long long>(sectorFrames);
    long long size = 0;
    if (unadjusted < sector)
    {
        long long k = sector;
        while (unadjusted < k)
            k /= 2;
        const long long j = k * 2;
        size = j - unadjusted > unadjusted - k ? k : j;
    }
    else
    {
        size = sector * std::lround(static_cast<double>(unadjusted) / static_cast<double>(sector));
    }
    return size > 0 ? static_cast<std::size_t>(size) : 0;
}

bool EnvelopeWindows::prepare(std::size_t windowFrames, std::size_t lookaheadWindows, std::size_t bufferFrames)
{
    if (windowFrames == 0 || bufferFrames == 0 || windowFrames > maximumLatencyFrames ||
        lookaheadWindows > maximumLatencyFrames)
        return false;
    const std::size_t latency = (lookaheadWindows + 2) * windowFrames - 1;
    if (latency > maximumLatencyFrames)
        return false;
    // create_sndbufs_for_envel: the buffer in whole windows, at least 256
    // frames.
    const std::size_t factor = std::max(windowFrames, sectorFrames);
    std::size_t buffer = bufferFrames / factor * factor;
    if (buffer == 0)
        buffer = factor;
    EnvelopeWindows fresh;
    fresh.window_ = windowFrames;
    fresh.latency_ = latency;
    fresh.windowsPerBuffer_ = std::max<std::size_t>(buffer / windowFrames, 1);
    fresh.ring_.assign(powerOfTwoAtLeast(latency + 1), 0.0F);
    fresh.mask_ = fresh.ring_.size() - 1;
    *this = std::move(fresh);
    return true;
}

void EnvelopeWindows::reset() noexcept
{
    std::fill(ring_.begin(), ring_.end(), 0.0F);
    frame_ = 0;
    position_ = 0;
    windowIndex_ = 0;
    running_ = 0.0F;
    sideRunning_ = 0.0F;
    peak_ = 0.0F;
    sidePeak_ = 0.0F;
    isLog_ = false;
}

bool EnvelopeWindows::push(float sample, float side) noexcept
{
    ring_[frame_ & mask_] = sample;
    // getmaxsamp: the largest |x|, as a double, back to float exactly.
    const float magnitude = std::fabs(sample);
    if (magnitude > running_)
        running_ = magnitude;
    const float sideMagnitude = std::fabs(side);
    if (sideMagnitude > sideRunning_)
        sideRunning_ = sideMagnitude;
    frame_++;
    if (++position_ < window_)
        return false;
    peak_ = running_;
    sidePeak_ = sideRunning_;
    running_ = 0.0F;
    sideRunning_ = 0.0F;
    position_ = 0;
    windowIndex_++;
    return true;
}

void EnvelopeWindows::render(std::size_t k, float thisValue, float nextValue) noexcept
{
    // apply_envel_to_buffer starts each buffer interpolating linearly.
    if (k % windowsPerBuffer_ == 0)
        isLog_ = false;
    // step_gen: the step per frame, from a float difference; past 1, a
    // power of ten from the float ratio, for this window and the rest of
    // the buffer (whose later steps, if linear, are taken as exponents).
    const float valueStep = nextValue - thisValue;
    double step = static_cast<double>(valueStep) / static_cast<double>(window_);
    if (std::fabs(step) > 1.0)
    {
        step = std::log10(static_cast<double>(nextValue / thisValue));
        isLog_ = true;
    }
    const double start = static_cast<double>(thisValue);
    const double count = static_cast<double>(window_);
    const std::size_t base = k * window_;
    for (std::size_t i = 0; i < window_; i++)
    {
        float& sample = ring_[(base + i) & mask_];
        double value = 0.0;
        if (isLog_)
        {
            const double multiplier = start * cdpExp10((static_cast<double>(i) / count) * step);
            value = static_cast<double>(sample) * multiplier;
        }
        else
        {
            value = static_cast<double>(sample) * std::fma(step, static_cast<double>(i), start);
        }
        // sfsys.h's min and max, NaN passing the first and failing the
        // second.
        value = value > 1.0 ? 1.0 : value;
        value = value > -1.0 ? value : -1.0;
        sample = static_cast<float>(value);
    }
}

double EnvelopeWindows::pull() const noexcept
{
    if (frame_ <= latency_)
        return 0.0;
    return static_cast<double>(ring_[(frame_ - 1 - latency_) & mask_]);
}

// ---------------------------------------------------------------------------
// EnvelopeWarp

bool EnvelopeWarp::valid(double first, double second) const noexcept
{
    switch (mode_)
    {
    case Mode::Exaggerate:
        return within(first, minimumExaggerate, maximumExaggerate);
    case Mode::Attenuate:
    case Mode::Lift:
    case Mode::Gate:
        return within(first, 0.0, 1.0);
    case Mode::Flatten:
        return within(first, minimumFlatten, maximumFlatten) && std::lround(first) <= maximumFirst_;
    case Mode::Invert:
        // first_param_greater_than_second(ENV_MIRROR, ENV_GATE).
        return within(first, 0.0, 1.0) && within(second, 0.0, 1.0) && !(first > second - flterr);
    case Mode::Limit:
        // first_param_not_less_than_second(ENV_LIMIT, ENV_THRESHOLD).
        return within(first, 0.0, 1.0) && within(second, 0.0, 1.0) && !(second > first);
    case Mode::Corrugate:
        return within(first, 1.0, maximumCorrugate) && within(second, 2.0, maximumCorrugate) &&
               std::lround(first) <= maximumFirst_ && std::lround(second) <= maximumSecond_;
    case Mode::Expand:
        // first_param_not_less_than_second(ENV_THRESHOLD, ENV_GATE).
        return within(first, 0.0, 1.0) && within(second, 0.0, 1.0) && !(first > second);
    case Mode::Ducked:
        return within(first, 0.0, 1.0) && within(second, 0.0, 1.0);
    }
    return false;
}

bool EnvelopeWarp::prepare(
    double sampleRate, Mode mode, double windowMs, double first, double second, int smoothing, int maximumFirst,
    int maximumSecond, std::size_t bufferFrames)
{
    const std::size_t window = EnvelopeWindows::windowFrames(sampleRate, windowMs);
    if (window == 0 || smoothing < 0 || smoothing > maximumSmoothing)
        return false;
    EnvelopeWarp fresh;
    fresh.mode_ = mode;
    fresh.smoothing_ = (mode == Mode::Gate || mode == Mode::Expand) ? smoothing : 0;
    // The largest live Flatten, Trofdel and Peak separation.
    if (mode == Mode::Flatten || mode == Mode::Corrugate)
    {
        if (!(first >= 0.0 && first <= maximumCorrugate) ||
            (mode == Mode::Corrugate && !(second >= 0.0 && second <= maximumCorrugate)))
            return false;
        const int cap = mode == Mode::Flatten ? maximumFlatten : maximumCorrugate;
        if (maximumFirst > cap || (mode == Mode::Corrugate && maximumSecond > cap))
            return false;
        fresh.maximumFirst_ = std::max(static_cast<int>(std::lround(first)), maximumFirst);
        fresh.maximumSecond_ = mode == Mode::Corrugate ? std::max(static_cast<int>(std::lround(second)), maximumSecond) : 0;
    }
    if (!fresh.valid(first, second))
        return false;
    std::size_t lookahead = 0;
    std::size_t history = 16;
    switch (mode)
    {
    case Mode::Flatten:
        // envflatn: window j averages windows j - N / 2 to j + ceil(N / 2) - 1.
        lookahead = static_cast<std::size_t>((fresh.maximumFirst_ + 1) / 2 - 1);
        history += static_cast<std::size_t>(fresh.maximumFirst_);
        break;
    case Mode::Gate:
    case Mode::Expand:
        // gatefilt: the mean of windows j to j + Smoothing - 1.
        lookahead = fresh.smoothing_ > 0 ? static_cast<std::size_t>(fresh.smoothing_ - 1) : 0;
        history += static_cast<std::size_t>(fresh.smoothing_);
        break;
    case Mode::Corrugate:
    {
        // A peak at r needs windows to r + max(1, S / 2); a trough dug at k
        // reaches T / 2 windows up for its peaks and (T - 1) / 2 down.
        const auto troughs = static_cast<std::size_t>(fresh.maximumFirst_);
        const auto separation = static_cast<std::size_t>(fresh.maximumSecond_);
        lookahead = std::max<std::size_t>(1, separation / 2) + troughs / 2 + (troughs - 1) / 2;
        history += troughs + separation;
        break;
    }
    default:
        break;
    }
    if (!fresh.engine_.prepare(window, lookahead, bufferFrames))
        return false;
    fresh.lookahead_ = lookahead;
    fresh.start_ = Controls{first, second};
    fresh.controls_ = fresh.start_;
    const std::size_t size = powerOfTwoAtLeast(lookahead + history);
    fresh.raw_.assign(size, 0.0F);
    fresh.windowControls_.assign(size, Controls{});
    fresh.imposed_.assign(size, 0.0F);
    if (mode == Mode::Corrugate)
    {
        fresh.shadow_.assign(size, 0);
        fresh.dug_.assign(size, 0.0F);
    }
    fresh.envelopeMask_ = size - 1;
    *this = std::move(fresh);
    return true;
}

bool EnvelopeWarp::setControls(double first, double second) noexcept
{
    if (!prepared())
        return false;
    const bool two = mode_ == Mode::Invert || mode_ == Mode::Limit || mode_ == Mode::Corrugate ||
                     mode_ == Mode::Expand || mode_ == Mode::Ducked;
    if (!two)
        second = 0.0;
    if (!valid(first, second))
        return false;
    controls_ = Controls{first, second};
    return true;
}

void EnvelopeWarp::reset() noexcept
{
    engine_.reset();
    controls_ = start_;
    std::fill(raw_.begin(), raw_.end(), 0.0F);
    std::fill(windowControls_.begin(), windowControls_.end(), Controls{});
    std::fill(imposed_.begin(), imposed_.end(), 0.0F);
    std::fill(shadow_.begin(), shadow_.end(), static_cast<unsigned char>(0));
    std::fill(dug_.begin(), dug_.end(), 0.0F);
    gating_ = true;
    upwards_ = false;
}

bool EnvelopeWarp::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size())
        return false;
    for (const double x : input)
    {
        if (!std::isfinite(x))
            return false;
    }
    for (std::size_t n = 0; n < input.size(); n++)
    {
        if (engine_.atWindowStart())
            windowControls_[engine_.windowIndex() & envelopeMask_] = controls_;
        if (engine_.push(static_cast<float>(input[n]), 0.0F))
            completeWindow(engine_.windowIndex() - 1);
        output[n] = engine_.pull();
    }
    return true;
}

void EnvelopeWarp::completeWindow(std::size_t m) noexcept
{
    raw(m) = engine_.peak();
    if (mode_ == Mode::Corrugate)
    {
        corrugate(m);
        return;
    }
    if (m < lookahead_)
        return;
    const std::size_t j = m - lookahead_;
    const float warped = warp(j);
    const float previous = j > 0 ? imposed_[(j - 1) & envelopeMask_] : 0.0F;
    finishWindow(j, replaceWindow(warped, raw(j), j == 0, previous));
}

void EnvelopeWarp::finishWindow(std::size_t j, float value) noexcept
{
    imposed_[j & envelopeMask_] = value;
    if (j > 0)
        engine_.render(j - 1, imposed_[(j - 1) & envelopeMask_], value);
}

bool EnvelopeWarp::gateFilter(std::size_t j, double gate) const noexcept
{
    // gatefilt: the mean of the next Smoothing windows, from this one.
    double average = 0.0;
    for (int n = 0; n < smoothing_; n++)
        average += static_cast<double>(raw(j + static_cast<std::size_t>(n)));
    average /= static_cast<double>(smoothing_);
    return average <= gate;
}

float EnvelopeWarp::warp(std::size_t j) noexcept
{
    const Controls c = windowControls_[j & envelopeMask_];
    const float e = raw(j);
    const double value = static_cast<double>(e);
    switch (mode_)
    {
    case Mode::Exaggerate:
        // envexagg.
        return static_cast<float>(std::pow(value, c.first));
    case Mode::Attenuate:
        // envatten.
        return static_cast<float>(value * c.first);
    case Mode::Lift:
    {
        // envlift.
        const auto lifted = static_cast<float>(value + c.first);
        return lifted > 1.0F ? 1.0F : lifted;
    }
    case Mode::Flatten:
    {
        // envflatn: the mean of N windows, the envelope's first window
        // standing in before it.
        const long long count = std::lround(c.first);
        const long long back = count / 2;
        double sum = 0.0;
        for (long long t = 0; t < count; t++)
        {
            const long long index = static_cast<long long>(j) - back + t;
            sum += static_cast<double>(raw(index < 0 ? 0 : static_cast<std::size_t>(index)));
        }
        sum /= static_cast<double>(count);
        return static_cast<float>(sum);
    }
    case Mode::Gate:
    {
        const double gate = c.first;
        if (smoothing_ == 0)
            return value < gate ? 0.0F : e;
        // do_smoothed_gating.
        if (value >= gate && (!gating_ || !gateFilter(j, gate)))
        {
            gating_ = false;
            return e;
        }
        gating_ = true;
        return 0.0F;
    }
    case Mode::Invert:
    {
        // envinvert and do_env_invert: Mirror is CDP's ENV_THRESHOLD.
        const double gate = c.first;
        const double mirror = c.second;
        const double hirange = 1.0 - mirror;
        const double lorange = mirror - gate;
        const double upratio = hirange / lorange;
        const double dnratio = lorange / hirange;
        if (value < gate)
            return 0.0F;
        if (value <= mirror)
            return static_cast<float>(std::fma(mirror - value, upratio, mirror));
        return static_cast<float>(std::fma(-(value - mirror), dnratio, mirror));
    }
    case Mode::Limit:
    {
        // envlimit.
        const double limit = c.first;
        const double threshold = c.second;
        const double toprange = 1.0 - threshold;
        const double newrange = limit - threshold;
        const double squeeze = newrange / toprange;
        if (value > threshold)
        {
            const double x = (value - threshold) * squeeze;
            return static_cast<float>(threshold + x);
        }
        return e;
    }
    case Mode::Expand:
    {
        // envexpand and do_expanding.
        const double gate = c.first;
        const double threshold = c.second;
        const double toprange = 1.0 - gate;
        const double newrange = 1.0 - threshold;
        const double squeeze = newrange / toprange;
        if (smoothing_ > 0)
        {
            if (value > gate && (!gating_ || !gateFilter(j, gate)))
            {
                gating_ = false;
                return static_cast<float>(std::fma(-(1.0 - value), squeeze, 1.0));
            }
            gating_ = true;
            return 0.0F;
        }
        if (value > gate)
            return static_cast<float>(std::fma(-(1.0 - value), squeeze, 1.0));
        return 0.0F;
    }
    case Mode::Ducked:
        // envduck: Gate replaces levels over Threshold.
        return value > c.second ? static_cast<float>(c.first) : e;
    case Mode::Corrugate:
        break;
    }
    return e;
}

// envkorrug, a window at a time: its peak pass marks window r once the
// windows its search reaches have come; its trough pass digs at window k
// once the peaks that can stop the dig are marked; window j is final once
// no later dig can reach it. Any window left above zero becomes 1.
void EnvelopeWarp::corrugate(std::size_t m) noexcept
{
    dug_[m & envelopeMask_] = raw(m);
    const auto troughs = static_cast<std::size_t>(maximumFirst_);
    const auto separation = static_cast<std::size_t>(maximumSecond_);
    const std::size_t peakReach = std::max<std::size_t>(1, separation / 2);
    const std::size_t digUp = troughs / 2;
    const std::size_t digDown = (troughs - 1) / 2;
    if (m < peakReach)
        return;
    const std::size_t r = m - peakReach;
    shadow_[r & envelopeMask_] = shadowPeak(r) ? 1 : 0;
    if (r < digUp)
        return;
    const std::size_t k = r - digUp;
    troughStep(k);
    if (k < digDown)
        return;
    const std::size_t j = k - digDown;
    const float level = dug_[j & envelopeMask_];
    finishWindow(j, level > 0.0F ? 1.0F : level);
}

bool EnvelopeWarp::shadowPeak(std::size_t r) const noexcept
{
    // The first pass: a window the envelope rises to and does not rise
    // from, ispeak over Peak separation windows around it.
    if (r == 0)
        return false;
    const float level = raw(r);
    if (!(level > raw(r - 1)) || !(raw(r + 1) <= level))
        return false;
    const long long width = std::lround(windowControls_[r & envelopeMask_].second);
    if (width < 2)
        return true;
    const long long at = static_cast<long long>(r);
    const long long lo = std::max<long long>(0, at - spanBelow(width));
    const long long hi = at + width / 2;
    for (long long q = lo; q <= hi; q++)
    {
        if (level < raw(static_cast<std::size_t>(q)))
            return false;
    }
    return true;
}

void EnvelopeWarp::troughStep(std::size_t k) noexcept
{
    // The second pass, over the envelope as its digs leave it.
    auto dug = [this](long long index) -> float& { return dug_[static_cast<std::size_t>(index) & envelopeMask_]; };
    const long long at = static_cast<long long>(k);
    if (upwards_)
    {
        if (dug(at + 1) <= dug(at))
            upwards_ = false;
        return;
    }
    if (!(dug(at + 1) > dug(at)))
        return;
    upwards_ = true;
    const Controls c = windowControls_[k & envelopeMask_];
    // istrough.
    const long long width = std::lround(c.second);
    if (width >= 2)
    {
        const long long lo = std::max<long long>(0, at - spanBelow(width));
        const long long hi = at + width / 2;
        for (long long q = lo; q <= hi; q++)
        {
            if (dug(at) > dug(q))
                return;
        }
    }
    // digout_trough: Trofdel windows around the trough, stopping short of
    // any peak.
    const long long del = std::lround(c.first);
    if (del < 2)
    {
        dug(at) = 0.0F;
        return;
    }
    long long downq = std::max<long long>(0, at - spanBelow(del));
    long long upq = at + del / 2;
    const long long lopos = downq;
    const long long hipos = upq;
    for (long long q = at; q >= lopos; q--)
    {
        if (shadow_[static_cast<std::size_t>(q) & envelopeMask_] != 0)
        {
            downq = q + 1;
            break;
        }
    }
    for (long long q = at; q <= hipos; q++)
    {
        if (shadow_[static_cast<std::size_t>(q) & envelopeMask_] != 0)
        {
            upq = q - 1;
            break;
        }
    }
    for (long long q = downq; q <= upq; q++)
        dug(q) = 0.0F;
}

// ---------------------------------------------------------------------------
// EnvelopeImpose

bool EnvelopeImpose::prepare(double sampleRate, Mode mode, double windowMs, std::size_t bufferFrames)
{
    const std::size_t window = EnvelopeWindows::windowFrames(sampleRate, windowMs);
    if (window == 0)
        return false;
    EnvelopeImpose fresh;
    if (!fresh.engine_.prepare(window, 0, bufferFrames))
        return false;
    fresh.mode_ = mode;
    *this = std::move(fresh);
    return true;
}

void EnvelopeImpose::reset() noexcept
{
    engine_.reset();
    previous_ = 0.0F;
}

bool EnvelopeImpose::process(
    std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size() || side.size() != output.size())
        return false;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        if (!std::isfinite(input[n]) || !std::isfinite(side[n]))
            return false;
    }
    for (std::size_t n = 0; n < input.size(); n++)
    {
        if (engine_.push(static_cast<float>(input[n]), static_cast<float>(side[n])))
        {
            const std::size_t m = engine_.windowIndex() - 1;
            const float value = mode_ == Mode::Impose
                                    ? engine_.sidePeak()
                                    : replaceWindow(engine_.sidePeak(), engine_.peak(), m == 0, previous_);
            if (m > 0)
                engine_.render(m - 1, previous_, value);
            previous_ = value;
        }
        output[n] = engine_.pull();
    }
    return true;
}

// ---------------------------------------------------------------------------
// EnvelopeTarget

bool EnvelopeTarget::valid(double target) const noexcept
{
    // read_brkvals_from_other_file's and read_dB_brkvals_from_other_file's
    // ranges.
    return mode_ == Mode::Level ? within(target, 0.0, 1.0) : within(target, minimumDecibels, 0.0);
}

float EnvelopeTarget::gain(double target) const noexcept
{
    if (mode_ == Mode::Level)
        return static_cast<float>(target);
    // convert_dB_at_or_below_zero_to_gain.
    double value = target;
    if (value <= minimumDecibels)
        value = 0.0;
    else if (!(value > flterr) && !(value < -flterr))
        value = 1.0;
    else
    {
        value = -value;
        value /= 20.0;
        value = cdpExp10(value);
        value = 1.0 / value;
    }
    // convert_brktable_to_envtable's float table.
    return static_cast<float>(value);
}

bool EnvelopeTarget::prepare(double sampleRate, Mode mode, double windowMs, double target, std::size_t bufferFrames)
{
    const std::size_t window = EnvelopeWindows::windowFrames(sampleRate, windowMs);
    if (window == 0)
        return false;
    EnvelopeTarget fresh;
    fresh.mode_ = mode;
    if (!fresh.valid(target) || !fresh.engine_.prepare(window, 0, bufferFrames))
        return false;
    fresh.start_ = fresh.gain(target);
    fresh.target_ = fresh.start_;
    *this = std::move(fresh);
    return true;
}

bool EnvelopeTarget::setTarget(double target) noexcept
{
    if (!prepared() || !valid(target))
        return false;
    target_ = gain(target);
    return true;
}

void EnvelopeTarget::reset() noexcept
{
    engine_.reset();
    target_ = start_;
    latched_ = 0.0F;
    previous_ = 0.0F;
}

bool EnvelopeTarget::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size())
        return false;
    for (const double x : input)
    {
        if (!std::isfinite(x))
            return false;
    }
    for (std::size_t n = 0; n < input.size(); n++)
    {
        if (engine_.atWindowStart())
            latched_ = target_;
        if (engine_.push(static_cast<float>(input[n]), 0.0F))
        {
            const std::size_t m = engine_.windowIndex() - 1;
            const float value = replaceWindow(latched_, engine_.peak(), m == 0, previous_);
            if (m > 0)
                engine_.render(m - 1, previous_, value);
            previous_ = value;
        }
        output[n] = engine_.pull();
    }
    return true;
}
}
