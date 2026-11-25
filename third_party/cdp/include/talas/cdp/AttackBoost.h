/*
 * Derived from the CDP System, Release 8: dev/env/envprepro.c,
 * dev/env/envfuncs.c, dev/env/envprocess.c, dev/env/envel.c,
 * dev/env/ap_envel.c and dev/cdp2k/tklib1.c
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

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `envel attack` modes 1 to 3: one attack point is boosted by Gain, the
// gain rising over Onset ms before it and falling back over Decay ms after
// it, as the square of the time over a 16-step breakpoint curve
// (Exponential, CDP's default -t1) or linearly (-t0), applied by CDP's
// breakpoint enveloping. Gate (CDP's
// mode 1) takes the attack at the first sample over Level, or the loudest
// at its peak; ApproximateTime (mode 2) takes the loudest sample in the 400
// ms from 200 ms before Level seconds (rounded down to 256 frames);
// ExactTime (mode 3) takes Level seconds itself.
//
// CDP acts once per file, as the stream does once after prepare or reset,
// its times counted from there; the gain is 1 before and after, so the
// stream passes the input on unchanged until the attack is found and once
// its decay is over. It waits the onset (plus the 400 ms search for mode 2,
// or peakWaitFrames for mode 1) to know where the gain begins to rise, so
// the output comes latencyFrames() late. CDP takes the onset up to 32767
// ms; the stream takes at most maximumOnsetMs. CDP's mode 1 runs on through
// a peak held at one level until it falls (taking the held level's first
// sample, or a later higher one); the stream waits peakWaitFrames for it to
// fall. CDP refuses a file where the gate is never passed, the attack is in
// its last 5 ms, or the boost might clip; a stream cannot know, so it plays
// on; CDP's decay stops at its file's end, a stream's does not. Mode 4,
// the loudest sample in the whole file, is not streamable. CDP applies its
// envelope a buffer at a time, leaving a stretch of gain within 0.000002
// of 1 and stepping by as little unchanged from a buffer's start:
// bufferFrames is CDP's buffer length (CDP_MEMORY_BBSIZE sets others).
class AttackBoost
{
public:
    enum class Mode
    {
        Gate,
        ApproximateTime,
        ExactTime,
    };

    enum class Envelope
    {
        Linear,
        Exponential,
    };

    static constexpr double minimumGain = 1.0;
    static constexpr double maximumGain = 32767.0;
    static constexpr double minimumOnsetMs = 5.0;
    static constexpr double maximumOnsetMs = 1000.0;
    static constexpr double minimumDecayMs = 5.0;
    static constexpr double maximumDecayMs = 3600000.0;
    static constexpr double maximumTimeSeconds = 3600.0;
    static constexpr std::size_t peakWaitFrames = 64;
    // CDP's buffer for breakpoint enveloping a mono input: its default 1 MB
    // less one 256-sample sector.
    static constexpr std::size_t defaultBufferFrames = 261888;

    // False, with no change, for a sample rate that is not finite and
    // positive, or values outside their ranges: Level 0 to 1 (Gate) or 0
    // to 3600 s (the times, where CDP allows up to its file's length less 5
    // ms), Gain 1 to 32767, Onset 5 to 1000 ms (CDP: 32767), Decay 5 ms to
    // an hour (CDP: its file's length less 5 ms), and at least one buffer
    // frame.
    [[nodiscard]] bool prepare(double sampleRate, Mode mode, double level, double gain, double onsetMs,
        double decayMs, Envelope envelope = Envelope::Exponential, std::size_t bufferFrames = defaultBufferFrames);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return sampleRate_ > 0.0; }

    // ceil(onset in frames) + 1, plus peakWaitFrames (Gate) or the 400 ms
    // search (ApproximateTime); 0 for ExactTime, whose envelope is known
    // from the start.
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return latency_; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const AttackBoost&) const = default;

private:
    // At most two plain breakpoints and two 16-step curves, then 1 for good.
    static constexpr std::size_t maximumBreakpoints = 40;

    void find(std::size_t frame, float sample) noexcept;
    void buildEnvelope(double attackTime) noexcept;
    [[nodiscard]] float envelopeFrame(std::size_t frame, float sample) noexcept;
    void startSegment(std::size_t index) noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    double sampleRate_ = 0.0;
    Mode mode_ = Mode::Gate;
    double level_ = 0.0;
    double gain_ = 1.0;
    double onsetMs_ = minimumOnsetMs;
    double decayMs_ = minimumDecayMs;
    bool steep_ = false;
    // ENVTYPE_STEEP's exponent, kept as data so that pow is called as CDP8
    // calls it.
    double attenator_ = 2.0;
    std::size_t bufferFrames_ = defaultBufferFrames;
    std::size_t latency_ = 0;
    std::size_t windowStart_ = 0;
    std::size_t searchLimit_ = 0;
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    // The search for the attack point.
    bool found_ = false;
    bool decided_ = false;
    std::size_t attackPoint_ = 0;
    double attackLevel_ = -1.0;
    // The envelope, as CDP's sample numbers and gains, and CDP's pass under
    // way through it.
    std::array<std::size_t, maximumBreakpoints> samples_{};
    std::array<double, maximumBreakpoints> gains_{};
    std::size_t breakpoints_ = 0;
    bool envelopeReady_ = false;
    std::size_t segment_ = 0;
    std::size_t nextSample_ = 0;
    double thisGain_ = 1.0;
    double thisIncr_ = 0.0;
    double thisStep_ = 0.0;
    bool passSkip_ = true;
};
}
