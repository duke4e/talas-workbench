/*
 * Derived from the CDP System, Release 8: dev/strange/strange.c and
 * ap_strange.c, dev/pitch/pitch.c and ap_pitch.c, dev/stretch/stretch.c and
 * ap_stretch.c, dev/repitch/repitch.c and ap_repitch.c, dev/cdp2k/tklib1.c,
 * tklib3.c and readdata.c
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
#include <vector>

namespace talas::cdp
{
// CDP `strange shift`, `strange waver`, `pitch transp`, `stretch spectrum`
// and `repitch transpose` on each frame between `pvoc anal` and `pvoc
// synth`. CDP takes their shift, frequencies and
// transposition as breakpoint files and reads them at each analysis
// frame's time: a value given before a process call reaches the frames
// analysed during it.

// `strange shift`: every partial's frequency, or those above or below a
// frequency, or inside or outside a band, moves by the shift in Hz, and
// each moved partial takes the channel of its new frequency if louder than
// what has moved there.
class StrangeShift
{
public:
    enum class Mode
    {
        All = 1,
        Above,
        Below,
        Between,
        Outside,
    };

    // False, with no change, for a shift beyond half the sample rate, a
    // frequency outside a quarter of a channel from 0 Hz to half the sample
    // rate or what the phase vocoder refuses. Mode 1 reads no frequency,
    // modes 2 and 3 only the first.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, double shift,
        double frequency, double highFrequency);
    // False, with no change, before prepare or outside those ranges.
    [[nodiscard]] bool setControls(double shift, double frequency, double highFrequency) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    [[nodiscard]] bool valid(double shift, double frequency, double highFrequency) const noexcept;
    void shiftFrame(std::span<float> frame) noexcept;
    void chanShift(std::span<float> frame, int cc, int vc) noexcept;

    PhaseVocoder vocoder_;
    Mode mode_ = Mode::All;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    double halfchwidth_ = 0.0;
    double shift_ = 0.0;
    double frequency_ = 0.0;
    double highFrequency_ = 0.0;
    double startShift_ = 0.0;
    double startFrequency_ = 0.0;
    double startHighFrequency_ = 0.0;
    // SHIFT_CHTOP and SHIFT_CHMID, accumulated as CDP adds them, and
    // SHIFT_OVER and SHIFT_DONE.
    std::vector<double> chtop_;
    std::vector<double> chmid_;
    std::vector<int> over_;
    std::vector<int> done_;
};

// `strange waver`: the spectrum above the bottom frequency stretches and
// relaxes in cycles of the vibrato frequency, each channel's frequency
// scaled the more the higher it lies (linearly, or by the exponent in mode
// 2), from the stretch up to twice it and back, and each partial takes the
// channel of its new frequency. CDP reads the vibrato frequency and the
// stretch, which it takes as breakpoint files, at the start of each cycle.
class StrangeWaver
{
public:
    enum class Mode
    {
        Standard = 1,
        Specified,
    };

    // CDP's WAVER_MIN_EXP and WAVER_MAX_EXP.
    static constexpr double minimumExponent = 0.02;
    static constexpr double maximumExponent = 50.0;
    // An hour a cycle, where CDP's slowest vibrato is its whole file.
    static constexpr double minimumVibrato = 1.0 / 3600.0;

    // False, with no change, for a vibrato frequency outside an hour a cycle
    // to half the frame rate, a stretch outside 1 to half the sample rate
    // over 10 Hz, a bottom frequency outside 5 Hz to half the sample rate,
    // an exponent outside 0.02 to 50 or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, double vibrato,
        double stretch, double bottomFrequency, double exponent);
    // False, with no change, before prepare or outside those ranges.
    [[nodiscard]] bool setControls(double vibrato, double stretch) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    [[nodiscard]] bool valid(double vibrato, double stretch) const noexcept;
    void stretchWindow(std::span<float> frame, double thisstr) noexcept;

    PhaseVocoder vocoder_;
    Mode mode_ = Mode::Standard;
    float frametime_ = 0.0F;
    double nyquist_ = 0.0;
    double exponent_ = 1.0;
    double vibrato_ = 8.0;
    double stretch_ = 1.0;
    double startVibrato_ = 8.0;
    double startStretch_ = 1.0;
    int botchan_ = 0;
    int strchans_ = 0;
    // WAVER_CHFBOT, and CDP's amp and freq arrays.
    std::vector<double> chfbot_;
    std::vector<float> amp_;
    std::vector<float> freq_;
    // do_waver's place in the cycle: the frames up and down, and those done.
    int upwcnt_ = 0;
    int dnwcnt_ = 0;
    bool oddCycle_ = false;
    int done_ = 0;
    double strstep_ = 0.0;
    double thisstr_ = 1.0;
};

// `pitch transp`: the spectrum above or below a split frequency, or both,
// moves up or down an octave (modes 1 to 3) or by a transposition in
// semitones (modes 4 to 6), channel by channel; CDP's -d depth draws the
// shift toward none. CDP reads the split, transpositions and depth, which
// it takes as breakpoint files, at each analysis frame.
class PitchTransp
{
public:
    enum class Mode
    {
        OctaveUp = 1,
        OctaveDown,
        OctaveUpAndDown,
        ShiftUp,
        ShiftDown,
        ShiftUpAndDown,
    };

    // A live split is CDP's breakpoint value, raised to 10 Hz, and a static
    // one under 10 Hz zero, as setup_pitchzero makes them. Live
    // transpositions are converted at each frame; static ones once, which
    // CDP8 converts again at each frame when any of them follows a file, so
    // in modes 4 to 6 a live split or transposition needs every
    // transposition live. Without the depth flag the depth is ignored.
    // False, with no change, for a split outside 5 Hz to half the sample
    // rate, transpositions beyond half the sample rate over 10 Hz, a depth
    // outside 0 to 1, shifts too great for the split (CDP's
    // sub_check_shiftp) or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, double split,
        double transposition, double transposition2, bool hasDepth, double depth, bool liveSplit,
        bool liveTransposition, bool liveTransposition2);
    // False, with no change, before prepare or outside those ranges.
    [[nodiscard]] bool setControls(double split, double transposition, double transposition2, double depth) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    struct Shifts
    {
        int fdcno = 0;
        double shf1 = 0.0;
        double shf2 = 0.0;
        double s1l1 = 0.0;
        double ls1 = 0.0;
        double ns1 = 0.0;
        double ns2 = 0.0;
    };
    [[nodiscard]] std::optional<Shifts> shifts(double split, double transposition, double transposition2) const noexcept;
    [[nodiscard]] bool shiftMode() const noexcept { return mode_ >= Mode::ShiftUp; }
    void shiftFrame(std::span<float> frame, Shifts current, double depth) noexcept;

    PhaseVocoder vocoder_;
    Mode mode_ = Mode::OctaveUp;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    bool hasDepth_ = false;
    bool liveSplit_ = false;
    bool liveTranspositions_ = false;
    Shifts shifts_;
    Shifts startShifts_;
    double depth_ = 1.0;
    double startDepth_ = 1.0;
    std::vector<float> amp_;
    std::vector<float> freq_;
};

// `stretch spectrum`: the channels above (mode 1) or below (mode 2) the
// divide frequency spread out or draw in, the farthest by the maximum
// stretch and those between by the exponent's curve; CDP's -d depth draws
// the stretch toward none. CDP reads the divide, stretch and exponent once
// and takes the depth as a breakpoint file, read at each analysis frame.
class StretchSpectrum
{
public:
    enum class Mode
    {
        Above = 1,
        Below,
    };

    // CDP's STR_MIN_EXP and STR_MAX_EXP.
    static constexpr double minimumExponent = 0.02;
    static constexpr double maximumExponent = 50.0;

    // Without the depth flag the depth is ignored; a live depth of zero
    // leaves the frame as it is, as CDP leaves a window whose breakpoint
    // depth is zero. False, with no change, for a divide outside 5 Hz to
    // half the sample rate, a stretch outside 10 Hz over half the sample
    // rate to its inverse, an exponent outside 0.02 to 50, a depth outside 0
    // to 1 or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, double divide,
        double stretch, double exponent, bool hasDepth, double depth, bool liveDepth);
    // False, with no change, before prepare or outside 0 to 1.
    [[nodiscard]] bool setDepth(double depth) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void stretchFrame(std::span<float> frame, double shift) noexcept;

    PhaseVocoder vocoder_;
    Mode mode_ = Mode::Above;
    int fdcno_ = 0;
    double exponent_ = 1.0;
    double sl1_ = 0.0;
    double nshift_ = 1.0;
    bool liveDepth_ = false;
    double depth_ = 1.0;
    double startDepth_ = 1.0;
    std::vector<float> amp_;
    std::vector<float> freq_;
};

// `repitch transpose`: every partial's frequency is multiplied by the
// transposition (a ratio, octaves or semitones), the partials move to the
// channels of their new frequencies, optionally (CDP's -x) to nearby empty
// ones for a fuller spectrum, and the frame keeps its total amplitude.
// CDP's -l and -h silence channels outside a band. CDP leaves the first
// frame as it is.
class RepitchTranspose
{
public:
    enum class Units
    {
        Ratio = 1,
        Octaves,
        Semitones,
    };

    // CDP's MIN_TRANSPOS and MAX_TRANSPOS, eight octaves either way.
    static constexpr double minimumRatio = 0.00383;
    static constexpr double maximumRatio = 256.0;

    // A live transposition is converted as CDP converts a breakpoint file
    // (semitones over 12), a static one as CDP converts a constant (times
    // its OCTAVES_PER_SEMITONE). Static low and high frequencies of zero are
    // CDP's -l and -h left out; a live one is CDP's breakpoint value, raised
    // to 10 Hz, and a static one under 10 Hz zero, as setup_pitchzero makes
    // them. False, with no change, for a transposition outside eight
    // octaves, band frequencies beyond half the sample rate or what the
    // phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Units units, double transposition,
        bool liveTransposition, double lowFrequency, double highFrequency, bool liveLow, bool liveHigh,
        bool fuller);
    // False, with no change, before prepare, for a transposition outside
    // eight octaves or band frequencies beyond half the sample rate.
    [[nodiscard]] bool setControls(double transposition, double lowFrequency, double highFrequency) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    [[nodiscard]] std::optional<float> ratio(double transposition) const noexcept;
    [[nodiscard]] double lowLimit(double frequency) const noexcept;
    [[nodiscard]] double highLimit(double frequency) const noexcept;
    void transpose(std::span<float> frame, float transpos) noexcept;

    PhaseVocoder vocoder_;
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
    std::vector<float> windowbuf_;
    std::size_t frames_ = 0;
};
}
