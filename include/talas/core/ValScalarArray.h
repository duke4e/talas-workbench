#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::core::val_scalar_array
{
inline constexpr std::size_t maximumTextBytes = 4096;
inline constexpr std::size_t maximumMembers = 32;

struct Member
{
    double literal = 0.0;
    std::string eventName;
    std::size_t eventIndex = std::numeric_limits<std::size_t>::max();
};

[[nodiscard]] inline bool beginsArray(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' '
        || text.front() == '\t' || text.front() == '\n'
        || text.front() == '\r'))
        text.remove_prefix(1);
    return text.starts_with("#(");
}

[[nodiscard]] inline std::optional<std::vector<Member>> parse(
    std::string_view text)
{
    if (text.size() > maximumTextBytes)
        return std::nullopt;
    const auto space = [](char value) noexcept {
        return value == ' ' || value == '\t'
            || value == '\n' || value == '\r';
    };
    const auto nameStart = [](char value) noexcept {
        return (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z')
            || value == '_';
    };
    const auto namePart = [&](char value) noexcept {
        return nameStart(value)
            || (value >= '0' && value <= '9');
    };
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    if (!text.starts_with("#("))
        return std::nullopt;
    text.remove_prefix(2);
    std::vector<Member> members;
    while (true)
    {
        while (!text.empty() && space(text.front()))
            text.remove_prefix(1);
        if (text.empty())
            return std::nullopt;
        if (text.front() == ')')
        {
            text.remove_prefix(1);
            while (!text.empty() && space(text.front()))
                text.remove_prefix(1);
            return members.size() >= 2 && text.empty()
                ? std::optional<std::vector<Member>>{std::move(members)}
                : std::nullopt;
        }
        if (members.size() >= maximumMembers)
            return std::nullopt;
        const auto end = text.find_first_of(" \t\n\r)");
        if (end == std::string_view::npos || end == 0)
            return std::nullopt;
        const auto token = text.substr(0, end);
        text.remove_prefix(end);
        Member member;
        if (token.starts_with('!'))
        {
            const auto name = token.substr(1);
            if (name.empty() || !nameStart(name.front()))
                return std::nullopt;
            for (const auto value : name)
                if (!namePart(value))
                    return std::nullopt;
            member.eventName = std::string{name};
        }
        else
        {
            const auto* begin = token.data();
            const auto* tokenEnd = begin + token.size();
            const auto [parsed, error] = std::from_chars(
                begin, tokenEnd, member.literal,
                std::chars_format::general);
            if (error != std::errc{} || parsed != tokenEnd
                || !std::isfinite(member.literal))
                return std::nullopt;
        }
        members.push_back(std::move(member));
        if (!text.empty() && text.front() != ')'
            && !space(text.front()))
            return std::nullopt;
    }
}

template <typename Resolver>
[[nodiscard]] inline std::optional<double> value(
    const Member& member,
    Resolver&& resolve) noexcept
{
    if (member.eventName.empty())
        return member.literal;
    return resolve(member);
}

template <typename Point, typename Resolver>
[[nodiscard]] inline bool buildPoints(
    std::span<const Member> inputs,
    std::span<const Member> outputs,
    Resolver&& resolve,
    double startSlope,
    double endSlope,
    std::vector<Point>& points)
{
    if (inputs.size() != outputs.size() || inputs.size() < 2
        || inputs.size() > maximumMembers
        || !std::isfinite(startSlope) || !std::isfinite(endSlope)
        || points.capacity() < inputs.size() + 2)
        return false;
    points.clear();
    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        const auto input = value(inputs[index], resolve);
        const auto output = value(outputs[index], resolve);
        if (!input.has_value() || !output.has_value()
            || !std::isfinite(*input) || !std::isfinite(*output)
            || *input < -1.0 || *input > 1.0
            || *output < -1.0 || *output > 1.0
            || (!points.empty() && *input <= points.back().input))
            return false;
        points.push_back(Point{*input, *output});
    }
    if (points.front().input > -1.0)
    {
        const auto distance = -1.0 - points.front().input;
        const auto adjustment = startSlope * distance;
        const auto guardedRaw = points.front().output + adjustment;
        const auto raw = std::fma(
            startSlope, distance, points.front().output);
        if (!std::isfinite(distance) || !std::isfinite(adjustment)
            || !std::isfinite(guardedRaw) || !std::isfinite(raw))
            return false;
        points.insert(points.begin(),
            Point{-1.0, std::clamp(raw, -1.0, 1.0)});
    }
    if (points.back().input < 1.0)
    {
        const auto distance = 1.0 - points.back().input;
        const auto adjustment = endSlope * distance;
        const auto guardedRaw = points.back().output + adjustment;
        const auto raw = std::fma(
            endSlope, distance, points.back().output);
        if (!std::isfinite(distance) || !std::isfinite(adjustment)
            || !std::isfinite(guardedRaw) || !std::isfinite(raw))
            return false;
        points.push_back(Point{1.0, std::clamp(raw, -1.0, 1.0)});
    }
    return true;
}
}
