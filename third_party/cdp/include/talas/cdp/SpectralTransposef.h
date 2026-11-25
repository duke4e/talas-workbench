/*
 * Derived from the CDP System, Release 8: dev/repitch/repitch.c and
 * ap_repitch.c, dev/cdp2k/formantsg.c, tklib1.c, tklib3.c and readdata.c
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
#include "talas/cdp/SpectralEnvelope.h"
#include "talas/cdp/SpectralPitch.h"

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `repitch transposef` on each frame between `pvoc anal` and `pvoc
// synth`: as `repitch transpose`, every partial's frequency is multiplied by
// the transposition (a ratio, octaves or semitones) and the partials move
// to the channels of their new frequencies, optionally (CDP's -x) to nearby
// empty ones for a fuller spectrum; but each partial that stays under the
// Nyquist frequency takes the frame's formant envelope (CDP's -fN or -pN
// with N bands, and -i for its quick search; see SpectralEnvelope) at its
// new frequency in proportion to its old one, so the spectral envelope
// stays where it was. CDP's -l and -h silence channels outside a band, and
// the frame keeps its total amplitude. CDP leaves the first frame as it is.
//
// CDP takes the transposition and the band frequencies as breakpoint files
// and reads them at each analysis frame's time: a value given before a
// process call reaches the frames analysed during it. CDP's mode 4, a
// binary file of a ratio for each frame, is a live transposition in ratios.
class RepitchTransposeFormants
{
public:
    using Units = RepitchTranspose::Units;

    // CDP's MIN_TRANSPOS and MAX_TRANSPOS, eight octaves either way.
    static constexpr double minimumRatio = RepitchTranspose::minimumRatio;
    static constexpr double maximumRatio = RepitchTranspose::maximumRatio;

    // A live transposition is converted as CDP converts a breakpoint file
    // (semitones over 12), a static one as CDP converts a constant (times
    // its OCTAVES_PER_SEMITONE). Static low and high frequencies of zero are
    // CDP's -l and -h left out; a live one is CDP's breakpoint value, raised
    // to 10 Hz, and a static one under 10 Hz zero, as setup_pitchzero makes
    // them (a band whose high frequency is under the low one is swapped).
    // False, with no change, for a transposition outside eight octaves (or,
    // at sample rates under 4700 Hz, CDP's 9 Hz over half the sample rate to
    // its inverse), band frequencies beyond half the sample rate, bands
    // outside SpectralEnvelope::bandRange for the frame or what the phase
    // vocoder refuses. The bands, -i and -x are read once.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Units units, bool pitchwise, int bands,
        bool quickSearch, double transposition, bool liveTransposition, double lowFrequency, double highFrequency,
        bool liveLow, bool liveHigh, bool fuller);
    // Each converted as prepare converts it (a band frequency prepare left
    // out stays out). False, with no change, before prepare, for a
    // transposition outside those ranges or band frequencies beyond half the
    // sample rate.
    [[nodiscard]] bool setControls(double transposition, double lowFrequency, double highFrequency) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    [[nodiscard]] double lowLimit(double frequency) const noexcept;
    [[nodiscard]] double highLimit(double frequency) const noexcept;
    void transposeFrame(std::span<float> frame) noexcept;
    void reposition(std::span<float> frame) noexcept;

    PhaseVocoder vocoder_;
    SpectralEnvelope envelope_;
    Units units_ = Units::Ratio;
    bool live_ = false;
    bool fuller_ = false;
    bool band_ = false;
    bool lowFlag_ = false;
    bool highFlag_ = false;
    bool liveLow_ = false;
    bool liveHigh_ = false;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    double halfchwidth_ = 0.0;
    double lowFrequency_ = 0.0;
    double highFrequency_ = 0.0;
    double startLowFrequency_ = 0.0;
    double startHighFrequency_ = 0.0;
    float transpos_ = 1.0F;
    float startTranspos_ = 1.0F;
    // CDP's windowbuf[0], whose frequencies carry over from frame to frame.
    std::vector<float> windowbuf_;
    std::size_t frames_ = 0;
};
}
