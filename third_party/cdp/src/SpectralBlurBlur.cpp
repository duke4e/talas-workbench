/*
 * Derived from the CDP System, Release 8: dev/blur/blur.c and ap_blur.c,
 * dev/hilite/hilite.c and ap_hilite.c, and dev/cdp2k/tklib1.c and tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralBlurBlur.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
namespace
{
// read_value_from_brktable's rounding of an integer field, within a range.
[[nodiscard]] bool roundsWithin(double value, int low, int high) noexcept
{
    if (!std::isfinite(value))
        return false;
    const auto rounded = std::round(value);
    return rounded >= low && rounded <= high;
}
}

std::optional<std::size_t> BlurAndTrace::latencyFor(int points, int overlap, int maximumBlur) noexcept
{
    const auto latency = PhaseVocoder::latencyFor(points, overlap);
    const auto d = PhaseVocoder::decimationFor(points, overlap);
    if (!latency.has_value() || !d.has_value() || maximumBlur < 1 || maximumBlur > BlurBlur::maximumBlurLimit)
        return std::nullopt;
    return *latency + static_cast<std::size_t>(maximumBlur) * static_cast<std::size_t>(*d);
}

bool BlurAndTrace::validBlur(double blur) const noexcept
{
    return roundsWithin(blur, 1, maximumBlur_);
}

bool BlurAndTrace::validTrace(double trace) const noexcept
{
    return roundsWithin(trace, 1, clength_);
}

bool BlurAndTrace::prepare(
    double sampleRate, int points, int overlap, double blur, int maximumBlur, bool tracing, double trace)
{
    if (maximumBlur < 1 || maximumBlur > BlurBlur::maximumBlurLimit)
        return false;
    BlurAndTrace core;
    if (!core.analysis_.prepare(sampleRate, points, overlap) || !core.synthesis_.prepare(sampleRate, points, overlap))
        return false;
    const auto frameSize = core.analysis_.frameSize();
    const auto d = static_cast<std::size_t>(core.analysis_.decimation());
    core.clength_ = static_cast<int>(frameSize / 2);
    core.maximumBlur_ = maximumBlur;
    if (!core.validBlur(blur) || (tracing && !core.validTrace(trace)))
        return false;
    const auto windows = static_cast<std::size_t>(maximumBlur) + 1;
    core.delay_ = static_cast<std::size_t>(maximumBlur) * d;
    core.discard_.assign(d, 0.0);
    core.silence_.assign(d, 0.0);
    core.ring_.assign(windows * frameSize, 0.0F);
    core.traces_.assign(windows, 0);
    core.amp_.assign(static_cast<std::size_t>(core.clength_), 0.0F);
    core.freq_.assign(core.amp_.size(), 0.0F);
    core.ampdif_.assign(core.amp_.size(), 0.0F);
    core.freqdif_.assign(core.amp_.size(), 0.0F);
    core.loudest_.assign(tracing ? core.amp_.size() : 0, {0, 0.0F});
    core.tracing_ = tracing;
    core.startBlur_ = static_cast<int>(std::round(blur));
    core.startTrace_ = tracing ? static_cast<int>(std::round(trace)) : 1;
    *this = std::move(core);
    reset();
    return true;
}

void BlurAndTrace::reset() noexcept
{
    analysis_.reset();
    synthesis_.reset();
    waited_ = 0;
    std::ranges::fill(ring_, 0.0F);
    std::ranges::fill(traces_, 0);
    std::ranges::fill(amp_, 0.0F);
    std::ranges::fill(freq_, 0.0F);
    std::ranges::fill(ampdif_, 0.0F);
    std::ranges::fill(freqdif_, 0.0F);
    // create_and_initialise_array_of_loudest_channel_pointers.
    std::ranges::fill(loudest_, std::pair{0, 0.0F});
    blur_ = startBlur_;
    trace_ = startTrace_;
    blurcnt_ = 0;
    blurfactor_ = 0;
    lastTotalWindows_ = 0;
    totalWindows_ = 0;
    synthesised_ = 0;
}

float* BlurAndTrace::slot(std::size_t window) noexcept
{
    const auto windows = static_cast<std::size_t>(maximumBlur_) + 1;
    return ring_.data() + (window % windows) * analysis_.frameSize();
}

bool BlurAndTrace::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); }))
    {
        return false;
    }
    // At most one frame spacing at a time, so that each piece completes at
    // most one analysis frame, which reaches the ring before the synthesis
    // vocoder takes the frame maximumBlur windows earlier at the same input
    // frame.
    const auto spacing = discard_.size();
    for (std::size_t offset = 0; offset < input.size();)
    {
        const auto count = std::min(spacing, input.size() - offset);
        (void)analysis_.process(input.subspan(offset, count), std::span<double>{discard_.data(), count},
            [this](std::span<float> frame) { analysed(frame); });
        // Synthesis starts delay_ frames in, its first window then being
        // the analysis's window 0.
        const auto waiting = std::min(count, delay_ - waited_);
        std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(offset), waiting, 0.0);
        waited_ += waiting;
        if (count > waiting)
        {
            (void)synthesis_.process(std::span<const double>{silence_.data(), count - waiting},
                output.subspan(offset + waiting, count - waiting), [this](std::span<float> frame) {
                    const float* written = slot(synthesised_++);
                    std::copy(written, written + frame.size(), frame.begin());
                });
        }
        offset += count;
    }
    return true;
}

// spec_blur_and_bltr's loop, at each analysis window.
void BlurAndTrace::analysed(std::span<const float> frame) noexcept
{
    // The trace at this window's time (prepare_to_do_trace reads it at each
    // window's time as do_the_bltr writes the window).
    if (tracing_)
        traces_[totalWindows_ % traces_.size()] = trace_;
    if (blurcnt_ == 0)
    {
        if (totalWindows_ > 0)
            bltr(frame);
        blurfactor_ = blur_;
        // A stream has no end: CDP's blurfactor = wlength - total_windows - 1
        // where the blur reaches past it does not apply.
        blurcnt_ = blurfactor_;
        // get_amp_and_frq.
        for (int cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
        {
            amp_[static_cast<std::size_t>(cc)] = frame[static_cast<std::size_t>(vc)];
            freq_[static_cast<std::size_t>(cc)] = frame[static_cast<std::size_t>(vc) + 1];
        }
    }
    blurcnt_--;
    totalWindows_++;
}

// do_the_bltr (hilite's, which blur's is without the trace), with
// prepare_to_do_trace, if_loud_enough_store_in_ring and create_trace: the
// windows from the last kept window up to this one.
void BlurAndTrace::bltr(std::span<const float> frame) noexcept
{
    float* amp = amp_.data();
    float* freq = freq_.data();
    float* ampdif = ampdif_.data();
    float* freqdif = freqdif_.data();
    const float* flbufptr0 = frame.data();
    int cc, vc;
    for (cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
    {
        ampdif[cc] = flbufptr0[vc] - amp[cc];
        ampdif[cc] /= (float)blurfactor_;
        freqdif[cc] = flbufptr0[vc + 1] - freq[cc];
        freqdif[cc] /= (float)blurfactor_;
    }
    std::size_t thiswindow;
    int j;
    for (thiswindow = lastTotalWindows_, j = 0; thiswindow < totalWindows_; thiswindow++, j++)
    {
        float* flbufptr1 = slot(thiswindow);
        int trace = 0;
        if (tracing_)
        {
            // prepare_to_do_trace: zero the loudest amplitudes, keeping
            // their channels.
            trace = traces_[thiswindow % traces_.size()];
            for (int n = 0; n < trace; n++)
                loudest_[static_cast<std::size_t>(n)].second = 0.0F;
        }
        for (cc = 0, vc = 0; cc < clength_; cc++, vc += 2)
        {
            // CDP8's build fuses each multiply and add.
            flbufptr1[vc] = std::fma((float)j, ampdif[cc], amp[cc]);
            flbufptr1[vc + 1] = std::fma((float)j, freqdif[cc], freq[cc]);
            if (tracing_)
            {
                // if_loud_enough_store_in_ring: the amplitude goes before the
                // first entry it beats (the list stays loudest first, so
                // that place is found by halving), the later entries move
                // down a place (make_room_in_loudchans_list_by_moving_later_
                // items_down_a_place) and the last falls out.
                const auto begin = loudest_.begin();
                const auto end = begin + trace;
                const auto val = flbufptr1[vc];
                const auto place = std::partition_point(
                    begin, end, [val](const std::pair<int, float>& entry) { return !(val > entry.second); });
                if (place != end)
                {
                    std::move_backward(place, end - 1, end);
                    *place = {cc, val};
                }
                flbufptr1[vc] = 0.0F;
            }
        }
        if (tracing_)
        {
            // create_trace.
            for (int n = 0; n < trace; n++)
            {
                const auto& [chan, val] = loudest_[static_cast<std::size_t>(n)];
                flbufptr1[chan * 2] = val;
            }
        }
    }
    lastTotalWindows_ = totalWindows_;
}

bool BlurBlur::setBlur(double blur) noexcept
{
    if (!core_.prepared() || !core_.validBlur(blur))
        return false;
    core_.blur_ = static_cast<int>(std::round(blur));
    return true;
}

bool HiliteBlurTrace::setControls(double blur, double trace) noexcept
{
    if (!core_.prepared() || !core_.validBlur(blur) || !core_.validTrace(trace))
        return false;
    core_.blur_ = static_cast<int>(std::round(blur));
    core_.trace_ = static_cast<int>(std::round(trace));
    return true;
}
}
