#pragma once

#include "talas/cdp/SubmixPairs.h"
#include "talas/core/FromChars.h"

#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace talas::runtime::cdp_mix_faders
{
// CDP Mix Faders' balance table, read as CDP's submix faders reads its
// balance file (ap_mix.c's read_and_scale_balance_data): times in seconds,
// each followed by the input's and the side chain's levels, written with
// digits, points, minus signs and spaces only (CDP's invalid_data_items), a
// line of text whose first mark is a semicolon a remark. CDP scales the last
// time's levels to sum to 1 at the end of each line of text (as fgets reads
// it, up to 199 characters) that leaves it a whole number of times, so a
// blank line scales them again; talas::cdp::MixFaders scales each time's levels
// once, so the extra scalings are done here. No times, a time without both
// levels, a number CDP cannot read (where CDP passes over the rest of its
// line) and a time CDP never scales (the first of two on a line of text,
// played unscaled, which the port cannot play) are refused. The port checks
// the times and levels as CDP checks them.
[[nodiscard]] inline std::optional<std::vector<talas::cdp::MixFaders::Line>> readBalance(std::string_view text)
{
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    std::vector<double> numbers;
    // The times each time's levels are scaled.
    std::vector<int> scalings;
    std::size_t begin = 0;
    while (begin < text.size())
    {
        // fgets(temp, 200, fp): up to 199 characters, through a newline.
        auto end = begin;
        while (end < text.size() && end - begin < 199)
        {
            if (text[end++] == '\n')
                break;
        }
        const auto line = text.substr(begin, end - begin);
        begin = end;
        std::size_t position = 0;
        while (position < line.size() && space(line[position]))
            ++position;
        if (position < line.size() && line[position] == ';')
            continue;
        if (line.find_first_not_of(" \t\n\r\f\v0123456789.-") != std::string_view::npos)
            return std::nullopt;
        while (position < line.size())
        {
            auto stop = position;
            while (stop < line.size() && !space(line[stop]))
                ++stop;
            double value = 0.0;
            const auto [last, error] = talas::fromChars(
                line.data() + position, line.data() + stop, value, std::chars_format::fixed);
            if (error != std::errc{} || last != line.data() + stop)
                return std::nullopt;
            numbers.push_back(value);
            position = stop;
            while (position < line.size() && space(line[position]))
                ++position;
        }
        if (!numbers.empty() && numbers.size() % 3 == 0)
        {
            scalings.resize(numbers.size() / 3);
            ++scalings.back();
        }
    }
    if (numbers.empty() || numbers.size() % 3 != 0 || scalings.size() != numbers.size() / 3)
        return std::nullopt;
    std::vector<talas::cdp::MixFaders::Line> lines;
    for (std::size_t index = 0; index < scalings.size(); ++index)
    {
        if (scalings[index] == 0)
            return std::nullopt;
        talas::cdp::MixFaders::Line line{numbers[3 * index], numbers[3 * index + 1], numbers[3 * index + 2]};
        // CDP's scalings but the one the port makes: the sum from the side
        // chain's level back, then each level times one over it.
        for (int again = 1; again < scalings[index] && line.input >= 0.0 && line.side >= 0.0; ++again)
        {
            double sum = 0.0;
            sum += line.side;
            sum += line.input;
            if (!(sum > 0.0))
                break;
            sum = 1.0 / sum;
            line.input *= sum;
            line.side *= sum;
        }
        lines.push_back(line);
    }
    return lines;
}
}
