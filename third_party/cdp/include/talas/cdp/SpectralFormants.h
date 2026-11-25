/*
 * Derived from the CDP System, Release 8: dev/formants/formants.c and
 * ap_formants.c, dev/hilite/hilite.c and ap_hilite.c, dev/include/vowels.h
 * and vowels2.h, and dev/cdp2k/formantsg.c, readfiles.c, tklib1.c and
 * tklib3.c
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

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// CDP `formants get infile2 fmntfile -fN|-pN [-i]` then `formants put mode
// infile fmntfile outfile [-i] [-llof] [-hhif] [-ggain]`, as one process on
// two streams analysed in step between `pvoc anal` and `pvoc synth`: the
// side (infile2) gives its formant envelope frame by frame (-fN or -pN with
// N bands, and get's -i for its quick search; see SpectralEnvelope), and the
// input (infile) takes it. In mode 1 (Replace) each channel's amplitude is
// scaled by the side's envelope over the input's own, taken with the same
// bands and put's -i; in mode 2 (Impose) by the side's envelope alone; both
// read at the channel's frequency. Channels below the low or above the high
// frequency are zeroed, the frame keeps its total amplitude, and the gain
// scales it. CDP leaves its first frame as it is, and reads every value
// once: nothing is live. The output comes latencyFrames late. (With put's
// -i CDP never sets the input envelope's Nyquist point, which its fresh
// memory holds at 0, as SpectralEnvelope does.)
//
// CDP8's put starts with a buffer of the formant file but none of the
// input, which it believes it has read: before the input's first frame it
// processes as many frames of its fresh (zeroed) input buffer as that
// formant buffer holds floats of an analysis frame, shiftWindows() of
// them. The input so plays shiftWindows() analysis frames late against the
// side (each input frame takes the side's envelope from that many frames
// later), after as many frames of silence, and the stream keeps this. The
// formant buffer holds 20 rows of formants (BUF_MULTIPLIER), the file's two
// header rows among them, so a side of fewer than 18 analysis frames can
// give CDP a smaller shift: a stream takes the side as long. CDP stops where
// the shorter file ends; a stream's two inputs are always alike in length.
class FormantsPut
{
public:
    enum class Mode
    {
        Replace = 1,
        Impose,
    };

    // CDP's PITCHZERO, the default low frequency (the high defaults to half
    // the sample rate), and FORM_MAX_GAIN.
    static constexpr double defaultLowFrequency = 5.0;
    static constexpr double defaultGain = 1.0;
    static constexpr double maximumGain = 40.0;

    // get's -pN (pitchwise) or -fN with N bands and its -i (getQuickSearch);
    // put's mode and -i (quickSearch, which only mode 1 uses), its low (-l)
    // and high (-h) frequencies from 5 Hz to half the sample rate, either
    // under 10 Hz taken as 0 (setup_pitchzero), the high then above the low,
    // and the gain (-g) from CDP's FLTERR (0.000002) to 40, one within
    // FLTERR of 1 leaving the frame as it is. False, with no change, outside
    // those ranges, for bands outside SpectralEnvelope::bandRange for the
    // frame or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, bool pitchwise, int bands,
        bool getQuickSearch, Mode mode, bool quickSearch, double lowFrequency, double highFrequency, double gain);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // The analysis frames the input plays late against the side (above),
    // and that delay in frames (shiftWindows() times the frames' spacing);
    // 0 before prepare.
    [[nodiscard]] int shiftWindows() const noexcept { return shift_; }
    [[nodiscard]] std::size_t shiftFrames() const noexcept
    {
        return static_cast<std::size_t>(shift_) * static_cast<std::size_t>(vocoder_.decimation());
    }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input or side.
    [[nodiscard]] bool process(
        std::span<const double> input, std::span<const double> side, std::span<double> output) noexcept;

private:
    void putFrame(std::span<float> frame, std::span<const float> sideFrame) noexcept;

    PhaseVocoder vocoder_;
    PhaseVocoder sideVocoder_;
    // get's envelope of the side (the formant file's frame, specenvamp2)
    // and put's of the input (specenvamp), with the same bands.
    SpectralEnvelope getEnvelope_;
    SpectralEnvelope putEnvelope_;
    Mode mode_ = Mode::Replace;
    double lowFrequency_ = 0.0;
    double highFrequency_ = 0.0;
    double gain_ = defaultGain;
    int shift_ = 0;
    // The side frame, which get rectifies; the input frames waiting out the
    // shift, oldest at ringAt_; the frame put works on.
    std::vector<float> side_;
    std::vector<float> ring_;
    std::size_t ringAt_ = 0;
    std::vector<float> work_;
    std::size_t frames_ = 0;
};

// CDP `hilite vowels infile outfile vowels halfwidth steepness range
// threshold` on each frame between `pvoc anal` and `pvoc synth`: an
// envelope of a vowel's three formants (CDP's table: a peak at each
// formant, falling linearly to nothing the halfwidth times its frequency
// either side, raised to the steepness, the second and third attenuated),
// the peaks standing the range above a floor of 1 - range and shaped by
// CDP's curve for the ear's sensitivity, is scaled to the frame's loudest
// channel; each channel louder than the threshold times that envelope at
// its frequency is set to it, and the frame keeps its total amplitude. The vowels change over time as the data gives them, moving
// linearly between one and the next, at each frame's time from the start
// of the stream (frame n at n times CDP's float frame time, in float). CDP
// reads every value once: nothing is live, and every frame is processed,
// the first included.
//
// After the last time CDP holds the last vowel to a time it adds just past
// the end of its file; a stream holds it for ever. A channel frequency
// above 96 kHz (the top of CDP's sensitivity curve, reachable only at
// sample rates above 96 kHz) stops CDP with an error; the stream holds the
// curve's last value there.
class HiliteVowels
{
public:
    // CDP's vowel numbers (vowels.h), named as its data takes them.
    enum class Vowel
    {
        Ee = 1, // heat
        I,      // hit
        Ai,     // hate
        E,      // pet
        A,      // pat
        Ar,     // heart
        O,      // hot
        Or,     // taught
        U,      // hood
        Xx,     // Southern English hub
        X,      // herb, the
        Oo,     // mood
        Oa,     // North of England boat
        Aii,    // Scottish educAted
        Uu,     // Scottish edUcated
        N,      // n
        Ui,     // Scottish could
        M,      // m
        R,      // r on a neutral vowel
        Th,     // th
    };

    // A time in seconds from the stream's start and the vowel there.
    struct Point
    {
        double time = 0.0;
        Vowel vowel = Vowel::A;
    };

    // vowels2.h's suggested values (V_HWIDTH, CURVIT, PEAK_RANGE) and the
    // threshold's default.
    static constexpr double defaultHalfWidth = 0.25;
    static constexpr double defaultSteepness = 3.0;
    static constexpr double defaultRange = 0.95;
    static constexpr double defaultThreshold = 0.5;

    // The points in time order, as readHiliteVowels gives them: at least
    // one, the first at or after 0 (CDP holds a later first vowel from 0),
    // each later time above the one before (times CDP's FLTERR apart or
    // less make CDP hold its last values between them), CDP's vowel names
    // (a vowel name in place of CDP's file is one point at 0); the halfwidth
    // from 0.01 to 10, the steepness from 0.1 to 10, the range and the
    // threshold from 0 to 1. False, with no change, outside those, for a
    // value that is not finite or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, std::span<const Point> vowels,
        double halfWidth, double steepness, double range, double threshold);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    struct Formants
    {
        double formant1 = 0.0;
        double formant2 = 0.0;
        double formant3 = 0.0;
        double f2atten = 0.0;
        double f3atten = 0.0;
    };

    void advance() noexcept;
    void filter(std::span<float> frame) noexcept;

    PhaseVocoder vocoder_;
    float frametime_ = 0.0F;
    double halfWidth_ = defaultHalfWidth;
    double steepness_ = defaultSteepness;
    double range_ = defaultRange;
    double threshold_ = defaultThreshold;
    // get_the_vowels' tables, the last time held for ever, and itemcnt.
    std::vector<double> times_;
    std::vector<Vowel> vowels_;
    int itemcnt_ = 0;
    // vowel_filter's state: the next table entry, the vowels either side
    // and their times, the steps between them and the formants in use.
    int t_ = 0;
    Formants start_;
    Formants end_;
    Formants step_;
    Formants now_;
    double starttime_ = 0.0;
    double endtime_ = 0.0;
    double timestep_ = 0.0;
    std::vector<double> vamp_;
    std::size_t frames_ = 0;
};

// get_vowel: CDP's name for a vowel (ee, i, ai, aii, e, a, ar, o, oo, oa,
// or, u, ui, uu, xx, x, n, m, r, th); nothing for any other word.
[[nodiscard]] std::optional<HiliteVowels::Vowel> hiliteVowel(std::string_view name) noexcept;

// The points of a `hilite vowels` data file, in its order, as
// get_the_vowels reads it: words separated by white space, alternately a
// time (read as sscanf's "%lf" reads it, its leading number taken and the
// rest of the word ignored) and a vowel name (hiliteVowel), a pair free to
// span lines; a line whose first character is ';' is a comment; CDP reads
// at most 199 characters at a time, so a longer line splits there, and a
// piece of it beginning ';' is skipped. Nothing for no pairs, a time
// without a vowel, a word that is not a time where one is due or not a
// vowel, a first time below 0 or a later one not above the one before, or
// a time that is not finite (which CDP takes, to no good end). The points
// are as the file gives them: prepare holds a later first vowel from 0.
[[nodiscard]] std::optional<std::vector<HiliteVowels::Point>> readHiliteVowels(std::string_view text);
}
