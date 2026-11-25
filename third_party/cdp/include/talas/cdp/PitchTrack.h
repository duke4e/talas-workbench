/*
 * Derived from the CDP System, Release 8: dev/repitch/repitch.c and
 * ap_repitch.c, dev/pitch/pitch.c and ap_pitch.c, dev/pv/pvoc.c,
 * dev/cdp2k/formantsg.c, parstruct.c, readfiles.c, tklib1.c and tklib3.c
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
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `repitch getpitch` (mode 1, the binary pitch file) over a whole sound,
// and the CDP pitch processes that read its pitch file window by window.
//
// getpitch cannot stream: after estimating each window's pitch it tidies
// the whole file (tidy_up_pitch_data), marking as silent the windows more
// than -s dB below the loudest window of the file, removing pitches without
// continuity with windows however far ahead, and interpolating unpitched
// runs from the next pitched window. So getPitch takes the whole sound, as
// a sample is loaded, and gives the pitch file's values; the pitch
// processes take that array at preparation and read it a window at a time,
// exactly as CDP when their input plays that sound from its start (CDP's
// pitch file is made from its infile).

// The pitch file's codes for a window with no pitch, and for a window CDP
// takes as silence (-s), as CDP writes them.
inline constexpr float notPitch = -1.0F;
inline constexpr float notSound = -2.0F;

// The number of analysis windows `pvoc anal` writes for a file of that many
// frames with those points and overlap, counted as pvoc_process's loop
// counts them: windows every D frames from -4 D (the first ends at frame
// 0), over the input followed by zeros, until a window's centre passes the
// last frame by half a window, or passes half a window when the file is
// shorter. For a window of M = 4, 2, 1 or a half times the points (made
// even) and D = M / 8, that is ceil((max(frames - 1, M / 2) + M / 2 + 4 D)
// / D), e.g. 264 windows for 8192 frames at 256 points, overlap 3; the
// least is 12 (only 2 points at overlap 4, M = 1, counts differently).
// CDP reads no more than 100000001 frames (pvoc's VERY_BIG_INT stops it).
// Nothing for no frames or what PhaseVocoder refuses.
[[nodiscard]] std::optional<std::size_t> analysisWindowCount(std::size_t frames, int points, int overlap) noexcept;

// getpitch's options, as CDP's defaults (tklib1.c) and ranges (tklib1.c,
// with ap_repitch.c's checks).
struct GetPitchSettings
{
    static constexpr double defaultTuningRange = 1.0;
    static constexpr double maximumTuningRange = 6.0;
    // CDP's BLIPLEN.
    static constexpr int defaultMinimumPitchedWindows = 2;
    // CDP's SILENCE_RATIO and SIGNOIS_MAX.
    static constexpr double defaultSignalToNoise = 80.0;
    static constexpr double maximumSignalToNoise = 1000.0;
    // CDP's ACCEPTABLE_MATCH and MAXIMI.
    static constexpr int defaultHarmonicsToConfirm = 5;
    static constexpr int maximumHarmonicsToConfirm = 8;
    // CDP's SPEC_MINFRQ, the lowest pitch's default and the least of both
    // pitch limits; the most is half the sample rate over MAXIMI (8).
    static constexpr double minimumPitch = 10.0;

    // -t: semitones within which harmonics count as in tune, 0 to 6.
    double tuningRange = defaultTuningRange;
    // -g: pitched windows needed in a row for a pitch to stand, 0 to the
    // sound's window count (0 keeps every pitch).
    int minimumPitchedWindows = defaultMinimumPitchedWindows;
    // -s: dB below the sound's loudest window under which a window is
    // silence, 0 to 1000.
    double signalToNoise = defaultSignalToNoise;
    // -n: how many of the 8 loudest peaks must be harmonics to confirm a
    // pitch, 1 to 8 (with -a, 1 never confirms one).
    int harmonicsToConfirm = defaultHarmonicsToConfirm;
    // -l and -h: the lowest and highest acceptable pitches in Hz, from 10
    // Hz to half the sample rate over 8, the lowest under the highest; the
    // highest defaults to half the sample rate over 8.
    double lowestPitch = minimumPitch;
    std::optional<double> highestPitch;
    // -a: the alternative pitch-finding method (found_pitch_2).
    bool alternativeMethod = false;
    // -z: keep unpitched windows as notPitch instead of interpolating
    // pitches across them.
    bool keepUnpitched = false;
};

// `repitch getpitch 1 in.ana out.ana pitch.frq [options]` after `pvoc anal 1
// in.wav in.ana -cPOINTS -oOVERLAP` of these samples: one pitch in Hz, or
// notPitch or notSound, for each of analysisWindowCount(samples.size(),
// points, overlap) windows, exactly as the binary pitch file stores them
// (mode 2's breakpoint file, with its -d, is not made). Nothing for no
// samples or a non-finite one, what PhaseVocoder refuses, settings outside
// their ranges, or where CDP gives up with "No valid pitch found": with
// -z, when no window holds a pitch; without it, when no window holds a
// pitch and the first is not silence, which takes a sound whose every
// window has no amplitude at all. (The first window, never analysed, has
// none, so it is silence unless the whole sound is, and without -z a
// sound with no pitch comes out notSound throughout: interpolation copies
// that first window's code forward.)
[[nodiscard]] std::optional<std::vector<float>> getPitch(std::span<const double> samples, double sampleRate,
    int points, int overlap, const GetPitchSettings& settings);

// `pvoc anal 1 in.wav in.ana -cPOINTS -oOVERLAP` of these samples, as
// getPitch analyses them: each of the analysisWindowCount(samples.size(),
// points, overlap) windows the .ana file holds goes to visit, in order, as
// CDP's programs that read a whole analysis file first (spec clean's noise
// file, strange invert's normalising pass) read it. False, having visited
// nothing, for no samples or a non-finite one or what PhaseVocoder refuses.
[[nodiscard]] bool analyseWholeSound(std::span<const double> samples, double sampleRate, int points,
    int overlap, const std::function<void(std::span<const float>)>& visit);

// The pitch processes below take a pitch array (getPitch's, or any made
// from the input sound) and read pitches[k] at analysis frame k. CDP
// refuses a pitch file whose length differs from the analysis file's; the
// stream takes frames past the array's end as unpitched, which both
// processes leave as they are. prepare refuses arrays holding infinities or
// pitches between 0 and CDP's MINPITCH (9 Hz), which getpitch never writes.

// `pitch altharms`: in each pitched frame, the channels away from the
// pitch's harmonics are silenced (specbare keeps, at each harmonic below
// the Nyquist frequency, the channels from 0.371 times the pitch below it
// to 0.589 times the pitch above it, 1.5 semitone ratios as differences;
// with -x only the loudest of them; those above the top harmonic's range
// stay), then every other channel still sounding, counting from the
// lowest: the odd-numbered (mode 1, usually an
// octave up with the formants kept) or the even-numbered (mode 2). CDP
// processes the first frame too; unpitched frames pass unchanged. CDP
// takes no breakpoint files here.
class PitchAltHarmonics
{
public:
    enum class Mode
    {
        DeleteOdd = 1,
        DeleteEven,
    };

    // lessBody is CDP's -x. False, with no change, for a mode outside 1 and
    // 2, a pitch array prepare refuses (see above) or what the phase
    // vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, std::span<const float> pitches,
        Mode mode, bool lessBody);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void altFrame(std::span<float> frame, double pitch) const noexcept;

    PhaseVocoder vocoder_;
    std::vector<float> pitches_;
    Mode mode_ = Mode::DeleteOdd;
    bool lessBody_ = false;
    double nyquist_ = 0.0;
    std::size_t frames_ = 0;
};

// `pitch octmove`: each pitched frame moves by a whole-number ratio (2 an
// octave, 3 a twelfth ...) without moving its formants. Mode 1 (up)
// silences the channels not within a semitone of a harmonic of the pitch
// times the ratio; modes 2 and 3 (down) write the new fundamental, the
// pitch over the ratio, and those of its harmonics below the Nyquist
// frequency that are not harmonics of the pitch, each into the channel of
// its frequency at the envelope of the frame's partials there (extract_
// specenv_over_partials: the loudest channel within a semitone of each
// harmonic of the pitch, interpolated over log frequency). Mode 3's bass
// boost, which CDP takes as a
// breakpoint file, changes nothing: specoct applies it only below the new
// fundamental, where it writes no harmonic. CDP's -i quick search, which
// octmove takes, changes nothing either (the envelope ignores it), so it
// is left out. The frame keeps its total amplitude. CDP leaves the first
// frame as it is, and unpitched frames.
class PitchOctaveMove
{
public:
    enum class Mode
    {
        Up = 1,
        Down,
        DownBassBoost,
    };

    // CDP's DEFAULT_OCT_TRANSPOS, OCT_MAX_HMOVE and OCT_MAX_BREI.
    static constexpr int defaultTransposition = 2;
    static constexpr int maximumTransposition = 4096;
    static constexpr double maximumBassBoost = 50.0;

    // The transposition ratio 2 to 4096; the bass boost (mode 3) 0 to 50.
    // False, with no change, outside those ranges, for a mode outside 1 to
    // 3, a pitch array prepare refuses (see above) or what the phase vocoder
    // refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, std::span<const float> pitches,
        Mode mode, int transposition, double bassBoost);
    // False, with no change, before prepare or outside 0 to 50.
    [[nodiscard]] bool setBassBoost(double bassBoost) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void octFrame(std::span<float> frame, double pitch) noexcept;
    void extractSpecenvOverPartials(std::span<const float> frame, double thispitch) noexcept;
    [[nodiscard]] double getspecenvamp(double thisfrq) const noexcept;

    PhaseVocoder vocoder_;
    std::vector<float> pitches_;
    Mode mode_ = Mode::Up;
    int transposition_ = defaultTransposition;
    double bassBoost_ = 0.0;
    double startBassBoost_ = 0.0;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    double halfchwidth_ = 0.0;
    // initialise_specenv's specenvfrq, specenvpch and specenvamp, whose
    // points beyond each frame's count keep earlier frames' values (CDP
    // zeroes specenvpch once, and the amplitudes at each frame).
    std::vector<float> specenvfrq_;
    std::vector<float> specenvpch_;
    std::vector<float> specenvamp_;
    std::size_t frames_ = 0;
};
}
