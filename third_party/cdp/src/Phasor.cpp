/*
 * Derived from the CDP System, Release 8: dev/standnew/phasor.c and
 * dev/cdp2k/readdata.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/Phasor.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace talas::cdp
{
namespace
{
// globcon.h's FLTERR, MS_TO_SECS and SEMITONES_PER_OCTAVE; tkglobals.h's
// F_SECSIZE; phasor.c's WRAP and ROOT2.
constexpr double FLTERR = 0.000002;
constexpr double MS_TO_SECS = 0.001;
constexpr double SEMITONES_PER_OCTAVE = 12.0;
constexpr std::size_t F_SECSIZE = 256;
constexpr std::size_t WRAP = 16;
constexpr double ROOT2 = 1.4142136;

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

[[nodiscard]] std::size_t roundUp(std::size_t value, std::size_t step) noexcept
{
    return (value + step - 1) / step * step;
}

// phasor.c's pancalc between the speakers, as CDP8's build fuses it.
void pancalc(double position, double& leftgain, double& rightgain) noexcept
{
    const auto relpos = position < 0.0 ? -position : position;
    auto temp = std::fma(relpos, relpos, 1.0);
    const auto reldist = ROOT2 / std::sqrt(temp);
    temp = (position + 1.0) / 2.0;
    rightgain = temp * reldist;
    leftgain = (1.0 - temp) * reldist;
}
}

std::size_t Phasor::packetFrames(double sampleRate, double frequency) noexcept
{
    // calculate_phase_shifts: (int)floor(srate / frq), then up to a
    // multiple of 4.
    const auto frames = std::floor(sampleRate / frequency);
    if (!(frames >= 0.0) || frames > static_cast<double>(maximumPacketFrames))
        return maximumPacketFrames + 1;
    return roundUp(static_cast<std::size_t>(frames), 4);
}

bool Phasor::valid(double& frequency, double& shift) const noexcept
{
    return inRange(frequency, minimumFrequency, maximumFrequency) && inRange(shift, minimumShift, maximumShift);
}

bool Phasor::prepare(double sampleRate, int streams, double frequency, double shift, int outputChannels,
    double offsetMs, bool surround, double lowestFrequency)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || streams < minimumStreams || streams > maximumStreams
        || outputChannels < 1 || outputChannels > streams || (surround && outputChannels <= 2)
        || !valid(frequency, shift) || !inRange(offsetMs, 0.0, maximumOffsetMs))
    {
        return false;
    }
    auto lowest = frequency;
    if (!std::isnan(lowestFrequency))
    {
        auto unused = 0.0;
        if (!valid(lowestFrequency, unused))
            return false;
        lowest = std::min(lowestFrequency, frequency);
    }
    const auto longest = packetFrames(sampleRate, lowest);
    if (longest > maximumPacketFrames || longest < 4)
        return false;
    // create_phaseshift_and_outchan_data_arrays and create_phasor_sndbufs:
    // CDP's input buffer holds ceil(srate / frq) frames for its lowest
    // Phasfrq, up to a multiple of 4, plus WRAP, up to whole sectors.
    const auto ceiling = static_cast<std::size_t>(std::ceil(sampleRate / lowest));
    const auto bufferFrames = roundUp(roundUp(ceiling, 4) + WRAP, F_SECSIZE);

    const auto channels = static_cast<std::size_t>(outputChannels);
    const auto count = static_cast<std::size_t>(streams);
    // calculate_streams: the streams' time offsets.
    const auto thisoffset = static_cast<std::size_t>(std::lround(offsetMs * MS_TO_SECS * sampleRate));
    std::array<std::size_t, maximumStreams> offsets{};
    std::size_t k = 1;
    for (; k < count - 1; k++)
    {
        offsets[k] = static_cast<std::size_t>(std::lround(
            (static_cast<double>(k) / static_cast<double>(count - 1)) * static_cast<double>(thisoffset)));
    }
    offsets[k] = thisoffset;
    // create_phaseshift_and_outchan_data_arrays: each stream's channels.
    std::array<Placement, maximumStreams> placements{};
    for (std::size_t n = 0; n < count; n++)
    {
        auto& p = placements[n];
        if (count == channels)
        {
            p = {n, 1.0, (n + 1) % channels, 0.0};
            continue;
        }
        const auto chanpos = surround
            ? (static_cast<double>(n) / static_cast<double>(count)) * static_cast<double>(channels)
            : static_cast<double>(n) / static_cast<double>(count - 1) * static_cast<double>(channels - 1);
        const auto chan = std::floor(chanpos);
        auto pos = chanpos - chan;
        pos = std::fma(pos, 2.0, -1.0);
        auto leftgain = 0.0;
        auto rightgain = 0.0;
        pancalc(pos, leftgain, rightgain);
        const auto c = static_cast<std::size_t>(chan);
        p = {c, leftgain, (c + 1) % channels, rightgain};
    }

    // create_phasor_sndbufs: the output buffer's offset extension.
    std::size_t offsetWrap = 0;
    if (offsetMs > 0.0)
    {
        const auto maxoffset = static_cast<std::size_t>(std::ceil(offsetMs * MS_TO_SECS * sampleRate));
        offsetWrap = roundUp(maxoffset * channels + WRAP, F_SECSIZE);
    }

    const auto latency = longest + 1;
    const auto ringFrames = std::bit_ceil(bufferFrames + 2);
    const auto outFrames = std::bit_ceil(latency + thisoffset + 1);
    ring_.assign(ringFrames, 0.0F);
    ringMask_ = ringFrames - 1;
    out_.assign(outFrames * channels, 0.0F);
    outMask_ = outFrames - 1;
    increments_.assign((count - 1) * longest, 0.0);
    packetBuffer_.assign((longest + thisoffset) * channels, 0.0F);

    sampleRate_ = sampleRate;
    streams_ = count;
    channels_ = channels;
    lowest_ = lowest;
    startFrequency_ = frequency;
    startShift_ = shift;
    latency_ = latency;
    bufferFrames_ = bufferFrames;
    outputBufferSamples_ = bufferFrames * channels;
    offsetWrapSamples_ = offsetWrap;
    offsets_ = offsets;
    maximumOffset_ = thisoffset;
    placements_ = placements;
    reset();
    return true;
}

bool Phasor::setControls(double frequency, double shift) noexcept
{
    if (channels_ == 0 || !valid(frequency, shift) || frequency < lowest_)
        return false;
    frequency_ = frequency;
    shift_ = shift;
    return true;
}

void Phasor::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    std::ranges::fill(out_, 0.0F);
    frequency_ = startFrequency_;
    shift_ = startShift_;
    frame_ = 0;
    nextStart_ = 0;
    pending_ = {};
    pendingFirst_ = 0;
    pendingCount_ = 0;
    base_ = 0;
    bufferPosition_ = 0;
    outputBase_ = 0;
}

void Phasor::computePacket(const Packet& packet) noexcept
{
    const auto P = packet.frames;
    const auto Pmax = latency_ - 1;
    const auto ch = channels_;
    // calculate_streams: CDP reloads its input buffer from the packet's
    // start only when the packet would pass its end.
    if (bufferPosition_ + P + 1 >= bufferFrames_)
    {
        base_ += bufferPosition_;
        bufferPosition_ = 0;
    }
    const auto ibuf = [this](std::size_t i) noexcept {
        return input(base_ + i);
    };

    // calculate_phase_shifts.
    const auto maxphaseshift = packet.shift;
    const auto minphaseshift = maxphaseshift / static_cast<double>(streams_ - 1);
    const auto midway = P / 2;
    const auto quartway = midway / 2;
    for (std::size_t n = 0; n + 1 < streams_; n++)
    {
        auto* sampincr = increments_.data() + n * Pmax;
        const auto product = static_cast<double>(n + 1) * minphaseshift;
        auto thismax = maxphaseshift > product ? product : maxphaseshift;
        thismax = thismax / SEMITONES_PER_OCTAVE;
        const auto posoffset = thismax / static_cast<double>(quartway);
        for (std::size_t k = 0; k < quartway; k++)
        {
            const auto value = static_cast<double>(k + 1) * posoffset;
            sampincr[k] = value > thismax ? thismax : value;
        }
        for (std::size_t k = 0, j = midway - 2; k + 1 < quartway; k++, j--)
            sampincr[j] = sampincr[k];
        sampincr[midway - 1] = 0.0;
        for (std::size_t k = 0, j = midway; k < midway; k++, j++)
            sampincr[j] = -sampincr[k];
        // pow(2.0, x), which CDP8's build calls as exp2.
        for (std::size_t k = 0; k < P; k++)
            sampincr[k] = std::exp2(sampincr[k]);
        auto sum = 0.0;
        for (std::size_t k = 0; k < P; k++)
            sum += sampincr[k];
        const auto error = (sum - static_cast<double>(P)) / static_cast<double>(P);
        for (std::size_t k = 0; k < P; k++)
            sampincr[k] -= error;
    }

    // calculate_streams: the packet buffer.
    auto* pbuf = packetBuffer_.data();
    const auto used = (P + maximumOffset_) * ch;
    std::fill(pbuf, pbuf + used, 0.0F);
    for (std::size_t n = 0; n < P; n++)
        pbuf[n * ch] = pbuf[n * ch] + ibuf(bufferPosition_ + n);
    const auto lastRead = static_cast<long>(P);
    for (std::size_t k = 1; k < streams_; k++)
    {
        const auto* sampincr = increments_.data() + (k - 1) * Pmax;
        const auto& place = placements_[k];
        auto pbufpos1 = offsets_[k] * ch + place.first;
        auto pbufpos2 = offsets_[k] * ch + place.second;
        auto ibufposd = static_cast<double>(bufferPosition_);
        for (std::size_t n = 0; n < P; n++)
        {
            ibufposd += sampincr[n];
            auto lopos = std::lround(ibufposd);
            if (lopos >= lastRead)
                lopos = lastRead;
            const auto at = static_cast<std::size_t>(lopos);
            const double loval = ibuf(at);
            const auto valdiff = static_cast<double>(ibuf(at + 1)) - loval;
            const auto timefrac = ibufposd - static_cast<double>(lopos);
            const auto val = std::fma(valdiff, timefrac, loval);
            const auto val1 = val * place.firstLevel;
            pbuf[pbufpos1] = static_cast<float>(static_cast<double>(pbuf[pbufpos1]) + val1);
            const auto val2 = val * place.secondLevel;
            pbuf[pbufpos2] = static_cast<float>(static_cast<double>(pbuf[pbufpos2]) + val2);
            pbufpos1 += ch;
            pbufpos2 += ch;
        }
    }
    bufferPosition_ += P;

    // The packet into the output, from its start.
    for (std::size_t i = 0; i < used; i++)
    {
        auto& slot = out_[((packet.start + i / ch) & outMask_) * ch + i % ch];
        slot = slot + pbuf[i];
    }
    // CDP moves its output buffer on once the next packet starts past its
    // first part; moving the offset extension down over itself, it clears
    // the samples from two parts on.
    const auto buflen = outputBufferSamples_;
    if ((packet.start + P) * ch - outputBase_ >= buflen)
    {
        outputBase_ += buflen;
        if (offsetWrapSamples_ > buflen)
        {
            const auto end = std::min(outputBase_ + buflen + offsetWrapSamples_, (packet.start * ch) + used);
            for (auto s = outputBase_ + 2 * buflen; s < end; s++)
                out_[((s / ch) & outMask_) * ch + s % ch] = 0.0F;
        }
    }
}

bool Phasor::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (channels_ == 0 || output.size() != input.size() * channels_
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    const auto ch = channels_;
    for (std::size_t n = 0; n < input.size(); n++)
    {
        const auto f = frame_;
        if (f == nextStart_)
        {
            // CDP reads its breakpoint files as each packet starts.
            const auto P = packetFrames(sampleRate_, frequency_);
            pending_[(pendingFirst_ + pendingCount_) % pending_.size()] = {f, P, shift_};
            pendingCount_++;
            nextStart_ += P;
        }
        ring_[f & ringMask_] = static_cast<float>(input[n]);
        if (pendingCount_ > 0)
        {
            const auto& packet = pending_[pendingFirst_];
            if (packet.start + packet.frames + 1 == f)
            {
                computePacket(packet);
                pendingFirst_ = (pendingFirst_ + 1) % pending_.size();
                pendingCount_--;
            }
        }
        for (std::size_t c = 0; c < ch; c++)
        {
            auto value = 0.0;
            if (f >= latency_)
            {
                auto& slot = out_[((f - latency_) & outMask_) * ch + c];
                value = static_cast<double>(slot);
                slot = 0.0F;
            }
            // CDP would scale the whole output where this passes 0.95.
            output[n * ch + c] = std::clamp(value, -1.0, 1.0);
        }
        frame_++;
    }
    return true;
}
}
