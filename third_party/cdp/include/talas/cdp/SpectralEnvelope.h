/*
 * Derived from the CDP System, Release 8: dev/cdp2k/formantsg.c, tklib1.c,
 * mainfuncs.c and dzsetup.c
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

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP's formant (spectral) envelope of an analysis frame, as its formant
// programs (`blur spread`, `strange glis`, `formants` ...) take it with
// -fN or -pN and -i: band edges set once for the frame geometry
// (set_specenv_frqs), each band's mean channel amplitude taken from a frame
// (extract_specenv), and the amplitude at any frequency read by linear
// interpolation over the bands' log10 pitches (getspecenvamp).
//
// Frequency-wise (-fN), a point every N half channel widths from half a
// channel up; pitch-wise (-pN), four octave bands over the lowest eight
// channels, then N bands an octave. CDP sums the channels whose
// frequencies fall in each band, searching four channels beyond its edges,
// or with -i (quick search) assigns channels to bands by their positions,
// ignoring their frequencies.
class SpectralEnvelope
{
public:
    // CDP's MAX_BANDS_PER_OCT and FBAND_DEFAULT.
    static constexpr int maximumPitchwiseBands = 12;
    static constexpr int defaultBands = 4;

    struct BandRange
    {
        int minimum = 0;
        int maximum = 0;
    };

    // The band counts prepare takes for an analysis frame of frameSize
    // floats (points + 2, PhaseVocoder::frameSize()): as CDP's command-line
    // programs check them (establish_formant_band_ranges, with their least
    // count left at 0), 1 to (channels - 1) / 2 frequency-wise (none would
    // overrun set_specenv_frqs's arrays) and 0 to 12 pitch-wise where there
    // are at least five channels (0 keeps only the four low octave bands),
    // less any count that fills set_specenv_frqs's arrays (in some frames
    // of under 80 points), which CDP takes but then writes its Nyquist point
    // past them. Nothing when there is none.
    [[nodiscard]] static std::optional<BandRange> bandRange(std::size_t frameSize, bool pitchwise) noexcept;

    // CDP's -pN (pitchwise) or -fN with N bands, and -i (quickSearch), for
    // frames of frameSize floats at that sample rate. False, with no
    // change, for a sample rate that is not positive or bands outside
    // bandRange.
    [[nodiscard]] bool prepare(double sampleRate, std::size_t frameSize, bool pitchwise, int bands, bool quickSearch);

    [[nodiscard]] bool prepared() const noexcept { return !frq_.empty(); }

    // extract_specenv: the envelope of a frame of the prepared size (CDP
    // rectifies the frame first). Ignored before prepare or for another
    // size.
    void extract(std::span<const float> frame) noexcept;

    // getspecenvamp: the envelope's amplitude at the frequency, zero below
    // 0 Hz; zero before prepare.
    [[nodiscard]] double amplitudeAt(double frequency) const noexcept;

    // The envelope's points (CDP's specenvcnt of them): their frequencies,
    // log10 pitches, band tops and, after extract, amplitudes.
    [[nodiscard]] std::span<const float> frequencies() const noexcept { return frq_; }
    [[nodiscard]] std::span<const float> pitches() const noexcept { return pch_; }
    [[nodiscard]] std::span<const float> tops() const noexcept { return top_; }
    [[nodiscard]] std::span<const float> amplitudes() const noexcept { return amp_; }

    // CDP's nyquist, chwidth and halfchwidth after prepare.
    [[nodiscard]] double nyquist() const noexcept { return nyquist_; }
    [[nodiscard]] double channelWidth() const noexcept { return chwidth_; }
    [[nodiscard]] double halfChannelWidth() const noexcept { return halfchwidth_; }

private:
    int clength_ = 0;
    bool quickSearch_ = false;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    double halfchwidth_ = 0.0;
    // specenvfrq, specenvpch, specenvtop and specenvamp.
    std::vector<float> frq_;
    std::vector<float> pch_;
    std::vector<float> top_;
    std::vector<float> amp_;
};
}
