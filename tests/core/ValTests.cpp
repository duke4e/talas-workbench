#include "talas/core/BuiltinNodes.h"
#include "talas/expr/Expression.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Val, the language of parameter fields, sends messages: unary
// messages bind first, binary operators next and left to right, keyword
// messages last. The expected values are worked examples of each
// message.
namespace
{
using talas::expr::EvaluationContext;
using talas::expr::ExpressionLanguageVersion;
using talas::expr::ExpressionValue;
using talas::expr::ValueDomain;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct Example
{
    std::string_view source;
    ValueDomain domain;
    double value;
    double tolerance = 1.0e-9;
};

ExpressionValue evaluate(
    std::string_view source,
    ValueDomain domain,
    const EvaluationContext& context = {})
{
    const auto parsed = talas::expr::parse(source, ExpressionLanguageVersion::ValV1);
    require(parsed.ok(), std::string{source} + " should parse: "
        + (parsed.diagnostics.empty() ? std::string{} : parsed.diagnostics.front().message));
    const auto evaluated = talas::expr::evaluate(*parsed.expression, context, {domain});
    require(evaluated.ok(), std::string{source} + " should evaluate: "
        + (evaluated.diagnostics.empty() ? std::string{} : evaluated.diagnostics.front().message));
    return evaluated.value;
}

void requireExamples(std::initializer_list<Example> examples, std::string_view table)
{
    EvaluationContext context;
    context.sampleRate = 48000.0;
    context.tempoBpm = 120.0;
    for (const auto& example : examples)
    {
        const auto value = evaluate(example.source, example.domain, context);
        require(std::abs(value.value - example.value) <= example.tolerance,
            std::string{table} + ": " + std::string{example.source} + " should be "
                + std::to_string(example.value) + ", not " + std::to_string(value.value));
    }
}

// Units convert as they are cascaded.
void testUnitsAndConversions()
{
    requireExamples({
        {"3 days", ValueDomain::Seconds, 259200.0},
        {"2.1 h", ValueDomain::Seconds, 7560.0},
        {"3 m", ValueDomain::Seconds, 180.0},
        {"4.7 s", ValueDomain::Seconds, 4.7},
        {"100 ms", ValueDomain::Seconds, 0.1},
        {"100 usec", ValueDomain::Seconds, 0.0001},
        {"4 beats", ValueDomain::Seconds, 2.0},
        // Beat names the unit too.
        {"1 beat", ValueDomain::Seconds, 0.5},
        {"440 hz", ValueDomain::Hertz, 440.0},
        {"60 nn", ValueDomain::NoteNumber, 60.0},
        {"4 c sharp", ValueDomain::NoteNumber, 61.0},
        {"4 do", ValueDomain::NoteNumber, 60.0},
        {"4 re", ValueDomain::NoteNumber, 62.0},
        {"3 b", ValueDomain::NoteNumber, 59.0},
        // A frequency written in bpm leaves its beats per minute, in hertz
        // its hertz.
        {"120 bpm removeUnits", ValueDomain::Scalar, 120.0},
        {"4 s bpm removeUnits", ValueDomain::Scalar, 15.0},
        {"(4 s bpm) removeUnits", ValueDomain::Scalar, 15.0},
        {"120 bpm hz removeUnits", ValueDomain::Scalar, 2.0},
        {"440 hz s", ValueDomain::Seconds, 1.0 / 440.0},
        {"120 bpm s", ValueDomain::Seconds, 0.5},
        {"10 ms hz", ValueDomain::Hertz, 100.0},
        {"440 hz nn", ValueDomain::NoteNumber, 69.0},
        {"4 a flat hz", ValueDomain::Hertz, 415.3046975799451},
        {"60 nn hz", ValueDomain::Hertz, 261.6255653005986},
        {"4 c hz s", ValueDomain::Seconds, 1.0 / 261.6255653005986},
        {"4 c removeUnits", ValueDomain::Scalar, 60.0},
        // A spectrum's fractions of half the sample rate scale to hertz
        // with SignalProcessor halfSampleRate hz.
        {"SignalProcessor sampleRate", ValueDomain::Scalar, 48000.0},
        {"SignalProcessor halfSampleRate", ValueDomain::Scalar, 24000.0},
        {"0.5 * SignalProcessor halfSampleRate hz", ValueDomain::Hertz, 12000.0},
        // samp counts a time in samples.
        {"(1.0 / 120) s samp removeUnits", ValueDomain::Scalar, 400.0},
        {"10 ms samp", ValueDomain::Seconds, 0.01},
        {"440 hz samp removeUnits", ValueDomain::Scalar, 48000.0 / 440.0, 1.0e-9},
        {"(2 raisedTo: ((1.0 / 120) s samp removeUnits log: 2) ceiling) * 2 samp",
            ValueDomain::Seconds, 1024.0 / 48000.0},
    }, "units");
}

// Most parameter fields expect values from 0 to
// 1. Of the prototypes' fields taking a plain number (no frequency,
// duration or other unit), most start at a value in that range.
void testMostFieldsExpectZeroToOne()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    std::size_t fields = 0;
    std::size_t unitRange = 0;
    for (const auto& descriptor : registry.nodeTypes())
    {
        for (const auto& parameter : descriptor.parameters)
        {
            if (parameter.kind != talas::core::ParameterKind::Expression || parameter.domain != ValueDomain::Scalar)
                continue;
            const auto parsed = talas::expr::parse(parameter.defaultExpression, ExpressionLanguageVersion::ValV1);
            if (!parsed.ok() || !parsed.expression->eventReferences().empty())
                continue;
            const auto evaluated = talas::expr::evaluate(*parsed.expression, {}, {ValueDomain::Scalar});
            if (!evaluated.ok())
                continue;
            ++fields;
            if (evaluated.value.value >= 0.0 && evaluated.value.value <= 1.0)
                ++unitRange;
        }
    }
    require(fields > 100 && unitRange * 2 > fields,
        "most plain-number fields should expect values from 0 to 1 ("
            + std::to_string(unitRange) + " of " + std::to_string(fields) + ")");
}

// The arithmetic operators' worked examples.
void testArithmetic()
{
    requireExamples({
        {"3.0 + 2.0", ValueDomain::Scalar, 5.0},
        {"3.0 - 2.0", ValueDomain::Scalar, 1.0},
        {"3.0 * 2.0", ValueDomain::Scalar, 6.0},
        {"3.0 / 2.0", ValueDomain::Scalar, 1.5},
        {"3.0 ** 2.0", ValueDomain::Scalar, 9.0},
        {"5.0 // 3.0", ValueDomain::Scalar, 1.0},
        {"5.0 mod: 3.0", ValueDomain::Scalar, 2.0},
        {"5.0 \\\\ 3.0", ValueDomain::Scalar, 2.0},
        // // rounds toward negative infinity, quo: toward
        // zero; \\ and rem: are their remainders.
        {"-7 // 3", ValueDomain::Scalar, -3.0},
        {"7 // 3", ValueDomain::Scalar, 2.0},
        {"-7 \\\\ 3", ValueDomain::Scalar, 2.0},
        {"7 \\\\ 3", ValueDomain::Scalar, 1.0},
        {"-7 quo: 3", ValueDomain::Scalar, -2.0},
        {"7 quo: 3", ValueDomain::Scalar, 2.0},
        {"-7 rem: 3", ValueDomain::Scalar, -1.0},
        {"7 rem: 3", ValueDomain::Scalar, 1.0},
        {"-2.5 rounded", ValueDomain::Scalar, -3.0},
        {"-2.5 truncated", ValueDomain::Scalar, -2.0},
        {"3.0 negated", ValueDomain::Scalar, -3.0},
        {"5.0 inverse", ValueDomain::Scalar, 0.2},
        {"-3.0 abs", ValueDomain::Scalar, 3.0},
        {"3 abs", ValueDomain::Scalar, 3.0},
        {"-10 sign", ValueDomain::Scalar, -1.0},
        {"0 sign", ValueDomain::Scalar, 0.0},
        {"10 sign", ValueDomain::Scalar, 1.0},
        {"2.0 squared", ValueDomain::Scalar, 4.0},
        {"2.0 sqrt", ValueDomain::Scalar, std::sqrt(2.0)},
        {"1 exp", ValueDomain::Scalar, std::exp(1.0)},
        {"3 twoExp", ValueDomain::Scalar, 8.0},
        {"100 log", ValueDomain::Scalar, 2.0},
        {"8 twoLog", ValueDomain::Scalar, 3.0},
        {"-20 db", ValueDomain::Scalar, 0.1},
        {"-6 db", ValueDomain::Scalar, 0.5011872336272722},
        {"3.14159 cos", ValueDomain::Scalar, -1.0, 1.0e-5},
        {"1 normCos", ValueDomain::Scalar, -1.0},
        {"1.5707963267948966 sin", ValueDomain::Scalar, 1.0},
        {"1 normSin", ValueDomain::Scalar, 0.0, 1.0e-12},
        {"3.2 truncated", ValueDomain::Scalar, 3.0},
        {"-3.6 truncated", ValueDomain::Scalar, -3.0},
        {"3.2 rounded", ValueDomain::Scalar, 3.0},
        {"3.6 rounded", ValueDomain::Scalar, 4.0},
        {"3.2 roundTo: 2", ValueDomain::Scalar, 4.0},
        {"3.666 roundTo: 0.01", ValueDomain::Scalar, 3.67},
        {"-5 clipTo01", ValueDomain::Scalar, 0.0},
        {"0.5 clipTo01", ValueDomain::Scalar, 0.5},
        {"1.2 clipTo01", ValueDomain::Scalar, 1.0},
        {"-5 wrapTo02", ValueDomain::Scalar, 1.0},
        {"0.5 wrapTo02", ValueDomain::Scalar, 0.5},
        {"2.2 wrapTo02", ValueDomain::Scalar, 0.2},
        {"3 vmin: 2", ValueDomain::Scalar, 2.0},
        {"3 vmax: 2", ValueDomain::Scalar, 3.0},
    }, "arithmetic");
}

// Array indexing and function lookup.
void testArrays()
{
    requireExamples({
        {"3 of: #(0.1 0.2 0.3 0.4 0.5 0.6)", ValueDomain::Scalar, 0.4},
        {"1.5 into: #( {0 @ 0} {1 @ 50} {2 @ -10} )", ValueDomain::Scalar, 20.0},
    }, "arrays");
}

// 1 is true and 0 is false.
void testLogic()
{
    requireExamples({
        {"-1 asLogicValue", ValueDomain::Scalar, 0.0},
        {"2 asLogicValue", ValueDomain::Scalar, 1.0},
        {"2 eq: 3", ValueDomain::Scalar, 0.0},
        {"2 ne: 3", ValueDomain::Scalar, 1.0},
        {"2 gt: 3", ValueDomain::Scalar, 0.0},
        {"2 lt: 3", ValueDomain::Scalar, 1.0},
        {"2 ge: 3", ValueDomain::Scalar, 0.0},
        {"2 le: 3", ValueDomain::Scalar, 1.0},
        // Le: is less than or equal to.
        {"3 le: 3", ValueDomain::Scalar, 1.0},
        {"4 le: 3", ValueDomain::Scalar, 0.0},
        {"3 lt: 3", ValueDomain::Scalar, 0.0},
        {"0.6 threshold: 0.5", ValueDomain::Scalar, 1.0},
        {"0.1 threshold: 0.5", ValueDomain::Scalar, 0.0},
        {"-3 neg: 0.1 zero: 0.2 pos: 0.3", ValueDomain::Scalar, 0.1},
        {"0 neg: 0.1 zero: 0.2 pos: 0.3", ValueDomain::Scalar, 0.2},
        {"5 neg: 0.1 zero: 0.2 pos: 0.3", ValueDomain::Scalar, 0.3},
        {"0 true: 2 twoExp false: 3 twoExp", ValueDomain::Scalar, 8.0},
        {"1 true: 2 twoExp false: 3 twoExp", ValueDomain::Scalar, 4.0},
        // More neg:zero:pos: and true:false: examples.
        {"-1 neg: 4 zero: 3 pos: 2", ValueDomain::Scalar, 4.0},
        {"1 neg: 4 zero: 3 pos: 2 initially: 8", ValueDomain::Scalar, 2.0},
        {"0 true: 3 false: 2", ValueDomain::Scalar, 2.0},
        {"0.5 true: 5 false: 6", ValueDomain::Scalar, 5.0},
        {"-2 false: 0 true: 100", ValueDomain::Scalar, 0.0},
        {"10 false: 40 true: 60", ValueDomain::Scalar, 60.0},
    }, "logic");
    // 22050 hz is 1 in SOS pitch, each octave down 1/15 less.
    requireExamples({
        {"22050.0 hz inSOSPitch", ValueDomain::Scalar, 1.0},
        {"11025 hz inSOSPitch", ValueDomain::Scalar, 1.0 - 1.0 / 15.0, 1.0e-12},
        {"1378.12 hz inSOSPitch", ValueDomain::Scalar, 0.7333, 1.0e-4},
        {"4 a inSOSPitch", ValueDomain::Scalar, 1.0 + std::log2(440.0 / 22050.0) / 15.0, 1.0e-12},
        {"0 hz inSOSPitch", ValueDomain::Scalar, 0.0},
    }, "SOS pitch");
    // On is a duration of two years.
    requireExamples({{"on", ValueDomain::Seconds, 63072000.0}}, "on");
}

// Unary, then binary left to right, then keyword.
void testPrecedence()
{
    requireExamples({
        {"2.0 raisedTo: 13 + 3 negated", ValueDomain::Scalar, 1024.0},
        {"((2.0 raisedTo: 13) + 3) negated", ValueDomain::Scalar, -8195.0},
        {"1 - 0.5 * 2", ValueDomain::Scalar, 1.0},
        {"2 + 3 * 4", ValueDomain::Scalar, 20.0},
        {"2 + (3 * 4)", ValueDomain::Scalar, 14.0},
        {"4 c + 9 nn", ValueDomain::NoteNumber, 69.0},
        {"2 squared + 1", ValueDomain::Scalar, 5.0},
    }, "precedence");
}

// Messages go to Event Values as well as numbers.
void testEventValues()
{
    struct Values
    {
        static bool resolve(void*, std::string_view name, ExpressionValue& value) noexcept
        {
            if (name == "KeyNumber")
                value = {69.0, ValueDomain::Scalar};
            else if (name == "Index")
                value = {0.5, ValueDomain::Scalar};
            else
                return false;
            return true;
        }
    };
    EvaluationContext context;
    context.resolveEvent = Values::resolve;
    require(std::abs(evaluate("!KeyNumber nn hz", ValueDomain::Hertz, context).value - 440.0) < 1.0e-9,
        "!KeyNumber nn hz should be the key's frequency");
    require(evaluate("(!KeyNumber eq: 4 a)", ValueDomain::Scalar, context).value == 1.0,
        "a key number should compare with a pitch");
    require(evaluate("(!Index * 7) of: #(60 62 64 65 67 69 71)", ValueDomain::Scalar, context).value
            == 65.0,
        "a fractional index should select the element it falls in");
}

// A field reads an Event Source written
// after a backquote. `MIDIController07 is the controller's raw value from 0
// to 1, the Event Value !cc07 the engine keeps for it; `MIDIPitchBend the
// wheel's from -1 to 1; a source the global map names, `Glide, the value of
// its own name. A controller past 127 is refused, and a backquote stays an
// error where Val is not the language.
void testEventSourcesInFields()
{
    struct Values
    {
        static bool resolve(void*, std::string_view name, ExpressionValue& value) noexcept
        {
            if (name == "cc07")
                value = {0.25, ValueDomain::Scalar};
            else if (name == "cc100")
                value = {0.75, ValueDomain::Scalar};
            else if (name == "MIDIPitchBend")
                value = {-0.5, ValueDomain::Scalar};
            else if (name == "Glide")
                value = {0.125, ValueDomain::Scalar};
            else if (name == "Volume")
                value = {0.5, ValueDomain::Scalar};
            else
                return false;
            return true;
        }
    };
    EvaluationContext context;
    context.resolveEvent = Values::resolve;
    require(evaluate("`MIDIController07", ValueDomain::Scalar, context).value == 0.25,
        "`MIDIController07 should read controller 7 from 0 to 1");
    require(evaluate("`MIDIController7 * 2", ValueDomain::Scalar, context).value == 0.5,
        "`MIDIController7 should be the same controller as `MIDIController07");
    require(evaluate("`MIDIController100 + `MIDIController07", ValueDomain::Scalar, context).value == 1.0,
        "controllers run to `MIDIController127");
    require(evaluate("`MIDIPitchBend", ValueDomain::Scalar, context).value == -0.5,
        "`MIDIPitchBend should read the wheel from -1 to 1");
    require(evaluate("(`Glide * 10) + !Volume", ValueDomain::Scalar, context).value == 1.75,
        "another Event Source should read beside an Event Value");
    require(evaluate("\"`MIDIController07\" !Volume", ValueDomain::Scalar, context).value == 0.5,
        "a backquote in a comment should be no Event Source");
    const auto parsed = talas::expr::parse("(`MIDIController07 * 0.25) + 0.5", ExpressionLanguageVersion::ValV1);
    require(parsed.ok() && parsed.expression->eventReferences() == std::vector<std::string>{"cc07"},
        "`MIDIController07 should name the controller's own Event Value, cc07");
    require(talas::expr::eventSourceEventName("MIDIController07") == std::optional<std::string>{"cc07"}
            && talas::expr::eventSourceEventName("MIDIController127") == std::optional<std::string>{"cc127"}
            && talas::expr::eventSourceEventName("MIDIPitchBend") == std::optional<std::string>{"MIDIPitchBend"}
            && talas::expr::eventSourceEventName("Glide") == std::optional<std::string>{"Glide"}
            && !talas::expr::eventSourceEventName("MIDIController128").has_value(),
        "each Event Source should name the Event Value the engine keeps for it");
    for (const auto* source : {"`MIDIController128", "`MIDIControllerX", "` 3", "3 + `"})
    {
        const auto refused = talas::expr::parse(source, ExpressionLanguageVersion::ValV1);
        require(!refused.ok() && !refused.diagnostics.empty(), std::string{source} + " should be refused");
    }
    require(!talas::expr::parse("`MIDIController07", ExpressionLanguageVersion::VariablesV2).ok(),
        "a backquote should stay an error outside Val");
    // Expand writes an Event Value as its source's expression; an Event
    // Source of the same name and a renamed Event Value are left alone.
    const std::array<talas::expr::EventReferenceReplacement, 1> replacements{{{"Volume", "`MIDIController07"}}};
    require(talas::expr::replaceEventReferences("!Volume * 2 + `Volume", ExpressionLanguageVersion::ValV1,
                replacements)
            == std::optional<std::string>{"(`MIDIController07) * 2 + `Volume"},
        "Expand should write the red !Volume as its blue source, leaving `Volume as it is");
    const std::array<talas::expr::EventReferenceRename, 1> renames{{{"Volume", "Loudness"}}};
    require(talas::expr::renameEventReferences("!Volume + `Volume", ExpressionLanguageVersion::ValV1, renames)
            == std::optional<std::string>{"!Loudness + `Volume"},
        "renaming an Event Value should leave an Event Source of its name");
}

// A plain number means the field's own unit, and a
// frequency entered in a duration field becomes its period; 440 hz
// inverse is a period.
void testFieldUnits()
{
    requireExamples({
        {"440", ValueDomain::Hertz, 440.0},
        {"0.5", ValueDomain::Seconds, 0.5},
        {"100 hz", ValueDomain::Seconds, 0.01},
        {"0.01 s", ValueDomain::Hertz, 100.0},
        {"4 a", ValueDomain::Hertz, 440.0},
        {"440 hz inverse", ValueDomain::Seconds, 1.0 / 440.0},
        {"3 - -2", ValueDomain::Scalar, 5.0},
        {"7 rem: 3", ValueDomain::Scalar, 1.0},
        {"abs(-3) + 1", ValueDomain::Scalar, 4.0},
    }, "fields");
}

void testUnknownMessagesAreReported()
{
    for (const auto* source : {"3 frobnicate", "3 frob: 4", "#(1 2) sqrt"})
    {
        const auto parsed = talas::expr::parse(source, ExpressionLanguageVersion::ValV1);
        require(!parsed.ok() && !parsed.diagnostics.empty(),
            std::string{source} + " should be rejected");
    }
}

// Evaluates a Val time function at the given seconds, carrying its
// state from one evaluation to the next; !X and !Y read the values set for
// them.
class Timeline
{
public:
    explicit Timeline(std::string_view source, bool rampsStartAtFirstValue = false, bool rampsHoldUntriggered = false,
        std::uint32_t randomSalt = 0)
        : source_(source)
    {
        context_.rampsStartAtFirstValue = rampsStartAtFirstValue;
        context_.rampsHoldUntriggered = rampsHoldUntriggered;
        const auto parsed = talas::expr::parse(source, ExpressionLanguageVersion::ValV1);
        require(parsed.ok(), std::string{source} + " should parse: "
            + (parsed.diagnostics.empty() ? std::string{} : parsed.diagnostics.front().message));
        expression_.emplace(*parsed.expression);
        expression_->saltRandomNumbers(randomSalt);
        require(talas::expr::prepareExpressionState(*expression_, state_),
            std::string{source} + " should prepare its state");
        context_.sampleRate = 1000.0;
        context_.user = &values_;
        context_.resolveEvent = [](void* user, std::string_view name,
                                    ExpressionValue& value) noexcept {
            const auto& values = *static_cast<std::array<double, 2>*>(user);
            if (name != "X" && name != "Y")
                return false;
            value = {name == "X" ? values[0] : values[1], ValueDomain::Scalar};
            return true;
        };
    }

    double at(double seconds, double x = 0.0, double y = 0.0)
    {
        values_ = {x, y};
        context_.elapsedSampleFrame = static_cast<std::uint64_t>(std::llround(seconds * 1000.0));
        const auto result = talas::expr::evaluateStateful(*expression_, context_, state_);
        require(result.ok(), source_ + " should evaluate: "
            + (result.diagnostics.empty() ? std::string{} : result.diagnostics.front().message));
        return result.value.value;
    }

private:
    std::string source_;
    std::optional<talas::expr::Expression> expression_;
    talas::expr::ExpressionState state_;
    EvaluationContext context_;
    std::array<double, 2> values_{};
};

void requireNear(double actual, double expected, const std::string& message)
{
    require(std::abs(actual - expected) < 1.0e-9,
        message + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

// Ramps, triangles and beats that start at a trigger.
void testFunctionsOfTime()
{
    Timeline repeating("1 repeatingRamp: 1 s");
    for (const auto& [time, value] : {std::pair{0.0, 0.0}, std::pair{0.25, 0.25},
             std::pair{0.5, 0.5}, std::pair{0.75, 0.75}, std::pair{1.0, 0.0}, std::pair{1.25, 0.25}})
    {
        requireNear(repeating.at(time), value, "1 repeatingRamp: 1 s at " + std::to_string(time));
    }
    Timeline full("1 fullRamp: 2 s");
    for (const auto& [time, value] : {std::pair{0.0, -1.0}, std::pair{1.0, 0.0},
             std::pair{2.0, 1.0}, std::pair{3.0, 1.0}})
    {
        requireNear(full.at(time), value, "1 fullRamp: 2 s at " + std::to_string(time));
    }
    Timeline triangle("1 triangle");
    for (const auto& [time, value] : {std::pair{0.0, 0.0}, std::pair{0.25, 0.5},
             std::pair{0.5, 1.0}, std::pair{0.75, 0.5}, std::pair{1.0, 0.0}})
    {
        requireNear(triangle.at(time), value, "1 triangle at " + std::to_string(time));
    }
    // Ramp rests at 1; when its receiver becomes positive it
    // grows from 0 to 1 over a second (ramp: over its duration) and sticks
    // at 1, and it cannot start again until the receiver is zero or less.
    // fullRamp goes from -1 to 1 and rests at 1 as well.
    Timeline triggered("!X ramp: 1 s");
    requireNear(triggered.at(0.0, 0.0), 1.0, "a ramp should rest at 1 until its trigger");
    requireNear(triggered.at(0.25, 0.0), 1.0, "a ramp should rest at 1 until its trigger");
    requireNear(triggered.at(0.5, 1.0), 0.0, "a ramp should start at the trigger");
    requireNear(triggered.at(1.0, 1.0), 0.5, "a ramp should rise while triggered");
    // Once triggered it runs on to its end when the trigger
    // falls; projects before format 103 hold it.
    requireNear(triggered.at(1.25, 0.0), 0.75, "a ramp should run on when the trigger ends");
    requireNear(triggered.at(2.0, 0.0), 1.0, "a ramp should reach its end with the trigger off");
    Timeline held("!X ramp: 1 s", false, true);
    (void) held.at(0.0, 0.0);
    (void) held.at(0.5, 1.0);
    (void) held.at(1.0, 1.0);
    requireNear(held.at(1.25, 0.0), 0.5, "a format 102 ramp should hold when the trigger ends");
    Timeline fullOn("!X fullRamp: 1 s");
    (void) fullOn.at(0.0, 1.0);
    requireNear(fullOn.at(0.5, 0.0), 0.0, "a full ramp should run on when the trigger ends");
    Timeline oneSecond("!X ramp");
    requireNear(oneSecond.at(0.0, 0.0), 1.0, "ramp should rest at 1");
    requireNear(oneSecond.at(1.0, 1.0), 0.0, "ramp should start from 0 at its trigger");
    requireNear(oneSecond.at(1.5, 1.0), 0.5, "ramp should take one second");
    requireNear(oneSecond.at(2.5, 1.0), 1.0, "ramp should stick at 1");
    requireNear(oneSecond.at(3.0, 1.0), 1.0, "ramp should not start again while positive");
    Timeline constant("1 ramp: 3 s");
    requireNear(constant.at(0.0), 0.0, "1 ramp: 3 s should start at once");
    requireNear(constant.at(1.5), 0.5, "1 ramp: 3 s should take three seconds");
    requireNear(constant.at(4.0), 1.0, "1 ramp: 3 s should stick at 1");
    Timeline fullRest("!X fullRamp: 2 s");
    requireNear(fullRest.at(0.0, 0.0), 1.0, "fullRamp: should rest at 1");
    requireNear(fullRest.at(1.0, 1.0), -1.0, "fullRamp: should start from -1");
    requireNear(fullRest.at(2.0, 1.0), 0.0, "fullRamp: should rise to 1 over its duration");
    // The ramps: ramp goes from 0 to 1 over a second once
    // triggered, fullRamp from -1 to 1; their repeating forms start again
    // while the trigger stays on, repeatingFullRamp: over its own duration.
    Timeline tableRepeating("!X repeatingRamp");
    requireNear(tableRepeating.at(0.0, 1.0), 0.0, "repeatingRamp should start at 0");
    requireNear(tableRepeating.at(0.5, 1.0), 0.5, "repeatingRamp should rise over a second");
    requireNear(tableRepeating.at(1.25, 1.0), 0.25, "repeatingRamp should start again while triggered");
    Timeline fullRepeating("!X repeatingFullRamp");
    requireNear(fullRepeating.at(0.0, 1.0), -1.0, "repeatingFullRamp should start at -1");
    requireNear(fullRepeating.at(1.5, 1.0), 0.0, "repeatingFullRamp should start again each second");
    Timeline fullRepeatingTimed("!X repeatingFullRamp: 2.3 s");
    requireNear(fullRepeatingTimed.at(0.0, 1.0), -1.0, "repeatingFullRamp: 2.3 s should start at -1");
    requireNear(fullRepeatingTimed.at(1.15, 1.0), 0.0, "repeatingFullRamp: 2.3 s should be half way at 1.15 s");
    requireNear(fullRepeatingTimed.at(2.3 + 0.575, 1.0), -0.5, "repeatingFullRamp: 2.3 s should repeat every 2.3 s");
    Timeline fullOnce("!X fullRamp");
    requireNear(fullOnce.at(0.0, 1.0), -1.0, "fullRamp should start at -1");
    requireNear(fullOnce.at(1.5, 1.0), 1.0, "fullRamp should stick at 1 after its second");
    // !KeyDown fullRamp: 3 s goes from -1 up to
    // 1 whenever a key goes down.
    Timeline keyed("!X fullRamp: 3 s");
    requireNear(keyed.at(0.5, 1.0), -1.0, "fullRamp: 3 s should start from -1 at the key down");
    requireNear(keyed.at(2.0, 1.0), 0.0, "fullRamp: 3 s should be half way after 1.5 s");
    requireNear(keyed.at(3.5, 1.0), 1.0, "fullRamp: 3 s should reach 1 after three seconds");
    // Projects before format 100 start a ramp at its first value.
    Timeline legacy("!X ramp: 1 s", true);
    requireNear(legacy.at(0.0, 0.0), 0.0, "a legacy ramp should start at 0 before its trigger");
    Timeline legacyFull("!X fullRamp: 2 s", true);
    requireNear(legacyFull.at(0.0, 0.0), -1.0, "a legacy fullRamp: should start at -1 before its trigger");
    // !LocalTime is the time in seconds since the Sound
    // started, so (!LocalTime mod: 6) nn grows to 6 half steps and starts
    // again.
    Timeline local("!LocalTime");
    requireNear(local.at(0.0), 0.0, "!LocalTime should start at 0");
    requireNear(local.at(2.5), 2.5, "!LocalTime should count seconds");
    Timeline interval("!LocalTime mod: 6");
    requireNear(interval.at(7.0), 1.0, "!LocalTime mod: 6 should start again after 6 s");
    Timeline beats("1 bpm: 60");
    for (const auto& [time, value] : {std::pair{0.0, -1.0}, std::pair{0.25, 1.0},
             std::pair{0.5, 0.0}, std::pair{0.75, 0.0}, std::pair{1.0, -1.0}, std::pair{1.005, 1.0}})
    {
        requireNear(beats.at(time), value, "1 bpm: 60 at " + std::to_string(time));
    }
    Timeline duty("1 bpm: 120 dutyCycle: 0.1");
    requireNear(duty.at(0.0), -1.0, "a beat should start at the trigger, stealing its voice");
    requireNear(duty.at(0.04), 1.0, "a beat should stay on for its duty cycle");
    requireNear(duty.at(0.06), 0.0, "a beat should turn off after its duty cycle");
    requireNear(duty.at(0.505), 1.0, "the next beat should follow after 60 / bpm seconds");
}

// Random numbers at intervals or on triggers, and
// smoothing to a new value.
void testRandomAndSmoothing()
{
    Timeline interval("1 s random");
    Timeline again("1 s random");
    const auto first = interval.at(0.0);
    require(first >= -1.0 && first < 1.0 && interval.at(0.5) == first,
        "1 s random should hold one number in [-1, 1) for a second");
    const auto second = interval.at(1.0);
    require(second != first && again.at(0.0) == first && again.at(1.0) == second,
        "1 s random should draw a repeatable new number each second");

    Timeline triggered("!X nextRandom");
    const auto before = triggered.at(0.0, 0.0);
    const auto drawn = triggered.at(0.1, 1.0);
    require(triggered.at(0.2, 1.0) == drawn && triggered.at(0.3, 0.0) == drawn
            && drawn != before,
        "nextRandom should draw only when its trigger rises");
    require(triggered.at(0.4, 1.0) != drawn,
        "nextRandom should draw again on the next trigger");

    Timeline smooth("!X smooth: 1 s");
    requireNear(smooth.at(0.0, 0.0), 0.0, "smooth: should start at its first value");
    requireNear(smooth.at(0.1, 1.0), 0.0, "smooth: should leave the old value when the input changes");
    requireNear(smooth.at(0.6, 1.0), 0.5, "smooth: should move in a straight line");
    requireNear(smooth.at(1.1, 1.0), 1.0, "smooth: should reach the new value after its duration");
    Timeline smoothed("!X smoothed");
    requireNear(smoothed.at(0.0, 0.0), 0.0, "smoothed should start at its first value");
    smoothed.at(0.0, 2.0);
    requireNear(smoothed.at(0.05, 2.0), 1.0, "smoothed should take 100 ms");
    // A gate falling from 1 to 0 smoothed falls over 100 ms.
    Timeline gate("!X smoothed");
    requireNear(gate.at(0.0, 1.0), 1.0, "smoothed should start at the gate's 1");
    requireNear(gate.at(0.01, 0.0), 1.0, "smoothed should leave 1 when the gate falls");
    requireNear(gate.at(0.06, 0.0), 0.5, "smoothed should be halfway down after 50 ms");
    requireNear(gate.at(0.11, 0.0), 0.0, "smoothed should reach 0 after 100 ms");
    // A random index scaled by !Jitter * 7 picks the first
    // interval at a Jitter of 0 and reaches the larger ones as it rises.
    const auto intervals = [](double jitter) {
        Timeline picked("((1 bpm: 600) nextRandom abs * (!X * 7)) of: #(0 2 4 5 7 9 11)");
        std::set<double> heard;
        for (int step = 0; step < 400; ++step)
            heard.insert(picked.at(0.01 * step, jitter));
        return heard;
    };
    require(intervals(0.0) == std::set<double>{0.0} && intervals(1.0).contains(11.0) && intervals(1.0).contains(7.0)
            && !intervals(0.3).contains(9.0),
        "a larger !Jitter should let the random index reach the larger intervals");
}

// SwarmFollowFromPosition:velocity:acceleration:
// friction: follows the receiver as a bee follows the queen, starting where
// and as fast as it is told; the more its acceleration, the closer it
// follows the queen's changes of direction, and friction slows it until it
// settles on a queen that stays put.
void testSwarmFollower()
{
    const auto bee = [](double acceleration, double friction) {
        return Timeline("!X swarmFollowFromPosition: 0 velocity: 0.8 acceleration: " + std::to_string(acceleration)
            + " friction: " + std::to_string(friction));
    };
    auto started = bee(0.5, 0.1);
    requireNear(started.at(0.0, 1.0), 0.0, "the bee should start at its position");
    const auto flown = started.at(0.1, 1.0);
    require(flown > 0.075 && flown < 0.085, "the bee should leave at its velocity, gaining speed toward the queen");
    auto settling = bee(2.0, 5.0);
    for (int tenth = 1; tenth <= 300; ++tenth)
        settling.at(tenth * 0.1, 1.0);
    require(std::abs(settling.at(30.1, 1.0) - 1.0) < 0.02, "friction should settle the bee on a queen that stays put");
    // A queen going back and forth every ten seconds.
    const auto distance = [&](double acceleration) {
        auto follower = bee(acceleration, 10.0);
        double total = 0.0;
        for (int step = 0; step <= 20000; ++step)
        {
            const auto queen = 0.5 * std::sin(2.0 * std::numbers::pi * step / 10000.0);
            total += std::abs(follower.at(step / 1000.0, queen) - queen);
        }
        return total / 20001.0;
    };
    require(distance(50.0) < 0.5 * distance(0.5),
        "a quicker bee should follow the queen's changes of direction more closely");
    // The bee may take Event Values, !Pan swarmFollowFromPosition: 0
    // velocity: !Vel acceleration: !Acc friction: !Damping.
    Timeline live("!X swarmFollowFromPosition: 0 velocity: !Y acceleration: 0.5 friction: 0.1");
    requireNear(live.at(0.0, 1.0, 0.8), 0.0, "an Event Value's bee should start at its position");
    requireNear(live.at(0.1, 1.0, 0.8), flown, "an Event Value should give the bee its velocity");
    Timeline revealed("(1 triangle * 0.5) swarmFollowFromPosition: 0 velocity: 0.8 acceleration: 0.5 friction: 0.1");
    for (int tenth = 0; tenth <= 100; ++tenth)
    {
        const auto place = revealed.at(tenth * 0.1);
        require(std::isfinite(place) && std::abs(place) < 2.0, "a bee should follow its triangle");
    }
}

// Messages that count, select, toggle,
// gate, time and latch triggers. A trigger is on while positive.
void testTriggerMessages()
{
    // The values of a trigger message as !X rises at 0.1, 0.3, 0.5 and 0.7.
    const auto pulses = [](std::string_view source, std::vector<double> ys = {}) {
        Timeline timeline(source);
        std::vector<double> values;
        for (std::size_t step = 0; step < 8; ++step)
        {
            const auto y = step < ys.size() ? ys[step] : 0.0;
            values.push_back(timeline.at(0.1 * static_cast<double>(step + 1), step % 2 == 0 ? 1.0 : 0.0, y));
        }
        return values;
    };
    require(pulses("!X countTriggers") == std::vector<double>{0, 0, 1, 1, 2, 2, 3, 3},
        "countTriggers should count each trigger from 0");
    Timeline before("!X countTriggers");
    require(before.at(0.0, 0.0) == -1.0, "countTriggers should be -1 before its first trigger");
    require(pulses("!X countTriggersMod: 3") == std::vector<double>{0, 0, 1, 1, 2, 2, 0, 0},
        "countTriggersMod: should wrap to 0");
    require(pulses("!X countTriggersMod: 3 reverse: !Y", {1, 1, 1, 1, 0, 0, 0, 0})
            == std::vector<double>{2, 2, 1, 1, 2, 2, 0, 0},
        "countTriggersMod:reverse: should count down while reverse is on");
    require(pulses("!X triggerEvery: 2") == std::vector<double>{1, 0, 0, 0, 1, 0, 0, 0},
        "triggerEvery: should pass every second trigger");
    require(pulses("!X asToggle") == std::vector<double>{1, 1, 0, 0, 1, 1, 0, 0},
        "asToggle should turn on each trigger");
    require(pulses("!X toggleWhen: !Y", {1, 1, 0, 0, 1, 1, 1, 1}) == std::vector<double>{1, 1, 1, 1, 0, 0, 1, 1},
        "toggleWhen: should turn only while its argument is on");
    require(pulses("!X gateWhen: !Y", {1, 0, 0, 0, 1, 1, 0, 0}) == std::vector<double>{1, 0, 0, 0, 1, 0, 0, 0},
        "gateWhen: should pass the gates begun while its argument was on");

    Timeline between("!X durationBetweenTriggers: 2");
    between.at(0.0, 1.0);
    between.at(0.2, 0.0);
    between.at(0.5, 1.0);
    requireNear(between.at(0.6, 0.0), 0.0, "durationBetweenTriggers: should wait for its triggers");
    requireNear(between.at(1.5, 1.0), 1.5, "durationBetweenTriggers: 2 should span the last two intervals");
    Timeline last("!X durationBetweenTriggers");
    last.at(0.0, 1.0);
    last.at(0.1, 0.0);
    requireNear(last.at(0.5, 1.0), 0.5, "durationBetweenTriggers should measure the last interval");
    Timeline gate("!X durationOfGate");
    gate.at(0.1, 1.0);
    requireNear(gate.at(0.3, 1.0), 0.0, "durationOfGate should wait for the gate to close");
    requireNear(gate.at(0.4, 0.0), 0.3, "durationOfGate should measure how long the gate stayed on");

    Timeline changed("!X hasChangedReset: !Y");
    require(changed.at(0.0, 0.0) == 0.0 && changed.at(0.1, 1.0) == 1.0 && changed.at(0.2, 0.0) == 1.0
            && changed.at(0.3, 0.0, 1.0) == 0.0 && changed.at(0.4, 0.0, 1.0) == 0.0
            && changed.at(0.5, 2.0, 0.0) == 1.0,
        "hasChangedReset: should latch a change until reset");
    // neg:zero:pos:initially: starts at its initial value and keeps it
    // until the receiver first changes, as when a live loop's LoopStart or
    // LoopEnd fader first moves.
    Timeline initially("(!X - !Y) neg: 4 zero: 3 pos: 2 initially: 8");
    require(initially.at(0.0, 0.5, 0.2) == 8.0 && initially.at(0.1, 0.5, 0.2) == 8.0
            && initially.at(0.2, 0.6, 0.2) == 2.0 && initially.at(0.3, 0.1, 0.2) == 4.0
            && initially.at(0.4, 0.2, 0.2) == 3.0 && initially.at(0.5, 0.5, 0.2) == 2.0,
        "initially: should hold until the receiver has a value of its own");
    // Format 118 writes a project's neg:zero:pos: as starting at 0.
    require(talas::expr::withSignSelectionsStartingAtZero("((!X - !Y) neg: 4 zero: 3 pos: 2) + 1")
                == std::optional<std::string>{"((!X - !Y) neg: 4 zero: 3 pos: 2 initially: 0) + 1"}
            && !talas::expr::withSignSelectionsStartingAtZero("!X neg: 4 zero: 3 pos: 2 initially: 8").has_value(),
        "a neg:zero:pos: should be written to start at 0, one with initially: left");
    Timeline sostenuto("!X sustainBy: !Y");
    require(sostenuto.at(0.0, 1.0, 0.0) == 1.0 && sostenuto.at(0.1, 1.0, 1.0) == 1.0
            && sostenuto.at(0.2, 0.0, 1.0) == 1.0 && sostenuto.at(0.3, 0.0, 0.0) == 0.0,
        "sustainBy: should hold a trigger that was on when the pedal went down");
    Timeline unheld("!X sustainBy: !Y");
    require(unheld.at(0.0, 0.0, 1.0) == 0.0 && unheld.at(0.1, 1.0, 1.0) == 1.0 && unheld.at(0.2, 0.0, 1.0) == 0.0,
        "sustainBy: should not hold a trigger begun after the pedal went down");
}

// Seeded, normal and exponential random numbers and
// chaotic sequences.
void testRandomDistributions()
{
    const auto draws = [](std::string_view source) {
        Timeline timeline(source);
        std::vector<double> values;
        for (std::size_t step = 0; step < 2000; ++step)
        {
            values.push_back(timeline.at(0.001 * static_cast<double>(2 * step), 1.0));
            timeline.at(0.001 * static_cast<double>(2 * step + 1), 0.0);
        }
        return values;
    };
    const auto seeded = draws("!X nextRandomWithSeed: 3413");
    require(seeded == draws("!X nextRandomWithSeed: 3413") && seeded != draws("!X nextRandomWithSeed: 7")
            && std::ranges::all_of(seeded, [](double value) { return value >= -1.0 && value < 1.0; }),
        "a seeded random sequence should repeat for its seed");
    const auto mean = [](const std::vector<double>& values) {
        return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    };
    const auto normal = draws("!X nextNormal");
    require(std::ranges::all_of(normal, [](double value) { return value >= -1.0 && value <= 1.0; })
            && std::abs(mean(normal)) < 0.05
            && std::ranges::count_if(normal, [](double value) { return std::abs(value) < 1.0 / 3.0; }) > 1200,
        "nextNormal should cluster around 0 within (-1, 1)");
    const auto exponential = draws("!X nextRandExp");
    require(std::ranges::all_of(exponential, [](double value) { return value >= 0.0 && value <= 24.0; })
            && std::ranges::count_if(exponential, [](double value) { return value < 3.0; }) > 1100,
        "nextRandExp should give many small intervals and few large ones in (0, 24)");
    Timeline interval("1 s randExp");
    require(interval.at(0.0) == interval.at(0.5), "randExp should hold a number for its interval");

    Timeline chaotic("!X nextChaotic: 0.998");
    requireNear(chaotic.at(0.0, 1.0), 3.998 * 0.5 * 0.5, "nextChaotic: should step x = r x (1 - x), r = 3 + argument");
    chaotic.at(0.1, 0.0);
    requireNear(chaotic.at(0.2, 1.0), 3.998 * 0.9995 * 0.0005, "nextChaotic: should step on each trigger");
    Timeline seed("!X nextChaotic: 0.5 seed: 0.25");
    requireNear(seed.at(0.0, 1.0), 3.5 * 0.25 * 0.75, "nextChaotic:seed: should start from its seed");
    Timeline reset("!X nextChaotic: 0.5 reset: !Y");
    const auto stepped = reset.at(0.0, 1.0);
    const auto injected = reset.at(0.1, 1.0, 1.0);
    require(stepped == 3.5 * 0.5 * 0.5 && injected != stepped && injected >= 0.0 && injected < 1.0,
        "nextChaotic:reset: should put in a random x when reset turns on");
    // (1 bpm: !BPM) nextChaotic: !Kaos seed: 0.2 reset: !Inject.
    Timeline both("!X nextChaotic: 0.5 seed: 0.25 reset: !Y");
    Timeline unreset("!X nextChaotic: 0.5 seed: 0.25");
    const auto first = both.at(0.0, 1.0);
    both.at(0.1, 0.0);
    const auto second = both.at(0.2, 1.0);
    unreset.at(0.0, 1.0);
    unreset.at(0.1, 0.0);
    require(first == 3.5 * 0.25 * 0.75 && second == 3.5 * first * (1.0 - first) && second == unreset.at(0.2, 1.0),
        "nextChaotic:seed:reset: should step from its seed");
    const auto reseeded = both.at(0.4, 1.0, 1.0);
    require(reseeded != 3.5 * second * (1.0 - second) && reseeded >= 0.0 && reseeded < 1.0,
        "nextChaotic:seed:reset: should put in a random x when reset turns on");
}

// SampleAndHold: samples its argument on each
// trigger; trackAndHold: follows it while the trigger is on. Float pi is
// Smalltalk's pi.
void testTrackers()
{
    Timeline sample("!X sampleAndHold: !Y");
    require(sample.at(0.0, 0.0, 5.0) == 0.0 && sample.at(0.1, 1.0, 6.0) == 6.0 && sample.at(0.2, 1.0, 7.0) == 6.0
            && sample.at(0.3, 0.0, 8.0) == 6.0 && sample.at(0.4, 1.0, 9.0) == 9.0,
        "sampleAndHold: should sample its argument on each trigger");
    Timeline track("!X trackAndHold: !Y");
    require(track.at(0.0, 1.0, 5.0) == 5.0 && track.at(0.1, 1.0, 6.0) == 6.0 && track.at(0.2, 0.0, 7.0) == 6.0
            && track.at(0.3, 1.0, 8.0) == 8.0,
        "trackAndHold: should follow its argument while the trigger is on");
    requireNear(evaluate("(Float pi / 2) sin", ValueDomain::Scalar).value, 1.0, "Float pi should be pi");
}

// Parameter fields are Smalltalk, so they take
// Smalltalk's messages to numbers as well as Val's; tests answer 1 for
// true and 0 for false.
void testSmalltalkNumberMessages()
{
    const auto pi = std::numbers::pi;
    requireExamples({
        {"5 reciprocal", ValueDomain::Scalar, 0.2},
        {"2.5 ceiling", ValueDomain::Scalar, 3.0},
        {"-2.5 ceiling", ValueDomain::Scalar, -2.0},
        {"2.5 floor", ValueDomain::Scalar, 2.0},
        {"-2.5 floor", ValueDomain::Scalar, -3.0},
        {"3.7 asInteger", ValueDomain::Scalar, 3.0},
        {"-3.7 asInteger", ValueDomain::Scalar, -3.0},
        {"3 asFloat", ValueDomain::Scalar, 3.0},
        {"10 sqrtRounded", ValueDomain::Scalar, 3.0},
        {"1 exp ln", ValueDomain::Scalar, 1.0},
        {"8 log: 2", ValueDomain::Scalar, 3.0},
        {"9 floorLog: 2", ValueDomain::Scalar, 3.0},
        {"5 floorToTwoPower", ValueDomain::Scalar, 4.0},
        {"5 ceilingToTwoPower", ValueDomain::Scalar, 8.0},
        {"8 ceilingToTwoPower", ValueDomain::Scalar, 8.0},
        {"180 degreesToRadians", ValueDomain::Scalar, pi},
        {"Float pi radiansToDegrees", ValueDomain::Scalar, 180.0},
        {"1 arcTan", ValueDomain::Scalar, pi / 4.0},
        {"0.5 arcSin", ValueDomain::Scalar, pi / 6.0},
        {"0.5 arcCos", ValueDomain::Scalar, pi / 3.0},
        {"(Float pi / 4) tan", ValueDomain::Scalar, 1.0},
        {"0 sinc", ValueDomain::Scalar, 1.0},
        {"(Float pi / 2) sinc", ValueDomain::Scalar, 2.0 / pi},
        {"3 max: 5", ValueDomain::Scalar, 5.0},
        {"3 min: 5", ValueDomain::Scalar, 3.0},
        {"3 maxAbs: -5", ValueDomain::Scalar, -5.0},
        {"3 minAbs: -5", ValueDomain::Scalar, 3.0},
        {"3.7 truncateTo: 2", ValueDomain::Scalar, 2.0},
        {"-3.7 truncateTo: 2", ValueDomain::Scalar, -2.0},
        {"3.2 ceilingTo: 2", ValueDomain::Scalar, 4.0},
        {"3 between: 1 and: 5", ValueDomain::Scalar, 1.0},
        {"6 between: 1 and: 5", ValueDomain::Scalar, 0.0},
        {"4 even", ValueDomain::Scalar, 1.0},
        {"3 even", ValueDomain::Scalar, 0.0},
        {"-3 odd", ValueDomain::Scalar, 1.0},
        {"-3 negative", ValueDomain::Scalar, 1.0},
        {"0 positive", ValueDomain::Scalar, 1.0},
        {"0 strictlyPositive", ValueDomain::Scalar, 0.0},
        {"3 = 3", ValueDomain::Scalar, 1.0},
        {"3 ~= 3", ValueDomain::Scalar, 0.0},
        {"Float phi", ValueDomain::Scalar, (1.0 + std::sqrt(5.0)) / 2.0},
        {"Double pi", ValueDomain::Scalar, pi},
    }, "Smalltalk numbers");
}

// Existing profiles keep their arithmetic precedence and reject messages.
void testOtherProfilesUnchanged()
{
    const auto parsed = talas::expr::parse("1 - 0.5 * 2", ExpressionLanguageVersion::VariablesV4);
    require(parsed.ok() && talas::expr::evaluate(*parsed.expression, {}).value.value == 0.0,
        "VariablesV4 should keep arithmetic precedence");
    require(!talas::expr::parse("3 twoExp", ExpressionLanguageVersion::VariablesV4).ok(),
        "VariablesV4 should not accept Val messages");
}
}

// A Sound pasted into a field is named with L, R or M,
// and L: n updates it every n milliseconds; the field does arithmetic and
// units with it.
void testPastedSounds()
{
    const auto parsed = talas::expr::parse("Oscillator1 L nn + 2 c", ExpressionLanguageVersion::ValV1);
    require(parsed.ok() && parsed.expression->identifiers() == std::vector<std::string>{"Oscillator1 L"},
        "a pasted Sound should be one identifier with its channel");
    EvaluationContext context;
    context.resolveIdentifier = [](void*, std::string_view name, ExpressionValue& value) noexcept {
        if (name != "Oscillator1 L" && name != "Osc R: 5" && name != "Env M")
            return false;
        value = {0.5, ValueDomain::Scalar};
        return true;
    };
    const auto pitch = talas::expr::evaluate(*parsed.expression, context, {ValueDomain::NoteNumber});
    require(pitch.ok() && std::abs(pitch.value.value - 36.5) < 1.0e-12,
        "Oscillator1 L nn + 2 c should deviate a half step around 2 c");
    require(std::abs(evaluate("(Osc R: 5) * 12 nn + 4 c", ValueDomain::NoteNumber, context).value - 66.0) < 1.0e-12
            && std::abs(evaluate("Env M * 2", ValueDomain::Scalar, context).value - 1.0) < 1.0e-12,
        "R: n and M should name pasted Sounds too");
    const auto periodic = talas::expr::parse("(Osc R: 5) * 12 nn + 4 c", ExpressionLanguageVersion::ValV1);
    require(periodic.ok() && periodic.expression->identifiers() == std::vector<std::string>{"Osc R: 5"},
        "L: n should keep its period in the name");
    require(talas::expr::parseSoundReference("Osc R: 5") == talas::expr::SoundReference{"Osc", 'R', 5.0}
            && talas::expr::parseSoundReference("Oscillator1 L") == talas::expr::SoundReference{"Oscillator1", 'L', std::nullopt}
            && !talas::expr::parseSoundReference("Oscillator1").has_value()
            && talas::expr::formatSoundReference({"Osc", 'M', 2.5}) == "Osc M: 2.5",
        "Sound reference names should round-trip");
    require(!talas::expr::parse("Osc L: 0", ExpressionLanguageVersion::ValV1).ok(),
        "a pasted Sound's period should be positive");

    // ADR-0020: the parts beside a Sound that read Event Values or variables
    // or call a function split out of the field, largest first.
    const auto split = [](std::string_view source) {
        const auto field = talas::expr::parse(source, ExpressionLanguageVersion::ValV1);
        require(field.ok(), "the split field should parse: " + std::string(source));
        return field.expression->splitAroundSoundReferences(source);
    };
    const auto live = split("Osc L * !Fader1 * 12 nn + !Pitch nn");
    require(live.has_value() && live->skeleton == "Osc L * __kc_live_1 * 12 nn + __kc_live_2"
            && live->liveParts == std::vector<std::string>{"!Fader1", "!Pitch nn"},
        "each largest live part beside a pasted Sound should split out");
    const auto plain = split("(Osc L: 5) * 12 nn + 4 c");
    require(plain.has_value() && plain->skeleton == "(Osc L: 5) * 12 nn + 4 c" && plain->liveParts.empty(),
        "a field with only literals beside its Sound should stay whole");
    const auto function = split("(Osc L) abs * !Gain");
    require(function.has_value() && function->skeleton == "(Osc L) abs * __kc_live_1"
            && function->liveParts == std::vector<std::string>{"!Gain"},
        "a function of a Sound should stay in the skeleton");
}

// Hh:mm:ss.ff SMPTE is a time at the
// project's timecode frame rate, and triggerAtTimeCode and
// gateOnAtTimeCodeForDuration: compare the MIDI time code with their times.
void testTimeCode()
{
    const auto seconds = [](std::string_view source, std::uint8_t framesPerSecond) {
        talas::expr::ExpressionDefinitionLibrary definitions;
        definitions.timecodeFramesPerSecond = framesPerSecond;
        const auto parsed = talas::expr::parse(source, ExpressionLanguageVersion::ValV1, definitions);
        require(parsed.ok(), std::string{source} + " should parse");
        const auto result = talas::expr::evaluate(
            *parsed.expression, {}, talas::expr::EvaluationOptions{ValueDomain::Seconds});
        require(result.ok(), std::string{source} + " should evaluate");
        return result.value.value;
    };
    requireNear(seconds("00:01:12.15 SMPTE", 30), 72.5, "15 frames at 30 fps should be half a second");
    requireNear(seconds("00:01:12.15 SMPTE", 25), 72.6, "15 frames at 25 fps should be 0.6 s");
    requireNear(seconds("01:00:00.00 SMPTE", 24), 3600.0, "hours should count 3600 s");
    require(!talas::expr::parse("00:01:12.15", ExpressionLanguageVersion::ValV1).ok(),
        "a time code without SMPTE should be refused");

    const auto at = [](std::string_view source, double timeCode) {
        const auto parsed = talas::expr::parse(source, ExpressionLanguageVersion::ValV1);
        require(parsed.ok() && parsed.expression->eventReferences()
                == std::vector<std::string>{"MIDITimeCode"},
            std::string{source} + " should read the MIDI time code");
        EvaluationContext context;
        context.user = &timeCode;
        context.resolveEvent = [](void* user, std::string_view name, ExpressionValue& value) noexcept {
            if (name != "MIDITimeCode")
                return false;
            value = {*static_cast<double*>(user), ValueDomain::Scalar};
            return true;
        };
        const auto result = talas::expr::evaluate(
            *parsed.expression, context, talas::expr::EvaluationOptions{ValueDomain::Scalar});
        require(result.ok(), std::string{source} + " should evaluate");
        return result.value.value;
    };
    require(at("12 s triggerAtTimeCode", 11.9) == 0.0 && at("12 s triggerAtTimeCode", 12.0) == 1.0
            && at("12 s triggerAtTimeCode", 40.0) == 1.0,
        "triggerAtTimeCode should turn on once the time code reaches its time");
    require(at("20 s gateOnAtTimeCodeForDuration: 1 s", 19.9) == 0.0
            && at("20 s gateOnAtTimeCodeForDuration: 1 s", 20.0) == 1.0
            && at("20 s gateOnAtTimeCodeForDuration: 1 s", 20.9) == 1.0
            && at("20 s gateOnAtTimeCodeForDuration: 1 s", 21.0) == 0.0,
        "gateOnAtTimeCodeForDuration: should stay on for its duration");
}

// More about value fields: exponents written either
// way, integers in any radix with letters for numerals past nine, the time
// units including a single day, pitch classes by letter or solfege name,
// sharps, arithmetic between units converting to the left operand's unit,
// removeUnits, SMPTE times and comments in double quotes.
void testValueFieldSyntax()
{
    requireExamples({
        {"44.1E3", ValueDomain::Scalar, 44100.0},
        {"1.0E-6", ValueDomain::Scalar, 0.000001},
        {"2E32", ValueDomain::Scalar, 2.0e32, 1.0e17},
        {"1e6", ValueDomain::Scalar, 1.0e6},
        {"16rFFEF", ValueDomain::Scalar, 65519.0},
        {"2r1011", ValueDomain::Scalar, 11.0},
        {"12rBAABAA", ValueDomain::Scalar, 2963506.0},
        {"3r211", ValueDomain::Scalar, 22.0},
        {"16rFF + 1", ValueDomain::Scalar, 256.0},
        {"1 day + 3 m + 17 s", ValueDomain::Seconds, 86400.0 + 180.0 + 17.0},
        {"2 days", ValueDomain::Seconds, 172800.0},
        {"1 h", ValueDomain::Seconds, 3600.0},
        {"250 usec", ValueDomain::Seconds, 0.00025},
        {"4 c sharp", ValueDomain::NoteNumber, 61.0},
        {"4 do", ValueDomain::NoteNumber, 60.0},
        {"4 re", ValueDomain::NoteNumber, 62.0},
        {"4 mi", ValueDomain::NoteNumber, 64.0},
        {"4 fa", ValueDomain::NoteNumber, 65.0},
        {"4 so", ValueDomain::NoteNumber, 67.0},
        {"4 sol", ValueDomain::NoteNumber, 67.0},
        {"4 la", ValueDomain::NoteNumber, 69.0},
        {"4 ti", ValueDomain::NoteNumber, 71.0},
        {"4 si", ValueDomain::NoteNumber, 71.0},
        {"60 nn hz", ValueDomain::Hertz, 261.6255653, 1.0e-6},
        {"100 hz nn", ValueDomain::NoteNumber, 43.35, 0.005},
        {"60 nn + 100 hz", ValueDomain::Hertz, 361.6255653, 1.0e-6},
        {"60 nn + 100 hz nn", ValueDomain::NoteNumber, 103.35, 0.005},
        {"60 nn hz + 100 hz", ValueDomain::Hertz, 361.6255653, 1.0e-6},
        {"440 hz removeUnits * 2", ValueDomain::Scalar, 880.0},
        {"4 c hz removeUnits / SignalProcessor sampleRate", ValueDomain::Scalar, 261.6255653 / 48000.0, 1.0e-9},
        {"1:04:32.2 SMPTE", ValueDomain::Seconds, 3600.0 + 4.0 * 60.0 + 32.0 + 2.0 / 30.0, 0.05},
        {"2 \"two cycles a second\" hz", ValueDomain::Hertz, 2.0},
        {"\"a comment first\" 0.5", ValueDomain::Scalar, 0.5},
    }, "value fields");
}

// A number between 1 and -1 needs its leading
// zero, 0.5 and not .5, as a period not after a numeral ends a statement;
// Val refuses it, Nothing more expected->, naming the
// number to write. Our own languages keep
// reading .5 as 0.5, and a project before format 106 gains the zeros.
void testLeadingZeros()
{
    for (const auto* refused : {".5", "-.005", "!Fader * .5", ".1 s", "1 + .25"})
    {
        const auto parsed = talas::expr::parse(refused, ExpressionLanguageVersion::ValV1);
        require(!parsed.ok() && !parsed.diagnostics.empty()
                && parsed.diagnostics.front().message.find("leading zero") != std::string::npos,
            std::string{""} + refused + " should be refused for its missing leading zero");
    }
    const auto named = talas::expr::parse("!Fader * .25", ExpressionLanguageVersion::ValV1);
    require(!named.ok() && named.diagnostics.front().message.find("0.25") != std::string::npos
            && named.diagnostics.front().message.starts_with("Nothing more expected->")
            && named.diagnostics.front().range == talas::expr::SourceRange{9, 3},
        "the refusal should point at .25 and name 0.25");
    requireExamples({
        {"0.1", ValueDomain::Scalar, 0.1},
        {"-0.005", ValueDomain::Scalar, -0.005},
        {"3. ", ValueDomain::Scalar, 3.0},
    }, "leading zeros");
    require(talas::expr::parse(".5", ExpressionLanguageVersion::VariablesV2).ok(),
        "our own languages should still read .5");
    require(talas::expr::withLeadingZeros(".5 + (!Fader * .25) \"keep .5\" + 3.5 + 1.") == "0.5 + (!Fader * 0.25) \"keep .5\" + 3.5 + 1.",
        "a field should gain a 0 before each number without one, and nowhere else");
    require(talas::expr::withLeadingZeros("| a | a := 1. .5") == "| a | a := 1. .5"
            && talas::expr::withLeadingZeros("'.5' size") == "'.5' size",
        "a field's program and strings should keep their periods");
}

// A variable typed into a field is a ? followed by a letter
// and then letters or digits, without spaces: ?freq, ?formant2,
// ?theDaysAreAhead123456789, ?syntheseVirtuelle and ?fumoBianco are legal;
// ?20Jahrhunderts and ?table Lookup are not, nor are gaaf, without its ?,
// and ?snails bury.
void testVariableNames()
{
    for (const auto* legal : {"?freq", "?formant2", "?theDaysAreAhead123456789", "?syntheseVirtuelle", "?fumoBianco"})
    {
        const auto parsed = talas::expr::parse(legal, ExpressionLanguageVersion::ValV1);
        require(parsed.ok() && parsed.expression->variableReferences().size() == 1
                && parsed.expression->variableReferences().front() == std::string_view{legal}.substr(1),
            std::string{""} + legal + " should be a variable");
    }
    for (const auto* illegal : {"?20Jahrhunderts", "?table Lookup", "?snails bury"})
    {
        require(!talas::expr::parse(illegal, ExpressionLanguageVersion::ValV1).ok(),
            std::string{""} + illegal + " should not be a variable");
    }
    const auto bare = talas::expr::parse("gaaf", ExpressionLanguageVersion::ValV1);
    require(!bare.ok() || bare.expression->variableReferences().empty(),
        "gaaf, without its ?, should not be a variable");
}

// A value field can hold anything from a constant to a
// Smalltalk program, which declares temporaries between bars, assigns
// them with :=, comments in double quotes (whose periods end nothing), and
// gives the field its last statement's value. A temporary is read as the
// value last assigned to it; one not declared, or read before it is
// assigned, and a statement before the last that assigns nothing, are
// refused.
void testFieldPrograms()
{
    EvaluationContext context;
    context.sampleRate = 48000.0;
    context.tempoBpm = 120.0;
    requireExamples({
        {"| a | a := 0.25. a * 2", ValueDomain::Scalar, 0.5},
        {"| a b | \"The trigger. And how long.\" a := 0.25. b := a + 0.5. b * 2.", ValueDomain::Scalar, 1.5},
        {"| a | a := 1. a := a + 1. a * 0.25", ValueDomain::Scalar, 0.5},
        {"| d | d := 6 s. d * 2", ValueDomain::Seconds, 12.0},
        {"| t f | t := 0.5. f := 440 hz. f * t", ValueDomain::Hertz, 220.0},
        {"| x | x := 1.5. x + 0.25", ValueDomain::Scalar, 1.75},
    }, "field programs");
    // A TimeLeft & Trigger program, its Sound and Event Values given.
    const auto timeLeft = talas::expr::parse(
        "| inputTrigger outputTriggerDuration |\n"
        "\"The trigger to start our trigger and how long it should be on.\"\n"
        "inputTrigger := !Level threshold: (!Threshold * 0.1).\n"
        "outputTriggerDuration := 6 s.\n"
        "1 - (inputTrigger * (1 - !TimeLeft asLogicValue)\n"
        "ramp: outputTriggerDuration)",
        ExpressionLanguageVersion::ValV1);
    require(timeLeft.ok() && timeLeft.expression->eventReferences().size() == 3,
        "TimeLeft & Trigger's program should parse, reading !Level, !Threshold and !TimeLeft");
    for (const auto* refused : {"| a | b := 1. 2", "| a | a + 1", "| a | a := 1. 2. a", "| a a := 1. a"})
    {
        require(!talas::expr::parse(refused, ExpressionLanguageVersion::ValV1).ok(),
            std::string{"a field program "} + refused + " should be refused");
    }
    require(std::abs(evaluate("0.5 + 0.25", ValueDomain::Scalar, context).value - 0.75) < 1.0e-12,
        "a field without a program should read as before");
}

// Each ramp-like function has a repeating version, which
// repeats for as long as its receiver is positive; bpm: is 1 for half of
// each beat while its receiver is positive, at a rate that may move, 1 bpm:
// (!Rate * 208), and once every three seconds at 60.0/3; bpm:dutyCycle:
// stays at 1 for its share of each beat, never at 0 and always at 1.
// Of:'s index may be a real-time expression. And true:false:
// evaluates only the argument it takes.
// Bpm: gives !KeyDown-like triggers, each
// beat -1 for its first five milliseconds to steal its voice, then 1 for
// the rest of its duty cycle, the dip no longer than the time on;
// !TimingClock's pulses do not dip; and a project before format 112 keeps
// its old beats through abs.
// A key below 60 but not 48 by
// comparisons multiplied; mod: 12 moving round a circle, clockwise and
// back; a fader octave by octave as the power of 2; and logarithms turning
// multiplication into addition and division into subtraction.
void testDocumentedArithmeticRemarks()
{
    Timeline taps("!X * (3 inverse)");
    require(std::abs(taps.at(0.0, 0.9) - 0.3) < 1.0e-12 && Timeline("0.9 * (3 inverse)").at(0.0) == Timeline("0.9 * 1 / 3").at(0.0),
        "!Delay * (3 inverse) should be !Delay * 1 / 3");
    Timeline keys("(!X lt: 60) * ((!X eq: 48) le: 0)");
    require(keys.at(0.0, 50.0) == 1.0 && keys.at(0.001, 48.0) == 0.0 && keys.at(0.002, 70.0) == 0.0,
        "keys below 60 other than 48 should give 1");
    require(Timeline("(1 - 2) mod: 12").at(0.0) == 11.0 && Timeline("(11 + 2) mod: 12").at(0.0) == 1.0,
        "adding moves clockwise round the circle of 12, subtracting counterclockwise");
    Timeline octaves("(8.1758d hz * (2 ** !X)) removeUnits");
    require(std::abs(octaves.at(0.0, 1.0) - 16.3516) < 1.0e-9 && std::abs(octaves.at(0.001, 3.0) - 65.4064) < 1.0e-9,
        "the fader should set how many octaves up, the power of 2");
    require(Timeline("(8 / 4) twoLog").at(0.0) == Timeline("8 twoLog - 4 twoLog").at(0.0)
            && Timeline("(8 * 4) twoLog").at(0.0) == Timeline("8 twoLog + 4 twoLog").at(0.0),
        "in log space multiplication should be addition and division subtraction");
}

// Each random expression returns numbers of its
// own: two fields' 1 s random, their salts apart, draw apart, the same salt
// alike, and salt 0 draws as projects before format 114 did.
void testRandomExpressionsDrawTheirOwn()
{
    const auto draws = [](std::uint32_t salt) {
        Timeline random("1 s random", false, false, salt);
        std::vector<double> values;
        for (const auto seconds : {0.0, 1.5, 2.5, 3.5})
            values.push_back(random.at(seconds));
        return values;
    };
    const auto start = draws(7);
    require(start == draws(7) && start != draws(9) && start != draws(0),
        "random expressions of fields apart should draw apart, each its own numbers");
    Timeline unsalted("1 s random");
    require(unsalted.at(0.0) == draws(0).front(), "salt 0 should draw as before format 114");
}

// (1 - !Freeze) trackAndHold: a ramp follows
// the ramp while !Freeze, here !X, is 0 and holds it while 1; the
// arpeggiator's latches; and an array collected in braces within an
// expression, each element of which runs on every tick.
void testLatchesHoldEventValues()
{
    Timeline frozen("(1 - !X) trackAndHold: (1 repeatingFullRamp: 2.56855 s)");
    std::vector<double> tracked;
    for (int tick = 0; tick <= 2000; ++tick)
        tracked.push_back(frozen.at(tick / 1000.0, tick >= 1000 ? 1.0 : 0.0));
    require(tracked[500] > tracked[400] && tracked[1500] == tracked[1000] && tracked[2000] == tracked[1000],
        "the ramp should be tracked while !Freeze is 0 and held while it is 1");
    // The arpeggiator: five sampleAndHold: latches, each taking
    // the key number on a different key down, which a metronome steps
    // through; !X stands for !KeyDown and !Y for !KeyNumber.
    Timeline arpeggio("(((1 bpm: 600) countTriggersMod: 5) of: {(0 to: 4) collect: [:i | ((!X countTriggersMod: 5) eq: i) "
                      "sampleAndHold: !Y]}) removeUnits");
    const std::array keys{60.0, 62.0, 64.0, 65.0, 67.0};
    std::set<double> heard;
    for (int tick = 0; tick < 2000; ++tick)
    {
        const auto down = tick < 100 && tick % 20 < 10 ? 1.0 : 0.0;
        const auto value = arpeggio.at(tick / 1000.0, down, keys[static_cast<std::size_t>(std::min(tick / 20, 4))]);
        if (tick >= 1000)
            heard.insert(value);
    }
    std::string values;
    for (const auto value : heard)
        values += " " + std::to_string(value);
    require(heard == std::set<double>{keys.begin(), keys.end()},
        "five keys played should be held in five latches and arpeggiated, each in turn, not" + values);
    // A braced Interval collected through a block is the Array of its
    // values at each step.
    // !Amp suffix2: i names a fader for each step, in two
    // digits, so a collect: makes as many faders as it has steps.
    const auto faders = talas::expr::parse("(!Index * 15) of: {(1 to: 16) collect: [:i | !Amp suffix2: i]}",
        ExpressionLanguageVersion::ValV1);
    require(faders.ok() && faders.expression->eventReferences().size() == 17
            && std::ranges::count(faders.expression->eventReferences(), std::string{"Amp01"}) == 1
            && std::ranges::count(faders.expression->eventReferences(), std::string{"Amp16"}) == 1,
        "{(1 to: 16) collect: [:i | !Amp suffix2: i]} should name sixteen faders, !Amp01 to !Amp16");
    const auto plain = talas::expr::parse("1 of: {(9 to: 10) collect: [:i | !Level suffix: i]}", ExpressionLanguageVersion::ValV1);
    require(plain.ok()
            && std::set<std::string>(plain.expression->eventReferences().begin(), plain.expression->eventReferences().end())
                == std::set<std::string>{"Level9", "Level10"},
        "suffix: should put the number after the name as it is");
    require(Timeline("1 of: {(0 to: 2) collect: [:i | i * 2]}").at(0.0) == 2.0
            && Timeline("2 of: {(1 to: 5 by: 2) collect: [:n | n + !X]}").at(0.0, 0.5) == 5.5,
        "{(0 to: 4) collect: [:i | ...]} should be the Array of the block's values");
    // A sine turns an ever increasing time into a repeating pattern.
    Timeline circling("(!LocalTime * 2 * Float pi) sin");
    require(std::abs(circling.at(0.25) - 1.0) < 1.0e-9 && std::abs(circling.at(1.25) - 1.0) < 1.0e-9
            && std::abs(circling.at(2.75) + 1.0) < 1.0e-9,
        "the sine of an increasing time should repeat once a second");
    // Eight random frequencies, each a power of the golden ratio
    // between its step's and twice it.
    for (int index = 0; index < 8; ++index)
    {
        Timeline powers(std::to_string(index)
            + " of: {(10 to: 17) collect: [:i | (Float phi ** ((!X nextRandom abs + 1 * i) rounded))]}");
        std::set<long> exponents;
        for (int step = 0; step < 40; ++step)
        {
            const auto power = std::log(powers.at(0.01 * step, step % 2)) / std::log((1.0 + std::sqrt(5.0)) / 2.0);
            const auto exponent = std::lround(power);
            require(std::abs(power - static_cast<double>(exponent)) < 1.0e-9 && exponent >= 10 + index
                    && exponent <= 2 * (10 + index),
                "each frequency should be a power of the golden ratio from its step to twice it, not phi ** "
                    + std::to_string(power));
            exponents.insert(exponent);
        }
        require(exponents.size() > 2, "the frequencies should be drawn at random");
    }
}

void testMetronomesStealTheirVoices()
{
    Timeline beats("1 bpm: 60");
    for (const auto& [seconds, expected] : std::initializer_list<std::pair<double, double>>{{0.0, -1.0}, {0.004, -1.0},
             {0.005, 1.0}, {0.499, 1.0}, {0.5, 0.0}, {0.999, 0.0}, {1.0, -1.0}, {1.004, -1.0}, {1.005, 1.0}})
    {
        require(beats.at(seconds) == expected, "1 bpm: 60 at " + std::to_string(seconds) + " s should be "
                + std::to_string(expected));
    }
    Timeline brief("1 bpm: 60 dutyCycle: 0.003");
    require(brief.at(0.0) == -1.0 && brief.at(0.002) == -1.0 && brief.at(0.003) == 0.0,
        "a beat on for 3 ms should dip for those 3 ms only");
    Timeline gated("(1 bpm: 60) durationOfGate");
    for (auto tick = 0; tick < 600; ++tick)
        (void) gated.at(tick / 1000.0);
    require(std::abs(gated.at(0.6) - 0.495) < 1.0e-9,
        "(1 bpm: 60) durationOfGate should be 0.495, not counting the -1 for voice stealing");
    Timeline clock("!TimingClock");
    require(clock.at(0.0) == 1.0, "!TimingClock's pulses should not dip");
    const std::vector<std::pair<std::string, std::string>> rewritten{{"1 bpm: 60", "(1 bpm: 60) abs"},
        {"(!X + 1) bpm: !Y dutyCycle: 0.25", "((!X + 1) bpm: !Y dutyCycle: 0.25) abs"},
        {"3 * (1 bpm: 120) + !Y", "3 * ((1 bpm: 120) abs) + !Y"}, {"!TimingClock + 1", "!TimingClock + 1"}};
    for (const auto& [source, legacy] : rewritten)
    {
        require(talas::expr::withMetronomesWithoutVoiceStealing(source) == legacy,
            "a format 111 expression " + source + " should become " + legacy + ", not "
                + talas::expr::withMetronomesWithoutVoiceStealing(source));
    }
    Timeline steady("(1 bpm: 60) abs");
    require(steady.at(0.0) == 1.0 && steady.at(0.004) == 1.0 && steady.at(0.499) == 1.0 && steady.at(0.5) == 0.0,
        "a format 111 project's beats should go straight to 1, as they did");
}

void testRepeatingFunctionsAndMetronomes()
{
    const auto near = [](double value, double expected, std::string_view what) {
        require(std::abs(value - expected) < 0.01,
            std::string{what} + " should be " + std::to_string(expected) + ", not " + std::to_string(value));
    };
    Timeline ramp("!X repeatingRamp: 1 s");
    (void) ramp.at(0.0, 1.0);
    near(ramp.at(0.25, 1.0), 0.25, "repeatingRamp: after 0.25 s");
    near(ramp.at(1.25, 1.0), 0.25, "repeatingRamp: repeating after 1.25 s");
    near(ramp.at(1.75, 1.0), 0.75, "repeatingRamp: after 1.75 s");
    Timeline plain("!X repeatingRamp");
    (void) plain.at(0.0, 1.0);
    near(plain.at(0.5, 1.0), 0.5, "repeatingRamp after 0.5 s");
    near(plain.at(1.5, 1.0), 0.5, "repeatingRamp repeating after 1.5 s");
    Timeline full("!X repeatingFullRamp: 1 s");
    (void) full.at(0.0, 1.0);
    near(full.at(0.25, 1.0), -0.5, "repeatingFullRamp: after 0.25 s");
    near(full.at(1.75, 1.0), 0.5, "repeatingFullRamp: repeating after 1.75 s");
    Timeline fullPlain("!X repeatingFullRamp");
    (void) fullPlain.at(0.0, 1.0);
    near(fullPlain.at(0.5, 1.0), 0.0, "repeatingFullRamp after 0.5 s");
    near(fullPlain.at(1.25, 1.0), -0.5, "repeatingFullRamp repeating after 1.25 s");

    // Rising edges and the share of the time at 1, a millisecond at a time.
    const auto metronome = [](std::string_view source, double seconds, double x) {
        Timeline clock(source);
        auto rises = 0;
        auto ones = 0;
        auto previous = 0.0;
        const auto steps = static_cast<int>(seconds * 1000.0);
        for (int step = 0; step < steps; ++step)
        {
            const auto value = clock.at(step / 1000.0, x);
            rises += previous <= 0.0 && value > 0.0;
            ones += value > 0.0;
            previous = value;
        }
        return std::pair{rises, static_cast<double>(ones) / steps};
    };
    const auto keyed = metronome("!X bpm: 60", 4.0, 1.0);
    require(keyed.first == 4 && std::abs(keyed.second - 0.5) < 0.01,
        "!KeyDown bpm: 60 should be 1 once a second, for half of it, while the key is down");
    require(metronome("!X bpm: 60", 4.0, 0.0).first == 0, "bpm: should rest at 0 while its receiver is not positive");
    const auto slower = metronome("1 bpm: (!X * 208)", 6.0, 0.5).first;
    const auto faster = metronome("1 bpm: (!X * 208)", 6.0, 1.0).first;
    require(slower == 11 && faster == 21, "1 bpm: (!Rate * 208) should repeat at 104 and 208 beats a minute ("
            + std::to_string(slower) + ", " + std::to_string(faster) + ")");
    require(metronome("1 bpm: (60.0/3)", 8.9, 0.0).first == 3, "1 bpm: (60.0/3) should trigger once every three seconds");
    // Quarter-note quintuplets at MM = 160 while a key is
    // down, 200 a minute; and a ramp from 0 to 1 over 12 seconds.
    require(metronome("!X bpm: 160 * (5/4)", 2.99, 1.0).first == 10,
        "!KeyDown bpm: 160 * (5/4) should trigger five times in the time of four at MM = 160");
    Timeline twelve("1 ramp: 12 s");
    (void) twelve.at(0.0);
    near(twelve.at(6.0), 0.5, "1 ramp: 12 s halfway");
    near(twelve.at(12.5), 1.0, "1 ramp: 12 s at its end");
    near(metronome("1 bpm: 60 dutyCycle: 0.25", 4.0, 0.0).second, 0.25, "a duty cycle of 0.25's share at 1");
    near(metronome("1 bpm: 60 dutyCycle: 0.75", 4.0, 0.0).second, 0.75, "a duty cycle of 0.75's share at 1");
    require(metronome("1 bpm: 60 dutyCycle: 0", 4.0, 0.0).first == 0, "a duty cycle of 0 should never go to 1");
    near(metronome("1 bpm: 60 dutyCycle: 1", 4.0, 0.0).second, 1.0, "a duty cycle of 1 should stick at 1");

    // From -1 to 1 over 10 seconds, and then again.
    Timeline cloud("1 repeatingFullRamp: (10 s)");
    near(cloud.at(0.0), -1.0, "1 repeatingFullRamp: (10 s) at its start");
    near(cloud.at(5.0), 0.0, "1 repeatingFullRamp: (10 s) halfway");
    near(cloud.at(9.99), 1.0, "1 repeatingFullRamp: (10 s) at its end");
    near(cloud.at(12.5), -0.5, "1 repeatingFullRamp: (10 s) repeating");

    // At 44.1 kHz, 256 samp inverse is the frequency
    // whose period is 256 samples, 172.265625 hz, its multiples the
    // harmonics; 16 samp inverse is a very high one.
    EvaluationContext cd;
    cd.sampleRate = 44100.0;
    near(evaluate("256 samp inverse", ValueDomain::Hertz, cd).value, 172.265625, "256 samp inverse");
    near(evaluate("256 samp inverse * 3", ValueDomain::Hertz, cd).value, 3.0 * 172.265625, "its third harmonic");
    near(evaluate("16 samp inverse", ValueDomain::Hertz, cd).value, 2756.25, "16 samp inverse");

    Timeline indexed("(!X * 3) of: #(8 9 10 11)");
    near(indexed.at(0.0, 0.0), 8.0, "of: at a moving index of 0");
    near(indexed.at(0.1, 0.5), 9.0, "of: at a moving index of 1.5");
    near(indexed.at(0.2, 1.0), 11.0, "of: at a moving index of 3");

    // The ramp in the true argument starts when it is first taken.
    Timeline lazy("!X true: (1 ramp: 1 s) false: 0");
    near(lazy.at(0.0, 0.0), 0.0, "true:false: while false");
    near(lazy.at(0.5, 0.0), 0.0, "true:false: still false");
    near(lazy.at(0.75, 1.0), 0.0, "true:false: should start its true argument only when it takes it");
    near(lazy.at(1.0, 1.0), 0.25, "true:false: should evaluate only the argument it takes");
}

// OnDuration fields that stretch a resynthesis:
// 2.39415 s + ((1 - !Rate) * 10), its normal duration at a !Rate of 1 and
// longer below it, and 5 s + (100 * !KeyDown) s, 5 seconds that jump to
// 105 while a key is down.
void testStretchingDurations()
{
    struct Values
    {
        double rate = 1.0;
        double keyDown = 0.0;
    } values;
    EvaluationContext context;
    context.user = &values;
    context.resolveEvent = [](void* user, std::string_view name, ExpressionValue& value) noexcept {
        const auto& given = *static_cast<const Values*>(user);
        if (name != "Rate" && name != "KeyDown")
            return false;
        value = {name == "Rate" ? given.rate : given.keyDown, ValueDomain::Scalar};
        return true;
    };
    const auto seconds = [&](std::string_view source) { return evaluate(source, ValueDomain::Seconds, context).value; };
    require(std::abs(seconds("2.39415 s + ((1 - !Rate) * 10)") - 2.39415) < 1.0e-12,
        "at a !Rate of 1 the OnDuration should be its normal 2.39415 s");
    values.rate = 0.5;
    require(std::abs(seconds("2.39415 s + ((1 - !Rate) * 10)") - 7.39415) < 1.0e-12,
        "a !Rate below 1 should make the OnDuration longer");
    require(std::abs(seconds("5 s + (100 * !KeyDown) s") - 5.0) < 1.0e-12,
        "the OnDuration should be 5 s with no key down");
    values.keyDown = 1.0;
    require(std::abs(seconds("5 s + (100 * !KeyDown) s") - 105.0) < 1.0e-12,
        "the OnDuration should jump to 105 s while a key is down");
}

// The ratio of two pitches as note numbers differs from
// their ratio in hertz: 4 a and 3 a are 69/57, about 1.2, as note numbers
// but exactly 2 in hertz.
void testNoteNumberAndHertzRatios()
{
    EvaluationContext context;
    require(std::abs(evaluate("4 a hz / 3 a hz", ValueDomain::Scalar, context).value - 2.0) < 1.0e-12,
        "4 a over 3 a in hertz should be exactly 2");
    require(std::abs(evaluate("4 a nn removeUnits / 3 a nn removeUnits", ValueDomain::Scalar, context).value - 69.0 / 57.0) < 1.0e-12,
        "4 a over 3 a in note numbers should be 69/57");
}

// A morph between the analyses at 3 c sharp and 4 d sharp
// follows the key, (!KeyNumber - 3 c sharp removeUnits) / (4 d sharp -
// 3 c sharp) removeUnits, the key's place from the low end of the range
// over the whole range.
void testKeyMorphRatio()
{
    struct Key
    {
        static bool resolve(void* user, std::string_view name, ExpressionValue& value) noexcept
        {
            if (name != "KeyNumber")
                return false;
            value = {*static_cast<double*>(user), ValueDomain::Scalar};
            return true;
        }
    };
    constexpr auto morph = "(!KeyNumber - 3 c sharp removeUnits) / (4 d sharp - 3 c sharp) removeUnits";
    for (const auto& [key, ratio] : {std::pair{49.0, 0.0}, std::pair{56.0, 0.5}, std::pair{63.0, 1.0}})
    {
        auto number = key;
        EvaluationContext context;
        context.user = &number;
        context.resolveEvent = Key::resolve;
        require(std::abs(evaluate(morph, ValueDomain::Scalar, context).value - ratio) < 1.0e-12,
            "key " + std::to_string(key) + " should morph " + std::to_string(ratio)
                + " of the way from 3 c sharp to 4 d sharp");
    }
}

// Mod: is clock arithmetic, 9 o'clock and two hours is 11,
// and 11 o'clock and two hours is 1, not 13.
void testClockArithmetic()
{
    requireExamples({
        {"(9 + 2) mod: 12", ValueDomain::Scalar, 11.0},
        {"(11 + 2) mod: 12", ValueDomain::Scalar, 1.0},
        {"(70 mod: 5)", ValueDomain::Scalar, 0.0},
        {"(64 mod: 5)", ValueDomain::Scalar, 4.0},
    }, "clock arithmetic");
}

// A step of 0.0026 hz is a hundredth of a cent at 4 a, an
// 880th of a cent at 4000 hz and a sixth of a cent at 30 hz.
void testSmallestFrequencyStepInCents()
{
    const auto cents = [](double hertz) {
        EvaluationContext context;
        const auto source = "((" + std::to_string(hertz) + " + 0.0026) hz nn - " + std::to_string(hertz)
            + " hz nn) removeUnits * 100";
        return evaluate(source, ValueDomain::Scalar, context).value;
    };
    for (const auto& [hertz, fraction] : {std::pair{440.0, 100.0}, std::pair{4000.0, 880.0}, std::pair{30.0, 6.0}})
    {
        const auto step = cents(hertz);
        require(std::abs(step * fraction - 1.0) < 0.15,
            "0.0026 hz at " + std::to_string(hertz) + " hz should be about 1/" + std::to_string(fraction)
                + " of a cent, not " + std::to_string(step));
    }
}

// 1 bpm: (4 s bpm removeUnits) triggers once every four
// seconds, bpm: taking the unitless beats per minute. A project before
// format 110 writes hz after such a bpm, keeping the hertz it had.
void testBpmWithoutItsUnits()
{
    // Beats at a pitch's frequency over 100: 60 nn hz bpm removeUnits
    // / 100 is middle c's beats per minute over 100.
    requireExamples({{"60 nn hz bpm removeUnits / 100", ValueDomain::Scalar, 261.6255653005986 * 60.0 / 100.0, 1.0e-9}},
        "beats at a pitch");
    // A bpm frequency a number multiplies or divides stays
    // in bpm, so a 2.96961 s four-beat sample's tempo is 80.8187.
    requireExamples({
        {"(2 s bpm * 4) removeUnits", ValueDomain::Scalar, 120.0, 1.0e-9},
        {"(2.96961 s bpm * 4) removeUnits", ValueDomain::Scalar, 80.8187, 1.0e-4},
        {"(120 bpm / 2) removeUnits", ValueDomain::Scalar, 60.0, 1.0e-9},
        {"(4 * (2 s bpm)) removeUnits", ValueDomain::Scalar, 120.0, 1.0e-9},
        {"(2 s bpm hz * 4) removeUnits", ValueDomain::Scalar, 2.0, 1.0e-9},
    }, "a tempo times a number");
    require(talas::expr::withBpmTimesNumbersRemoveUnitsInHertz("(2 s bpm * 4) removeUnits") == "(2 s bpm hz * 4) removeUnits"
            && talas::expr::withBpmTimesNumbersRemoveUnitsInHertz("120 bpm removeUnits") == "120 bpm removeUnits"
            && talas::expr::withBpmTimesNumbersRemoveUnitsInHertz("(2 s bpm * 4) s") == "(2 s bpm * 4) s",
        "an older project's bpm times a number before removeUnits should gain hz, and nothing else");
    Timeline every("1 bpm: (4 s bpm removeUnits)");
    require(every.at(0.0) == -1.0 && every.at(1.0) == 1.0 && every.at(2.5) == 0.0 && every.at(4.0) == -1.0
            && every.at(4.005) == 1.0,
        "1 bpm: (4 s bpm removeUnits) should beat every four seconds");
    require(talas::expr::withBpmRemoveUnitsInHertz("120 bpm removeUnits") == "120 bpm hz removeUnits"
            && talas::expr::withBpmRemoveUnitsInHertz("1 bpm: ((4 s bpm) removeUnits)") == "1 bpm: ((4 s bpm hz) removeUnits)"
            && talas::expr::withBpmRemoveUnitsInHertz("!BPM bpm s") == "!BPM bpm s"
            && talas::expr::withBpmRemoveUnitsInHertz("'bpm removeUnits' size") == "'bpm removeUnits' size",
        "an older project's bpm before removeUnits should gain hz, and nothing else");
}

// Triangle: rises and falls over its duration, the
// repeating triangles go on while their trigger is on, and the seeded
// normal and exponential random numbers repeat for their seed.
void testTrianglesAndSeededRandomNumbers()
{
    Timeline once("!X triangle: 10 s");
    once.at(0.0, 0.0);
    for (const auto& [time, value] : {std::pair{1.0, 0.0}, {3.5, 0.5}, {6.0, 1.0}, {8.5, 0.5}, {11.0, 0.0}})
        requireNear(once.at(time, 1.0), value, "triangle: 10 s should rise for 5 s and fall for 5 s, at "
            + std::to_string(time));
    Timeline repeating("!X repeatingTriangle");
    repeating.at(0.0, 0.0);
    for (const auto& [time, value] : {std::pair{1.0, 0.0}, {1.25, 0.5}, {1.5, 1.0}, {1.75, 0.5}, {2.0, 0.0}, {2.25, 0.5}})
        requireNear(repeating.at(time, 1.0), value, "repeatingTriangle should go up and down each 0.5 s, at "
            + std::to_string(time));
    Timeline longer("!X repeatingTriangle: 2.3 s");
    longer.at(0.0, 0.0);
    for (const auto& [time, value] : {std::pair{1.0, 0.0}, {2.15, 1.0}, {3.3, 0.0}, {4.45, 1.0}})
        requireNear(longer.at(time, 1.0), value, "repeatingTriangle: 2.3 s should go up for 1.15 s and down for 1.15 s, at "
            + std::to_string(time));
    const auto draws = [](std::string_view source) {
        Timeline drawn(source);
        std::vector<double> values;
        for (int beat = 0; beat < 40; ++beat)
        {
            drawn.at(beat * 0.1, 0.0);
            values.push_back(drawn.at(beat * 0.1 + 0.05, 1.0));
        }
        return values;
    };
    const auto normal = draws("!X nextNormalWithSeed: 3413");
    const auto exponential = draws("!X nextRandExpWithSeed: 3413");
    require(normal == draws("!X nextNormalWithSeed: 3413") && normal != draws("!X nextNormalWithSeed: 7")
            && std::ranges::all_of(normal, [](double value) { return value > -1.0 && value < 1.0; }),
        "nextNormalWithSeed: should repeat its normal numbers in (-1, 1) for its seed");
    require(exponential == draws("!X nextRandExpWithSeed: 3413")
            && std::ranges::all_of(exponential, [](double value) { return value >= 0.0 && value <= 24.0; }),
        "nextRandExpWithSeed: should repeat its exponential numbers in (0, 24) for its seed");
}

// SignalProcessor answers the inverse sample rate and
// its half's, a wavetable's length, and the default bank sizes of the
// FilterBank, FormantBank, CloudBank, SumOfSines and OscillatorBank, and
// Vocoder, which are those prototypes' own.
void testSignalProcessorMessages()
{
    requireExamples({
        {"SignalProcessor inverseSampleRate", ValueDomain::Scalar, 1.0 / 48000.0, 1.0e-15},
        {"SignalProcessor inverseHalfSampleRate", ValueDomain::Scalar, 1.0 / 24000.0, 1.0e-15},
        {"SignalProcessor tableLength", ValueDomain::Scalar, 4096.0},
        {"SignalProcessor bankSizeForBandpassFilters", ValueDomain::Scalar, 8.0},
        {"SignalProcessor bankSizeForFormantOscillator", ValueDomain::Scalar, 4.0},
        {"SignalProcessor bankSizeForGrainCloud", ValueDomain::Scalar, 8.0},
        {"SignalProcessor bankSizeForSOS", ValueDomain::Scalar, 8.0},
        {"SignalProcessor bankSizeForVocoder", ValueDomain::Scalar, 16.0},
    }, "SignalProcessor");
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto bankSize = [&](std::string_view typeId) {
        const auto* type = registry.findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a prototype");
        const auto field = std::ranges::find(type->parameters, std::string_view{"bankSize"}, &talas::core::ParameterDescriptor::id);
        require(field != type->parameters.end(), std::string{typeId} + " should have a BankSize");
        return field->defaultExpression;
    };
    require(bankSize("talas.filterBank") == "8" && bankSize("talas.formantBank") == "4" && bankSize("talas.sumOfSines") == "8"
            && bankSize("talas.oscillatorBank") == "8" && bankSize("talas.vocoder") == "16",
        "SignalProcessor's bank sizes should be the prototypes' defaults");
    const auto* cloud = registry.findNodeType("talas.cloudBank");
    require(cloud != nullptr, "talas.cloudBank should be a prototype");
    const auto cloudBankSize = std::ranges::find(cloud->parameters, std::string_view{"bankSize"}, &talas::core::ParameterDescriptor::id);
    require(cloudBankSize != cloud->parameters.end() && cloudBankSize->defaultKeywordValue == std::optional<double>{8.0},
        "SignalProcessor bankSizeForGrainCloud should be the CloudBank's default BankSize");
}

// The values of source every 5 ms for 20 s, with !X and !Y held.
std::vector<double> sampled(std::string_view source, double x = 0.0, double y = 0.0)
{
    Timeline timeline(source);
    std::vector<double> values;
    for (int step = 0; step < 4000; ++step)
        values.push_back(timeline.at(step * 0.005, x, y));
    return values;
}

// The most useful arithmetic expressions: scaling and
// offsetting ranges, (-1,1) and (0,1) converted into each other, one value
// against another, quantizing, vmin: and vmax:, sign, mod:, normSin and
// normCos, panning by amplitude and by energy, and octaves as powers of 2.
void testTopTenArithmeticExpressions()
{
    const auto over = [](std::string_view source, double x = 0.0, double y = 0.0) { return sampled(source, x, y); };
    const auto within = [](const std::vector<double>& values, double low, double high) {
        const auto [least, most] = std::ranges::minmax(values);
        return least >= low && most <= high && least < low + 0.2 * (high - low) && most > high - 0.2 * (high - low);
    };
    // A key number's (0,127) scaled down into (0,1), by 1/127 or by
    // dividing by its largest value; the random (-1,1) constrained between
    // a Min and a Max.
    for (const auto& source : {"!X * (1.0/127.0)", "!X / 127.0"})
    {
        Timeline scaled(source);
        requireNear(scaled.at(0.0, 0.0), 0.0, std::string{""} + source + " at key 0");
        requireNear(scaled.at(0.0, 127.0), 1.0, std::string{""} + source + " at key 127");
    }
    require(within(over("(0.1 s random abs) * (!Y - !X) + !X", 0.2, 0.6), 0.2, 0.6),
        "a random number scaled by Max - Min and offset by Min should lie between them");
    // 0.1 s random * 0.5 lies in (-0.5,0.5); adding 1 and halving,
    // or taking the absolute value, puts it in (0,1).
    require(within(over("0.1 s random * 0.5"), -0.5, 0.5),
        "0.1 s random * 0.5 should lie in (-0.5,0.5)");
    require(within(over("0.1 s random + 1 * 0.5"), 0.0, 1.0) && within(over("0.1 s random abs"), 0.0, 1.0),
        "0.1 s random + 1 * 0.5 and 0.1 s random abs should lie in (0,1), never negative");
    // 1 repeatingRamp * 2 is (0,2), shifted down by 1 to the
    // (-1,1) a TimeIndex takes, which repeatingFullRamp gives directly.
    const auto ramp = over("1 repeatingRamp * 2");
    require(within(ramp, 0.0, 2.0), "1 repeatingRamp * 2 should be in (0,2)");
    const auto shifted = over("1 repeatingRamp * 2 - 1");
    const auto full = over("1 repeatingFullRamp");
    for (std::size_t index = 0; index < full.size(); ++index)
        require(std::abs(full[index] - shifted[index]) < 1.0e-9,
            "1 repeatingFullRamp should be 1 repeatingRamp * 2 - 1, at step " + std::to_string(index));
    // 1 - !Density falls as !Density rises; 1 / !Fader1 grows
    // steeply as the fader nears 0.
    Timeline opposed("1 - !X");
    require(opposed.at(0.0, 0.25) == 0.75 && opposed.at(0.0, 0.75) == 0.25,
        "1 - !Density should get smaller as !Density gets larger");
    Timeline inverse("1 / !X");
    requireNear(inverse.at(0.0, 0.5), 2.0, "1 / !Fader1 at 0.5");
    requireNear(inverse.at(0.0, 0.01), 100.0, "1 / !Fader1 at 0.01");
    requireNear(inverse.at(0.0, 0.001), 1000.0, "1 / !Fader1 at 0.001");
    // !LogFreq rounded nn steps by half steps; rounding a (0,1)
    // value leaves its extremes, so multiply by the levels, round and
    // divide back: 0, 0.25, 0.5, 0.75 and 1, as roundTo: 0.25 does.
    Timeline halfSteps("(!X rounded nn) removeUnits");
    requireNear(halfSteps.at(0.0, 60.4), 60.0, "!LogFreq rounded nn at 60.4");
    requireNear(halfSteps.at(0.0, 60.6), 61.0, "!LogFreq rounded nn at 60.6");
    require(std::ranges::all_of(over("!X rounded", 0.3), [](double value) { return value == 0.0; })
            && std::ranges::all_of(over("!X rounded", 0.7), [](double value) { return value == 1.0; }),
        "rounding a (0,1) value should leave only 0 or 1");
    for (const auto& source : {"((1 bpm: 80.8187) nextRandom abs * 4) rounded / 4",
             "(1 bpm: 80.8187) nextRandom abs roundTo: 0.25"})
    {
        std::set<double> levels;
        for (const auto value : over(source))
            levels.insert(value);
        require(levels == std::set<double>{0.0, 0.25, 0.5, 0.75, 1.0},
            std::string{""} + source + " should choose 0, 0.25, 0.5, 0.75 or 1");
    }
    // The loop runs from the smaller fader to the larger, whichever
    // is larger, and the sign of their difference turns it backwards.
    for (const auto& [start, end] : {std::pair{0.2, 0.7}, {0.7, 0.2}})
    {
        Timeline smaller("!X vmin: !Y");
        Timeline larger("!X vmax: !Y");
        Timeline direction("(!Y - !X) sign");
        require(smaller.at(0.0, start, end) == 0.2 && larger.at(0.0, start, end) == 0.7
                && direction.at(0.0, start, end) == (end > start ? 1.0 : -1.0),
            "the loop should begin at the smaller fader, end at the larger, and play backwards "
            "when the start exceeds the end");
    }
    // Mod: 12 tells how many half steps a key is above a C, so any
    // C passes the test; spelling in thirds from A comes back to A after
    // going twice around the seven letters.
    Timeline cness("(!X mod: 12) eq: 0");
    require(cness.at(0.0, 48.0) == 1.0 && cness.at(0.0, 60.0) == 1.0 && cness.at(0.0, 72.0) == 1.0
            && cness.at(0.0, 61.0) == 0.0 && cness.at(0.0, 67.0) == 0.0,
        "(!KeyNumber mod: 12) eq: 0 should be true for every C and no other key");
    Timeline gated("!Y gateWhen: ((!X mod: 12) eq: 0)");
    require(gated.at(0.0, 61.0, 1.0) == 0.0 && gated.at(0.1, 60.0, 0.0) == 0.0 && gated.at(0.2, 60.0, 1.0) == 1.0,
        "!KeyDown gateWhen: ((!KeyNumber mod: 12) eq: 0) should open only for a C");
    std::string letters;
    for (int third = 0; third <= 7; ++third)
        letters += "ABCDEFG"[static_cast<std::size_t>(evaluate("(" + std::to_string(third) + " * 2) mod: 7",
            ValueDomain::Scalar).value)];
    require(letters == "ACEGBDFA", "spelling in thirds from A should be A C E G B D F A, not " + letters);
    // NormSin takes (-1,1) into (-pi,pi) before the sine, so an
    // added normSin varies a pitch by a half step up and down.
    requireExamples({
        {"-1 normSin", ValueDomain::Scalar, 0.0, 1.0e-12},
        {"0.5 normSin", ValueDomain::Scalar, 1.0, 1.0e-12},
        {"-0.5 normSin", ValueDomain::Scalar, -1.0, 1.0e-12},
        {"(60 + 0.5 normSin) nn", ValueDomain::NoteNumber, 61.0, 1.0e-12},
        {"(60 + -0.5 normSin) nn", ValueDomain::NoteNumber, 59.0, 1.0e-12},
        // Square roots and their squares, powers and the hypercube.
        {"4 sqrt", ValueDomain::Scalar, 2.0},
        {"9 sqrt", ValueDomain::Scalar, 3.0},
        {"9 sqrt * 9 sqrt", ValueDomain::Scalar, 9.0, 1.0e-12},
        {"3 ** 2", ValueDomain::Scalar, 9.0},
        {"3 * 3", ValueDomain::Scalar, 9.0},
        {"3 ** 4", ValueDomain::Scalar, 81.0},
        // Multiplying powers adds the exponents.
        {"(2 ** 2) * (2 ** 3)", ValueDomain::Scalar, 32.0},
        {"2 ** 5", ValueDomain::Scalar, 32.0},
        // Octaves of 220 hz, twoLog and twoExp, and exp's octave
        // of e.
        {"220 hz * 2", ValueDomain::Hertz, 440.0},
        {"220 hz * 2 * 2", ValueDomain::Hertz, 880.0},
        {"(261.6256 / 8.1758) twoLog", ValueDomain::Scalar, 5.0, 1.0e-4},
        {"(261.6256 / 8.1758) twoLog * 12", ValueDomain::Scalar, 60.0, 1.0e-3},
        {"(60 / 12) twoExp", ValueDomain::Scalar, 32.0},
        {"2 ** (60 / 12)", ValueDomain::Scalar, 32.0},
        {"8.1758d hz * (12 / 12) exp", ValueDomain::Hertz, 8.1758 * std::exp(1.0), 1.0e-9},
        {"8.1758d", ValueDomain::Scalar, 8.1758},
        {"1.14d10", ValueDomain::Scalar, 1.14e10},
        {"2.5d-3", ValueDomain::Scalar, 2.5e-3},
        {"4 d", ValueDomain::NoteNumber, 62.0},
        {"-6db", ValueDomain::Scalar, 0.5011872336272722},
    }, "normSin, powers and exponents");
    require(talas::expr::withPitchDApart("4d + 8.1758d hz") == "4 d + 8.1758 d hz"
            && talas::expr::withPitchDApart("1.14d10 + -6db + 4 d") == "1.14d10 + -6db + 4 d"
            && talas::expr::withPitchDApart("'4d' size") == "'4d' size",
        "an older project's number before d should gain a space, and nothing else");
    // Equal-amplitude panning sums the amplitudes to 1, the
    // square roots and the sine and cosine sum their squares to 1.
    for (const auto pan : {0.0, 0.2, 0.5, 0.9, 1.0})
    {
        Timeline left("1 - !X");
        Timeline right("!X");
        Timeline energyLeft("(1 - !X) sqrt");
        Timeline energyRight("!X sqrt");
        Timeline quarterLeft("(1 - (!X ** 2)) sqrt");
        Timeline squares("(((0.5 * !X) normCos) ** 2) + (((0.5 * !X) normSin) ** 2)");
        Timeline check("(((1 - (!X ** 2)) sqrt) ** 2) + (!X ** 2)");
        const auto label = " at pan " + std::to_string(pan);
        requireNear(left.at(0.0, pan) + right.at(0.0, pan), 1.0, "1 - !Pan plus !Pan" + label);
        require(std::abs(std::pow(energyLeft.at(0.0, pan), 2.0) + std::pow(energyRight.at(0.0, pan), 2.0) - 1.0) < 1.0e-12
                && std::abs(std::pow(quarterLeft.at(0.0, pan), 2.0) + std::pow(right.at(0.0, pan), 2.0) - 1.0) < 1.0e-12
                && std::abs(squares.at(0.0, pan) - 1.0) < 1.0e-12 && std::abs(check.at(0.0, pan) - 1.0) < 1.0e-12,
            "the squares of Left and Right should sum to 1" + label);
    }
    // 8.1758d hz * (2 ** (!KeyNumber / 12)) and its twoExp form are
    // the key's equal-tempered pitch; with exp each 12 keys multiply by e.
    for (const auto key : {0.0, 57.0, 60.0, 69.0})
    {
        Timeline powers("(8.1758d hz * (2 ** (!X / 12))) removeUnits");
        Timeline twoExp("(8.1758d hz * (!X / 12) twoExp) removeUnits");
        Timeline natural("(8.1758d hz * (!X / 12) exp) removeUnits");
        const auto pitch = 440.0 * std::pow(2.0, (key - 69.0) / 12.0);
        require(std::abs(powers.at(0.0, key) / pitch - 1.0) < 1.0e-4 && std::abs(twoExp.at(0.0, key) - powers.at(0.0, key)) < 1.0e-9
                && std::abs(natural.at(0.0, key) - 8.1758 * std::exp(key / 12.0)) < 1.0e-9,
            "2 ** (!KeyNumber / 12) and twoExp should give key " + std::to_string(key)
                + "'s pitch, exp its power of e");
    }
}

// Of: takes the element at its index truncated, which a
// fader or a random number scaled by the array's size reaches in equal
// spans; elements may be EventExpressions, in curly braces when complex; an
// array keeps random pitches in a scale; countTriggersMod: steps through
// one, reversed by a random switch; an array of arrays switches between
// scales; and into: warps a fader through its input@output pairs.
void testArraysIndexScalesAndWarps()
{
    requireExamples({{"4 of: #(12 6 9 3 2)", ValueDomain::Scalar, 2.0}}, "the last element");
    Timeline fader("!X * 5 of: #(12 6 9 3 2)");
    for (const auto& [position, value] : {std::pair{0.0, 12.0}, {0.19, 12.0}, {0.2, 6.0}, {0.39, 6.0}, {0.4, 9.0},
             {0.59, 9.0}, {0.6, 3.0}, {0.79, 3.0}, {0.8, 2.0}, {0.99, 2.0}})
        require(fader.at(0.0, position) == value, "!Fader * 5 of: #(12 6 9 3 2) at "
            + std::to_string(position) + " should be " + std::to_string(value));
    const auto chosen = [](std::string_view source, double x = 0.0, double y = 0.0) {
        const auto values = sampled(source, x, y);
        return std::set<double>(values.begin(), values.end());
    };
    require(chosen("(0.1 s random abs * 5) of: #(12 6 9 3 2)") == std::set<double>{2.0, 3.0, 6.0, 9.0, 12.0},
        "a random index scaled by 5 should reach every element");
    Timeline expressions("(!X * 3) of: #(!Y {!Y + 1} 6)");
    require(expressions.at(0.0, 0.0, 0.5) == 0.5 && expressions.at(0.0, 0.5, 0.5) == 1.5
            && expressions.at(0.0, 0.9, 0.5) == 6.0,
        "an array's elements may be Event Values and expressions in braces");
    const auto drawn = sampled("(!X * 3) of: #({0.1 s normal abs} {0.1 s randExp} {!Y + 7})", 0.0, 0.5);
    require(std::ranges::all_of(drawn, [](double value) { return value >= 0.0 && value < 1.0; })
            && sampled("(!X * 3) of: #({0.1 s normal abs} {0.1 s randExp} {!Y + 7})", 0.9, 0.5).back() == 7.5,
        "complex elements in curly braces should each be their own expression");
    // A random index into the intervals of a scale keeps the pitches
    // in the scale.
    require(chosen("(!Y nn + ((1 bpm: 600) nextRandom abs * 7 of: #(0 2 3 5 7 8 11)) nn) removeUnits", 0.0, 60.0)
            == std::set<double>{60.0, 62.0, 63.0, 65.0, 67.0, 68.0, 71.0},
        "random pitches should be restricted to the scale's intervals above the key");
    // While the key is down, each beat reads the next interval, and
    // reverse: reads them backwards.
    const auto beats = [](std::string_view source, double down, double reverse) {
        Timeline timeline(source);
        std::vector<double> values;
        for (int step = 0; step < 13 * 20; ++step)
        {
            const auto value = timeline.at(0.005 * step, down, reverse);
            if (step % 20 == 5)
                values.push_back(value);
        }
        return values;
    };
    const auto listed = [](const std::vector<double>& values) {
        std::string text;
        for (const auto value : values)
            text += " " + std::to_string(static_cast<int>(value));
        return text;
    };
    const auto forwards = beats("((!X bpm: 600) countTriggersMod: 6) of: #(0 2 4 7 11 12)", 1.0, 0.0);
    require(forwards == std::vector<double>{0, 2, 4, 7, 11, 12, 0, 2, 4, 7, 11, 12, 0},
        "countTriggersMod: 6 should step through the six intervals on each beat, not" + listed(forwards));
    const auto backwards = beats("((!X bpm: 600) countTriggersMod: 6 reverse: !Y) of: #(0 2 4 7 11 12)", 1.0, 1.0);
    require(backwards == std::vector<double>{12, 11, 7, 4, 2, 0, 12, 11, 7, 4, 2, 0, 12},
        "countTriggersMod:reverse: should step backwards through the intervals, not" + listed(backwards));
    const auto stepped = sampled("(!X bpm: 600) countTriggersMod: 6 reverse: ((600 / 6) bpm s random gt: 0)", 1.0);
    std::set<double> moves;
    for (std::size_t index = 30; index < stepped.size(); index += 20)
        moves.insert(std::fmod(stepped[index] - stepped[index - 20] + 6.0, 6.0));
    require(moves == std::set<double>{1.0, 5.0},
        "a random switch every 6th beat should read the sequence forwards and in reverse");
    // A controller indexes an array of pitches, and an array of
    // two such expressions switches between scales.
    Timeline pitches("(!Y nn + ((!X of: #(0 3 5 7 10)) nn)) removeUnits");
    require(pitches.at(0.0, 3.2, 60.0) == 67.0, "!KeyPitch + (!Step of: #(0 3 5 7 10)) nn");
    Timeline modes("(!Y * 2) of: #({(!X * 5) of: #(0 2 4 5 7)} {(!X * 5) of: #(0 3 5 7 10)})");
    require(modes.at(0.0, 0.5, 0.0) == 4.0 && modes.at(0.0, 0.5, 0.9) == 5.0 && modes.at(0.0, 0.9, 0.9) == 10.0,
        "an array of arrays should let !Mode switch between scales");
    // Into: inverts and warps the fader through its pairs,
    // interpolating between them.
    Timeline warped("!X into: #({0@1} {0.25@0.5} {1@0})");
    for (const auto& [position, value] : {std::pair{0.0, 1.0}, {0.125, 0.75}, {0.25, 0.5}, {0.625, 0.25}, {1.0, 0.0}})
        requireNear(warped.at(0.0, position), value, "!Fader into: #({0@1} {0.25@0.5} {1@0}) at "
            + std::to_string(position));
}

// Comparisons are 1 when true and 0 when
// false, so they gate like !KeyDown; multiplying is AND, adding OR and le: 0
// NOT; two frequencies cannot be multiplied; neg:zero:pos:initially: starts
// at its initial value; true:false: chooses one of its arguments; and nested
// true:false: messages make a case statement.
void testLogicAndConditionalCases()
{
    const auto truth = [](std::string_view source, double key, double other = 0.0) {
        Timeline timeline(source);
        return timeline.at(0.0, key, other);
    };
    require(truth("!X eq: 60", 60.0) == 1.0 && truth("!X eq: 60", 61.0) == 0.0,
        "!KeyNumber eq: 60 should be true for middle C only");
    require(truth("!X eq: 69", 69.0) == 1.0 && truth("!X ne: 69", 69.0) == 0.0 && truth("!X ne: 69", 70.0) == 1.0
            && truth("!X lt: 60", 59.0) == 1.0 && truth("!X lt: 60", 60.0) == 0.0,
        "4 a, any key but 4 a, and any key below middle C");
    for (const auto& [a, b] : {std::pair{0.0, 0.0}, {0.0, 1.0}, {1.0, 0.0}, {1.0, 1.0}})
    {
        require(truth("!X * !Y", a, b) == (a == 1.0 && b == 1.0 ? 1.0 : 0.0)
                && (truth("!X + !Y", a, b) > 0.0) == (a == 1.0 || b == 1.0) && truth("!X le: 0", a) == 1.0 - a,
            "AND is *, OR is + and NOT is le: 0");
    }
    for (const auto key : {47.0, 48.0, 59.0, 60.0, 71.0, 72.0, 73.0})
    {
        const auto below = key < 60.0 && key != 48.0;
        require(truth("(!X lt: 60) * (!X ne: 48)", key) == (below ? 1.0 : 0.0)
                && (truth("(!X lt: 60) * (!X ne: 48) + (!X eq: 72)", key) > 0.0) == (below || key == 72.0),
            "below 4 c and not 3 c, or 5 c, at key " + std::to_string(key));
    }
    requireExamples({{"440 hz * 2", ValueDomain::Hertz, 880.0}}, "a frequency times a number");
    const auto product = talas::expr::parse("440 hz * (2 hz)", ExpressionLanguageVersion::ValV1);
    require(product.ok() && !talas::expr::evaluate(*product.expression, {}, {ValueDomain::Hertz}).ok(),
        "multiplying two frequencies should be refused");
    // The loop's rate starts at 1 and then follows the direction of
    // LoopEnd - LoopStart, negated when the end is below the start.
    Timeline rate("(!Y - !X) neg: 2 negated zero: 0 pos: 2 initially: 1");
    require(rate.at(0.0, 0.2, 0.7) == 1.0 && rate.at(0.1, 0.2, 0.8) == 2.0 && rate.at(0.2, 0.9, 0.8) == -2.0
            && rate.at(0.3, 0.8, 0.8) == 0.0,
        "neg:zero:pos:initially: should start at 1, then go backwards when LoopEnd is below LoopStart");
    require(truth("!X true: (0) false: (1)", 0.0) == 1.0 && truth("!X true: (0) false: (1)", 1.0) == 0.0,
        "!Gate true: (0) false: (1) should be 1 when !Gate is 0");
    require(truth("((!Y lt: 0.5) true: (72 nn) false: (!X nn)) removeUnits", 60.0, 0.3) == 72.0
            && truth("((!Y lt: 0.5) true: (72 nn) false: (!X nn)) removeUnits", 60.0, 0.7) == 60.0,
        "soft keys should play 5 c and loud ones the keyboard's pitch");
    const auto frequency = [&](double key, double velocity) {
        return truth("(!Y lt: 0.5) true: 0 false: ((!X lt: 60) true: 440 false: ((!X gt: 72) true: 440 / 2 "
                     "false: (!X nn hz removeUnits)))", key, velocity);
    };
    require(frequency(80.0, 0.3) == 0.0 && frequency(50.0, 0.7) == 440.0 && frequency(80.0, 0.7) == 220.0
            && std::abs(frequency(60.0, 0.7) - 261.6255653005986) < 1.0e-9,
        "the case statement should test the velocity, then below middle C, then above 72, "
        "then take the keyboard's pitch");
    // With the velocity too small the rest is not even evaluated,
    // so a ramp there waits for it.
    Timeline rest("(!Y lt: 0.5) true: 0 false: (1 ramp: 1 s)");
    require(rest.at(0.0, 0.0, 0.7) == 0.0 && rest.at(0.25, 0.0, 0.7) == 0.25 && rest.at(0.5, 0.0, 0.3) == 0.0
            && rest.at(0.75, 0.0, 0.7) == 0.5,
        "the rest of the expression should not be evaluated while the velocity is too small");
}

// An EventExpression is a number, an Event Value, or a
// unary, binary or keyword message whose receiver and arguments are
// EventExpressions, so expressions nest; an Event Value's name starts with
// a letter and holds only letters, digits and underscores; and units say
// what a number is, convert into one another and come off with removeUnits.
void testEventExpressionsAndUnits()
{
    requireExamples({{"0.333", ValueDomain::Scalar, 0.333}}, "a number is an Event");
    Timeline unary("!X cos");
    Timeline negated("!X negated");
    Timeline product("!X * !Y");
    Timeline sum("!X + !Y");
    Timeline nested("!X sin abs * !Y");
    Timeline balance("((!X ** 2) + ((1 - !X) ** 2)) sqrt");
    requireNear(unary.at(0.0, 0.0), 1.0, "!Fader cos");
    requireNear(negated.at(0.0, 0.25), -0.25, "!Fader negated");
    requireNear(product.at(0.0, 0.5, 0.4), 0.2, "!Amp * !Scale");
    requireNear(sum.at(0.0, 0.5, 0.25), 0.75, "!Fader1 + !Fader2");
    requireNear(nested.at(0.0, -std::numbers::pi / 2.0, 120.0), 120.0, "!Fader sin abs * !BPM");
    requireNear(balance.at(0.0, 0.5), std::sqrt(0.5), "((!Left ** 2) + ((1 - !Left) ** 2)) sqrt");
    // The nested expression as a keyword message's argument: 120
    // beats a minute, on for half of each beat while the key is down.
    Timeline keyword("!X bpm: (!Y sin abs * 120) dutyCycle: 0.5");
    std::vector<double> beat;
    for (int step = 0; step <= 101; ++step)
        beat.push_back(keyword.at(step * 0.005, 1.0, std::numbers::pi / 2.0));
    require(beat[10] == 1.0 && beat[60] == 0.0 && beat[99] == 0.0 && beat[100] == -1.0 && beat[101] == 1.0,
        "!KeyDown bpm: (!Fader sin abs * !BPM) dutyCycle: 0.5 should beat twice a second, on for half");
    // Legal and illegal Event Value names.
    for (const auto* legal : {"!K9Corps", "!Hello_Boys", "!LongNameWithUppercaseAtBeginningsOfWords"})
    {
        const auto parsed = talas::expr::parse(legal, ExpressionLanguageVersion::ValV1);
        require(parsed.ok() && parsed.expression->eventReferences() == std::vector<std::string>{std::string{legal}.substr(1)},
            std::string{""} + legal + " should be an Event Value");
    }
    for (const auto* illegal : {"!10Hz", "!Yes&No"})
    {
        const auto parsed = talas::expr::parse(illegal, ExpressionLanguageVersion::ValV1);
        require(!parsed.ok() || std::ranges::find(parsed.expression->eventReferences(), std::string{illegal}.substr(1))
                == parsed.expression->eventReferences().end(),
            std::string{""} + illegal + " should not be an Event Value's name");
    }
    // A Frequency field takes a plain number as hertz, so a key
    // number needs nn; a duration there is its rate, a frequency in a
    // duration field its period, so one cycle of the key's pitch; samples
    // last as long as the sample rate says; bpm is a tempo's frequency;
    // and dB is a level, 0 dB the largest a field limited to 1 takes.
    requireExamples({
        {"69", ValueDomain::Hertz, 69.0},
        {"69 nn", ValueDomain::Hertz, 440.0},
        {"69 nn hz * 3", ValueDomain::Hertz, 1320.0, 1.0e-9},
        {"1 s", ValueDomain::Hertz, 1.0},
        {"69 nn hz", ValueDomain::Seconds, 1.0 / 440.0, 1.0e-12},
        {"1024 samp", ValueDomain::Seconds, 1024.0 / 48000.0, 1.0e-12},
        {"120 bpm", ValueDomain::Hertz, 2.0},
        {"0.5 * 1000 hz", ValueDomain::Hertz, 500.0},
        {"-6 dB", ValueDomain::Scalar, 0.5011872336272722},
        {"0 dB", ValueDomain::Scalar, 1.0},
        {"6 dB", ValueDomain::Scalar, 1.9952623149688795, 1.0e-12},
        {"4 c", ValueDomain::NoteNumber, 60.0},
        {"440 hz nn", ValueDomain::NoteNumber, 69.0, 1.0e-9},
        {"1 s samp", ValueDomain::Samples, 48000.0},
        {"(60 nn) removeUnits / 127", ValueDomain::Scalar, 60.0 / 127.0, 1.0e-12},
        {"60 nn / 127 nn", ValueDomain::Scalar, 60.0 / 127.0, 1.0e-12},
        {"60 / 127", ValueDomain::Scalar, 60.0 / 127.0, 1.0e-12},
        // Messages that say where a number comes from.
        {"SignalProcessor sampleRate * 0.25", ValueDomain::Scalar, 12000.0},
        {"440 hz s", ValueDomain::Seconds, 0.0022727272727273, 1.0e-15},
        {"4 c hz * 4", ValueDomain::Hertz, 1046.5024, 1.0e-3},
    }, "Event Values and units in fields");
    EvaluationContext faster;
    faster.sampleRate = 96000.0;
    require(std::abs(evaluate("1024 samp", ValueDomain::Seconds, faster).value - 1024.0 / 96000.0) < 1.0e-12,
        "1024 samp should last half as long at twice the sample rate");
}

// The Expressions Library's fader arithmetic: a second
// formant a fixed interval above the keyboard's pitch, one fader inverted
// with a maximum of 10, and a nonlinear fader warped through into:.
void testExpressionsLibraryFaderArithmetic()
{
    Timeline interval("(!X nn + !Y nn) removeUnits");
    require(interval.at(0.0, 60.0, 7.0) == 67.0 && interval.at(0.0, 60.0, -24.0) == 36.0
            && interval.at(0.0, 72.0, 7.0) == 79.0,
        "!KeyPitch + !Interval nn should stay the interval's half steps above the key");
    Timeline inverted("1 / (!X max: (1/10))");
    for (const auto& [fader, value] : {std::pair{1.0, 1.0}, {0.875, 8.0 / 7.0}, {0.5, 2.0}, {0.1, 10.0}, {0.05, 10.0},
             {0.0, 10.0}})
        requireNear(inverted.at(0.0, fader), value, "1 / (!BW1 max: (1/10)) at " + std::to_string(fader));
    Timeline warped("!X into: #({0@0} {0.01@0.03} {0.1@ 0.06} {0.5@0.13} {1@0.2})");
    for (const auto& [fader, value] : {std::pair{0.0, 0.0}, {0.01, 0.03}, {0.1, 0.06}, {0.3, 0.095}, {0.5, 0.13},
             {1.0, 0.2}})
        requireNear(warped.at(0.0, fader), value, "the nonlinear Density fader at " + std::to_string(fader));
}

// An EventVariable keeps a value between evaluations,
// read as its initial value until <~ (keeping the value) or <+ (leaving
// nothing) assigns it; expressions in a sequence leave the last value one of
// them leaves; nil is nothing; a temporary's time function is shared by its
// reads and runs on its own; updateInternalState only updates it; and a
// program read every millisecond is evaluated once a millisecond.
void testEventVariables()
{
    Timeline counter("| count | count := EventVariable new initialValue: -1. "
                     "!X true: (count <~ (count + 0.1)) false: (count)");
    requireNear(counter.at(0.0, 0.0), -1.0, "count before Sw01 is clicked");
    requireNear(counter.at(0.1, 1.0), -0.9, "a click should increment count");
    requireNear(counter.at(0.2, 0.0), -0.9, "unchecked, the value should be count");
    requireNear(counter.at(0.3, 1.0), -0.8, "a second click should increment count again");
    const std::string twoCounts = "| count downCount | count := EventVariable new initialValue: -1. "
                                  "downCount := EventVariable new initialValue: 1. ";
    Timeline both(twoCounts + "(!X true: ( (downCount <+ (downCount - 0.1)), (count <~ (count + 0.1))) "
                              "false: (downCount))");
    Timeline popped(twoCounts + "(!X true: ( (downCount <+ (downCount - 0.1)), (count <+ (count + 0.1)), count) "
                                "false: (downCount))");
    for (auto* timeline : {&both, &popped})
    {
        requireNear(timeline->at(0.0, 0.0), 1.0, "downCount before the first click");
        requireNear(timeline->at(0.1, 1.0), -0.9, "a click should give the incremented count");
        requireNear(timeline->at(0.2, 0.0), 0.9, "unchecked, the decremented downCount");
        requireNear(timeline->at(0.3, 1.0), -0.8, "a second click's count");
        requireNear(timeline->at(0.4, 0.0), 0.8, "and downCount");
    }
    // Counted while the threshold is exceeded, count grows on every
    // evaluation; remembering the trigger's state counts its rises alone.
    const std::vector<double> levels{0.0, 0.5, 0.7, 0.9, 0.0, 0.0, 0.3, 0.3, -0.2};
    const auto counts = [&](std::string_view program) {
        Timeline timeline(program);
        std::vector<double> values;
        for (std::size_t tick = 0; tick < levels.size(); ++tick)
            values.push_back(timeline.at(0.001 * static_cast<double>(tick), levels[tick]));
        return values;
    };
    require(counts("| count | count := EventVariable new initialValue: 0. "
                   "(!X asLogicValue true: (count <~ (count + 1)) false: nil), count")
            == std::vector<double>{0, 1, 2, 3, 3, 3, 4, 5, 5},
        "count should grow on every evaluation the threshold is exceeded");
    require(counts("| count state thresh | count := EventVariable new initialValue: 0. "
                   "state := EventVariable new initialValue: 0. thresh := !X asLogicValue. "
                   "((thresh eq: state) true: nil false: ((state <+ thresh), "
                   "(thresh true: (count <~ (count + 1)) false: nil))), count")
            == std::vector<double>{0, 1, 1, 1, 1, 1, 2, 2, 2},
        "count should grow only when the threshold's state changes to 1");
    // Tracking while the key is down and holding while it is up,
    // the time function running on its own throughout.
    for (const auto* program : {"| prevVal timeFunc | prevVal := EventVariable new. "
                                "timeFunc := (!Y repeatingRamp: 5 s) normSin. "
                                "(timeFunc), (!X true: (prevVal <~ timeFunc) false: prevVal)",
             "| prevVal timeFunc | prevVal := EventVariable new. timeFunc := (!Y repeatingRamp: 5 s) normSin. "
             "(timeFunc updateInternalState), (!X true: (prevVal <~ timeFunc) false: prevVal)"})
    {
        Timeline held(program);
        double released = 0.0;
        for (int step = 0; step <= 500; ++step)
        {
            const auto time = 0.005 * step;
            const auto down = time < 1.0 || time >= 2.0;
            const auto value = held.at(time, down ? 1.0 : 0.0, 1.0);
            if (step == 199)
                released = value;
            const auto expected = down ? std::sin(std::numbers::pi * time / 5.0) : released;
            require(std::abs(value - expected) < 2.0e-3, std::string{""} + program + " at "
                + std::to_string(time) + " should be " + std::to_string(expected) + ", not " + std::to_string(value));
        }
    }
    // A time function in a branch not taken stops, carrying on
    // from where it left off when the branch is next taken; in a project
    // before format 119 it ran on.
    {
        constexpr std::string_view source = "| prevVal timeFunc | prevVal := EventVariable new. "
                                            "timeFunc := (!Y repeatingRamp: 5 s) normSin. "
                                            "(!X true: (prevVal <~ timeFunc) false: prevVal)";
        const auto marked = talas::expr::withTimeRunningOnInSkippedBranches(source, {});
        require(marked.has_value(), "a program with a conditional and state should be marked to run on");
        require(talas::expr::withTimeRunningOnInSkippedBranches("!X true: (!Y ramp: 1 s) false: 0", {}).has_value()
                && !talas::expr::withTimeRunningOnInSkippedBranches(*marked, {}).has_value()
                && !talas::expr::withTimeRunningOnInSkippedBranches("!Y repeatingRamp: 5 s", {}).has_value()
                && !talas::expr::withTimeRunningOnInSkippedBranches("!X true: 1 false: 0", {}).has_value(),
            "only a source with a conditional and state, not yet marked, should be marked");
        Timeline frozen(source);
        Timeline runsOn(*marked);
        for (int step = 0; step <= 500; ++step)
        {
            const auto time = 0.005 * step;
            const auto down = time < 1.0 || time >= 2.0;
            const auto value = frozen.at(time, down ? 1.0 : 0.0, 1.0);
            // Last evaluated at 0.995 s, it misses the second up to 1.995 s.
            const auto own = time < 1.0 ? time : time < 2.0 ? 0.995 : time - 1.0;
            const auto expected = std::sin(std::numbers::pi * own / 5.0);
            require(std::abs(value - expected) < 1e-9, "the frozen time function at " + std::to_string(time)
                + " should be " + std::to_string(expected) + ", not " + std::to_string(value));
            const auto ran = runsOn.at(time, down ? 1.0 : 0.0, 1.0);
            const auto running = std::sin(std::numbers::pi * (time < 1.0 ? time : time < 2.0 ? 0.995 : time) / 5.0);
            require(std::abs(ran - running) < 1e-9, "the marked time function at " + std::to_string(time)
                + " should be " + std::to_string(running) + ", not " + std::to_string(ran));
        }
    }
    // Read every millisecond, the program runs once a millisecond,
    // however often it is asked.
    Timeline every("| count | count := EventVariable new initialValue: 0. count readEveryMillisecond. "
                   "count <~ (count + 1)");
    require(every.at(0.0) == 1.0 && every.at(0.0) == 1.0 && every.at(0.001) == 2.0 && every.at(0.001) == 2.0
            && every.at(0.002) == 3.0,
        "a program read every millisecond should count each millisecond once");
    // A temporary read twice, holding another read twice, shares its time
    // functions throughout.
    Timeline twice("| r ramp x | r := EventVariable new. ramp := !X repeatingRamp: 1 s. x := ramp + ramp. "
                   "(x - x), (r <~ (x + ramp))");
    for (int step = 1; step <= 10; ++step)
        requireNear(twice.at(0.05 * step, 1.0), 3.0 * std::fmod(0.05 * (step - 1), 1.0),
            "shared temporaries should share their ramp, at " + std::to_string(0.05 * step));
    requireExamples({{"nil", ValueDomain::Scalar, 0.0}, {"(3), (4)", ValueDomain::Scalar, 4.0}}, "nil and comma-separated statements");
    require(!talas::expr::parse("!X <~ 1", ExpressionLanguageVersion::ValV1).ok()
            && !talas::expr::parse("| a | a := 3. a <~ 1", ExpressionLanguageVersion::ValV1).ok(),
        "only an EventVariable can be assigned");
}

// Functions of time: a key restarts its
// metronome; a ramp starts at 0 and rises to 1; a full ramp four times as
// long takes four times as long from -1 to 1; repeating ramps and
// triangles repeat over 12 * !Duration s; a normal jitter keeps near its
// fader; and the jitter at random intervals of time centres on 1.
void testGeneratorsFromTheTutorial()
{
    // Pressing a key again restarts !KeyDown bpm: 60.
    Timeline metronome("!X bpm: 60");
    std::vector<double> beats;
    for (int step = 0; step <= 700; ++step)
    {
        const auto time = 0.005 * step;
        const auto down = time < 2.3 || time >= 2.5;
        beats.push_back(metronome.at(time, down ? 1.0 : 0.0));
    }
    require(beats[410] == 1.0 && beats[510] == 1.0 && beats[610] == 0.0 && beats[690] == 0.0,
        "a key pressed again should restart the metronome");
    // An octave repeats twice as fast.
    requireExamples({{"57 nn hz bpm removeUnits / 100", ValueDomain::Scalar, 220.0 * 60.0 / 100.0, 1.0e-9},
                        {"69 nn hz bpm removeUnits / 100", ValueDomain::Scalar, 440.0 * 60.0 / 100.0, 1.0e-9}},
        "an octave's beats");
    // Ramp from 0 to 1 over its second, from the key.
    Timeline ramp("!X ramp");
    ramp.at(0.0, 0.0);
    requireNear(ramp.at(0.1, 1.0), 0.0, "!KeyDown ramp should start at 0");
    requireNear(ramp.at(0.6, 1.0), 0.5, "!KeyDown ramp halfway");
    requireNear(ramp.at(1.1, 1.0), 1.0, "!KeyDown ramp should reach 1");
    // FullRamp: 6.8226 s * 4 takes four times as long from -1 to 1.
    Timeline once("!X fullRamp: 6.8226 s");
    Timeline longer("!X fullRamp: 6.8226 s * 4");
    once.at(0.0, 1.0);
    longer.at(0.0, 1.0);
    // The clock ticks each millisecond, so these come within a millisecond.
    require(std::abs(once.at(6.8226 / 2.0, 1.0)) < 1.0e-3 && std::abs(longer.at(6.8226 / 2.0, 1.0) + 0.75) < 1.0e-3
            && std::abs(longer.at(6.8226 * 2.0, 1.0)) < 1.0e-3,
        "a full ramp four times as long should take four times as long from -1 to 1");
    // 12 * !Duration s, here 6 s, repeats the full ramp and the
    // triangle.
    Timeline full("1 repeatingFullRamp: 12 * !Y s");
    Timeline triangle("1 repeatingTriangle: 12 * !Y s");
    for (const auto& [time, swept, turned] : {std::tuple{0.0, -1.0, 0.0}, {1.5, -0.5, 0.5}, {3.0, 0.0, 1.0},
             {4.5, 0.5, 0.5}, {7.5, -0.5, 0.5}, {9.0, 0.0, 1.0}})
    {
        requireNear(full.at(time, 0.0, 0.5), swept, "1 repeatingFullRamp: 12 * !Duration s at "
            + std::to_string(time));
        requireNear(triangle.at(time, 0.0, 0.5), turned, "1 repeatingTriangle: 12 * !Duration s at "
            + std::to_string(time));
    }
    // Jitter around the TimeIndex fader, no more than !Jitter.
    const auto jittered = sampled("!X + (!Y * (0.1 s normal smooth: 0.1 s))", 0.5, 0.1);
    const auto [low, high] = std::ranges::minmax(jittered);
    require(low >= 0.4 && high <= 0.6 && low < 0.49 && high > 0.51,
        "the jitter should move around the fader by at most !Jitter");
    // A normal value at random intervals, centred on 1.
    const auto centred = sampled("(((1 s randExp) * 0.1 s) normal * !Y + 1)", 0.0, 0.2);
    const auto mean = std::accumulate(centred.begin(), centred.end(), 0.0) / static_cast<double>(centred.size());
    require(std::ranges::all_of(centred, [](double value) { return value >= 0.8 && value <= 1.2; })
            && std::ranges::any_of(centred, [](double value) { return value > 1.01; })
            && std::ranges::any_of(centred, [](double value) { return value < 0.99; }) && std::abs(mean - 1.0) < 0.05,
        "the jitter at random intervals should centre on 1");
    // (1 s randExp) * !TimeConstant s picks a random interval once a
    // second, scaled by the fader; normal draws once per such interval.
    const auto intervals = sampled("((1 s randExp) * !X s) removeUnits", 1.0);
    const auto doubled = sampled("((1 s randExp) * !X s) removeUnits", 2.0);
    auto steadyEachSecond = true;
    for (std::size_t second = 0; second < 20; ++second)
        for (std::size_t step = 1; step < 199; ++step)
            steadyEachSecond = steadyEachSecond && intervals[second * 200 + step] == intervals[second * 200 + 1];
    require(steadyEachSecond && intervals[201] != intervals[1]
            && std::ranges::equal(doubled, intervals, [](double twice, double once) { return std::abs(twice - 2.0 * once) < 1.0e-12; }),
        "the interval should be drawn once a second and scaled by !TimeConstant");
    std::size_t draws = 0;
    for (std::size_t step = 1; step < centred.size(); ++step)
        draws += centred[step] != centred[step - 1] ? 1U : 0U;
    require(draws > 10 && draws < centred.size() / 4,
        "normal should draw a new number once per random interval, not each tick (" + std::to_string(draws) + ")");
}

// Expressions worked out: a random interval adds one of 0, 1, 2, 3, 5, 8
// or 13 half steps to the pitch, its index a metronome's random number from
// -1 to 1 times !Jitter * 7, the metronome on for as long as its first
// number is 1 and beating at 0 to 1000 a minute by !Rate; a TimeIndex goes
// from -1 to 1 in up to 100 seconds by !Rate; 60 over a recording's
// duration beats once a recording, slower than once a second for a long
// one and faster for a short one; a 0 to 1 controller times 127 nn spans
// the MIDI notes; !Frequency * 1.5 + 0.5 scales from an octave down to an
// octave up; delays of 800 usec times (0.5 * !Pan) normCos and normSin
// differ by 800 usec at either side and not at all in the middle; and a
// frequency and its period are each other's inverse.
void testWorkedTutorialExpressions()
{
    const auto near = [](double value, double expected, double tolerance, std::string_view what) {
        require(std::abs(value - expected) <= tolerance,
            std::string{what} + " should be " + std::to_string(expected) + ", not " + std::to_string(value));
    };
    Timeline interval("60 nn + ((((1 bpm: (!X * 1000)) nextRandom * !Y * 7)) of: #(0 1 2 3 5 8 13)) nn");
    std::set<long> added;
    for (int step = 0; step < 20000; ++step)
    {
        const auto pitch = interval.at(step / 1000.0, 1.0, 1.0);
        const auto halfSteps = std::lround(pitch - 60.0);
        require(std::abs(pitch - 60.0 - static_cast<double>(halfSteps)) < 1.0e-9,
            "the pitch should move by whole half steps, not " + std::to_string(pitch));
        added.insert(halfSteps);
    }
    require(added.size() > 3 && std::ranges::all_of(added, [](long n) { return n == 0 || n == 1 || n == 2 || n == 3 || n == 5 || n == 8 || n == 13; }),
        "the pitch should move by 0, 1, 2, 3, 5, 8 or 13 half steps");
    Timeline index("(1 bpm: (!X * 1000)) nextRandom * !Y * 7");
    auto lowest = 0.0;
    auto highest = 0.0;
    for (int step = 0; step < 20000; ++step)
    {
        const auto value = index.at(step / 1000.0, 1.0, 0.5);
        lowest = std::min(lowest, value);
        highest = std::max(highest, value);
    }
    require(lowest >= -3.5 && lowest < -1.0 && highest <= 3.5 && highest > 1.0,
        "random numbers from -1 to 1 times !Jitter * 7 should spread over -3.5 to 3.5 at a !Jitter of 0.5");
    const auto beats = [](std::string_view source, double seconds, double x) {
        Timeline clock(source);
        auto rises = 0;
        auto previous = 0.0;
        for (int step = 0; step < static_cast<int>(seconds * 1000.0); ++step)
        {
            const auto value = clock.at(step / 1000.0, x);
            rises += previous <= 0.0 && value > 0.0;
            previous = value;
        }
        return rises;
    };
    require(beats("1 bpm: (!X * 1000)", 2.99, 1.0) == 50 && beats("1 bpm: (!X * 1000)", 2.99, 0.5) == 25
            && beats("1 bpm: (!X * 1000)", 2.99, 0.0) == 0,
        "1 bpm: (!Rate * 1000) should beat 1000 times a minute at the top of !Rate, 500 halfway and not at all at 0");
    require(beats("0 bpm: (!X * 1000)", 2.99, 1.0) == 0, "a metronome whose first number is 0 should stay off");

    Timeline slowest("1 repeatingFullRamp: ((!X * 100) s)");
    near(slowest.at(0.0, 1.0), -1.0, 0.01, "the TimeIndex at the start");
    near(slowest.at(50.0, 1.0), 0.0, 0.01, "halfway through 100 s at a !Rate of 1");
    near(slowest.at(99.9, 1.0), 1.0, 0.01, "at the end of 100 s at a !Rate of 1");
    Timeline tutorial("1 repeatingFullRamp: ((!X * 100) s)");
    near(tutorial.at(0.0, 0.6), -1.0, 0.01, "the TimeIndex at the start");
    near(tutorial.at(30.0, 0.6), 0.0, 0.01, "halfway through 60 s at a !Rate of 0.6");
    Timeline fastest("1 repeatingFullRamp: ((!X * 100) s)");
    for (int step = 0; step < 100; ++step)
    {
        const auto value = fastest.at(step / 1000.0, 0.0);
        require(value >= -1.0 && value <= 1.0, "a !Rate of 0 should keep the TimeIndex within -1 to 1");
    }

    require(beats("1 bpm: (60.0/2.5)", 9.99, 0.0) == 4, "60 over a 2.5 s recording should beat once every 2.5 s");
    require(beats("1 bpm: (60.0/0.5)", 1.99, 0.0) == 4, "60 over a 0.5 s recording should beat twice a second");
    require(beats("1 bpm: 60", 3.99, 0.0) == 4, "a setting of 60 should beat once a second");

    Timeline notes("!X * 127 nn");
    near(notes.at(0.0, 0.0), 0.0, 1.0e-9, "a controller at 0 times 127 nn");
    near(notes.at(0.0, 0.5), 63.5, 1.0e-9, "a controller at 0.5 times 127 nn");
    near(notes.at(0.0, 1.0), 127.0, 1.0e-9, "a controller at 1 times 127 nn");

    Timeline scale("!X * 1.5 + 0.5");
    near(scale.at(0.0, 0.0), 0.5, 1.0e-12, "an octave down at the bottom of !Frequency");
    near(scale.at(0.0, 1.0), 2.0, 1.0e-12, "an octave up at the top of !Frequency");

    EvaluationContext context;
    context.sampleRate = 48000.0;
    for (const auto& [pan, right, left] : std::array<std::tuple<std::string_view, double, double>, 3>{{
             {"0", 800.0e-6, 0.0}, {"0.5", 565.685e-6, 565.685e-6}, {"1", 0.0, 800.0e-6}}})
    {
        const auto rightDelay = evaluate("(0.5 * " + std::string{pan} + ") normCos * 800 usec", ValueDomain::Seconds, context).value;
        const auto leftDelay = evaluate("(0.5 * " + std::string{pan} + ") normSin * 800 usec", ValueDomain::Seconds, context).value;
        near(rightDelay, right, 1.0e-9, "the right delay at a !Pan of " + std::string{pan});
        near(leftDelay, left, 1.0e-9, "the left delay at a !Pan of " + std::string{pan});
    }

    near(evaluate("440 hz inverse", ValueDomain::Seconds, context).value, 1.0 / 440.0, 1.0e-12,
        "a frequency's inverse is its period in seconds");
    // A negative number is no position in an array, each one
    // standing for the first; abs makes a random index one.
    requireExamples({{"-1 of: #(53 50 48)", ValueDomain::Scalar, 53.0}, {"-0.6 of: #(53 50 48)", ValueDomain::Scalar, 53.0},
                        {"(-0.6 abs * 3) of: #(53 50 48)", ValueDomain::Scalar, 50.0}},
        "negative array positions");
    near(evaluate("0.01 s inverse", ValueDomain::Hertz, context).value, 100.0, 1.0e-9,
        "a period's inverse is its frequency in cycles a second");
}

// Units in the Sound Editor: a plain number in a
// Frequency field is hertz, nn a note number, 5 c a pitch, which hz and nn
// turn into one another; samples there are the frequency of that period;
// dB a level; and a loop's duration in bpm, times its beats, its tempo.
void testUnitsInTheSoundEditor()
{
    requireExamples({
        {"523.2512", ValueDomain::Hertz, 523.2512},
        {"0.5", ValueDomain::Scalar, 0.5},
        {"523.2512 hz", ValueDomain::Hertz, 523.2512},
        {"72.25 nn", ValueDomain::Hertz, 440.0 * std::pow(2.0, (72.25 - 69.0) / 12.0), 1.0e-9},
        {"5 c", ValueDomain::NoteNumber, 72.0},
        {"5 c hz", ValueDomain::Hertz, 523.2511306011972, 1.0e-9},
        {"5 c hz nn", ValueDomain::NoteNumber, 72.0, 1.0e-9},
        {"100 samp", ValueDomain::Hertz, 480.0, 1.0e-9},
        {"1 s", ValueDomain::Hertz, 1.0},
        {"-6 dB", ValueDomain::Scalar, 0.5011872336272722},
        {"0 dB", ValueDomain::Scalar, 1.0},
        {"(3.75 s bpm) removeUnits", ValueDomain::Scalar, 16.0, 1.0e-9},
        {"(3.75 s bpm * 8) removeUnits", ValueDomain::Scalar, 128.0, 1.0e-9},
        // A frequency twice another is an octave above it, from any
        // pitch.
        {"((4 a hz * 2) nn - 4 a) removeUnits", ValueDomain::Scalar, 12.0, 1.0e-9},
        {"((2 e hz * 2) nn - 2 e) removeUnits", ValueDomain::Scalar, 12.0, 1.0e-9},
        {"(4 a + 12 nn) hz", ValueDomain::Hertz, 880.0, 1.0e-9},
    }, "units in the Sound Editor");
}

// The logistic map Tool's Val: (1 bpm: !BPM)
// nextChaotic: !k seed: 0.9005 reset: !Reset steps x = r x (1 - x), r = 3 +
// k; at r 3.7397 its values settle into a 5-cycle, at 3.7021 a 7-cycle, at
// 3.8493 nearly a 3-cycle, and at 4.0 and 3.6262 into no short cycle.
void testLogisticMapCycles()
{
    // The stream after 20,000 triggers, a random x injected every 1,000
    // when `injected`.
    const auto settled = [](double r, bool injected = false) {
        Timeline chaotic("!X nextChaotic: " + std::to_string(r - 3.0) + " seed: 0.9005 reset: !Y");
        std::vector<double> values;
        for (int step = 0; step < 20200; ++step)
        {
            const auto inject = injected && step < 19000 && step % 1000 == 500 ? 1.0 : 0.0;
            const auto value = chaotic.at(0.002 * step, 1.0, inject);
            chaotic.at(0.002 * step + 0.001, 0.0);
            if (step >= 20000)
                values.push_back(value);
        }
        return values;
    };
    const auto period = [](const std::vector<double>& values) {
        for (std::size_t length = 1; length < 50; ++length)
        {
            auto repeats = true;
            for (std::size_t index = 0; index + length < values.size() && repeats; ++index)
                repeats = std::abs(values[index] - values[index + length]) < 1.0e-9;
            if (repeats)
                return length;
        }
        return std::size_t{0};
    };
    require(period(settled(3.7397)) == 5, "r 3.7397 should settle into a 5-cycle");
    require(period(settled(3.7397, true)) == 5,
        "random values injected into x should still settle into the 5-cycle of a");
    require(period(settled(3.7021)) == 7, "r 3.7021 should settle into a 7-cycle");
    require(period(settled(4.0)) == 0 && period(settled(3.6262)) == 0,
        "r 4.0 and 3.6262 should settle into no short cycle");
    const auto nearly = settled(3.8493);
    auto apart = 0.0;
    for (std::size_t index = 0; index + 3 < nearly.size(); ++index)
        apart = std::max(apart, std::abs(nearly[index] - nearly[index + 3]));
    require(apart < 0.06, "r 3.8493 should nearly settle into a 3-cycle");
    // Whether the stream is constant, cyclic or chaotic depends on
    // a alone: at 2.5 it settles at 1 - 1/a.
    const auto constant = settled(2.5);
    require(period(constant) == 1 && std::abs(constant.front() - 0.6) < 1.0e-9,
        "a of 2.5 should settle at a constant, 0.6");
}

// Conversions: the table of asLogicValue and db
// values, 6 db doubling the amplitude and -6 db halving it, and the logic
// operations made of asLogicValue: a AND b, a OR b, NOT a, a XOR b and a
// EQV b.
void testConversionTable()
{
    requireExamples({
        {"-2 asLogicValue", ValueDomain::Scalar, 0.0},
        {"0 asLogicValue", ValueDomain::Scalar, 0.0},
        {"3 asLogicValue", ValueDomain::Scalar, 1.0},
        {"0 db", ValueDomain::Scalar, 1.0},
        {"6 db", ValueDomain::Scalar, 1.99, 0.01},
        {"-6 db", ValueDomain::Scalar, 0.5, 0.005},
        {"-12 db", ValueDomain::Scalar, 0.25, 0.005},
    }, "conversions");
    for (const auto a : {-1.0, 0.0, 2.0})
    {
        for (const auto b : {-3.0, 0.0, 0.5})
        {
            const auto logic = [](double value) { return value > 0.0 ? 1.0 : 0.0; };
            const auto la = logic(a);
            const auto lb = logic(b);
            const auto text = [](double value) { return "(" + std::to_string(value) + ")"; };
            const auto expect = [&](const std::string& source, double expected, const char* name) {
                require(std::abs(evaluate(source, ValueDomain::Scalar, {}).value - expected) < 1.0e-12,
                    std::string{""} + name + " of " + std::to_string(a) + " and " + std::to_string(b));
            };
            expect(text(a) + " asLogicValue * " + text(b) + " asLogicValue", la * lb, "a AND b");
            expect("(" + text(a) + " asLogicValue + " + text(b) + " asLogicValue) asLogicValue", logic(la + lb), "a OR b");
            expect("1 - " + text(a) + " asLogicValue", 1.0 - la, "NOT a");
            expect(text(a) + " asLogicValue ne: " + text(b) + " asLogicValue", la != lb ? 1.0 : 0.0, "a XOR b");
            expect(text(a) + " asLogicValue eq: " + text(b) + " asLogicValue", la == lb ? 1.0 : 0.0, "a EQV b");
        }
    }
}

// !TimingClock is a trigger 24 times a beat and
// !TimingClockDuration the seconds of one of its triggers, so that 24 of
// them are a beat's seconds (0.5 at 120 bpm, 1 at 60) and their inverse the
// beats in a second.
void testTimingClock()
{
    requireExamples({{"!TimingClockDuration * 24", ValueDomain::Scalar, 0.5},
                        {"(!TimingClockDuration * 24) inverse", ValueDomain::Scalar, 2.0}},
        "timing clock");
    EvaluationContext slow;
    slow.sampleRate = 48000.0;
    slow.tempoBpm = 60.0;
    require(std::abs(evaluate("!TimingClockDuration * 24", ValueDomain::Scalar, slow).value - 1.0) < 1.0e-12,
        "at 60 bpm, !TimingClockDuration * 24 should be 1");
    Timeline clock("!TimingClock");
    auto rises = 0;
    auto previous = 0.0;
    for (int millisecond = 0; millisecond < 1000; ++millisecond)
    {
        const auto value = clock.at(millisecond / 1000.0);
        rises += value > 0.0 && previous <= 0.0 ? 1 : 0;
        previous = value;
    }
    require(rises == 48, "!TimingClock should trigger 24 times a beat, 48 a second at 120 bpm, not "
            + std::to_string(rises));
}

// The Event Source `MIDITimingClock (also written
// `MIDITimingClock extractClock) triggers once for each MIDI timing clock
// message, which a renderer counts in midiTimingClockCountEventName, and
// !TimingClock follows it once the messages come; before them !TimingClock
// stands in at the Sound's tempo and `MIDITimingClock is 0. A device at 60
// bpm sends 24 messages a second: 24 triggers, where the stand-in at 120
// bpm gives 48.
void testMidiTimingClockFollowsItsMessages()
{
    struct Clock
    {
        double count = 0.0;
        bool kept = true;
        static bool resolve(void* user, std::string_view name, ExpressionValue& value) noexcept
        {
            const auto& clock = *static_cast<const Clock*>(user);
            if (!clock.kept || name != talas::expr::midiTimingClockCountEventName)
                return false;
            value = {clock.count, ValueDomain::Scalar};
            return true;
        }
    };
    for (const std::string_view source : {"`MIDITimingClock", "`MIDITimingClock extractClock", "!TimingClock"})
    {
        const auto standsIn = source == "!TimingClock";
        for (const bool kept : {true, false})
        {
            const auto parsed = talas::expr::parse(source, ExpressionLanguageVersion::ValV1);
            require(parsed.ok() && parsed.expression->readsMidiTimingClock()
                    && parsed.expression->eventReferences().empty(),
                std::string{source} + " should read the MIDI timing clock, no Event Value");
            auto expression = *parsed.expression;
            talas::expr::ExpressionState state;
            require(talas::expr::prepareExpressionState(expression, state), std::string{source} + " should prepare");
            Clock clock;
            clock.kept = kept;
            EvaluationContext context;
            context.sampleRate = 1000.0;
            context.user = &clock;
            context.resolveEvent = Clock::resolve;
            // The first second without messages, then 24 in the next.
            std::array<int, 2> rises{};
            auto previous = 0.0;
            std::size_t message = 0;
            for (int millisecond = 0; millisecond < 2000; ++millisecond)
            {
                if (millisecond >= 1000 && message < 24
                    && millisecond == 1000 + static_cast<int>(std::lround(static_cast<double>(message) * 1000.0 / 24.0)))
                {
                    clock.count += 1.0;
                    ++message;
                }
                context.elapsedSampleFrame = static_cast<std::uint64_t>(millisecond);
                const auto result = talas::expr::evaluateStateful(expression, context, state);
                require(result.ok(), std::string{source} + " should evaluate");
                const auto value = result.value.value;
                require(value == 0.0 || value == 1.0, std::string{source} + " should be a trigger of 0 and 1");
                rises[millisecond / 1000] += value > 0.0 && previous <= 0.0 ? 1 : 0;
                previous = value;
            }
            require(rises[0] == (standsIn ? 48 : 0),
                std::string{source} + " without messages should " + (standsIn ? "stand in at 120 bpm" : "stay at 0")
                    + ", not " + std::to_string(rises[0]));
            const auto expected = kept ? 24 : standsIn ? 48 : 0;
            require(rises[1] == expected,
                std::string{source} + (kept ? " should trigger once for each of 24 messages in a second"
                                            : " should keep the stand-in where no renderer counts the messages")
                    + ", not " + std::to_string(rises[1]));
        }
    }
    // !MIDITimingClock, with an exclamation mark, stays an Event Value of
    // that name.
    const auto plain = talas::expr::parse("!MIDITimingClock", ExpressionLanguageVersion::ValV1);
    require(plain.ok() && !plain.expression->readsMidiTimingClock()
            && plain.expression->eventReferences() == std::vector<std::string>{"MIDITimingClock"},
        "!MIDITimingClock should stay an Event Value");
}

// A value of a field of a project with the field reading settings given
// (format 156).
double fieldValue(std::string_view source, const talas::expr::ExpressionDefinitionLibrary& definitions,
    ValueDomain domain = ValueDomain::Scalar, double tempoBpm = 120.0)
{
    const auto parsed = talas::expr::parse(source, ExpressionLanguageVersion::ValV1, definitions);
    require(parsed.ok(), std::string{source} + " should parse: "
        + (parsed.diagnostics.empty() ? std::string{} : parsed.diagnostics.front().message));
    EvaluationContext context;
    context.sampleRate = 48000.0;
    context.tempoBpm = tempoBpm;
    const auto evaluated = talas::expr::evaluate(*parsed.expression, context, {domain});
    require(evaluated.ok(), std::string{source} + " should evaluate: "
        + (evaluated.diagnostics.empty() ? std::string{} : evaluated.diagnostics.front().message));
    return evaluated.value.value;
}

// Format 156: a project may take the floor-based rounding rule, where
// rounded adds 0.5 and truncates toward negative infinity, so -2.5 rounded
// is -2, and truncated rounds toward negative infinity, so -2.5 truncated
// is -3; the table's other examples answer as they do in any project.
// Smalltalk's rounded and truncated stay a project's unless it takes the
// floor-based rule.
void testManualRoundingTable()
{
    talas::expr::ExpressionDefinitionLibrary manual;
    manual.rounding = talas::expr::RoundingRule::ManualTable;
    const talas::expr::ExpressionDefinitionLibrary smalltalk;
    struct Row
    {
        std::string_view source;
        double manual;
        double smalltalk;
    };
    for (const auto& row : {
             // The table's examples.
             Row{"-7 // 3", -3.0, -3.0}, Row{"7 // 3", 2.0, 2.0}, Row{"-7 \\\\ 3", 2.0, 2.0},
             Row{"7 \\\\ 3", 1.0, 1.0}, Row{"2 inverse", 0.5, 0.5}, Row{"-7 quo: 3", -2.0, -2.0},
             Row{"7 quo: 3", 2.0, 2.0}, Row{"-7 rem: 3", -1.0, -1.0}, Row{"7 rem: 3", 1.0, 1.0},
             Row{"-2.5 rounded", -2.0, -3.0}, Row{"-2.5 truncated", -3.0, -2.0}, Row{"5 vmax: 2", 5.0, 5.0},
             Row{"5 vmin: 2", 2.0, 2.0},
             // rounded: add 0.5 and truncate toward negative infinity.
             Row{"2.5 rounded", 3.0, 3.0}, Row{"2.49 rounded", 2.0, 2.0}, Row{"-2.51 rounded", -3.0, -3.0},
             Row{"-0.5 rounded", 0.0, -1.0}, Row{"-3.5 rounded", -3.0, -4.0},
             // truncated: round toward negative infinity.
             Row{"3.7 truncated", 3.0, 3.0}, Row{"-3.2 truncated", -4.0, -3.0}, Row{"-3 truncated", -3.0, -3.0},
         })
    {
        requireNear(fieldValue(row.source, manual), row.manual,
            "" + std::string{row.source} + " under the floor-based rule");
        requireNear(fieldValue(row.source, smalltalk), row.smalltalk,
            "" + std::string{row.source} + " under Smalltalk's");
    }
    // Units stay as they were, as with Smalltalk's rounded.
    requireNear(fieldValue("-2.5 s rounded", manual, ValueDomain::Seconds), -2.0,
        "a time should round as a number does");
    // asInteger, which the table leaves out, stays toward zero.
    requireNear(fieldValue("-2.5 asInteger", manual), -2.0, "asInteger should still truncate toward zero");
    // The choice is the project's library's: a field without one keeps
    // Smalltalk's.
    requireNear(evaluate("-2.5 rounded", ValueDomain::Scalar).value, -3.0,
        "a field parsed without the project's settings should round as Smalltalk does");
}

// Format 156: a beat in a field lasts a quarter
// note at the project's tempo for fields, 60 in a new project, whatever
// the tempo the Sound plays to; a project keeping none, as every older one,
// follows that tempo, the Timeline's.
void testFieldBeatTempo()
{
    talas::expr::ExpressionDefinitionLibrary sixty;
    sixty.fieldBeatTempoBpm = talas::expr::newProjectFieldBeatTempoBpm;
    const talas::expr::ExpressionDefinitionLibrary timeline;
    requireNear(fieldValue("1 beat", sixty, ValueDomain::Seconds, 120.0), 1.0,
        "at quarter note = 60, a beat should last 1 s while the Timeline plays at 120");
    requireNear(fieldValue("3 beats", sixty, ValueDomain::Seconds, 90.0), 3.0,
        "three beats at 60 should last 3 s");
    requireNear(fieldValue("0.5 beats s", sixty, ValueDomain::Seconds, 120.0), 0.5,
        "half a beat at 60 should be half a second");
    requireNear(fieldValue("1 beat", timeline, ValueDomain::Seconds, 120.0), 0.5,
        "a project keeping no beat tempo should follow the Timeline's 120");
    requireNear(fieldValue("1 beat", timeline, ValueDomain::Seconds, 60.0), 1.0,
        "a project keeping no beat tempo should follow the Timeline's tempo");
    talas::expr::ExpressionDefinitionLibrary ninety;
    ninety.fieldBeatTempoBpm = 90.0;
    requireNear(fieldValue("3 beats", ninety, ValueDomain::Seconds, 120.0), 2.0,
        "a project's beat tempo of 90 should make three beats 2 s");
    // A beat of something read as the Sound plays.
    const auto parsed = talas::expr::parse("!Beats beats", ExpressionLanguageVersion::ValV1, sixty);
    require(parsed.ok(), "!Beats beats should parse");
    EvaluationContext context;
    context.tempoBpm = 120.0;
    double beats = 4.0;
    context.user = &beats;
    context.resolveEvent = [](void* user, std::string_view name, ExpressionValue& value) noexcept {
        if (name != "Beats")
            return false;
        value = {*static_cast<double*>(user), ValueDomain::Scalar};
        return true;
    };
    const auto read = talas::expr::evaluate(*parsed.expression, context, {ValueDomain::Seconds});
    require(read.ok() && read.value.domain == ValueDomain::Seconds && std::abs(read.value.value - 4.0) < 1.0e-12,
        "an Event Value of 4 in beats at 60 should last 4 s");
    // !TimingClock keeps the tempo the Sound plays to.
    requireNear(fieldValue("!TimingClockDuration * 24", sixty, ValueDomain::Scalar, 120.0), 0.5,
        "the timing clock should keep the Timeline's 120");
}

// Format 156: a project may read a number without units given to
// inSOSPitch as a fraction of 22050 hz, so 11025.0 inSOSPitch is 0.5; a
// frequency in hertz or a pitch keeps its place on the spectral scale, as
// does every number in a project that does not choose it.
void testLinearUnitlessSOSPitch()
{
    talas::expr::ExpressionDefinitionLibrary linear;
    linear.unitlessSOSPitch = talas::expr::UnitlessSOSPitch::Linear;
    const talas::expr::ExpressionDefinitionLibrary hertz;
    requireNear(fieldValue("11025.0 inSOSPitch", linear), 0.5, "11025.0 inSOSPitch should be 0.5");
    requireNear(fieldValue("22050 inSOSPitch", linear), 1.0, "22050 inSOSPitch should be 1");
    requireNear(fieldValue("0 inSOSPitch", linear), 0.0, "0 inSOSPitch should be 0");
    requireNear(fieldValue("11025 hz inSOSPitch", linear), 1.0 - 1.0 / 15.0,
        "11025 hz should keep its place on the spectral scale");
    requireNear(fieldValue("4 a inSOSPitch", linear), 1.0 + std::log2(440.0 / 22050.0) / 15.0,
        "a pitch should keep its place on the spectral scale");
    require(std::abs(fieldValue("1378.12 hz inSOSPitch", linear) - 0.7333) < 1.0e-4,
        "1378.12 hz inSOSPitch should stay 0.73");
    requireNear(fieldValue("11025.0 inSOSPitch", hertz), 1.0 - 1.0 / 15.0,
        "a project reading numbers as hertz should give 11025.0 inSOSPitch 14/15");
}

// The Spectral Analysis and GA
// chapters' expressions. (1 s normal smooth: 1 s) draws a number from -1 to
// 1 each second, from a normal distribution, and moves to it in a straight
// line; (1 s random smooth: 1 s) draws from a uniform one; with t := (1 s
// randExp) the draws come at a random rate. 1 bpm: (2 s bpm removeUnits)
// triggers every 2 s, and 120 bpm s * 8 is eight beats, 4 s. !KeyDown ramp
// rises from 0 to 1 at each key down and !KeyDown triangle rises and falls
// back; !KeyNumber - 36 / {63 - 36}, left to right, maps keys 36 to 63 onto
// 0 to 1.
void testDocumentedAnalysisExpressions()
{
    const auto drawn = [](std::string_view source) {
        Timeline line(source);
        std::vector<double> values;
        for (int second = 1; second <= 400; ++second)
            values.push_back(line.at(static_cast<double>(second)));
        return values;
    };
    const auto nearZero = [](const std::vector<double>& values) {
        return static_cast<double>(std::ranges::count_if(values, [](double value) { return std::abs(value) < 0.5; }))
            / static_cast<double>(values.size());
    };
    const auto normal = drawn("1 s normal smooth: 1 s");
    const auto uniform = drawn("1 s random smooth: 1 s");
    require(std::ranges::all_of(normal, [](double value) { return value >= -1.0 && value <= 1.0; })
            && std::ranges::all_of(uniform, [](double value) { return value >= -1.0 && value <= 1.0; }),
        "the smoothed random numbers should lie from -1 to 1");
    require(nearZero(normal) > nearZero(uniform) + 0.15 && std::abs(nearZero(uniform) - 0.5) < 0.1,
        "normal numbers should lie nearer 0 than uniform ones, which are as likely anywhere");
    {
        Timeline line("1 s normal smooth: 1 s");
        std::vector<double> path;
        for (int tick = 0; tick <= 4000; tick += 250)
            path.push_back(line.at(static_cast<double>(tick) / 1000.0));
        auto straight = true;
        for (std::size_t index = 4; index + 4 < path.size(); index += 4)
        {
            for (std::size_t step = 1; step < 4; ++step)
            {
                const auto expected = path[index] + (path[index + 4] - path[index]) * static_cast<double>(step) / 4.0;
                straight = straight && std::abs(path[index + step] - expected) < 0.02;
            }
        }
        require(straight, "between draws the value should move in a straight line");
    }
    {
        Timeline line("| t | t := (1 s randExp). (t s random smooth: t s)");
        auto inRange = true;
        for (int tick = 0; tick <= 20000; tick += 100)
        {
            const auto value = line.at(static_cast<double>(tick) / 1000.0);
            inRange = inRange && value >= -1.0 && value <= 1.0;
        }
        require(inRange, "random smoothing at a random rate should stay from -1 to 1");
    }
    {
        Timeline metronome("1 bpm: (2 s bpm removeUnits)");
        std::vector<double> rises;
        auto previous = 0.0;
        for (int tick = 0; tick <= 9000; ++tick)
        {
            const auto value = metronome.at(static_cast<double>(tick) / 1000.0);
            if (value > 0.0 && previous <= 0.0)
                rises.push_back(static_cast<double>(tick) / 1000.0);
            previous = value;
        }
        require(rises.size() >= 4 && std::abs(rises[1] - rises[0] - 2.0) < 0.01 && std::abs(rises[2] - rises[1] - 2.0) < 0.01,
            "1 bpm: of a duration's bpm should trigger once each duration");
    }
    {
        const auto beats = talas::expr::parse("120 bpm s * 8", ExpressionLanguageVersion::ValV1);
        require(beats.ok(), "120 bpm s * 8 should parse");
        talas::expr::EvaluationContext context;
        const auto eight = talas::expr::evaluate(*beats.expression, context, {ValueDomain::Seconds});
        require(eight.ok(), "120 bpm s * 8 should evaluate as a time");
        requireNear(eight.value.value, 4.0, "120 bpm s * 8 should be the duration of 8 beats");
    }
    {
        Timeline ramp("!X ramp");
        Timeline triangle("!X triangle");
        (void)ramp.at(0.0, 0.0);
        (void)triangle.at(0.0, 0.0);
        requireNear(ramp.at(0.001, 1.0), 0.0, "!KeyDown ramp should start from 0 at a key down");
        requireNear(ramp.at(0.501, 1.0), 0.5, "!KeyDown ramp should rise in a line");
        requireNear(ramp.at(1.001, 1.0), 1.0, "!KeyDown ramp should reach 1");
        (void)triangle.at(0.001, 1.0);
        requireNear(triangle.at(0.501, 1.0), 1.0, "!KeyDown triangle should rise to 1");
        requireNear(triangle.at(1.001, 1.0), 0.0, "!KeyDown triangle should fall back to 0");
    }
    Timeline keys("!X - 36 / {63 - 36}");
    requireNear(keys.at(0.0, 36.0), 0.0, "key 36 should map to 0");
    requireNear(keys.at(0.001, 63.0), 1.0, "key 63 should map to 1");
    requireNear(Timeline("!X - 36 / 127").at(0.0, 36.0), 0.0, "!KeyNumber - 36 / 127 should be 0 at key 36");
    requireNear(Timeline("!X / 127").at(0.0, 127.0), 1.0, "!KeyNumber / 127 should be 1 at key 127");
}

int main()
{
    try
    {
        testPastedSounds();
        testDocumentedAnalysisExpressions();
        testUnitsAndConversions();
        testArithmetic();
        testArrays();
        testLogic();
        testPrecedence();
        testEventValues();
        testEventSourcesInFields();
        testFieldUnits();
        testUnknownMessagesAreReported();
        testFunctionsOfTime();
        testRandomAndSmoothing();
        testTriggerMessages();
        testSwarmFollower();
        testRandomDistributions();
        testTrackers();
        testSmalltalkNumberMessages();
        testTimeCode();
        testValueFieldSyntax();
        testLeadingZeros();
        testTimingClock();
        testMidiTimingClockFollowsItsMessages();
        testConversionTable();
        testVariableNames();
        testFieldPrograms();
        testRepeatingFunctionsAndMetronomes();
        testMetronomesStealTheirVoices();
        testRandomExpressionsDrawTheirOwn();
        testLatchesHoldEventValues();
        testDocumentedArithmeticRemarks();
        testNoteNumberAndHertzRatios();
        testKeyMorphRatio();
        testBpmWithoutItsUnits();
        testTrianglesAndSeededRandomNumbers();
        testSignalProcessorMessages();
        testTopTenArithmeticExpressions();
        testArraysIndexScalesAndWarps();
        testLogicAndConditionalCases();
        testEventExpressionsAndUnits();
        testExpressionsLibraryFaderArithmetic();
        testEventVariables();
        testGeneratorsFromTheTutorial();
        testWorkedTutorialExpressions();
        testUnitsInTheSoundEditor();
        testLogisticMapCycles();
        testClockArithmetic();
        testSmallestFrequencyStepInCents();
        testStretchingDurations();
        testOtherProfilesUnchanged();
        testMostFieldsExpectZeroToOne();
        testManualRoundingTable();
        testFieldBeatTempo();
        testLinearUnitlessSOSPitch();
        std::cout << "Val tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
