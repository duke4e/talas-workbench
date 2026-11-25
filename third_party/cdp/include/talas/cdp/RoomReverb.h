/*
 * Derived from the CDP System, Release 8: dev/externals/reverb/rmverb.cpp,
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
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp
{
// One line of rmverb's early-reflection file: a time in seconds and an
// amplitude above 0 up to 1.
struct RoomReflection
{
    double time = 0.0;
    double amplitude = 0.0;

    bool operator==(const RoomReflection&) const = default;
};

// The reflections a file gives, read as rmverb's readtaps reads them: in
// lines of up to 255 characters, a line that is empty or starts with a
// semicolon skipped, a time under one sample taken as zero, and a repeated
// time other than zero dropped. Nothing for a line without two numbers, a
// negative or decreasing time, an amplitude outside 0 (exclusive) to 1, no
// reflections, or more than RoomReverb::maximumReflections.
[[nodiscard]] std::optional<std::vector<RoomReflection>> readRoomReflections(
    std::string_view text,
    double sampleRate);

// rmverb's rmsize: Gardner's small, medium and large rooms.
enum class RoomSize
{
    small = 1,
    medium = 2,
    large = 3,
};

struct RoomReverbSettings
{
    RoomSize room = RoomSize::small;
    // How much of the dense reverberation joins the early reflections,
    // above 0 up to 1.
    double reverbGain = 0.5;
    // How much of the dry input the output takes, 0 to 1; the reverberation
    // takes twice the rest.
    double mix = 0.5;
    // Feedback around the room's diffuser, 0 to 1: it sets the decay.
    double feedback = 0.5;
    // The cutoff of the lowpass in the diffuser's feedback (air absorption),
    // 0 up to 0.35 of the sample rate; 0 leaves it out.
    double absorbHz = 2500.0;
    // A one-pole lowpass on the reverberator's input, 0 up to 0.35 of the
    // sample rate; 0 leaves it out.
    double lowpassHz = 0.0;
    // Where the first reflection falls, in milliseconds, up to 10 s; a
    // negative value keeps the room's, 0 puts it on the direct sound.
    double predelayMilliseconds = -1.0;
    // 12 dB low and high shelves on the input; 0 leaves them out.
    double lowCutHz = 0.0;
    double highCutHz = 0.0;
    // A lowpass inside each nested allpass's loop too (rmverb's -d), at the
    // absorption cutoff.
    bool doubleDamping = false;
    // rmverb's -c1: one output, its dry part taken after the input lowpass,
    // written to both outputs.
    bool monoOutput = false;
    // rmverb's -e: these early reflections in place of the room's; empty
    // keeps the room's.
    std::vector<RoomReflection> reflections;
};

// CDP `rmverb`: a room reverb after Gardner's nested-allpass diffusers. The
// input, with an optional low cut and high cut, and an optional lowpass on
// the reverberator's feed, drives a room's early reflections; they and the
// feed enter the room's diffuser, whose output, scaled by the reverb gain
// and 0.707 and added to the early reflections, reaches each output through
// its own pair of allpasses, mixed with the dry input. A mono input and two
// outputs, as CDP's default; no trailer, as the tail plays on in the stream.
class RoomReverb
{
public:
    static constexpr std::size_t maximumReflections = 256;
    static constexpr double maximumReflectionSeconds = 10.0;
    static constexpr double maximumPredelayMilliseconds = 10000.0;

    // False, with no change, for values outside CDP's ranges: a room other
    // than 1 to 3, a reverb gain outside 0 (exclusive) to 1, Mix or Feedback
    // outside 0 to 1, an absorption or lowpass cutoff outside 0 to 0.35 of
    // the sample rate, a negative cut, a high cut at or below the low cut,
    // more than maximumReflections reflections, a reflection time that
    // decreases or lies outside 0 to maximumReflectionSeconds, an amplitude
    // outside 0 (exclusive) to 1, a predelay over 10 s, a reflection that
    // (after the predelay) falls within a sample of the direct sound but not
    // on it, where CDP's tap would start past its line, or a sample rate too
    // low for the large room's delays (under 334 Hz).
    [[nodiscard]] bool prepare(double sampleRate, const RoomReverbSettings& settings);

    // Silence in every line and filter.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> left,
        std::span<double> right) noexcept;

private:
    // reverberator.cpp's delay, its gain left at 1.
    struct Delay
    {
        std::vector<double> buffer;
        unsigned int ptr = 0;
    };
    // allpassfilter, its buffer half as long again as its time (VMODMAX),
    // with an optional pre-delay line.
    struct Allpass
    {
        std::vector<double> buffer;
        unsigned int reader = 0;
        unsigned int writer = 0;
        double gain = 0.0;
        bool preDelayed = false;
        Delay preDelay;
    };
    struct OnePole
    {
        double a1 = 1.0;
        double a2 = 0.0;
        double lastOut = 0.0;
    };
    // nested_allpass: an outer allpass around one or two plain allpasses,
    // with the -d lowpass in its loop.
    struct NestedAllpass
    {
        std::vector<double> buffer;
        unsigned int reader = 0;
        unsigned int writer = 0;
        double outerGain = 0.0;
        bool hasFirst = false;
        bool hasSecond = false;
        Allpass first;
        Allpass second;
        bool damping = false;
        OnePole lowpass;
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

    static double tick(Delay& line, double sample) noexcept;
    static double tick(Allpass& filter, double sample) noexcept;
    static double tick(OnePole& filter, double sample) noexcept;
    static double tick(NestedAllpass& filter, double sample) noexcept;
    static double tick(ToneControl& filter, double sample) noexcept;
    static double tick(TapLine& line, double sample) noexcept;
    double diffuse(double sample) noexcept;

    bool prepared_ = false;
    RoomSize room_ = RoomSize::small;
    // The room's diffuser: small_diffuser, medium_diffuser or
    // large_diffuser, each using the members it names.
    std::array<NestedAllpass, 2> nested_{};
    bool secondNested_ = false;
    Allpass innerAllpass1_;
    Allpass innerAllpass2_;
    bool preDelayed_ = false;
    Delay preDelay_;
    Delay postDelay_;
    Delay delay1_;
    Delay delay2_;
    Delay delay3_;
    Delay delay4_;
    OnePole feedbackLowpass_;
    double feedbackGain_ = 0.0;
    double out1_ = 0.0;
    double out2_ = 0.0;
    double out3_ = 0.0;
    // rmverb.cpp's main loop.
    TapLine earlies_;
    std::array<Allpass, 2> leftAllpasses_{};
    std::array<Allpass, 2> rightAllpasses_{};
    ToneControl lowCut_;
    ToneControl highCut_;
    bool lowpassActive_ = false;
    OnePole lowpass_;
    double diffGain_ = 1.0;
    double dryGain_ = 0.0;
    double wetGain_ = 2.0;
    bool mono_ = false;
};
}
