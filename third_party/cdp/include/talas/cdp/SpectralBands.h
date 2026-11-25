/*
 * Derived from the CDP System, Release 8: dev/hilite/hilite.c and
 * ap_hilite.c, and dev/cdp2k/tklib1.c, tklib3.c and special.c
 * Copyright (c) 1983-2023 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#pragma once

#include "talas/cdp/PhaseVocoder.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// CDP `hilite greq` and `hilite band` on each frame between `pvoc anal` and
// `pvoc synth`. Both take their bands from a data file, read once: nothing
// is live, and each frame is processed on its own.

// `hilite greq`: a graphic equaliser, each frame's amplitudes multiplied by
// a contour of raised-cosine bands around centre frequencies, each band its
// bandwidth in octaves wide, read at each channel's frequency; with -r the
// contour turned upside down, a notch filter. CDP leaves the first frame as
// it is. As CDP does, every band's skirt takes its shape from the last
// band's bandwidth (its only scale), and bands out of frequency order are
// sorted with a slip that repeats frequencies and makes others bandwidths.
class HiliteGraphicEq
{
public:
    enum class Mode
    {
        // One bandwidth, then the centre frequencies.
        OneBandwidth = 1,
        // A centre frequency and a bandwidth for each band.
        Bandwidths,
    };

    // A band as the data file gives it.
    struct Band
    {
        double frequency = 0.0;
        double bandwidth = 0.0;
    };

    // CDP's SPEC_MINFRQ and OCTAVES_PER_SEMITONE.
    static constexpr double minimumFrequency = 10.0;
    static constexpr double minimumBandwidth = 0.08333333333;

    // CDP's range for a centre frequency, 10 Hz to two thirds of half the
    // sample rate, and for a bandwidth, a twelfth of an octave to
    // LOG2(half the sample rate - 10).
    [[nodiscard]] static double maximumFrequency(double sampleRate) noexcept;
    [[nodiscard]] static double maximumBandwidth(double sampleRate) noexcept;

    // The bands in the file's order, as CDP reads them (see
    // readHiliteGraphicEq). False, with no change, for no bands, a frequency
    // or bandwidth outside those ranges or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(
        double sampleRate, int points, int overlap, std::span<const Band> bands, bool notch);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void filter(std::span<float> frame) noexcept;

    PhaseVocoder vocoder_;
    bool notch_ = false;
    double scalefact_ = 0.0;
    std::vector<double> filtpeak_;
    std::vector<double> fbandbot_;
    std::vector<double> fbandtop_;
    std::vector<float> fsampbuf_;
    std::size_t frames_ = 0;
};

// The bands of a `hilite greq` data file, in its order, as get_filt_params
// reads it: in mode 1 a bandwidth, then centre frequencies; in mode 2 pairs
// of a centre frequency and a bandwidth. Numbers are plain decimals ("-",
// digits and one "."), any number of them a line; a line is read up to the
// first word that is not one, so a ';' comment, or the rest of a line from
// any other word, is skipped; CDP reads at most 199 characters at a time,
// so a longer line splits there. Nothing for no
// bands or a value left unpaired. The ranges, which depend on the sample
// rate, are prepare's.
[[nodiscard]] std::optional<std::vector<HiliteGraphicEq::Band>> readHiliteGraphicEq(
    std::string_view text, HiliteGraphicEq::Mode mode);

// `hilite band`: the spectrum split into bands, each with its own
// amplitude change, a ramp from one amplitude at its low frequency to
// another at its high frequency, and a transposition, by a ratio or by
// added Hz, its partials moving to the channels of their new frequencies,
// replacing what is there and, unless added to the spectrum, leaving the
// mean of their neighbours behind. A band takes the channels from the first
// at or above its low frequency to the first at or above its high one.
// Channels outside the bands pass, their frequencies made positive. Where
// an added transposition takes a partial below 0 Hz CDP stops with an
// error; the stream leaves it in its channel, as at the Nyquist or above.
class HiliteBand
{
public:
    // A line of the data file: lofrq hifrq bitflag [amp1 [amp2]]
    // [[+]transpose].
    struct Band
    {
        double lowFrequency = 0.0;
        double highFrequency = 0.0;
        // The bitflag's four digits, left to right.
        bool changeAmplitude = false;
        bool rampAmplitude = false;
        bool transpose = false;
        bool addToSpectrum = false;
        // amp1, amp2 and the transposition, a ratio, or Hz with '+'. Without
        // changeAmplitude amp1 is taken as zero, as CDP's unset field reads
        // here: a ramp alone runs from zero at lofrq to amp2 at hifrq.
        // The ramp is over each channel's frequency within the band, not
        // over time.
        double amplitude = 0.0;
        double endAmplitude = 0.0;
        double transposition = 1.0;
        bool additive = false;
    };

    // The bands in the file's order. CDP takes each band's lower and higher
    // frequency in either order, the higher held to half the sample rate,
    // and sorts the bands by their lower frequency, with a sort that can
    // leave them out of order. False, with no change, for a band's first
    // frequency outside 0 to half the sample rate, its second below zero,
    // the two equal (within CDP's FLTERR), a band whose top passes the next
    // one's bottom after that sort, an addition without a transposition, a
    // first band with no digit set, a ratio below zero (CDP stops at the
    // first frame then) or what the phase vocoder refuses. No bands pass the
    // spectrum, its frequencies made positive.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, std::span<const Band> bands);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    // CDP's struct bandd.
    struct Bandd
    {
        int bdoflag = 0;
        int badditive = 0;
        double bfrqlo = 0.0;
        double bfrqhi = 0.0;
        double bfrqdif = 0.0;
        double bamp = 0.0;
        double bampdif = 0.0;
        double btrans = 0.0;
    };

    void split(std::span<float> frame) noexcept;
    void channel(int cc, const Bandd& bb) noexcept;

    PhaseVocoder vocoder_;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    double halfchwidth_ = 0.0;
    std::vector<Bandd> bands_;
    std::vector<float> amp_;
    std::vector<float> freq_;
};

// The lines of a `hilite band` data file, as read_specsplit_data reads
// them: blank lines skipped, each other line "lofrq hifrq bitflag", then
// amp1 if the bitflag's first digit is 1, amp2 if its second, and the
// transposition, '+' before it for added Hz, if its third; its fourth adds
// the transposed partials to the spectrum. Numbers are plain decimals ("-",
// digits and one "."); the bitflag is a word whose first four characters
// are 0 or 1; the rest of a line is ignored; CDP reads at most 199
// characters at a time, so a longer line splits there into two. Nothing
// for a line without those (a ';' comment included), a frequency
// below zero, a fourth digit without the third, or a first line with no
// digit set (CDP counts set digits over the whole file, so a later line may
// have none: it changes nothing but still takes up its band). A second
// digit without the first is CDP's ramp from zero. The checks that depend
// on the sample rate, and the sorting, are prepare's.
[[nodiscard]] std::optional<std::vector<HiliteBand::Band>> readHiliteBand(std::string_view text);
}
