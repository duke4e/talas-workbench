#include "talas/expr/Expression.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
constexpr std::uint64_t propertySeed = 0x4558505250524f50ULL;

[[nodiscard]] std::string caseContext(std::size_t caseIndex)
{
    return " seed=" + std::to_string(propertySeed)
        + " case=" + std::to_string(caseIndex);
}

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

[[nodiscard]] std::uint64_t nextRandom(std::uint64_t& state) noexcept
{
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

bool resolveValue(
    void*,
    std::string_view name,
    talas::expr::ExpressionValue& output) noexcept
{
    output = {
        static_cast<double>(name.size() + 1U),
        talas::expr::ValueDomain::Scalar,
    };
    return true;
}

[[nodiscard]] std::string generateExpression(
    std::uint64_t& state,
    std::size_t depth)
{
    constexpr auto leaves = std::array{
        std::string_view{"0"},
        std::string_view{"1.5"},
        std::string_view{"440 hz"},
        std::string_view{"100 ms"},
        std::string_view{"1 s"},
        std::string_view{"60 nn"},
        std::string_view{"-6 db"},
        std::string_view{"2 beats"},
        std::string_view{"48000 samples"},
        std::string_view{"!Event"},
        std::string_view{"value"},
    };
    if (depth == 0 || nextRandom(state) % 3U == 0)
    {
        return std::string{leaves[static_cast<std::size_t>(
            nextRandom(state) % leaves.size())]};
    }
    constexpr auto operations = std::array{'+', '-', '*', '/'};
    return "(" + generateExpression(state, depth - 1U) + " "
        + operations[static_cast<std::size_t>(
            nextRandom(state) % operations.size())]
        + " " + generateExpression(state, depth - 1U) + ")";
}

[[nodiscard]] std::string generateV2Expression(
    std::uint64_t& state,
    std::size_t depth)
{
    if (depth == 0 || nextRandom(state) % 4U == 0)
        return generateExpression(state, 0);

    switch (nextRandom(state) % 10U)
    {
    case 0:
        return "abs(" + generateV2Expression(state, depth - 1U) + ")";
    case 1:
        return "sqrt(abs(" + generateV2Expression(state, depth - 1U)
            + "))";
    case 2:
        return "clamp(" + generateV2Expression(state, depth - 1U)
            + ", " + generateV2Expression(state, depth - 1U)
            + ", " + generateV2Expression(state, depth - 1U) + ")";
    case 3:
        return "map(" + generateV2Expression(state, depth - 1U)
            + ", 0, 1, " + generateV2Expression(state, depth - 1U)
            + ", " + generateV2Expression(state, depth - 1U) + ")";
    case 4:
        return "if(" + generateV2Expression(state, depth - 1U)
            + " != 0, " + generateV2Expression(state, depth - 1U)
            + ", " + generateV2Expression(state, depth - 1U) + ")";
    case 5:
        return "(" + generateV2Expression(state, depth - 1U)
            + " < " + generateV2Expression(state, depth - 1U) + ")";
    case 6:
        return "[" + generateV2Expression(state, depth - 1U)
            + ", " + generateV2Expression(state, depth - 1U)
            + "][" + std::to_string(nextRandom(state) % 2U) + "]";
    case 7:
        return "[[" + generateV2Expression(state, depth - 1U)
            + ", " + generateV2Expression(state, depth - 1U)
            + "][1], " + generateV2Expression(state, depth - 1U)
            + "][0]";
    default:
        return generateExpression(state, depth);
    }
}

[[nodiscard]] bool verifyParsedExpression(
    std::string_view source,
    std::size_t caseIndex,
    talas::expr::ExpressionLanguageVersion languageVersion)
{
    const auto parsed = talas::expr::parse(source, languageVersion);
    if (!parsed.ok())
        return false;
    talas::expr::EvaluationContext context;
    context.sampleRate = 48000.0;
    context.tempoBpm = 120.0;
    context.resolveEvent = resolveValue;
    context.resolveVariable = resolveValue;
    context.resolveIdentifier = resolveValue;
    constexpr auto domains = std::array{
        talas::expr::ValueDomain::Scalar,
        talas::expr::ValueDomain::Hertz,
        talas::expr::ValueDomain::Seconds,
        talas::expr::ValueDomain::NoteNumber,
        talas::expr::ValueDomain::Samples,
    };
    for (const auto domain : domains)
    {
        const auto options = talas::expr::EvaluationOptions{domain};
        const auto first = talas::expr::evaluate(
            *parsed.expression, context, options);
        const auto second = talas::expr::evaluate(
            *parsed.expression, context, options);
        require(
            first.ok() == second.ok()
                && first.diagnostics.size() == second.diagnostics.size(),
            "expression determinism status failed"
                + caseContext(caseIndex));
        talas::expr::ExpressionValue fast;
        const auto fastOk = talas::expr::tryEvaluate(
            *parsed.expression, context, options, fast);
        require(
            fastOk == first.ok(),
            "expression fast evaluator disagreed"
                + caseContext(caseIndex));
        if (first.ok())
        {
            require(
                std::isfinite(first.value.value)
                    && first.value.domain == domain
                    && first.value.value == second.value.value
                    && fast.value == first.value.value
                    && fast.domain == first.value.domain,
                "expression value property failed"
                    + caseContext(caseIndex));
        }
    }
    return true;
}
}

int main()
{
    auto random = propertySeed;
    for (std::size_t caseIndex = 0; caseIndex < 512; ++caseIndex)
    {
        auto expression = generateExpression(random, 4);
        require(
            verifyParsedExpression(
                expression,
                caseIndex,
                talas::expr::ExpressionLanguageVersion::LegacyV1),
            "generated expression did not parse"
                + caseContext(caseIndex));
        if (!expression.empty())
        {
            const auto index = static_cast<std::size_t>(
                nextRandom(random) % expression.size());
            expression[index] = static_cast<char>(nextRandom(random));
            (void) verifyParsedExpression(
                expression,
                caseIndex,
                talas::expr::ExpressionLanguageVersion::LegacyV1);
        }
    }

    for (std::size_t caseIndex = 0; caseIndex < 512; ++caseIndex)
    {
        auto expression = generateV2Expression(random, 3);
        require(
            verifyParsedExpression(
                expression,
                caseIndex + 512U,
                talas::expr::ExpressionLanguageVersion::PureV2),
            "generated pure-v2 expression did not parse"
                + caseContext(caseIndex + 512U));
        if (!expression.empty())
        {
            const auto index = static_cast<std::size_t>(
                nextRandom(random) % expression.size());
            expression[index] = static_cast<char>(nextRandom(random));
            (void) verifyParsedExpression(
                expression,
                caseIndex + 512U,
                talas::expr::ExpressionLanguageVersion::PureV2);
        }
    }

    for (std::size_t caseIndex = 0; caseIndex < 256; ++caseIndex)
    {
        const auto decibels = static_cast<std::int64_t>(
            nextRandom(random) % 2401U) - 1200;
        const auto source = std::to_string(decibels) + " db";
        const auto parsed = talas::expr::parse(
            source,
            talas::expr::ExpressionLanguageVersion::VariablesV2);
        require(
            parsed.ok(),
            "generated decibel literal did not parse"
                + caseContext(caseIndex + 1024U));
        const auto evaluated = talas::expr::evaluate(
            *parsed.expression,
            {},
            {talas::expr::ValueDomain::Scalar});
        talas::expr::ExpressionValue fast;
        const auto expected = std::pow(
            10.0,
            static_cast<double>(decibels) / 20.0);
        require(
            evaluated.ok()
                && evaluated.value.value == expected
                && talas::expr::tryEvaluate(
                    *parsed.expression,
                    {},
                    {talas::expr::ValueDomain::Scalar},
                    fast)
                && fast == evaluated.value,
            "generated decibel conversion oracle failed"
                + caseContext(caseIndex + 1024U));
    }

    for (std::size_t caseIndex = 0; caseIndex < 512; ++caseIndex)
    {
        const auto first = static_cast<std::int64_t>(
            nextRandom(random) % 2001U) - 1000;
        const auto step = static_cast<std::int64_t>(
            nextRandom(random) % 201U) - 100;
        const auto count = static_cast<std::size_t>(
            nextRandom(random)
                % talas::expr::ExpressionLimits::maximumSequenceElements)
            + 1U;
        const auto index = static_cast<std::size_t>(
            nextRandom(random) % count);
        auto expression = "series(" + std::to_string(first) + ", "
            + std::to_string(step) + ", " + std::to_string(count)
            + ")[" + std::to_string(index) + "]";
        const auto parsed = talas::expr::parse(
            expression,
            talas::expr::ExpressionLanguageVersion::PureV3);
        require(
            parsed.ok(),
            "generated PureV3 series did not parse"
                + caseContext(caseIndex + 1024U));
        const auto evaluated = talas::expr::evaluate(
            *parsed.expression,
            {},
            {talas::expr::ValueDomain::Scalar});
        const auto expected = first
            + step * static_cast<std::int64_t>(index);
        require(
            evaluated.ok()
                && evaluated.value.value == static_cast<double>(expected),
            "generated PureV3 series oracle failed"
                + caseContext(caseIndex + 1024U));
        require(
            !talas::expr::parse(
                 expression,
                 talas::expr::ExpressionLanguageVersion::PureV2).ok(),
            "PureV2 unexpectedly accepted generated PureV3 syntax"
                + caseContext(caseIndex + 1024U));
        require(
            verifyParsedExpression(
                expression,
                caseIndex + 1024U,
                talas::expr::ExpressionLanguageVersion::PureV3),
            "generated PureV3 series failed evaluator parity"
                + caseContext(caseIndex + 1024U));

        const auto mutationIndex = static_cast<std::size_t>(
            nextRandom(random) % expression.size());
        expression[mutationIndex] = static_cast<char>(nextRandom(random));
        (void) verifyParsedExpression(
            expression,
            caseIndex + 1024U,
            talas::expr::ExpressionLanguageVersion::PureV3);
    }

    talas::expr::ExpressionDefinitionLibrary definitions;
    definitions.definitions = {
        {"affine",
         {{"value", talas::expr::ValueDomain::Scalar},
          {"scale", talas::expr::ValueDomain::Scalar},
          {"offset", talas::expr::ValueDomain::Scalar}},
         talas::expr::ValueDomain::Scalar,
         "value * scale + offset"},
        {"nested",
         {{"value", talas::expr::ValueDomain::Scalar},
          {"scale", talas::expr::ValueDomain::Scalar},
          {"offset", talas::expr::ValueDomain::Scalar}},
         talas::expr::ValueDomain::Scalar,
         "affine(value, scale, offset) + affine(value, scale, offset)"},
    };
    for (std::size_t caseIndex = 0; caseIndex < 256; ++caseIndex)
    {
        const auto value = static_cast<std::int64_t>(
            nextRandom(random) % 2001U) - 1000;
        const auto scale = static_cast<std::int64_t>(
            nextRandom(random) % 41U) - 20;
        const auto offset = static_cast<std::int64_t>(
            nextRandom(random) % 2001U) - 1000;
        const auto source = "nested(" + std::to_string(value) + ", "
            + std::to_string(scale) + ", " + std::to_string(offset)
            + ")";
        const auto parsed = talas::expr::parse(
            source,
            talas::expr::ExpressionLanguageVersion::PureV4,
            definitions);
        require(
            parsed.ok(),
            "generated PureV4 definition call did not parse"
                + caseContext(caseIndex + 1536U));
        const auto evaluated = talas::expr::evaluate(
            *parsed.expression,
            {},
            {talas::expr::ValueDomain::Scalar});
        const auto expected = 2 * (value * scale + offset);
        require(
            evaluated.ok()
                && evaluated.value.value == static_cast<double>(expected),
            "generated PureV4 definition oracle failed"
                + caseContext(caseIndex + 1536U));
        require(
            !talas::expr::parse(
                 source,
                 talas::expr::ExpressionLanguageVersion::PureV3).ok(),
            "PureV3 unexpectedly accepted a generated PureV4 call"
                + caseContext(caseIndex + 1536U));
    }

    for (std::size_t caseIndex = 0; caseIndex < 256; ++caseIndex)
    {
        const auto delta = static_cast<std::int64_t>(
            nextRandom(random) % 41U) - 20;
        const auto initial = static_cast<std::int64_t>(
            nextRandom(random) % 201U) - 100;
        const auto source = "accumulate(" + std::to_string(delta)
            + ", 0, " + std::to_string(initial) + ")";
        const auto parsed = talas::expr::parse(
            source,
            talas::expr::ExpressionLanguageVersion::SessionV1);
        require(
            parsed.ok() && parsed.expression->stateSlotCount() == 1,
            "generated SessionV1 accumulator did not compile"
                + caseContext(caseIndex + 1792U));
        talas::expr::ExpressionState state;
        require(
            talas::expr::prepareExpressionState(*parsed.expression, state),
            "generated SessionV1 state did not prepare"
                + caseContext(caseIndex + 1792U));
        const auto retainedInitial = state;
        auto expected = initial;
        const auto evaluations = static_cast<std::size_t>(
            nextRandom(random) % 32U) + 1U;
        for (std::size_t evaluationIndex = 0;
             evaluationIndex < evaluations;
             ++evaluationIndex)
        {
            expected += delta;
            const auto result = talas::expr::evaluateStateful(
                *parsed.expression,
                {},
                state,
                {talas::expr::ValueDomain::Scalar});
            require(
                result.ok()
                    && result.value.value
                        == static_cast<double>(expected),
                "generated SessionV1 accumulation oracle failed"
                    + caseContext(caseIndex + 1792U));
        }
        const auto retainedCommitted = state;
        auto independent = retainedInitial;
        const auto independentResult = talas::expr::evaluateStateful(
            *parsed.expression,
            {},
            independent,
            {talas::expr::ValueDomain::Scalar});
        require(
            independentResult.ok()
                && independentResult.value.value
                    == static_cast<double>(initial + delta)
                && state == retainedCommitted,
            "copied SessionV1 state should remain independently owned"
                + caseContext(caseIndex + 1792U));
        require(
            !talas::expr::evaluate(
                 *parsed.expression,
                 {},
                 {talas::expr::ValueDomain::Scalar}).ok()
                && !talas::expr::parse(
                     source,
                     talas::expr::ExpressionLanguageVersion::PureV4).ok(),
            "SessionV1 state syntax should stay closed to pure evaluators"
                + caseContext(caseIndex + 1792U));
    }

    for (std::size_t caseIndex = 0; caseIndex < 128; ++caseIndex)
    {
        const auto value = static_cast<std::uint64_t>(
            nextRandom(random) % 20000U) + 1U;
        const auto initial = static_cast<std::uint64_t>(
            nextRandom(random) % 20000U) + 1U;
        const auto usePrevious = (caseIndex & 1U) == 0U;
        const auto source = usePrevious
            ? "previous(" + std::to_string(value)
                + " hz, " + std::to_string(initial) + " hz)"
            : "sampleHold(" + std::to_string(value)
                + " hz, 1, " + std::to_string(initial) + " hz)";
        const auto parsed = talas::expr::parse(
            source,
            talas::expr::ExpressionLanguageVersion::SessionV1);
        require(
            parsed.ok() && parsed.expression->stateSlotCount() == 1,
            "generated SessionV1 non-scalar state operator did not compile"
                + caseContext(caseIndex + 2048U));
        talas::expr::ExpressionState state;
        require(talas::expr::prepareExpressionState(
                    *parsed.expression,
                    state),
            "generated SessionV1 non-scalar state did not prepare"
                + caseContext(caseIndex + 2048U));
        const auto first = talas::expr::evaluateStateful(
            *parsed.expression,
            {},
            state,
            {talas::expr::ValueDomain::Hertz});
        const auto second = talas::expr::evaluateStateful(
            *parsed.expression,
            {},
            state,
            {talas::expr::ValueDomain::Hertz});
        const auto expectedFirst = usePrevious ? initial : value;
        require(
            first.ok() && second.ok()
                && first.value.domain == talas::expr::ValueDomain::Hertz
                && second.value.domain == talas::expr::ValueDomain::Hertz
                && first.value.value
                    == static_cast<double>(expectedFirst)
                && second.value.value == static_cast<double>(value),
            "generated SessionV1 previous/sampleHold domain oracle failed"
                + caseContext(caseIndex + 2048U));
    }

    for (std::size_t caseIndex = 0; caseIndex < 256; ++caseIndex)
    {
        const auto variable = "v" + std::to_string(
            nextRandom(random) % 4096U);
        const auto source = "@" + variable + " + @" + variable;
        require(
            verifyParsedExpression(
                source,
                caseIndex + 2304U,
                talas::expr::ExpressionLanguageVersion::VariablesV1),
            "generated VariablesV1 reference expression did not parse"
                + caseContext(caseIndex + 2304U));
        require(
            !talas::expr::parse(
                 source,
                 talas::expr::ExpressionLanguageVersion::SessionV1).ok(),
            "SessionV1 unexpectedly accepted generated variable syntax"
                + caseContext(caseIndex + 2304U));
    }

    for (std::size_t caseIndex = 0; caseIndex < 512; ++caseIndex)
    {
        const auto left = static_cast<std::int64_t>(
            nextRandom(random) % 200001U) - 100000;
        auto right = static_cast<std::int64_t>(
            nextRandom(random) % 2001U) - 1000;
        if (right == 0)
            right = 1;
        const auto source = std::to_string(left) + " rem: "
            + std::to_string(right);
        const auto parsed = talas::expr::parse(
            source,
            talas::expr::ExpressionLanguageVersion::VariablesV2);
        require(
            parsed.ok(),
            "generated VariablesV2 remainder did not parse"
                + caseContext(caseIndex + 2560U));
        const auto evaluated = talas::expr::evaluate(
            *parsed.expression,
            {},
            {talas::expr::ValueDomain::Scalar});
        auto expected = std::fmod(
            static_cast<double>(left),
            static_cast<double>(right));
        if (expected == 0.0)
            expected = 0.0;
        talas::expr::ExpressionValue prepared;
        require(
            evaluated.ok()
                && evaluated.value.value == expected
                && !std::signbit(evaluated.value.value)
                    == !std::signbit(expected)
                && talas::expr::tryEvaluate(
                    *parsed.expression,
                    {},
                    {talas::expr::ValueDomain::Scalar},
                    prepared)
                && prepared == evaluated.value,
            "generated VariablesV2 remainder oracle failed"
                + caseContext(caseIndex + 2560U));
        require(
            !talas::expr::parse(
                 source,
                 talas::expr::ExpressionLanguageVersion::VariablesV1).ok(),
            "VariablesV1 unexpectedly accepted generated remainder syntax"
                + caseContext(caseIndex + 2560U));
    }
}
