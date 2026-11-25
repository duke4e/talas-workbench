#pragma once

#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace talas::runtime
{
inline constexpr std::size_t maximumPreparedMidiByteCount = 256;

struct PreparedMidiByteExpression
{
    std::optional<std::uint8_t> literal;
    std::optional<talas::expr::Expression> expression;
};

[[nodiscard]] inline std::optional<std::uint8_t>
parsePreparedMidiByteLiteral(std::string_view token) noexcept
{
    int base = 10;
    if (token.starts_with("16r") || token.starts_with("16R"))
    {
        token.remove_prefix(3);
        base = 16;
    }
    if (token.empty())
        return std::nullopt;
    unsigned int value = 0;
    const auto [end, error] = std::from_chars(
        token.data(), token.data() + token.size(), value, base);
    if (error != std::errc{}
        || end != token.data() + token.size()
        || value > 255)
    {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] inline std::optional<
    std::vector<PreparedMidiByteExpression>>
parsePreparedMidiByteText(
    std::string_view text,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions)
{
    std::vector<PreparedMidiByteExpression> result;
    result.reserve(maximumPreparedMidiByteCount);
    std::size_t offset = 0;
    while (offset < text.size())
    {
        while (offset < text.size()
               && std::isspace(
                   static_cast<unsigned char>(text[offset])))
        {
            ++offset;
        }
        if (offset >= text.size())
            break;
        if (result.size() >= maximumPreparedMidiByteCount)
            return std::nullopt;

        if (text[offset] == '{')
        {
            const auto expressionStart = offset + 1;
            const auto expressionEnd = text.find('}', expressionStart);
            if (expressionEnd == std::string_view::npos)
                return std::nullopt;
            const auto parsed = talas::expr::parse(
                text.substr(
                    expressionStart,
                    expressionEnd - expressionStart),
                language,
                definitions);
            if (!parsed.ok() || !parsed.expression.has_value()
                || !parsed.expression->identifiers().empty())
            {
                return std::nullopt;
            }
            result.push_back({std::nullopt, *parsed.expression});
            offset = expressionEnd + 1;
            if (offset < text.size()
                && !std::isspace(
                    static_cast<unsigned char>(text[offset])))
            {
                return std::nullopt;
            }
            continue;
        }

        const auto tokenStart = offset;
        while (offset < text.size()
               && !std::isspace(
                   static_cast<unsigned char>(text[offset])))
        {
            ++offset;
        }
        const auto literal = parsePreparedMidiByteLiteral(
            text.substr(tokenStart, offset - tokenStart));
        if (!literal.has_value())
            return std::nullopt;
        result.push_back({literal, std::nullopt});
    }
    return result;
}

// A Sound's amplitude goes out as a 7-bit
// controller value, at most once a millisecond. A negative amplitude sends
// 0, our choice.
[[nodiscard]] inline std::uint8_t midiControllerByteFromAmplitude(double amplitude) noexcept
{
    return static_cast<std::uint8_t>(std::round(std::clamp(amplitude, 0.0, 1.0) * 127.0));
}

inline constexpr double midiControllerSendsPerSecond = 1000.0;

// A frequency between equal-tempered pitches goes out
// as the nearest pitch plus a pitch bend, for a bend range of an octave
// either way. The 14-bit bend is 8192 at the pitch itself.
[[nodiscard]] inline std::optional<std::uint16_t> midiPitchBendFromFrequency(double frequencyHz) noexcept
{
    if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0)
        return std::nullopt;
    const auto note = 69.0 + 12.0 * std::log2(frequencyHz / 440.0);
    if (!std::isfinite(note))
        return std::nullopt;
    const auto octaves = (note - std::round(note)) / 12.0;
    const auto bend = 8192.0 + octaves * (octaves < 0.0 ? 8192.0 : 8191.0);
    return static_cast<std::uint16_t>(std::clamp(std::round(bend), 0.0, 16383.0));
}

// A static Value: an amplitude, or in projects before format 67 a
// controller integer from 0 to 127.
[[nodiscard]] inline std::optional<std::uint8_t>
parsePreparedMidiControllerValueText(
    std::string_view text,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions,
    bool amplitude = false)
{
    const auto trim = [](std::string_view value) noexcept {
        while (!value.empty()
               && std::isspace(
                   static_cast<unsigned char>(value.front())))
        {
            value.remove_prefix(1);
        }
        while (!value.empty()
               && std::isspace(
                   static_cast<unsigned char>(value.back())))
        {
            value.remove_suffix(1);
        }
        return value;
    };

    text = trim(text);
    if (text.empty())
        return std::nullopt;
    if (text.front() == '{' && text.back() == '}')
        text = trim(text.substr(1, text.size() - 2));
    if (text.empty())
        return std::nullopt;

    const auto parsed = talas::expr::parse(text, language, definitions);
    if (!parsed.ok() || !parsed.expression.has_value()
        || !parsed.expression->eventReferences().empty()
        || !parsed.expression->identifiers().empty())
    {
        return std::nullopt;
    }
    const auto evaluated = talas::expr::evaluate(
        *parsed.expression,
        {},
        talas::expr::EvaluationOptions{talas::expr::ValueDomain::Scalar});
    if (amplitude)
    {
        if (!evaluated.ok() || !std::isfinite(evaluated.value.value))
            return std::nullopt;
        return midiControllerByteFromAmplitude(evaluated.value.value);
    }
    if (!evaluated.ok()
        || !std::isfinite(evaluated.value.value)
        || evaluated.value.value < 0.0
        || evaluated.value.value > 127.0
        || std::abs(
               evaluated.value.value
               - std::round(evaluated.value.value))
            > 1.0e-9)
    {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(
        std::round(evaluated.value.value));
}
}
