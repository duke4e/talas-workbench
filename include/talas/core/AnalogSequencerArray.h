#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::core
{
struct AnalogSequencerArrayMember
{
    std::string_view source;
    std::size_t offset = 0;
    bool implicitDomainNumber = false;
};

struct AnalogSequencerArray
{
    bool numericLiteral = false;
    std::vector<AnalogSequencerArrayMember> members;
};

// Structural parsing only: each source remains subject to the project's
// expression dialect and domain checks. Views refer to the caller's text.
[[nodiscard]] inline std::optional<AnalogSequencerArray>
parseAnalogSequencerArray(std::string_view text)
{
    if (text.size() > 65'536) return {};
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    const auto nameStart = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    const auto namePart = [&](char c) { return nameStart(c) || (c >= '0' && c <= '9'); };
    const auto word = [&](std::string_view value) {
        if (value.empty() || !nameStart(value.front())) return false;
        for (const auto c : value) if (!namePart(c)) return false;
        return true;
    };
    std::size_t cursor = 0;
    const auto skipSpace = [&] { while (cursor < text.size() && space(text[cursor])) ++cursor; };
    skipSpace();
    AnalogSequencerArray result;
    result.numericLiteral = text.substr(cursor).starts_with("#(");
    if (result.numericLiteral) cursor += 2;
    while (true)
    {
        skipSpace();
        if (cursor == text.size())
            return !result.numericLiteral && !result.members.empty()
                ? std::optional{std::move(result)} : std::nullopt;
        if (result.numericLiteral && text[cursor] == ')')
        {
            ++cursor; skipSpace();
            return cursor == text.size() && !result.members.empty()
                ? std::optional{std::move(result)} : std::nullopt;
        }
        if (result.members.size() == 256) return {};
        auto start = cursor;
        if (!result.numericLiteral && text[cursor] == '{')
        {
            const auto close = text.find('}', ++cursor);
            if (close == std::string_view::npos) return {};
            start = cursor;
            while (start < close && space(text[start])) ++start;
            auto end = close;
            while (end > start && space(text[end - 1])) --end;
            if (start == end || text.substr(start, end - start).find('{') != std::string_view::npos) return {};
            result.members.push_back({text.substr(start, end - start), start});
            cursor = close + 1;
            if (cursor < text.size() && !space(text[cursor]) && text[cursor] != '{') return {};
            continue;
        }
        while (cursor < text.size() && !space(text[cursor])
            && !(result.numericLiteral && text[cursor] == ')')) ++cursor;
        auto atom = text.substr(start, cursor - start);
        bool implicitDomainNumber = false;
        if (!result.numericLiteral && !atom.empty() && (atom.front() == '!' || atom.front() == '@'))
        {
            if (atom.size() > 129 || !word(atom.substr(1))) return {};
        }
        else
        {
            auto number = atom;
            if (!result.numericLiteral && number.starts_with('+')) number.remove_prefix(1);
            double value = 0;
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr == number.data() || !std::isfinite(value)) return {};
            const auto suffix = number.substr(static_cast<std::size_t>(parsed.ptr - number.data()));
            if (!suffix.empty() && (result.numericLiteral || !word(suffix))) return {};
            implicitDomainNumber = suffix.empty();
        }
        auto end = cursor;
        // A following bare word belongs to this atom as a unit. The expression
        // parser validates its spelling and whether this dialect permits it.
        if (!result.numericLiteral)
        {
            skipSpace();
            if (cursor < text.size() && nameStart(text[cursor]))
            {
                const auto unitStart = cursor;
                while (cursor < text.size() && !space(text[cursor])) ++cursor;
                if (!word(text.substr(unitStart, cursor - unitStart))) return {};
                end = cursor;
                implicitDomainNumber = false;
            }
        }
        result.members.push_back({text.substr(start, end - start), start, implicitDomainNumber});
    }
}
}
