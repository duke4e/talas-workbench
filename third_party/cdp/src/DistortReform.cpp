/*
 * Derived from the CDP System, Release 8: dev/distort/distort.c and
 * distortion.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/DistortReform.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace talas::cdp
{
namespace
{
// distcon.h's FCLIPMAX, SINETABLEN, PULSEWIDTH and MAXWIDTH, and globcon.h's
// PI.
constexpr float fclipmax = 0.7325F;
constexpr double sineTableLength = 1024.0;
constexpr double pulseWidth = 0.001;
constexpr double maxWidth = 6.0;
constexpr double pi = 3.141592654;
// The ring holds the latency and the frame arriving.
constexpr std::size_t ringFrames = 4096;

// triangle, statement for statement; end_triangle gives the same samples
// across two buffers.
void triangle(float* b, int lastzero, float cyclemax, int endcycle) noexcept
{
    double realval = 0.0;
    double step = 0.0;
    const int start = lastzero + 1;
    const int half_cyclecnt = endcycle - lastzero;
    const int halfcnt = (half_cyclecnt + 1) / 2;
    const int middle = lastzero + halfcnt;
    b[lastzero] = 0;
    step = static_cast<double>(cyclemax) / static_cast<double>(halfcnt);
    for (int j = start; j < middle; j++)
    {
        realval += step;
        b[j] = static_cast<float>(realval);
    }
    if ((half_cyclecnt & 1) == 0)
        realval += step;
    b[middle] = static_cast<float>(realval);
    for (int j = middle + 1; j < endcycle; j++)
    {
        realval -= step;
        b[j] = static_cast<float>(realval);
    }
}

// do_exagg2, statement for statement.
void doExagg2(int phase, int lastpos, int thispos, double powfac, float* b) noexcept
{
    const auto first = std::fabs(static_cast<double>(b[lastpos]));
    const auto last = std::fabs(static_cast<double>(b[thispos]));
    const auto valdiff = first > last ? first : last;
    for (int n = lastpos; n <= thispos; n++)
    {
        auto val = static_cast<double>(b[n]) / valdiff;
        val *= static_cast<float>(phase);
        val = std::pow(val, powfac);
        val *= valdiff * static_cast<float>(phase);
        b[n] = static_cast<float>(val);
    }
}

// do_exagg, statement for statement.
void doExagg(double lastmax, int lastpos, double thismax, int thispos, double powfac,
    float* b) noexcept
{
    const double maxdif = thismax - lastmax;
    const double posdif = thispos - lastpos;
    for (int n = lastpos + 1, m = 1; n < thispos; n++, m++)
    {
        const double local_max = lastmax + (maxdif * static_cast<double>(m) / posdif);
        auto rati_o = std::min((static_cast<double>(b[n]) / local_max), 1.0);
        rati_o = std::pow(rati_o, powfac);
        b[n] = static_cast<float>(static_cast<double>(b[n]) * rati_o);
    }
}
}

std::size_t DistortReform::defaultBufferFrames(Mode mode) noexcept
{
    return half_cycles::defaultBufferFrames(mode == Mode::Exaggerate ? 3 : 2);
}

bool DistortReform::prepare(
    double sampleRate, Mode mode, double exaggeration, std::size_t bufferFrames)
{
    if (bufferFrames == 0)
        bufferFrames = defaultBufferFrames(mode);
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || pulseWidth * sampleRate > static_cast<double>(INT_MAX)
        || !std::isfinite(exaggeration) || exaggeration < minimumExaggeration
        || exaggeration > maximumExaggeration
        || mode < Mode::FixedSquare || mode > Mode::Exaggerate
        || bufferFrames < 2 * latencyFrames + 2)
    {
        return false;
    }
    mode_ = mode;
    startPowfac_ = exaggeration;
    // A change a block, for the blocks of 64 frames and more that the
    // latency holds.
    exaggerations_.assign(mode == Mode::Exaggerate ? latencyFrames / 64 + 3 : 0, Exaggeration{});
    // distort_preprocess and gen_distort_sin_table.
    pulse_ = static_cast<int>(std::lround(pulseWidth * sampleRate));
    sine_.assign(static_cast<std::size_t>(sineTableLength), 0.0);
    for (std::size_t n = 0; n < sine_.size(); n++)
        sine_[n] = std::sin(pi * static_cast<double>(n) / sineTableLength);
    bufferFrames_ = bufferFrames;
    scanner_.prepare(bufferFrames);
    ring_.assign(ringFrames, 0.0F);
    mask_ = ringFrames - 1;
    cycle_.assign(latencyFrames + 2, 0.0F);
    reset();
    return true;
}

bool DistortReform::setExaggeration(double exaggeration) noexcept
{
    if (ring_.empty() || mode_ != Mode::Exaggerate || !std::isfinite(exaggeration)
        || exaggeration < minimumExaggeration || exaggeration > maximumExaggeration)
    {
        return false;
    }
    nextPowfac_ = exaggeration;
    return true;
}

double DistortReform::exaggerationAt(double frame) const noexcept
{
    if (exaggerationCount_ == 0)
        return startPowfac_;
    // The last change at or before the frame; past the oldest change kept,
    // the oldest.
    const auto kept = std::min(exaggerationCount_, exaggerations_.size());
    const auto oldest = exaggerationCount_ - kept;
    auto value = exaggerations_[oldest % exaggerations_.size()].value;
    for (auto n = oldest + 1; n < exaggerationCount_; ++n)
    {
        const auto& change = exaggerations_[n % exaggerations_.size()];
        if (static_cast<double>(change.frame) > frame)
            break;
        value = change.value;
    }
    return value;
}

void DistortReform::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    powfac_ = startPowfac_;
    nextPowfac_ = startPowfac_;
    exaggerationCount_ = 0;
    frame_ = 0;
    scanner_.reset();
    crossingFirst_ = 0.0F;
    random_.seed(1);
}

bool DistortReform::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (ring_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    const auto reshapeHalfCycle = [this](const half_cycles::HalfCycle& halfCycle) {
        reshape(halfCycle);
        return true;
    };
    // The exaggeration from this call's first frame.
    if (!exaggerations_.empty()
        && (exaggerationCount_ == 0
            || exaggerations_[(exaggerationCount_ - 1) % exaggerations_.size()].value != nextPowfac_))
    {
        exaggerations_[exaggerationCount_ % exaggerations_.size()] = Exaggeration{frame_, nextPowfac_};
        ++exaggerationCount_;
    }
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto k = frame_;
        const auto b = static_cast<float>(input[index]);
        at(k) = b;
        // The stream waits no longer for a half-cycle to end.
        if (k - scanner_.lastzero() >= latencyFrames)
            scanner_.cut(k, reshapeHalfCycle);
        // Past an odd buffer, CDP reads Exaggerate's work buffer, which holds
        // the last half-cycle it reshaped across buffers, and memory past its
        // buffers for the other modes, read as zero.
        scanner_.examine(k, b, mode_ == Mode::Exaggerate ? crossingFirst_ : 0.0F,
            reshapeHalfCycle);
        output[index] = k >= latencyFrames
            ? static_cast<double>(at(k - latencyFrames))
            : 0.0;
        ++frame_;
    }
    return true;
}

void DistortReform::reshape(const half_cycles::HalfCycle& halfCycle) noexcept
{
    const auto start = halfCycle.start;
    const auto boundary = halfCycle.boundary;
    const auto phase = halfCycle.phase;
    const auto cyclemax = halfCycle.cyclemax;
    const auto length = static_cast<int>(halfCycle.end - start);
    if (length <= 0)
        return;
    // The half-cycle and the frame that ended it, in a straight buffer.
    float* b = cycle_.data();
    for (int j = 0; j <= length; j++)
        b[j] = at(start + static_cast<std::size_t>(j));
    const auto i = length;
    switch (mode_)
    {
    case Mode::FixedSquare:
    {
        const auto clipmax = fclipmax * static_cast<float>(phase);
        for (int j = 0; j < i; j++)
            b[j] = clipmax;
        break;
    }
    case Mode::Square:
        for (int j = 0; j < i; j++)
            b[j] = cyclemax;
        break;
    case Mode::FixedTriangle:
        if (i > 3)
            triangle(b, 0, fclipmax * static_cast<float>(phase), i);
        break;
    case Mode::Triangle:
        if (i > 3)
            triangle(b, 0, cyclemax, i);
        break;
    case Mode::Invert:
        for (int j = 0; j < i; j++)
            b[j] = cyclemax - b[j];
        break;
    case Mode::Click:
        if (boundary == 0)
        {
            const auto step = std::min(pulse_, i);
            genpulse(step, cyclemax, b, b + i);
        }
        else
        {
            // distort_func_crosbuf starts the clicks again in the second
            // buffer, and fills the first with them when they would end
            // there.
            const auto end_bit = static_cast<int>(boundary - start);
            const auto step = std::min(pulse_, i);
            if (step > end_bit)
            {
                genpulse(end_bit, cyclemax, b, b + end_bit);
                genpulse(step - end_bit, cyclemax, b + end_bit, b + i);
            }
            else
            {
                genpulse(end_bit, cyclemax, b, b + end_bit);
                genpulse(0, cyclemax, b + end_bit, b + i);
            }
        }
        break;
    case Mode::Sine:
        if (i > 5)
        {
            const double index_factor = sineTableLength / static_cast<double>(i);
            for (int j = 0; j < i; j++)
            {
                b[j] = static_cast<float>(
                    sine_[static_cast<std::size_t>(std::lround(j * index_factor))]
                    * cyclemax);
            }
        }
        break;
    case Mode::Exaggerate:
        // get_powfac's reading time, the half-cycle's midpoint.
        powfac_ = exaggerationAt((static_cast<double>(start) + static_cast<double>(halfCycle.end)) / 2.0);
        // A half-cycle of zeros alone, which only CDP's buffer ends and the
        // stream's cut make, has no contour; CDP's scan would run on past
        // it.
        if (cyclemax != 0.0F)
        {
            exaggerate(0, i, phase, b);
            for (int j = 0; j < i; j++)
            {
                if (!std::isfinite(b[j]))
                    b[j] = 0.0F;
            }
        }
        if (boundary != 0)
            crossingFirst_ = b[0];
        break;
    }
    for (int j = 0; j < i; j++)
        at(start + static_cast<std::size_t>(j)) = b[j];
}

// genpulse, statement for statement.
void DistortReform::genpulse(int step, float maxamp, float* bufptr, float* bufend) noexcept
{
    int sum = 0;
    float ampval = maxamp;
    while (sum < step)
    {
        int n = static_cast<int>(std::lround(random_.next() * maxWidth));
        n++;
        sum += n;
        const int z = std::min(n, step - sum);
        for (int m = 0; m < z; m++)
            *bufptr++ = ampval;
        ampval = -ampval;
    }
    while (bufptr < bufend)
        *bufptr++ = 0;
}

// exaggerate, statement for statement, its scans held to the half-cycle
// (where its end stops them anyway).
void DistortReform::exaggerate(int lastzero, int thiszero, int phase, float* b) noexcept
{
    const auto powfac = powfac_;
    int n = lastzero + 1;
    double lastmax = 0.0;
    double thismax = 0.0;
    int lastpos = 0;
    int thispos = 0;
    int OK = 1;
    if (phase == 1)
    {
        while (n < thiszero && b[n] >= b[n - 1])
            n++;
        lastmax = static_cast<double>(b[n - 1]);
        lastpos = n - 1;
        doExagg2(phase, lastzero, lastpos, powfac, b);
        while (n < thiszero)
        {
            while (b[n] < b[n - 1])
            {
                if (++n >= thiszero)
                {
                    OK = 0;
                    break;
                }
            }
            if (OK)
            {
                while (n < thiszero && b[n] >= b[n - 1])
                    n++;
                thismax = static_cast<double>(b[n - 1]);
                thispos = n - 1;
                doExagg(lastmax, lastpos, thismax, thispos, powfac, b);
                lastmax = thismax;
                lastpos = thispos;
            }
            else
                break;
        }
        doExagg2(phase, lastpos, n - 1, powfac, b);
    }
    else
    {
        while (n < thiszero && b[n] <= b[n - 1])
            n++;
        lastmax = static_cast<double>(b[n - 1]);
        lastpos = n - 1;
        doExagg2(phase, lastzero, lastpos, powfac, b);
        while (n < thiszero)
        {
            while (b[n] > b[n - 1])
            {
                if (++n >= thiszero)
                {
                    OK = 0;
                    break;
                }
            }
            if (OK)
            {
                while (n < thiszero && b[n] <= b[n - 1])
                    n++;
                thismax = static_cast<double>(b[n - 1]);
                thispos = n - 1;
                doExagg(lastmax, lastpos, thismax, thispos, powfac, b);
                lastmax = thismax;
                lastpos = thispos;
            }
            else
                break;
        }
        doExagg2(phase, lastpos, n - 1, powfac, b);
    }
}
}
