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
#include "talas/cdp/VaryingBank2.h"

#include "FilterBankData.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace talas::cdp
{
namespace
{
// filtcon.h's BSIZE.
constexpr int blockSize = 128;

// ap_filter.c's getmaxlinelen2: the longest line, the '#' line and the
// characters before its '#' not counted, plus 4. Nothing for a '#' before
// any line ends, or no line ending after the '#' line. Its (char)fgetc(fp)
// != EOF also stops at a 0xff byte.
[[nodiscard]] std::optional<std::size_t> getmaxlinelen2(std::string_view text) noexcept
{
    std::size_t maxcnt = 0;
    std::size_t thiscnt = 0;
    auto gothash = false;
    auto gotdata1 = false;
    auto gotdata2 = false;
    std::size_t position = 0;
    char c = 0;
    const auto next = [&]() noexcept {
        if (position >= text.size() || static_cast<unsigned char>(text[position]) == 0xff)
            return false;
        c = text[position++];
        return true;
    };
    while (next())
    {
        if (!gothash)
        {
            if (c == '#')
            {
                if (!gotdata1)
                    return std::nullopt;
                while (next())
                {
                    if (c == '\n' || c == '\0')
                    {
                        gothash = true;
                        thiscnt = 0;
                        break;
                    }
                }
                if (!gothash)
                    break;
            }
            else
            {
                if (c == '\n' || c == '\0')
                {
                    maxcnt = std::max(maxcnt, thiscnt);
                    thiscnt = 0;
                    gotdata1 = true;
                }
                else
                    thiscnt++;
            }
        }
        else
        {
            if (c == '\n' || c == '\0')
            {
                maxcnt = std::max(maxcnt, thiscnt);
                thiscnt = 0;
                gotdata2 = true;
            }
            else
                thiscnt++;
        }
    }
    if (!gotdata2)
        return std::nullopt;
    maxcnt = std::max(maxcnt, thiscnt);
    maxcnt += 4;
    return maxcnt;
}

// One section's table as CDP holds it: lines of a time and pairs.
struct Table
{
    std::vector<double> words;
    std::size_t entrycnt = 0;
    std::size_t wordcnt = 0;
};

// newfval2's and test_pitch_and_spectrum_varying_filterdata's search: the
// last line at or before the time, as the index of its time.
[[nodiscard]] std::size_t findLine(const Table& table, double thistime) noexcept
{
    std::size_t timepoint = 0;
    while (thistime >= table.words[timepoint])
    {
        timepoint += table.entrycnt;
        if (timepoint >= table.wordcnt)
            break;
    }
    timepoint -= table.entrycnt;
    return timepoint;
}

// prepare's tables from the data.
void makeTables(const VaryingBank2Data& data, Table& fbrk, Table& hbrk)
{
    fbrk.entrycnt = 1 + 2 * data.pitchCount;
    fbrk.words.clear();
    for (std::size_t line = 0; line < data.pitchTimes.size(); ++line)
    {
        fbrk.words.push_back(data.pitchTimes[line]);
        for (std::size_t n = 0; n < data.pitchCount; ++n)
        {
            const auto& band = data.pitches[line * data.pitchCount + n];
            fbrk.words.push_back(band.frequency);
            fbrk.words.push_back(band.amplitude);
        }
    }
    fbrk.wordcnt = fbrk.words.size();
    hbrk.entrycnt = 1 + 2 * data.partialCount;
    hbrk.words.clear();
    for (std::size_t line = 0; line < data.partialTimes.size(); ++line)
    {
        hbrk.words.push_back(data.partialTimes[line]);
        for (std::size_t n = 0; n < data.partialCount; ++n)
        {
            const auto& partial = data.partials[line * data.partialCount + n];
            hbrk.words.push_back(partial.number);
            hbrk.words.push_back(partial.amplitude);
        }
    }
    hbrk.wordcnt = hbrk.words.size();
}

// fltpcon.c's test_pitch_and_spectrum_varying_filterdata, statement for
// statement: false for a partial beyond half the sample rate at a block
// before both sections' last lines. Each pitch's first partial overwrites
// the pitch, which its other partials then multiply, as in newfval2.
[[nodiscard]] bool testPitchAndSpectrum(const Table& fbrk, const Table& hbrk, double sampleRate)
{
    const auto harmcnt = (hbrk.entrycnt - 1) / 2;
    const auto count = ((fbrk.entrycnt - 1) / 2) * harmcnt;
    std::vector<double> infrq(count, 0.0);
    std::vector<double> harmFrq(harmcnt, 0.0);
    std::size_t n, m, k, timepoint, htimepoint, nextrow;
    double thistime, lotime, hitime, timefrac, valdiff;
    const auto timestep = static_cast<double>(blockSize) / sampleRate;
    auto lasttime = fbrk.words[fbrk.wordcnt - fbrk.entrycnt];
    lasttime = std::min(lasttime, hbrk.words[hbrk.wordcnt - hbrk.entrycnt]);
    for (thistime = 0.0; thistime < lasttime; thistime += timestep)
    {
        timepoint = findLine(fbrk, thistime);
        lotime = fbrk.words[timepoint];
        nextrow = timepoint + fbrk.entrycnt;
        for (n = timepoint + 1, k = 0; n < nextrow; n += 2, k += harmcnt)
            infrq[k] = fbrk.words[n];
        if (nextrow != fbrk.wordcnt)
        {
            nextrow += fbrk.entrycnt;
            timepoint += fbrk.entrycnt;
            hitime = fbrk.words[timepoint];
            timefrac = (thistime - lotime) / (hitime - lotime);
            for (n = timepoint + 1, k = 0; n < nextrow; n += 2, k += harmcnt)
            {
                valdiff = fbrk.words[n] - infrq[k];
                valdiff *= timefrac;
                infrq[k] = infrq[k] + valdiff;
            }
        }
        htimepoint = findLine(hbrk, thistime);
        lotime = hbrk.words[htimepoint];
        nextrow = htimepoint + hbrk.entrycnt;
        for (n = htimepoint + 1, k = 0; n < nextrow; n += 2, k++)
            harmFrq[k] = hbrk.words[n];
        if (nextrow != hbrk.wordcnt)
        {
            nextrow += hbrk.entrycnt;
            htimepoint += hbrk.entrycnt;
            hitime = hbrk.words[htimepoint];
            timefrac = (thistime - lotime) / (hitime - lotime);
            for (n = htimepoint + 1, k = 0; n < nextrow; n += 2, k++)
            {
                valdiff = hbrk.words[n] - harmFrq[k];
                valdiff *= timefrac;
                harmFrq[k] = harmFrq[k] + valdiff;
            }
        }
        for (k = 0; k < count; k += harmcnt)
        {
            for (m = 0; m < harmcnt; m++)
            {
                if ((infrq[k + m] = infrq[k] * harmFrq[m]) > sampleRate / 2.0)
                    return false;
            }
        }
    }
    return true;
}
}

std::optional<VaryingBank2Data> readVaryingBank2Data(
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
    // read_data_from_t_and_p_vary_infile, statement for statement, reading
    // with fgets into getmaxlinelen2's buffer.
    const auto maxlinelen = getmaxlinelen2(text);
    if (!maxlinelen.has_value())
        return std::nullopt;
    std::vector<double> fbrk;
    std::vector<double> hbrk;
    std::size_t total_wordcnt = 0;
    std::size_t number_of_rows = 0;
    std::size_t columns_in_this_row = 0;
    std::size_t columns_in_row = 0;
    std::size_t fwordcnt = 0;
    std::size_t fentrycnt = 0;
    auto harmdata = false;
    auto ishash = false;
    std::size_t position = 0;
    while (position < text.size())
    {
        // fgets(temp, maxlinelen, fp): up to maxlinelen - 1 characters,
        // through a newline; the string ends at a NUL.
        auto end = position;
        while (end < text.size() && end - position < *maxlinelen - 1)
        {
            if (text[end++] == '\n')
                break;
        }
        auto temp = text.substr(position, end - position);
        position = end;
        temp = temp.substr(0, temp.find('\0'));
        columns_in_this_row = 0;
        if (emptyOrComment(temp))
            continue;
        for (const auto thisword : words(temp))
        {
            if (thisword.front() == '#')
            {
                if (harmdata)
                    return std::nullopt;
                fwordcnt = total_wordcnt;
                fentrycnt = columns_in_row;
                total_wordcnt = 0;
                number_of_rows = 0;
                harmdata = true;
                ishash = true;
                break;
            }
            const auto val = level(thisword);
            if (!val.has_value() || !std::isfinite(*val))
                return std::nullopt;
            (harmdata ? hbrk : fbrk).push_back(*val);
            columns_in_this_row++;
            total_wordcnt++;
        }
        if (ishash)
        {
            ishash = false;
            continue;
        }
        if (number_of_rows == 0)
        {
            if ((columns_in_row = columns_in_this_row) < 3)
                return std::nullopt;
            else if ((columns_in_row - 1) % 2 != 0)
                return std::nullopt;
        }
        else if (columns_in_this_row != columns_in_row)
            return std::nullopt;
        number_of_rows++;
    }
    const auto hwordcnt = total_wordcnt;
    if (columns_in_row < 3)
        return std::nullopt;
    const auto hentrycnt = columns_in_row;
    // CDP reads FLT_FBRK and FLT_HBRK unset without a '#' or lines on each
    // side of it, and its search and interpolation run past FLT_FBRK when
    // values before the '#' on its line do not make whole lines.
    if (!harmdata || fbrk.empty() || hbrk.empty() || fentrycnt < 3 || fwordcnt % fentrycnt != 0)
        return std::nullopt;
    if (fbrk[0] != 0.0 || hbrk[0] != 0.0)
        return std::nullopt;
    // check_seq_and_range_of_filter_data2.
    auto lasttime = -1.0;
    const auto maxpartial = sampleRate / (2.0 * minimumFrequency);
    const auto minpartial = 1.0;
    const auto frqmax = sampleRate / 2.0;
    const auto midimax = uncheckedHztomidi(frqmax);
    const auto midimin = uncheckedHztomidi(minimumFrequency);
    for (std::size_t n = 0; n < fwordcnt; n++)
    {
        const auto m = n % fentrycnt;
        if (m == 0)
        {
            if (fbrk[n] <= lasttime)
                return std::nullopt;
            lasttime = fbrk[n];
        }
        else if (m % 2 == 1)
        {
            if (pitches == FilterPitches::Midi)
            {
                if (fbrk[n] < midimin || fbrk[n] > midimax)
                    return std::nullopt;
                fbrk[n] = miditohz(fbrk[n]);
            }
            else if (fbrk[n] < minimumFrequency || fbrk[n] > frqmax)
                return std::nullopt;
        }
        else
            fbrk[n] = std::max(fbrk[n], minimumFilterAmplitude);
    }
    lasttime = -1.0;
    for (std::size_t n = 0; n < hwordcnt; n++)
    {
        const auto m = n % hentrycnt;
        if (m == 0)
        {
            if (hbrk[n] <= lasttime)
                return std::nullopt;
            lasttime = hbrk[n];
        }
        else if (m % 2 == 1)
        {
            if (hbrk[n] < minpartial || hbrk[n] > maxpartial)
                return std::nullopt;
        }
        else
            hbrk[n] = std::max(hbrk[n], minimumFilterAmplitude);
    }
    VaryingBank2Data data;
    data.pitchCount = (fentrycnt - 1) / 2;
    for (std::size_t row = 0; row < fwordcnt / fentrycnt; ++row)
    {
        const auto* line = fbrk.data() + row * fentrycnt;
        data.pitchTimes.push_back(line[0]);
        for (std::size_t n = 0; n < data.pitchCount; ++n)
            data.pitches.push_back({line[1 + 2 * n], line[2 + 2 * n]});
    }
    data.partialCount = (hentrycnt - 1) / 2;
    for (std::size_t row = 0; row < hwordcnt / hentrycnt; ++row)
    {
        const auto* line = hbrk.data() + row * hentrycnt;
        data.partialTimes.push_back(line[0]);
        for (std::size_t n = 0; n < data.partialCount; ++n)
            data.partials.push_back({line[1 + 2 * n], line[2 + 2 * n]});
    }
    return data;
}

bool VaryingBank2::prepare(
    double sampleRate,
    const VaryingBank2Data& data,
    double q,
    double gain,
    bool doubleFiltering,
    std::size_t bufferFrames)
{
    const auto risingFromZero = [](const std::vector<double>& times) {
        return !times.empty() && times.front() == 0.0
            && std::ranges::all_of(times, [](double time) { return std::isfinite(time); })
            && std::ranges::adjacent_find(times, std::greater_equal<>{}) == times.end();
    };
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || data.pitchCount == 0 || data.partialCount == 0
        || !risingFromZero(data.pitchTimes) || !risingFromZero(data.partialTimes)
        || data.pitches.size() != data.pitchTimes.size() * data.pitchCount
        || data.partials.size() != data.partialTimes.size() * data.partialCount
        || !std::ranges::all_of(data.pitches, [](const FilterBand& band) {
               return std::isfinite(band.frequency) && band.frequency > 0.0
                   && std::isfinite(band.amplitude)
                   && band.amplitude >= filter_bank::minimumFilterAmplitude;
           })
        || !std::ranges::all_of(data.partials, [&](const FilterPartial& partial) {
               return std::isfinite(partial.number) && partial.number >= 1.0
                   && partial.number <= sampleRate / (2.0 * filter_bank::minimumFrequency)
                   && std::isfinite(partial.amplitude)
                   && partial.amplitude >= filter_bank::minimumFilterAmplitude;
           })
        || data.pitchCount > maximumBands || data.pitchCount * data.partialCount > maximumBands
        || !std::isfinite(q) || q < minimumQ || q > maximumQ
        || !std::isfinite(gain) || gain < minimumGain || gain > maximumGain
        || bufferFrames == 0)
    {
        return false;
    }
    Table fbrk;
    Table hbrk;
    makeTables(data, fbrk, hbrk);
    if (!testPitchAndSpectrum(fbrk, hbrk, sampleRate))
        return false;
    const auto count = data.pitchCount * data.partialCount;
    // filter_pconsistency's FLT_INV_SR, and filter_process's FLT_TIMESTEP.
    inverseSampleRate_ = 1.0 / sampleRate;
    timestep_ = static_cast<double>(blockSize) / sampleRate;
    startQ_ = q;
    doubleFiltering_ = doubleFiltering;
    startGain_ = gain;
    bufferFrames_ = bufferFrames;
    fbrk_ = std::move(fbrk.words);
    hbrk_ = std::move(hbrk.words);
    entrycnt_ = fbrk.entrycnt;
    wordcnt_ = fbrk.wordcnt;
    hentrycnt_ = hbrk.entrycnt;
    hwordcnt_ = hbrk.wordcnt;
    harmcnt_ = data.partialCount;
    count_ = count;
    frequencies_.assign(count, 0.0);
    amplitudes_.assign(count, 0.0);
    partialNumbers_.assign(harmcnt_, 0.0);
    partialAmplitudes_.assign(harmcnt_, 0.0);
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

bool VaryingBank2::setQ(double q) noexcept
{
    if (a_.empty() || !std::isfinite(q) || q < minimumQ || q > maximumQ)
        return false;
    nextQ_ = q;
    return true;
}

void VaryingBank2::reset() noexcept
{
    std::ranges::fill(y_, 0.0);
    std::ranges::fill(z_, 0.0);
    std::ranges::fill(d_, 0.0);
    std::ranges::fill(e_, 0.0);
    gain_ = startGain_;
    q_ = nextQ_ = startQ_;
    // filter_pconsistency's newfval2 at time 0, whose bands
    // initialise_filter_params tunes at the Q CDP holds then; then
    // filter_process's newfval2 at time 0 again, which leaves FLT_TOTALTIME
    // one FLT_TIMESTEP on.
    totalTime_ = 0.0;
    newfval2(totalTime_);
    for (std::size_t fno = 0; fno < count_; fno++)
    {
        filter_bank::bandCoefficients(frequencies_[fno], amplitudes_[fno], q_,
            inverseSampleRate_, doubleFiltering_, a_[fno], b_[fno], ampl_[fno]);
    }
    fsams_ = blockSize;
    bufferFrame_ = 0;
}

// newfval2, statement for statement: the pitches and partials at the time,
// each section interpolated linearly between its lines and holding after its
// last, then each pitch's partials. The first partial overwrites its pitch,
// which the other partials then multiply.
void VaryingBank2::newfval2(double thistime) noexcept
{
    std::size_t n, m, k, timepoint, nextrow;
    double lotime, hitime, timefrac, valdiff;
    auto* infrq = frequencies_.data();
    auto* inamp = amplitudes_.data();
    const auto* fbrk = fbrk_.data();
    const auto* hbrk = hbrk_.data();
    timepoint = 0;
    while (thistime >= fbrk[timepoint])
    {
        timepoint += entrycnt_;
        if (timepoint >= wordcnt_)
            break;
    }
    timepoint -= entrycnt_;
    lotime = fbrk[timepoint];
    nextrow = timepoint + entrycnt_;
    for (n = timepoint + 1, k = 0; n < nextrow; n += 2, k += harmcnt_)
    {
        infrq[k] = fbrk[n];
        inamp[k] = fbrk[n + 1];
    }
    if (nextrow != wordcnt_)
    {
        nextrow += entrycnt_;
        timepoint += entrycnt_;
        hitime = fbrk[timepoint];
        timefrac = (thistime - lotime) / (hitime - lotime);
        for (n = timepoint + 1, k = 0; n < nextrow; n += 2, k += harmcnt_)
        {
            valdiff = fbrk[n] - infrq[k];
            valdiff *= timefrac;
            infrq[k] = infrq[k] + valdiff;
            valdiff = fbrk[n + 1] - inamp[k];
            valdiff *= timefrac;
            inamp[k] = inamp[k] + valdiff;
        }
    }
    timepoint = 0;
    while (thistime >= hbrk[timepoint])
    {
        timepoint += hentrycnt_;
        if (timepoint >= hwordcnt_)
            break;
    }
    timepoint -= hentrycnt_;
    lotime = hbrk[timepoint];
    nextrow = timepoint + hentrycnt_;
    for (n = timepoint + 1, k = 0; n < nextrow; n += 2, k++)
    {
        partialNumbers_[k] = hbrk[n];
        partialAmplitudes_[k] = hbrk[n + 1];
    }
    if (nextrow != hwordcnt_)
    {
        nextrow += hentrycnt_;
        timepoint += hentrycnt_;
        hitime = hbrk[timepoint];
        timefrac = (thistime - lotime) / (hitime - lotime);
        for (n = timepoint + 1, k = 0; n < nextrow; n += 2, k++)
        {
            valdiff = hbrk[n] - partialNumbers_[k];
            valdiff *= timefrac;
            partialNumbers_[k] = partialNumbers_[k] + valdiff;
            valdiff = hbrk[n + 1] - partialAmplitudes_[k];
            valdiff *= timefrac;
            partialAmplitudes_[k] = partialAmplitudes_[k] + valdiff;
        }
    }
    for (k = 0; k < count_; k += harmcnt_)
    {
        for (m = 0; m < harmcnt_; m++)
        {
            infrq[k + m] = infrq[k] * partialNumbers_[m];
            inamp[k + m] = inamp[k] * partialAmplitudes_[m];
        }
    }
    totalTime_ += timestep_;
}

bool VaryingBank2::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (a_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    // do_fvary2_filters for one channel, statement for statement, its fsams
    // starting again at FLT_FSAMS, a block, with each of CDP's buffers.
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        if (bufferFrame_ == 0)
            fsams_ = blockSize;
        if (fsams_ <= 0)
        {
            // The Q its breakpoints give now.
            q_ = nextQ_;
            newfval2(totalTime_);
            fsams_ = blockSize;
            for (std::size_t fno = 0; fno < count_; fno++)
            {
                filter_bank::bandCoefficients(frequencies_[fno], amplitudes_[fno], q_,
                    inverseSampleRate_, doubleFiltering_, a_[fno], b_[fno], ampl_[fno]);
            }
        }
        output[frame] = filter_bank::filterFrame(
            input[frame], a_, b_, ampl_, y_, z_, d_, e_, doubleFiltering_, gain_);
        fsams_--;
        if (++bufferFrame_ == bufferFrames_)
            bufferFrame_ = 0;
    }
    return true;
}
}
