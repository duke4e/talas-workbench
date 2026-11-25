/*
 * Derived from the CDP System, Release 8: dev/filter/filters0.c,
 * ap_filter.c and fltprepro.c, and dev/cdp2k/tklib1.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/UserBank.h"

#include "FilterBankData.h"

#include <algorithm>
#include <cmath>

namespace talas::cdp
{
std::optional<std::vector<FilterBand>> readFilterBands(
    std::string_view text,
    FilterPitches pitches,
    double sampleRate)
{
    using namespace filter_bank;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || (pitches != FilterPitches::Hertz && pitches != FilterPitches::Midi))
    {
        return std::nullopt;
    }
    // special.c's range for the pitches.
    const auto lowest = pitches == FilterPitches::Hertz
        ? minimumFrequency
        : uncheckedHztomidi(minimumFrequency);
    const auto highest = pitches == FilterPitches::Hertz ? sampleRate / 2.0 : maximumMidi;
    std::vector<FilterBand> bands;
    auto valid = true;
    // read_filter_data: two values a line.
    forEachLine(text, [&](std::string_view line) {
        if (!valid || emptyOrComment(line))
            return;
        const auto values = words(line);
        if (values.size() != 2)
        {
            valid = false;
            return;
        }
        auto frequency = scanNumber(values[0]);
        if (!frequency.has_value() || !std::isfinite(*frequency) || *frequency < lowest
            || *frequency > highest)
        {
            valid = false;
            return;
        }
        if (pitches == FilterPitches::Midi)
            *frequency = miditohz(*frequency);
        const auto amplitude = level(values[1]);
        if (!amplitude.has_value() || !std::isfinite(*amplitude)
            || *amplitude < UserBank::minimumGain || *amplitude > UserBank::maximumGain
            || bands.size() == UserBank::maximumBands)
        {
            valid = false;
            return;
        }
        bands.push_back({*frequency, *amplitude});
    });
    if (!valid || bands.empty())
        return std::nullopt;
    return bands;
}

bool UserBank::prepare(
    double sampleRate,
    const std::vector<FilterBand>& bands,
    double q,
    double gain,
    bool doubleFiltering)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || bands.empty() || bands.size() > maximumBands
        || !std::ranges::all_of(bands, [&](const FilterBand& band) {
               return std::isfinite(band.frequency) && band.frequency > 0.0
                   && std::isfinite(band.amplitude)
                   && band.amplitude >= minimumGain && band.amplitude <= maximumGain;
           })
        || !std::isfinite(q) || q < minimumQ || q > maximumQ
        || !std::isfinite(gain) || gain < minimumGain || gain > maximumGain)
    {
        return false;
    }
    // filter_pconsistency's FLT_INV_SR, then initialise_filter_params.
    const auto count = bands.size();
    std::vector<double> frequencies(count);
    std::vector<double> amplitudes(count);
    for (std::size_t n = 0; n < count; ++n)
    {
        frequencies[n] = bands[n].frequency;
        amplitudes[n] = bands[n].amplitude;
    }
    frequencies_ = std::move(frequencies);
    amplitudes_ = std::move(amplitudes);
    inverseSampleRate_ = 1.0 / sampleRate;
    a_.assign(count, 0.0);
    b_.assign(count, 0.0);
    ampl_.assign(count, 0.0);
    y_.assign(count, 0.0);
    z_.assign(count, 0.0);
    d_.assign(count, 0.0);
    e_.assign(count, 0.0);
    doubleFiltering_ = doubleFiltering;
    startGain_ = gain;
    gain_ = gain;
    startQ_ = q;
    q_ = nextQ_ = q;
    blockCount_ = 0;
    computeCoefficients(q);
    return true;
}

void UserBank::computeCoefficients(double q) noexcept
{
    for (std::size_t n = 0; n < frequencies_.size(); ++n)
    {
        filter_bank::bandCoefficients(frequencies_[n], amplitudes_[n], q,
            inverseSampleRate_, doubleFiltering_, a_[n], b_[n], ampl_[n]);
    }
}

bool UserBank::setQ(double q) noexcept
{
    if (a_.empty() || !std::isfinite(q) || q < minimumQ || q > maximumQ)
        return false;
    nextQ_ = q;
    return true;
}

void UserBank::reset() noexcept
{
    std::ranges::fill(y_, 0.0);
    std::ranges::fill(z_, 0.0);
    std::ranges::fill(d_, 0.0);
    std::ranges::fill(e_, 0.0);
    gain_ = startGain_;
    if (q_ != startQ_)
        computeCoefficients(startQ_);
    q_ = nextQ_ = startQ_;
    blockCount_ = 0;
}

bool UserBank::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (a_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        // do_qvary_filters's block: the Q its breakpoints give now. The same
        // Q gives the same filters, as do_filters keeps them.
        if (--blockCount_ <= 0)
        {
            if (nextQ_ != q_)
            {
                q_ = nextQ_;
                computeCoefficients(q_);
            }
            blockCount_ = blockSize;
        }
        output[frame] = filter_bank::filterFrame(
            input[frame], a_, b_, ampl_, y_, z_, d_, e_, doubleFiltering_, gain_);
    }
    return true;
}
}
