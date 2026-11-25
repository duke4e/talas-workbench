/*
 * Derived from the CDP System, Release 8: dev/distort/distortm.c and
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
#include "talas/cdp/DistortMultiply.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace talas::cdp
{
namespace
{
// The ring holds the latency and the frame arriving.
constexpr std::size_t ringFrames = 4096;

[[nodiscard]] bool finite(std::span<const double> input) noexcept
{
    return std::ranges::all_of(input, [](double sample) { return std::isfinite(sample); });
}

// read_value_from_brktable's rounding of an integer field.
[[nodiscard]] std::optional<int> wholeFactor(double factor, int minimum, int maximum) noexcept
{
    if (!std::isfinite(factor))
        return std::nullopt;
    const auto rounded = std::round(factor);
    if (rounded < minimum || rounded > maximum)
        return std::nullopt;
    return static_cast<int>(rounded);
}
}

bool DistortMultiply::prepare(int factor, std::size_t bufferFrames)
{
    if (factor < minimumFactor || factor > maximumFactor || bufferFrames < 2 * latencyFrames + 2)
        return false;
    factor_ = factor;
    scanner_.prepare(bufferFrames);
    ring_.assign(ringFrames, 0.0F);
    factors_.assign(ringFrames, static_cast<std::uint8_t>(factor));
    mask_ = ringFrames - 1;
    in_.assign(latencyFrames + 1, 0.0F);
    out_.assign(latencyFrames + 1, 0.0F);
    reset();
    return true;
}

bool DistortMultiply::setFactor(double factor) noexcept
{
    const auto whole = wholeFactor(factor, minimumFactor, maximumFactor);
    if (ring_.empty() || !whole.has_value())
        return false;
    nextFactor_ = *whole;
    return true;
}

void DistortMultiply::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    std::ranges::fill(factors_, static_cast<std::uint8_t>(factor_));
    nextFactor_ = factor_;
    frame_ = 0;
    outputPhase_ = 1;
    scanner_.reset();
}

bool DistortMultiply::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (ring_.empty() || input.size() != output.size() || !finite(input))
        return false;
    const auto multiplyHalfCycle = [this](const half_cycles::HalfCycle& halfCycle) {
        multiply(halfCycle);
        return true;
    };
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto k = frame_;
        const auto b = static_cast<float>(input[index]);
        at(k) = b;
        factors_[k & mask_] = static_cast<std::uint8_t>(nextFactor_);
        // The stream waits no longer for a half-cycle to end.
        if (k - scanner_.lastzero() >= latencyFrames)
            scanner_.cut(k, multiplyHalfCycle);
        // Past an odd buffer CDP reads the zeroed output buffer.
        scanner_.examine(k, b, 0.0F, multiplyHalfCycle);
        output[index] = k >= latencyFrames ? static_cast<double>(at(k - latencyFrames)) : 0.0;
        ++frame_;
    }
    return true;
}

// set_phaseswitch and do_multiply, statement for statement;
// do_cros_multiply gives the same samples across two buffers.
void DistortMultiply::multiply(const half_cycles::HalfCycle& halfCycle) noexcept
{
    const auto lastzero = halfCycle.start;
    const auto half_cyclelen = static_cast<int>(halfCycle.end - lastzero);
    if (half_cyclelen <= 0)
        return;
    for (int n = 0; n < half_cyclelen; n++)
        in_[static_cast<std::size_t>(n)] = at(lastzero + static_cast<std::size_t>(n));
    // do_md reads the factor where the half-cycle's marker is.
    const int factor = factors_[halfCycle.marker & mask_];
    const auto marked = at(halfCycle.marker);
    int phaseswitch = 0;
    if ((factor & 1) != 0)
        phaseswitch = marked > 0.0F ? outputPhase_ : -outputPhase_;
    else
        phaseswitch = marked > 0.0F ? 1 : -1;
    const auto* b1 = in_.data();
    auto* ob1 = out_.data();
    const int mid_cyclelen = half_cyclelen / factor;
    if (mid_cyclelen == 0)
    {
        // Just copied, more cycles asked for than there are samples.
        for (int n = 0; n < half_cyclelen; n++)
            ob1[n] = phaseswitch == 1 ? b1[n] : -b1[n];
    }
    else
    {
        int remnant = half_cyclelen - (mid_cyclelen * factor);
        int mid_cycle = mid_cyclelen;
        for (int n = 0, m = 0; n < mid_cycle; n++, m++)
        {
            const auto index = static_cast<int>(
                std::lround(static_cast<double>(m) * static_cast<double>(factor)));
            ob1[n] = phaseswitch == 1 ? b1[index] : -b1[index];
        }
        if (remnant != 0)
        {
            ob1[mid_cycle] = ob1[mid_cycle - 1] / 2;
            mid_cycle++;
            remnant--;
        }
        for (int k = 1; k < factor; k++)
        {
            const int start = mid_cycle;
            mid_cycle += mid_cyclelen;
            int n = start;
            if ((k & 1) != 0)
            {
                for (int m = 0; n < mid_cycle; n++, m++)
                    ob1[n] = -ob1[m];
            }
            else
            {
                for (int m = 0; n < mid_cycle; n++, m++)
                    ob1[n] = ob1[m];
            }
            if (remnant != 0)
            {
                ob1[mid_cycle] = ob1[n - 1] * 0.5F;
                mid_cycle++;
                remnant--;
            }
        }
        if ((factor & 1) != 0)
            outputPhase_ = -outputPhase_;
    }
    for (int n = 0; n < half_cyclelen; n++)
        at(lastzero + static_cast<std::size_t>(n)) = ob1[n];
}

bool DistortDivide::prepare(int factor, bool interpolate, std::size_t bufferFrames)
{
    if (factor < minimumFactor || factor > maximumFactor || bufferFrames < 2 * latencyFrames + 2)
        return false;
    factor_ = factor;
    interpolate_ = interpolate;
    scanner_.prepare(bufferFrames);
    ring_.assign(ringFrames, 0.0F);
    factors_.assign(ringFrames, static_cast<std::uint8_t>(factor));
    mask_ = ringFrames - 1;
    in_.assign(latencyFrames + 2, 0.0F);
    reset();
    return true;
}

bool DistortDivide::setFactor(double factor) noexcept
{
    const auto whole = wholeFactor(factor, minimumFactor, maximumFactor);
    if (ring_.empty() || !whole.has_value())
        return false;
    nextFactor_ = *whole;
    return true;
}

void DistortDivide::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    std::ranges::fill(factors_, static_cast<std::uint8_t>(factor_));
    nextFactor_ = factor_;
    groupFactor_ = factor_;
    frame_ = 0;
    outputPhase_ = 1;
    halfCycles_ = 0;
    cutting_ = false;
    startIndex_ = 0;
    startMarker_ = 0;
    endIndex_ = 0;
    scanner_.reset();
}

bool DistortDivide::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (ring_.empty() || input.size() != output.size() || !finite(input))
        return false;
    const auto add = [this](const half_cycles::HalfCycle& halfCycle) {
        return addHalfCycle(halfCycle);
    };
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto k = frame_;
        const auto b = static_cast<float>(input[index]);
        at(k) = b;
        factors_[k & mask_] = static_cast<std::uint8_t>(nextFactor_);
        // The stream waits no longer for a group to end.
        const auto groupStart = halfCycles_ > 0 ? startIndex_ : scanner_.lastzero();
        if (k - groupStart >= latencyFrames)
        {
            cutting_ = true;
            scanner_.cut(k, add);
            cutting_ = false;
        }
        // Past an odd buffer CDP reads the zeroed output buffer.
        scanner_.examine(k, b, 0.0F, add);
        output[index] = k >= latencyFrames ? static_cast<double>(at(k - latencyFrames)) : 0.0;
        ++frame_;
    }
    return true;
}

// do_the_division: the group's first half-cycle is its source; at the
// Factor-th the group is divided.
bool DistortDivide::addHalfCycle(const half_cycles::HalfCycle& halfCycle) noexcept
{
    halfCycles_++;
    if (halfCycles_ == 1)
    {
        startIndex_ = halfCycle.start;
        startMarker_ = halfCycle.marker;
        endIndex_ = halfCycle.end;
        // do_md reads the factor where the group's first marker is.
        groupFactor_ = factors_[startMarker_ & mask_];
    }
    if (halfCycles_ < groupFactor_ && !cutting_)
        return false;
    divide(halfCycle.end);
    halfCycles_ = 0;
    return true;
}

// set_ratio, get_iphase and do_divide or do_idivide with getval or
// interpval; their forms across two buffers read the same samples.
void DistortDivide::divide(std::size_t end) noexcept
{
    const auto startindex = startIndex_;
    const auto goal = static_cast<int>(end - startindex);
    const auto source = static_cast<int>(endIndex_ - startindex);
    if (goal <= 0)
        return;
    for (int n = 0; n <= goal && n <= static_cast<int>(in_.size()) - 1; n++)
        in_[static_cast<std::size_t>(n)] = at(startindex + static_cast<std::size_t>(n));
    const double ratio = static_cast<double>(source) / static_cast<double>(goal);
    const auto input_phase = (at(startMarker_) > 0 ? 1 : -1) * outputPhase_;
    const auto* bbuf = in_.data();
    for (int m = 0; m < goal; m++)
    {
        float value = 0.0F;
        if (interpolate_)
        {
            const double findex = static_cast<double>(m) * ratio;
            const int index = static_cast<int>(findex);
            const double frac = findex - static_cast<double>(index);
            const float ba = bbuf[index];
            const float bb = bbuf[index + 1];
            const auto diff = static_cast<float>(static_cast<double>(bb - ba) * frac);
            value = ba + diff;
        }
        else
        {
            const auto index = static_cast<int>(std::lround(static_cast<double>(m) * ratio));
            value = bbuf[index];
        }
        at(startindex + static_cast<std::size_t>(m)) = input_phase == 1 ? value : -value;
    }
    outputPhase_ = -outputPhase_;
}
}
