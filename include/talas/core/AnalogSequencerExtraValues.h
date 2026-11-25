#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::core
{
struct AnalogSequencerExtraSequence
{
    std::string name;
    std::vector<std::string> expressions;
};

// GeneratedIndex is an Event Value target, not a value expression. Keep its
// bounded name grammar alongside ExtraValues so authoring, discovery and
// expansion agree on the exact name that is published.
[[nodiscard]] inline std::optional<std::string>
parseAnalogSequencerGeneratedIndex(std::string_view text)
{
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n'
            || c == '\f' || c == '\v';
    };
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && space(text.back()))
        text.remove_suffix(1);
    if (text.empty())
        return std::string{};
    if (text.front() != '!')
        return std::nullopt;
    text.remove_prefix(1);
    const auto nameStart = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    if (text.empty() || !nameStart(text.front()) || text.size() > 128
        || !std::ranges::all_of(text, [&](char c) {
            return nameStart(c) || (c >= '0' && c <= '9');
        }))
        return std::nullopt;
    if (text == "Pitch" || text == "KeyPitch" || text == "KeyNumber"
        || text == "KeyDown" || text == "KeyVelocity"
        || text == "KeyTimbre")
        return std::nullopt;
    return std::string{text};
}

// The field contains adjacent named Smalltalk arrays. Brace members
// retain expression text so project audit and runtime preparation use the same
// grammar without treating the leading EventValue name as a value expression.
[[nodiscard]] inline std::optional<std::vector<AnalogSequencerExtraSequence>>
parseAnalogSequencerExtraValues(std::string_view text)
{
    if (text.size() > 65'536) return {};
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n'
            || c == '\f' || c == '\v';
    };
    const auto trim = [&] {
        while (!text.empty() && space(text.front())) text.remove_prefix(1);
    };
    const auto nameStart = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    std::vector<AnalogSequencerExtraSequence> result;
    trim();
    while (!text.empty())
    {
        if (!text.starts_with("#(") || result.size() == 32) return {};
        text.remove_prefix(2);
        trim();
        if (!text.starts_with('!')) return {};
        text.remove_prefix(1);
        if (text.empty() || !nameStart(text.front())) return {};
        std::size_t length = 1;
        while (length < text.size() && (nameStart(text[length])
                || (text[length] >= '0' && text[length] <= '9'))) ++length;
        if (length > 128 || length == text.size() || !space(text[length])) return {};
        AnalogSequencerExtraSequence sequence{std::string(text.substr(0, length)), {}};
        if (sequence.name == "Pitch" || sequence.name == "KeyPitch"
            || sequence.name == "KeyNumber" || sequence.name == "KeyDown"
            || sequence.name == "KeyVelocity" || sequence.name == "KeyTimbre"
            || std::ranges::any_of(result, [&](const auto& item) {
                return item.name == sequence.name;
            })) return {};
        text.remove_prefix(length);
        trim();
        while (!text.empty() && text.front() != ')')
        {
            if (sequence.expressions.size() == 256) return {};
            if (text.front() == '{')
            {
                const auto end = text.find('}');
                if (end == std::string_view::npos || end == 1) return {};
                sequence.expressions.emplace_back(text.substr(1, end - 1));
                text.remove_prefix(end + 1);
            }
            else if (text.front() == '!')
            {
                // An Event Value is a simple value, needing no braces
                // (1 !Fader1 {2 + 3}).
                std::size_t size = 1;
                while (size < text.size() && (nameStart(text[size]) || (size > 1 && text[size] >= '0' && text[size] <= '9')))
                    ++size;
                if (size == 1 || size > 129) return {};
                sequence.expressions.emplace_back(text.substr(0, size));
                text.remove_prefix(size);
            }
            else
            {
                double value = 0;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr == text.data() || !std::isfinite(value))
                    return {};
                const auto size = static_cast<std::size_t>(parsed.ptr - text.data());
                sequence.expressions.emplace_back(text.substr(0, size));
                text.remove_prefix(size);
            }
            if (!text.empty() && !space(text.front()) && text.front() != ')') return {};
            trim();
        }
        if (text.empty() || sequence.expressions.empty()) return {};
        text.remove_prefix(1);
        result.push_back(std::move(sequence));
        trim();
    }
    return result;
}
}
