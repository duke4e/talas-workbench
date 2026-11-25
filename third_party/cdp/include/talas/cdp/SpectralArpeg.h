/*
 * Derived from the CDP System, Release 8: dev/hilite/hilite.c and
 * ap_hilite.c, and dev/cdp2k/tklib1.c and tklib3.c
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
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `hilite arpeg` on each frame between `pvoc anal` and `pvoc synth`: a
// band, the bandwidth wide, sweeps between the low and high frequencies (in
// either order) at the rate in sweeps a second, along a downward ramp, a
// sine, a saw (up and back) or an upward ramp from the start phase. Modes 1
// to 4 play the channels the band reaches amplified, each sustained over the
// sustain in frames: decaying (linearly, or by the decay as an exponent) at
// its start frequency, or the source's with -T (track), and attacked again
// once it falls below the source, or only once it has died with -K (keep).
//
// CDP takes the rate, the frequencies, the bandwidth, the amplification and
// the sustain as breakpoint files and reads them, the sustain rounded, at
// each analysis frame's time: a value given before a process call reaches
// the frames analysed during it. CDP sweeps each frame by the mean of its
// rate and the last frame's.
class HiliteArpeg
{
public:
    enum class Mode
    {
        // Only the band's channels.
        On = 1,
        // The band's channels amplified among the rest.
        Boost,
        // Until the sweep first wraps, only the channels from the band's top
        // up, amplified; then Boost.
        BelowBoost,
        // Until a sine or saw sweep passes its middle, or another first
        // wraps, only the channels up to the band's bottom, amplified; then
        // Boost.
        AboveBoost,
        // Only the channels up to the band's middle, amplified.
        Below,
        // Only the channels from the band's middle up, amplified.
        Above,
        // Until a sine or saw sweep passes its middle, or another first
        // wraps, only the channels up to the band's bottom, amplified; then
        // the whole spectrum as it is.
        OnceBelow,
        // Until the sweep first wraps, only the channels from the band's top
        // up, amplified; then the whole spectrum as it is.
        OnceAbove,
    };

    enum class Wave
    {
        DownRamp = 1,
        Sine,
        Saw,
        UpRamp,
    };

    // CDP's PITCHZERO, ARPE_MAX_AMPL, ARPE_MIN_NONLIN and ARPE_MAX_NONLIN.
    static constexpr double minimumFrequency = 5.0;
    static constexpr double maximumAmplification = 10000.0;
    static constexpr double minimumDecay = 0.02;
    static constexpr double maximumDecay = 50.0;
    // An hour of frames, where CDP's longest sustain is its whole file.
    static constexpr double maximumSustainSeconds = 3600.0;

    // CDP's defaults: a sine at 1 sweep a second from phase 0 (0.51 in modes
    // 4 and 8), 5 Hz (zero) to half the sample rate, the narrowest band,
    // amplification 10, decay 1 (CDP's linear decay, without -N), sustain 3,
    // neither -T nor -K.
    //
    // A live frequency is CDP's breakpoint value, raised to 10 Hz; a static
    // one under 10 Hz is zero, as setup_pitchzero makes them. CDP divides the
    // amplification by the sustain once, but each frame for a breakpoint
    // amplification or sustain; a fixed amplification with a live sustain it
    // divides again at every frame, so the amplification falls away, which
    // the port keeps. Modes 5 to 8 read no sustain, nor modes 5 and 6 the
    // bandwidth, nor modes 5 to 8 the decay, -T or -K; there the
    // amplification acts as CDP's breakpoint amplification does (a fixed one
    // CDP divides by the zero sustain of those modes, and plays NaN).
    //
    // False, with no change, for a wave CDP refuses in the mode (a down ramp
    // in modes 3 and 7, an up ramp in 4 and 8), a rate outside 0 to the frame
    // rate, a start phase outside 0 to 1 (or under a half for a sine or saw
    // in modes 4 and 8), frequencies outside 5 Hz to half the sample rate, a
    // bandwidth outside the channel width to half the sample rate, an
    // amplification outside 0 to 10000, a decay outside 0.02 to 50, a sustain
    // outside 1 frame to an hour or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, Wave wave, double rate,
        double startPhase, double lowFrequency, double highFrequency, double bandwidth, double amplification,
        double decay, double sustain, bool track, bool keep, bool liveLow, bool liveHigh,
        bool liveAmplification, bool liveSustain);
    // False, with no change, before prepare or outside those ranges.
    [[nodiscard]] bool setControls(double rate, double lowFrequency, double highFrequency, double bandwidth,
        double amplification, double sustain) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // Half the sample rate and the narrowest bandwidth, once prepared.
    [[nodiscard]] double nyquist() const noexcept { return nyquist_; }
    [[nodiscard]] double narrowestBandwidth() const noexcept { return chwidth_; }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    // The frame rate, the fastest rate; zero for what the phase vocoder
    // refuses.
    [[nodiscard]] static double maximumRate(double sampleRate, int points, int overlap) noexcept;
    // The channel width, CDP's default and narrowest bandwidth; zero for
    // what the phase vocoder refuses.
    [[nodiscard]] static double minimumBandwidth(double sampleRate, int points, int overlap) noexcept;

private:
    [[nodiscard]] bool valid(double rate, double lowFrequency, double highFrequency, double bandwidth,
        double amplification, double sustain) const noexcept;
    void arpeggiate(std::span<float> frame) noexcept;
    void onOrBoost(std::span<float> frame, double bandhilimit, double bandlolimit) noexcept;
    [[nodiscard]] bool stillInStartPortion() noexcept;

    PhaseVocoder vocoder_;
    Mode mode_ = Mode::On;
    Wave wave_ = Wave::Sine;
    float frametime_ = 0.0F;
    float arate_ = 0.0F;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    double startPhase_ = 0.0;
    double decay_ = 1.0;
    // ARPE_SUSFLAG: fixed pitch (1), limited sustains (2), nonlinear (4).
    int susflag_ = 0;
    bool liveLow_ = false;
    bool liveHigh_ = false;
    bool liveAmplification_ = false;
    bool liveSustain_ = false;
    double rate_ = 1.0;
    double low_ = 0.0;
    double high_ = 0.0;
    // ARPE_HBAND: half the bandwidth.
    double halfBand_ = 0.0;
    double amplification_ = 10.0;
    int sustain_ = 3;
    double startRate_ = 1.0;
    double startLow_ = 0.0;
    double startHigh_ = 0.0;
    double startBandwidth_ = 0.0;
    double startAmplification_ = 10.0;
    double startSustain_ = 3.0;
    // ARPE_TAB, with the zero CDP reads one past its end, on the heap so
    // that a realtime operation stays small.
    std::vector<double> table_;
    // ARPE_WAVETABPOS, ARPE_LAST_TABPOS, ARPE_LASTARPFRQ and ARPE_AMPL.
    double wavetabpos_ = 0.0;
    double lastTabpos_ = 0.0;
    double lastRate_ = 1.0;
    double ampl_ = 0.0;
    bool started_ = false;
    bool inStartPortion_ = true;
    // ARPE_KEEP, and the start amplitude and frequency of each channel's
    // arpegtone (windowbuf[0]).
    std::vector<int> keep_;
    std::vector<float> windowbuf_;
};
}
