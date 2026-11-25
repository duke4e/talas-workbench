#pragma once

#include "talas/core/FromChars.h"
#include "talas/expr/Expression.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::runtime::checked_static_expression_array
{
inline constexpr std::size_t maximumElements = 65536;

enum class Domain
{
    Scalar,
    Hertz,
    // Half steps, as an interval in nn is.
    NoteNumber,
};

struct Error
{
    std::size_t elementIndex = 0;
    std::size_t sourceOffset = 0;
    std::string message;
};

struct Result
{
    std::vector<double> values;
    std::optional<Error> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return !error.has_value();
    }
};

[[nodiscard]] inline bool isAsciiSpace(char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\n'
        || value == '\r' || value == '\f' || value == '\v';
}

[[nodiscard]] inline bool isSeparator(char value) noexcept
{
    return isAsciiSpace(value) || value == ',' || value == ';'
        || value == '[' || value == ']' || value == '('
        || value == ')';
}

[[nodiscard]] inline std::string_view trimAsciiSpace(
    std::string_view text) noexcept
{
    while (!text.empty() && isAsciiSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isAsciiSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] inline Result fail(
    std::size_t elementIndex,
    std::size_t sourceOffset,
    std::string message)
{
    Result result;
    result.error = Error{
        elementIndex,
        sourceOffset,
        std::move(message),
    };
    return result;
}

[[nodiscard]] inline std::optional<double> parseFiniteLiteral(
    std::string_view text) noexcept
{
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-'))
    {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty() || text.front() == '+' || text.front() == '-')
        return std::nullopt;

    auto format = std::chars_format::general;
    if (text.size() >= 2 && text[0] == '0'
        && (text[1] == 'x' || text[1] == 'X'))
    {
        // Floating-point from_chars expects hexadecimal digits without 0x.
        text.remove_prefix(2);
        if (text.empty() || text.front() == '+' || text.front() == '-')
            return std::nullopt;
        format = std::chars_format::hex;
    }
    if (text.empty())
        return std::nullopt;

    double magnitude = 0.0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = talas::fromChars(begin, end, magnitude, format);
    if (parsed.ec != std::errc{} || parsed.ptr != end
        || !std::isfinite(magnitude))
    {
        return std::nullopt;
    }
    return negative ? -magnitude : magnitude;
}

[[nodiscard]] inline std::optional<double> evaluateExpression(
    std::string_view source,
    Domain domain,
    double sampleRate,
    double tempoBpm,
    std::string& error)
{
    const auto parsed = talas::expr::parse(
        source,
        talas::expr::ExpressionLanguageVersion::VariablesV2);
    if (!parsed.ok())
    {
        error = parsed.diagnostics.empty()
            ? "brace expression failed to parse"
            : parsed.diagnostics.front().message;
        return std::nullopt;
    }

    const auto& expression = *parsed.expression;
    if (expression.isStateful()
        || !expression.eventReferences().empty()
        || !expression.variableReferences().empty()
        || !expression.identifiers().empty())
    {
        error = "brace expression must be stateless and contain no Event Value, variable, or identifier references";
        return std::nullopt;
    }

    talas::expr::EvaluationContext context;
    context.sampleRate = sampleRate;
    context.tempoBpm = tempoBpm;
    const auto evaluateAs = [&](talas::expr::ValueDomain expected) {
        return talas::expr::evaluate(
            expression,
            context,
            talas::expr::EvaluationOptions{expected});
    };

    auto evaluation = evaluateAs(
        domain == Domain::Scalar
            ? talas::expr::ValueDomain::Scalar
            : domain == Domain::Hertz
            ? talas::expr::ValueDomain::Hertz
            : talas::expr::ValueDomain::NoteNumber);
    if (!evaluation.ok() && domain != Domain::Scalar)
    {
        auto scalarEvaluation = evaluateAs(talas::expr::ValueDomain::Scalar);
        if (scalarEvaluation.ok())
            evaluation = std::move(scalarEvaluation);
    }
    if (!evaluation.ok())
    {
        error = evaluation.diagnostics.empty()
            ? "brace expression failed to evaluate"
            : evaluation.diagnostics.front().message;
        return std::nullopt;
    }
    if (!std::isfinite(evaluation.value.value))
    {
        error = "brace expression result must be finite";
        return std::nullopt;
    }
    return evaluation.value.value;
}

// An element given in liveValues follows Event Values, variables or time,
// so its value there replaces the element's text.
[[nodiscard]] inline Result parse(
    std::string_view text,
    Domain domain,
    double sampleRate,
    double tempoBpm,
    std::span<const std::optional<double>> liveValues = {})
{
    Result result;
    std::size_t offset = 0;
    std::size_t elementIndex = 0;
    while (offset < text.size())
    {
        while (offset < text.size() && isSeparator(text[offset]))
            ++offset;
        if (offset == text.size())
            break;
        if (elementIndex >= maximumElements)
        {
            return fail(
                elementIndex,
                offset,
                "array exceeds the fixed element budget");
        }

        const auto elementOffset = offset;
        std::optional<double> value;
        if (elementIndex < liveValues.size() && liveValues[elementIndex].has_value())
        {
            if (text[offset] == '{')
            {
                const auto close = text.find('}', offset);
                offset = close == std::string_view::npos ? text.size() : close + 1;
            }
            else
            {
                while (offset < text.size() && !isSeparator(text[offset]))
                    ++offset;
            }
            value = liveValues[elementIndex];
        }
        else if (text[offset] == '{')
        {
            const auto expressionOffset = ++offset;
            while (offset < text.size() && text[offset] != '}')
            {
                if (text[offset] == '{')
                {
                    return fail(
                        elementIndex,
                        offset,
                        "nested braces are not supported");
                }
                ++offset;
            }
            if (offset == text.size())
            {
                return fail(
                    elementIndex,
                    elementOffset,
                    "brace expression is missing its closing brace");
            }

            const auto source = trimAsciiSpace(text.substr(
                expressionOffset,
                offset - expressionOffset));
            if (source.empty())
            {
                return fail(
                    elementIndex,
                    elementOffset,
                    "brace expression must not be empty");
            }
            std::string evaluationError;
            value = evaluateExpression(
                source,
                domain,
                sampleRate,
                tempoBpm,
                evaluationError);
            if (!value.has_value())
            {
                return fail(
                    elementIndex,
                    elementOffset,
                    std::move(evaluationError));
            }
            ++offset;
            if (offset < text.size() && !isSeparator(text[offset]))
            {
                return fail(
                    elementIndex,
                    offset,
                    "brace expression must be followed by a separator or end of text");
            }
        }
        else
        {
            while (offset < text.size() && !isSeparator(text[offset]))
            {
                if (text[offset] == '{' || text[offset] == '}')
                {
                    return fail(
                        elementIndex,
                        offset,
                        "brace characters must delimit a complete array element");
                }
                ++offset;
            }
            const auto source = text.substr(
                elementOffset,
                offset - elementOffset);
            value = parseFiniteLiteral(source);
            if (!value.has_value())
            {
                return fail(
                    elementIndex,
                    elementOffset,
                    "literal array element must be finite numeric text");
            }
        }

        result.values.push_back(*value);
        ++elementIndex;
    }

    if (result.values.empty())
        return fail(0, 0, "array must not be empty");
    return result;
}
}
