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
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// One line of CDP's filter data: a frequency in Hz and an amplitude.
struct FilterBand
{
    double frequency = 0.0;
    double amplitude = 0.0;

    bool operator==(const FilterBand&) const = default;
};

// How the filter data gives its pitches: CDP's mode 1 (Hz) or 2 (MIDI note
// numbers, which CDP converts from A at 6.875 Hz).
enum class FilterPitches
{
    Hertz = 1,
    Midi,
};

// The bands a filter data file gives, read as CDP's `filter userbank` reads
// them: lines that are empty or start with a semicolon skipped, two values a
// line, a pitch and an amplitude, which may be given in dB ("-6dB"). Nothing
// for a line without exactly two values, a pitch outside 0.1 Hz (or its note
// number) to half the sample rate (or note 127), an amplitude outside 0.001
// to 10000, no bands, or more than UserBank::maximumBands.
[[nodiscard]] std::optional<std::vector<FilterBand>> readFilterBands(
    std::string_view text,
    FilterPitches pitches,
    double sampleRate);

// CDP `filter userbank`: a bank of two-pole resonators at the given
// frequencies, each with its amplitude and the bank's Q, summed and scaled
// by the gain; double filtering runs each band through its resonator twice.
// An output beyond ±1 is clipped and lowers the gain by 0.9999 for good, as
// CDP does to keep the bank from blowing up.
class UserBank
{
public:
    static constexpr double minimumQ = 0.001;
    static constexpr double maximumQ = 10000.0;
    static constexpr double minimumGain = 0.001;
    static constexpr double maximumGain = 10000.0;
    static constexpr std::size_t maximumBands = 2000;

    // False, with no change, for a sample rate that is not positive, no or
    // too many bands, a band whose frequency is not positive or whose
    // amplitude is outside 0.001 to 10000, a Q outside 0.001 to 10000, or a
    // gain outside 0.001 to 10000.
    [[nodiscard]] bool prepare(
        double sampleRate,
        const std::vector<FilterBand>& bands,
        double q,
        double gain,
        bool doubleFiltering);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !a_.empty(); }

    // CDP takes the Q as a breakpoint file and reads it at the start of each
    // 128-frame block, recomputing its filters: it takes effect at the next
    // block. False, with no change, before prepare or for a Q outside 0.001
    // to 10000.
    [[nodiscard]] bool setQ(double q) noexcept;

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept;

private:
    // initialise_filter_params, or do_qvary_filters's get_coeffs2 at a Q.
    void computeCoefficients(double q) noexcept;

    static constexpr int blockSize = 128;

    std::vector<double> frequencies_;
    std::vector<double> amplitudes_;
    double inverseSampleRate_ = 0.0;
    double q_ = 1.0;
    double nextQ_ = 1.0;
    double startQ_ = 1.0;
    int blockCount_ = 0;
    std::vector<double> a_;
    std::vector<double> b_;
    std::vector<double> ampl_;
    std::vector<double> y_;
    std::vector<double> z_;
    std::vector<double> d_;
    std::vector<double> e_;
    bool doubleFiltering_ = false;
    double startGain_ = 1.0;
    double gain_ = 1.0;
};
}
