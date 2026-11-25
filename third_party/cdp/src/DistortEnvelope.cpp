/*
 * Derived from the CDP System, Release 8: dev/distort/distorte.c,
 * disprepro.c, ap_distort.c and distortion.c, and dev/cdp2k/tklib1.c
 * Copyright (c) 1983-2013 Trevor Wishart and Composers Desktop Project Ltd
 * Modifications copyright (c) 2026 the Talas authors
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; either version 2.1 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see the GNU
 * Lesser General Public License in third_party/cdp/LICENSE.
 */
#include "talas/cdp/DistortEnvelope.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace talas::cdp
{
namespace
{
// The ring holds the latency and the frame arriving.
constexpr std::size_t ringFrames = 4096;
// globcon.h's FLTERR.
constexpr double fltErr = 0.000002;

[[nodiscard]] bool flteq(double f1, double f2) noexcept
{
    const auto upperbnd = f2 + fltErr;
    const auto lowerbnd = f2 - fltErr;
    return !((f1 > upperbnd) || (f1 < lowerbnd));
}

[[nodiscard]] bool space(char character) noexcept
{
    return character == ' ' || character == '\t' || character == '\n'
        || character == '\r' || character == '\v' || character == '\f';
}

[[nodiscard]] bool digit(char character) noexcept
{
    return character >= '0' && character <= '9';
}

// get_float_from_within_string: a number of digits and one point, perhaps
// after a minus sign; anything else ends the line.
[[nodiscard]] std::optional<double> nextNumber(std::string_view line, std::size_t& position)
{
    auto p = position;
    while (p < line.size() && space(line[p]))
        ++p;
    if (p >= line.size())
        return std::nullopt;
    const auto valstart = p;
    int decimal_point_cnt = 0;
    bool has_digits = false;
    switch (line[p])
    {
    case '-':
        break;
    case '.':
        decimal_point_cnt = 1;
        break;
    default:
        if (!digit(line[p]))
            return std::nullopt;
        has_digits = true;
        break;
    }
    ++p;
    while (p < line.size() && !space(line[p]))
    {
        if (digit(line[p]))
            has_digits = true;
        else if (line[p] == '.')
        {
            if (++decimal_point_cnt > 1)
                return std::nullopt;
        }
        else
            return std::nullopt;
        ++p;
    }
    if (!has_digits)
        return std::nullopt;
    const std::string token{line.substr(valstart, p - valstart)};
    char* end = nullptr;
    const auto value = std::strtod(token.c_str(), &end);
    if (end == token.c_str())
        return std::nullopt;
    position = p;
    return value;
}
}

std::optional<std::vector<double>> readDistortEnvelope(std::string_view text)
{
    std::vector<double> env;
    std::size_t begin = 0;
    while (begin < text.size())
    {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline + 1;
        const auto line = text.substr(begin, end - begin);
        begin = end;
        std::size_t position = 0;
        while (const auto value = nextNumber(line, position))
        {
            const auto cnt = env.size();
            if (cnt == 0)
            {
                // CDP forces a first time near zero to zero; a later first
                // time would take an unset value for time zero.
                if (*value < 0.0 || !flteq(*value, 0.0))
                    return std::nullopt;
                env.push_back(0.0);
                continue;
            }
            if (cnt % 2 == 0)
            {
                if (*value <= env[cnt - 2])
                    return std::nullopt;
            }
            else if (*value < 0.0 || *value > 1.0)
                return std::nullopt;
            env.push_back(*value);
        }
    }
    if (env.empty() || env.size() % 2 != 0)
        return std::nullopt;
    const auto cnt = env.size();
    const auto maxtime = env[cnt - 2];
    for (std::size_t n = 2; n < cnt; n += 2)
        env[n] = static_cast<float>(env[n] / maxtime);
    env[cnt - 2] = 1.0;
    return env;
}

bool DistortEnvelope::prepare(
    Shape shape, int cycles, double trough, double exponent, std::span<const double> envelope,
    bool liveTrough, bool liveExponent)
{
    if (cycles < 1 || cycles > maximumCycles || !std::isfinite(trough) || trough < 0.0
        || trough > 1.0 || !std::isfinite(exponent) || exponent < minimumExponent
        || exponent > maximumExponent || shape < Shape::Rising || shape > Shape::User
        || (shape == Shape::User && (envelope.size() < 2 || envelope.size() % 2 != 0)))
    {
        return false;
    }
    // reset_distorte_modes: no -e flag, which an exponent of 1 stands for,
    // gives the linear shapes; -t the troffed rising and falling ones. A
    // breakpoint file is given with its flag.
    const auto curved = liveExponent || exponent != 1.0;
    const auto troffed = liveTrough || trough != 0.0;
    switch (shape)
    {
    case Shape::Rising:
        curve_ = curved ? (troffed ? Curve::RisingTroffed : Curve::Rising)
                        : (troffed ? Curve::LinearRiseTroffed : Curve::LinearRise);
        break;
    case Shape::Falling:
        curve_ = curved ? (troffed ? Curve::FallingTroffed : Curve::Falling)
                        : (troffed ? Curve::LinearFallTroffed : Curve::LinearFall);
        break;
    case Shape::Troughed:
        curve_ = curved ? Curve::Troffed : Curve::LinearTrough;
        break;
    case Shape::User:
        curve_ = Curve::User;
        break;
    }
    startCycles_ = cycles;
    startTrof_ = trough;
    startExpon_ = exponent;
    envelope_.assign(envelope.begin(), envelope.end());
    ring_.assign(ringFrames, 0.0F);
    counts_.assign(ringFrames, 0);
    troughs_.assign(ringFrames, 0.0);
    exponents_.assign(ringFrames, 0.0);
    mask_ = ringFrames - 1;
    reset();
    return true;
}

bool DistortEnvelope::setControls(double cycles, double trough, double exponent) noexcept
{
    if (ring_.empty() || !std::isfinite(cycles) || !std::isfinite(trough) || trough < 0.0
        || trough > 1.0 || !std::isfinite(exponent) || exponent < minimumExponent
        || exponent > maximumExponent)
    {
        return false;
    }
    // read_value_from_brktable's rounding of an integer field.
    const auto rounded = std::round(cycles);
    if (rounded < 1.0 || rounded > maximumCycles)
        return false;
    nextCycles_ = static_cast<int>(rounded);
    nextTrof_ = trough;
    nextExpon_ = exponent;
    return true;
}

void DistortEnvelope::startGroup(std::size_t k) noexcept
{
    // process_with_swapped_bufs_on_full_cycles reads the breakpoints where
    // the group starts, and distort_env its trough's complement.
    cycles_ = counts_[k & mask_];
    trof_ = troughs_[k & mask_];
    expon_ = exponents_[k & mask_];
    oneLessTrof_ = 1.0 - trof_;
}

void DistortEnvelope::reset() noexcept
{
    std::ranges::fill(ring_, 0.0F);
    std::ranges::fill(counts_, static_cast<std::uint16_t>(startCycles_));
    std::ranges::fill(troughs_, startTrof_);
    std::ranges::fill(exponents_, startExpon_);
    nextCycles_ = startCycles_;
    nextTrof_ = startTrof_;
    nextExpon_ = startExpon_;
    cycles_ = startCycles_;
    trof_ = startTrof_;
    expon_ = startExpon_;
    oneLessTrof_ = 1.0 - startTrof_;
    frame_ = 0;
    start_ = 0;
    phase_ = 0;
    secondHalf_ = false;
    done_ = 0;
}

bool DistortEnvelope::process(std::span<const double> input, std::span<double> output) noexcept
{
    if (ring_.empty() || input.size() != output.size()
        || !std::ranges::all_of(input, [](double sample) {
               return std::isfinite(sample);
           }))
    {
        return false;
    }
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        const auto k = frame_;
        const auto b = static_cast<float>(input[index]);
        at(k) = b;
        counts_[k & mask_] = static_cast<std::uint16_t>(nextCycles_);
        troughs_[k & mask_] = nextTrof_;
        exponents_[k & mask_] = nextExpon_;
        if (k == start_)
            startGroup(k);
        // The stream waits no longer for a group to end.
        if (k - start_ >= latencyFrames)
        {
            envelopeGroup(k);
            done_ = 0;
            startGroup(k);
        }
        if (phase_ == 0 && b != 0.0F)
            phase_ = b > 0.0F ? 1 : -1;
        // get_full_cycle, as Distort Reverse counts its cycles.
        for (;;)
        {
            const auto firstSide = phase_ >= 0 ? b >= 0.0F : b <= 0.0F;
            const auto secondSide = phase_ >= 0 ? b <= 0.0F : b >= 0.0F;
            if (!secondHalf_ && firstSide)
                break;
            secondHalf_ = true;
            if (secondSide)
                break;
            secondHalf_ = false;
            if (++done_ == cycles_)
            {
                envelopeGroup(k);
                done_ = 0;
                startGroup(k);
            }
        }
        output[index] = k >= latencyFrames ? static_cast<double>(at(k - latencyFrames)) : 0.0;
        ++frame_;
    }
    return true;
}

// distorte_func's shapes; distorte_func_crosbuf's give the same samples
// across two buffers.
void DistortEnvelope::envelopeGroup(std::size_t end) noexcept
{
    const auto cyclestart = start_;
    const auto cyclelen = static_cast<int>(end - cyclestart);
    start_ = end;
    if (cyclelen <= 0)
        return;
    if (curve_ == Curve::Troffed || curve_ == Curve::LinearTrough)
    {
        // distorte_troffed and distorte_lintrof: down to the middle, then up.
        int cyclehlf = cyclelen / 2;
        int k = 0;
        std::size_t j = cyclestart;
        const auto cyclemid = cyclestart + static_cast<std::size_t>(cyclehlf);
        while (j < cyclemid)
        {
            auto z = 1.0 - (static_cast<double>(k) / static_cast<double>(cyclehlf));
            if (curve_ == Curve::Troffed)
                z = (std::pow(z, expon_) * oneLessTrof_) + trof_;
            else
                z = (z * oneLessTrof_) + trof_;
            at(j) = static_cast<float>(at(j) * z);
            j++;
            k++;
        }
        k = 0;
        cyclehlf = cyclelen - cyclehlf;
        while (j < end)
        {
            auto z = static_cast<double>(k) / static_cast<double>(cyclehlf);
            if (curve_ == Curve::Troffed)
                z = (std::pow(z, expon_) * oneLessTrof_) + trof_;
            else
                z = (z * oneLessTrof_) + trof_;
            at(j) = static_cast<float>(at(j) * z);
            j++;
            k++;
        }
        return;
    }
    std::size_t point = 0;
    int k = 0;
    for (auto j = cyclestart; j < end; j++, k++)
        at(j) = static_cast<float>(static_cast<double>(at(j)) * gain(k, cyclelen, point));
}

// do_rising, do_falling, the linear shapes, their troffed forms and
// read_from_user_envelope, expression for expression.
double DistortEnvelope::gain(int k, int cyclelen, std::size_t& point) const noexcept
{
    switch (curve_)
    {
    case Curve::Rising:
    {
        double z = static_cast<double>(k) / static_cast<double>(cyclelen);
        z = std::pow(z, expon_);
        return z;
    }
    case Curve::Falling:
    {
        double z = 1.0 - (static_cast<double>(k) / static_cast<double>(cyclelen));
        z = std::pow(z, expon_);
        return z;
    }
    case Curve::LinearRise:
        return static_cast<double>(k) / static_cast<double>(cyclelen);
    case Curve::LinearFall:
        return 1.0 - (static_cast<double>(k) / static_cast<double>(cyclelen));
    case Curve::RisingTroffed:
    {
        double z = static_cast<double>(k) / static_cast<double>(cyclelen);
        z = std::pow(z, expon_);
        z *= oneLessTrof_;
        z += trof_;
        return z;
    }
    case Curve::FallingTroffed:
    {
        double z = 1.0 - (static_cast<double>(k) / static_cast<double>(cyclelen));
        z = (std::pow(z, expon_) * oneLessTrof_) + trof_;
        return z;
    }
    case Curve::LinearRiseTroffed:
    {
        double z = static_cast<double>(k) / static_cast<double>(cyclelen);
        z = (z * oneLessTrof_) + trof_;
        return z;
    }
    case Curve::LinearFallTroffed:
    {
        double z = 1.0 - (static_cast<double>(k) / static_cast<double>(cyclelen));
        z = (z * oneLessTrof_) + trof_;
        return z;
    }
    case Curve::User:
    {
        const double table_index = static_cast<double>(k) / static_cast<double>(cyclelen);
        if (k == 0)
            point = 0;
        // CDP fails past the envelope's end, which an index under 1 never
        // reaches.
        while (table_index > envelope_[point] && point + 2 < envelope_.size())
            point += 2;
        if (point != 0)
        {
            const auto hi_env = envelope_[point + 1];
            const auto hi_index = envelope_[point];
            const auto lo_env = envelope_[point - 1];
            const auto lo_index = envelope_[point - 2];
            double env = (table_index - lo_index) / (hi_index - lo_index);
            env *= (hi_env - lo_env);
            env += lo_env;
            return env;
        }
        return envelope_[point + 1];
    }
    case Curve::Troffed:
    case Curve::LinearTrough:
        break;
    }
    return 1.0;
}
}
