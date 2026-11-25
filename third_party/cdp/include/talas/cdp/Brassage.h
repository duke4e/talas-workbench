/*
 * Derived from the CDP System, Release 8: dev/modify/granula1.c,
 * dev/modify/brapcon.c, dev/modify/ap_modify.c, dev/cdp2k/tklib1.c,
 * dev/cdp2k/tklib3.c, dev/cdp2k/readdata.c and dev/newsfsys/osbind.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
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

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `modify brassage` modes 1 (pitchshift), 3 (reverb), 4 (scramble) and
// 5 (granulate): the input cut into grains that are copied (transposed,
// scaled, read from a little earlier) back over the output at about their
// own place, each with linear splices of round(5 ms) frames at its ends
// (CDP's tables, or its computed ramps over a grain under twice a splice).
//
// A grain starts at input frame P (the first at 0) and is G frames long, G
// = round(Grainsize ms x 0.001 x rate), Grainsize 50 ms in every mode but
// Scramble. The next starts ostep = round(G / Density) frames later (Density
// 2 where the mode has none): CDP's velocity is 1, so input and output
// advance alike. The grain is placed round(ostep x 0.5 x drand48()) frames
// after P (CDP's fixed scatter) and, where the mode has a Range, read from
// round(min(R, P) x drand48()) frames before P (R the Range in frames).
// Pitchshift and Reverb read it at a ratio of 2^(Pitch x 0.08333333333),
// stepping a read position (a double, which CDP counts from the start of
// its input buffer) and interpolating linearly; Reverb scales it by Amp,
// mixes a stereo input to mono ((L + R) x 0.5 in float) and places each
// grain between the left and right channel at a drawn position (dehole's
// gains). The draws per grain: the scatter, then the range, then (Reverb)
// the position.
//
// | Mode       | CDP | fields (all live)                | output         |
// | Pitchshift | 1   | Pitch (semitones)                | input channels |
// | Reverb     | 3   | Density, Pitch, Amp, Range (ms)  | 2 channels     |
// | Scramble   | 4   | Grainsize (ms), Range (ms)       | input channels |
// | Granulate  | 5   | Density                          | input channels |
//
// CDP takes every field as a breakpoint file and reads it at each grain's P
// (granulate's read_values_from_all_existing_brktables at absicnt / srate):
// a grain takes the values set when its frame P comes in, so a host that
// steps them between two frames at a block start plays alike. CDP sizes
// its buffers by its tables' extremes (the highest Pitch, the lowest
// Density, the longest Grainsize) and by CDP_MEMORY_BBSIZE (1024 kB by
// default); its input buffer (bufferFrames()) moves on whenever a grain
// starts past its end, and a grain whose Range takes it before the buffer's
// start reads a buffer of its own (baktrak_granula): the transposed read
// positions count from those starts, which changes their rounding. prepare
// takes the extremes as bounds that the setters may not pass.
//
// A grain needs the input up to its last frame read: the output comes
// latencyFrames() late, L = Gmax - 1 (Scramble, Granulate; Gmax the longest
// grain) or floor((G - 1) x 2^(highest Pitch x 0.08333333333)) + 2
// (Pitchshift, Reverb: the frame after the last one read, and one for the
// rounding of the summed read steps) frames, and each grain is computed in
// the call that takes frame P + L.
//
// CDP scales any output buffer whose peak passes 1.0, and then the whole
// file by the smallest such factor (write_samps_granula, renormalise): a
// stream cannot know its peak in advance, so it matches CDP bit for bit
// whenever no output sample passes 1 and otherwise plays unscaled, clipped
// at -1 and 1. CDP stops at the first grain that would read past its
// file's end; the stream goes on. CDP's drand48 is rand() / RAND_MAX
// seeded with time(): the seed is a setting here.
class Brassage
{
public:
    enum class Mode
    {
        Pitchshift,
        Reverb,
        Scramble,
        Granulate,
    };

    struct Settings
    {
        Mode mode = Mode::Pitchshift;
        // 1 or 2 (CDP8's build, without MULTICHAN, grains a stereo file's
        // channels alike and takes any other file as mono).
        int channels = 1;
        // Semitones: Pitchshift within +-maximumPitchshift(rate), Reverb
        // -0.33 to 0.33.
        double pitch = 0.0;
        // Reverb 2 to 16384 (no grain step under 1 frame), Granulate
        // 0.000002 to 2.
        double density = 8.0;
        // Reverb, 0 to 1.
        double amp = 1.0;
        // Scramble, ms, from 2.
        double grainsizeMs = 50.0;
        // Reverb 0 to 50 ms, Scramble from 0 ms.
        double rangeMs = 0.0;
        // The live fields' bounds (NaN, or a bound the value passes, is the
        // value): CDP's highest Pitch, lowest Density, longest Grainsize in
        // its tables. longestRangeMs only sizes the input history.
        double highestPitch = std::numeric_limits<double>::quiet_NaN();
        double lowestDensity = std::numeric_limits<double>::quiet_NaN();
        double longestGrainsizeMs = std::numeric_limits<double>::quiet_NaN();
        double longestRangeMs = std::numeric_limits<double>::quiet_NaN();
        // srand's seed (CDP: time()).
        std::uint32_t seed = 1;
        // CDP_MEMORY_BBSIZE, 10 to 20480 kB.
        int memoryKilobytes = 1024;
        bool operator==(const Settings&) const = default;
    };

    // The longest latency, input history or output span the stream takes.
    static constexpr std::size_t maximumFrames = std::size_t{1} << 22;
    static constexpr int maximumChannels = 2;

    // CDP's Pitchshift range: log2(nyquist / 10) octaves, in semitones.
    [[nodiscard]] static double maximumPitchshift(double sampleRate) noexcept;

    // False, with no change, for a sample rate that is not finite and
    // positive, values outside the ranges (within 0.000002 of a range end is
    // that end), a grain under 2 frames, a Reverb Density whose grain step
    // rounds to 0 frames, or a latency, history or output span over
    // maximumFrames.
    [[nodiscard]] bool prepare(double sampleRate, const Settings& settings);

    // The live fields, for the grains starting from the next process call
    // on. False, with no change, before prepare, for a field the mode does
    // not have, a value outside its range or past prepare's bound.
    [[nodiscard]] bool setPitch(double semitones) noexcept;
    [[nodiscard]] bool setDensity(double density) noexcept;
    [[nodiscard]] bool setAmp(double amp) noexcept;
    [[nodiscard]] bool setGrainsize(double milliseconds) noexcept;
    [[nodiscard]] bool setRange(double milliseconds) noexcept;

    // Silence, prepare's fields, the first grain at the next frame, the
    // draws from the seed.
    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return inputChannels_ > 0; }

    [[nodiscard]] std::size_t latencyFrames() const noexcept { return latency_; }

    [[nodiscard]] std::size_t inputChannels() const noexcept { return inputChannels_; }

    // 2 for Reverb, else inputChannels().
    [[nodiscard]] std::size_t outputChannels() const noexcept { return outputChannels_; }

    // CDP's input buffer (its dz->buflen, in grain frames), which its
    // transposed reads count from.
    [[nodiscard]] std::size_t bufferFrames() const noexcept { return bufferFrames_; }

    // Both interleaved. False, with no change, before prepare, for an input
    // that is not whole frames, an output not outputChannels() samples for
    // each input frame, or a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const Brassage&) const = default;

private:
    struct Fields
    {
        double pitch = 0.0;
        double density = 0.0;
        double amp = 0.0;
        double grainsizeMs = 0.0;
        double rangeMs = 0.0;
        // Converted as CDP converts its parameters.
        double octaves = 0.0;
        long grainsize = 0;
        long range = 0;
        bool operator==(const Fields&) const = default;
    };

    struct Grain
    {
        std::size_t start = 0;
        std::size_t place = 0;
        std::size_t read = 0;
        long offset = 0;
        long size = 0;
        double transposition = 1.0;
        float amp = 1.0F;
        double position = 0.0;
        bool operator==(const Grain&) const = default;
    };

    struct Random
    {
        Drand48 generator;
        bool operator==(const Random& other) const noexcept { return generator.state == other.generator.state; }
    };

    [[nodiscard]] bool pitched() const noexcept
    {
        return settings_.mode == Mode::Pitchshift || settings_.mode == Mode::Reverb;
    }
    [[nodiscard]] bool hasDensity() const noexcept
    {
        return settings_.mode == Mode::Reverb || settings_.mode == Mode::Granulate;
    }
    [[nodiscard]] bool hasRange() const noexcept
    {
        return settings_.mode == Mode::Reverb || settings_.mode == Mode::Scramble;
    }
    [[nodiscard]] long outstep(const Fields& fields) const noexcept;
    void schedule() noexcept;
    void render(const Grain& grain) noexcept;
    void splice(long size) noexcept;
    [[nodiscard]] float input(std::size_t frame, std::size_t channel) const noexcept
    {
        return ring_[(frame & ringMask_) * grainChannels_ + channel];
    }

    Settings settings_;
    Fields first_;
    Fields fields_;
    double sampleRate_ = 0.0;
    double convertor_ = 0.0;
    double pitchLimit_ = 0.0;
    std::size_t inputChannels_ = 0;
    std::size_t grainChannels_ = 0;
    std::size_t outputChannels_ = 0;
    std::size_t latency_ = 0;
    std::size_t bufferFrames_ = 0;
    long splice_ = 0;
    long fixedGrain_ = 0;

    std::vector<float> ring_;
    std::size_t ringMask_ = 0;
    std::vector<float> out_;
    std::size_t outMask_ = 0;
    std::vector<float> grain_;
    std::vector<double> beginTable_;
    std::vector<double> endTable_;
    std::vector<Grain> queue_;
    std::size_t queueFirst_ = 0;
    std::size_t queueCount_ = 0;

    Random random_;
    std::size_t frame_ = 0;
    std::size_t next_ = 0;
    std::size_t bufferStart_ = 0;
};
}
