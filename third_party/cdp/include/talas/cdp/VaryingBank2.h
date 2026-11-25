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
#pragma once

#include "talas/cdp/UserBank.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// One partial of `filter varibank2`'s second data section: a multiple of
// each pitch (possibly fractional) and the amplitude it scales the pitch's
// amplitude by.
struct FilterPartial
{
    double number = 0.0;
    double amplitude = 0.0;

    bool operator==(const FilterPartial&) const = default;
};

// `filter varibank2`'s data: lines of a time and pitch and amplitude pairs,
// then a line starting with '#', then lines of a time and partial number
// and amplitude pairs, each section with the same number of pairs on each
// of its lines and times rising from exactly 0.
struct VaryingBank2Data
{
    std::vector<double> pitchTimes;
    // pitchCount pitches per line, line after line, in Hz.
    std::vector<FilterBand> pitches;
    std::size_t pitchCount = 0;
    std::vector<double> partialTimes;
    // partialCount partials per line, line after line.
    std::vector<FilterPartial> partials;
    std::size_t partialCount = 0;
};

// The data read as CDP's read_data_from_t_and_p_vary_infile reads its file,
// then check_seq_and_range_of_filter_data2: empty and semicolon lines
// skipped, every value may be given in dB, words after the '#' on its line
// ignored, pitches within 0.1 Hz (or its note number) to half the sample
// rate (or its note number), partial numbers within 1 to the sample rate
// over 0.2, and amplitudes raised to 0.001. CDP's line buffer holds the
// longest line but the '#' line, plus 3 characters: a longer '#' line runs
// on into the partials. Nothing for a '#' character before any line ends,
// no line ending after the '#' line, no '#', a second '#', no lines before
// or after it, a section's first line with fewer than three values or
// unpaired values, a line with another number of values than its section's
// first, a first time that is not exactly 0, a time that does not rise, a
// value out of range or not finite, or values before the '#' on its line
// that do not make whole lines (CDP then reads past its table).
[[nodiscard]] std::optional<VaryingBank2Data> readVaryingBank2Data(
    std::string_view text,
    FilterPitches pitches,
    double sampleRate);

// CDP `filter varibank2`: CDP's user bank whose pitches and partials each
// glide linearly from line to line of their sections of its data; each pitch
// sounds each partial's multiple of it, at its amplitude times the
// partial's, and as CDP writes each pitch's first partial over the pitch,
// the first partial's number and amplitude scale the others too. After its
// last line a section holds. The first 128 frames take the data at time 0;
// then CDP retunes every 128 frames, at the time a block further on each
// time, its count starting again with each of its sound buffers: no
// retuning where a buffer starts, and the data a block later from there on.
// An output beyond ±1 is clipped and lowers the gain by 0.9999 for good.
class VaryingBank2
{
public:
    static constexpr double minimumQ = 0.001;
    static constexpr double maximumQ = 10000.0;
    static constexpr double minimumGain = 0.001;
    static constexpr double maximumGain = 10000.0;
    // filtcon.h's FLT_MAX_FILTERS, which CDP's `filter bank` keeps and
    // VaryingBank keeps too; CDP's varibank2 itself has no cap.
    static constexpr std::size_t maximumBands = 2000;
    // CDP8's sound buffer for a mono input: its default 1 MB of memory in
    // one buffer (CDP_MEMORY_BBSIZE sets other sizes).
    static constexpr std::size_t defaultBufferFrames = 262144;

    // False, with no change, for a sample rate that is not positive, data
    // that readVaryingBank2Data would not give, more than maximumBands
    // pitches times partials, a Q or gain outside 0.001 to 10000, no buffer
    // frames, or, as CDP's test_pitch_and_spectrum_varying_filterdata finds
    // at every 128-frame block before the earlier of the sections' last
    // lines, a partial beyond half the sample rate (later ones sound). The Q is the first block's, and every
    // block's until setQ (CDP gives the first block its default Q of 20 when
    // the Q is a breakpoint file).
    [[nodiscard]] bool prepare(
        double sampleRate,
        const VaryingBank2Data& data,
        double q,
        double gain,
        bool doubleFiltering,
        std::size_t bufferFrames = defaultBufferFrames);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !a_.empty(); }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return 0; }

    // CDP takes the Q as a breakpoint file, which it reads at each frame
    // but uses only as it retunes its filters, every 128 frames from frame
    // 128: it takes effect at the next retuning. False, with no change,
    // before prepare or for a Q outside 0.001 to 10000.
    [[nodiscard]] bool setQ(double q) noexcept;

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    void newfval2(double thistime) noexcept;

    double inverseSampleRate_ = 0.0;
    double timestep_ = 0.0;
    double q_ = 1.0;
    // The Q the next retuning takes, and the one prepare gave.
    double nextQ_ = 1.0;
    double startQ_ = 1.0;
    bool doubleFiltering_ = false;
    double startGain_ = 1.0;
    std::size_t bufferFrames_ = defaultBufferFrames;
    // CDP's FLT_FBRK and FLT_HBRK tables with their FLT_ENTRYCNT,
    // FLT_WORDCNT, HRM_ENTRYCNT and HRM_WORDCNT, and FLT_HARMCNT and FLT_CNT.
    std::vector<double> fbrk_;
    std::vector<double> hbrk_;
    std::size_t entrycnt_ = 0;
    std::size_t wordcnt_ = 0;
    std::size_t hentrycnt_ = 0;
    std::size_t hwordcnt_ = 0;
    std::size_t harmcnt_ = 0;
    std::size_t count_ = 0;
    // CDP's FLT_INFRQ and FLT_INAMP (FLT_FRQ and FLT_AMP hold the same),
    // HARM_FRQ_CALC and HARM_AMP_CALC, and the filters.
    std::vector<double> frequencies_;
    std::vector<double> amplitudes_;
    std::vector<double> partialNumbers_;
    std::vector<double> partialAmplitudes_;
    std::vector<double> a_;
    std::vector<double> b_;
    std::vector<double> ampl_;
    std::vector<double> y_;
    std::vector<double> z_;
    std::vector<double> d_;
    std::vector<double> e_;
    double gain_ = 1.0;
    // CDP's FLT_TOTALTIME, do_fvary2_filters's fsams, and the frame in CDP's
    // sound buffer.
    double totalTime_ = 0.0;
    int fsams_ = 0;
    std::size_t bufferFrame_ = 0;
};
}
