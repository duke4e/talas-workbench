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
#include "talas/cdp/Brassage.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR, MS_TO_SECS, OCTAVES_PER_SEMITONE and ONE_OVER_LN2;
// modicon.h's brassage defaults and limits; brapcon.c's SAFETY, OSAFETY and
// GRS_ROUNDUP; tkglobals.h's F_SECSIZE; granula1.c's dehole constants.
constexpr double FLTERR = 0.000002;
constexpr double MS_TO_SECS = 0.001;
constexpr double OCTAVES_PER_SEMITONE = 0.08333333333;
constexpr double ONE_OVER_LN2 = 1.442695;
constexpr double SEMITONES_PER_OCTAVE = 12.0;
constexpr double MIN_LIKELY_PITCH = 10.0;
constexpr double GRS_DEFAULT_DENSITY = 2.0;
constexpr double GRS_MAX_DENSITY = 16384.0;
constexpr double GRS_DEFAULT_GRAINSIZE = 50.0;
constexpr double GRS_DEFAULT_REVERB_PSHIFT = 0.33;
constexpr double GRS_DEFAULT_REVERB_SRCHRANGE = 50.0;
constexpr double GRS_DEFAULT_SPLICELEN = 5.0;
constexpr double GRS_MIN_SPLICELEN = 1.0;
constexpr double GRS_DEFAULT_SCATTER = 0.5;
constexpr long SAFETY = 4096;
constexpr long OSAFETY = 256;
constexpr double GRS_ROUNDUP = 0.9999;
constexpr long F_SECSIZE = 256;
constexpr double NONLIN = 0.5;
constexpr double DEVIATE = 0.25;

// tklib1.c's flteq.
[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const auto upperbnd = f2 + FLTERR;
    const auto lowerbnd = f2 - FLTERR;
    return !(f1 > upperbnd || f1 < lowerbnd);
}

// readdata.c: a value within FLTERR of a range end is that end.
[[nodiscard]] bool inRange(double& value, double lo, double hi) noexcept
{
    if (!std::isfinite(value))
        return false;
    if (flteq(value, lo))
        value = lo;
    if (flteq(value, hi))
        value = hi;
    return !(value > hi || value < lo);
}

[[nodiscard]] std::size_t ceilPow2(std::size_t value) noexcept
{
    return std::bit_ceil(std::max<std::size_t>(value, 1));
}

// granula1.c's dehole, as CDP8's build compiles it (pow(comp, 0.5) as a
// square root, pos * 2 - 1 fused).
[[nodiscard]] double dehole(double pos) noexcept
{
    auto comp = 1.0 - std::fabs(std::fma(pos, 2.0, -1.0));
    comp = std::fabs(std::sqrt(comp));
    static_assert(NONLIN == 0.5);
    comp *= DEVIATE;
    comp += (1.0 - DEVIATE);
    return comp;
}
}

double Brassage::maximumPitchshift(double sampleRate) noexcept
{
    // tklib1.c: LOG2(nyquist / MIN_LIKELY_PITCH) * SEMITONES_PER_OCTAVE.
    return std::log((sampleRate / 2.0) / MIN_LIKELY_PITCH) * ONE_OVER_LN2 * SEMITONES_PER_OCTAVE;
}

long Brassage::outstep(const Fields& fields) const noexcept
{
    // granula1.c's set_outstep.
    const auto size = settings_.mode == Mode::Scramble ? fields.grainsize : fixedGrain_;
    if (hasDensity())
        return std::lround(static_cast<double>(size) / fields.density);
    return std::lround(static_cast<double>(size) / GRS_DEFAULT_DENSITY);
}

bool Brassage::prepare(double sampleRate, const Settings& settings)
{
    auto s = settings;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || s.mode < Mode::Pitchshift || s.mode > Mode::Granulate
        || s.channels < 1 || s.channels > maximumChannels || s.memoryKilobytes < 10 || s.memoryKilobytes > 20 * 1024)
    {
        return false;
    }
    const auto pitchLimit = s.mode == Mode::Reverb ? GRS_DEFAULT_REVERB_PSHIFT : maximumPitchshift(sampleRate);
    const auto unbounded = std::numeric_limits<double>::max();
    // A bound that is NaN, or that the value passes, is the value.
    const auto bound = [](double& b, double value, double lo, double hi, bool highest) {
        if (std::isnan(b))
        {
            b = value;
            return true;
        }
        if (!inRange(b, lo, hi))
            return false;
        b = highest ? std::max(b, value) : std::min(b, value);
        return true;
    };
    switch (s.mode)
    {
    case Mode::Pitchshift:
        if (!(pitchLimit > 0.0) || !inRange(s.pitch, -pitchLimit, pitchLimit)
            || !bound(s.highestPitch, s.pitch, -pitchLimit, pitchLimit, true))
        {
            return false;
        }
        break;
    case Mode::Reverb:
        if (!inRange(s.density, GRS_DEFAULT_DENSITY, GRS_MAX_DENSITY) || !inRange(s.pitch, -pitchLimit, pitchLimit)
            || !inRange(s.amp, 0.0, 1.0) || !inRange(s.rangeMs, 0.0, GRS_DEFAULT_REVERB_SRCHRANGE)
            || !bound(s.lowestDensity, s.density, GRS_DEFAULT_DENSITY, GRS_MAX_DENSITY, false)
            || !bound(s.highestPitch, s.pitch, -pitchLimit, pitchLimit, true)
            || !bound(s.longestRangeMs, s.rangeMs, 0.0, GRS_DEFAULT_REVERB_SRCHRANGE, true))
        {
            return false;
        }
        break;
    case Mode::Scramble:
        // tklib1.c: the lowest Grainsize falls through to GRS_MIN_SPLICELEN
        // * 2.
        if (!inRange(s.grainsizeMs, GRS_MIN_SPLICELEN * 2.0, unbounded) || !inRange(s.rangeMs, 0.0, unbounded)
            || !bound(s.longestGrainsizeMs, s.grainsizeMs, GRS_MIN_SPLICELEN * 2.0, unbounded, true)
            || !bound(s.longestRangeMs, s.rangeMs, 0.0, unbounded, true))
        {
            return false;
        }
        break;
    case Mode::Granulate:
        if (!inRange(s.density, FLTERR, GRS_DEFAULT_DENSITY)
            || !bound(s.lowestDensity, s.density, FLTERR, GRS_DEFAULT_DENSITY, false))
        {
            return false;
        }
        break;
    }
    // The fields a mode has not: CDP's defaults (tklib1.c), their bounds
    // the values.
    if (s.mode != Mode::Pitchshift && s.mode != Mode::Reverb)
        s.highestPitch = s.pitch = 0.0;
    if (s.mode != Mode::Reverb && s.mode != Mode::Granulate)
        s.lowestDensity = s.density = GRS_DEFAULT_DENSITY;
    if (s.mode != Mode::Reverb)
        s.amp = 1.0;
    if (s.mode != Mode::Scramble)
        s.longestGrainsizeMs = s.grainsizeMs = GRS_DEFAULT_GRAINSIZE;
    if (s.mode != Mode::Reverb && s.mode != Mode::Scramble)
        s.longestRangeMs = s.rangeMs = 0.0;
    const bool isPitched = s.mode == Mode::Pitchshift || s.mode == Mode::Reverb;
    const bool isDensity = s.mode == Mode::Reverb || s.mode == Mode::Granulate;
    const bool isRange = s.mode == Mode::Reverb || s.mode == Mode::Scramble;
    const bool isScramble = s.mode == Mode::Scramble;
    const auto limit = static_cast<double>(maximumFrames);

    // param_check_and_convert: the times to frames.
    const auto convertor = MS_TO_SECS * sampleRate;
    const auto toFrames = [convertor, limit](double ms, long& frames) {
        const auto value = ms * convertor;
        if (!(value < limit))
            return false;
        frames = std::lround(value);
        return true;
    };
    long fixedGrain = 0, grain = 0, longestGrain = 0, range = 0, longestRange = 0, shortestGrain = 0;
    if (!toFrames(GRS_DEFAULT_GRAINSIZE, fixedGrain) || !toFrames(s.grainsizeMs, grain)
        || !toFrames(s.longestGrainsizeMs, longestGrain) || !toFrames(GRS_MIN_SPLICELEN * 2.0, shortestGrain)
        || !toFrames(s.rangeMs, range) || !toFrames(s.longestRangeMs, longestRange))
    {
        return false;
    }
    const auto splice = std::lround(GRS_DEFAULT_SPLICELEN * convertor);
    if (!isScramble)
    {
        grain = fixedGrain;
        longestGrain = fixedGrain;
        shortestGrain = fixedGrain;
    }
    if (!isRange)
    {
        range = 0;
        longestRange = 0;
    }
    if (shortestGrain < 2)
        return false;
    // set_outstep: a step of 0 is CDP's "velocity or instep has become so
    // small" refusal.
    if (isDensity && std::lround(static_cast<double>(fixedGrain) / s.density) < 1)
        return false;

    const auto chans = static_cast<long>(s.channels);
    // initialise_channel_configuration (CDP8's build leaves modicon.h's
    // MULTICHAN undefined): Reverb spatialises (mixing a stereo input to
    // mono) to stereo.
    const bool spatialise = s.mode == Mode::Reverb;
    const long inchans = spatialise ? 1 : chans;
    const long outchans = spatialise ? 2 : chans;
    const bool stereo = chans == 2;

    // calc_overflows (after convert_pitch), calc_max_scatter and
    // adjust_overflows: CDP's buffer overflows from its tables' extremes.
    const auto highestOctaves = s.highestPitch * OCTAVES_PER_SEMITONE;
    const auto mt = std::exp2(isPitched ? highestOctaves : 1.0);
    const auto mgi = longestGrain + 1;
    if (!(static_cast<double>(mgi) * mt < limit))
        return false;
    const auto bufSmpxs = (std::lround(static_cast<double>(mgi) * mt) + SAFETY) * inchans;
    const auto scatterDensity = isDensity ? s.lowestDensity : GRS_DEFAULT_DENSITY;
    const auto osDouble = static_cast<double>(mgi) / scatterDensity + GRS_ROUNDUP;
    if (!(osDouble < limit))
        return false;
    const auto os = static_cast<long>(osDouble);
    const auto mscat = static_cast<long>(std::fma(static_cast<double>(os), GRS_DEFAULT_SCATTER, GRS_ROUNDUP));
    const auto lbufSmpxs = (mgi + OSAFETY + mscat) * outchans;

    // grab_an_appropriate_block_of_memory and create_granula_buffers:
    // Malloc(-1) is CDP_MEMORY_BBSIZE kB.
    const long bufdivisor = (stereo ? 2 : 0) + 3 + (spatialise ? 2 : 0);
    const long standardBlock = static_cast<long>(s.memoryKilobytes) * 1024 / static_cast<long>(sizeof(float));
    long thisBloksize = standardBlock;
    long buflen = 0;
    while (buflen <= 0)
    {
        buflen = thisBloksize;
        thisBloksize += standardBlock;
        buflen -= bufSmpxs + lbufSmpxs;
        if (stereo)
            buflen -= 2 * bufSmpxs;
        buflen /= bufdivisor;
        const auto sectorBlok = F_SECSIZE * (stereo ? 2 : 1);
        buflen = (buflen / sectorBlok) * sectorBlok;
    }
    const auto bbbSize = buflen;
    while (lbufSmpxs > buflen * (spatialise ? 2 : 1))
        buflen += bbbSize;

    // The latency: the grain's last frame read (its next frame, when
    // transposed, which the sums of transpositions may round past).
    std::size_t latency = 0;
    if (isPitched)
    {
        const auto reach = std::floor(static_cast<double>(fixedGrain - 1) * mt);
        if (!(reach + 2.0 < limit))
            return false;
        latency = static_cast<std::size_t>(reach) + 2;
    }
    else
    {
        latency = static_cast<std::size_t>(longestGrain - 1);
    }
    const auto history = latency + static_cast<std::size_t>(longestRange) + 2;
    // The output: a grain lands up to half its step late.
    const auto longestStep = static_cast<double>(longestGrain) / scatterDensity;
    if (history > maximumFrames || !(longestStep < limit))
        return false;
    const auto longestScatter = static_cast<std::size_t>(std::lround(longestStep * GRS_DEFAULT_SCATTER)) + 1;
    const auto span = longestScatter + static_cast<std::size_t>(longestGrain) + 2;
    if (span > maximumFrames)
        return false;
    long shortestStep = 1;
    if (s.mode != Mode::Reverb)
        shortestStep = std::max(1L, std::lround(static_cast<double>(shortestGrain) / GRS_DEFAULT_DENSITY));

    settings_ = s;
    sampleRate_ = sampleRate;
    convertor_ = convertor;
    pitchLimit_ = pitchLimit;
    inputChannels_ = static_cast<std::size_t>(chans);
    grainChannels_ = static_cast<std::size_t>(inchans);
    outputChannels_ = static_cast<std::size_t>(outchans);
    latency_ = latency;
    bufferFrames_ = static_cast<std::size_t>(buflen / inchans);
    splice_ = splice;
    fixedGrain_ = fixedGrain;
    first_ = {};
    first_.pitch = s.pitch;
    first_.density = s.density;
    first_.amp = s.amp;
    first_.grainsizeMs = s.grainsizeMs;
    first_.rangeMs = s.rangeMs;
    first_.octaves = s.pitch * OCTAVES_PER_SEMITONE;
    first_.grainsize = grain;
    first_.range = range;

    const auto ringFrames = ceilPow2(history);
    ring_.assign(ringFrames * grainChannels_, 0.0F);
    ringMask_ = ringFrames - 1;
    const auto outFrames = ceilPow2(span);
    out_.assign(outFrames * outputChannels_, 0.0F);
    outMask_ = outFrames - 1;
    grain_.assign((static_cast<std::size_t>(longestGrain) + 1) * grainChannels_, 0.0F);
    queue_.assign(latency / static_cast<std::size_t>(shortestStep) + 2, Grain{});

    // make_splice_tables (linear).
    beginTable_.assign(static_cast<std::size_t>(splice) + 2, 0.0);
    endTable_.assign(static_cast<std::size_t>(splice) + 2, 0.0);
    if (splice > 0)
    {
        const auto length = static_cast<double>(splice);
        const auto dif = 1.0 / length;
        auto lastval = dif;
        beginTable_[0] = 0.0;
        beginTable_[1] = lastval;
        for (long n = 2; n < splice; n++)
        {
            const auto val = lastval + dif;
            lastval = val;
            beginTable_[static_cast<std::size_t>(n)] = val;
        }
        auto lastsum = dif;
        auto e = static_cast<std::size_t>(splice);
        endTable_[--e] = 0.0;
        if (e > 0)
            endTable_[--e] = lastsum;
        for (long n = splice - 3; n >= 0; n--)
        {
            const auto val = lastsum + dif;
            lastsum = val;
            endTable_[--e] = val;
        }
    }
    reset();
    return true;
}

bool Brassage::setPitch(double semitones) noexcept
{
    if (!prepared() || !pitched() || !inRange(semitones, -pitchLimit_, pitchLimit_)
        || semitones > settings_.highestPitch)
    {
        return false;
    }
    fields_.pitch = semitones;
    fields_.octaves = semitones * OCTAVES_PER_SEMITONE;
    return true;
}

bool Brassage::setDensity(double density) noexcept
{
    if (!prepared() || !hasDensity())
        return false;
    const auto hi = settings_.mode == Mode::Reverb ? GRS_MAX_DENSITY : GRS_DEFAULT_DENSITY;
    const auto lo = settings_.mode == Mode::Reverb ? GRS_DEFAULT_DENSITY : FLTERR;
    if (!inRange(density, lo, hi) || density < settings_.lowestDensity
        || std::lround(static_cast<double>(fixedGrain_) / density) < 1)
    {
        return false;
    }
    fields_.density = density;
    return true;
}

bool Brassage::setAmp(double amp) noexcept
{
    if (!prepared() || settings_.mode != Mode::Reverb || !inRange(amp, 0.0, 1.0))
        return false;
    fields_.amp = amp;
    return true;
}

bool Brassage::setGrainsize(double milliseconds) noexcept
{
    if (!prepared() || settings_.mode != Mode::Scramble
        || !inRange(milliseconds, GRS_MIN_SPLICELEN * 2.0, std::numeric_limits<double>::max())
        || milliseconds > settings_.longestGrainsizeMs)
    {
        return false;
    }
    fields_.grainsizeMs = milliseconds;
    fields_.grainsize = std::lround(milliseconds * convertor_);
    return true;
}

bool Brassage::setRange(double milliseconds) noexcept
{
    const auto hi = settings_.mode == Mode::Reverb ? GRS_DEFAULT_REVERB_SRCHRANGE : std::numeric_limits<double>::max();
    if (!prepared() || !hasRange() || !inRange(milliseconds, 0.0, hi) || milliseconds > settings_.longestRangeMs)
        return false;
    fields_.rangeMs = milliseconds;
    fields_.range = std::lround(milliseconds * convertor_);
    return true;
}

void Brassage::reset() noexcept
{
    fields_ = first_;
    std::fill(ring_.begin(), ring_.end(), 0.0F);
    std::fill(out_.begin(), out_.end(), 0.0F);
    std::fill(grain_.begin(), grain_.end(), 0.0F);
    std::fill(queue_.begin(), queue_.end(), Grain{});
    queueFirst_ = 0;
    queueCount_ = 0;
    // granula_pconsistency's initrand48, seeded.
    random_.generator.seed(settings_.seed);
    frame_ = 0;
    next_ = 0;
    bufferStart_ = 0;
}

void Brassage::schedule() noexcept
{
    // granulate: the draws and the fields at the grain's time.
    Grain g;
    g.start = next_;
    g.size = settings_.mode == Mode::Scramble ? fields_.grainsize : fixedGrain_;
    const auto ostep = outstep(fields_);
    // do_scatter.
    const auto scat = GRS_DEFAULT_SCATTER * random_.generator.next();
    g.place = g.start + static_cast<std::size_t>(std::lround(static_cast<double>(ostep) * scat));
    long rang = 0;
    if (hasRange())
    {
        // set_range.
        const auto val = std::min(fields_.range, static_cast<long>(g.start));
        rang = std::lround(static_cast<double>(val) * random_.generator.next());
    }
    g.read = g.start - static_cast<std::size_t>(rang);
    // CDP's input buffer: it reads on while the read point is past the
    // buffer's end, and reads a buffer from the read point (baktrak_granula)
    // when the range takes it back before the buffer's start.
    const auto buffer = bufferFrames_;
    if (g.read >= bufferStart_ + buffer)
        bufferStart_ += (g.read - bufferStart_) / buffer * buffer;
    g.offset = g.read < bufferStart_ ? 0L : static_cast<long>(g.read - bufferStart_);
    if (pitched())
        g.transposition = std::exp2(fields_.octaves);
    g.amp = static_cast<float>(fields_.amp);
    if (settings_.mode == Mode::Reverb)
    {
        // set_dvalue, RANGED: Space 0 to 1.
        g.position = std::fma(random_.generator.next(), 1.0, 0.0);
    }
    next_ = g.start + static_cast<std::size_t>(ostep);
    queue_[(queueFirst_ + queueCount_) % queue_.size()] = g;
    queueCount_++;
}

void Brassage::splice(long size) noexcept
{
    // do_multichan_splices: tables while the splice is under half the grain.
    const auto ch = grainChannels_;
    float* gb = grain_.data();
    const auto at = [gb, ch](long frame, std::size_t c) -> float& {
        return gb[static_cast<std::size_t>(frame) * ch + c];
    };
    if (splice_ < size / 2)
    {
        for (long n = 0; n < splice_; n++)
        {
            for (std::size_t c = 0; c < ch; c++)
                at(n, c) = static_cast<float>(at(n, c) * beginTable_[static_cast<std::size_t>(n)]);
        }
    }
    else
    {
        const auto k = std::min(splice_, size);
        if (k > 0)
        {
            const auto dif = 1.0 / static_cast<double>(k);
            auto lastval = dif;
            for (std::size_t c = 0; c < ch; c++)
                at(0, c) = 0.0F;
            for (std::size_t c = 0; c < ch; c++)
                at(1, c) = static_cast<float>(at(1, c) * lastval);
            for (long n = 2; n < k; n++)
            {
                const auto val = lastval + dif;
                lastval = val;
                for (std::size_t c = 0; c < ch; c++)
                    at(n, c) = static_cast<float>(at(n, c) * val);
            }
        }
    }
    if (splice_ > 0 && splice_ < size / 2)
    {
        const auto from = size - splice_;
        for (long n = 0; n < splice_; n++)
        {
            for (std::size_t c = 0; c < ch; c++)
                at(from + n, c) = static_cast<float>(at(from + n, c) * endTable_[static_cast<std::size_t>(n)]);
        }
    }
    else
    {
        const auto k = std::min(splice_, size);
        if (k > 0)
        {
            const auto dif = 1.0 / static_cast<double>(k);
            auto lastsum = dif;
            for (std::size_t c = 0; c < ch; c++)
                at(size - 1, c) = 0.0F;
            for (std::size_t c = 0; c < ch; c++)
                at(size - 2, c) = static_cast<float>(at(size - 2, c) * lastsum);
            auto frame = size - 2;
            for (long n = k - 3; n >= 0; n--)
            {
                const auto val = lastsum + dif;
                lastsum = val;
                frame--;
                for (std::size_t c = 0; c < ch; c++)
                    at(frame, c) = static_cast<float>(at(frame, c) * val);
            }
        }
    }
}

void Brassage::render(const Grain& g) noexcept
{
    const auto ch = grainChannels_;
    const auto size = static_cast<std::size_t>(g.size);
    float* gb = grain_.data();
    if (pitched())
    {
        // make_multichan_grain, interpolating (interp_gval, or with Reverb's
        // amp interp_gval_with_amp), fused as CDP8's build fuses them.
        const auto base = g.read - static_cast<std::size_t>(g.offset);
        auto flcnt = static_cast<double>(g.offset);
        auto iicnt = g.offset;
        auto frac = flcnt - static_cast<double>(iicnt);
        const bool withAmp = settings_.mode == Mode::Reverb;
        for (std::size_t n = 0; n < size; n++)
        {
            const auto frame = base + static_cast<std::size_t>(iicnt);
            for (std::size_t c = 0; c < ch; c++)
            {
                auto tthis = input(frame, c);
                auto next = input(frame + 1, c);
                if (withAmp)
                {
                    tthis = tthis * g.amp;
                    next = next * g.amp;
                }
                *gb++ = static_cast<float>(
                    std::fma(static_cast<double>(next - tthis), frac, static_cast<double>(tthis)));
            }
            flcnt += g.transposition;
            iicnt = static_cast<long>(flcnt);
            frac = flcnt - static_cast<double>(iicnt);
        }
    }
    else
    {
        for (std::size_t n = 0; n < size; n++)
        {
            for (std::size_t c = 0; c < ch; c++)
                *gb++ = input(g.read + n, c);
        }
    }
    splice(g.size);
    const auto och = outputChannels_;
    gb = grain_.data();
    if (settings_.mode == Mode::Reverb)
    {
        // write_stereo_grain.
        auto rpos = g.position;
        const auto adjust = dehole(rpos);
        const auto lpos = (1.0 - rpos) * adjust;
        rpos *= adjust;
        for (std::size_t n = 0; n < size; n++)
        {
            float* o = &out_[((g.place + n) & outMask_) * och];
            o[0] += static_cast<float>(static_cast<double>(gb[n]) * lpos);
            o[1] += static_cast<float>(static_cast<double>(gb[n]) * rpos);
        }
    }
    else
    {
        // write_grain.
        for (std::size_t n = 0; n < size; n++)
        {
            float* o = &out_[((g.place + n) & outMask_) * och];
            for (std::size_t c = 0; c < och; c++)
                o[c] += gb[n * och + c];
        }
    }
}

bool Brassage::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (!prepared() || input.size() % inputChannels_ != 0
        || output.size() != input.size() / inputChannels_ * outputChannels_
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    const auto ich = inputChannels_;
    const auto gch = grainChannels_;
    const auto och = outputChannels_;
    const auto frames = input.size() / ich;
    for (std::size_t n = 0; n < frames; n++)
    {
        const auto f = frame_;
        float* slot = &ring_[(f & ringMask_) * gch];
        if (gch == ich)
        {
            for (std::size_t c = 0; c < ich; c++)
                slot[c] = static_cast<float>(input[n * ich + c]);
        }
        else
        {
            // read_a_large_buf, BOTH_CHANNELS: the mix to mono.
            const auto left = static_cast<float>(input[n * ich]);
            const auto right = static_cast<float>(input[n * ich + 1]);
            slot[0] = (left + right) * 0.5F;
        }
        const auto taken = f + 1;
        while (next_ < taken)
            schedule();
        while (queueCount_ > 0 && queue_[queueFirst_].start + latency_ < taken)
        {
            render(queue_[queueFirst_]);
            queueFirst_ = (queueFirst_ + 1) % queue_.size();
            queueCount_--;
        }
        for (std::size_t c = 0; c < och; c++)
        {
            auto value = 0.0;
            if (f >= latency_)
            {
                auto& out = out_[((f - latency_) & outMask_) * och + c];
                value = static_cast<double>(out);
                out = 0.0F;
            }
            // CDP would scale its output buffer where this passes 1.
            output[n * och + c] = std::clamp(value, -1.0, 1.0);
        }
        frame_ = taken;
    }
    return true;
}
}
