/*
 * Derived from the CDP System, Release 8: dev/externals/reverb/reverb.cpp,
 * reverberator.cpp and reflect.cpp
 * Copyright (c) 1983-2013 Richard Dobson and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
struct ReverbSettings
{
    // How much of the reverberator joins the early reflections, 0 to 1.
    double reverbGain = 0.5;
    // How much of the dry input the output takes, 0 up to 1.
    double mix = 0.5;
    // Seconds for the reverberation to fall 60 dB; it also chooses the small
    // (up to 0.6 s), medium (up to 1.3 s) or large room's reflections.
    double reverbTime = 1.0;
    // Absorption in the combs' feedback, 0 to 1.
    double damping = 0.5;
    // A one-pole lowpass on the reverberator's input; 0 leaves it out.
    double lowpassHz = 0.0;
    // Where the first reflection falls, in milliseconds; 0 keeps the room's.
    double predelayMilliseconds = 0.0;
    // A 12 dB low shelf and a one-pole lowpass on the input; 0 leaves them out.
    double lowCutHz = 0.0;
    double highCutHz = 0.0;
};

// CDP `reverb`: Moorer's reverberator after Brandt's Csound version. The
// input, with an optional low cut and high cut, feeds a room's early
// reflections, and they and the input feed six lowpass combs and four
// allpasses; the result, scaled by the reverb gain and added to the early
// reflections, reaches each output through its own allpass diffuser, mixed
// with the dry input. Two outputs from a mono input, as CDP's default.
class Reverb
{
public:
    // False, with no change, for values outside CDP's ranges: a reverb gain
    // outside 0 to 1, Mix outside 0 up to 1, a reverb time that is not
    // positive, damping outside 0 to 1, a lowpass beyond a third of the
    // sample rate, a negative predelay, or a high cut at or below the low
    // cut.
    [[nodiscard]] bool prepare(double sampleRate, const ReverbSettings& settings);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> left,
        std::span<double> right) noexcept;

private:
    struct Tap
    {
        double pos = 0.0;
        double val = 0.0;
    };
    // reverberator.cpp's tapdelay, with its output gain left at 1.
    struct TapLine
    {
        std::vector<double> buffer;
        std::vector<unsigned int> pointers;
        std::vector<unsigned int> startPointers;
        std::vector<double> coefficients;
        unsigned int reader = 0;
        double oneOverTaps = 0.0;
    };
    struct Allpass
    {
        std::vector<double> buffer;
        unsigned int reader = 0;
        unsigned int writer = 0;
        double gain = 0.0;
    };
    struct LowpassComb
    {
        std::vector<double> buffer;
        unsigned int reader = 0;
        unsigned int writer = 0;
        double gain = 0.0;
        double prescale = 1.0;
        // lowpass1.
        double lowpassGain = 0.0;
        double lowpassTemp = 0.0;
        double lowpassOutput = 0.0;
    };
    struct OnePole
    {
        bool active = false;
        double a1 = 1.0;
        double a2 = 0.0;
        double lastOut = 0.0;
    };
    struct ToneControl
    {
        bool active = false;
        double boost = 0.0;
        double a0 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double x1 = 0.0;
        double x2 = 0.0;
        double y1 = 0.0;
        double y2 = 0.0;
    };

    static double tick(TapLine& line, double sample) noexcept;
    static double tick(Allpass& filter, double sample) noexcept;
    static double tick(LowpassComb& comb, double sample) noexcept;
    static double tick(OnePole& filter, double sample) noexcept;
    static double tick(ToneControl& filter, double sample) noexcept;

    bool prepared_ = false;
    TapLine earlies_;
    std::array<LowpassComb, 6> combs_{};
    std::array<Allpass, 4> allpasses_{};
    std::array<Allpass, 2> diffusers_{};
    OnePole highCut_;
    OnePole lowpass_;
    ToneControl lowCut_;
    double scale_ = 1.0 / 6.0;
    float reverbGain_ = 0.0F;
    float dryGain_ = 0.0F;
    float wetGain_ = 1.0F;
};
}
