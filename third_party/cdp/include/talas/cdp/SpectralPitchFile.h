/*
 * Derived from the CDP System, Release 8: dev/pitch/pitch.c and ap_pitch.c,
 * dev/spec/ap_simple.c, dev/cdp2k/formantsg.c, tklib1.c, tklib3.c and
 * readdata.c
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
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::cdp
{
// CDP `pitch tune`, `pitch pick`, `pitch chord`, `pitch chordf` and `spec
// bare` on each frame between `pvoc anal` and `pvoc synth`. Of these only
// `spec bare` reads a pitch file (CDP's ANAL_WITH_PITCHDATA, a file `repitch
// getpitch` made from the same sound): CDP8 opens the other four with
// ANALFILE_ONLY, and they take their pitches or transpositions from data
// files instead, read once. Each frame is processed on its own (the chords
// keep a frame's worth of work space): nothing reads ahead.

// `pitch tune`: each channel's frequency moves toward the frequency of a
// template, the harmonics of the given pitches (CDP's pitch_template), in
// the channel holding it, by the focus (0 none, 1 all the way); channels
// holding none are scaled by 1 - clarity. With CDP's -t trace, only
// channels CDP marks as among the trace loudest are tuned: each such
// channel with no template frequency moves its amplitude to the nearest
// channel holding one (by frequency ratio), which it tunes, and the
// other channels are scaled by 1 - clarity. CDP marks
// channel 2c for each loudest channel c (it flags its ring's
// amplitude-buffer offsets but reads the flags by channel), so the
// channels tuned are the even channels whose halves are among the
// loudest, those under half the channel count. With CDP's -b bass cut,
// channels whose frequency is under it are silenced first. CDP processes
// every frame, the first included.
//
// CDP takes the focus, clarity, trace and bass cut as breakpoint files
// and reads them at each analysis frame's time: a value given before a
// process call reaches the frames analysed during it.
class PitchTune
{
public:
    // A data file's values: CDP's mode 1 (Hz) or 2 (MIDI).
    enum class Mode
    {
        Frequencies = 1,
        Midi,
    };

    // CDP's defaults: -f 1 and -c 1 (as tklib1.c sets it; the usage text
    // says 0), DEFAULT_TRACE and SPEC_MINFRQ.
    static constexpr double defaultFocus = 1.0;
    static constexpr double defaultClarity = 1.0;
    static constexpr double defaultTrace = 16.0;
    static constexpr double defaultBassCut = 10.0;
    // CDP's MINPITCH, the least bass cut.
    static constexpr double minimumBassCut = 9.0;

    // The template's pitches in Hz, as readPitchTemplate gives them, each
    // above 0 and under half the sample rate (and, unlike CDP, which steps
    // through every harmonic of each and hangs on the tiniest, not under
    // half the sample rate over 2^24). The focus and clarity from 0 to 1.
    // A trace of 0 is CDP's -t left out, as is a static trace of CDP's
    // default 16 (CDP turns -t on only for a value other than its
    // default); otherwise from 1 to the channel count (points / 2 + 1),
    // rounded half away from zero, live or not. Likewise a bass cut of 0 is CDP's -b left out, as is
    // a static 10 Hz; otherwise from 9 Hz to half the sample rate, a static
    // one under 10 Hz taken as 0 Hz and a live one as 10 Hz (as
    // setup_pitchzero makes them). Values within CDP's FLTERR of a limit
    // are taken as it. False, with no change, outside those ranges, for no
    // pitches or what the phase vocoder refuses. Whether -t and -b are on,
    // and the template, are read once.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, std::span<const double> pitches,
        double focus, double clarity, double trace, bool liveTrace, double bassCut, bool liveBassCut);
    // The trace and bass cut reach the frames only where prepare turned
    // them on, each converted as prepare converts it (the others are
    // ignored). False, with no change, before prepare or outside those
    // ranges.
    [[nodiscard]] bool setControls(double focus, double clarity, double trace, double bassCut) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    [[nodiscard]] std::optional<double> traceValue(double trace) const noexcept;
    [[nodiscard]] std::optional<double> bassCutValue(double bassCut) const noexcept;
    void tuneFrame(std::span<float> frame) noexcept;
    void replacePartialByTunedPartial(int cc, int vc, float* flbuf) const noexcept;
    void focusPartialTowardsTunedValue(int vc, int cc, float* flbuf) const noexcept;

    PhaseVocoder vocoder_;
    int clength_ = 0;
    double nyquist_ = 0.0;
    // CDP's vflag[TUNE_TRACE] and vflag[TUNE_FBOT].
    bool tracing_ = false;
    bool bassCutting_ = false;
    bool liveBassCut_ = false;
    // TUNE_FOC, TUNE_CLAR (as 1 - clarity), TUNE_INDX and TUNE_BFRQ.
    double focus_ = defaultFocus;
    double clarity_ = 0.0;
    int trace_ = 16;
    double bassCut_ = 0.0;
    double startFocus_ = defaultFocus;
    double startClarity_ = 0.0;
    int startTrace_ = 16;
    double startBassCut_ = 0.0;
    // CDP's frq_template, a frequency (or -1) for each channel.
    std::vector<float> template_;
    // CDP's ring of the loudest channels (offset, amplitude) and its
    // TUNE_LOUD flags, as many as the frame's floats.
    std::vector<std::pair<int, float>> ring_;
    std::vector<int> loud_;
};

// The pitches of a `pitch tune` data file, in its order, in Hz as CDP
// stores them (a float each), as get_input_frqs reads them: numbers are
// plain decimals ("-", digits and one "."), any number of them a line; a
// line is read up to the first word that is not one; CDP reads at most 199
// characters at a time, so a longer line splits there. In Mode::Midi each
// value is converted to Hz as miditohz converts it. CDP also takes one
// value on its command line instead of a file, read alike. Nothing for no
// values. The range, which depends on the sample rate, is prepare's.
[[nodiscard]] std::optional<std::vector<double>> readPitchTemplate(std::string_view text, PitchTune::Mode mode);

// `pitch pick`: only the channels that might hold the given partials keep
// their amplitudes; the others are scaled by (1 - clarity) cubed. The
// partials (each in the first channel whose top, half a channel width up
// from its centre, is above it) are those of a harmonic series on the
// fundamental (mode 1), its octaves (2), its odd harmonics (3), steps of
// the step in Hz from the fundamental (4), or the harmonics displaced by
// the step (5; the fundamental itself undisplaced), up to half the sample
// rate. CDP processes every frame, the first included.
//
// CDP takes the clarity as a breakpoint file and reads it at each analysis
// frame's time; the fundamental and step are read once.
class PitchPick
{
public:
    enum class Mode
    {
        Harmonics = 1,
        Octaves,
        OddHarmonics,
        Linear,
        DisplacedHarmonics,
    };

    // CDP's defaults.
    static constexpr double defaultClarity = 1.0;
    // CDP's SPEC_MINFRQ, the least fundamental and (mode 4) step.
    static constexpr double minimumFrequency = 10.0;

    // The fundamental from 10 Hz to half the sample rate; the step (modes
    // 4 and 5 only) from 10 Hz (mode 4) or minus half the sample rate (mode
    // 5) to half the sample rate; the clarity from 0 to 1. Values within
    // CDP's FLTERR of a limit are taken as it. False, with no change,
    // outside those ranges or for what the phase vocoder refuses.
    [[nodiscard]] bool prepare(
        double sampleRate, int points, int overlap, Mode mode, double fundamental, double step, double clarity);
    // False, with no change, before prepare or outside 0 to 1.
    [[nodiscard]] bool setClarity(double clarity) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void pickFrame(std::span<float> frame) const noexcept;

    PhaseVocoder vocoder_;
    // PICK_CLAR, as (1 - clarity) cubed.
    double clarity_ = 0.0;
    double startClarity_ = 0.0;
    // CDP's PICK_BFLG bitflags, PICK_LONGPOW2 and PICK_DIVMASK.
    std::vector<std::uint32_t> bitflags_;
    int longpow2_ = 0;
    int divmask_ = 0;
};

// The transpositions of a `pitch chord` or `pitch chordf` data file, in its
// order, as frequency ratios as CDP holds them (a float each), as chordget
// reads them: semitones, read as get_input_frqs reads its numbers (see
// readPitchTemplate). Nothing for no values or one outside CDP's range,
// over MIN_TRANSPOS (0.00383) to MAX_TRANSPOS (256).
[[nodiscard]] std::optional<std::vector<float>> readChordTranspositions(std::string_view text);

// `pitch chord` and `pitch chordf`: transposed copies of the spectrum laid
// over it, every channel's partial moved, once for each transposition, to
// the channel of its new frequency (up to half the sample rate). As CDP
// does, only the amplitudes move: each channel keeps the frequency it was
// analysed with. A moved partial takes its channel if empty, else an empty
// one up to three below or above (above only under the channel offset
// clength - 1, as CDP mixes the two), else replaces what is there if
// louder than the frame's own partial in that channel, else a quieter one
// nearby (move_data_into_some_appropriate_channel); with CDP's -x
// (lessBody: its usage calls it a fuller spectrum, but CDP8 sets
// CHD_LESSFUL) it replaces what is there only if its source channel was
// louder. The frame keeps its total amplitude. CDP leaves the first frame
// as it is.
//
// CDP's -b bottom and -t top: with both, channels outside them are
// silenced (where either is live a top under the bottom is swapped; with
// both static every channel is silenced then). As CDP8 sets each one's
// flag for the other's test, with -t alone the channels under the bottom
// (CDP's default, 5 Hz, for `chordf` 0 Hz) are silenced instead, and with
// -b alone those above the top (half the sample rate). A static bottom of
// 5 Hz or top of half the sample rate is CDP's flag left out. CDP takes the
// bottom and top as breakpoint files and reads them at each analysis
// frame's time: a value given before a process call reaches the frames
// analysed during it.
class PitchChord
{
public:
    // CDP's PITCHZERO, the least (and default) bottom and top.
    static constexpr double minimumFrequency = 5.0;

    // The transpositions as readChordTranspositions gives them; the bottom
    // and top from 5 Hz to half the sample rate (within CDP's FLTERR of a
    // limit taken as it). False, with no change, for no transpositions, one
    // outside CDP's range, a bottom or top outside that range or what the
    // phase vocoder refuses. The transpositions, -x and whether -b and -t
    // are on are read once.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, std::span<const float> transpositions,
        double bottom, bool liveBottom, double top, bool liveTop, bool lessBody);
    // False, with no change, before prepare or outside that range.
    [[nodiscard]] bool setBand(double bottom, double top) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // Half the sample rate, once prepared.
    [[nodiscard]] double nyquist() const noexcept { return nyquist_; }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void chordFrame(std::span<float> frame) noexcept;

    PhaseVocoder vocoder_;
    int clength_ = 0;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    double halfchwidth_ = 0.0;
    std::vector<float> transpos_;
    bool lessBody_ = false;
    // CDP's vflag[CHORD_FTOP] (set by -b) and vflag[CHORD_FBOT] (set by -t).
    bool bottomFlag_ = false;
    bool topFlag_ = false;
    bool liveBand_ = false;
    double lofrq_ = 0.0;
    double hifrq_ = 0.0;
    double startLofrq_ = 0.0;
    double startHifrq_ = 0.0;
    // CDP's windowbuf[0].
    std::vector<float> windowbuf_;
    std::size_t frames_ = 0;
};

// `pitch chordf`: as `pitch chord`, but each moved partial takes the frame's
// formant envelope (CDP's -fN or -pN with N bands, and -i for its quick
// search; see SpectralEnvelope) at its new frequency in proportion to its
// old one, so the spectral envelope stays where it was; partials where the
// envelope is silent are dropped. Static bottoms and tops under 10 Hz are
// taken as 0 Hz, live ones as 10 Hz (as setup_pitchzero makes them).
class PitchChordFormants
{
public:
    static constexpr double minimumFrequency = PitchChord::minimumFrequency;

    // As PitchChord's, and false for bands outside SpectralEnvelope's
    // bandRange for the frame. The bands and -i are read once.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, bool pitchwise, int bands,
        bool quickSearch, std::span<const float> transpositions, double bottom, bool liveBottom, double top,
        bool liveTop, bool lessBody);
    // False, with no change, before prepare or outside that range.
    [[nodiscard]] bool setBand(double bottom, double top) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // Half the sample rate, once prepared.
    [[nodiscard]] double nyquist() const noexcept { return nyquist_; }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    [[nodiscard]] double limit(double frequency, bool live) const noexcept;
    void chordFrame(std::span<float> frame) noexcept;

    PhaseVocoder vocoder_;
    SpectralEnvelope envelope_;
    int clength_ = 0;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    double halfchwidth_ = 0.0;
    std::vector<float> transpos_;
    bool lessBody_ = false;
    bool bottomFlag_ = false;
    bool topFlag_ = false;
    bool liveBottom_ = false;
    bool liveTop_ = false;
    double lofrq_ = 0.0;
    double hifrq_ = 0.0;
    double startLofrq_ = 0.0;
    double startHifrq_ = 0.0;
    std::vector<float> windowbuf_;
    std::size_t frames_ = 0;
};

// `spec bare`: each frame keeps only the channels near the harmonics of the
// pitch its pitch file gives, from (1 - 1/r) times the pitch below each
// harmonic to (r - 1) times the pitch above it, r being 1.5 times a
// semitone's ratio (CDP's SPAN_FACTOR times SEMITONE_INTERVAL), silencing
// those between, scanning the channels in order (by their frequencies as
// analysed, not rectified); with CDP's -x (lessBody) only the loudest
// channel near each harmonic. The channels from the first past the last
// harmonic under half the sample rate keep their amplitudes. A frame whose
// pitch is negative (CDP's NOT_PITCH or NOT_SOUND, unpitched or silent)
// passes as it is.
//
// CDP leaves the first frame as it is and, as it reads its pitches from
// its first processed frame on, gives frame k (k from 1) the pitch file's
// value k - 1: the stream does the same with pitches[k - 1]. CDP refuses a
// pitch file of another length than its analysis file; a stream has no end,
// and passes the frames past the pitches given as unpitched.
class SpecBare
{
public:
    // CDP's MINPITCH: no pitch file `repitch getpitch` writes holds a
    // positive pitch under it.
    static constexpr double minimumPitch = 9.0;

    // The pitches, one for each analysis frame (CDP's pitch file, as
    // `repitch getpitch` writes it from the same sound), copied; each finite
    // and negative or from 9 Hz up (a pitch of 0 Hz hangs CDP). False, with
    // no change, for any other pitch or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(
        double sampleRate, int points, int overlap, std::span<const float> pitches, bool lessBody);
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    void bareFrame(std::span<float> frame, double pitch) const noexcept;

    PhaseVocoder vocoder_;
    double nyquist_ = 0.0;
    bool lessBody_ = false;
    std::vector<float> pitches_;
    std::size_t frames_ = 0;
};
}
