/*
 * Derived from the CDP System, Release 8: dev/focus/focus.c and
 * ap_focus.c, dev/cdp2k/formantsg.c, readdata.c, tklib1.c and tklib3.c
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
#include <vector>

namespace talas::cdp
{
// CDP `focus focus` between `pvoc anal` and `pvoc synth`: each frame's
// formant envelope (CDP's -fN or -pN with N bands, and -i for its quick
// search; see SpectralEnvelope) is searched for peaks between the bottom
// and top frequencies (in either order), the loudest up to the peak count
// kept; each channel's amplitude is then scaled by a filter of
// raised-cosine bands the bandwidth in octaves wide, centred on the peaks'
// frequencies, at the channel's frequency. CDP takes the bandwidth and the
// frequencies as breakpoint files and reads them at each analysis frame's
// time: a value given before a process call reaches the frames analysed
// during it. Without stability CDP leaves the first frame as it is and
// filters each later one by its own peaks.
//
// CDP keeps the peak list from frame to frame: its search fills the list's
// places from the one it last found quietest, and the filter takes only the
// first places up to the peaks found, so a frame with fewer peaks than the
// count can take channels from earlier frames and miss its own; a frame
// with none builds its filter from the first band of the last frame that
// found one.
//
// With stability (CDP's -s) of S windows, each window from S - 1 on scores
// the peaks of the S windows up to it and keeps those found most often
// (ties by their amplitudes summed over the S windows); its filter is built
// on, and applied to, the window S - S / 2 (rounded down) windows before
// it, and is silent where none of the S windows has a peak. CDP writes as
// its first frame its stability store's first place, which nothing has
// filled (silent in CDP8's build), then each window one frame late:
// windows 0 to S / 2 - 1 with the first filter, designed at window S - 1,
// each later one with its own. CDP's frame 1 so needs window S - 1, S - 2
// windows later (later frames need fewer), which a stream bounds with that
// latency: analysis runs S - 2 frames ahead of synthesis, in two phase
// vocoders as BlurAndTrace runs them (see SpectralBlurBlur.h), the output
// being CDP's that many frames (times their spacing D) later on top of the
// phase vocoder's latency.
//
// At its file's end CDP filters the windows after the last it filtered in
// its loop, but the first of them, with its last filter, dropping that one
// (write_last_focu_buffers): with W windows in its file, a stream, which
// has no end, differs from CDP from frame W - S + S / 2 + 1 on (for S of 3
// or more; with 2, CDP just never writes its last window).
class FocusFocus
{
public:
    // CDP's MAXPKCNT; tklib1.c's defaults of the peak count and bandwidth
    // (DEFAULT_OCTAVE_BWIDTH); OCTAVES_PER_SEMITONE, the least bandwidth;
    // PITCHZERO, the least frequency; MAXSTABIL and DEFAULT_STABILITY.
    static constexpr int maximumPeaks = 16;
    static constexpr int defaultPeaks = 16;
    static constexpr double defaultBandwidth = 0.333333;
    static constexpr double minimumBandwidth = 0.08333333333;
    static constexpr double minimumFrequency = 5.0;
    static constexpr double minimumStability = 2.0;
    static constexpr double maximumStability = 4097.0;
    static constexpr double defaultStability = 9.0;

    // The windows CDP stabilises over: none without stability or for a
    // stability within 0.000002 of the default 9 (get_variants_and_flags
    // sets -s only for a value other than its default, so -s9 is CDP's
    // focus without -s); else the stability rounded. Nothing outside 2 to
    // 4097 (within 0.000002, which CDP takes as the limit).
    [[nodiscard]] static std::optional<int> stabilityWindows(bool stable, double stability) noexcept;

    // The latency prepare gives those points, overlap and stability: the
    // phase vocoder's, and with stabilityWindows S, S - 2 times its frame
    // spacing. Nothing for what prepare refuses of them.
    [[nodiscard]] static std::optional<std::size_t> latencyFor(
        int points, int overlap, bool stable, double stability) noexcept;

    // The most bandwidth for that sample rate and points (CDP's LOG2 of the
    // Nyquist frequency over half a channel width); zero for what the phase
    // vocoder refuses.
    [[nodiscard]] static double maximumBandwidth(double sampleRate, int points, int overlap) noexcept;

    // CDP's -fN or -pN (pitchwise) with N bands and -i (quickSearch); the
    // peak count from 1 to 16; the bandwidth from minimumBandwidth to
    // maximumBandwidth octaves; the bottom and top frequencies (CDP's -b and
    // -t, defaults 5 Hz and half the sample rate) from 5 Hz to half the
    // sample rate, in either order; with stable (CDP's -s), the stability
    // as stabilityWindows takes it. A live frequency is CDP's breakpoint
    // value, raised to 10 Hz; a fixed one under 10 Hz is zero, as
    // setup_pitchzero makes them. Values within 0.000002 of a limit are
    // that limit, as CDP reads them. False, with no change, outside those
    // ranges, for frequencies within 0.000002 of each other once so made
    // (CDP's zero-width band), bands outside SpectralEnvelope::bandRange for
    // the frame or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, bool pitchwise, int bands,
        bool quickSearch, int peaks, double bandwidth, double bottomFrequency, double topFrequency,
        bool liveBottom, bool liveTop, bool stable, double stability);
    // False, with no change, before prepare or outside the bandwidth's range.
    [[nodiscard]] bool setBandwidth(double bandwidth) noexcept;
    // False, with no change, before prepare, outside 5 Hz to half the sample
    // rate or for a zero-width band.
    [[nodiscard]] bool setFrequencies(double bottomFrequency, double topFrequency) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return analysis_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept;
    // After prepare: the bandwidth's limit and the windows stabilised over
    // (0 for none).
    [[nodiscard]] double maximumBandwidth() const noexcept { return maximumBandwidth_; }
    [[nodiscard]] int stabilityWindows() const noexcept { return stabl_; }
    // Half the sample rate, once prepared.
    [[nodiscard]] double nyquist() const noexcept { return nyquist_; }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    struct Design
    {
        int chan = 0;
        int score = 0;
        double amp = 0.0;
    };

    void analysed(std::span<float> frame) noexcept;
    void specfocus(std::span<float> frame) noexcept;
    void doStablePeaksFilter(std::span<const float> frame, double lofrq_limit, double hifrq_limit) noexcept;
    void extractFormantPeaks(int& pkcnt_here, double lofrq_limit, double hifrq_limit) noexcept;
    [[nodiscard]] int extractFormantPeaks2(int* fppk, double lofrq_limit, double hifrq_limit) const noexcept;
    void insertNewPeakInPeaklist(int position_in_formant) noexcept;
    void sortFocuPeaksByFrq(int pkcnt_here) noexcept;
    void establishBottomAndTopOfFilterBands(int pkcnt_here) noexcept;
    void constructFilterEnvelope(int pkcnt_here, const float* fbuf) noexcept;
    void doFocuFilter(float* thisbuf) const noexcept;
    void designFilter(const float* middle) noexcept;
    void sortEquivalentScores(int this_pkcnt) noexcept;
    void generateFocuFilterParams() noexcept;
    [[nodiscard]] float* sbuf(std::size_t window) noexcept;
    [[nodiscard]] std::size_t slot(std::size_t window) const noexcept;

    PhaseVocoder analysis_;
    PhaseVocoder synthesis_;
    SpectralEnvelope envelope_;
    int clength_ = 0;
    int specenvcnt_ = 0;
    double nyquist_ = 0.0;
    double maximumBandwidth_ = 0.0;
    // FOCU_PKCNT (CDP's itemcnt), FOCU_STABL (0 for none), FOCU_SL1 and the
    // stability's offset.
    int itemcnt_ = 1;
    int stabl_ = 0;
    int sl1_ = 0;
    int offset_ = 0;
    // FOCU_BW, FOCU_LOFRQ and FOCU_HIFRQ as read and as prepared, and which
    // frequencies are live.
    double bandwidth_ = defaultBandwidth;
    double low_ = 0.0;
    double high_ = 0.0;
    double startBandwidth_ = defaultBandwidth;
    double startLow_ = 0.0;
    double startHigh_ = 0.0;
    bool liveLow_ = false;
    bool liveHigh_ = false;
    // scalefact, FOCU_BRATIO_UP and FOCU_BRATIO_DN.
    double scalefact_ = 0.0;
    double bratioUp_ = 0.0;
    double bratioDn_ = 0.0;
    // filtpeak, fbandtop, fbandbot and peakno (itemcnt + 1 each), and
    // fsampbuf (clength + 1); outer_focu_loop's least, peakscore and
    // total_windows.
    std::vector<double> filtpeak_;
    std::vector<double> fbandtop_;
    std::vector<double> fbandbot_;
    std::vector<int> peakno_;
    std::vector<float> fsampbuf_;
    int least_ = 0;
    int peakscore_ = 0;
    std::size_t totalWindows_ = 0;
    // The stability store (CDP's stable->spec, fpk, total_pkcnt and sbuf),
    // as rings of stability places by window, each window's at window
    // modulo the stability; design_score and des.
    std::vector<float> spec_;
    std::vector<int> fpk_;
    std::vector<int> totalPkcnt_;
    std::vector<float> sbuf_;
    std::vector<int> designScore_;
    std::vector<Design> des_;
    // With stability: the frames before synthesis starts ((stability - 2) *
    // D), those played of them, and the frames synthesised; the analysis
    // vocoder's discarded output and the synthesis vocoder's (discarded)
    // input, a frame spacing each.
    std::size_t delay_ = 0;
    std::size_t waited_ = 0;
    std::size_t synthesised_ = 0;
    std::vector<double> discard_;
    std::vector<double> silence_;
};
}
