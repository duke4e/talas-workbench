#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// A list field may be generated in Smalltalk, as
// {(1 to: 9) collect: [:i | (i - 1) / 8.0 * 2 - 1]} is. A field that is wholly
// an Interval, with or without a by: step, collected through a one-argument
// block stands for the block's value at each step of the Interval.
namespace talas::core::interval_collect_array
{
inline constexpr std::size_t maximumElements = 65536;

enum class Status
{
    NotInterval,
    Expanded,
    Unsupported,
};

// For Expanded, the block's body at each step with its argument written
// out, for the field's own evaluator to take as a braced element.
struct Expansion
{
    Status status = Status::NotInterval;
    std::vector<std::string> elements;
};

namespace detail
{
[[nodiscard]] inline bool isSpace(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\n' || value == '\r'
        || value == '\f' || value == '\v';
}

[[nodiscard]] inline bool isDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

[[nodiscard]] inline bool isLetter(char value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

[[nodiscard]] inline bool isIdentifierCharacter(char value) noexcept
{
    return isLetter(value) || isDigit(value) || value == '_';
}

[[nodiscard]] inline std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && isSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

class Cursor
{
public:
    explicit Cursor(std::string_view text) noexcept
        : text_(text)
    {
    }

    [[nodiscard]] bool consume(std::string_view token) noexcept
    {
        skipSpace();
        if (text_.substr(offset_, token.size()) != token)
            return false;
        offset_ += token.size();
        return true;
    }

    // A keyword such as to:, not the start of a longer one.
    [[nodiscard]] bool consumeKeyword(std::string_view keyword) noexcept
    {
        skipSpace();
        if (offset_ != 0 && isIdentifierCharacter(text_[offset_ - 1]))
            return false;
        return consume(keyword);
    }

    // A Smalltalk number literal: digits, an optional fraction, an
    // optional leading minus.
    [[nodiscard]] std::optional<double> number() noexcept
    {
        skipSpace();
        const auto start = offset_;
        auto end = offset_;
        if (end < text_.size() && text_[end] == '-')
            ++end;
        const auto digits = end;
        while (end < text_.size() && isDigit(text_[end]))
            ++end;
        if (end == digits)
            return std::nullopt;
        if (end + 1 < text_.size() && text_[end] == '.' && isDigit(text_[end + 1]))
        {
            ++end;
            while (end < text_.size() && isDigit(text_[end]))
                ++end;
        }
        if (end < text_.size() && isIdentifierCharacter(text_[end]))
            return std::nullopt;
        double value = 0.0;
        const auto [parsedEnd, error] = std::from_chars(
            text_.data() + start, text_.data() + end, value);
        if (error != std::errc{} || parsedEnd != text_.data() + end)
            return std::nullopt;
        offset_ = end;
        return value;
    }

    [[nodiscard]] std::string_view identifier() noexcept
    {
        skipSpace();
        const auto start = offset_;
        if (offset_ >= text_.size() || !isLetter(text_[offset_]))
            return {};
        while (offset_ < text_.size() && isIdentifierCharacter(text_[offset_]))
            ++offset_;
        return text_.substr(start, offset_ - start);
    }

    [[nodiscard]] std::string_view rest() const noexcept
    {
        return text_.substr(offset_);
    }

private:
    void skipSpace() noexcept
    {
        while (offset_ < text_.size() && isSpace(text_[offset_]))
            ++offset_;
    }

    std::string_view text_;
    std::size_t offset_ = 0;
};

[[nodiscard]] inline std::string formatStep(double value)
{
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc{})
        return {};
    return "(" + std::string(buffer.data(), end) + ")";
}

// The body with each use of the argument replaced by the step's value; a
// name within a longer identifier, an Event Value (!i), a variable (@i, ?i)
// or a keyword (i:) is left alone.
[[nodiscard]] inline std::string substitute(
    std::string_view body,
    std::string_view argument,
    std::string_view value)
{
    std::string result;
    result.reserve(body.size() + value.size());
    std::size_t offset = 0;
    while (offset < body.size())
    {
        const auto found = body.find(argument, offset);
        if (found == std::string_view::npos)
            break;
        const auto after = found + argument.size();
        const auto before = found == 0 ? '\0' : body[found - 1];
        const auto isUse = !isIdentifierCharacter(before)
            && before != '!' && before != '@' && before != '?'
            && (after >= body.size()
                || (!isIdentifierCharacter(body[after]) && body[after] != ':'));
        result.append(body.substr(offset, found - offset));
        if (isUse)
            result.append(value);
        else
            result.append(argument);
        offset = after;
    }
    result.append(body.substr(offset));
    return result;
}

// !Amp suffix2: (3) is the Event Value !Amp03: the step's whole number
// tacked onto the name, with a leading 0 below 10 ({(1 to: 16) collect: [:i |
// !Amp suffix2: i]} creates 16 faders). Anything else is left for the field's
// evaluator to refuse.
[[nodiscard]] inline std::string suffixed(std::string text)
{
    constexpr std::string_view keyword = "suffix2:";
    std::size_t from = 0;
    while (true)
    {
        const auto found = text.find(keyword, from);
        if (found == std::string::npos)
            return text;
        from = found + keyword.size();
        auto nameEnd = found;
        while (nameEnd > 0 && isSpace(text[nameEnd - 1]))
            --nameEnd;
        auto nameStart = nameEnd;
        while (nameStart > 0 && isIdentifierCharacter(text[nameStart - 1]))
            --nameStart;
        if (nameStart == nameEnd || nameStart == 0 || text[nameStart - 1] != '!' || !isLetter(text[nameStart]))
            continue;
        auto cursor = from;
        while (cursor < text.size() && isSpace(text[cursor]))
            ++cursor;
        const auto parenthesized = cursor < text.size() && text[cursor] == '(';
        if (parenthesized)
            ++cursor;
        const auto digits = cursor;
        while (cursor < text.size() && isDigit(text[cursor]))
            ++cursor;
        if (cursor == digits || (cursor < text.size() && (text[cursor] == '.' || isIdentifierCharacter(text[cursor]))))
            continue;
        if (parenthesized)
        {
            if (cursor >= text.size() || text[cursor] != ')')
                continue;
            ++cursor;
        }
        auto number = text.substr(digits, (parenthesized ? cursor - 1 : cursor) - digits);
        if (number.size() < 2)
            number.insert(0, "0");
        const auto name = text.substr(nameStart, nameEnd - nameStart) + number;
        text.replace(nameStart, cursor - nameStart, name);
        from = nameStart + name.size();
    }
}
}

[[nodiscard]] inline Expansion expand(std::string_view text)
{
    text = detail::trim(text);
    if (text.size() < 2 || text.front() != '{' || text.back() != '}')
        return {};
    const auto inner = text.substr(1, text.size() - 2);
    if (inner.find_first_of("{}") != std::string_view::npos)
        return {};
    detail::Cursor cursor{inner};
    if (!cursor.consume("("))
        return {};
    const auto first = cursor.number();
    if (!first.has_value() || !cursor.consumeKeyword("to:"))
        return {};
    const auto last = cursor.number();
    if (!last.has_value())
        return {};
    auto step = 1.0;
    if (cursor.consumeKeyword("by:"))
    {
        const auto by = cursor.number();
        if (!by.has_value())
            return {};
        step = *by;
    }
    if (!cursor.consume(")") || !cursor.consumeKeyword("collect:")
        || !cursor.consume("[") || !cursor.consume(":"))
        return {};
    const auto argument = cursor.identifier();
    if (argument.empty() || !cursor.consume("|"))
        return {};
    auto body = detail::trim(cursor.rest());
    if (body.empty() || body.back() != ']')
        return {};
    body = detail::trim(body.substr(0, body.size() - 1));

    Expansion expansion;
    expansion.status = Status::Unsupported;
    if (body.empty() || step == 0.0 || !std::isfinite(*first)
        || !std::isfinite(*last) || !std::isfinite(step))
        return expansion;
    // An Interval holds (last - first) // step + 1 values, none when
    // last lies before first along the step.
    const auto span = std::floor((*last - *first) / step);
    if (!std::isfinite(span) || span >= static_cast<double>(maximumElements))
        return expansion;
    const auto count = span < 0.0 ? std::size_t{0} : static_cast<std::size_t>(span) + 1;
    expansion.elements.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto value = detail::formatStep(
            *first + static_cast<double>(index) * step);
        if (value.empty())
        {
            expansion.elements.clear();
            return expansion;
        }
        expansion.elements.push_back(detail::suffixed(detail::substitute(body, argument, value)));
    }
    expansion.status = Status::Expanded;
    return expansion;
}
}
