/*
 * Derived from the CDP System, Release 8: dev/blur/blur.c and ap_blur.c,
 * dev/cdp2k/tklib3.c and dev/sfsys/osbind.c
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

#include "talas/cdp/Drand48.h"
#include "talas/cdp/PhaseVocoder.h"

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `blur chorus` and `blur noise` on each frame between `pvoc anal` and
// `pvoc synth`. CDP takes their fields as breakpoint files and reads them at
// each analysis frame's time: a value given before a process call reaches
// the frames analysed during it.

// `blur chorus`: each channel's amplitude is scaled by the amplitude spread
// to a random power from -1 to 1, or its frequency by the frequency spread
// to a random power from -1 to 1 (0 to 1 upwards, -1 to 0 downwards), the
// partial moving to the channel of its new frequency if louder than what
// that channel holds, or both. Scaled amplitudes are normalised to the
// frame's first total amplitude. The random powers come from tables CDP
// draws once, of four frames' floats each, read from a random place in
// each frame; CDP never seeds its generator, so they are the same at each
// run and after reset.
class BlurChorus
{
public:
    enum class Mode
    {
        Amplitude = 1,
        Frequency,
        FrequencyUp,
        FrequencyDown,
        AmplitudeAndFrequency,
        AmplitudeAndFrequencyUp,
        AmplitudeAndFrequencyDown,
    };

    // CDP's CHORU_MINAMP, CHORU_MAXAMP, CHORU_MINFRQ and CHORU_MAXFRQ.
    static constexpr double minimumAmplitudeSpread = 1.0;
    static constexpr double maximumAmplitudeSpread = 1028.0;
    static constexpr double minimumFrequencySpread = 1.0;
    static constexpr double maximumFrequencySpread = 4.0;

    // The live flags say which spreads CDP would read from breakpoint files.
    // Only both together matter: in modes 5 to 7 CDP then scales each
    // frequency by the bare random number (-1 to 1, 0 to 1 or -1 to 0)
    // where it means the spread to that power, so the frequency spread goes
    // unused, partials fall to a random fraction of their frequency in modes
    // 5 and 6, and in mode 7 only the rare negative analysis frequency
    // survives the sign change. False, with no change, for an amplitude
    // spread outside 1 to 1028 or a frequency spread outside 1 to 4, of the
    // spreads the mode reads (mode 1 reads no frequency spread, modes 2 to 4
    // no amplitude spread), or what the phase vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, Mode mode, double amplitudeSpread,
        double frequencySpread, bool liveAmplitudeSpread, bool liveFrequencySpread);
    // False, with no change, before prepare or outside those ranges.
    [[nodiscard]] bool setSpreads(double amplitudeSpread, double frequencySpread) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    [[nodiscard]] bool valid(double amplitudeSpread, double frequencySpread) const noexcept;
    void chorus(std::span<float> frame) noexcept;

    PhaseVocoder vocoder_;
    Mode mode_ = Mode::Amplitude;
    double nyquist_ = 0.0;
    double chwidth_ = 0.0;
    double halfchwidth_ = 0.0;
    double amplitudeSpread_ = 2.0;
    double frequencySpread_ = 1.1;
    double startAmplitudeSpread_ = 2.0;
    double startFrequencySpread_ = 1.1;
    // CHORU_RTABA and CHORU_RTABF, the random powers.
    std::vector<double> rtaba_;
    std::vector<double> rtabf_;
    // The generator after the tables, where each frame's draws begin.
    Drand48 startRandom_;
    Drand48 random_;
    bool bareFrequencyFactor_ = false;
    std::vector<float> windowbuf_;
};

// `blur noise`: each channel's amplitude moves by the noise, 0 to 1, from
// itself toward the frame's mean amplitude.
class BlurNoise
{
public:
    // False, with no change, for noise outside 0 to 1 or what the phase
    // vocoder refuses.
    [[nodiscard]] bool prepare(double sampleRate, int points, int overlap, double noise);
    // False, with no change, before prepare or outside 0 to 1.
    [[nodiscard]] bool setNoise(double noise) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool prepared() const noexcept { return vocoder_.prepared(); }
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return vocoder_.latencyFrames(); }
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

private:
    PhaseVocoder vocoder_;
    double noise_ = 0.0;
    double startNoise_ = 0.0;
};
}
