/*
 * Derived from the CDP System, Release 8: dev/pv/pvoc.c, dev/stretch/
 * stretch.c and ap_stretch.c, dev/spec/simple.c and ap_simple.c, dev/focus/
 * focus.c and ap_focus.c, dev/combine/combine.c and ap_combine.c, dev/blur/
 * blur.c and ap_blur.c, and dev/cdp2k/mainfuncs.c, parstruct.c, readdata.c,
 * special.c, tklib1.c, tklib3.c and writedata.c
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
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// CDP's spectral processes that change a sound's length or the order of its
// windows, which cannot stream: an offline "process to sample". Each input
// goes through `pvoc anal 1 in.wav in.ana -cPOINTS -oOVERLAP`, the CDP
// program runs over the whole .ana file (or files), and `pvoc synth` turns
// its output back into sound: renderOfflineSpectralProcess gives exactly
// the samples CDP8's pvoc synth writes to its WAV.
//
// The programs are ported with their buffers (20 windows each, CDP's
// BUF_MULTIPLIER), so that what CDP's buffer arithmetic does to the windows
// near a buffer's end, bugs included, happens here too; each quirk is
// described in the port (src/SpectralWholeFile*.cpp). Where CDP's
// behaviour is undefined (it would read memory it never wrote, move a
// negative count or seek past its file), the port refuses.

struct OfflineSpectralField
{
    // camelCase, e.g. "stretch".
    std::string_view id;
    // A short UI label.
    std::string_view label;
    // Number: a number as CDP reads it (no exponent). Breakpoints: a number,
    // or a CDP breakpoint table, "time value" pairs on lines. Choice: one of
    // choices. Flag: "true" or "false". Text: a CDP data file's text, or
    // (blur shuffle's domain-image) a command-line string.
    enum class Kind
    {
        Number,
        Breakpoints,
        Choice,
        Flag,
        Text,
    } kind = Kind::Number;
    // As text. An empty default stands for CDP's own default, which depends
    // on the input (as the help says); an empty text given for such a field
    // takes that default too.
    std::string_view defaultValue;
    std::vector<std::string_view> choices;
    // CDP's meaning and range, one sentence.
    std::string_view help;
};

struct OfflineSpectralProcess
{
    // e.g. "stretchTime".
    std::string_view id;
    // e.g. "stretch time".
    std::string_view cdpCommand;
    // e.g. "Stretch Time".
    std::string_view label;
    std::size_t minimumInputs = 1;
    std::size_t maximumInputs = 1;
    std::vector<OfflineSpectralField> fields;
};

[[nodiscard]] const std::vector<OfflineSpectralProcess>& offlineSpectralProcesses();
[[nodiscard]] const OfflineSpectralProcess* findOfflineSpectralProcess(std::string_view id);

struct OfflineSpectralRequest
{
    // An OfflineSpectralProcess id.
    std::string process;
    // pvoc anal -c (made even as CDP makes it) and -o, as PhaseVocoder takes
    // them.
    int points = 1024;
    int overlap = 3;
    // id -> text; missing ones take their default. Ids no field has are
    // refused.
    std::map<std::string, std::string, std::less<>> fields;
};

struct OfflineSpectralResult
{
    // Exactly the samples CDP8's pvoc synth writes.
    std::vector<double> samples;
    // A sentence naming what CDP refuses; empty on success.
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// The CDP program's output .ana file: its windows, frameSize floats each
// (points made even, plus 2).
struct OfflineSpectralAnalysis
{
    std::vector<float> windows;
    std::size_t frameSize = 0;
    // As OfflineSpectralResult's.
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// `pvoc anal` of each input (the samples of each, at this sample rate, as
// pvoc anal reads a mono float WAV) and the CDP program over the .ana files:
// the .ana file it writes. Refused, with the reason in error: an unknown
// process or field, a field's text that CDP would not read or that is
// outside CDP's range, an input count outside the process's, an empty or
// non-finite input, points or overlap that `pvoc anal` refuses, what the
// CDP program refuses (its consistency checks and its run-time errors),
// what would leave CDP's behaviour undefined, an output of no windows (pvoc
// synth refuses a file of zero duration), and an output of more than 2^27
// analysis floats.
[[nodiscard]] OfflineSpectralAnalysis renderOfflineSpectralAnalysis(const OfflineSpectralRequest& request,
    std::span<const std::vector<double>> inputs, double sampleRate);

// renderOfflineSpectralAnalysis, then `pvoc synth` of its file
// (synthesiseAnalysisFile): exactly the samples CDP8 writes, refused as
// renderOfflineSpectralAnalysis refuses. A file of 9 windows or fewer (at
// the usual D of an eighth of the window; spec grab's one window always)
// synthesises to no samples, as pvoc synth writes none for it.
[[nodiscard]] OfflineSpectralResult renderOfflineSpectralProcess(const OfflineSpectralRequest& request,
    std::span<const std::vector<double>> inputs, double sampleRate);

// `pvoc synth` of an .ana file holding these windows (frameSize floats
// each, as PhaseVocoder's frames), made by `pvoc anal` with these points
// and overlap at this sample rate: exactly the samples it writes. Its
// sample rate is the header's, D times arate (arate being the sample rate
// over D in float), as pvoc synth computes it. pvoc synth shifts out D
// samples a window once the overlap-add has begun (from the tenth window
// at D = M / 8); after the file's last window it goes on reading zero
// windows, writes no all-zero block (outfloats once input has ended) and
// stops at the read after one (the NEW 2014 local_maxsample test): so it
// writes the blocks of the file's windows, then the overlap-add tail up to
// its first silent block, and nothing for a file too short to reach its
// first block. Empty for that, for no windows, a float count that is not
// a whole number of windows, a non-finite sample rate or what
// PhaseVocoder refuses.
[[nodiscard]] std::vector<double> synthesiseAnalysisFile(
    std::span<const float> windows, int points, int overlap, double sampleRate);
}
