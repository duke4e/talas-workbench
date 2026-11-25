/*
 * Derived from the CDP System, Release 8: dev/editsf/cut.c,
 * dev/editsf/ap_edit.c and dev/cdp2k/tklib1.c
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

#include <cstddef>
#include <span>
#include <vector>

namespace talas::cdp
{
// CDP `sfedit noisecut`: noise, wavesets (zero crossing to zero crossing)
// shorter than one over Noise frequency, is replaced by silence. The input
// is split where it switches between noise and tone, a switch kept only if
// the noise before it lasted over Max noise, or the tone before it over
// Min tone (a switch too soon removes the one before); pairs of switches no
// more than two splices apart are dropped; tone fades in over the Splice
// after a switch into it and out over the Splice before a switch out of it.
// Keep noise (CDP's -n) silences the tone instead.
//
// CDP finds the switches over its whole file, then edits it; a switch is
// settled once the segment after it is long enough and no switch within two
// splices can follow, so the stream waits Min tone or Max noise, whichever
// is longer, and three splices (at least the waveset length of the noise
// frequency), and the output comes latencyFrames() late.
//
// CDP's second search starts where its first ended, not afresh: its first
// waveset always counts as noise, and the rest of its start follows from
// whether the first search found the file's last waveset to be noise, a
// whole-file fact a stream does not have: lastWavesetNoise is that choice. It
// changes the output only where the second search then keeps a switch the
// first did not, where CDP writes past its array (and may crash, or play as
// its algorithm would, which the stream does). CDP also keeps two faults the
// port emulates with bufferFrames, CDP's sound buffer length counted from the
// stream's start (CDP_MEMORY_BBSIZE sets other sizes): a waveset ending just
// as a buffer starts is merged with the next one, and a splice running across
// a buffer's start restarts its gain there from its count (not stepping on)
// and makes CDP skip the next switch, so that noise and tone swap roles until
// another such splice. CDP refuses a file that is all noise or all tone, and
// stops its output where final noise begins, which a stream cannot do, so the
// stream plays on; where CDP reads before its array (a first pair of switches
// dropped as too close), the stream keeps the next switch. Mono, as CDP.
class NoiseCut
{
public:
    static constexpr double maximumSpliceMs = 50.0;
    static constexpr double minimumNoiseFrequency = 1000.0;
    static constexpr double maximumMaxNoiseMs = 50.0;
    static constexpr double maximumMinToneMs = 1000.0;
    // CDP's one sound buffer for a mono input: its default 1 MB of memory.
    static constexpr std::size_t defaultBufferFrames = 262144;

    // False, with no change, for a sample rate that is not finite and
    // positive, values outside CDP's ranges (Splice 0 to 50 ms, Noise
    // frequency 1000 Hz to half the sample rate, Max noise 0 to 50 ms, Min
    // tone 0 to 1000 ms), no buffer frames, or a splice of the buffer's
    // length or more (rounded to frames), which CDP refuses.
    [[nodiscard]] bool prepare(double sampleRate, double spliceMs, double noiseFrequency, double maxNoiseMs,
        double minToneMs, bool keepNoise = false, bool lastWavesetNoise = false,
        std::size_t bufferFrames = defaultBufferFrames);

    void reset() noexcept;

    [[nodiscard]] bool prepared() const noexcept { return !ring_.empty(); }

    // max(three splices + the longer of Max noise and Min tone, the noise
    // waveset length - 1), in frames.
    [[nodiscard]] std::size_t latencyFrames() const noexcept { return latency_; }

    // False, with no change, before prepare, for spans of different sizes or
    // a non-finite input.
    [[nodiscard]] bool process(std::span<const double> input, std::span<double> output) noexcept;

    bool operator==(const NoiseCut&) const = default;

private:
    // A first-in first-out ring of frame positions, preallocated.
    struct Queue
    {
        std::vector<std::size_t> items;
        std::size_t head = 0;
        std::size_t count = 0;
        void clear() noexcept { head = count = 0; }
        [[nodiscard]] bool empty() const noexcept { return count == 0; }
        [[nodiscard]] std::size_t front() const noexcept { return items[head]; }
        [[nodiscard]] std::size_t back() const noexcept { return items[(head + count - 1) % items.size()]; }
        void push(std::size_t value) noexcept;
        void popFront() noexcept;
        void popBack() noexcept;
        bool operator==(const Queue&) const = default;
    };

    [[nodiscard]] bool scan(std::size_t frame, float sample) noexcept;
    void firstSearch(std::size_t position) noexcept;
    void secondSearch(std::size_t position) noexcept;
    void settle(std::size_t frame) noexcept;
    void eliminate(std::size_t point) noexcept;
    void assignSplice(std::size_t point) noexcept;
    [[nodiscard]] float edit(std::size_t frame) noexcept;
    [[nodiscard]] bool noisefirst() const noexcept;
    [[nodiscard]] float& at(std::size_t frame) noexcept { return ring_[frame & mask_]; }

    std::size_t splicelen_ = 0;
    std::size_t minfrq_ = 2;
    std::size_t minNoise_ = 0;
    std::size_t minTone_ = 0;
    std::size_t maxMin_ = 0;
    double splincr_ = 0.0;
    bool keepNoise_ = false;
    bool lastWavesetNoise_ = false;
    std::size_t bufferFrames_ = defaultBufferFrames;
    std::size_t latency_ = 0;
    std::vector<float> ring_;
    std::size_t mask_ = 0;
    std::size_t frame_ = 0;
    // The waveset scan: the signal's starting side (phase 0, positive),
    // and whether the second half of a waveset is under way.
    bool started_ = false;
    bool phase0_ = true;
    bool secondHalf_ = false;
    // CDP's first search, kept only for the noisefirst it leaves.
    int isnoise1_ = -1;
    bool noisefirst1_ = false;
    std::size_t brkpntcnt1_ = 0;
    std::size_t lastzcross1_ = 0;
    std::size_t lastpos1_ = 0;
    // CDP's second search: its switches stored but not yet settled, the
    // last two stored, and noisefirst if it set it.
    int isnoise2_ = 0;
    bool firstWaveset_ = true;
    std::size_t brkpntcnt2_ = 0;
    std::size_t lastzcross2_ = 0;
    std::size_t last_ = 0;
    std::size_t previous_ = 0;
    bool noisefirstSet_ = false;
    bool noisefirst2_ = false;
    Queue stored_;
    // The pairs no more than two splices apart dropped: switches not yet
    // settled, and how many have come.
    Queue stack_;
    std::size_t arrived_ = 0;
    // The settled switches' splice starts, how many have been assigned, and
    // whether the first was dropped as starting before the stream.
    Queue starts_;
    std::size_t assigned_ = 0;
    bool firstDropped_ = false;
    // The edit.
    bool inNoise_ = false;
    bool inSplice_ = false;
    std::size_t spliceCnt_ = 0;
    double spliceval_ = 0.0;
    double incr_ = 0.0;
    std::size_t skip_ = 0;
};
}
