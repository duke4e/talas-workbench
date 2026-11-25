#include "talas/expr/Expression.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace
{
[[noreturn]] void invariantFailure() noexcept
{
    __builtin_trap();
}

bool resolveStableValue(
    void*,
    std::string_view name,
    talas::expr::ExpressionValue& output) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto byte : name)
    {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    output = {
        static_cast<double>(hash % 4096U) / 128.0,
        talas::expr::ValueDomain::Scalar,
    };
    return true;
}

bool sameDiagnostics(
    const std::vector<talas::expr::Diagnostic>& left,
    const std::vector<talas::expr::Diagnostic>& right) noexcept
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (left[index].code != right[index].code
            || left[index].range.offset != right[index].range.offset
            || left[index].range.length != right[index].range.length
            || left[index].message != right[index].message)
        {
            return false;
        }
    }
    return true;
}

void verifyDomain(
    const talas::expr::Expression& expression,
    const talas::expr::EvaluationContext& context,
    talas::expr::ValueDomain domain)
{
    const auto options = talas::expr::EvaluationOptions{domain};
    const auto first = talas::expr::evaluate(expression, context, options);
    const auto second = talas::expr::evaluate(expression, context, options);
    if (first.ok() != second.ok()
        || !sameDiagnostics(first.diagnostics, second.diagnostics))
    {
        invariantFailure();
    }

    talas::expr::ExpressionValue firstFast;
    talas::expr::ExpressionValue secondFast;
    const auto firstFastOk = talas::expr::tryEvaluate(
        expression, context, options, firstFast);
    const auto secondFastOk = talas::expr::tryEvaluate(
        expression, context, options, secondFast);
    if (firstFastOk != secondFastOk || firstFastOk != first.ok())
        invariantFailure();

    if (!first.ok())
        return;
    if (!std::isfinite(first.value.value)
        || first.value.domain != domain
        || first.value.value != second.value.value
        || first.value.domain != second.value.domain
        || firstFast.value != first.value.value
        || firstFast.domain != first.value.domain
        || secondFast.value != firstFast.value
        || secondFast.domain != firstFast.domain)
    {
        invariantFailure();
    }
}

void verifyStatefulDomain(
    const talas::expr::Expression& expression,
    const talas::expr::EvaluationContext& context,
    talas::expr::ValueDomain domain)
{
    talas::expr::ExpressionState initial;
    talas::expr::ExpressionStateScratch diagnosticScratch;
    talas::expr::ExpressionStateScratch diagnosticTwinScratch;
    talas::expr::ExpressionStateScratch fastScratch;
    if (!talas::expr::prepareExpressionState(expression, initial)
        || !talas::expr::prepareExpressionStateScratch(
            expression,
            diagnosticScratch)
        || !talas::expr::prepareExpressionStateScratch(
            expression,
            diagnosticTwinScratch)
        || !talas::expr::prepareExpressionStateScratch(
            expression,
            fastScratch))
    {
        invariantFailure();
    }
    auto diagnosticState = initial;
    auto diagnosticTwinState = initial;
    auto fastState = initial;
    const auto options = talas::expr::EvaluationOptions{domain};
    for (std::size_t step = 0; step < 2; ++step)
    {
        const auto stateBefore = diagnosticState;
        const auto diagnostic = talas::expr::evaluateStateful(
            expression,
            context,
            diagnosticState,
            diagnosticScratch,
            options);
        const auto diagnosticTwin = talas::expr::evaluateStateful(
            expression,
            context,
            diagnosticTwinState,
            diagnosticTwinScratch,
            options);
        talas::expr::ExpressionValue fast;
        const auto fastOk = talas::expr::tryEvaluateStateful(
            expression,
            context,
            fastState,
            fastScratch,
            options,
            fast);
        if (diagnostic.ok() != diagnosticTwin.ok()
            || diagnostic.ok() != fastOk
            || !sameDiagnostics(
                diagnostic.diagnostics,
                diagnosticTwin.diagnostics)
            || diagnosticState != diagnosticTwinState
            || diagnosticState != fastState)
        {
            invariantFailure();
        }
        if (!diagnostic.ok())
        {
            if (diagnostic.diagnostics.empty()
                || diagnosticState != stateBefore)
            {
                invariantFailure();
            }
            continue;
        }
        if (!std::isfinite(diagnostic.value.value)
            || diagnostic.value.domain != domain
            || diagnosticTwin.value != diagnostic.value
            || fast.value != diagnostic.value.value
            || fast.domain != diagnostic.value.domain)
        {
            invariantFailure();
        }
    }
}
}

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size)
{
    if (size > talas::expr::ExpressionLimits::maximumSourceBytes)
        return 0;

    const auto source = std::string_view{
        reinterpret_cast<const char*>(data), size};
    talas::expr::EvaluationContext context;
    context.sampleRate = 48000.0;
    context.tempoBpm = 120.0;
    context.resolveEvent = resolveStableValue;
    context.resolveVariable = resolveStableValue;
    context.resolveIdentifier = resolveStableValue;
    constexpr auto domains = std::array{
        talas::expr::ValueDomain::Scalar,
        talas::expr::ValueDomain::Hertz,
        talas::expr::ValueDomain::Seconds,
        talas::expr::ValueDomain::NoteNumber,
        talas::expr::ValueDomain::Samples,
    };
    constexpr auto languageVersions = std::array{
        talas::expr::ExpressionLanguageVersion::LegacyV1,
        talas::expr::ExpressionLanguageVersion::PureV2,
        talas::expr::ExpressionLanguageVersion::PureV3,
        talas::expr::ExpressionLanguageVersion::PureV4,
        talas::expr::ExpressionLanguageVersion::SessionV1,
        talas::expr::ExpressionLanguageVersion::VariablesV1,
        talas::expr::ExpressionLanguageVersion::VariablesV2,
    };
    for (const auto languageVersion : languageVersions)
    {
        const auto parsed = talas::expr::parse(source, languageVersion);
        if (!parsed.ok())
            continue;
        for (const auto domain : domains)
        {
            if (parsed.expression->isStateful())
                verifyStatefulDomain(*parsed.expression, context, domain);
            else
                verifyDomain(*parsed.expression, context, domain);
        }
    }
    static const auto definitions = [] {
        talas::expr::ExpressionDefinitionLibrary library;
        library.definitions = {
            {"twice",
             {{"value", talas::expr::ValueDomain::Scalar}},
             talas::expr::ValueDomain::Scalar,
             "value + value"},
            {"bounded",
             {{"value", talas::expr::ValueDomain::Scalar}},
             talas::expr::ValueDomain::Scalar,
             "clamp(twice(value), -4096, 4096)"},
        };
        return library;
    }();
    const auto defined = talas::expr::parse(
        source,
        talas::expr::ExpressionLanguageVersion::PureV4,
        definitions);
    if (defined.ok())
    {
        for (const auto domain : domains)
            verifyDomain(*defined.expression, context, domain);
    }
    const auto sessionDefined = talas::expr::parse(
        source,
        talas::expr::ExpressionLanguageVersion::SessionV1,
        definitions);
    if (sessionDefined.ok())
    {
        for (const auto domain : domains)
        {
            if (sessionDefined.expression->isStateful())
            {
                verifyStatefulDomain(
                    *sessionDefined.expression,
                    context,
                    domain);
            }
            else
            {
                verifyDomain(
                    *sessionDefined.expression,
                    context,
                    domain);
            }
        }
    }
    const auto variableDefined = talas::expr::parse(
        source,
        talas::expr::ExpressionLanguageVersion::VariablesV1,
        definitions);
    if (variableDefined.ok())
    {
        for (const auto domain : domains)
        {
            if (variableDefined.expression->isStateful())
            {
                verifyStatefulDomain(
                    *variableDefined.expression,
                    context,
                    domain);
            }
            else
            {
                verifyDomain(
                    *variableDefined.expression,
                    context,
                    domain);
            }
        }
    }
    static const auto remainderDefinitions = [] {
        talas::expr::ExpressionDefinitionLibrary library;
        library.definitions = {
            {"wrap",
             {{"value", talas::expr::ValueDomain::Scalar}},
             talas::expr::ValueDomain::Scalar,
             "value rem: 3"},
        };
        return library;
    }();
    const auto remainderDefined = talas::expr::parse(
        source,
        talas::expr::ExpressionLanguageVersion::VariablesV2,
        remainderDefinitions);
    if (remainderDefined.ok())
    {
        for (const auto domain : domains)
        {
            if (remainderDefined.expression->isStateful())
            {
                verifyStatefulDomain(
                    *remainderDefined.expression,
                    context,
                    domain);
            }
            else
            {
                verifyDomain(
                    *remainderDefined.expression,
                    context,
                    domain);
            }
        }
    }
    return 0;
}
