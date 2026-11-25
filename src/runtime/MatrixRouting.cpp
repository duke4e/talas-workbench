#include "MatrixRouting.h"

#include "CheckedStaticExpressionArray.h"
#include "IntervalCollectArray.h"

#include <cmath>
#include <string>

namespace talas::runtime
{
namespace
{
// The text of each level, braces kept, or nullopt for a malformed row.
[[nodiscard]] std::optional<std::vector<std::string_view>> splitMatrixRow(std::string_view text)
{
    namespace array = checked_static_expression_array;
    std::vector<std::string_view> elements;
    std::size_t offset = 0;
    while (offset < text.size())
    {
        while (offset < text.size() && array::isSeparator(text[offset]))
            ++offset;
        if (offset == text.size())
            break;
        const auto start = offset;
        if (text[offset] == '{')
        {
            offset = text.find('}', offset);
            if (offset == std::string_view::npos)
                return std::nullopt;
            ++offset;
            if (offset < text.size() && !array::isSeparator(text[offset]))
                return std::nullopt;
        }
        else
        {
            while (offset < text.size() && !array::isSeparator(text[offset]))
                ++offset;
        }
        elements.push_back(text.substr(start, offset - start));
    }
    return elements;
}
}

// A row lists one mixing level per input; an expression, or a level with
// units, sits within curly braces, and an expression may follow Event
// Values, as in {!Val1 * !KeyVelocity}.
MatrixRowParseResult parseMatrixRowGains(
    std::string_view text,
    std::size_t expectedCount,
    double sampleRate,
    double tempoBpm,
    std::span<const std::optional<double>> liveGains)
{
    MatrixRowParseResult result;
    const auto elements = splitMatrixRow(text);
    if (!elements.has_value() || elements->empty())
    {
        result.error = MatrixRowParseError::NonNumeric;
        return result;
    }
    for (std::size_t index = 0; index < elements->size(); ++index)
    {
        if (index < liveGains.size() && liveGains[index].has_value())
        {
            result.gains.push_back(*liveGains[index]);
            continue;
        }
        const auto parsed = checked_static_expression_array::parse(
            (*elements)[index],
            checked_static_expression_array::Domain::Scalar,
            sampleRate,
            tempoBpm);
        if (!parsed.ok() || parsed.values.size() != 1)
        {
            result.error = MatrixRowParseError::NonNumeric;
            result.gains.clear();
            return result;
        }
        result.gains.push_back(parsed.values.front());
    }
    for (const auto gain : result.gains)
    {
        if (!std::isfinite(gain))
        {
            result.error = MatrixRowParseError::NonFinite;
            result.gains.clear();
            return result;
        }
    }
    if (result.gains.size() != expectedCount)
    {
        result.error = MatrixRowParseError::WrongCount;
        result.gains.clear();
        return result;
    }
    return result;
}

std::optional<std::vector<MatrixRowExpression>> matrixRowExpressions(std::string_view text)
{
    if (const auto collected = interval_collect_array::expand(text);
        collected.status == interval_collect_array::Status::Expanded)
    {
        std::vector<MatrixRowExpression> expressions;
        for (std::size_t index = 0; index < collected.elements.size(); ++index)
            expressions.push_back({index, collected.elements[index]});
        return expressions;
    }
    const auto elements = splitMatrixRow(text);
    if (!elements.has_value())
        return std::nullopt;
    std::vector<MatrixRowExpression> expressions;
    for (std::size_t index = 0; index < elements->size(); ++index)
    {
        const auto element = (*elements)[index];
        if (element.size() >= 2 && element.front() == '{' && element.back() == '}')
        {
            expressions.push_back({index,
                std::string{checked_static_expression_array::trimAsciiSpace(element.substr(1, element.size() - 2))}});
        }
        else if (element.size() >= 2 && element.front() == '!')
        {
            expressions.push_back({index, std::string{element}});
        }
    }
    return expressions;
}

std::string matrixLiveGainId(std::string_view rowId, std::size_t index)
{
    return std::string(rowId) + "#" + std::to_string(index + 1);
}
}
