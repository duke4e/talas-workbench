/*
 * Derived from the CDP System, Release 8: dev/filter/filters0.c and
 * ap_filter.c, and dev/cdp2k/tklib1.c
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

// What CDP's user and time-varying filter banks share: reading filter data
// and running two-pole resonators over it.

#include "talas/core/FromChars.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::cdp::filter_bank
{
// globcon.h's PI, LOW_A, ONE_OVER_LN2, FLTERR and the dB limits;
// filtcon.h's FLT_MINFRQ and MINFILTAMP; columns.h's MIDIMAX.
inline constexpr double pi = 3.141592654;
inline constexpr double lowA = 6.875;
inline constexpr double oneOverLn2 = 1.442695;
inline constexpr double floatError = 0.000002;
inline constexpr double minimumDb = -96.0;
inline constexpr double maximumDb = 96.0;
inline constexpr double minimumFrequency = 0.1;
inline constexpr double maximumMidi = 127.0;
inline constexpr double minimumFilterAmplitude = 0.001;

[[nodiscard]] inline bool space(char character) noexcept
{
    return character == ' ' || character == '\t' || character == '\n'
        || character == '\r' || character == '\v' || character == '\f';
}

// tklib1.c's flteq.
[[nodiscard]] inline bool flteq(double f1, double f2) noexcept
{
    const auto upperbnd = f2 + floatError;
    const auto lowerbnd = f2 - floatError;
    return !((f1 > upperbnd) || (f1 < lowerbnd));
}

// sscanf's "%lf" for a word: the longest number at its start.
[[nodiscard]] inline std::optional<double> scanNumber(std::string_view word) noexcept
{
    std::size_t position = 0;
    if (position < word.size() && word[position] == '+')
        ++position;
    double value = 0.0;
    const auto* first = word.data() + position;
    const auto* last = word.data() + word.size();
    const auto [end, error] = talas::fromChars(first, last, value);
    if (error != std::errc{} || end == first)
        return std::nullopt;
    return value;
}

// tklib1.c's miditohz, unchecked_hztomidi and get_level with get_leveldb.
[[nodiscard]] inline double miditohz(double val) noexcept
{
    val += 3.0;
    val /= 12.0;
    val = std::pow(2.0, val);
    val *= lowA;
    return val;
}

[[nodiscard]] inline double uncheckedHztomidi(double hz) noexcept
{
    double val;
    val = hz;
    val /= lowA;
    val = (std::log(val) * oneOverLn2);
    val *= 12.0;
    val -= 3.0;
    return val;
}

[[nodiscard]] inline std::optional<double> level(std::string_view word) noexcept
{
    const auto decibels = word.size() >= 2
        && (word.ends_with("dB") || word.ends_with("DB") || word.ends_with("db"));
    auto val = scanNumber(word);
    if (!val.has_value() || !decibels)
        return val;
    auto is_neg = false;
    *val = std::max(*val, minimumDb);
    *val = std::min(*val, maximumDb);
    if (flteq(*val, 0.0))
        return 1.0;
    if (*val < 0.0)
    {
        *val = -(*val);
        is_neg = true;
    }
    *val /= 20.0;
    *val = std::pow(10.0, *val);
    if (is_neg)
        *val = 1.0 / (*val);
    return val;
}

// is_an_empty_line_or_a_comment.
[[nodiscard]] inline bool emptyOrComment(std::string_view line) noexcept
{
    const auto first = std::ranges::find_if_not(line, space);
    return first == line.end() || *first == ';';
}

// get_word_from_string over a line.
[[nodiscard]] inline std::vector<std::string_view> words(std::string_view line)
{
    std::vector<std::string_view> result;
    std::size_t position = 0;
    while (position < line.size())
    {
        while (position < line.size() && space(line[position]))
            ++position;
        if (position == line.size())
            break;
        const auto start = position;
        while (position < line.size() && !space(line[position]))
            ++position;
        result.push_back(line.substr(start, position - start));
    }
    return result;
}

// Each line of the text.
template <class Visit> void forEachLine(std::string_view text, Visit&& visit)
{
    std::size_t begin = 0;
    while (begin < text.size())
    {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        visit(text.substr(begin, end - begin));
        begin = end + 1;
    }
}

// ap_filter.c's get_coeffs1 and get_coeffs2 for one band.
inline void bandCoefficients(
    double frequency, double amplitude, double q, double inverseSampleRate,
    bool doubleFiltering, double& a, double& b, double& ampl) noexcept
{
    const auto ww = 2.0 * pi * frequency * inverseSampleRate;
    const auto cosw = std::cos(ww);
    const auto sinw = std::sin(ww);
    double g, r;
    r = std::exp(-(ww) / (2.0 * q));
    a = 2.0 * r * cosw;
    b = -(r)*r;
    g = 1.0 / ((1.0 + b) * sinw);
    ampl = amplitude / g;
    if (doubleFiltering)
        ampl /= g;
}

// filters0.c's filtering for one frame of one channel, statement for
// statement, then check_float_limits, clipping float samples as CDP does by
// default and lowering the gain by 0.9999 for good.
[[nodiscard]] inline double filterFrame(
    double input,
    std::span<const double> a,
    std::span<const double> b,
    std::span<const double> ampl,
    std::span<double> y,
    std::span<double> z,
    std::span<double> d,
    std::span<double> e,
    bool doubleFiltering,
    double& gain) noexcept
{
    double sum, xx;
    const auto sample = static_cast<double>(static_cast<float>(input));
    sum = 0.0;
    for (std::size_t fno = 0; fno < a.size(); ++fno)
    {
        xx = sample + (a[fno] * y[fno]) + (b[fno] * z[fno]);
        z[fno] = y[fno];
        y[fno] = xx;
        if (doubleFiltering)
        {
            xx += (a[fno] * d[fno]) + (b[fno] * e[fno]);
            e[fno] = d[fno];
            d[fno] = xx;
        }
        sum += (xx * ampl[fno]);
    }
    sum *= gain;
    if (sum > 1.0)
    {
        gain *= 0.9999;
        sum = 1.0;
    }
    if (sum < -1.0)
    {
        gain *= 0.9999;
        sum = -1.0;
    }
    return static_cast<double>(static_cast<float>(sum));
}
}
