#include "talas/expr/Expression.h"

#include <clocale>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

namespace
{
using talas::expr::Diagnostic;
using talas::expr::EvaluationContext;
using talas::expr::EvaluationOptions;
using talas::expr::ExpressionLanguageVersion;
using talas::expr::ExpressionValue;
using talas::expr::SourceRange;
using talas::expr::ValueDomain;

class NumericLocaleGuard
{
public:
    NumericLocaleGuard()
    {
        if (const auto* current = std::setlocale(LC_NUMERIC, nullptr))
            previous_ = current;
    }

    ~NumericLocaleGuard()
    {
        if (!previous_.empty())
            std::setlocale(LC_NUMERIC, previous_.c_str());
    }

    bool setDecimalCommaLocale()
    {
        const char* candidates[] = {
            "de_DE.UTF-8",
            "fr_FR.UTF-8",
            "hr_HR.UTF-8",
            "de_DE",
            "fr_FR",
            "hr_HR",
        };

        for (const auto* candidate : candidates)
        {
            if (std::setlocale(LC_NUMERIC, candidate) == nullptr)
                continue;

            const auto* decimalPoint = std::localeconv()->decimal_point;
            if (decimalPoint != nullptr && std::string_view(decimalPoint) == ",")
                return true;
        }

        return false;
    }

private:
    std::string previous_;
};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(double actual, double expected, double tolerance, const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

const talas::expr::Expression& requireExpression(const talas::expr::ParseResult& result, const std::string& source)
{
    require(result.ok(), source + " should parse without diagnostics");
    return *result.expression;
}

ExpressionValue evaluateOk(std::string_view source, ValueDomain expectedDomain, EvaluationContext context = {})
{
    const auto parseResult = talas::expr::parse(source);
    const auto& expression = requireExpression(parseResult, std::string(source));
    const auto evaluation = talas::expr::evaluate(expression, context, EvaluationOptions{expectedDomain});
    require(evaluation.ok(), std::string(source) + " should evaluate without diagnostics");
    require(evaluation.value.domain == expectedDomain, std::string(source) + " should evaluate to the expected domain");
    return evaluation.value;
}

ExpressionValue evaluateV2Ok(
    std::string_view source,
    ValueDomain expectedDomain,
    EvaluationContext context = {})
{
    const auto parseResult = talas::expr::parse(
        source,
        ExpressionLanguageVersion::PureV2);
    const auto& expression = requireExpression(
        parseResult,
        std::string(source));
    const auto evaluation = talas::expr::evaluate(
        expression,
        context,
        EvaluationOptions{expectedDomain});
    require(
        evaluation.ok(),
        std::string(source) + " should evaluate without diagnostics");
    require(
        evaluation.value.domain == expectedDomain,
        std::string(source) + " should evaluate to the expected domain");
    return evaluation.value;
}

ExpressionValue evaluateV3Ok(
    std::string_view source,
    ValueDomain expectedDomain,
    EvaluationContext context = {})
{
    const auto parseResult = talas::expr::parse(
        source,
        ExpressionLanguageVersion::PureV3);
    const auto& expression = requireExpression(
        parseResult,
        std::string(source));
    const auto evaluation = talas::expr::evaluate(
        expression,
        context,
        EvaluationOptions{expectedDomain});
    require(
        evaluation.ok(),
        std::string(source) + " should evaluate without diagnostics");
    require(
        evaluation.value.domain == expectedDomain,
        std::string(source) + " should evaluate to the expected domain");
    return evaluation.value;
}

bool hasDiagnostic(const std::vector<Diagnostic>& diagnostics, Diagnostic::Code code)
{
    for (const auto& diagnostic : diagnostics)
    {
        if (diagnostic.code == code)
            return true;
    }

    return false;
}

bool resolveEvent(void*, std::string_view name, ExpressionValue& out) noexcept
{
    if (name == "Pitch")
    {
        out = {69.0, ValueDomain::NoteNumber};
        return true;
    }
    if (name == "KeyDown")
    {
        out = {1.0, ValueDomain::Scalar};
        return true;
    }
    if (name == "Fader")
    {
        out = {0.5, ValueDomain::Scalar};
        return true;
    }

    return false;
}

bool resolveIdentifier(void*, std::string_view name, ExpressionValue& out) noexcept
{
    if (name == "pi")
    {
        out = {3.14159265358979323846, ValueDomain::Scalar};
        return true;
    }

    return false;
}

struct VariableResolverState
{
    double gain = 0.25;
    double step = 1.0;
};

bool resolveVariable(void* user, std::string_view name, ExpressionValue& out) noexcept
{
    auto& state = *static_cast<VariableResolverState*>(user);
    if (name == "gain")
    {
        out = {state.gain, ValueDomain::Scalar};
        return true;
    }
    if (name == "step")
    {
        out = {state.step, ValueDomain::Scalar};
        return true;
    }
    return false;
}

bool resolveNonFiniteEvent(void*, std::string_view name, ExpressionValue& out) noexcept
{
    if (name == "Bad")
    {
        out = {std::numeric_limits<double>::infinity(), ValueDomain::Scalar};
        return true;
    }

    return false;
}

void testLiteralsParseAndEvaluate()
{
    requireNear(evaluateOk("440", ValueDomain::Scalar).value, 440.0, 1.0e-12, "plain number should evaluate as scalar");
    requireNear(evaluateOk("440 hz", ValueDomain::Hertz).value, 440.0, 1.0e-12, "space-separated hertz should evaluate");
    requireNear(evaluateOk("440hz", ValueDomain::Hertz).value, 440.0, 1.0e-12, "suffix hertz should evaluate");
    require(evaluateOk("11.71875 hz", ValueDomain::Hertz).value == 11.71875,
            "exactly representable decimal literals should be correctly rounded");
    requireNear(evaluateOk("100 ms", ValueDomain::Seconds).value, 0.1, 1.0e-12, "milliseconds should evaluate as seconds");
    requireNear(evaluateOk("1 s", ValueDomain::Seconds).value, 1.0, 1.0e-12, "seconds should evaluate");
    requireNear(evaluateOk("60 nn", ValueDomain::NoteNumber).value, 60.0, 1.0e-12, "note number should evaluate");
    requireNear(evaluateOk("2 beats", ValueDomain::Seconds).value, 1.0, 1.0e-12, "beats should evaluate using tempo");
    requireNear(evaluateOk("48000 samples", ValueDomain::Seconds).value, 1.0, 1.0e-12, "samples should evaluate using sample rate");
    requireNear(evaluateOk("0 db", ValueDomain::Scalar).value, 1.0, 1.0e-12, "zero decibels should evaluate as unity linear gain");
    requireNear(
        evaluateOk("-6 db", ValueDomain::Scalar).value,
        std::pow(10.0, -6.0 / 20.0),
        1.0e-12,
        "negative decibels should evaluate as a linear amplitude ratio");
    requireNear(
        evaluateOk("6 db", ValueDomain::Scalar).value,
        std::pow(10.0, 6.0 / 20.0),
        1.0e-12,
        "positive decibels should evaluate as a linear amplitude ratio");

    const auto compiledDb = talas::expr::parse(
        "-6 db",
        ExpressionLanguageVersion::VariablesV2);
    const auto& compiledDbExpression = requireExpression(
        compiledDb,
        "VariablesV2 decibel literal");
    ExpressionValue compiledDbValue;
    require(
        talas::expr::tryEvaluate(
            compiledDbExpression,
            {},
            EvaluationOptions{ValueDomain::Scalar},
            compiledDbValue),
        "fast VariablesV2 evaluator should admit a finite decibel literal");
    requireNear(
        compiledDbValue.value,
        std::pow(10.0, -6.0 / 20.0),
        1.0e-12,
        "fast VariablesV2 decibel conversion should match the diagnostic evaluator");
}

void testParenthesizedDecibelNegationNegatesTheLinearValue()
{
    const auto gain = std::pow(10.0, 6.0 / 20.0);
    requireNear(evaluateOk("-(6 db)", ValueDomain::Scalar).value,
        -gain, 1.0e-12,
        "a minus outside parentheses should negate the linear gain");
    requireNear(evaluateOk("-(-6 db)", ValueDomain::Scalar).value,
        -1.0 / gain, 1.0e-12,
        "only the inner minus belongs to the decibel literal");
    requireNear(evaluateOk("-6 db", ValueDomain::Scalar).value,
        1.0 / gain, 1.0e-12,
        "a minus written on the literal remains negative decibels");
    const auto compiled = talas::expr::parse(
        "-(6 db)", ExpressionLanguageVersion::VariablesV2);
    ExpressionValue compiledValue;
    require(talas::expr::tryEvaluate(
                requireExpression(compiled, "VariablesV2 -(6 db)"),
                {},
                EvaluationOptions{ValueDomain::Scalar},
                compiledValue)
            && std::abs(compiledValue.value + gain) < 1.0e-12,
        "the compiled evaluator should negate the linear gain too");
}

void testPrecedence()
{
    requireNear(evaluateOk("1 + 2 * 3", ValueDomain::Scalar).value, 7.0, 1.0e-12, "multiplication should bind tighter than addition");
    requireNear(evaluateOk("(1 + 2) * 3", ValueDomain::Scalar).value, 9.0, 1.0e-12, "parentheses should override precedence");
}

void testUnaryOperators()
{
    requireNear(evaluateOk("-440 hz", ValueDomain::Hertz).value, -440.0, 1.0e-12, "unary negate should apply to unit literal");
    requireNear(evaluateOk("1--2", ValueDomain::Scalar).value, 3.0, 1.0e-12, "binary minus followed by unary minus should parse");
    requireNear(evaluateOk("-(1 + 2)", ValueDomain::Scalar).value, -3.0, 1.0e-12, "unary negate should apply to grouped expressions");
}

void testReferencesEvaluateThroughResolvers()
{
    EvaluationContext context;
    context.resolveEvent = resolveEvent;
    context.resolveIdentifier = resolveIdentifier;

    requireNear(evaluateOk("!Pitch", ValueDomain::NoteNumber, context).value, 69.0, 1.0e-12, "event note number should resolve");
    requireNear(evaluateOk("!KeyDown", ValueDomain::Scalar, context).value, 1.0, 1.0e-12, "event scalar should resolve");
    requireNear(evaluateOk("!Fader * 12 nn + 60 nn", ValueDomain::NoteNumber, context).value, 66.0, 1.0e-12, "event expression should combine scalar and note-number values");
    requireNear(evaluateOk("pi * 2", ValueDomain::Scalar, context).value, 6.283185307179586, 1.0e-12, "identifier should resolve");
}

void testReferenceExtractionIsStableAndDeduped()
{
    const auto parseResult = talas::expr::parse("!Fader + !Fader + !Pitch + pi + pi");
    const auto& expression = requireExpression(parseResult, "reference extraction");

    const std::vector<std::string> expectedEvents = {"Fader", "Pitch"};
    const std::vector<std::string> expectedIdentifiers = {"pi"};
    require(expression.eventReferences() == expectedEvents, "event references should be deduped in first-seen order");
    require(expression.identifiers() == expectedIdentifiers, "identifiers should be deduped in first-seen order");
}

void testUnitConversions()
{
    EvaluationContext context;
    context.sampleRate = 48000.0;
    context.tempoBpm = 120.0;

    requireNear(evaluateOk("100 ms", ValueDomain::Seconds, context).value, 0.1, 1.0e-12, "100 ms should be 0.1 seconds");
    requireNear(evaluateOk("2 beats", ValueDomain::Seconds, context).value, 1.0, 1.0e-12, "2 beats at 120 BPM should be 1 second");
    requireNear(evaluateOk("48000 samples", ValueDomain::Seconds, context).value, 1.0, 1.0e-12, "48000 samples at 48000 Hz should be 1 second");
    requireNear(evaluateOk("1 s + 48000 samples", ValueDomain::Seconds, context).value, 2.0, 1.0e-12, "seconds and samples should combine as seconds");
    requireNear(evaluateOk("1 s + 2 beats", ValueDomain::Seconds, context).value, 2.0, 1.0e-12, "seconds and beats should combine as seconds");
    requireNear(evaluateOk("1 / 0.002272727 s", ValueDomain::Hertz, context).value, 440.0000528, 1.0e-3, "reciprocal seconds should produce hertz");
    constexpr std::array historicTimeForms{
        std::pair{std::string_view{"200 usec"}, 0.0002},
        std::pair{std::string_view{"2 m"}, 120.0},
        std::pair{std::string_view{"2 h"}, 7200.0},
        std::pair{std::string_view{"2 days"}, 172800.0},
        std::pair{std::string_view{"2 samp"}, 2.0 / 48000.0},
        std::pair{std::string_view{"1 / 2 hz"}, 0.5},
    };
    for (const auto& [source, seconds] : historicTimeForms)
    {
        requireNear(evaluateOk(source, ValueDomain::Seconds, context).value,
            seconds, 1.0e-9,
            "historic time form should evaluate in legacy expressions");
        requireNear(evaluateV2Ok(source, ValueDomain::Seconds, context).value,
            seconds, 1.0e-9,
            "historic time form should evaluate in compiled expressions");
    }
    requireNear(evaluateOk("69 nn", ValueDomain::Hertz, context).value, 440.0, 1.0e-9, "note number should convert to hertz only when hertz is expected");
    requireNear(evaluateOk("1.5", ValueDomain::Scalar, context).value, 1.5, 1.0e-12, "decimal point parsing should be locale-independent");
}

// In Val an octave followed by
// a pitch name is a note number, and sharp or flat moves it a half step.
void testTalasPitchNotation()
{
    EvaluationContext context;
    context.sampleRate = 48000.0;
    constexpr std::array pitches{
        std::pair{std::string_view{"4 c"}, 60.0},
        std::pair{std::string_view{"4 d"}, 62.0},
        std::pair{std::string_view{"4 re"}, 62.0},
        std::pair{std::string_view{"4 c sharp"}, 61.0},
        std::pair{std::string_view{"3 b"}, 59.0},
        std::pair{std::string_view{"4 ti"}, 71.0},
        std::pair{std::string_view{"4 so"}, 67.0},
        std::pair{std::string_view{"4 a"}, 69.0},
        std::pair{std::string_view{"4 a flat"}, 68.0},
        std::pair{std::string_view{"4 c + 9 nn"}, 69.0},
    };
    for (const auto& [source, noteNumber] : pitches)
    {
        requireNear(evaluateOk(source, ValueDomain::NoteNumber, context).value,
            noteNumber, 1.0e-12,
            "octave-and-letter pitch notation should evaluate in legacy expressions");
        requireNear(evaluateV2Ok(source, ValueDomain::NoteNumber, context).value,
            noteNumber, 1.0e-12,
            "octave-and-letter pitch notation should evaluate in compiled expressions");
    }
    requireNear(evaluateOk("4 a flat", ValueDomain::Hertz, context).value,
        415.3046975799451, 1.0e-9,
        "an octave-and-letter pitch should convert to hertz");
}

// The keyword `default` names a field's natural value, such as a Sample's
// recorded pitch; preparation binds it to a literal.
void testDefaultKeywordBindsToALiteral()
{
    for (const auto language : {ExpressionLanguageVersion::LegacyV1,
             ExpressionLanguageVersion::VariablesV2})
    {
        const auto parsed = talas::expr::parse("default * 2 + Other", language);
        require(parsed.ok(), "an expression with default should parse");
        const auto bound = parsed.expression->specializeIdentifier(
            "default", {440.0, ValueDomain::Hertz});
        require(bound.has_value()
                && bound->identifiers() == std::vector<std::string>{"Other"}
                && parsed.expression->identifiers().size() == 2,
            "binding default should leave other identifiers and the source unchanged");
        EvaluationContext context;
        context.resolveIdentifier = [](void*, std::string_view name,
                                        ExpressionValue& value) noexcept {
            value = {10.0, ValueDomain::Hertz};
            return name == "Other";
        };
        const auto value = talas::expr::evaluate(*bound, context, {ValueDomain::Hertz});
        require(value.ok() && value.value.value == 890.0,
            "a bound default should evaluate as the literal it names");
        const auto unchanged = talas::expr::parse("3 hz", language)
                                   .expression->specializeIdentifier(
                                       "default", {440.0, ValueDomain::Hertz});
        require(unchanged.has_value()
                && talas::expr::evaluate(*unchanged, {}, {ValueDomain::Hertz}).value.value == 3.0,
            "an expression without default should be unchanged");
    }
}

void testDecimalParsingIgnoresNumericLocale()
{
    NumericLocaleGuard localeGuard;
    require(localeGuard.setDecimalCommaLocale(), "test environment should provide a decimal-comma LC_NUMERIC locale");

    requireNear(evaluateOk("1.5", ValueDomain::Scalar).value, 1.5, 1.0e-12, "dot decimal should parse under decimal-comma LC_NUMERIC");
    requireNear(
        evaluateOk("-6.5 db", ValueDomain::Scalar).value,
        std::pow(10.0, -6.5 / 20.0),
        1.0e-12,
        "decibel conversion should remain locale-independent");

    const auto commaDecimal = talas::expr::parse("1,5");
    require(!commaDecimal.ok(), "comma decimal should not parse as a valid expression under decimal-comma LC_NUMERIC");
    require(!commaDecimal.expression.has_value(), "comma decimal should not produce an expression");
    require(hasDiagnostic(commaDecimal.diagnostics, Diagnostic::Code::UnexpectedCharacter), "comma decimal should report unexpected comma");
}

void testParseErrors()
{
    const std::vector<std::pair<std::string_view, Diagnostic::Code>> cases = {
        {"", Diagnostic::Code::EmptyExpression},
        {"1 +", Diagnostic::Code::ExpectedExpression},
        {"(1 + 2", Diagnostic::Code::ExpectedRightParen},
        {"440 frogs", Diagnostic::Code::UnknownUnit},
        {"!", Diagnostic::Code::ExpectedExpression},
        {"!123", Diagnostic::Code::ExpectedExpression},
        {"1 2", Diagnostic::Code::TrailingInput},
        {"1 @ 2", Diagnostic::Code::UnexpectedCharacter},
        {"1,5", Diagnostic::Code::UnexpectedCharacter},
    };

    for (const auto& [source, code] : cases)
    {
        const auto result = talas::expr::parse(source);
        require(!result.ok(), std::string(source) + " should fail to parse");
        require(hasDiagnostic(result.diagnostics, code), std::string(source) + " should report expected diagnostic code");
    }

    const auto functionCall = talas::expr::parse("pi(2)");
    require(!functionCall.ok(), "function calls should not parse in this slice");
    require(hasDiagnostic(functionCall.diagnostics, Diagnostic::Code::UnexpectedToken), "function calls should report unexpected token");
}

void testEvaluationErrors()
{
    {
        const auto parseResult = talas::expr::parse("1 / 0");
        const auto& expression = requireExpression(parseResult, "1 / 0");
        const auto evaluation = talas::expr::evaluate(expression, {}, EvaluationOptions{ValueDomain::Scalar});
        require(!evaluation.ok(), "division by zero should fail evaluation");
        require(hasDiagnostic(evaluation.diagnostics, Diagnostic::Code::DivisionByZero), "division by zero should report diagnostic");
    }

    {
        const auto parseResult = talas::expr::parse("1 s + 2 hz");
        const auto& expression = requireExpression(parseResult, "1 s + 2 hz");
        const auto evaluation = talas::expr::evaluate(expression, {}, EvaluationOptions{ValueDomain::Seconds});
        require(!evaluation.ok(), "seconds plus hertz should fail evaluation");
        require(hasDiagnostic(evaluation.diagnostics, Diagnostic::Code::UnitMismatch), "unit mismatch should report diagnostic");
    }

    {
        const auto parseResult = talas::expr::parse("!Pitch");
        const auto& expression = requireExpression(parseResult, "!Pitch");
        const auto evaluation = talas::expr::evaluate(expression, {}, EvaluationOptions{ValueDomain::NoteNumber});
        require(!evaluation.ok(), "event reference without resolver should fail evaluation");
        require(hasDiagnostic(evaluation.diagnostics, Diagnostic::Code::UnknownEvent), "missing event resolver should report unknown event");
    }

    {
        EvaluationContext context;
        context.resolveEvent = resolveEvent;
        const auto parseResult = talas::expr::parse("!Missing");
        const auto& expression = requireExpression(parseResult, "!Missing");
        const auto evaluation = talas::expr::evaluate(expression, context, EvaluationOptions{ValueDomain::Scalar});
        require(!evaluation.ok(), "event reference missed by resolver should fail evaluation");
        require(hasDiagnostic(evaluation.diagnostics, Diagnostic::Code::UnknownEvent), "event resolver miss should report unknown event");
    }

    {
        const auto parseResult = talas::expr::parse("pi");
        const auto& expression = requireExpression(parseResult, "pi");
        const auto evaluation = talas::expr::evaluate(expression, {}, EvaluationOptions{ValueDomain::Scalar});
        require(!evaluation.ok(), "identifier without resolver should fail evaluation");
        require(hasDiagnostic(evaluation.diagnostics, Diagnostic::Code::UnknownIdentifier), "missing identifier resolver should report unknown identifier");
    }

    {
        EvaluationContext context;
        context.resolveIdentifier = resolveIdentifier;
        const auto parseResult = talas::expr::parse("missing");
        const auto& expression = requireExpression(parseResult, "missing");
        const auto evaluation = talas::expr::evaluate(expression, context, EvaluationOptions{ValueDomain::Scalar});
        require(!evaluation.ok(), "identifier missed by resolver should fail evaluation");
        require(hasDiagnostic(evaluation.diagnostics, Diagnostic::Code::UnknownIdentifier), "identifier resolver miss should report unknown identifier");
    }

    {
        EvaluationContext context;
        context.sampleRate = 0.0;
        const auto parseResult = talas::expr::parse("48000 samples");
        const auto& expression = requireExpression(parseResult, "48000 samples");
        const auto evaluation = talas::expr::evaluate(expression, context, EvaluationOptions{ValueDomain::Seconds});
        require(!evaluation.ok(), "samples conversion with zero sample rate should fail evaluation");
        require(hasDiagnostic(evaluation.diagnostics, Diagnostic::Code::MissingSampleRate), "zero sample rate should report missing sample rate");
    }

    {
        EvaluationContext context;
        context.tempoBpm = 0.0;
        const auto parseResult = talas::expr::parse("2 beats");
        const auto& expression = requireExpression(parseResult, "2 beats");
        const auto evaluation = talas::expr::evaluate(expression, context, EvaluationOptions{ValueDomain::Seconds});
        require(!evaluation.ok(), "beats conversion with zero tempo should fail evaluation");
        require(hasDiagnostic(evaluation.diagnostics, Diagnostic::Code::MissingTempo), "zero tempo should report missing tempo");
    }

    {
        EvaluationContext context;
        context.resolveEvent = resolveNonFiniteEvent;
        const auto parseResult = talas::expr::parse("!Bad");
        const auto& expression = requireExpression(parseResult, "!Bad");
        const auto evaluation = talas::expr::evaluate(expression, context, EvaluationOptions{ValueDomain::Scalar});
        require(!evaluation.ok(), "non-finite resolver value should fail evaluation");
        require(hasDiagnostic(evaluation.diagnostics, Diagnostic::Code::NonFiniteResult), "non-finite resolver value should report non-finite result");
    }

    {
        const auto parseResult = talas::expr::parse(
            "10000 db",
            ExpressionLanguageVersion::VariablesV2);
        const auto& expression = requireExpression(
            parseResult,
            "overflowing decibel literal");
        const auto evaluation = talas::expr::evaluate(
            expression,
            {},
            EvaluationOptions{ValueDomain::Scalar});
        ExpressionValue fastValue;
        require(
            !evaluation.ok()
                && hasDiagnostic(
                    evaluation.diagnostics,
                    Diagnostic::Code::NonFiniteResult)
                && !talas::expr::tryEvaluate(
                    expression,
                    {},
                    EvaluationOptions{ValueDomain::Scalar},
                    fastValue),
            "overflowing decibel conversion should fail in both evaluators");
    }
}

void testLanguageVersionIsExplicitAndLegacySafe()
{
    const auto legacy = talas::expr::parse("abs(-2)");
    require(!legacy.ok(), "default parser should retain legacy grammar");
    require(
        hasDiagnostic(legacy.diagnostics, Diagnostic::Code::UnexpectedToken),
        "legacy function call should retain its unexpected-token diagnostic");

    const auto comparison = talas::expr::parse("1 < 2");
    require(!comparison.ok(), "legacy parser should reject comparisons");

    const auto v2 = talas::expr::parse(
        "abs(-2)",
        ExpressionLanguageVersion::PureV2);
    const auto& expression = requireExpression(v2, "explicit pure-v2 call");
    require(
        expression.languageVersion() == ExpressionLanguageVersion::PureV2,
        "compiled expression should retain its exact language version");
    require(
        expression.instructionCount() > 0
            && expression.instructionCount()
                <= talas::expr::ExpressionLimits::maximumInstructions,
        "pure-v2 expression should publish a bounded instruction program");
    require(
        expression.maximumStackDepth() > 0
            && expression.maximumStackDepth()
                <= talas::expr::ExpressionLimits::maximumEvaluationStack,
        "pure-v2 expression should publish its bounded stack proof");

    const auto references = talas::expr::parse(
        "if(!Gate, abs(value), !Fallback)",
        ExpressionLanguageVersion::PureV2);
    const auto& referenceExpression = requireExpression(
        references,
        "pure-v2 reference extraction");
    require(
        referenceExpression.eventReferences()
            == std::vector<std::string>{"Gate", "Fallback"}
            && referenceExpression.identifiers()
                == std::vector<std::string>{"value"},
        "function names should not be exposed as dynamic identifiers");

    const auto requireLegacyDiagnostic = [](
                                             std::string_view source,
                                             Diagnostic::Code code,
                                             std::size_t offset,
                                             std::size_t length) {
        const auto implicit = talas::expr::parse(source);
        const auto explicitLegacy = talas::expr::parse(
            source,
            ExpressionLanguageVersion::LegacyV1);
        require(
            !implicit.ok() && !explicitLegacy.ok()
                && implicit.diagnostics.size() == 1
                && explicitLegacy.diagnostics.size() == 1
                && implicit.diagnostics.front().code == code
                && explicitLegacy.diagnostics.front().code == code
                && implicit.diagnostics.front().range.offset == offset
                && explicitLegacy.diagnostics.front().range.offset == offset
                && implicit.diagnostics.front().range.length == length
                && explicitLegacy.diagnostics.front().range.length == length,
            "legacy operator diagnostics should preserve exact code and range");
    };
    requireLegacyDiagnostic(
        "1 < 2", Diagnostic::Code::UnexpectedCharacter, 2, 1);
    requireLegacyDiagnostic(
        "1 <= 2", Diagnostic::Code::UnexpectedCharacter, 2, 1);
    requireLegacyDiagnostic(
        "1 > 2", Diagnostic::Code::UnexpectedCharacter, 2, 1);
    requireLegacyDiagnostic(
        "1 >= 2", Diagnostic::Code::UnexpectedCharacter, 2, 1);
    requireLegacyDiagnostic(
        "1 == 2", Diagnostic::Code::UnexpectedCharacter, 2, 1);
    requireLegacyDiagnostic(
        "1 != 2", Diagnostic::Code::TrailingInput, 2, 1);

    std::string legacyLarge = "1";
    for (std::size_t index = 1; index < 129; ++index)
        legacyLarge += "+1";
    const auto legacyLargeResult = talas::expr::parse(legacyLarge);
    const auto& legacyLargeExpression = requireExpression(
        legacyLargeResult,
        "legacy expression above the pure-v2 node budget");
    const auto legacyLargeEvaluation = talas::expr::evaluate(
        legacyLargeExpression,
        {},
        EvaluationOptions{ValueDomain::Scalar});
    require(
        legacyLargeEvaluation.ok()
            && legacyLargeEvaluation.value.value == 129.0,
        "legacy format-22 expressions should preserve pre-v2 behavior");

    std::string legacyLongSource = "1";
    legacyLongSource.append(
        talas::expr::ExpressionLimits::maximumSourceBytes,
        ' ');
    const auto legacyLongSourceResult = talas::expr::parse(legacyLongSource);
    require(
        legacyLongSourceResult.ok(),
        "legacy source above the pure-v2 byte budget should remain valid");

    std::string legacyDeep = "1";
    for (std::size_t index = 0;
         index <= talas::expr::ExpressionLimits::maximumNestingDepth;
         ++index)
    {
        legacyDeep = "(" + legacyDeep + ")";
    }
    require(
        talas::expr::parse(legacyDeep).ok(),
        "legacy nesting above the pure-v2 depth budget should remain valid");

    std::string legacyOverflow(10000, '(');
    legacyOverflow += "1";
    legacyOverflow.append(10000, ')');
    const auto overflow = talas::expr::parse(legacyOverflow);
    require(
        !overflow.ok() && overflow.diagnostics.size() == 1,
        "pathological legacy nesting should fail closed instead of exhausting the stack");
}

void testPureFunctionsAndDomains()
{
    requireNear(
        evaluateV2Ok("abs(-440 hz)", ValueDomain::Hertz).value,
        440.0,
        1.0e-12,
        "abs should preserve a value domain");
    requireNear(
        evaluateV2Ok("sqrt(9)", ValueDomain::Scalar).value,
        3.0,
        1.0e-12,
        "sqrt should evaluate scalar input");
    requireNear(
        evaluateV2Ok("exp(log(4))", ValueDomain::Scalar).value,
        4.0,
        1.0e-12,
        "log and exp should compose deterministically");
    requireNear(
        evaluateV2Ok("clamp(2 s, 500 ms, 1 s)", ValueDomain::Seconds).value,
        1.0,
        1.0e-12,
        "clamp should normalize compatible time domains");
    EvaluationContext timeContext;
    timeContext.sampleRate = 48000.0;
    requireNear(
        evaluateV2Ok(
            "clamp(96000 samples, 500 ms, 1 s)",
            ValueDomain::Seconds,
            timeContext)
            .value,
        1.0,
        1.0e-12,
        "clamp should normalize all three compatible time arguments");
    requireNear(
        evaluateV2Ok(
            "map(5, 0, 10, 100 hz, 200 hz)",
            ValueDomain::Hertz)
            .value,
        150.0,
        1.0e-12,
        "map should preserve the output domain");

    const std::vector<std::string_view> invalid = {
        "sqrt(-1)",
        "sqrt(1 hz)",
        "log(0)",
        "exp(10000)",
        "clamp(1, 2, 0)",
        "clamp(1 s, 0 hz, 2 s)",
        "map(1, 0, 0, 0, 1)",
        "map(1 s, 0 hz, 2 s, 0, 1)",
    };
    for (const auto source : invalid)
    {
        const auto parsed = talas::expr::parse(
            source,
            ExpressionLanguageVersion::PureV2);
        const auto& expression = requireExpression(
            parsed,
            std::string(source));
        const auto evaluated = talas::expr::evaluate(
            expression,
            {},
            EvaluationOptions{ValueDomain::Scalar});
        require(!evaluated.ok(), std::string(source) + " should fail closed");
        require(
            evaluated.diagnostics.front().range.offset == 0
                && evaluated.diagnostics.front().range.length == source.size(),
            std::string(source) + " should retain the exact call range");
    }

    const auto unknown = talas::expr::parse(
        "native(1)",
        ExpressionLanguageVersion::PureV2);
    require(!unknown.ok(), "unknown functions should fail closed");
    require(
        hasDiagnostic(unknown.diagnostics, Diagnostic::Code::UnknownFunction),
        "unknown function should have a closed diagnostic");
    const auto wrongArity = talas::expr::parse(
        "clamp(1, 2)",
        ExpressionLanguageVersion::PureV2);
    require(!wrongArity.ok(), "wrong function arity should fail closed");
    require(
        hasDiagnostic(
            wrongArity.diagnostics,
            Diagnostic::Code::InvalidArgumentCount),
        "wrong function arity should have an exact diagnostic");
}

void testComparisonsAndLazyConditional()
{
    requireNear(
        evaluateV2Ok("1 + 2 * 3 == 7", ValueDomain::Scalar).value,
        1.0,
        0.0,
        "comparison should bind below arithmetic");
    requireNear(
        evaluateV2Ok("500 ms < 1 s", ValueDomain::Scalar).value,
        1.0,
        0.0,
        "comparison should normalize compatible time domains");
    requireNear(
        evaluateV2Ok("2 != 2", ValueDomain::Scalar).value,
        0.0,
        0.0,
        "comparison equality should be exact");
    requireNear(
        evaluateV2Ok("if(1, 4 hz, !Missing)", ValueDomain::Hertz).value,
        4.0,
        0.0,
        "if should not resolve its unselected branch");
    requireNear(
        evaluateV2Ok("if(0, 1 / 0, 3)", ValueDomain::Scalar).value,
        3.0,
        0.0,
        "if should not evaluate its unselected branch");

    const auto selectedFailure = talas::expr::parse(
        "if(0, 1, !Missing)",
        ExpressionLanguageVersion::PureV2);
    const auto selectedEvaluation = talas::expr::evaluate(
        requireExpression(selectedFailure, "selected missing branch"),
        {},
        EvaluationOptions{ValueDomain::Scalar});
    require(!selectedEvaluation.ok(), "selected branch failure should surface");
    require(
        hasDiagnostic(
            selectedEvaluation.diagnostics,
            Diagnostic::Code::UnknownEvent),
        "selected missing branch should report its resolver failure");

    const auto badCondition = talas::expr::parse(
        "if(1 s, 2, 3)",
        ExpressionLanguageVersion::PureV2);
    const auto badConditionEvaluation = talas::expr::evaluate(
        requireExpression(badCondition, "non-scalar condition"),
        {},
        EvaluationOptions{ValueDomain::Scalar});
    require(!badConditionEvaluation.ok(), "if condition should be scalar");
    require(
        hasDiagnostic(
            badConditionEvaluation.diagnostics,
            Diagnostic::Code::UnitMismatch),
        "non-scalar if condition should report a domain mismatch");
}

void testIntoXArrayInterpolatesChangingEventArrays()
{
    struct Controls
    {
        double middleX = 0.5;
        double middleY = 0.2;
        double interpolation = 0.0;
    } controls;
    EvaluationContext context;
    context.user = &controls;
    context.resolveEvent = [](void* user, std::string_view name,
                              ExpressionValue& out) noexcept {
        const auto& values = *static_cast<Controls*>(user);
        if (name == "MidX")
            out = {values.middleX, ValueDomain::Scalar};
        else if (name == "MidY")
            out = {values.middleY, ValueDomain::Scalar};
        else if (name == "Interpolation")
            out = {values.interpolation, ValueDomain::Scalar};
        else
            return false;
        return true;
    };
    const auto source =
        "intoXArray(0.25, [0, !MidX, 1], [0, !MidY, 1], !Interpolation)";
    const auto parsed = talas::expr::parse(
        source, ExpressionLanguageVersion::PureV5);
    const auto& expression = requireExpression(parsed, source);
    require(
        expression.eventReferences()
            == std::vector<std::string>{"MidX", "MidY", "Interpolation"},
        "intoXArray should expose changing members for control discovery");
    const auto evaluate = [&] {
        const auto result = talas::expr::evaluate(
            expression, context, EvaluationOptions{ValueDomain::Scalar});
        require(result.ok(), "valid intoXArray should evaluate");
        return result.value.value;
    };
    requireNear(evaluate(), 0.0, 0.0, "zero interpolation should hold left point");
    controls.interpolation = 0.5;
    requireNear(evaluate(), 0.1, 1.0e-12, "half interpolation should be linear");
    controls.interpolation = 1.0;
    requireNear(evaluate(), 0.0375, 1.0e-12, "full interpolation should be a Hermite segment");
    controls.interpolation = 0.75;
    requireNear(evaluate(), 0.06875, 1.0e-12, "intermediate interpolation should blend linear and spline");
    controls.middleY = 0.6;
    controls.interpolation = 0.5;
    requireNear(evaluate(), 0.3, 1.0e-12, "changing a Y member should update without reparsing");
    controls.middleX = 0.75;
    requireNear(evaluate(), 0.2, 1.0e-12, "changing an X member should update without reparsing");

    controls.middleX = 0.0;
    const auto unordered = talas::expr::evaluate(
        expression, context, EvaluationOptions{ValueDomain::Scalar});
    require(!unordered.ok()
            && hasDiagnostic(unordered.diagnostics, Diagnostic::Code::InvalidArgument),
        "duplicate changing X points should fail closed");
    controls.middleX = 0.5;
    controls.interpolation = 1.1;
    const auto outside = talas::expr::evaluate(
        expression, context, EvaluationOptions{ValueDomain::Scalar});
    require(!outside.ok()
            && hasDiagnostic(outside.diagnostics, Diagnostic::Code::InvalidArgument),
        "interpolation outside zero to one should fail closed");
    require(!talas::expr::parse(
        "intoXArray(0, [0, 1], [0, 0.5, 1], 0.5)",
        ExpressionLanguageVersion::PureV5).ok(),
        "unequal array lengths should fail at parse time");
    require(!talas::expr::parse(
        "intoXArray(0, [0], [1], 0.5)",
        ExpressionLanguageVersion::PureV5).ok(),
        "a single point should fail at parse time");
    require(!talas::expr::parse(
        source, ExpressionLanguageVersion::PureV4).ok(),
        "frozen PureV4 must reject new intoXArray syntax");
    require(!talas::expr::parse(
        source, ExpressionLanguageVersion::VariablesV2).ok(),
        "default VariablesV2 must reject new intoXArray syntax");
    talas::expr::ExpressionDefinitionLibrary reserved;
    reserved.definitions = {
        {"intoXArray", {}, ValueDomain::Scalar, "1"},
    };
    const auto shadowed = talas::expr::parse(
        "1", ExpressionLanguageVersion::PureV5, reserved);
    require(!shadowed.ok()
            && hasDiagnostic(shadowed.diagnostics,
                Diagnostic::Code::InvalidDefinition),
        "PureV5 definitions must not shadow intoXArray");
}

void testBoundedSelectionListsAndSafeIndexing()
{
    requireNear(
        evaluateV2Ok("[10, 20, 30][1]", ValueDomain::Scalar).value,
        20.0,
        0.0,
        "selection list should return the exact indexed element");
    requireNear(
        evaluateV2Ok("[1 hz, 2 hz][1 == 1]", ValueDomain::Hertz).value,
        2.0,
        0.0,
        "comparison result should be a valid scalar index");
    requireNear(
        evaluateV2Ok("[[1, 2][1], 3][0]", ValueDomain::Scalar).value,
        2.0,
        0.0,
        "nested indexed lists should remain bounded and deterministic");
    requireNear(
        evaluateV2Ok("[!Missing, 4][1]", ValueDomain::Scalar).value,
        4.0,
        0.0,
        "unselected list elements should not invoke their resolvers");

    const auto references = talas::expr::parse(
        "[!First, !Second, identifier][0]",
        ExpressionLanguageVersion::PureV2);
    const auto& referenceExpression = requireExpression(
        references,
        "selection-list reference discovery");
    require(
        referenceExpression.eventReferences()
            == std::vector<std::string>{"First", "Second"}
            && referenceExpression.identifiers()
                == std::vector<std::string>{"identifier"},
        "dependency discovery should include every list element");

    const auto selectedFailure = talas::expr::parse(
        "[1, !Missing][1]",
        ExpressionLanguageVersion::PureV2);
    const auto selectedEvaluation = talas::expr::evaluate(
        requireExpression(selectedFailure, "selected missing list element"),
        {},
        EvaluationOptions{ValueDomain::Scalar});
    require(
        !selectedEvaluation.ok()
            && hasDiagnostic(
                selectedEvaluation.diagnostics,
                Diagnostic::Code::UnknownEvent),
        "selected list element resolver failure should surface");

    const std::vector<std::pair<std::string_view, SourceRange>> invalidIndices{
        {"[1,2][-1]", {6, 2}},
        {"[1,2][0.5]", {6, 3}},
        {"[1,2][1 s]", {6, 3}},
        {"[1,2][2]", {6, 1}},
    };
    for (const auto& [source, expectedRange] : invalidIndices)
    {
        const auto parsed = talas::expr::parse(
            source,
            ExpressionLanguageVersion::PureV2);
        const auto evaluated = talas::expr::evaluate(
            requireExpression(parsed, std::string(source)),
            {},
            EvaluationOptions{ValueDomain::Scalar});
        require(
            !evaluated.ok()
                && evaluated.diagnostics.size() == 1
                && evaluated.diagnostics.front().code
                    == Diagnostic::Code::InvalidIndex
                && evaluated.diagnostics.front().message
                    == "Pure expression selection-list index is invalid"
                && evaluated.diagnostics.front().range.offset
                    == expectedRange.offset
                && evaluated.diagnostics.front().range.length
                    == expectedRange.length,
            std::string(source)
                + " should reject the index at its exact source range");
    }

    for (const auto source : {
             std::string_view{"[][0]"},
             std::string_view{"[1,2]"},
             std::string_view{"[1,][0]"},
             std::string_view{"[1 2][0]"},
         })
    {
        require(
            !talas::expr::parse(
                 source,
                 ExpressionLanguageVersion::PureV2).ok(),
            std::string(source) + " should fail closed");
    }

    std::string maximumList = "[";
    for (std::size_t index = 0;
         index < talas::expr::ExpressionLimits::maximumListElements;
         ++index)
    {
        if (index != 0)
            maximumList += ",";
        maximumList += std::to_string(index);
    }
    maximumList += "][31]";
    requireNear(
        evaluateV2Ok(maximumList, ValueDomain::Scalar).value,
        31.0,
        0.0,
        "selection list at the fixed element boundary should evaluate");
    maximumList.insert(maximumList.find(']'), ",32");
    const auto oversized = talas::expr::parse(
        maximumList,
        ExpressionLanguageVersion::PureV2);
    require(
        !oversized.ok()
            && oversized.diagnostics.size() == 1
            && oversized.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded,
        "selection list above the fixed element budget should fail closed");

    const auto legacy = talas::expr::parse("[1,2][0]");
    require(
        !legacy.ok()
            && legacy.diagnostics.size() == 1
            && legacy.diagnostics.front().code
                == Diagnostic::Code::UnexpectedCharacter
            && legacy.diagnostics.front().range.offset == 0
            && legacy.diagnostics.front().range.length == 1,
        "legacy list syntax should retain exact invalid-character behavior");
}

struct SeriesResolverState
{
    double index = 2.0;
    double count = 4.0;
    double first = 10.0;
    double step = 5.0;
    std::uint32_t order = 0;
    std::size_t indexCalls = 0;
    std::size_t countCalls = 0;
    std::size_t firstCalls = 0;
    std::size_t stepCalls = 0;
};

bool resolveSeriesEvent(
    void* user,
    std::string_view name,
    ExpressionValue& out) noexcept
{
    auto& state = *static_cast<SeriesResolverState*>(user);
    if (name == "Index")
    {
        ++state.indexCalls;
        state.order = state.order * 10U + 4U;
        out = {state.index, ValueDomain::Scalar};
        return true;
    }
    if (name == "Count")
    {
        ++state.countCalls;
        state.order = state.order * 10U + 3U;
        out = {state.count, ValueDomain::Scalar};
        return true;
    }
    if (name == "First")
    {
        ++state.firstCalls;
        state.order = state.order * 10U + 1U;
        out = {state.first, ValueDomain::Scalar};
        return true;
    }
    if (name == "Step")
    {
        ++state.stepCalls;
        state.order = state.order * 10U + 2U;
        out = {state.step, ValueDomain::Scalar};
        return true;
    }
    return false;
}

void testPureV3BoundedArithmeticSeries()
{
    require(
        talas::expr::latestPureExpressionLanguage
            == ExpressionLanguageVersion::PureV5
            && talas::expr::isPureExpressionLanguage(
                ExpressionLanguageVersion::PureV2)
            && talas::expr::isPureExpressionLanguage(
                ExpressionLanguageVersion::PureV3)
            && talas::expr::isPureExpressionLanguage(
                ExpressionLanguageVersion::PureV4)
            && talas::expr::isPureExpressionLanguage(
                ExpressionLanguageVersion::PureV5)
            && talas::expr::supportsPureExpressionSeries(
                ExpressionLanguageVersion::PureV3)
            && talas::expr::supportsPureExpressionSeries(
                ExpressionLanguageVersion::PureV4)
            && !talas::expr::supportsPureExpressionDefinitions(
                ExpressionLanguageVersion::PureV3)
            && talas::expr::supportsPureExpressionDefinitions(
                ExpressionLanguageVersion::PureV4)
            && !talas::expr::isPureExpressionLanguage(
                ExpressionLanguageVersion::LegacyV1),
        "the latest bounded pure profile should be explicit");

    const auto frozenV2 = talas::expr::parse(
        "series(1, 2, 4)[1]",
        ExpressionLanguageVersion::PureV2);
    require(
        !frozenV2.ok()
            && frozenV2.diagnostics.size() == 1
            && frozenV2.diagnostics.front().code
                == Diagnostic::Code::UnknownFunction
            && frozenV2.diagnostics.front().range.offset == 0
            && frozenV2.diagnostics.front().range.length == 6,
        "PureV2 should retain its closed grammar and reject series exactly");

    const auto v3 = talas::expr::parse(
        "series(10, 5, 4)[2]",
        ExpressionLanguageVersion::PureV3);
    const auto& expression = requireExpression(v3, "PureV3 series");
    require(
        expression.languageVersion() == ExpressionLanguageVersion::PureV3
            && expression.instructionCount() > 0
            && expression.instructionCount()
                <= talas::expr::ExpressionLimits::maximumInstructions
            && expression.maximumStackDepth()
                <= talas::expr::ExpressionLimits::maximumEvaluationStack,
        "PureV3 series should publish a bounded immutable program");
    const auto evaluated = talas::expr::evaluate(
        expression,
        {},
        EvaluationOptions{ValueDomain::Scalar});
    require(
        evaluated.ok() && evaluated.value.value == 20.0,
        "series should compute first plus step times index");

    requireNear(
        evaluateV3Ok(
            "series(100 hz, 25 hz, 4)[3]",
            ValueDomain::Hertz)
            .value,
        175.0,
        0.0,
        "series should preserve compatible value domains");
    EvaluationContext timeContext;
    timeContext.sampleRate = 48000.0;
    requireNear(
        evaluateV3Ok(
            "series(1 s, 48000 samples, 3)[2]",
            ValueDomain::Seconds,
            timeContext)
            .value,
        3.0,
        0.0,
        "series should normalize compatible time domains");
    requireNear(
        evaluateV3Ok(
            "series(7, 0, 64)[63]",
            ValueDomain::Scalar)
            .value,
        7.0,
        0.0,
        "series should accept the exact count and index boundary");
    requireNear(
        evaluateV3Ok(
            "series(7, 2, 3)[-0]",
            ValueDomain::Scalar)
            .value,
        7.0,
        0.0,
        "negative zero should select the first series element");

    SeriesResolverState state;
    EvaluationContext resolverContext;
    resolverContext.user = &state;
    resolverContext.resolveEvent = resolveSeriesEvent;
    const auto dynamic = talas::expr::parse(
        "series(!First, !Step, !Count)[!Index]",
        ExpressionLanguageVersion::PureV3);
    const auto& dynamicExpression = requireExpression(
        dynamic,
        "dynamic PureV3 series");
    require(
        dynamicExpression.eventReferences()
            == std::vector<std::string>{
                "First", "Step", "Count", "Index"},
        "series dependency discovery should include every operand");
    const auto dynamicEvaluation = talas::expr::evaluate(
        dynamicExpression,
        resolverContext,
        EvaluationOptions{ValueDomain::Scalar});
    require(
        dynamicEvaluation.ok() && dynamicEvaluation.value.value == 20.0
            && state.order == 4312U && state.indexCalls == 1
            && state.countCalls == 1 && state.firstCalls == 1
            && state.stepCalls == 1,
        "series should validate index and count before resolving values once");

    state = {};
    state.index = -1.0;
    const auto invalidIndex = talas::expr::evaluate(
        dynamicExpression,
        resolverContext,
        EvaluationOptions{ValueDomain::Scalar});
    require(
        !invalidIndex.ok()
            && invalidIndex.diagnostics.front().code
                == Diagnostic::Code::InvalidIndex
            && invalidIndex.diagnostics.front().message
                == "Pure expression series index is invalid"
            && invalidIndex.diagnostics.front().range.offset == 30
            && invalidIndex.diagnostics.front().range.length == 6
            && state.order == 4U && state.countCalls == 0
            && state.firstCalls == 0 && state.stepCalls == 0,
        "invalid series index should fail at its range before other resolvers");

    state = {};
    state.count = 65.0;
    const auto invalidCount = talas::expr::evaluate(
        dynamicExpression,
        resolverContext,
        EvaluationOptions{ValueDomain::Scalar});
    require(
        !invalidCount.ok()
            && invalidCount.diagnostics.front().code
                == Diagnostic::Code::InvalidSequenceCount
            && invalidCount.diagnostics.front().range.offset == 22
            && invalidCount.diagnostics.front().range.length == 6
            && state.order == 43U && state.firstCalls == 0
            && state.stepCalls == 0,
        "invalid series count should fail at its range before value resolvers");

    for (const auto& [source, code] :
         std::vector<std::pair<std::string_view, Diagnostic::Code>>{
             {"series(1, 2, 0)[0]", Diagnostic::Code::InvalidSequenceCount},
             {"series(1, 2, 2.5)[0]", Diagnostic::Code::InvalidSequenceCount},
             {"series(1, 2, 2 s)[0]", Diagnostic::Code::InvalidSequenceCount},
             {"series(1, 2, 65)[0]", Diagnostic::Code::InvalidSequenceCount},
             {"series(1, 2, 4)[4]", Diagnostic::Code::InvalidIndex},
             {"series(1 hz, 2 s, 4)[1]", Diagnostic::Code::UnitMismatch},
             {"series(1e308, 1e308, 2)[1]", Diagnostic::Code::NonFiniteResult},
         })
    {
        const auto parsed = talas::expr::parse(
            source,
            ExpressionLanguageVersion::PureV3);
        const auto result = talas::expr::evaluate(
            requireExpression(parsed, std::string(source)),
            timeContext,
            EvaluationOptions{
                source.find("hz") != std::string_view::npos
                    ? ValueDomain::Hertz
                    : ValueDomain::Scalar});
        require(
            !result.ok() && result.diagnostics.front().code == code,
            std::string(source) + " should fail with its closed diagnostic");
    }

    for (const auto source : {
             std::string_view{"series(1, 2)[0]"},
             std::string_view{"series(1, 2, 3)"},
             std::string_view{"series(1, 2, 3, 4)[0]"},
             std::string_view{"series(1, 2, 3,)[0]"},
             std::string_view{"series(1, 2, 3)[]"},
         })
    {
        require(
            !talas::expr::parse(
                 source,
                 ExpressionLanguageVersion::PureV3).ok(),
            std::string(source) + " should fail closed during parsing");
    }
}

void testPureV4UserDefinedExpressions()
{
    talas::expr::ExpressionDefinitionLibrary library;
    library.definitions = {
        {"doubleValue",
         {{"value", ValueDomain::Scalar}},
         ValueDomain::Scalar,
         "value + value"},
        {"scaled",
         {{"value", ValueDomain::Scalar},
          {"amount", ValueDomain::Scalar}},
         ValueDomain::Scalar,
         "doubleValue(value) * amount"},
        {"twiceHz",
         {{"value", ValueDomain::Hertz}},
         ValueDomain::Hertz,
         "value * 2"},
        {"guard",
         {{"condition", ValueDomain::Scalar}},
         ValueDomain::Scalar,
         "if(condition, !First, !Missing)"},
    };

    const auto parsed = talas::expr::parse(
        "scaled(!First, !Step)",
        ExpressionLanguageVersion::PureV4,
        library);
    const auto& expression = requireExpression(
        parsed,
        "PureV4 nested user-defined expression");
    SeriesResolverState state;
    EvaluationContext context;
    context.user = &state;
    context.resolveEvent = resolveSeriesEvent;
    const auto evaluated = talas::expr::evaluate(
        expression,
        context,
        EvaluationOptions{ValueDomain::Scalar});
    require(
        evaluated.ok() && evaluated.value.value == 100.0
            && state.firstCalls == 1 && state.stepCalls == 1
            && state.order == 12U,
        "user-defined arguments should resolve once from left to right");

    const auto hz = talas::expr::parse(
        "twiceHz(220 hz)",
        ExpressionLanguageVersion::PureV4,
        library);
    const auto hzResult = talas::expr::evaluate(
        requireExpression(hz, "PureV4 domain fixture"),
        {},
        EvaluationOptions{ValueDomain::Hertz});
    require(
        hzResult.ok() && hzResult.value.value == 440.0,
        "user-defined parameter and result domains should be enforced");

    const auto wrongDomain = talas::expr::parse(
        "twiceHz(220)",
        ExpressionLanguageVersion::PureV4,
        library);
    const auto wrongDomainResult = talas::expr::evaluate(
        requireExpression(wrongDomain, "PureV4 wrong-domain fixture"),
        {},
        EvaluationOptions{ValueDomain::Hertz});
    require(
        !wrongDomainResult.ok()
            && wrongDomainResult.diagnostics.front().code
                == Diagnostic::Code::UnitMismatch,
        "user-defined arguments should reject incompatible domains");

    state = {};
    const auto guarded = talas::expr::parse(
        "guard(1)",
        ExpressionLanguageVersion::PureV4,
        library);
    const auto guardedResult = talas::expr::evaluate(
        requireExpression(guarded, "PureV4 lazy-body fixture"),
        context,
        EvaluationOptions{ValueDomain::Scalar});
    require(
        guardedResult.ok() && guardedResult.value.value == state.first
            && state.firstCalls == 1
            && guarded.expression->eventReferences()
                == std::vector<std::string>{"First", "Missing"},
        "a user-defined body should preserve lazy conditional resolution");

    talas::expr::ExpressionDefinitionLibrary failingEvaluation;
    failingEvaluation.definitions = {
        {"divideByZero", {}, ValueDomain::Scalar, "100 / 0"},
    };
    const auto failingCall = talas::expr::parse(
        "divideByZero()",
        ExpressionLanguageVersion::PureV4,
        failingEvaluation);
    const auto failingResult = talas::expr::evaluate(
        requireExpression(
            failingCall,
            "PureV4 definition-origin evaluation fixture"),
        {},
        EvaluationOptions{ValueDomain::Scalar});
    require(
        !failingResult.ok()
            && failingResult.diagnostics.front().code
                == Diagnostic::Code::DivisionByZero
            && failingResult.diagnostics.front().range.offset == 0
            && failingResult.diagnostics.front().range.length == 7
            && failingResult.diagnostics.front().definitionIndex
                == std::optional<std::size_t>{0},
        "evaluation diagnostics should retain the exact inlined definition origin");

    talas::expr::ExpressionDefinitionLibrary failingResultDomain;
    failingResultDomain.definitions = {
        {"badDomain", {}, ValueDomain::Hertz, "100 + 20"},
    };
    const auto badDomainCall = talas::expr::parse(
        "badDomain()",
        ExpressionLanguageVersion::PureV4,
        failingResultDomain);
    const auto badDomainResult = talas::expr::evaluate(
        requireExpression(
            badDomainCall,
            "PureV4 definition result-domain origin fixture"),
        {},
        EvaluationOptions{ValueDomain::Hertz});
    require(
        !badDomainResult.ok()
            && badDomainResult.diagnostics.front().code
                == Diagnostic::Code::UnitMismatch
            && badDomainResult.diagnostics.front().range.offset == 0
            && badDomainResult.diagnostics.front().range.length == 8
            && badDomainResult.diagnostics.front().definitionIndex
                == std::optional<std::size_t>{0},
        "definition result-domain diagnostics should use the definition body range");

    talas::expr::ExpressionDefinitionLibrary invalidUnused = library;
    invalidUnused.definitions.push_back(
        {"broken", {}, ValueDomain::Scalar, "1 +"});
    const auto invalidBody = talas::expr::parse(
        "1",
        ExpressionLanguageVersion::PureV4,
        invalidUnused);
    require(
        !invalidBody.ok(),
        "an invalid unused definition should invalidate the closed library");

    talas::expr::ExpressionDefinitionLibrary reserved;
    reserved.definitions = {
        {"abs", {}, ValueDomain::Scalar, "1"},
    };
    const auto reservedName = talas::expr::parse(
        "1",
        ExpressionLanguageVersion::PureV4,
        reserved);
    require(
        !reservedName.ok()
            && reservedName.diagnostics.front().code
                == Diagnostic::Code::InvalidDefinition,
        "definition names should not shadow built-in functions");

    talas::expr::ExpressionDefinitionLibrary reservedParameter;
    reservedParameter.definitions = {
        {"identity",
         {{"abs", ValueDomain::Scalar}},
         ValueDomain::Scalar,
         "abs"},
    };
    const auto reservedParameterName = talas::expr::parse(
        "0",
        ExpressionLanguageVersion::PureV4,
        reservedParameter);
    require(
        !reservedParameterName.ok()
            && reservedParameterName.diagnostics.front().code
                == Diagnostic::Code::InvalidDefinition
            && reservedParameterName.diagnostics.front().definitionIndex
                == std::optional<std::size_t>{0},
        "definition parameters should not shadow built-in functions");

    for (const auto& historicalName : {
             std::string{"previous"},
             std::string{"accumulate"},
             std::string{"sampleHold"}})
    {
        talas::expr::ExpressionDefinitionLibrary historicalDefinition;
        historicalDefinition.definitions = {
            {historicalName, {}, ValueDomain::Scalar, "7"},
        };
        const auto historicalCall = talas::expr::parse(
            historicalName + "()",
            ExpressionLanguageVersion::PureV4,
            historicalDefinition);
        const auto historicalResult = talas::expr::evaluate(
            requireExpression(
                historicalCall,
                "PureV4 historical stateful-name definition fixture"),
            {},
            EvaluationOptions{ValueDomain::Scalar});
        require(
            historicalResult.ok() && historicalResult.value.value == 7.0,
            "Session-only function names should remain available to frozen PureV4 definitions");

        talas::expr::ExpressionDefinitionLibrary historicalParameter;
        historicalParameter.definitions = {
            {"identity",
             {{historicalName, ValueDomain::Scalar}},
             ValueDomain::Scalar,
             historicalName},
        };
        const auto historicalParameterCall = talas::expr::parse(
            "identity(9)",
            ExpressionLanguageVersion::PureV4,
            historicalParameter);
        const auto historicalParameterResult = talas::expr::evaluate(
            requireExpression(
                historicalParameterCall,
                "PureV4 historical stateful-name parameter fixture"),
            {},
            EvaluationOptions{ValueDomain::Scalar});
        require(
            historicalParameterResult.ok()
                && historicalParameterResult.value.value == 9.0,
            "Session-only function names should remain available to frozen PureV4 parameters");
    }

    std::string maximumParsedBody = "value";
    for (std::size_t index = 1; index < 128; ++index)
        maximumParsedBody += "+value";
    talas::expr::ExpressionDefinitionLibrary uncallable;
    uncallable.definitions = {
        {"uncallable",
         {{"value", ValueDomain::Scalar}},
         ValueDomain::Scalar,
         maximumParsedBody},
    };
    const auto uncallableUnused = talas::expr::parse(
        "0",
        ExpressionLanguageVersion::PureV4,
        uncallable);
    require(
        !uncallableUnused.ok()
            && uncallableUnused.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded
            && uncallableUnused.diagnostics.front().definitionIndex
                == std::optional<std::size_t>{0},
        "every stored definition should compile within the fixed callable budget even when unused");

    talas::expr::ExpressionDefinitionLibrary recursive;
    recursive.definitions = {
        {"first", {}, ValueDomain::Scalar, "second()"},
        {"second", {}, ValueDomain::Scalar, "first()"},
    };
    const auto cycle = talas::expr::parse(
        "first()",
        ExpressionLanguageVersion::PureV4,
        recursive);
    require(
        !cycle.ok() && cycle.diagnostics.size() == 1
            && cycle.diagnostics.front().code
                == Diagnostic::Code::RecursiveDefinition,
        "recursive user-defined expressions should fail closed");

    talas::expr::ExpressionDefinitionLibrary tooDeep;
    for (std::size_t index = 0;
         index <= talas::expr::ExpressionLimits::maximumDefinitionExpansionDepth;
         ++index)
    {
        const auto name = "level" + std::to_string(index);
        const auto source = index
                == talas::expr::ExpressionLimits::maximumDefinitionExpansionDepth
            ? std::string{"1"}
            : "level" + std::to_string(index + 1) + "()";
        tooDeep.definitions.push_back(
            {name, {}, ValueDomain::Scalar, source});
    }
    const auto depthFailure = talas::expr::parse(
        "level0()",
        ExpressionLanguageVersion::PureV4,
        tooDeep);
    require(
        !depthFailure.ok()
            && depthFailure.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded,
        "definition expansion depth should fail before compilation");

    const auto libraryUnderV3 = talas::expr::parse(
        "1",
        ExpressionLanguageVersion::PureV3,
        library);
    require(
        !libraryUnderV3.ok()
            && libraryUnderV3.diagnostics.front().code
                == Diagnostic::Code::InvalidDefinition,
        "non-empty definition libraries should require PureV4 authority");

    const auto frozenV3 = talas::expr::parse(
        "scaled(1, 2)",
        ExpressionLanguageVersion::PureV3);
    require(
        !frozenV3.ok()
            && frozenV3.diagnostics.front().code
                == Diagnostic::Code::UnknownFunction,
        "PureV3 should retain its closed grammar without user definitions");
}

void testSessionV1BoundedStatefulExpressions()
{
    require(
        !talas::expr::isPureExpressionLanguage(
                ExpressionLanguageVersion::SessionV1)
            && talas::expr::isCompiledExpressionLanguage(
                ExpressionLanguageVersion::SessionV1)
            && talas::expr::supportsExpressionDefinitions(
                ExpressionLanguageVersion::SessionV1)
            && talas::expr::supportsStatefulExpressions(
                ExpressionLanguageVersion::SessionV1),
        "SessionV1 should be an explicit compiled non-pure profile");

    require(
        !talas::expr::parse(
             "previous(1, 0)",
             ExpressionLanguageVersion::LegacyV1).ok(),
        "LegacyV1 should retain its frozen rejection of stateful syntax");
    for (const auto frozen : {
             ExpressionLanguageVersion::PureV2,
             ExpressionLanguageVersion::PureV3,
             ExpressionLanguageVersion::PureV4})
    {
        const auto rejected = talas::expr::parse("previous(1, 0)", frozen);
        require(
            !rejected.ok()
                && rejected.diagnostics.size() == 1
                && rejected.diagnostics.front().code
                    == Diagnostic::Code::UnknownFunction
                && rejected.diagnostics.front().range.offset == 0
                && rejected.diagnostics.front().range.length == 8,
            "frozen pure profiles should preserve their exact unknown-function diagnostic");
    }

    SeriesResolverState values;
    EvaluationContext context;
    context.user = &values;
    context.resolveEvent = resolveSeriesEvent;
    const auto parsed = talas::expr::parse(
        "previous(!First, 0)",
        ExpressionLanguageVersion::SessionV1);
    const auto& expression = requireExpression(
        parsed,
        "SessionV1 previous fixture");
    require(
        expression.isStateful() && expression.stateSlotCount() == 1,
        "previous should compile to one fixed state slot");
    const auto missingState = talas::expr::evaluate(
        expression,
        context,
        EvaluationOptions{ValueDomain::Scalar});
    require(
        !missingState.ok()
            && missingState.diagnostics.size() == 1
            && missingState.diagnostics.front().code
                == Diagnostic::Code::MissingState,
        "ordinary evaluation should reject a stateful program");

    talas::expr::ExpressionState previousState;
    talas::expr::ExpressionStateScratch previousScratch;
    require(
        talas::expr::prepareExpressionState(expression, previousState)
            && talas::expr::prepareExpressionStateScratch(
                expression,
                previousScratch)
            && previousState.slotCount == 1
            && previousScratch.candidate.slotCount == 1,
        "state and caller-owned scratch preparation should bind the exact compiled slot count");
    auto first = talas::expr::evaluateStateful(
        expression,
        context,
        previousState,
        EvaluationOptions{ValueDomain::Scalar});
    require(first.ok() && first.value.value == 0.0,
        "previous should return its initial value first");
    values.first = 20.0;
    auto second = talas::expr::evaluateStateful(
        expression,
        context,
        previousState,
        EvaluationOptions{ValueDomain::Scalar});
    require(second.ok() && second.value.value == 10.0,
        "previous should return the last accepted value");

    const auto rollbackParsed = talas::expr::parse(
        "previous(!First, 0) + (1 / !Step)",
        ExpressionLanguageVersion::SessionV1);
    const auto& rollbackExpression = requireExpression(
        rollbackParsed,
        "transactional state rollback fixture");
    talas::expr::ExpressionState rollbackState;
    require(talas::expr::prepareExpressionState(
                rollbackExpression,
                rollbackState),
        "rollback fixture state should prepare");
    values.first = 4.0;
    values.step = 1.0;
    require(talas::expr::evaluateStateful(
                rollbackExpression,
                context,
                rollbackState,
                {ValueDomain::Scalar})
                .value.value == 1.0,
        "rollback fixture should seed state");
    const auto lastGoodState = rollbackState;
    values.first = 8.0;
    values.step = 0.0;
    require(
        !talas::expr::evaluateStateful(
             rollbackExpression,
             context,
             rollbackState,
             {ValueDomain::Scalar})
             .ok()
            && rollbackState == lastGoodState,
        "a later failure should commit no partial expression state");
    values.first = 12.0;
    values.step = 1.0;
    const auto afterFailure = talas::expr::evaluateStateful(
        rollbackExpression,
        context,
        rollbackState,
        {ValueDomain::Scalar});
    require(afterFailure.ok() && afterFailure.value.value == 5.0,
        "evaluation after failure should observe the last accepted state");

    const auto accumulated = talas::expr::parse(
        "accumulate(!Step, !Index, 10)",
        ExpressionLanguageVersion::SessionV1);
    const auto& accumulatedExpression = requireExpression(
        accumulated,
        "SessionV1 accumulate fixture");
    talas::expr::ExpressionState accumulatedState;
    require(talas::expr::prepareExpressionState(
                accumulatedExpression,
                accumulatedState),
        "accumulator state should prepare");
    values.step = 5.0;
    values.index = 0.0;
    require(talas::expr::evaluateStateful(
                accumulatedExpression, context, accumulatedState,
                {ValueDomain::Scalar}).value.value == 15.0
            && talas::expr::evaluateStateful(
                accumulatedExpression, context, accumulatedState,
                {ValueDomain::Scalar}).value.value == 20.0,
        "accumulate should add one delta per accepted evaluation");
    values.index = 1.0;
    require(talas::expr::evaluateStateful(
                accumulatedExpression, context, accumulatedState,
                {ValueDomain::Scalar}).value.value == 10.0,
        "accumulate reset should restore the declared initial value");

    const auto held = talas::expr::parse(
        "sampleHold(!First, !Index, -1)",
        ExpressionLanguageVersion::SessionV1);
    const auto& heldExpression = requireExpression(
        held,
        "SessionV1 sampleHold fixture");
    talas::expr::ExpressionState heldState;
    require(talas::expr::prepareExpressionState(heldExpression, heldState),
        "sampleHold state should prepare");
    values.first = 3.0;
    values.index = 0.0;
    require(talas::expr::evaluateStateful(
                heldExpression, context, heldState,
                {ValueDomain::Scalar}).value.value == -1.0,
        "sampleHold should retain initial value before a rising edge");
    values.index = 1.0;
    require(talas::expr::evaluateStateful(
                heldExpression, context, heldState,
                {ValueDomain::Scalar}).value.value == 3.0,
        "sampleHold should sample on the first rising edge");
    values.first = 9.0;
    require(talas::expr::evaluateStateful(
                heldExpression, context, heldState,
                {ValueDomain::Scalar}).value.value == 3.0,
        "sampleHold should not resample while trigger remains high");

    const auto requireStatefulValue = [](
        std::string_view source,
        ValueDomain expectedDomain,
        double expectedValue,
        double sampleRate = 48000.0) {
        const auto parsedValue = talas::expr::parse(
            source,
            ExpressionLanguageVersion::SessionV1);
        const auto& expressionValue = requireExpression(
            parsedValue,
            "stateful domain fixture");
        talas::expr::ExpressionState state;
        require(
            talas::expr::prepareExpressionState(expressionValue, state),
            "stateful domain fixture should prepare");
        EvaluationContext domainContext;
        domainContext.sampleRate = sampleRate;
        const auto evaluated = talas::expr::evaluateStateful(
            expressionValue,
            domainContext,
            state,
            {expectedDomain});
        require(
            evaluated.ok()
                && evaluated.value.domain == expectedDomain
                && evaluated.value.value == expectedValue,
            "stateful operators should preserve compatible value domains");
    };
    requireStatefulValue("previous(1 hz, 2 hz)", ValueDomain::Hertz, 2.0);
    requireStatefulValue("accumulate(1 hz, 0, 2 hz)", ValueDomain::Hertz, 3.0);
    requireStatefulValue("sampleHold(440 hz, 1, 220 hz)", ValueDomain::Hertz, 440.0);
    requireStatefulValue("previous(480 samples, 1 s)", ValueDomain::Seconds, 1.0, 480.0);

    for (const auto invalidSource : {
             std::string_view{"previous(1 hz, 2)"},
             std::string_view{"accumulate(1 hz, 1 hz, 2 hz)"},
             std::string_view{"sampleHold(440 hz, 1 hz, 220 hz)"}})
    {
        const auto invalidParsed = talas::expr::parse(
            invalidSource,
            ExpressionLanguageVersion::SessionV1);
        const auto& invalidExpression = requireExpression(
            invalidParsed,
            "stateful invalid-domain fixture");
        talas::expr::ExpressionState invalidState;
        require(talas::expr::prepareExpressionState(
                    invalidExpression,
                    invalidState),
            "stateful invalid-domain state should prepare");
        const auto before = invalidState;
        const auto invalidResult = talas::expr::evaluateStateful(
            invalidExpression,
            {},
            invalidState,
            {ValueDomain::Hertz});
        require(
            !invalidResult.ok() && invalidState == before,
            "stateful domain failures should commit no state");
    }

    const auto missingRateParsed = talas::expr::parse(
        "previous(480 samples, 1 s)",
        ExpressionLanguageVersion::SessionV1);
    const auto& missingRateExpression = requireExpression(
        missingRateParsed,
        "stateful sample-rate failure fixture");
    talas::expr::ExpressionState missingRateState;
    require(talas::expr::prepareExpressionState(
                missingRateExpression,
                missingRateState),
        "stateful sample-rate failure state should prepare");
    EvaluationContext missingRateContext;
    missingRateContext.sampleRate = 0.0;
    require(
        !talas::expr::evaluateStateful(
             missingRateExpression,
             missingRateContext,
             missingRateState,
             {ValueDomain::Seconds}).ok(),
        "Samples-to-Seconds state domains should fail without a valid sample rate");

    talas::expr::ExpressionDefinitionLibrary definitions;
    definitions.definitions = {
        {"delay",
         {{"value", ValueDomain::Scalar}},
         ValueDomain::Scalar,
         "previous(value, 0)"},
    };
    const auto calls = talas::expr::parse(
        "delay(!First) + delay(!Step)",
        ExpressionLanguageVersion::SessionV1,
        definitions);
    const auto& callsExpression = requireExpression(
        calls,
        "stateful definition call-site fixture");
    require(callsExpression.stateSlotCount() == 2,
        "two inlined definition calls should own distinct state slots");
    talas::expr::ExpressionState callsState;
    require(talas::expr::prepareExpressionState(callsExpression, callsState),
        "definition call-site state should prepare");
    values.first = 2.0;
    values.step = 7.0;
    require(talas::expr::evaluateStateful(
                callsExpression, context, callsState,
                {ValueDomain::Scalar}).value.value == 0.0,
        "distinct stateful call sites should start independently");
    values.first = 4.0;
    values.step = 11.0;
    require(talas::expr::evaluateStateful(
                callsExpression, context, callsState,
                {ValueDomain::Scalar}).value.value == 9.0,
        "distinct stateful call sites should retain their own prior values");

    std::string tooManySlots;
    for (std::size_t index = 0;
         index <= talas::expr::ExpressionLimits::maximumStateSlots;
         ++index)
    {
        if (!tooManySlots.empty())
            tooManySlots += "+";
        tooManySlots += "previous(1,0)";
    }
    const auto overBudget = talas::expr::parse(
        tooManySlots,
        ExpressionLanguageVersion::SessionV1);
    require(
        !overBudget.ok()
            && overBudget.diagnostics.size() == 1
            && overBudget.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded,
        "state slot overflow should fail before publication");
}

void testVariablesV1BoundedVariableReferences()
{
    require(
        talas::expr::isCompiledExpressionLanguage(
                ExpressionLanguageVersion::VariablesV1)
            && talas::expr::supportsExpressionDefinitions(
                ExpressionLanguageVersion::VariablesV1)
            && talas::expr::supportsStatefulExpressions(
                ExpressionLanguageVersion::VariablesV1)
            && talas::expr::supportsVariableReferences(
                ExpressionLanguageVersion::VariablesV1)
            && !talas::expr::supportsVariableReferences(
                ExpressionLanguageVersion::SessionV1),
        "VariablesV1 should be the explicit bounded variable-reference profile");

    for (const auto frozen : {
             ExpressionLanguageVersion::LegacyV1,
             ExpressionLanguageVersion::PureV2,
             ExpressionLanguageVersion::PureV3,
             ExpressionLanguageVersion::PureV4,
             ExpressionLanguageVersion::SessionV1})
    {
        const auto rejected = talas::expr::parse("@gain", frozen);
        require(
            !rejected.ok()
                && rejected.diagnostics.size() == 1
                && rejected.diagnostics.front().code
                    == Diagnostic::Code::UnexpectedCharacter
                && rejected.diagnostics.front().range.offset == 0
                && rejected.diagnostics.front().range.length == 1,
            "historical profiles should preserve the exact pre-variable '@' diagnostic");
    }

    VariableResolverState variables;
    EvaluationContext context;
    context.user = &variables;
    context.resolveEvent = resolveEvent;
    context.resolveVariable = resolveVariable;
    context.resolveIdentifier = resolveIdentifier;
    const auto parsed = talas::expr::parse(
        "@gain + !Fader + pi + @gain",
        ExpressionLanguageVersion::VariablesV1);
    const auto& expression = requireExpression(
        parsed,
        "VariablesV1 separated resolver fixture");
    require(
        expression.variableReferences()
                == std::vector<std::string>{"gain"}
            && expression.eventReferences()
                == std::vector<std::string>{"Fader"}
            && expression.identifiers()
                == std::vector<std::string>{"pi"},
        "variable, event, and identifier references should retain separate deduped authority");
    require(
        expression.variableReferenceMetadata().size() == 2
            && expression.variableReferenceMetadata()[0].name == "gain"
            && expression.variableReferenceMetadata()[0].range.offset == 0
            && expression.variableReferenceMetadata()[0].range.length == 5
            && !expression.variableReferenceMetadata()[0]
                    .definitionIndex.has_value()
            && expression.variableReferenceMetadata()[1].range.offset == 22,
        "each variable use should retain immutable exact source metadata");
    const auto evaluated = talas::expr::evaluate(
        expression,
        context,
        {ValueDomain::Scalar});
    require(
        evaluated.ok()
            && evaluated.value.value
                == variables.gain * 2.0 + 0.5 + 3.14159265358979323846,
        "VariablesV1 should resolve '@' only through the variable resolver");

    auto missingContext = context;
    missingContext.resolveVariable = nullptr;
    const auto missing = talas::expr::evaluate(
        expression,
        missingContext,
        {ValueDomain::Scalar});
    require(
        !missing.ok()
            && missing.diagnostics.size() == 1
            && missing.diagnostics.front().code
                == Diagnostic::Code::UnknownVariable
            && missing.diagnostics.front().range.offset == 0
            && missing.diagnostics.front().range.length == 5
            && missing.diagnostics.front().message
                == "Unknown variable reference: gain",
        "missing variable authority should fail exactly without identifier fallback");

    talas::expr::ExpressionDefinitionLibrary definitions;
    definitions.definitions = {
        {"scaled",
         {{"value", ValueDomain::Scalar}},
         ValueDomain::Scalar,
         "value * @gain"},
    };
    const auto defined = talas::expr::parse(
        "scaled(4) + scaled(8)",
        ExpressionLanguageVersion::VariablesV1,
        definitions);
    const auto& definedExpression = requireExpression(
        defined,
        "VariablesV1 definition import fixture");
    require(
        definedExpression.variableReferences()
            == std::vector<std::string>{"gain"},
        "definition expansion should preserve and dedupe variable references");
    require(
        definedExpression.variableReferenceMetadata().size() == 2
            && definedExpression.variableReferenceMetadata()[0]
                    .definitionIndex == 0
            && definedExpression.variableReferenceMetadata()[1]
                    .definitionIndex == 0,
        "definition-expanded variable uses should retain their exact definition authority");
    const auto definedValue = talas::expr::evaluate(
        definedExpression,
        context,
        {ValueDomain::Scalar});
    require(
        definedValue.ok() && definedValue.value.value == 3.0,
        "definition-expanded variable references should evaluate through the same authority");

    const auto stateful = talas::expr::parse(
        "previous(@gain, 0) + (1 / @step)",
        ExpressionLanguageVersion::VariablesV1);
    const auto& statefulExpression = requireExpression(
        stateful,
        "VariablesV1 transactional state fixture");
    talas::expr::ExpressionState state;
    require(
        talas::expr::prepareExpressionState(statefulExpression, state),
        "VariablesV1 state should prepare against fixed slots");
    require(
        talas::expr::evaluateStateful(
            statefulExpression,
            context,
            state,
            {ValueDomain::Scalar}).ok(),
        "VariablesV1 state fixture should establish a last-good state");
    const auto lastGood = state;
    variables.gain = 0.75;
    variables.step = 0.0;
    require(
        !talas::expr::evaluateStateful(
             statefulExpression,
             context,
             state,
             {ValueDomain::Scalar}).ok()
            && state == lastGood,
        "variable resolution/evaluation failure should roll back the entire state transaction");

    const auto invalid = talas::expr::parse(
        "@",
        ExpressionLanguageVersion::VariablesV1);
    require(
        !invalid.ok()
            && invalid.diagnostics.size() == 1
            && invalid.diagnostics.front().code
                == Diagnostic::Code::ExpectedExpression
            && invalid.diagnostics.front().message
                == "Expected variable name after '@'",
        "VariablesV1 should reject an empty variable token exactly");

    std::string tooManyVariables;
    for (std::size_t index = 0;
         index <= talas::expr::ExpressionLimits::maximumVariableReferences;
         ++index)
    {
        if (!tooManyVariables.empty())
            tooManyVariables += "+";
        tooManyVariables += "@v" + std::to_string(index);
    }
    const auto overBudget = talas::expr::parse(
        tooManyVariables,
        ExpressionLanguageVersion::VariablesV1);
    require(
        !overBudget.ok()
            && overBudget.diagnostics.size() == 1
            && overBudget.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded,
        "variable-reference overflow should fail before publication");
}

void testVariablesV2CheckedRemainderExpressions()
{
    require(
        talas::expr::latestExpressionLanguage
                == ExpressionLanguageVersion::VariablesV2
            && talas::expr::isCompiledExpressionLanguage(
                ExpressionLanguageVersion::VariablesV2)
            && talas::expr::supportsExpressionDefinitions(
                ExpressionLanguageVersion::VariablesV2)
            && talas::expr::supportsStatefulExpressions(
                ExpressionLanguageVersion::VariablesV2)
            && talas::expr::supportsVariableReferences(
                ExpressionLanguageVersion::VariablesV2),
        "VariablesV2 should inherit the complete frozen VariablesV1 profile");

    for (const auto frozen : {
             ExpressionLanguageVersion::LegacyV1,
             ExpressionLanguageVersion::PureV2,
             ExpressionLanguageVersion::PureV3,
             ExpressionLanguageVersion::PureV4,
             ExpressionLanguageVersion::SessionV1,
             ExpressionLanguageVersion::VariablesV1})
    {
        const auto rejected = talas::expr::parse("5 rem: 2", frozen);
        require(
            !rejected.ok()
                && rejected.diagnostics.size() == 2
                && rejected.diagnostics[0].code
                    == Diagnostic::Code::UnknownUnit
                && rejected.diagnostics.front().range.offset == 2
                && rejected.diagnostics.front().range.length == 3
                && rejected.diagnostics[1].code
                    == Diagnostic::Code::UnexpectedCharacter
                && rejected.diagnostics[1].range.offset == 5
                && rejected.diagnostics[1].range.length == 1,
            "every historical profile should retain its exact pre-rem unit/colon diagnostics");
    }

    const auto evaluateRemainder = [](std::string_view source) {
        const auto parsed = talas::expr::parse(
            source,
            ExpressionLanguageVersion::VariablesV2);
        const auto& expression = requireExpression(
            parsed,
            std::string(source));
        const auto interpreted = talas::expr::evaluate(
            expression,
            {},
            {ValueDomain::Scalar});
        require(
            interpreted.ok()
                && interpreted.value.domain == ValueDomain::Scalar,
            std::string(source)
                + " should evaluate through checked VariablesV2 semantics");
        ExpressionValue prepared;
        require(
            talas::expr::tryEvaluate(
                expression,
                {},
                {ValueDomain::Scalar},
                prepared)
                && prepared == interpreted.value,
            std::string(source)
                + " should retain exact interpreter/prepared parity");
        return interpreted.value.value;
    };

    require(
        evaluateRemainder("5 rem: 2") == 1.0
            && evaluateRemainder("5.5 rem: 2") == 1.5
            && evaluateRemainder("-5 rem: 2") == -1.0
            && evaluateRemainder("5 rem: -2") == 1.0
            && evaluateRemainder("2 rem: 5") == 2.0
            && evaluateRemainder("20 rem: 6 rem: 4") == 2.0
            && evaluateRemainder("10 + 7 rem: 4 * 2") == 16.0,
        "rem: should use truncation-toward-zero remainder, multiplicative precedence, and left associativity");

    const auto normalizedZero = evaluateRemainder("-4 rem: 2");
    require(
        normalizedZero == 0.0 && !std::signbit(normalizedZero),
        "rem: should normalize an exact remainder to positive zero");

    for (const auto spelling : {
             "5 rem 2",
             "5 Rem: 2",
             "5 rem : 2",
             "5 % 2"})
    {
        require(
            !talas::expr::parse(
                 spelling,
                 ExpressionLanguageVersion::VariablesV2).ok(),
            std::string(spelling)
                + " should not alias exact case-sensitive compound rem:");
    }

    const auto zeroDivisor = talas::expr::parse(
        "5 rem: 0",
        ExpressionLanguageVersion::VariablesV2);
    const auto& zeroDivisorExpression = requireExpression(
        zeroDivisor,
        "VariablesV2 zero-divisor remainder fixture");
    const auto zeroDivisorResult = talas::expr::evaluate(
        zeroDivisorExpression,
        {},
        {ValueDomain::Scalar});
    ExpressionValue ignored;
    require(
        !zeroDivisorResult.ok()
            && zeroDivisorResult.diagnostics.size() == 1
            && zeroDivisorResult.diagnostics.front().code
                == Diagnostic::Code::DivisionByZero
            && !talas::expr::tryEvaluate(
                zeroDivisorExpression,
                {},
                {ValueDomain::Scalar},
                ignored),
        "remainder by zero should fail identically in checked and prepared evaluation");

    const auto unitMismatch = talas::expr::parse(
        "5 hz rem: 2",
        ExpressionLanguageVersion::VariablesV2);
    const auto& unitMismatchExpression = requireExpression(
        unitMismatch,
        "VariablesV2 remainder domain fixture");
    const auto unitMismatchResult = talas::expr::evaluate(
        unitMismatchExpression,
        {},
        {ValueDomain::Hertz});
    require(
        !unitMismatchResult.ok()
            && unitMismatchResult.diagnostics.size() == 1
            && unitMismatchResult.diagnostics.front().code
                == Diagnostic::Code::UnitMismatch
            && !talas::expr::tryEvaluate(
                unitMismatchExpression,
                {},
                {ValueDomain::Hertz},
                ignored),
        "rem: should reject every unit-bearing operand in both evaluators");

    talas::expr::ExpressionDefinitionLibrary definitions;
    definitions.definitions = {
        {"wrap",
         {{"value", ValueDomain::Scalar}},
         ValueDomain::Scalar,
         "value rem: 3"},
    };
    const auto defined = talas::expr::parse(
        "wrap(8)",
        ExpressionLanguageVersion::VariablesV2,
        definitions);
    const auto& definedExpression = requireExpression(
        defined,
        "VariablesV2 definition-expanded remainder fixture");
    const auto definedResult = talas::expr::evaluate(
        definedExpression,
        {},
        {ValueDomain::Scalar});
    require(
        definedResult.ok() && definedResult.value.value == 2.0,
        "VariablesV2 definitions should compile rem: through the same operator authority");

    VariableResolverState rangeState;
    EvaluationContext rangeContext;
    rangeContext.user = &rangeState;
    rangeContext.resolveVariable = resolveVariable;
    const auto ranged = talas::expr::parse(
        "  @gain rem: 0  ",
        ExpressionLanguageVersion::VariablesV2);
    const auto& rangedExpression = requireExpression(
        ranged,
        "VariablesV2 remainder source-range fixture");
    const auto rangedResult = talas::expr::evaluate(
        rangedExpression,
        rangeContext,
        {ValueDomain::Scalar});
    require(
        rangedExpression.variableReferenceMetadata().size() == 1
            && rangedExpression.variableReferenceMetadata().front().range
                == SourceRange{2, 5}
            && !rangedResult.ok()
            && rangedResult.diagnostics.size() == 1
            && rangedResult.diagnostics.front().code
                == Diagnostic::Code::DivisionByZero
            && rangedResult.diagnostics.front().range
                == SourceRange{2, 12}
            && !talas::expr::tryEvaluate(
                rangedExpression,
                rangeContext,
                {ValueDomain::Scalar},
                ignored),
        "VariablesV2 remainder should retain exact success metadata and checked error ranges in both evaluators");

    rangeState.gain = std::numeric_limits<double>::infinity();
    for (const auto& [source, expectedRange] : {
             std::pair<std::string_view, SourceRange>{
                 "@gain rem: 2", {0, 5}},
             std::pair<std::string_view, SourceRange>{
                 "2 rem: @gain", {7, 5}},
         })
    {
        const auto nonFinite = talas::expr::parse(
            source,
            ExpressionLanguageVersion::VariablesV2);
        const auto& nonFiniteExpression = requireExpression(
            nonFinite,
            std::string(source));
        const auto nonFiniteResult = talas::expr::evaluate(
            nonFiniteExpression,
            rangeContext,
            {ValueDomain::Scalar});
        require(
            !nonFiniteResult.ok()
                && nonFiniteResult.diagnostics.size() == 1
                && nonFiniteResult.diagnostics.front().code
                    == Diagnostic::Code::NonFiniteResult
                && nonFiniteResult.diagnostics.front().range
                    == expectedRange
                && !talas::expr::tryEvaluate(
                    nonFiniteExpression,
                    rangeContext,
                    {ValueDomain::Scalar},
                    ignored),
            std::string(source)
                + " should reject its non-finite remainder operand at the exact source range in both evaluators");
    }

    const auto repeatedRemainder = [](std::size_t operandCount) {
        std::string source = "1";
        for (std::size_t index = 1; index < operandCount; ++index)
            source += " rem: 1";
        return source;
    };
    const auto maximumNodeSource =
        "-(" + repeatedRemainder(128) + ')';
    const auto maximumNodeRemainder = talas::expr::parse(
        maximumNodeSource,
        ExpressionLanguageVersion::VariablesV2);
    require(
        maximumNodeRemainder.ok()
            && maximumNodeRemainder.expression->instructionCount()
                == talas::expr::ExpressionLimits::maximumInstructions,
        "VariablesV2 remainder chain should admit the exact syntax-node boundary");
    const auto excessiveNodeRemainder = talas::expr::parse(
        "-(" + maximumNodeSource + ')',
        ExpressionLanguageVersion::VariablesV2);
    require(
        !excessiveNodeRemainder.ok()
            && excessiveNodeRemainder.diagnostics.size() == 1
            && excessiveNodeRemainder.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded
            && excessiveNodeRemainder.diagnostics.front().message.find(
                   "syntax-node")
                != std::string::npos,
        "VariablesV2 remainder chain should reject the first syntax node beyond the fixed boundary");

    const auto conditionalRemainder = [](std::size_t operandCount) {
        std::string source = "if(1,1,1)";
        for (std::size_t index = 1; index < operandCount; ++index)
            source += " rem: if(1,1,1)";
        return source;
    };
    auto maximumInstructionSource = conditionalRemainder(42);
    for (std::size_t index = 0; index < 5; ++index)
        maximumInstructionSource = "abs(" + maximumInstructionSource + ')';
    const auto maximumInstructionRemainder = talas::expr::parse(
        maximumInstructionSource,
        ExpressionLanguageVersion::VariablesV2);
    require(
        maximumInstructionRemainder.ok()
            && maximumInstructionRemainder.expression->instructionCount()
                == talas::expr::ExpressionLimits::maximumInstructions,
        "VariablesV2 remainder program should admit the largest complete conditional shape within the instruction boundary");
    const auto excessiveInstructionRemainder = talas::expr::parse(
        "abs(" + maximumInstructionSource + ')',
        ExpressionLanguageVersion::VariablesV2);
    require(
        !excessiveInstructionRemainder.ok()
            && excessiveInstructionRemainder.diagnostics.size() == 1
            && excessiveInstructionRemainder.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded
            && excessiveInstructionRemainder.diagnostics.front().message.find(
                   "instruction")
                != std::string::npos,
        "VariablesV2 remainder program should reject the first complete conditional shape beyond the instruction boundary");

    std::string maximumTokenRemainder =
        "clamp(1 hz,1 hz,1 hz)";
    for (std::size_t index = 1; index < 50; ++index)
    {
        maximumTokenRemainder += index == 1
            ? " rem: clamp(1 hz,1,1)"
            : " rem: clamp(1,1,1)";
    }
    for (std::size_t index = 0; index < 29; ++index)
        maximumTokenRemainder = '(' + maximumTokenRemainder + ')';
    const auto maximumTokenExpression = talas::expr::parse(
        maximumTokenRemainder,
        ExpressionLanguageVersion::VariablesV2);
    require(
        maximumTokenExpression.ok()
            && maximumTokenExpression.expression->instructionCount()
                == 249,
        "compound rem: tokens should admit the exact token boundary below the node and instruction limits");
    auto excessiveTokenRemainder = maximumTokenRemainder;
    const auto plainClamp = excessiveTokenRemainder.find(
        "clamp(1,1,1)");
    require(
        plainClamp != std::string::npos,
        "VariablesV2 token-boundary fixture should retain one plain clamp call");
    excessiveTokenRemainder.replace(
        plainClamp,
        std::string_view{"clamp(1,1,1)"}.size(),
        "clamp(1 hz,1,1)");
    const auto excessiveTokenExpression = talas::expr::parse(
        excessiveTokenRemainder,
        ExpressionLanguageVersion::VariablesV2);
    require(
        !excessiveTokenExpression.ok()
            && excessiveTokenExpression.diagnostics.size() == 1
            && excessiveTokenExpression.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded
            && excessiveTokenExpression.diagnostics.front().message.find(
                   "token")
                != std::string::npos,
        "compound rem: tokens should reject the first additional valid token beyond the fixed token boundary");
}

void testFixedBudgetsFailClosed()
{
    std::string maximumSource = "1";
    maximumSource.append(
        talas::expr::ExpressionLimits::maximumSourceBytes - 1,
        ' ');
    require(
        talas::expr::parse(
            maximumSource,
            ExpressionLanguageVersion::PureV2).ok(),
        "source at the fixed byte boundary should parse");
    maximumSource.push_back(' ');
    const auto oversizedSource = talas::expr::parse(
        maximumSource,
        ExpressionLanguageVersion::PureV2);
    require(!oversizedSource.ok(), "oversized source should fail closed");
    require(
        oversizedSource.diagnostics.size() == 1
            && oversizedSource.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded,
        "oversized source should have one terminal budget diagnostic");

    std::string tooDeep = "1";
    for (std::size_t index = 0;
         index <= talas::expr::ExpressionLimits::maximumNestingDepth;
         ++index)
    {
        tooDeep = "(" + tooDeep + ")";
    }
    const auto deepResult = talas::expr::parse(
        tooDeep,
        ExpressionLanguageVersion::PureV2);
    require(!deepResult.ok(), "excessive nesting should fail closed");
    require(
        deepResult.diagnostics.size() == 1
            && deepResult.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded,
        "nesting overflow should have one terminal budget diagnostic");

    std::string references;
    for (std::size_t index = 0;
         index <= talas::expr::ExpressionLimits::maximumIdentifiers;
         ++index)
    {
        if (!references.empty())
            references += "+";
        references += "v" + std::to_string(index);
    }
    const auto referenceResult = talas::expr::parse(
        references,
        ExpressionLanguageVersion::PureV2);
    require(!referenceResult.ok(), "excessive references should fail closed");
    require(
        referenceResult.diagnostics.size() == 1
            && referenceResult.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded
            && referenceResult.diagnostics.front().message.find(
                   "identifier limit")
                != std::string::npos,
        "reference overflow should have one terminal budget diagnostic");

    std::string eventReferences;
    for (std::size_t index = 0;
         index <= talas::expr::ExpressionLimits::maximumEventReferences;
         ++index)
    {
        if (!eventReferences.empty())
            eventReferences += "+";
        eventReferences += "!Event" + std::to_string(index);
    }
    const auto eventReferenceResult = talas::expr::parse(
        eventReferences,
        ExpressionLanguageVersion::PureV2);
    require(
        !eventReferenceResult.ok()
            && eventReferenceResult.diagnostics.size() == 1
            && eventReferenceResult.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded
            && eventReferenceResult.diagnostics.front().message.find(
                   "event-reference limit")
                != std::string::npos,
        "event-reference overflow should retain its structured budget kind");

    std::string nodeHeavy = "1";
    for (std::size_t index = 1; index < 129; ++index)
        nodeHeavy += "+1";
    const auto nodeResult = talas::expr::parse(
        nodeHeavy,
        ExpressionLanguageVersion::PureV2);
    require(!nodeResult.ok(), "syntax-node overflow should fail closed");
    require(
        nodeResult.diagnostics.size() == 1
            && nodeResult.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded
            && nodeResult.diagnostics.front().message.find("syntax-node")
                != std::string::npos,
        "syntax-node overflow should have one terminal budget diagnostic");

    std::string instructionHeavy;
    for (std::size_t index = 0; index < 43; ++index)
    {
        if (!instructionHeavy.empty())
            instructionHeavy += "+";
        instructionHeavy += "if(1,1,1)";
    }
    const auto instructionResult = talas::expr::parse(
        instructionHeavy,
        ExpressionLanguageVersion::PureV2);
    require(!instructionResult.ok(), "instruction overflow should fail closed");
    require(
        instructionResult.diagnostics.size() == 1
            && instructionResult.diagnostics.front().code
                == Diagnostic::Code::BudgetExceeded
            && instructionResult.diagnostics.front().message.find("instruction")
                != std::string::npos,
        "instruction overflow should have one terminal budget diagnostic");
}

void testFastEvaluatorDoesNotAllocate()
{
    const auto parsed = talas::expr::parse(
        "[!Missing, if(!Fader > 0, map(!Fader, 0, 1, 100 hz, 200 hz), 50 hz)][1]",
        ExpressionLanguageVersion::PureV2);
    const auto& expression = requireExpression(
        parsed,
        "allocation-free pure-v2 fixture");
    EvaluationContext context;
    context.resolveEvent = resolveEvent;
    ExpressionValue result;

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto evaluated = talas::expr::tryEvaluate(
        expression,
        context,
        EvaluationOptions{ValueDomain::Hertz},
        result);
    countAllocations.store(false, std::memory_order_release);

    require(evaluated, "fast pure-v2 evaluator should succeed");
    requireNear(
        result.value,
        150.0,
        1.0e-12,
        "fast pure-v2 evaluator should return the expected value");
    require(
        allocationCount.load(std::memory_order_relaxed) == 0,
        "fast pure-v2 evaluator should not allocate");

    const auto seriesParsed = talas::expr::parse(
        "series(!First, !Step, !Count)[!Index]",
        ExpressionLanguageVersion::PureV3);
    const auto& seriesExpression = requireExpression(
        seriesParsed,
        "allocation-free PureV3 series fixture");
    SeriesResolverState state;
    EvaluationContext seriesContext;
    seriesContext.user = &state;
    seriesContext.resolveEvent = resolveSeriesEvent;
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto seriesEvaluated = talas::expr::tryEvaluate(
        seriesExpression,
        seriesContext,
        EvaluationOptions{ValueDomain::Scalar},
        result);
    countAllocations.store(false, std::memory_order_release);
    require(
        seriesEvaluated && result.value == 20.0
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "fast PureV3 series evaluator should be allocation-free");

    talas::expr::ExpressionDefinitionLibrary definitions;
    definitions.definitions = {
        {"twice",
         {{"value", ValueDomain::Scalar}},
         ValueDomain::Scalar,
         "value + value"},
    };
    const auto definedParsed = talas::expr::parse(
        "twice(!First)",
        ExpressionLanguageVersion::PureV4,
        definitions);
    const auto& definedExpression = requireExpression(
        definedParsed,
        "allocation-free PureV4 definition fixture");
    SeriesResolverState definedState;
    EvaluationContext definedContext;
    definedContext.user = &definedState;
    definedContext.resolveEvent = resolveSeriesEvent;
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto definedEvaluated = talas::expr::tryEvaluate(
        definedExpression,
        definedContext,
        EvaluationOptions{ValueDomain::Scalar},
        result);
    countAllocations.store(false, std::memory_order_release);
    require(
        definedEvaluated && result.value == 20.0
            && definedState.firstCalls == 1
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "fast PureV4 definition evaluator should use fixed locals without allocation");

    const auto statefulParsed = talas::expr::parse(
        "previous(!First, 0)",
        ExpressionLanguageVersion::SessionV1);
    const auto& statefulExpression = requireExpression(
        statefulParsed,
        "allocation-free SessionV1 fixture");
    talas::expr::ExpressionState expressionState;
    require(talas::expr::prepareExpressionState(
                statefulExpression,
                expressionState),
        "SessionV1 fixed state should prepare");
    definedState = {};
    definedContext.user = &definedState;
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto statefulEvaluated = talas::expr::tryEvaluateStateful(
        statefulExpression,
        definedContext,
        expressionState,
        EvaluationOptions{ValueDomain::Scalar},
        result);
    countAllocations.store(false, std::memory_order_release);
    require(
        statefulEvaluated && result.value == 0.0
            && definedState.firstCalls == 1
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "fast SessionV1 evaluator should use fixed transactional state without allocation");
}

void testStructuralEquivalenceUsesExactCompiledAuthority()
{
    const auto first = talas::expr::parse(
        "!Value * 2",
        ExpressionLanguageVersion::SessionV1);
    const auto identical = talas::expr::parse(
        "!Value * 2",
        ExpressionLanguageVersion::SessionV1);
    const auto differentTree = talas::expr::parse(
        "2 * !Value",
        ExpressionLanguageVersion::SessionV1);
    const auto differentLanguage = talas::expr::parse(
        "!Value * 2",
        ExpressionLanguageVersion::PureV2);
    require(
        first.ok() && identical.ok()
            && differentTree.ok()
            && differentLanguage.ok(),
        "structural-equivalence fixtures should parse");
    require(
        first.expression->structurallyEquivalent(
            *identical.expression)
            && !first.expression->structurallyEquivalent(
                *differentTree.expression)
            && !first.expression->structurallyEquivalent(
                *differentLanguage.expression),
        "structural equivalence should accept only the exact compiled expression authority");
}
void testVoiceNumberRequiresAnExplicitLanguageProfile()
{
    for (const auto language : {ExpressionLanguageVersion::LegacyV1,
             ExpressionLanguageVersion::PureV2, ExpressionLanguageVersion::PureV3,
             ExpressionLanguageVersion::PureV4, ExpressionLanguageVersion::SessionV1,
             ExpressionLanguageVersion::VariablesV1, ExpressionLanguageVersion::VariablesV2,
             ExpressionLanguageVersion::VariablesV3})
        require(!talas::expr::parse("?VoiceNumber * 0.1 + 0.001", language).ok(),
            "existing expression languages must not gain implicit green-variable syntax");
    const auto voiceLanguage = ExpressionLanguageVersion::VoiceV1;
    require(talas::expr::latestExpressionLanguage == ExpressionLanguageVersion::VariablesV2,
        "an unintegrated voice profile must not silently become the project default");
    for (const auto* invalid : {"?", "?voiceNumber", "?VoiceNumber2", "?Other", "? VoiceNumber", "?VoiceNumber()"})
        require(!talas::expr::parse(invalid, voiceLanguage).ok(), "only the exact implicit voice token is defined");
    require(talas::expr::parse("?VoiceNumber * 0.1 + 0.001", voiceLanguage).ok(),
        "the explicit voice profile must parse the documented per-voice seed expression");
}
void testVoiceNumberBindingIsImmutableAndTyped()
{
    const auto parsed = talas::expr::parse("?VoiceNumber * 0.1 + 0.001", ExpressionLanguageVersion::VoiceV1);
    require(parsed.ok() && parsed.expression->usesVoiceNumber(), "voice seed template must remain unbound");
    const auto original = *parsed.expression;
    for (const std::uint16_t ordinal : {1, 2, 128, 65535})
    {
        const auto bound = original.specializeVoiceNumber(ordinal);
        require(bound.has_value() && !bound->usesVoiceNumber() && bound->identifiers().empty(),
            "specialization must remove the implicit identifier completely");
        require(!bound->sharesCompiledStorageWith(original) && original.usesVoiceNumber()
            && original.structurallyEquivalent(*parsed.expression),
            "binding must leave the source and its aliases unchanged");
        require(bound->instructionCount() == original.instructionCount()
            && bound->maximumStackDepth() == original.maximumStackDepth(),
            "literal binding must preserve instruction and stack bounds");
        const auto value = talas::expr::evaluate(*bound, {});
        require(value.ok() && value.value.domain == ValueDomain::Scalar, "voice number is a scalar constant");
        requireNear(value.value.value, ordinal * 0.1 + 0.001, 1e-12, "voice seed formula must use the exact ordinal");
        const auto alreadyBound = bound->specializeVoiceNumber(7);
        require(alreadyBound && alreadyBound->sharesCompiledStorageWith(*bound),
            "specializing a completed literal must preserve it, not invent a new binding");
    }
    require(!original.specializeVoiceNumber(0).has_value(), "zero cannot identify a one-based voice");
    const auto first = original.specializeVoiceNumber(1);
    const auto second = original.specializeVoiceNumber(2);
    require(first && second && !first->structurallyEquivalent(*second),
        "separate voice constants must have distinct expression authority");
    const auto ordinary = talas::expr::parse("3 + 4", ExpressionLanguageVersion::VariablesV2);
    const auto unchanged = ordinary.expression->specializeVoiceNumber(1);
    require(unchanged && unchanged->sharesCompiledStorageWith(*ordinary.expression),
        "old expressions without voice markers need no new storage");
    for (const auto& item : std::vector<std::pair<std::string, ValueDomain>>{
            {"?VoiceNumber * 3 hz", ValueDomain::Hertz},
            {"?VoiceNumber * 0.5 s", ValueDomain::Seconds},
            {"?VoiceNumber * 2 nn", ValueDomain::NoteNumber}})
    {
        const auto expression = talas::expr::parse(item.first, ExpressionLanguageVersion::VoiceV1);
        const auto bound = expression.expression->specializeVoiceNumber(2);
        require(bound && talas::expr::evaluate(*bound, {}, {item.second}).ok(),
            "voice scalar must compose with explicitly typed literal units");
    }
}

void testUnboundVoiceNumberCannotBecomeAResolverValue()
{
    for (const auto* source : {"?VoiceNumber", "if(0, ?VoiceNumber, 42)",
             "accumulate(?VoiceNumber, 0, 0)", "if(0, accumulate(?VoiceNumber, 0, 0), 42)"})
    {
        const auto parsed = talas::expr::parse(source, ExpressionLanguageVersion::VoiceV1);
        require(parsed.ok() && parsed.expression->usesVoiceNumber(), "unbound test template must compile");
        std::size_t resolverCalls = 0;
        EvaluationContext context;
        context.user = &resolverCalls;
        context.resolveIdentifier = [](void* user, std::string_view, ExpressionValue& value) noexcept {
            ++*static_cast<std::size_t*>(user); value = {999, ValueDomain::Scalar}; return true;
        };
        ExpressionValue output{123, ValueDomain::Hertz};
        talas::expr::ExpressionState state;
        require(talas::expr::prepareExpressionState(*parsed.expression, state), "template state shape must prepare");
        const auto before = state;
        const auto evaluated = parsed.expression->isStateful()
            ? talas::expr::evaluateStateful(*parsed.expression, context, state, {})
            : talas::expr::evaluate(*parsed.expression, context);
        require(!evaluated.ok() && evaluated.diagnostics.front().code == Diagnostic::Code::UnknownIdentifier
            && evaluated.diagnostics.front().range.length == std::string_view{"?VoiceNumber"}.size(),
            "an unbound marker must fail at its source even on an inactive branch");
        const auto ok = parsed.expression->isStateful()
            ? talas::expr::tryEvaluateStateful(*parsed.expression, context, state, {}, output)
            : talas::expr::tryEvaluate(*parsed.expression, context, {}, output);
        require(!ok && output.value == 123 && output.domain == ValueDomain::Hertz
            && state == before && resolverCalls == 0,
            "unbound evaluation must not call a resolver, commit state, or overwrite caller output");
    }
}

void testVoiceNumberPreservesOtherBindingsAndDefinitions()
{
    for (const auto* source : {"?VoiceNumber + A * 10 + B", "A * 10 + ?VoiceNumber + B",
             "A * 10 + B + ?VoiceNumber"})
    {
        const auto parsed = talas::expr::parse(source, ExpressionLanguageVersion::VoiceV1);
        require(parsed.ok(), "identifier-remapping fixture must parse");
        const auto bound = parsed.expression->specializeVoiceNumber(4);
        require(bound && bound->identifiers() == std::vector<std::string>{"A", "B"},
            "only the implicit identifier may disappear");
        EvaluationContext context;
        context.resolveIdentifier = [](void*, std::string_view name, ExpressionValue& value) noexcept {
            if (name != "A" && name != "B") return false;
            value = {name == "A" ? 2.0 : 3.0, ValueDomain::Scalar}; return true;
        };
        const auto result = talas::expr::evaluate(*bound, context);
        require(result.ok() && result.value.value == 27,
            "remaining identifier indices must survive first/middle/last marker removal");
    }
    talas::expr::ExpressionDefinitionLibrary definitions;
    definitions.definitions = {
        {"voiceSeed", {{"amount", ValueDomain::Scalar}}, ValueDomain::Scalar,
            "?VoiceNumber * amount + Other"},
        {"outerSeed", {{"amount", ValueDomain::Scalar}}, ValueDomain::Scalar,
            "voiceSeed(amount) + voiceSeed(amount * 2)"},
    };
    const auto parsed = talas::expr::parse("outerSeed(!Amount) + @Bias",
        ExpressionLanguageVersion::VoiceV1, definitions);
    require(parsed.ok() && parsed.expression->usesVoiceNumber(),
        "markers introduced by nested definitions must remain discoverable");
    const auto bound = parsed.expression->specializeVoiceNumber(3);
    require(bound && bound->eventReferences() == parsed.expression->eventReferences()
        && bound->variableReferences() == parsed.expression->variableReferences()
        && bound->variableReferenceMetadata() == parsed.expression->variableReferenceMetadata(),
        "voice specialization must preserve event and variable authority");
    std::size_t amountCalls = 0;
    EvaluationContext context;
    context.user = &amountCalls;
    context.resolveEvent = [](void* user, std::string_view name, ExpressionValue& value) noexcept {
        if (name != "Amount") return false;
        ++*static_cast<std::size_t*>(user); value = {0.1, ValueDomain::Scalar}; return true;
    };
    context.resolveVariableIndex = [](void*, std::size_t index, ExpressionValue& value) noexcept {
        if (index != 0) return false;
        value = {0.25, ValueDomain::Scalar}; return true;
    };
    context.resolveIdentifier = [](void*, std::string_view name, ExpressionValue& value) noexcept {
        if (name != "Other") return false;
        value = {5, ValueDomain::Scalar}; return true;
    };
    const auto result = talas::expr::evaluate(*bound, context);
    require(result.ok() && amountCalls == 1, "argument evaluation locals must survive specialization");
    requireNear(result.value.value, 11.15, 1e-12, "nested definitions must share the exact voice constant");
    const auto unbound = talas::expr::evaluate(*parsed.expression, context);
    require(!unbound.ok() && unbound.diagnostics.front().definitionIndex == std::optional<std::size_t>{0},
        "unbound diagnostics must retain their definition source identity");
}

void testBoundVoiceNumberStateAndEvaluationDoNotAllocate()
{
    const auto parsed = talas::expr::parse("accumulate(?VoiceNumber, 0, 0) + (?VoiceNumber rem: 2)",
        ExpressionLanguageVersion::VoiceV1);
    require(parsed.ok() && parsed.expression->isStateful(), "voice profile must retain state and remainder features");
    const auto one = parsed.expression->specializeVoiceNumber(1);
    const auto two = parsed.expression->specializeVoiceNumber(2);
    require(one && two && one->stateSlotCount() == parsed.expression->stateSlotCount(),
        "specialization must preserve the prepared state shape");
    talas::expr::ExpressionState first, second;
    require(talas::expr::prepareExpressionState(*one, first) && talas::expr::prepareExpressionState(*two, second),
        "each bound voice must prepare independent state");
    for (std::size_t frame = 0; frame < 16; ++frame)
    {
        ExpressionValue a, b;
        allocationCount = 0; countAllocations = true;
        const auto okA = talas::expr::tryEvaluateStateful(*one, {}, first, {}, a);
        const auto okB = talas::expr::tryEvaluateStateful(*two, {}, second, {}, b);
        countAllocations = false;
        require(okA && okB && allocationCount == 0, "bound voice expressions must run without allocation");
        require(a.value == static_cast<double>(frame + 2) && b.value == static_cast<double>(2 * (frame + 1)),
            "voice state and scalar binding must remain independent");
    }
    first.reset();
    ExpressionValue reset;
    require(talas::expr::tryEvaluateStateful(*one, {}, first, {}, reset) && reset.value == 2,
        "reset must retain the bound ordinal and clear only expression state");
    require(parsed.expression->usesVoiceNumber() && !one->usesVoiceNumber() && !two->usesVoiceNumber(),
        "rendering and reset must leave all expression storage immutable");
}

void testRampMessageUsesMillisecondSoundClock()
{
    const auto parsed = talas::expr::parse(
        "1 ramp: 4 ms", ExpressionLanguageVersion::VariablesV3);
    require(parsed.ok() && parsed.expression->isStateful()
            && parsed.expression->usesTime()
            && parsed.expression->stateSlotCount() == 1,
        "ramp: should compile as a timed stateful parameter expression");
    require(!talas::expr::parse("1 ramp: 4 ms",
                ExpressionLanguageVersion::PureV4).ok(),
        "pure expressions must not acquire a runtime clock");
    require(!talas::expr::parse("1 ramp: 4 ms",
                ExpressionLanguageVersion::VariablesV2).ok(),
        "frozen VariablesV2 must continue rejecting ramp:");
    talas::expr::ExpressionState state;
    require(talas::expr::prepareExpressionState(*parsed.expression, state),
        "timed ramp state should prepare before realtime use");
    EvaluationContext context;
    context.sampleRate = 4000.0;
    constexpr std::array<std::pair<std::uint64_t, double>, 9> cases{{
        {0, 0.0}, {1, 0.0}, {3, 0.0},
        {4, 0.25}, {7, 0.25}, {8, 0.5},
        {12, 0.75}, {16, 1.0}, {20, 1.0},
    }};
    for (const auto& [frame, expected] : cases)
    {
        context.elapsedSampleFrame = frame;
        ExpressionValue result;
        allocationCount = 0;
        countAllocations = true;
        const auto ok = talas::expr::tryEvaluateStateful(
            *parsed.expression, context, state, {}, result);
        countAllocations = false;
        require(ok && allocationCount == 0,
            "ramp: evaluation should not allocate on the audio thread");
        requireNear(result.value, expected, 1e-12,
            "ramp: should hold between millisecond ticks");
    }
    const auto committed = state;
    context.elapsedSampleFrame = 12;
    ExpressionValue rejected;
    require(!talas::expr::tryEvaluateStateful(
                *parsed.expression, context, state, {}, rejected)
            && state == committed,
        "a regressing Sound clock should reject without changing ramp state");
    state.reset();
    context.elapsedSampleFrame = 0;
    ExpressionValue reset;
    require(talas::expr::tryEvaluateStateful(
                *parsed.expression, context, state, {}, reset)
            && reset.value == 0.0,
        "reset should restart a timed expression from its first tick");

    const auto triggered = talas::expr::parse(
        "!Gate ramp: 4 ms", ExpressionLanguageVersion::VariablesV3);
    require(triggered.ok() && triggered.expression->usesTime(),
        "an Event Value should be able to trigger a timed ramp");
    talas::expr::ExpressionState triggeredState;
    require(talas::expr::prepareExpressionState(
                *triggered.expression, triggeredState),
        "triggered ramp state should prepare");
    double gate = 1.0;
    context.user = &gate;
    context.resolveEvent = [](void* user, std::string_view name,
                              ExpressionValue& value) noexcept {
        if (name != "Gate")
            return false;
        value = {*static_cast<double*>(user), ValueDomain::Scalar};
        return true;
    };
    // Once triggered it runs on when the trigger falls; projects before
    // format 103 hold it.
    for (const auto holds : {false, true})
    {
        triggeredState.reset();
        context.rampsHoldUntriggered = holds;
        for (const auto& [frame, nextGate, expected] :
             std::array<std::tuple<std::uint64_t, double, double>, 5>{{
                 {0, 1.0, 0.0}, {4, 1.0, 0.25}, {8, 0.0, holds ? 0.25 : 0.5},
                 {12, 1.0, 0.0}, {16, 1.0, 0.25},
             }})
        {
            gate = nextGate;
            context.elapsedSampleFrame = frame;
            ExpressionValue value;
            require(talas::expr::tryEvaluateStateful(
                        *triggered.expression, context, triggeredState, {}, value),
                "triggered timed ramp should evaluate");
            requireNear(value.value, expected, 1e-12,
                "ramp: should restart on a positive trigger edge");
        }
    }
    context.rampsHoldUntriggered = false;
}

void testVariablesV4PostfixUnits()
{
    constexpr auto language = ExpressionLanguageVersion::VariablesV4;
    require(talas::expr::supportsPostfixUnits(language)
            && talas::expr::supportsTimedExpressions(language)
            && talas::expr::supportsVariableReferences(language)
            && talas::expr::supportsCheckedRemainder(language)
            && !talas::expr::isPureExpressionLanguage(language)
            && talas::expr::latestExpressionLanguage == ExpressionLanguageVersion::VariablesV2,
        "postfix units require an explicit profile retaining VariablesV3 authority");
    for (const auto frozen : {ExpressionLanguageVersion::LegacyV1,
             ExpressionLanguageVersion::PureV2, ExpressionLanguageVersion::PureV3,
             ExpressionLanguageVersion::PureV4, ExpressionLanguageVersion::PureV5,
             ExpressionLanguageVersion::SessionV1, ExpressionLanguageVersion::VariablesV1,
             ExpressionLanguageVersion::VariablesV2, ExpressionLanguageVersion::VariablesV3,
             ExpressionLanguageVersion::VoiceV1})
    {
        const auto rejected = talas::expr::parse("!Interval nn", frozen);
        require(!talas::expr::supportsPostfixUnits(frozen) && !rejected.ok()
                && rejected.diagnostics.size() == 1
                && rejected.diagnostics.front().code == Diagnostic::Code::TrailingInput
                && rejected.diagnostics.front().range == SourceRange{10, 2},
            "frozen profiles retain the exact trailing-unit rejection");
    }
    struct Input { ExpressionValue value{0, ValueDomain::Scalar}; std::size_t reads = 0; } input;
    EvaluationContext context;
    context.sampleRate = 48000;
    context.tempoBpm = 120;
    context.user = &input;
    context.resolveEvent = [](void* user, std::string_view name, ExpressionValue& value) noexcept {
        if (name != "Value") return false;
        auto& input = *static_cast<Input*>(user);
        ++input.reads;
        value = input.value;
        return true;
    };
    context.resolveVariable = context.resolveEvent;
    for (const auto& [source, scalar, domain, expected] : std::array{
             std::tuple{"60 nn + !Value nn", 4.5, ValueDomain::NoteNumber, 64.5},
             std::tuple{"!Value nn", 69.0, ValueDomain::Hertz, 440.0},
             std::tuple{"!Value hz", 220.0, ValueDomain::Hertz, 220.0},
             std::tuple{"!Value db", -6.020599913279624, ValueDomain::Scalar, 0.5},
             std::tuple{"(!Value * 5) ms", 20.0, ValueDomain::Seconds, 0.1},
             std::tuple{"!Value usec", 250000.0, ValueDomain::Seconds, 0.25},
             std::tuple{"!Value s", 0.75, ValueDomain::Seconds, 0.75},
             std::tuple{"!Value m", 2.0, ValueDomain::Seconds, 120.0},
             std::tuple{"!Value h", 2.0, ValueDomain::Seconds, 7200.0},
             std::tuple{"!Value days", 0.5, ValueDomain::Seconds, 43200.0},
             std::tuple{"!Value beats", 3.0, ValueDomain::Seconds, 1.5},
             std::tuple{"!Value samp", 24000.0, ValueDomain::Seconds, 0.5},
             std::tuple{"!Value samples", 24.0, ValueDomain::Samples, 24.0},
             std::tuple{"2 * !Value nn + 1 nn", 4.5, ValueDomain::NoteNumber, 10.0},
             std::tuple{"@Value ms", 20.0, ValueDomain::Seconds, 0.02},
             std::tuple{"if(0, !Missing nn, !Value nn)", 60.5, ValueDomain::NoteNumber, 60.5}})
    {
        const auto parsed = talas::expr::parse(source, language);
        require(parsed.ok(), std::string(source) + " postfix fixture parses");
        input.value = {scalar, ValueDomain::Scalar};
        const auto evaluated = talas::expr::evaluate(*parsed.expression, context, {domain});
        require(evaluated.ok(), std::string(source) + " evaluates");
        requireNear(evaluated.value.value, expected, 1e-10, source);
        input.reads = 0;
        ExpressionValue result;
        allocationCount = 0;
        countAllocations = true;
        const auto ok = talas::expr::tryEvaluate(*parsed.expression, context, {domain}, result);
        countAllocations = false;
        require(ok && allocationCount == 0 && input.reads == 1,
            "a postfix unit evaluates its operand once without allocating");
        requireNear(result.value, expected, 1e-10, source);
        require(result.domain == domain, "postfix result retains its declared domain");
    }
    const auto unknown = talas::expr::parse("!Value bogus", language);
    require(!unknown.ok() && unknown.diagnostics.front().code == Diagnostic::Code::UnknownUnit
            && unknown.diagnostics.front().range == SourceRange{7, 5},
        "unknown postfix units have the exact suffix range");
    const auto typed = talas::expr::parse("!Value nn", language);
    input.value = {60, ValueDomain::NoteNumber};
    const auto invalidDomain = talas::expr::evaluate(*typed.expression, context, {ValueDomain::NoteNumber});
    require(!invalidDomain.ok() && invalidDomain.diagnostics.front().code == Diagnostic::Code::UnitMismatch,
        "postfix units require scalar input instead of silently reinterpreting a typed value");
    const auto overflow = talas::expr::parse("!Value days", language);
    input.value = {1e308, ValueDomain::Scalar};
    const auto invalidValue = talas::expr::evaluate(*overflow.expression, context, {ValueDomain::Seconds});
    require(!invalidValue.ok() && invalidValue.diagnostics.front().code == Diagnostic::Code::NonFiniteResult,
        "unit scaling overflow fails closed");
    const auto beats = talas::expr::parse("!Value beats", language);
    input.value.value = 1;
    context.tempoBpm = 0;
    const auto missingTempo = talas::expr::evaluate(*beats.expression, context, {ValueDomain::Seconds});
    require(!missingTempo.ok() && missingTempo.diagnostics.front().code == Diagnostic::Code::MissingTempo,
        "dynamic beat units require a valid tempo");
    std::string overBudget = "!Value";
    for (std::size_t i = 0; i < talas::expr::ExpressionLimits::maximumNodes; ++i) overBudget += " nn";
    const auto oversized = talas::expr::parse(overBudget, language);
    require(!oversized.ok() && oversized.diagnostics.back().code == Diagnostic::Code::BudgetExceeded,
        "postfix nodes remain within the fixed parser and instruction budgets");
}

void testVariablesV4UnitsInDefinitionsAndState()
{
    constexpr auto language = ExpressionLanguageVersion::VariablesV4;
    talas::expr::ExpressionDefinitionLibrary definitions;
    definitions.definitions.push_back({"note", {{"offset", ValueDomain::Scalar}},
        ValueDomain::NoteNumber, "60 nn + offset nn"});
    const auto defined = talas::expr::parse("note(4.5)", language, definitions);
    require(defined.ok(), "definition import retains postfix operand indices and units");
    const auto value = talas::expr::evaluate(*defined.expression, {}, {ValueDomain::NoteNumber});
    require(value.ok() && value.value.value == 64.5, "postfix formal conversion survives definition expansion");
    const auto stateful = talas::expr::parse("accumulate(1, 0, 0) nn / (1 - !Fault)", language);
    require(stateful.ok() && stateful.expression->stateSlotCount() == 1,
        "unit suffixes compose with inherited transactional state");
    double fault = 0;
    EvaluationContext context;
    context.user = &fault;
    context.resolveEvent = [](void* user, std::string_view, ExpressionValue& value) noexcept {
        value = {*static_cast<double*>(user), ValueDomain::Scalar}; return true;
    };
    talas::expr::ExpressionState state;
    require(talas::expr::prepareExpressionState(*stateful.expression, state), "prepare V4 state");
    ExpressionValue result;
    require(talas::expr::tryEvaluateStateful(*stateful.expression, context, state,
            {ValueDomain::NoteNumber}, result) && result.value == 1, "commit first typed state value");
    const auto committed = state;
    fault = 1;
    allocationCount = 0;
    countAllocations = true;
    const auto failed = talas::expr::tryEvaluateStateful(*stateful.expression, context, state,
        {ValueDomain::NoteNumber}, result);
    countAllocations = false;
    require(!failed && allocationCount == 0 && state == committed,
        "failure after a unit conversion rolls back inherited state without allocation");
    fault = 0;
    require(talas::expr::tryEvaluateStateful(*stateful.expression, context, state,
            {ValueDomain::NoteNumber}, result) && result.value == 2, "repaired state advances once");
    require(talas::expr::parse("1 ramp: (4 + 1) ms", language).ok()
            && talas::expr::parse("(7 rem: 4) ms", language).ok(),
        "timed selectors and checked remainder can use postfix results");
}

void testRampKeywordOrderMustBeExplicit()
{
    const auto language = ExpressionLanguageVersion::VariablesV4;
    for (const auto* accepted : {
             "1 ramp: 4 ms",
             "!Gate ramp: 4 ms",
             "-1 ramp: 4 ms",
             "(!Gain * 2) ramp: 4 ms",
             "!Gain * (1 ramp: 4 ms)",
             "(1 ramp: 4 ms) + 1",
             "1 ramp: (4 + 1) ms",
             "(1 ramp: 100 ms) < 0.5",
         })
    {
        require(talas::expr::parse(accepted, language).ok(),
            std::string{"unambiguous ramp: should parse: "} + accepted);
    }
    // Keyword messages evaluate last, so each of these would ramp a
    // different receiver or duration than it appears to.
    for (const auto* ambiguous : {
             "!Gain * 2 ramp: 4 ms",
             "1 + 1 ramp: 4 ms",
             "0.5 < 1 ramp: 4 ms",
             "1 ramp: 4 ms + 1",
             "1 ramp: 4 ms * 2",
             "1 ramp: 4 ms < 0.5",
             "1 ramp: 2 ramp: 4 ms",
             "!Gain * -1 ramp: 4 ms",
         })
    {
        const auto parsed = talas::expr::parse(ambiguous, language);
        require(!parsed.ok()
                && std::ranges::any_of(parsed.diagnostics,
                    [](const auto& diagnostic) {
                        return diagnostic.message.find("Parenthesize")
                            != std::string::npos;
                    }),
            std::string{"ambiguous ramp: should ask for parentheses: "}
                + ambiguous);
    }
}

void testEventReferenceRenamingUsesExactLexerTokens()
{
    const std::array renames{
        talas::expr::EventReferenceRename{"Gain", "LeadGain"},
        talas::expr::EventReferenceRename{"Gate", "Pulse"},
    };
    const auto source = "!Gain + !Gain2 + (!Gate != !Gain)";
    const auto rewritten = talas::expr::renameEventReferences(
        source, ExpressionLanguageVersion::PureV5, renames);
    const auto names = talas::expr::scanEventReferenceNames(
        source, ExpressionLanguageVersion::PureV5);
    require(rewritten ==
                "!LeadGain + !Gain2 + (!Pulse != !LeadGain)"
            && names == std::optional<std::vector<std::string>>{
                std::vector<std::string>{"Gain", "Gain2", "Gate"}}
            && talas::expr::parse(
                   *rewritten, ExpressionLanguageVersion::PureV5).ok(),
        "event renaming should touch exact lexer tokens and preserve !=");
    require(talas::expr::renameEventReferences(
                "{!Gain}", ExpressionLanguageVersion::PureV5, renames)
                == "{!LeadGain}"
            && talas::expr::renameEventReferences(
                "!Unknown", ExpressionLanguageVersion::PureV5, renames)
                == "!Unknown",
        "static terminal text and unrelated names should remain exact");

    const std::array invalid{
        talas::expr::EventReferenceRename{"Gain", "!Bad"}};
    const std::array repeated{
        talas::expr::EventReferenceRename{"Gain", "First"},
        talas::expr::EventReferenceRename{"Gain", "Second"},
    };
    const std::string oversizedName(
        talas::expr::ExpressionLimits::maximumSourceBytes, 'A');
    const std::array oversized{
        talas::expr::EventReferenceRename{"Gain", oversizedName}};
    require(!talas::expr::renameEventReferences(
                "!Gain", ExpressionLanguageVersion::PureV5, invalid)
            && !talas::expr::renameEventReferences(
                "!Gain", ExpressionLanguageVersion::PureV5, repeated)
            && !talas::expr::renameEventReferences(
                "!Gain", ExpressionLanguageVersion::PureV5, oversized),
        "invalid, conflicting or oversized rename maps should reject");
}

void testReplicationVariablesDoNotGlueToNeighbouringTokens()
{
    const auto glued = talas::expr::specializeReplicationVariables(
        "2?VoiceNumber + !Fader?VoiceNumber + ?VoiceNumber.5", 3, 4);
    require(glued
            == "2 3 + !Fader 3 + 3 .5",
        "a substituted voice number must stay a separate token");
    for (const auto* source : {
             "2?VoiceNumber", "!Fader?VoiceNumber", "?VoiceNumber.5"})
    {
        const auto specialized =
            talas::expr::specializeReplicationVariables(source, 3, 4);
        const auto audited =
            talas::expr::specializeReplicationVariablesForAudit(source, 3, 4);
        require(specialized && audited
                && audited->size() == std::string_view{source}.size()
                && !talas::expr::parse(
                    *specialized, ExpressionLanguageVersion::PureV5).ok()
                && !talas::expr::parse(
                    *audited, ExpressionLanguageVersion::PureV5).ok(),
            std::string{"glued replication text must not parse: "}
                + source);
    }
    const auto spaced = talas::expr::specializeReplicationVariables(
        "?VoiceNumber * 0.1 + 0.001", 3, 4);
    require(spaced == "3 * 0.1 + 0.001",
        "ordinary replication text should keep its spelling");
}

void testReplicationVariableSpecializationUsesExactTokens()
{
    const auto rewritten = talas::expr::specializeReplicationVariables(
        "?VoiceNumber + ?NumberVoices + ?VoiceNumber2 + !Gain != !Gain2",
        2, 4);
    require(rewritten
            == "2 + 4 + ?VoiceNumber2 + !Gain != !Gain2",
        "replication should specialize only exact one-based green variables");
    const auto executable = talas::expr::specializeReplicationVariables(
        "?VoiceNumber / ?NumberVoices + !Gain", 3, 4);
    require(executable == "3 / 4 + !Gain"
            && talas::expr::parse(
                   *executable,
                   ExpressionLanguageVersion::PureV5).ok(),
        "specialized Replicator expression should parse in the project language");
    const auto audited = talas::expr::specializeReplicationVariablesForAudit(
        "?VoiceNumber / ?NumberVoices + !Gain", 3, 4);
    require(audited && audited->size()
            == std::string_view{"?VoiceNumber / ?NumberVoices + !Gain"}.size()
            && talas::expr::parse(
                   *audited,
                   ExpressionLanguageVersion::PureV5).ok(),
        "audit specialization should preserve authored diagnostic offsets");
    require(!talas::expr::specializeReplicationVariables("?VoiceNumber", 0, 4)
            && !talas::expr::specializeReplicationVariables(
                "?VoiceNumber", 5, 4)
            && !talas::expr::specializeReplicationVariables(
                std::string(
                    talas::expr::ExpressionLimits::maximumSourceBytes + 1,
                    'x'),
                1, 4),
        "invalid voice indices and oversized sources should reject");
}

}

int main()
{
    testVoiceNumberBindingIsImmutableAndTyped();
    testUnboundVoiceNumberCannotBecomeAResolverValue();
    testVoiceNumberPreservesOtherBindingsAndDefinitions();
    testBoundVoiceNumberStateAndEvaluationDoNotAllocate();
    testRampMessageUsesMillisecondSoundClock();
    testRampKeywordOrderMustBeExplicit();
    testParenthesizedDecibelNegationNegatesTheLinearValue();
    testReplicationVariablesDoNotGlueToNeighbouringTokens();
    testVariablesV4PostfixUnits();
    testVariablesV4UnitsInDefinitionsAndState();
    testVoiceNumberRequiresAnExplicitLanguageProfile();
    testLiteralsParseAndEvaluate();
    testPrecedence();
    testUnaryOperators();
    testReferencesEvaluateThroughResolvers();
    testReferenceExtractionIsStableAndDeduped();
    testEventReferenceRenamingUsesExactLexerTokens();
    testReplicationVariableSpecializationUsesExactTokens();
    testUnitConversions();
    testTalasPitchNotation();
    testDefaultKeywordBindsToALiteral();
    testDecimalParsingIgnoresNumericLocale();
    testParseErrors();
    testEvaluationErrors();
    testLanguageVersionIsExplicitAndLegacySafe();
    testPureFunctionsAndDomains();
    testComparisonsAndLazyConditional();
    testIntoXArrayInterpolatesChangingEventArrays();
    testBoundedSelectionListsAndSafeIndexing();
    testPureV3BoundedArithmeticSeries();
    testPureV4UserDefinedExpressions();
    testSessionV1BoundedStatefulExpressions();
    testVariablesV1BoundedVariableReferences();
    testVariablesV2CheckedRemainderExpressions();
    testFixedBudgetsFailClosed();
    testFastEvaluatorDoesNotAllocate();
    testStructuralEquivalenceUsesExactCompiledAuthority();
    return 0;
}
