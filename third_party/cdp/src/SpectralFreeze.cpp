/*
 * Derived from the CDP System, Release 8: dev/focus/focus.c and
 * ap_focus.c, and dev/cdp2k/tklib3.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/SpectralFreeze.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

namespace talas::cdp
{
namespace
{
// dev/include/speccon.h.
constexpr int FRZ_BEFORE = -1;
constexpr int FRZ_LIMIT = 0;
constexpr int FRZ_AFTER = 1;
// A stream's file end: CDP's wlength, which a stream never reaches.
constexpr long long endOfStream = std::numeric_limits<long long>::max() / 4;
// The most times a stream's data holds.
constexpr std::size_t maximumTimes = 100000;

[[nodiscard]] float frametimeFor(double sampleRate, int decimation) noexcept
{
    const auto arate = (float)((float)sampleRate / (float)decimation);
    return (float)(1.0 / arate);
}
}

std::optional<FocusFreeze::Plan> FocusFreeze::plan(
    double sampleRate, int points, int overlap, std::string_view data)
{
    const auto d = PhaseVocoder::decimationFor(points, overlap);
    if (!d.has_value() || !(sampleRate > 0.0) || !std::isfinite(sampleRate))
        return std::nullopt;
    const float frametime = frametimeFor(sampleRate, *d);
    // read_specfreeze_data: the file as fgets(temp, 200, fp) reads it, at
    // most 199 characters at a time, each line ending a read.
    std::vector<long long> segtime;
    std::vector<int> timetype;
    std::size_t n = 0;
    bool isDirectionInfo = false;
    const auto grow = [&](std::size_t size) {
        if (segtime.size() < size)
        {
            segtime.resize(size, 0);
            timetype.resize(size, FRZ_LIMIT);
        }
    };
    std::size_t at = 0;
    while (at < data.size())
    {
        std::size_t length = 0;
        while (at + length < data.size() && length < 199)
        {
            ++length;
            if (data[at + length - 1] == '\n')
                break;
        }
        const std::string temp{data.substr(at, length)};
        at += length;
        const char* p = temp.c_str();
        for (;;)
        {
            if (n + 2 > maximumTimes)
                return std::nullopt;
            grow(n + 2);
            timetype[n] = FRZ_LIMIT;
            while (std::isspace(static_cast<unsigned char>(*p)))
                p++;
            if (*p == '\n' || *p == '\0')
                break;
            // set_timetype.
            if (*p == 'b')
            {
                timetype[n] = FRZ_BEFORE;
                isDirectionInfo = true;
                p++;
            }
            else if (*p == 'a')
            {
                timetype[n] = FRZ_AFTER;
                isDirectionInfo = true;
                p++;
            }
            // read_time.
            if (*p != '.' && *p != '-' && !std::isdigit(static_cast<unsigned char>(*p)))
                return std::nullopt;
            char* end = nullptr;
            const double dtime = std::strtod(p, &end);
            if (end == p || !std::isfinite(dtime) || std::fabs(dtime / frametime) > 1.0e15)
                return std::nullopt;
            while (!std::isspace(static_cast<unsigned char>(*p)) && *p != '\n' && *p != '\0')
                p++;
            segtime[n] = std::lround(dtime / frametime);
            if (n == 0)
            {
                if (segtime[0] != 0)
                {
                    // insert_values_at_time_zero.
                    if (segtime[0] < 0)
                    {
                        segtime[0] = 0;
                        n = 0;
                    }
                    else
                    {
                        segtime[1] = segtime[0];
                        timetype[1] = timetype[0];
                        segtime[0] = 0;
                        timetype[0] = FRZ_LIMIT;
                        n = 1;
                    }
                }
            }
            else if (segtime[n] <= segtime[n - 1])
            {
                // check_time_sequencing.
                return std::nullopt;
            }
            // CDP stops at its file's end, which a stream never reaches.
            ++n;
        }
    }
    if (!isDirectionInfo)
        return std::nullopt;
    grow(n + 2);
    segtime[n] = endOfStream;
    timetype[n] = FRZ_LIMIT;
    std::size_t itemcnt = n + 1;
    // get_frztimes.
    std::vector<long long> frztime(itemcnt + 1, -1);
    for (std::size_t k = 0; k + 1 < itemcnt; k++)
    {
        switch (timetype[k])
        {
        case FRZ_BEFORE:
        case FRZ_LIMIT:
            frztime[k] = timetype[k + 1] == FRZ_BEFORE ? segtime[k + 1] : -1;
            break;
        default:
            if (timetype[k + 1] == FRZ_BEFORE)
                return std::nullopt;
            frztime[k] = segtime[k];
            break;
        }
    }
    // rationalise_frztimes, as CDP8 writes it: its inner loop moves only
    // the next time and freeze down (and, for the last segment, reads the
    // freeze past the array's end, which the shortened array then drops).
    for (std::size_t k = 1; k + 1 < itemcnt; k++)
    {
        if (frztime[k - 1] < 0 && frztime[k] < 0)
        {
            for (std::size_t m = k; m + 1 < itemcnt; m++)
            {
                segtime[k] = segtime[k + 1];
                frztime[k] = frztime[k + 1];
            }
            itemcnt--;
            segtime[itemcnt - 1] = segtime[itemcnt];
        }
    }
    Plan result;
    for (std::size_t k = 0; k + 1 < itemcnt; k++)
    {
        Segment segment;
        segment.start = segtime[k];
        segment.end = segtime[k + 1];
        segment.freeze = frztime[k];
        if (segment.end < segment.start)
            return std::nullopt;
        result.segments.push_back(segment);
    }
    // The windows the stream waits for, at most maximumLookahead.
    long long need = 0;
    for (const auto& segment : result.segments)
    {
        if (segment.freeze >= 0 && segment.end > segment.start)
            need = std::max(need, segment.freeze - segment.start);
    }
    result.lookahead = static_cast<int>(std::min<long long>(need, maximumLookahead));
    // Each frozen window is kept from its analysis until the segment's last
    // window is synthesised, lookahead windows later; slots are shared by
    // segments whose spans do not meet.
    const long long lookahead = result.lookahead;
    std::vector<std::size_t> order;
    for (std::size_t k = 0; k < result.segments.size(); k++)
    {
        const auto& segment = result.segments[k];
        if (segment.freeze >= 0 && segment.end > segment.start
            && std::max(segment.start, segment.freeze - lookahead) < segment.end)
        {
            order.push_back(k);
        }
    }
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
        return result.segments[a].freeze < result.segments[b].freeze;
    });
    std::vector<long long> releases;
    for (const auto k : order)
    {
        auto& segment = result.segments[k];
        const auto release = segment.end - 1 + lookahead;
        const auto free = std::ranges::find_if(releases, [&](long long last) { return last < segment.freeze; });
        if (free == releases.end())
        {
            segment.slot = static_cast<int>(releases.size());
            releases.push_back(release);
        }
        else
        {
            segment.slot = static_cast<int>(free - releases.begin());
            *free = release;
        }
    }
    result.slots = static_cast<int>(releases.size());
    return result;
}

std::optional<std::size_t> FocusFreeze::latencyFor(double sampleRate, int points, int overlap, std::string_view data)
{
    const auto latency = PhaseVocoder::latencyFor(points, overlap);
    const auto d = PhaseVocoder::decimationFor(points, overlap);
    if (!latency.has_value() || !d.has_value())
        return std::nullopt;
    const auto planned = plan(sampleRate, points, overlap, data);
    if (!planned.has_value())
        return std::nullopt;
    return *latency + static_cast<std::size_t>(planned->lookahead) * static_cast<std::size_t>(*d);
}

bool FocusFreeze::prepare(double sampleRate, int points, int overlap, Mode mode, std::string_view data)
{
    if (mode < Mode::Amplitudes || mode > Mode::Both)
        return false;
    auto planned = plan(sampleRate, points, overlap, data);
    if (!planned.has_value())
        return false;
    FocusFreeze freeze;
    if (!freeze.analysis_.prepare(sampleRate, points, overlap) || !freeze.synthesis_.prepare(sampleRate, points, overlap))
        return false;
    const auto frameSize = freeze.analysis_.frameSize();
    const auto d = static_cast<std::size_t>(freeze.analysis_.decimation());
    freeze.mode_ = mode;
    freeze.lookahead_ = planned->lookahead;
    freeze.delay_ = static_cast<std::size_t>(planned->lookahead) * d;
    freeze.discard_.assign(d, 0.0);
    freeze.silence_.assign(d, 0.0);
    freeze.ring_.assign((static_cast<std::size_t>(planned->lookahead) + 1) * frameSize, 0.0F);
    freeze.templates_.assign(static_cast<std::size_t>(planned->slots) * frameSize, 0.0F);
    freeze.segments_ = std::move(planned->segments);
    for (std::size_t k = 0; k < freeze.segments_.size(); k++)
    {
        if (freeze.segments_[k].slot >= 0)
            freeze.byFreeze_.push_back(k);
    }
    std::ranges::stable_sort(freeze.byFreeze_, [&](std::size_t a, std::size_t b) {
        return freeze.segments_[a].freeze < freeze.segments_[b].freeze;
    });
    *this = std::move(freeze);
    reset();
    return true;
}

void FocusFreeze::reset() noexcept
{
    analysis_.reset();
    synthesis_.reset();
    waited_ = 0;
    std::ranges::fill(ring_, 0.0F);
    std::ranges::fill(templates_, 0.0F);
    analysed_ = 0;
    synthesised_ = 0;
    nextCapture_ = 0;
    segment_ = 0;
}

float* FocusFreeze::ring(std::size_t window) noexcept
{
    const auto windows = static_cast<std::size_t>(lookahead_) + 1;
    return ring_.data() + (window % windows) * analysis_.frameSize();
}

bool FocusFreeze::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); }))
    {
        return false;
    }
    // A frame spacing at a time, so that each piece completes at most one
    // analysis window, which reaches the ring before synthesis takes the
    // window lookahead windows earlier at the same input frame.
    const auto spacing = discard_.size();
    for (std::size_t offset = 0; offset < input.size();)
    {
        const auto count = std::min(spacing, input.size() - offset);
        (void)analysis_.process(input.subspan(offset, count), std::span<double>{discard_.data(), count},
            [this](std::span<float> frame) { analysed(frame); });
        const auto waiting = std::min(count, delay_ - waited_);
        std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(offset), waiting, 0.0);
        waited_ += waiting;
        if (count > waiting)
        {
            (void)synthesis_.process(std::span<const double>{silence_.data(), count - waiting},
                output.subspan(offset + waiting, count - waiting),
                [this](std::span<float> frame) { synthesised(frame); });
        }
        offset += count;
    }
    return true;
}

// The analysis window, kept in the ring and, when a segment freezes on it,
// in that segment's slot (get_freeze_template copies the window as read).
void FocusFreeze::analysed(std::span<const float> frame) noexcept
{
    const auto window = analysed_++;
    std::ranges::copy(frame, ring(window));
    const auto size = analysis_.frameSize();
    while (nextCapture_ < byFreeze_.size()
        && segments_[byFreeze_[nextCapture_]].freeze <= static_cast<long long>(window))
    {
        const auto& segment = segments_[byFreeze_[nextCapture_]];
        if (segment.freeze == static_cast<long long>(window))
            std::ranges::copy(frame, templates_.data() + static_cast<std::size_t>(segment.slot) * size);
        nextCapture_++;
    }
}

// specfreeze's freeze_data for a window of a frozen segment.
void FocusFreeze::synthesised(std::span<float> frame) noexcept
{
    const auto window = static_cast<long long>(synthesised_);
    const float* original = ring(synthesised_++);
    std::copy(original, original + frame.size(), frame.begin());
    while (segment_ + 1 < segments_.size() && window >= segments_[segment_].end)
        segment_++;
    if (segments_.empty())
        return;
    const auto& segment = segments_[segment_];
    if (window < segment.start || window >= segment.end || segment.freeze < 0 || segment.slot < 0
        || window + lookahead_ < segment.freeze)
    {
        return;
    }
    const float* windowbuf = templates_.data() + static_cast<std::size_t>(segment.slot) * frame.size();
    switch (mode_)
    {
    case Mode::Amplitudes:
        for (std::size_t vc = 0; vc < frame.size(); vc += 2)
            frame[vc] = windowbuf[vc];
        break;
    case Mode::Frequencies:
        for (std::size_t vc = 1; vc < frame.size(); vc += 2)
            frame[vc] = windowbuf[vc];
        break;
    case Mode::Both:
        std::copy(windowbuf, windowbuf + frame.size(), frame.begin());
        break;
    }
}
}
