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
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// CDP's time-varying filter data: lines of a time followed by the same
// number of pitch and amplitude pairs, read as `filter varibank` reads
// them: every value may be given in dB, empty and semicolon lines skipped,
// times rising from 0 (a first line after 0 is copied to 0), pitches within
// 0.1 Hz (or its note number) to half the sample rate (or note 127), and
// amplitudes raised to 0.001.
struct VaryingFilterData
{
    std::vector<double> times;
    // bands per line, line after line.
    std::vector<FilterBand> bands;
    std::size_t bandCount = 0;
};

// Nothing for lines of unequal or too few values, an unpaired pitch, a time
// that does not rise, a pitch out of range, or more than
// VaryingBank::maximumBands bands.
[[nodiscard]] std::optional<VaryingFilterData> readVaryingFilterData(
    std::string_view text,
    FilterPitches pitches,
    double sampleRate);

// CDP `filter varibank`: CDP's user bank whose bands' frequencies and
// amplitudes glide exponentially from line to line of its data, retuned
// every 128 frames, each band with Harmonics harmonics, each Rolloff dB
// below the last. After the last line the bands hold.
class VaryingBank
{
public:
    static constexpr double minimumQ = 0.001;
    static constexpr double maximumQ = 10000.0;
    static constexpr double minimumGain = 0.001;
    static constexpr double maximumGain = 10000.0;
    static constexpr int maximumHarmonics = 200;
    static constexpr double minimumRolloff = -96.0;
    static constexpr std::size_t maximumBands = 2000;

    // False, with no change, for a sample rate that is not positive, no
    // data, a Q or gain outside 0.001 to 10000, harmonics outside 1 to 200,
    // a rolloff outside -96 to 0 dB, a harmonic beyond half the sample rate,
    // or more than maximumBands bands with their harmonics.
    [[nodiscard]] bool prepare(
        double sampleRate,
        const VaryingFilterData& data,
        double q,
        double gain,
        int harmonics,
        double rolloffDb,
        bool doubleFiltering);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !a_.empty(); }

    // CDP takes the Q as a breakpoint file and reads it at the start of each
    // 128-frame block: it takes effect at the next block. False, with no
    // change, before prepare or for a Q outside 0.001 to 10000.
    [[nodiscard]] bool setQ(double q) noexcept;

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    void nextSegment() noexcept;

    double inverseSampleRate_ = 0.0;
    double q_ = 1.0;
    // The Q the next block takes, and the one prepare gave.
    double nextQ_ = 1.0;
    double startQ_ = 1.0;
    bool doubleFiltering_ = false;
    double startGain_ = 1.0;
    // The lines, harmonics included, and their frames.
    std::vector<std::int64_t> sampleTimes_;
    std::vector<double> lineFrequencies_;
    std::vector<double> lineAmplitudes_;
    std::size_t count_ = 0;
    // CDP's FLT_FRQ, FLT_AMP, FLT_FINCR, FLT_AINCR, FLT_LASTFVAL and
    // FLT_LASTAVAL, and the filters.
    std::vector<double> frequencies_;
    std::vector<double> amplitudes_;
    std::vector<double> frequencySteps_;
    std::vector<double> amplitudeSteps_;
    std::vector<double> lastFrequencies_;
    std::vector<double> lastAmplitudes_;
    std::vector<double> a_;
    std::vector<double> b_;
    std::vector<double> ampl_;
    std::vector<double> y_;
    std::vector<double> z_;
    std::vector<double> d_;
    std::vector<double> e_;
    double gain_ = 1.0;
    std::int64_t segmentFrames_ = 0;
    int blockCount_ = 0;
    std::size_t lineIndex_ = 1;
    std::size_t frequencyIndex_ = 0;
};
}
