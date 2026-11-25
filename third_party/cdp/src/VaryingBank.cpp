/*
 * Derived from the CDP System, Release 8: dev/filter/filters0.c,
 * ap_filter.c, fltpcon.c and fltprepro.c, and dev/cdp2k/tklib1.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/VaryingBank.h"

#include "FilterBankData.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace talas::cdp
{
namespace
{
// filtcon.h's BSIZE.
constexpr int blockSize = 128;

// tklib1.c's dbtogain.
[[nodiscard]] double dbtogain(double val) noexcept
{
    auto is_neg = false;
    if (val <= filter_bank::minimumDb)
        return 0.0;
    if (filter_bank::flteq(val, 0.0))
        return 1.0;
    if (val < 0.0)
    {
        val = -val;
        is_neg = true;
    }
    val /= 20.0;
    val = std::pow(10.0, val);
    if (is_neg)
        val = 1.0 / val;
    return val;
}
}

std::optional<VaryingFilterData> readVaryingFilterData(
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
    // get_data_from_tvary_infile: every value read by get_level, rows of
    // equal length with a time and pairs.
    std::vector<double> values;
    std::size_t columns = 0;
    std::size_t rows = 0;
    auto valid = true;
    forEachLine(text, [&](std::string_view line) {
        if (!valid || emptyOrComment(line))
            return;
        const auto entries = words(line);
        for (const auto entry : entries)
        {
            const auto value = level(entry);
            if (!value.has_value() || !std::isfinite(*value))
            {
                valid = false;
                return;
            }
            values.push_back(*value);
        }
        if (rows == 0)
        {
            columns = entries.size();
            if (columns < 3 || (columns - 1) % 2 != 0)
                valid = false;
        }
        else if (entries.size() != columns)
            valid = false;
        ++rows;
    });
    if (!valid || rows == 0)
        return std::nullopt;
    const auto bandCount = (columns - 1) / 2;
    if (bandCount > VaryingBank::maximumBands)
        return std::nullopt;
    // A first line after time 0 is copied to 0.
    if (values[0] != 0.0)
    {
        if (flteq(values[0], 0.0))
            values[0] = 0.0;
        else
        {
            std::vector<double> first(values.begin(),
                values.begin() + static_cast<std::ptrdiff_t>(columns));
            first[0] = 0.0;
            values.insert(values.begin(), first.begin(), first.end());
            ++rows;
        }
    }
    if (values[0] < 0.0)
        return std::nullopt;
    // check_seq_and_range_of_filter_data.
    const auto lowest = pitches == FilterPitches::Hertz
        ? minimumFrequency
        : uncheckedHztomidi(minimumFrequency);
    const auto highest = pitches == FilterPitches::Hertz ? sampleRate / 2.0 : maximumMidi;
    auto lasttime = 0.0;
    for (std::size_t n = 1; n < values.size(); ++n)
    {
        const auto m = n % columns;
        if (m == 0)
        {
            if (values[n] <= lasttime)
                return std::nullopt;
            lasttime = values[n];
        }
        else if (m % 2 == 1)
        {
            if (values[n] < lowest || values[n] > highest)
                return std::nullopt;
            if (pitches == FilterPitches::Midi)
                values[n] = miditohz(values[n]);
        }
        else
            values[n] = std::max(values[n], minimumFilterAmplitude);
    }
    VaryingFilterData data;
    data.bandCount = bandCount;
    for (std::size_t row = 0; row < rows; ++row)
    {
        const auto* line = values.data() + row * columns;
        data.times.push_back(line[0]);
        for (std::size_t band = 0; band < bandCount; ++band)
            data.bands.push_back({line[1 + 2 * band], line[2 + 2 * band]});
    }
    return data;
}

bool VaryingBank::prepare(
    double sampleRate,
    const VaryingFilterData& data,
    double q,
    double gain,
    int harmonics,
    double rolloffDb,
    bool doubleFiltering)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || data.bandCount == 0 || data.times.empty()
        || data.bands.size() != data.times.size() * data.bandCount
        || data.times.front() != 0.0
        || !std::ranges::all_of(data.times, [](double time) { return std::isfinite(time) && time >= 0.0; })
        || !std::ranges::is_sorted(data.times)
        || !std::ranges::all_of(data.bands, [](const FilterBand& band) {
               return std::isfinite(band.frequency) && band.frequency > 0.0
                   && std::isfinite(band.amplitude) && band.amplitude > 0.0;
           })
        || !std::isfinite(q) || q < minimumQ || q > maximumQ
        || !std::isfinite(gain) || gain < minimumGain || gain > maximumGain
        || harmonics < 1 || harmonics > maximumHarmonics
        || !std::isfinite(rolloffDb) || rolloffDb < minimumRolloff || rolloffDb > 0.0
        || data.bandCount * static_cast<std::size_t>(harmonics) > maximumBands)
    {
        return false;
    }
    // get_data_from_tvary_infile ends the data with a copy of its last line
    // far beyond the sound, where the bands hold.
    std::vector<double> times = data.times;
    std::vector<FilterBand> bands = data.bands;
    times.push_back(std::numeric_limits<double>::infinity());
    bands.insert(bands.end(), data.bands.end() - static_cast<std::ptrdiff_t>(data.bandCount),
        data.bands.end());
    // fltpcon.c's FLTBANKV setup: dbtogain, then
    // put_tvarying_filter_data_in_arrays with the harmonics.
    const auto rolloff = dbtogain(rolloffDb);
    const auto count = data.bandCount * static_cast<std::size_t>(harmonics);
    std::vector<std::int64_t> sampleTimes;
    std::vector<double> lineFrequencies;
    std::vector<double> lineAmplitudes;
    for (std::size_t line = 0; line < times.size(); ++line)
    {
        sampleTimes.push_back(std::isfinite(times[line])
                ? static_cast<std::int64_t>(std::lround(times[line] * sampleRate))
                : std::numeric_limits<std::int64_t>::max());
        for (std::size_t band = 0; band < data.bandCount; ++band)
        {
            const auto& source = bands[line * data.bandCount + band];
            for (int j = 1; j <= harmonics; j++)
            {
                const auto frequency = source.frequency * static_cast<double>(j);
                if (frequency > sampleRate / 2.0)
                    return false;
                lineFrequencies.push_back(frequency);
            }
            auto atten = 1.0;
            for (int j = 1; j <= harmonics; j++)
            {
                lineAmplitudes.push_back(source.amplitude * atten);
                atten *= rolloff;
            }
        }
    }
    inverseSampleRate_ = 1.0 / sampleRate;
    startQ_ = q;
    doubleFiltering_ = doubleFiltering;
    startGain_ = gain;
    sampleTimes_ = std::move(sampleTimes);
    lineFrequencies_ = std::move(lineFrequencies);
    lineAmplitudes_ = std::move(lineAmplitudes);
    count_ = count;
    frequencies_.assign(count, 0.0);
    amplitudes_.assign(count, 0.0);
    frequencySteps_.assign(count, 1.0);
    amplitudeSteps_.assign(count, 1.0);
    lastFrequencies_.assign(count, 0.0);
    lastAmplitudes_.assign(count, 0.0);
    a_.assign(count, 0.0);
    b_.assign(count, 0.0);
    ampl_.assign(count, 0.0);
    y_.assign(count, 0.0);
    z_.assign(count, 0.0);
    d_.assign(count, 0.0);
    e_.assign(count, 0.0);
    reset();
    return true;
}

bool VaryingBank::setQ(double q) noexcept
{
    if (a_.empty() || !std::isfinite(q) || q < minimumQ || q > maximumQ)
        return false;
    nextQ_ = q;
    return true;
}

void VaryingBank::reset() noexcept
{
    // initialise_fltbankv_internal_params, then the first newfval.
    for (std::size_t n = 0; n < count_; ++n)
    {
        frequencies_[n] = lineFrequencies_[n];
        amplitudes_[n] = lineAmplitudes_[n];
        lastFrequencies_[n] = frequencies_[n];
        lastAmplitudes_[n] = amplitudes_[n];
        frequencySteps_[n] = 1.0;
        amplitudeSteps_[n] = 1.0;
    }
    std::ranges::fill(y_, 0.0);
    std::ranges::fill(z_, 0.0);
    std::ranges::fill(d_, 0.0);
    std::ranges::fill(e_, 0.0);
    gain_ = startGain_;
    q_ = nextQ_ = startQ_;
    frequencyIndex_ = count_;
    lineIndex_ = 1;
    blockCount_ = 0;
    segmentFrames_ = 0;
    nextSegment();
}

// newfval, statement for statement, holding after the last line.
void VaryingBank::nextSegment() noexcept
{
    if (lineIndex_ >= sampleTimes_.size())
    {
        segmentFrames_ = std::numeric_limits<std::int64_t>::max();
        return;
    }
    const auto k = lineIndex_;
    const auto lasttime = sampleTimes_[k - 1];
    const auto thistime = sampleTimes_[k];
    segmentFrames_ = thistime == std::numeric_limits<std::int64_t>::max()
        ? thistime : thistime - lasttime;
    const auto one_over_steps = static_cast<double>(blockSize) / static_cast<double>(segmentFrames_);
    const auto total = count_ * sampleTimes_.size();
    if (frequencyIndex_ >= total)
        return;
    for (std::size_t n = 0, m = frequencyIndex_; n < count_; n++, m++)
    {
        auto thisval = lineFrequencies_[m];
        if (filter_bank::flteq(lastFrequencies_[n], thisval))
            frequencySteps_[n] = 1.0;
        else
            frequencySteps_[n] = std::pow(thisval / lastFrequencies_[n], one_over_steps);
        frequencies_[n] = lastFrequencies_[n];
        lastFrequencies_[n] = thisval;
        thisval = lineAmplitudes_[m];
        if (filter_bank::flteq(thisval, lastAmplitudes_[n]))
            amplitudeSteps_[n] = 1.0;
        else
            amplitudeSteps_[n] = std::pow(thisval / lastAmplitudes_[n], one_over_steps);
        amplitudes_[n] = lastAmplitudes_[n];
        lastAmplitudes_[n] = thisval;
    }
    frequencyIndex_ += count_;
    lineIndex_++;
}

bool VaryingBank::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (a_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    // do_fvary_filters for one channel, statement for statement.
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        if (segmentFrames_ <= 0)
            nextSegment();
        if ((blockCount_ -= 1) <= 0)
        {
            // The Q its breakpoints give now.
            q_ = nextQ_;
            for (std::size_t fno = 0; fno < count_; fno++)
            {
                filter_bank::bandCoefficients(frequencies_[fno], amplitudes_[fno], q_,
                    inverseSampleRate_, doubleFiltering_, a_[fno], b_[fno], ampl_[fno]);
            }
            for (std::size_t m = 0; m < count_; m++)
            {
                frequencies_[m] *= frequencySteps_[m];
                amplitudes_[m] *= amplitudeSteps_[m];
            }
            blockCount_ = blockSize;
        }
        output[frame] = filter_bank::filterFrame(
            input[frame], a_, b_, ampl_, y_, z_, d_, e_, doubleFiltering_, gain_);
        if (segmentFrames_ != std::numeric_limits<std::int64_t>::max())
            segmentFrames_--;
    }
    return true;
}
}
