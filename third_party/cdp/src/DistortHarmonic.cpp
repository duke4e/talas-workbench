/*
 * Derived from the CDP System, Release 8: dev/distort/distorth.c,
 * distortf.c, distortion.c and ap_distort.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/DistortHarmonic.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <string>

namespace talas::cdp
{
namespace
{
// The ring holds the latency and the frame arriving.
constexpr std::size_t ringFrames = 4096;
// special.h's MIN_HARMONIC and MAX_HARMONIC; get_harmonic_data compares the
// amplitudes to MIN_HARM_AMP and MAX_HARM_AMP cast to whole numbers.
constexpr int minimumHarmonic = 2;
constexpr int maximumHarmonic = 1024;
constexpr double minimumAmplitude = 0.0;
constexpr double maximumAmplitude = 32.0;
// distcon.h's MAXWAVELEN.
constexpr double maxWaveLength = 0.5;

[[nodiscard]] bool space(char character) noexcept
{
    return character == ' ' || character == '\t' || character == '\n'
        || character == '\r' || character == '\v' || character == '\f';
}

[[nodiscard]] bool validHarmonic(const DistortHarmonicPartial& partial) noexcept
{
    return partial.number >= minimumHarmonic && partial.number <= maximumHarmonic
        && std::isfinite(partial.amplitude) && partial.amplitude >= minimumAmplitude
        && partial.amplitude <= maximumAmplitude;
}

// get_full_cycle's halves: a cycle runs while the samples keep to its first
// half's side of zero (zero counting on both sides), then while they keep to
// its second's; true when the sample that leaves the second, starting the
// next cycle, arrives.
[[nodiscard]] bool cycleEnds(float b, int phase, bool& secondHalf) noexcept
{
    const auto firstSide = phase >= 0 ? b >= 0.0F : b <= 0.0F;
    const auto secondSide = phase >= 0 ? b <= 0.0F : b >= 0.0F;
    if (!secondHalf && firstSide)
        return false;
    secondHalf = true;
    if (secondSide)
        return false;
    secondHalf = false;
    return true;
}
}

std::optional<std::vector<DistortHarmonicPartial>> readDistortHarmonics(std::string_view text)
{
    std::vector<DistortHarmonicPartial> harmonics;
    std::size_t begin = 0;
    while (begin < text.size())
    {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline + 1;
        const std::string line{text.substr(begin, end - begin)};
        begin = end;
        if (std::ranges::all_of(line, space))
            continue;
        // sscanf's "%d%lf": a whole number as strtol reads it, then a number
        // as strtod reads it from where the first stopped.
        const char* cursor = line.c_str();
        char* stop = nullptr;
        errno = 0;
        const auto number = std::strtol(cursor, &stop, 10);
        if (stop == cursor || errno == ERANGE || number < INT_MIN || number > INT_MAX)
            return std::nullopt;
        cursor = stop;
        const auto amplitude = std::strtod(cursor, &stop);
        if (stop == cursor)
            return std::nullopt;
        const DistortHarmonicPartial partial{static_cast<int>(number), amplitude};
        if (!validHarmonic(partial))
            return std::nullopt;
        harmonics.push_back(partial);
    }
    if (harmonics.empty())
        return std::nullopt;
    return harmonics;
}

bool DistortHarmonic::prepare(std::span<const DistortHarmonicPartial> harmonics, double prescale)
{
    if (harmonics.empty() || !std::ranges::all_of(harmonics, validHarmonic)
        || !std::isfinite(prescale) || prescale < minimumPrescale || prescale > maximumPrescale)
    {
        return false;
    }
    harmonics_.assign(harmonics.begin(), harmonics.end());
    prescale_ = prescale;
    ring_.assign(ringFrames, 0.0F);
    mask_ = ringFrames - 1;
    cycle_.assign(latencyFrames + 1, 0.0F);
    reset();
    return true;
}

void DistortHarmonic::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    frame_ = 0;
    start_ = 0;
    phase_ = 0;
    secondHalf_ = false;
}

bool DistortHarmonic::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (ring_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto k = frame_;
        // prescale, as CDP scales each buffer it reads.
        const auto b = static_cast<float>(static_cast<double>(static_cast<float>(input[index])) * prescale_);
        at(k) = b;
        // The stream waits no longer for a cycle to end.
        if (k - start_ >= latencyFrames)
            harmonize(k);
        if (phase_ == 0 && b != 0.0F)
            phase_ = b > 0.0F ? 1 : -1;
        if (cycleEnds(b, phase_, secondHalf_))
            harmonize(k);
        output[index] = k >= latencyFrames ? static_cast<double>(at(k - latencyFrames)) : 0.0;
        ++frame_;
    }
    return true;
}

// do_harmonic; do_harmonic_crosbuf adds the same samples across two
// buffers.
void DistortHarmonic::harmonize(std::size_t end) noexcept
{
    const auto cyclestart = start_;
    const auto cyclelen = static_cast<int>(end - cyclestart);
    start_ = end;
    // get_halfcycle's copy of the cycle as it came in.
    for (int j = 0; j < cyclelen; j++)
        cycle_[static_cast<std::size_t>(j)] = at(cyclestart + static_cast<std::size_t>(j));
    for (const auto& harmonic : harmonics_)
    {
        auto k = cyclestart;
        int i = harmonic.number;
        if (i >= cyclelen / 4)
            continue;
        while (k < end)
        {
            const double val = at(k) + (cycle_[static_cast<std::size_t>(i)] * harmonic.amplitude);
            at(k++) = static_cast<float>(val);
            i += harmonic.number;
            while (i >= cyclelen)
                i -= cyclelen;
        }
    }
}

bool DistortFractal::prepare(double sampleRate, int scale, double loudness, double prescale, bool liveScale)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || scale < minimumScale
        || static_cast<double>(scale) > maxWaveLength * sampleRate || !std::isfinite(loudness)
        || loudness < minimumLoudness || loudness > maximumLoudness || !std::isfinite(prescale)
        || prescale < minimumPrescale || prescale > maximumLoudness)
    {
        return false;
    }
    startScale_ = scale;
    startLoudness_ = loudness;
    prescale_ = prescale;
    maximumScale_ = maxWaveLength * sampleRate;
    ring_.assign(ringFrames, 0.0F);
    mask_ = ringFrames - 1;
    // create_sndbufs_plus_cyclebuf's cycle buffer, which the group's kept
    // samples never outgrow within the latency, and one past them.
    miniature_.assign(latencyFrames + 2, 0.0F);
    // A group ends a cycle at most each frame within the latency.
    cycleStarts_.assign(liveScale ? latencyFrames + 3 : static_cast<std::size_t>(scale) + 1, 0);
    reset();
    return true;
}

bool DistortFractal::setControls(double scale, double loudness) noexcept
{
    if (ring_.empty() || !std::isfinite(scale) || !std::isfinite(loudness)
        || loudness < minimumLoudness || loudness > maximumLoudness)
    {
        return false;
    }
    // read_value_from_brktable's rounding of an integer field.
    const auto rounded = std::round(scale);
    if (rounded < minimumScale || rounded > maximumScale_
        || (cycleStarts_.size() < latencyFrames + 3 && rounded != startScale_))
    {
        return false;
    }
    nextScale_ = static_cast<int>(rounded);
    nextLoudness_ = loudness;
    return true;
}

void DistortFractal::reset() noexcept
{
    scale_ = nextScale_ = startScale_;
    loudness_ = nextLoudness_ = startLoudness_;
    groupStarting_ = true;
    std::ranges::fill(ring_, 0.0F);
    std::ranges::fill(miniature_, 0.0F);
    frame_ = 0;
    start_ = 0;
    kept_ = 0;
    phase_ = 0;
    secondHalf_ = false;
    cycles_ = 0;
    count_ = 0;
    cycleStarts_[0] = 0;
}

bool DistortFractal::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (ring_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto k = frame_;
        const auto b = static_cast<float>(static_cast<double>(static_cast<float>(input[index])) * prescale_);
        at(k) = b;
        // distortf reads its breakpoints where the group starts.
        if (groupStarting_)
        {
            scale_ = nextScale_;
            loudness_ = nextLoudness_;
            groupStarting_ = false;
        }
        // The stream waits no longer for a group to end.
        if (k - start_ >= latencyFrames)
            layMiniature(k);
        if (phase_ == 0 && b != 0.0F)
            phase_ = b > 0.0F ? 1 : -1;
        if (cycleEnds(b, phase_, secondHalf_))
        {
            if (++cycles_ == scale_)
                layMiniature(k);
            else
                cycleStarts_[static_cast<std::size_t>(cycles_)] = k;
        }
        // get_scaled_halfcycle keeps every Scale-th sample of the group.
        if (++count_ >= scale_)
        {
            miniature_[kept_++] = b;
            count_ = 0;
        }
        output[index] = k >= latencyFrames ? static_cast<double>(at(k - latencyFrames)) : 0.0;
        ++frame_;
    }
    return true;
}

// do_fractal; do_fractal_crosbuf adds the same samples across two buffers.
void DistortFractal::layMiniature(std::size_t end) noexcept
{
    const auto cycles = static_cast<std::size_t>(cycles_ == scale_ ? scale_ : cycles_ + 1);
    cycleStarts_[cycles] = end;
    const auto grouplen = static_cast<double>(kept_);
    for (std::size_t n = 0; n < cycles; n++)
    {
        const auto first = cycleStarts_[n];
        const auto last = cycleStarts_[n + 1];
        const double ratio = grouplen / static_cast<double>(last - first);
        long k = 0;
        for (auto i = first; i < last; i++)
        {
            const auto position = std::lround(ratio * static_cast<double>(k++));
            at(i) += static_cast<float>(loudness_ * miniature_[static_cast<std::size_t>(position)]);
        }
    }
    start_ = end;
    cycleStarts_[0] = end;
    cycles_ = 0;
    count_ = 0;
    kept_ = 0;
    // The next group starts here and reads its breakpoints.
    scale_ = nextScale_;
    loudness_ = nextLoudness_;
}
}
