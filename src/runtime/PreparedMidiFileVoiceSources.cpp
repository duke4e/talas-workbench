#include "PreparedMidiFileVoiceSources.h"
#include "talas/core/AnalogSequencerExtraValues.h"
#include "talas/core/AnalogSequencerArray.h"
#include "AnalogSequencerMath.h"
#include "ScopedEventValueResolution.h"
#include "StandardMidiFile.h"
#include "talas/core/FromChars.h"
#include "talas/project/MidiDefaultMap.h"
#include "talas/project/SmalltalkSubset.h"
#include "talas/runtime/PreparedMidiMapper.h"
#include "talas/expr/Expression.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>

namespace talas::runtime
{
namespace
{
[[nodiscard]] bool hasNonBlankAscii(std::string_view text) noexcept
{
    return std::ranges::any_of(text, [](char value) {
        return value != ' ' && value != '\t' && value != '\r'
            && value != '\n' && value != '\f' && value != '\v';
    });
}

[[nodiscard]] bool bindSequencerVariables(PreparedAnalogSequencerSequence& sequence,
    const talas::expr::Expression& expression,
    const talas::project::PreparedProjectVariableTable& variables, std::string& error)
{
    auto references = expression.variableReferences().size();
    for (const auto& cells : sequence.variableCellIndices)
        references += cells.size();
    if (references > talas::project::maximumPreparedProjectVariableReferenceUses)
    {
        error = "Analog Sequencer exceeds the prepared variable reference limit";
        return false;
    }
    std::vector<std::uint32_t> indices;
    indices.reserve(expression.variableReferences().size());
    for (const auto& reference : expression.variableReferences())
    {
        const auto cell = talas::project::resolvePreparedProjectVariableCell(
            variables, sequence.expressionScope, reference);
        if (!cell || *cell > std::numeric_limits<std::uint32_t>::max())
        {
            error = "Analog Sequencer has an unbound project variable: " + reference;
            return false;
        }
        indices.push_back(static_cast<std::uint32_t>(*cell));
    }
    sequence.variableCellIndices.push_back(std::move(indices));
    return true;
}

[[nodiscard]] std::optional<PreparedAnalogSequencerSequence> parseSequencerArray(
    std::string_view text, const RenderSettings& settings,
    talas::expr::ValueDomain domain, RuntimeScopeId scope, std::string& error)
{
    const auto array = talas::core::parseAnalogSequencerArray(text);
    if (!array)
    {
        error = "Analog Sequencer arrays require 1 to 256 numeric, reference or brace-expression members";
        return {};
    }
    PreparedAnalogSequencerSequence result;
    result.domain = domain;
    result.expressionScope = scope;
    result.values.reserve(array->members.size());
    std::vector<talas::expr::ExpressionValue> initialVariables;
    for (const auto& cell : settings.preparedVariables.cells)
        initialVariables.push_back({cell.initialValue, cell.domain});
    talas::expr::EvaluationContext context;
    context.sampleRate = settings.sampleRate;
    context.tempoBpm = settings.tempoBpm;
    context.rampsStartAtFirstValue = settings.rampsStartAtFirstValue;
    context.rampsHoldUntriggered = settings.rampsHoldUntriggered;
    for (const auto& member : array->members)
    {
        double value = 0;
        if (array->numericLiteral)
        {
            talas::fromChars(member.source.data(), member.source.data() + member.source.size(), value);
            result.values.push_back(value);
            continue;
        }
        // As in numeric #(...) lists, an unadorned number uses the field's
        // native domain. Explicit unit and brace expressions retain DSL typing.
        auto expressionSource = member.source;
        if (member.implicitDomainNumber && expressionSource.starts_with('+'))
            expressionSource.remove_prefix(1);
        std::string typedNumber;
        if (member.implicitDomainNumber && domain != talas::expr::ValueDomain::Scalar)
            typedNumber = std::string{expressionSource}
                + (domain == talas::expr::ValueDomain::Seconds ? " s" : " nn");
        const auto parsed = talas::expr::parse(typedNumber.empty() ? expressionSource : typedNumber,
            settings.expressionLanguage,
            settings.expressionDefinitions);
        if (!parsed.ok() || !parsed.expression->identifiers().empty()
            || (parsed.expression->isStateful() && !parsed.expression->usesTime()))
        {
            error = "Analog Sequencer key arrays require stateless or clocked expressions without runtime identifiers";
            return {};
        }
        for (const auto& reference : parsed.expression->eventReferences())
            if (detail::findNearestEventValue(settings.eventValues, settings.preparedVariables,
                    scope, reference) == settings.eventValues.end())
            {
                error = "Analog Sequencer key array has an unbound EventValue: " + reference;
                return {};
            }
        if (!bindSequencerVariables(result, *parsed.expression, settings.preparedVariables, error))
            return {};
        result.expressions.push_back(*parsed.expression);
        if (parsed.expression->eventReferences().empty()
            && parsed.expression->variableReferences().empty() && !parsed.expression->usesTime())
        {
            const auto evaluated = talas::expr::evaluate(*parsed.expression, context, {domain});
            if (!evaluated.ok() || !std::isfinite(evaluated.value.value))
            {
                error = "Analog Sequencer key array expression has an invalid value or unit";
                return {};
            }
            value = evaluated.value.value;
        }
        else if (parsed.expression->usesTime())
        {
            talas::expr::ExpressionState state;
            if (!talas::expr::prepareExpressionState(*parsed.expression, state)
                || !evaluateAnalogSequencerValue(result, result.expressions.size() - 1,
                    settings.eventValues, settings.preparedVariables, settings.sampleRate,
                    settings.tempoBpm, value, initialVariables, &state))
            {
                error = "Analog Sequencer timed key array expression requires a finite initial value with valid units";
                return {};
            }
        }
        // Stateless live members defer execution until an accepted stage.
        // Clocked members run continuously and require a finite initial value.
        result.values.push_back(value);
    }
    return result;
}

[[nodiscard]] bool isPreparedInputVoiceOwner(const CompileStep& step) noexcept
{
    if (hasPreparedAnalogSequencerKeyOwner(step))
        return true;
    if (step.typeId == "talas.midiVoice")
        return true;
    if (step.typeId != "talas.midiMapper")
        return false;
    const ParameterValue* model = nullptr;
    const ParameterValue* map = nullptr;
    for (const auto& parameter : step.parameters)
    {
        if (parameter.id == "voiceModel")
        {
            if (model != nullptr) return false;
            model = &parameter;
        }
        else if (parameter.id == "map")
        {
            if (map != nullptr) return false;
            map = &parameter;
        }
    }
    if (model == nullptr || !model->choice.has_value()
        || *model->choice != "polyphonicInput"
        || map == nullptr || !map->text.has_value())
        return false;
    const auto parsed = parsePreparedMidiMapperMode(*map->text);
    return parsed.ok() && parsed.mode == PreparedMidiMapperMode::Passthrough;
}

[[nodiscard]] std::vector<std::pair<std::uint8_t, std::string>>
taggedControllerSources(const CompileStep& step)
{
    std::vector<std::pair<std::uint8_t, std::string>> sources;
    if (step.typeId != "talas.midiMapper") return sources;
    const auto map = std::ranges::find(step.parameters, "map",
        &ParameterValue::id);
    if (map == step.parameters.end() || !map->text) return sources;
    const auto parsed = parsePreparedMidiMapperMode(*map->text);
    if (!parsed.ok()) return sources;
    for (std::size_t index = 0; index < parsed.localAssociationCount; ++index)
    {
        const auto& association = parsed.localAssociations[index];
        if (!association.channel) continue;
        const auto name = resolvePreparedMidiMapperLocalSource(
            association.localEventSource, association.channel);
        if (name)
            sources.emplace_back(*association.channel, *name);
    }
    return sources;
}

// This is a bounded literal subset of the Smalltalk MIDI Script field.
// Preparation rejects programs it cannot execute; no script code runs in the
// audio callback. Without an MM declaration, a beat is one second.
struct ScriptCursor
{
    std::string_view source;
    std::size_t offset = 0;

    void space() noexcept
    {
        while (offset < source.size() && (source[offset] == ' ' || source[offset] == '\t'
            || source[offset] == '\r' || source[offset] == '\n')) ++offset;
    }

    bool take(std::string_view token) noexcept
    {
        space();
        if (source.substr(offset).starts_with(token))
        {
            offset += token.size();
            return true;
        }
        return false;
    }

    bool number(double& value) noexcept
    {
        space();
        const auto* begin = source.data() + offset;
        const auto* end = source.data() + source.size();
        const auto parsed = talas::fromChars(begin, end, value, std::chars_format::general);
        if (parsed.ec != std::errc{} || parsed.ptr == begin || !std::isfinite(value))
            return false;
        offset = static_cast<std::size_t>(parsed.ptr - source.data());
        // from_chars accepts `90.` as a number; in this script grammar the
        // final dot terminates the Smalltalk statement.
        if (source[offset - 1] == '.') --offset;
        return true;
    }

    std::string_view unit() noexcept
    {
        space();
        const auto begin = offset;
        while (offset < source.size() && ((source[offset] >= 'a' && source[offset] <= 'z')
            || (source[offset] >= 'A' && source[offset] <= 'Z'))) ++offset;
        return source.substr(begin, offset - begin);
    }

    std::string_view controllerName() noexcept
    {
        space();
        if (offset == source.size() || source[offset] != '!') return {};
        const auto begin = ++offset;
        if (offset == source.size()
            || !((source[offset] >= 'a' && source[offset] <= 'z')
                || (source[offset] >= 'A' && source[offset] <= 'Z')
                || source[offset] == '_')) return {};
        while (offset < source.size()
            && ((source[offset] >= 'a' && source[offset] <= 'z')
                || (source[offset] >= 'A' && source[offset] <= 'Z')
                || (source[offset] >= '0' && source[offset] <= '9')
                || source[offset] == '_')) ++offset;
        return source.substr(begin, offset - begin);
    }

    bool done() noexcept { space(); return offset == source.size(); }
};

[[nodiscard]] bool scriptScalar(
    ScriptCursor& cursor, std::optional<double> loopIndex,
    double& value, unsigned depth = 0) noexcept;

[[nodiscard]] bool scriptAtom(
    ScriptCursor& cursor, std::optional<double> loopIndex,
    double& value, unsigned depth) noexcept
{
    if (depth >= 8) return false;
    if (cursor.take("("))
        return scriptScalar(cursor, loopIndex, value, depth + 1)
            && cursor.take(")");
    if (loopIndex && cursor.take("i"))
    {
        value = *loopIndex;
        return true;
    }
    return cursor.number(value);
}

[[nodiscard]] bool scriptProduct(
    ScriptCursor& cursor, std::optional<double> loopIndex,
    double& value, unsigned depth) noexcept
{
    if (!scriptAtom(cursor, loopIndex, value, depth)) return false;
    while (true)
    {
        const bool multiply = cursor.take("*");
        if (!multiply && !cursor.take("/")) break;
        double right = 0.0;
        if (!scriptAtom(cursor, loopIndex, right, depth)
            || (!multiply && right == 0.0)) return false;
        value = multiply ? value * right : value / right;
        if (!std::isfinite(value)) return false;
    }
    return true;
}

[[nodiscard]] bool scriptScalar(
    ScriptCursor& cursor, std::optional<double> loopIndex,
    double& value, unsigned depth) noexcept
{
    if (!scriptProduct(cursor, loopIndex, value, depth)) return false;
    while (true)
    {
        const bool add = cursor.take("+");
        if (!add && !cursor.take("-")) break;
        double right = 0.0;
        if (!scriptProduct(cursor, loopIndex, right, depth)) return false;
        value = add ? value + right : value - right;
        if (!std::isfinite(value)) return false;
    }
    return true;
}

[[nodiscard]] bool scriptTime(
    ScriptCursor& cursor, double bpm, std::optional<double> loopIndex,
    double& seconds) noexcept
{
    double value = 0.0;
    if (!scriptScalar(cursor, loopIndex, value) || value < 0.0) return false;
    const auto unit = cursor.unit();
    if (unit == "s") seconds = value;
    else if (unit == "ms") seconds = value / 1000.0;
    else if ((unit == "beat" || unit == "beats") && bpm > 0.0)
        seconds = value * 60.0 / bpm;
    else return false;
    return std::isfinite(seconds);
}

[[nodiscard]] bool scriptPitch(
    ScriptCursor& cursor, std::optional<double> loopIndex,
    double& pitch) noexcept
{
    double value = 0.0;
    if (!scriptScalar(cursor, loopIndex, value)) return false;
    const auto unit = cursor.unit();
    if (unit == "hz" && value > 0.0)
        pitch = 69.0 + 12.0 * std::log2(value / 440.0);
    else if (unit == "nn") pitch = value;
    else
    {
        if (unit.size() != 1 || std::trunc(value) != value) return false;
        int semitone = 0;
        switch (unit[0])
        {
        case 'c': semitone = 0; break;
        case 'd': semitone = 2; break;
        case 'e': semitone = 4; break;
        case 'f': semitone = 5; break;
        case 'g': semitone = 7; break;
        case 'a': semitone = 9; break;
        case 'b': semitone = 11; break;
        default: return false;
        }
        const auto beforeAccidental = cursor.offset;
        const auto accidental = cursor.unit();
        if (accidental == "sharp") ++semitone;
        else if (accidental == "flat") --semitone;
        else cursor.offset = beforeAccidental;
        pitch = 12.0 * (value + 1.0) + semitone;
        if (cursor.take("+"))
        {
            double interval = 0.0;
            if (!scriptScalar(cursor, loopIndex, interval)
                || cursor.unit() != "nn") return false;
            pitch += interval;
        }
    }
    return std::isfinite(pitch);
}

[[nodiscard]] bool scriptFrame(double seconds, double sampleRate, std::size_t& frame) noexcept
{
    constexpr double maximumScriptFrame = 1e12;
    const auto exact = seconds * sampleRate;
    if (!std::isfinite(exact) || exact < 0.0 || exact > maximumScriptFrame) return false;
    frame = static_cast<std::size_t>(std::llround(exact));
    return true;
}

// The literal statements a script may write out: room for the prepared
// event limit's notes, 2,187 of them in a sequence made six
// times over into itself, at some 128 bytes each.
constexpr std::size_t maximumScriptStatementBytes = std::size_t{1} << 22;

// A note without a frequency plays defaultPitch: its MIDIVoice's or
// MIDIMapper's LowPitch, or note 0 in older projects. A
// script's own text holds at most 64 KiB, the statements written out for
// it at most maximumScriptStatementBytes.
[[nodiscard]] bool parseLiteralMidiScript(
    std::string_view source, double sampleRate, std::size_t maximumEvents, double defaultPitch,
    std::vector<PreparedMidiScriptNoteEvent>& events,
    std::vector<PreparedMidiScriptControllerEvent>& controllers,
    std::string& error, std::size_t maximumSourceBytes = 65536)
{
    if (source.empty() || source.size() > maximumSourceBytes || !std::isfinite(sampleRate)
        || sampleRate <= 0.0)
    {
        error = "MIDI Voice Script requires nonempty bounded text and a positive sample rate";
        return false;
    }
    ScriptCursor cursor{source};
    double bpm = 60.0;
    std::map<std::string, PreparedMidiScriptControllerEvent> previousControllers;
    const auto appendNote = [&](ScriptCursor& noteCursor, double metronome,
                                std::optional<double> loopIndex,
                                bool allowEndWithoutPeriod) {
        if (!noteCursor.take("self") || !noteCursor.take("keyDownAt:"))
        {
            error = "MIDI Voice Script supports literal self keyDownAt: statements only";
            return false;
        }
        double onset = 0.0, duration = 0.01, pitch = defaultPitch;
        double velocity = 1.0, timbre = 0.0;
        if (!scriptTime(noteCursor, metronome, loopIndex, onset))
        {
            error = "MIDI Voice Script onset requires s, ms, or beats";
            return false;
        }
        bool hasDuration = false, hasFrequency = false;
        bool hasVelocity = false, hasTimbre = false;
        while (!noteCursor.take("."))
        {
            if (allowEndWithoutPeriod && noteCursor.done()) break;
            if (!hasDuration && noteCursor.take("duration:"))
            {
                if (!scriptTime(noteCursor, metronome, loopIndex, duration)
                    || duration <= 0.0)
                {
                    error = "MIDI Voice Script duration must be positive and use a time unit";
                    return false;
                }
                hasDuration = true;
            }
            else if (!hasFrequency && noteCursor.take("frequency:"))
            {
                if (!scriptPitch(noteCursor, loopIndex, pitch))
                {
                    error = "MIDI Voice Script frequency requires hz, nn, or an octave note with an optional accidental and nn offset";
                    return false;
                }
                hasFrequency = true;
            }
            else if (!hasVelocity && noteCursor.take("velocity:"))
            {
                if (!scriptScalar(noteCursor, loopIndex, velocity)
                    || velocity < 0.0 || velocity > 1.0)
                {
                    error = "MIDI Voice Script velocity must be between 0 and 1";
                    return false;
                }
                hasVelocity = true;
            }
            else if (!hasTimbre && noteCursor.take("timbre:"))
            {
                if (!scriptScalar(noteCursor, loopIndex, timbre)
                    || timbre < 0.0 || timbre > 1.0)
                {
                    error = "MIDI Voice Script timbre must be between 0 and 1";
                    return false;
                }
                hasTimbre = true;
            }
            else
            {
                error = "MIDI Voice Script contains an unsupported or duplicate selector";
                return false;
            }
        }
        if (!std::isfinite(pitch) || pitch < 0.0 || pitch > 127.0)
        {
            error = "MIDI Voice Script frequency lies outside the prepared note range";
            return false;
        }
        std::size_t start = 0, length = 0;
        if (!scriptFrame(onset, sampleRate, start)
            || !scriptFrame(duration, sampleRate, length) || length == 0
            || start > std::numeric_limits<std::size_t>::max() - length)
        {
            error = "MIDI Voice Script time cannot be represented as a positive sample duration";
            return false;
        }
        if (events.size() + controllers.size() > maximumEvents
            || maximumEvents - events.size() - controllers.size() < 2)
        {
            error = "MIDI Voice Script exceeds the prepared event limit";
            return false;
        }
        const auto note = static_cast<std::uint8_t>(std::lround(pitch));
        const auto allocationVelocity = static_cast<std::uint8_t>(
            std::max<long>(1, std::lround(velocity * 127.0)));
        events.push_back({start, true, note, allocationVelocity,
            pitch, velocity, timbre});
        events.push_back({start + length, false, note, 0,
            pitch, velocity, timbre});
        return true;
    };
    while (!cursor.done())
    {
        if (cursor.take("MM"))
        {
            double declaredBpm = 0.0;
            if (!cursor.take(":=") || !cursor.number(declaredBpm)
                || declaredBpm <= 0.0 || !cursor.take("."))
            {
                error = "MIDI Voice Script has an invalid MM tempo declaration";
                return false;
            }
            bpm = declaredBpm;
            continue;
        }
        const auto statementStart = cursor.offset;
        if (cursor.take("self") && cursor.take("controller:"))
        {
            const auto name = cursor.controllerName();
            const auto set = cursor.take("setTo:");
            const auto slide = !set && cursor.take("slideTo:");
            double value = 0.0;
            double time = 0.0;
            std::size_t frame = 0;
            if (name.empty() || (!set && !slide)
                || !scriptScalar(cursor, std::nullopt, value))
            {
                error = "MIDI Voice Script controller requires literal !Name setTo: or slideTo: value";
                return false;
            }
            double steps = 0.0;
            double stepSize = 0.0;
            bool fixedStepSize = false;
            if (slide && cursor.take("steps:"))
            {
                if (!scriptScalar(cursor, std::nullopt, steps)
                    || steps < 1.0 || steps > 1024.0
                    || std::trunc(steps) != steps)
                {
                    error = "MIDI Voice Script controller slide requires 1 to 1024 integer steps";
                    return false;
                }
            }
            else if (slide && cursor.take("stepSize:"))
            {
                fixedStepSize = true;
                if (!scriptScalar(cursor, std::nullopt, stepSize)
                    || stepSize <= 0.0)
                {
                    error = "MIDI Voice Script controller slide requires a positive stepSize";
                    return false;
                }
            }
            else if (slide)
            {
                // slideTo: 1 byTime: 6 s slides in 10 steps.
                steps = 10.0;
            }
            if (!cursor.take(set ? "atTime:" : "byTime:")
                || !scriptTime(cursor, bpm, std::nullopt, time)
                || !cursor.take(".")
                || !scriptFrame(time, sampleRate, frame))
            {
                error = "MIDI Voice Script controller requires a representable time with units";
                return false;
            }
            const std::string controllerName(name);
            const auto previous = previousControllers.find(controllerName);
            if (set)
            {
                if (previous != previousControllers.end()
                    && frame < previous->second.frame)
                {
                    error = "MIDI Voice Script controller statements must advance in time";
                    return false;
                }
                if (events.size() + controllers.size() >= maximumEvents)
                {
                    error = "MIDI Voice Script exceeds the prepared event limit";
                    return false;
                }
                controllers.push_back({frame, controllerName, value});
            }
            else
            {
                if (previous == previousControllers.end()
                    || frame <= previous->second.frame)
                {
                    error = "MIDI Voice Script controller slide requires a prior earlier value";
                    return false;
                }
                const auto frameSpan = frame - previous->second.frame;
                const auto valueSpan = value - previous->second.value;
                const auto quotient = fixedStepSize
                    ? std::abs(valueSpan) / stepSize : steps;
                if (!std::isfinite(valueSpan) || !std::isfinite(quotient)
                    || quotient <= 0.0 || quotient > 1024.0)
                {
                    error = "MIDI Voice Script controller slide exceeds prepared steps or event limit";
                    return false;
                }
                const auto count = fixedStepSize
                    ? std::max<std::size_t>(1, static_cast<std::size_t>(
                        std::ceil(quotient
                            - 1e-12 * std::max(1.0, quotient))))
                    : static_cast<std::size_t>(steps);
                if (count == 0 || count > 1024 || frameSpan < count
                    || events.size() + controllers.size() > maximumEvents
                    || maximumEvents - events.size() - controllers.size() < count)
                {
                    error = "MIDI Voice Script controller slide exceeds prepared steps or event limit";
                    return false;
                }
                auto lastFrame = previous->second.frame;
                for (std::size_t step = 1; step <= count; ++step)
                {
                    const auto ratio = static_cast<double>(step)
                        / static_cast<double>(count);
                    const auto stepFrame = step == count ? frame
                        : previous->second.frame + static_cast<std::size_t>(
                            std::llround(static_cast<double>(frameSpan) * ratio));
                    const auto stepValue = step == count ? value
                        : fixedStepSize
                        ? previous->second.value + std::copysign(
                            std::min(std::abs(valueSpan),
                                stepSize * static_cast<double>(step)), valueSpan)
                        : previous->second.value + valueSpan * ratio;
                    if (stepFrame <= lastFrame || !std::isfinite(stepValue))
                    {
                        error = "MIDI Voice Script controller slide steps collapse at this sample rate";
                        return false;
                    }
                    controllers.push_back({stepFrame, controllerName, stepValue});
                    lastFrame = stepFrame;
                }
            }
            previousControllers[controllerName] = {frame, controllerName, value};
            continue;
        }
        cursor.offset = statementStart;
        double first = 0.0;
        if (cursor.number(first))
        {
            double last = 0.0;
            if (!cursor.take("to:") || !cursor.number(last)
                || !cursor.take("do:") || !cursor.take("[")
                || !cursor.take(":i") || !cursor.take("|")
                || std::trunc(first) != first || std::trunc(last) != last
                || first < -1000000.0 || last > 1000000.0
                || last < first || last - first + 1.0 > 1024.0)
            {
                error = "MIDI Voice Script loop requires bounded integer to:do: indices";
                return false;
            }
            const auto bodyStart = cursor.offset;
            const auto bodyEnd = source.find(']', bodyStart);
            if (bodyEnd == std::string_view::npos
                || source.substr(bodyStart, bodyEnd - bodyStart).find('[')
                    != std::string_view::npos)
            {
                error = "MIDI Voice Script loop requires one non-nested note body";
                return false;
            }
            const auto body = source.substr(bodyStart, bodyEnd - bodyStart);
            cursor.offset = bodyEnd + 1;
            if (!cursor.take("."))
            {
                error = "MIDI Voice Script loop needs a closing period";
                return false;
            }
            for (auto index = static_cast<std::int64_t>(first);
                 index <= static_cast<std::int64_t>(last); ++index)
            {
                ScriptCursor bodyCursor{body};
                if (!appendNote(bodyCursor, bpm, static_cast<double>(index), true))
                {
                    error = "MIDI Voice Script loop: " + error;
                    return false;
                }
                if (!bodyCursor.done())
                {
                    error = "MIDI Voice Script loop requires one note body";
                    return false;
                }
            }
            continue;
        }
        if (!appendNote(cursor, bpm, std::nullopt, false)) return false;
    }
    if (events.empty() && controllers.empty())
    {
        error = "MIDI Voice Script must schedule at least one event";
        return false;
    }
    // Notes that overlap on one MIDI key, as random notes can,
    // each take a key of their own while they sound, the nearest free
    // one: the allocator releases the oldest voice of a key, which would end
    // the wrong one of two such notes. A voice plays its note's pitch, not
    // its key, so the key only pairs a note's start with its end.
    {
        std::vector<std::size_t> starts;
        for (std::size_t index = 0; index + 1 < events.size(); index += 2)
            starts.push_back(index);
        std::ranges::stable_sort(starts, {}, [&](std::size_t index) { return events[index].frame; });
        std::array<std::vector<std::size_t>, 128> soundingUntil;
        for (const auto index : starts)
        {
            const auto start = events[index].frame;
            const auto free = [&](int key) {
                auto& ends = soundingUntil[static_cast<std::size_t>(key)];
                std::erase_if(ends, [start](std::size_t end) { return end <= start; });
                return ends.empty();
            };
            const auto wanted = static_cast<int>(events[index].note);
            auto chosen = -1;
            for (int distance = 0; distance < 128 && chosen < 0; ++distance)
            {
                for (const auto key : {wanted - distance, wanted + distance})
                {
                    if (key >= 0 && key < 128 && free(key))
                    {
                        chosen = key;
                        break;
                    }
                }
            }
            if (chosen < 0)
            {
                error = "MIDI Voice Script holds more than 128 notes at once";
                return false;
            }
            soundingUntil[static_cast<std::size_t>(chosen)].push_back(events[index + 1].frame);
            events[index].note = static_cast<std::uint8_t>(chosen);
            events[index + 1].note = static_cast<std::uint8_t>(chosen);
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return a.frame < b.frame || (a.frame == b.frame && !a.noteOn && b.noteOn);
    });
    std::stable_sort(controllers.begin(), controllers.end(), [](const auto& a, const auto& b) {
        return a.frame < b.frame;
    });
    return true;
}
}

namespace
{
namespace smalltalk = talas::project::smalltalk;

// The MIDI Script field is a Smalltalk program, whose expressions and control
// structures generate events algorithmically. A script the literal subset does
// not take runs once, as the Sound is prepared, in the bounded interpreter;
// each self keyDownAt: or self controller: message it sends is written out
// as the literal statement it stands for, its times in seconds, and the
// literal subset then checks and schedules those.
class MidiScriptStatements final : public smalltalk::Host
{
public:
    // files: the project's binary resources, which a script reads by name.
    explicit MidiScriptStatements(double sampleRate, std::span<const BinaryResource> files = {}, bool exactNumbers = true,
        talas::expr::RoundingRule rounding = talas::expr::RoundingRule::Smalltalk)
        : files_(files)
        , sampleRate_(sampleRate)
        , exactNumbers_(exactNumbers)
        , rounding_(rounding)
    {
    }

    bool exactNumbers() override { return exactNumbers_; }
    // The project's rounded and truncated.
    talas::expr::RoundingRule rounding() override { return rounding_; }

    std::string statements;

    // The project's files, as a score the script reads, by their resource
    // names.
    smalltalk::FileSystem* files() override
    {
        if (!fileSystem_.has_value())
        {
            fileSystem_.emplace();
            for (const auto& file : files_)
                fileSystem_->write(file.id, std::string(file.bytes.begin(), file.bytes.end()));
        }
        return &*fileSystem_;
    }

    std::optional<smalltalk::Value> variable(std::string_view name) override
    {
        if (name == "self")
            return smalltalk::Value::object(0);
        if (name == "MM")
            return smalltalk::Value::of(bpm_);
        // The event collections' classes.
        for (std::size_t index = 0; index < classNames.size(); ++index)
        {
            if (name == classNames[index])
                return smalltalk::Value::object(firstClass + index);
        }
        return std::nullopt;
    }

    // MM := 120 sets the beat for the times that follow.
    bool assign(std::string_view name, const smalltalk::Value& value) override
    {
        if (name != "MM")
            return false;
        const auto tempo = smalltalk::numberOf(value);
        if (tempo.kind == smalltalk::Value::Kind::Number && tempo.unit == talas::expr::Unit::None
            && std::isfinite(tempo.number) && tempo.number > 0.0)
        {
            bpm_ = tempo.number;
        }
        else
        {
            bpm_ = 0.0;
        }
        return true;
    }

    std::optional<smalltalk::Value> send(const smalltalk::Value& receiver, std::string_view selector,
        std::span<const smalltalk::Value> arguments, std::string& error) override
    {
        if (receiver.kind == smalltalk::Value::Kind::Object && receiver.index >= firstClass
            && receiver.index < firstClass + classNames.size())
        {
            return create(receiver.index - firstClass, selector, arguments, error);
        }
        if (receiver.kind == smalltalk::Value::Kind::Object && receiver.index >= firstEvent
            && receiver.index - firstEvent < events_.size())
        {
            return transform(receiver.index - firstEvent, selector, arguments, error);
        }
        if (receiver.kind != smalltalk::Value::Kind::Object || receiver.index != 0)
        {
            error = "Only self takes MIDI events in a MIDI Script";
            return std::nullopt;
        }
        // ^ self abortForTalas, as a script does that finds no file.
        if (selector == "abortForTalas")
        {
            error = "The MIDI Script stopped with abortForTalas";
            return std::nullopt;
        }
        std::vector<std::string_view> keywords;
        for (auto rest = selector; !rest.empty();)
        {
            const auto colon = rest.find(':');
            if (colon == std::string_view::npos)
                break;
            keywords.push_back(rest.substr(0, colon + 1));
            rest.remove_prefix(colon + 1);
        }
        if (keywords.size() != arguments.size() || keywords.empty())
        {
            error = "self does not understand " + std::string{selector};
            return std::nullopt;
        }
        std::string statement = "self";
        if (keywords.front() == "keyDownAt:")
        {
            for (std::size_t index = 0; index < keywords.size(); ++index)
            {
                const auto keyword = keywords[index];
                std::string literal;
                if (keyword == "keyDownAt:" || keyword == "duration:")
                    literal = time(arguments[index], error);
                else if (keyword == "frequency:")
                    literal = pitch(arguments[index], error);
                else if (keyword == "velocity:" || keyword == "timbre:")
                    literal = scalar(arguments[index], error);
                else
                    error = "self keyDownAt: does not take " + std::string{keyword};
                if (literal.empty())
                    return std::nullopt;
                statement += " " + std::string{keyword} + " " + literal;
            }
        }
        else if (keywords.front() == "controller:" && keywords.size() >= 3)
        {
            const auto& name = arguments[0];
            if (name.kind != smalltalk::Value::Kind::Expression || name.known || !name.text.starts_with('!'))
            {
                error = "self controller: takes an Event Value's name, as !Name";
                return std::nullopt;
            }
            statement += " controller: " + name.text;
            for (std::size_t index = 1; index < keywords.size(); ++index)
            {
                const auto keyword = keywords[index];
                const auto literal = keyword == "atTime:" || keyword == "byTime:" ? time(arguments[index], error)
                                                                                     : scalar(arguments[index], error);
                if (literal.empty())
                    return std::nullopt;
                statement += " " + std::string{keyword} + " " + literal;
            }
        }
        else
        {
            error = "self understands keyDownAt: and controller: in a MIDI Script";
            return std::nullopt;
        }
        if (statements.size() + statement.size() > maximumScriptStatementBytes)
        {
            error = "MIDI Voice Script exceeds the prepared event limit";
            return std::nullopt;
        }
        statements += statement + " .\n";
        return receiver;
    }

private:
    // A Note, a Rest, or a sequence, mix or timed collection of events, with
    // its duration in beats; a Note's pitch is a note number.
    struct Event
    {
        enum class Kind : std::uint8_t
        {
            Note,
            Rest,
            Sequence,
            Mix,
            Timed,
            // An EventCollection: events without times until randomized
            //.
            Generic,
        };
        Kind kind = Kind::Note;
        double beats = 0.0;
        double pitch = 60.0;
        double velocity = 1.0;
        std::vector<std::size_t> children;
        std::vector<double> starts;
        // A randomized collection lasts its totalBeats: at least.
        double atLeast = 0.0;
    };
    static constexpr std::array<std::string_view, 6> classNames{
        "Note", "Rest", "EventSequence", "EventMix", "TimedEventCollection", "EventCollection"};
    static constexpr std::size_t firstClass = 1;
    static constexpr std::size_t firstEvent = std::size_t{1} << 20;
    static constexpr std::size_t maximumScriptEvents = std::size_t{1} << 18;

    [[nodiscard]] std::optional<smalltalk::Value> keep(Event event, std::string& error)
    {
        // Notes and Rests alike in every value are one object, so that =
        // finds them alike, as a MarkovChain trained on a MIDI file's
        // events needs.
        const auto leaf = event.kind == Event::Kind::Note || event.kind == Event::Kind::Rest;
        const auto key = std::tuple{static_cast<int>(event.kind), event.beats, event.pitch, event.velocity};
        if (leaf)
        {
            if (const auto found = leaves_.find(key); found != leaves_.end())
                return smalltalk::Value::object(firstEvent + found->second);
        }
        if (events_.size() >= maximumScriptEvents)
        {
            error = "MIDI Voice Script exceeds the prepared event limit";
            return std::nullopt;
        }
        events_.push_back(std::move(event));
        if (leaf)
            leaves_.emplace(key, events_.size() - 1);
        return smalltalk::Value::object(firstEvent + events_.size() - 1);
    }

    [[nodiscard]] std::optional<std::size_t> eventOf(const smalltalk::Value& value) const
    {
        if (value.kind != smalltalk::Value::Kind::Object || value.index < firstEvent
            || value.index - firstEvent >= events_.size())
        {
            return std::nullopt;
        }
        return value.index - firstEvent;
    }

    [[nodiscard]] static std::optional<double> beatsOf(const smalltalk::Value& value, std::string& error)
    {
        const auto given = real(value, error);
        if (!given)
            return std::nullopt;
        if (given->unit != talas::expr::Unit::None && given->unit != talas::expr::Unit::Beats)
        {
            error = "A duration in beats has no units";
            return std::nullopt;
        }
        if (given->number < 0.0)
        {
            error = "A duration in beats is not negative";
            return std::nullopt;
        }
        return given->number;
    }

    // A pitch as a note number, from hz or nn.
    [[nodiscard]] static std::optional<double> noteNumberOf(const smalltalk::Value& value, std::string& error)
    {
        const auto given = real(value, error);
        if (!given)
            return std::nullopt;
        if (given->unit == talas::expr::Unit::NoteNumber)
            return given->number;
        if (given->unit == talas::expr::Unit::Hertz && given->number > 0.0)
            return 69.0 + 12.0 * std::log2(given->number / 440.0);
        error = "A pitch needs hz or nn";
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::vector<std::size_t>> eventsIn(const smalltalk::Value& value, std::string& error) const
    {
        if (value.kind != smalltalk::Value::Kind::Array)
        {
            error = "events: takes an Array of Notes, Rests and event collections";
            return std::nullopt;
        }
        std::vector<std::size_t> found;
        for (const auto& element : *value.elements)
        {
            const auto event = eventOf(element);
            if (!event)
            {
                error = "events: takes an Array of Notes, Rests and event collections";
                return std::nullopt;
            }
            found.push_back(*event);
        }
        return found;
    }

    // Note durationInBeats:frequency:velocity: in any order, Rest
    // durationInBeats:, EventSequence events:, EventMix events: and
    // TimedEventCollection startingBeats:events:.
    std::optional<smalltalk::Value> create(std::size_t type, std::string_view selector,
        std::span<const smalltalk::Value> arguments, std::string& error)
    {
        Event event;
        std::vector<std::string_view> keywords;
        for (auto rest = selector; !rest.empty();)
        {
            const auto colon = rest.find(':');
            if (colon == std::string_view::npos)
                break;
            keywords.push_back(rest.substr(0, colon + 1));
            rest.remove_prefix(colon + 1);
        }
        if (keywords.size() != arguments.size() || keywords.empty())
        {
            error = std::string{classNames[type]} + " does not understand " + std::string{selector};
            return std::nullopt;
        }
        if (type == 0 || type == 1)
        {
            event.kind = type == 0 ? Event::Kind::Note : Event::Kind::Rest;
            auto timed = false;
            for (std::size_t index = 0; index < keywords.size(); ++index)
            {
                if (keywords[index] == "durationInBeats:")
                {
                    const auto beats = beatsOf(arguments[index], error);
                    if (!beats)
                        return std::nullopt;
                    event.beats = *beats;
                    timed = true;
                }
                else if (type == 0 && keywords[index] == "frequency:")
                {
                    const auto pitch = noteNumberOf(arguments[index], error);
                    if (!pitch)
                        return std::nullopt;
                    event.pitch = *pitch;
                }
                else if (type == 0 && keywords[index] == "velocity:")
                {
                    const auto velocity = real(arguments[index], error);
                    if (!velocity)
                        return std::nullopt;
                    event.velocity = velocity->number;
                }
                else
                {
                    error = std::string{classNames[type]} + " does not understand " + std::string{selector};
                    return std::nullopt;
                }
            }
            // A tag left off takes its default: a
            // beat, middle c and a velocity of 1.
            if (!timed)
                event.beats = 1.0;
            return keep(std::move(event), error);
        }
        if ((type == 2 || type == 3 || type == 5) && selector == "events:")
        {
            auto children = eventsIn(arguments[0], error);
            if (!children)
                return std::nullopt;
            event.kind = type == 2 ? Event::Kind::Sequence : type == 3 ? Event::Kind::Mix : Event::Kind::Generic;
            event.children = std::move(*children);
            return keep(std::move(event), error);
        }
        if (type == 4 && selector == "timesAndEventsFromMIDIFile:channel:")
            return fromMidiFile(arguments[0], arguments[1], error);
        if (type == 4 && selector == "startingBeats:events:")
        {
            auto children = eventsIn(arguments[1], error);
            if (!children)
                return std::nullopt;
            if (arguments[0].kind != smalltalk::Value::Kind::Array || arguments[0].elements->size() != children->size())
            {
                error = "startingBeats: takes an Array of beats, one for each event";
                return std::nullopt;
            }
            for (const auto& start : *arguments[0].elements)
            {
                const auto beats = beatsOf(start, error);
                if (!beats)
                    return std::nullopt;
                event.starts.push_back(*beats);
            }
            event.kind = Event::Kind::Timed;
            event.children = std::move(*children);
            return keep(std::move(event), error);
        }
        error = std::string{classNames[type]} + " does not understand " + std::string{selector};
        return std::nullopt;
    }

    [[nodiscard]] double duration(std::size_t index) const
    {
        const auto& event = events_[index];
        switch (event.kind)
        {
        case Event::Kind::Note:
        case Event::Kind::Rest:
            return event.beats;
        case Event::Kind::Sequence:
        {
            auto total = 0.0;
            for (const auto child : event.children)
                total += duration(child);
            return total;
        }
        case Event::Kind::Mix:
        {
            auto longest = 0.0;
            for (const auto child : event.children)
                longest = std::max(longest, duration(child));
            return longest;
        }
        case Event::Kind::Timed:
        {
            auto end = 0.0;
            for (std::size_t child = 0; child < event.children.size(); ++child)
                end = std::max(end, event.starts[child] + duration(event.children[child]));
            return std::max(end, event.atLeast);
        }
        case Event::Kind::Generic:
            return 0.0;
        }
        return 0.0;
    }

    // A copy of an event with each Note changed by `note`, durations scaled
    // by `scale`.
    template <typename Change>
    std::optional<std::size_t> rebuilt(std::size_t index, double scale, const Change& note, std::string& error,
        std::size_t depth = 0)
    {
        if (depth > 256)
        {
            error = "MIDI Voice Script event collections nest too deeply";
            return std::nullopt;
        }
        auto event = events_[index];
        event.beats *= scale;
        for (auto& start : event.starts)
            start *= scale;
        event.atLeast *= scale;
        if (event.kind == Event::Kind::Note)
        {
            if (!note(event, error))
                return std::nullopt;
        }
        for (auto& child : event.children)
        {
            const auto copy = rebuilt(child, scale, note, error, depth + 1);
            if (!copy)
                return std::nullopt;
            child = *copy;
        }
        const auto kept = keep(std::move(event), error);
        if (!kept)
            return std::nullopt;
        return kept->index - firstEvent;
    }

    // The script's event transformations, each making a
    // new event, and playOnVoice: with onBeat: and bpm:.
    std::optional<smalltalk::Value> transform(std::size_t index, std::string_view selector,
        std::span<const smalltalk::Value> arguments, std::string& error)
    {
        const auto made = [&](std::optional<std::size_t> event) -> std::optional<smalltalk::Value> {
            if (!event)
                return std::nullopt;
            return smalltalk::Value::object(firstEvent + *event);
        };
        const auto unitless = [&](const smalltalk::Value& value) -> std::optional<double> {
            const auto given = real(value, error);
            if (!given)
                return std::nullopt;
            if (given->unit != talas::expr::Unit::None)
            {
                error = std::string{selector} + " takes a number without units";
                return std::nullopt;
            }
            return given->number;
        };
        const auto keepNote = [](Event&, std::string&) { return true; };
        if (selector == "durationInBeats" && arguments.empty())
            return smalltalk::Value::of(duration(index));
        if (selector == "dim:")
        {
            const auto factor = unitless(arguments[0]);
            if (!factor)
                return std::nullopt;
            if (*factor < 0.0)
            {
                error = "dim: takes a duration scale factor that is not negative";
                return std::nullopt;
            }
            return made(rebuilt(index, *factor, keepNote, error));
        }
        if (selector == "totalBeats:")
        {
            const auto beats = beatsOf(arguments[0], error);
            if (!beats)
                return std::nullopt;
            const auto current = duration(index);
            if (current <= 0.0)
            {
                error = "totalBeats: needs an event that lasts";
                return std::nullopt;
            }
            return made(rebuilt(index, *beats / current, keepNote, error));
        }
        if (selector == "trsp:" || selector == "freqScale:")
        {
            const auto amount = unitless(arguments[0]);
            if (!amount)
                return std::nullopt;
            if (selector == "freqScale:" && !(*amount > 0.0))
            {
                error = "freqScale: takes a positive ratio";
                return std::nullopt;
            }
            const auto steps = selector == "trsp:" ? *amount : 12.0 * std::log2(*amount);
            return made(rebuilt(index, 1.0, [steps](Event& note, std::string&) {
                note.pitch += steps;
                return true;
            }, error));
        }
        if (selector == "inv:")
        {
            const auto center = noteNumberOf(arguments[0], error);
            if (!center)
                return std::nullopt;
            return made(rebuilt(index, 1.0, [center](Event& note, std::string&) {
                note.pitch = 2.0 * *center - note.pitch;
                return true;
            }, error));
        }
        if (selector == "dbl:")
        {
            // Each Note sounds with its doubling the interval away.
            const auto interval = unitless(arguments[0]);
            if (!interval)
                return std::nullopt;
            return made(doubled(index, *interval, error));
        }
        if (selector == "retrograde")
            return made(retrograde(index, error));
        if (selector == "asMonophonicEventSequence" && arguments.empty())
            return made(monophonic(index, error));
        // An event collection's events, as an Array.
        if (selector == "events" && arguments.empty())
        {
            const auto& event = events_[index];
            if (event.kind == Event::Kind::Note || event.kind == Event::Kind::Rest)
            {
                error = "A Note or Rest has no events";
                return std::nullopt;
            }
            std::vector<smalltalk::Value> children;
            for (const auto child : event.children)
                children.push_back(smalltalk::Value::object(firstEvent + child));
            return smalltalk::Value::array(std::move(children));
        }
        // velScale: 0.75 scales each Note's velocity, at most 1.
        if (selector == "velScale:")
        {
            const auto factor = unitless(arguments[0]);
            if (!factor)
                return std::nullopt;
            if (*factor < 0.0)
            {
                error = "velScale: takes a scale that is not negative";
                return std::nullopt;
            }
            return made(rebuilt(index, 1.0, [factor](Event& note, std::string&) {
                note.velocity = std::min(1.0, note.velocity * *factor);
                return true;
            }, error));
        }
        if (selector == "playOnVoice:" || selector == "playOnVoice:bpm:" || selector == "playOnVoice:onBeat:"
            || selector == "playOnVoice:onBeat:bpm:")
        {
            if (arguments[0].kind != smalltalk::Value::Kind::Object || arguments[0].index != 0)
            {
                error = "playOnVoice: takes self, the voice the Script is in";
                return std::nullopt;
            }
            auto onBeat = 0.0;
            auto tempo = bpm_;
            for (std::size_t argument = 1; argument < arguments.size(); ++argument)
            {
                const auto given = unitless(arguments[argument]);
                if (!given)
                    return std::nullopt;
                const auto isTempo = selector.ends_with("bpm:") && argument + 1 == arguments.size();
                (isTempo ? tempo : onBeat) = *given;
            }
            if (!(tempo > 0.0) || onBeat < 0.0)
            {
                error = "playOnVoice: needs a positive bpm and a start beat that is not negative";
                return std::nullopt;
            }
            if (!play(index, onBeat, 60.0 / tempo, error))
                return std::nullopt;
            return smalltalk::Value::object(firstEvent + index);
        }
        if (selector == "randomOrder" && arguments.empty())
            return made(reordered(index, error));
        if ((selector == "randomizeUsing:totalBeats:quantizeTo:maxSpacing:" && arguments.size() == 4)
            || (selector == "randomizeTimesUsing:pickingEventsUsing:totalBeats:quantizeTo:maxSpacing:"
                && arguments.size() == 5))
        {
            const auto both = arguments.size() == 4;
            const auto total = beatsOf(arguments[both ? 1 : 2], error);
            const auto shortest = total ? beatsOf(arguments[both ? 2 : 3], error) : std::nullopt;
            const auto longest = shortest ? beatsOf(arguments[both ? 3 : 4], error) : std::nullopt;
            if (!longest)
                return std::nullopt;
            return made(randomized(index, arguments[0], arguments[both ? 0 : 1], *total, *shortest, *longest, error));
        }
        error = "An event does not understand " + std::string{selector};
        return std::nullopt;
    }

    // The next number from 0 to 1 of a Random or OneOverF the script made.
    std::optional<double> nextOf(const smalltalk::Value& stream, std::string& error)
    {
        if (stream.kind != smalltalk::Value::Kind::Object)
        {
            error = "A random stream is a Random or a OneOverF";
            return std::nullopt;
        }
        const auto drawn = sendBack(stream, "next", {}, error);
        if (!drawn)
            return std::nullopt;
        if (drawn->kind != smalltalk::Value::Kind::Number || !(drawn->number >= 0.0) || !(drawn->number <= 1.0))
        {
            error = "A random stream answers numbers from 0 to 1 to next";
            return std::nullopt;
        }
        return drawn->number;
    }

    // One of `count` choices for a number from 0 to 1.
    [[nodiscard]] static std::size_t choice(double drawn, std::size_t count)
    {
        return std::min(count - 1, static_cast<std::size_t>(drawn * static_cast<double>(count)));
    }

    [[nodiscard]] bool isCollection(std::size_t index, std::string_view selector, std::string& error) const
    {
        const auto kind = events_[index].kind;
        if (kind == Event::Kind::Note || kind == Event::Kind::Rest)
        {
            error = std::string{selector}
                + " transforms an EventSequence, EventMix, TimedEventCollection or EventCollection";
            return false;
        }
        return true;
    }

    // randomOrder: the collection's events in a random order, the
    // starting beats of a timed one kept; drawn from a stream of the
    // script's own, Random new's, that goes on from one randomOrder to the
    // next.
    std::optional<std::size_t> reordered(std::size_t index, std::string& error)
    {
        if (!isCollection(index, "randomOrder", error))
            return std::nullopt;
        auto event = events_[index];
        for (auto position = event.children.size(); position > 1; --position)
        {
            orderState_ = static_cast<std::uint32_t>((static_cast<std::uint64_t>(orderState_) * 16807u) % 2147483647u);
            const auto drawn = static_cast<double>(orderState_ - 1u) / 2147483646.0;
            std::swap(event.children[position - 1], event.children[choice(drawn, position)]);
        }
        const auto kept = keep(std::move(event), error);
        if (!kept)
            return std::nullopt;
        return kept->index - firstEvent;
    }

    // randomizeUsing:totalBeats:quantizeTo:maxSpacing: and
    // randomizeTimesUsing:pickingEventsUsing:...:
    // a TimedEventCollection of the collection's events, each start an event
    // picked at random, the first on beat 0 and each next a random whole
    // number of the shortest spacing later, up to the longest, while the
    // starts fall within the total beats it lasts. For each start the event
    // is picked before the spacing to the next is drawn.
    std::optional<std::size_t> randomized(std::size_t index, const smalltalk::Value& times,
        const smalltalk::Value& picks, double total, double shortest, double longest, std::string& error)
    {
        if (!isCollection(index, "randomizeUsing:", error))
            return std::nullopt;
        const auto& source = events_[index];
        if (source.children.empty())
        {
            error = "A randomized collection needs events to pick from";
            return std::nullopt;
        }
        if (!(shortest > 0.0) || longest < shortest)
        {
            error = "quantizeTo: takes a spacing above 0 and maxSpacing: one no shorter";
            return std::nullopt;
        }
        const auto steps = static_cast<std::size_t>(std::floor(longest / shortest + 1e-9));
        Event event;
        event.kind = Event::Kind::Timed;
        event.atLeast = total;
        const auto children = source.children;
        for (auto start = 0.0; start < total;)
        {
            if (event.children.size() >= 65536)
            {
                error = "A randomized collection holds at most 65536 events";
                return std::nullopt;
            }
            const auto picked = nextOf(picks, error);
            if (!picked)
                return std::nullopt;
            event.children.push_back(children[choice(*picked, children.size())]);
            event.starts.push_back(start);
            const auto spaced = nextOf(times, error);
            if (!spaced)
                return std::nullopt;
            start += shortest * static_cast<double>(choice(*spaced, steps) + 1);
        }
        const auto kept = keep(std::move(event), error);
        if (!kept)
            return std::nullopt;
        return kept->index - firstEvent;
    }

    std::optional<std::size_t> doubled(std::size_t index, double interval, std::string& error, std::size_t depth = 0)
    {
        if (depth > 256)
        {
            error = "MIDI Voice Script event collections nest too deeply";
            return std::nullopt;
        }
        auto event = events_[index];
        if (event.kind == Event::Kind::Note)
        {
            auto upper = event;
            upper.pitch += interval;
            const auto first = keep(event, error);
            const auto second = first ? keep(std::move(upper), error) : std::nullopt;
            if (!second)
                return std::nullopt;
            Event pair;
            pair.kind = Event::Kind::Mix;
            pair.children = {first->index - firstEvent, second->index - firstEvent};
            const auto kept = keep(std::move(pair), error);
            if (!kept)
                return std::nullopt;
            return kept->index - firstEvent;
        }
        for (auto& child : event.children)
        {
            const auto copy = doubled(child, interval, error, depth + 1);
            if (!copy)
                return std::nullopt;
            child = *copy;
        }
        const auto kept = keep(std::move(event), error);
        if (!kept)
            return std::nullopt;
        return kept->index - firstEvent;
    }

    // A sequence plays its events in the reverse order, each reversed; a
    // timed collection mirrors its start times within its duration.
    std::optional<std::size_t> retrograde(std::size_t index, std::string& error, std::size_t depth = 0)
    {
        if (depth > 256)
        {
            error = "MIDI Voice Script event collections nest too deeply";
            return std::nullopt;
        }
        auto event = events_[index];
        const auto total = duration(index);
        for (auto& child : event.children)
        {
            const auto copy = retrograde(child, error, depth + 1);
            if (!copy)
                return std::nullopt;
            child = *copy;
        }
        if (event.kind == Event::Kind::Sequence)
            std::ranges::reverse(event.children);
        if (event.kind == Event::Kind::Timed)
        {
            for (std::size_t child = 0; child < event.children.size(); ++child)
                event.starts[child] = total - event.starts[child] - duration(event.children[child]);
        }
        const auto kept = keep(std::move(event), error);
        if (!kept)
            return std::nullopt;
        return kept->index - firstEvent;
    }

    // Writes each Note out as a self keyDownAt: statement, its start and
    // duration in seconds.
    bool play(std::size_t index, double startBeat, double secondsPerBeat, std::string& error, std::size_t depth = 0)
    {
        if (depth > 256)
        {
            error = "MIDI Voice Script event collections nest too deeply";
            return false;
        }
        const auto& event = events_[index];
        switch (event.kind)
        {
        case Event::Kind::Rest:
            return true;
        case Event::Kind::Note:
        {
            if (event.beats <= 0.0)
                return true;
            const auto statement = "self keyDownAt: " + number(startBeat * secondsPerBeat) + " s duration: "
                + number(event.beats * secondsPerBeat) + " s frequency: " + number(event.pitch) + " nn velocity: "
                + number(event.velocity) + " .\n";
            if (statements.size() + statement.size() > maximumScriptStatementBytes)
            {
                error = "MIDI Voice Script exceeds the prepared event limit";
                return false;
            }
            statements += statement;
            return true;
        }
        case Event::Kind::Sequence:
        {
            auto at = startBeat;
            for (const auto child : event.children)
            {
                if (!play(child, at, secondsPerBeat, error, depth + 1))
                    return false;
                at += duration(child);
            }
            return true;
        }
        case Event::Kind::Mix:
            for (const auto child : event.children)
            {
                if (!play(child, startBeat, secondsPerBeat, error, depth + 1))
                    return false;
            }
            return true;
        case Event::Kind::Timed:
            for (std::size_t child = 0; child < event.children.size(); ++child)
            {
                if (!play(event.children[child], startBeat + event.starts[child], secondsPerBeat, error, depth + 1))
                    return false;
            }
            return true;
        case Event::Kind::Generic:
            error = "An EventCollection has no times until randomizeUsing:totalBeats:quantizeTo:maxSpacing: gives it "
                    "some";
            return false;
        }
        return true;
    }

    std::vector<Event> events_;
    std::map<std::tuple<int, double, double, double>, std::size_t> leaves_;
    std::uint32_t orderState_ = 1u;
    std::span<const BinaryResource> files_;
    std::optional<smalltalk::MemoryFileSystem> fileSystem_;

    // TimedEventCollection timesAndEventsFromMIDIFile: 'omino.mid' channel: 1
    //: the Notes of one channel of a MIDI file the project
    // holds, by its name or the last part of its name, each at its starting
    // beat, a beat the file's quarter note, velocities from 0 to 1. A note
    // the file leaves on ends with the file's last event.
    std::optional<smalltalk::Value> fromMidiFile(
        const smalltalk::Value& name, const smalltalk::Value& channel, std::string& error)
    {
        if (name.kind != smalltalk::Value::Kind::String && name.kind != smalltalk::Value::Kind::Symbol)
        {
            error = "timesAndEventsFromMIDIFile: takes a file's name";
            return std::nullopt;
        }
        const auto number = real(channel, error);
        if (!number || number->unit != talas::expr::Unit::None || number->number < 1.0 || number->number > 16.0
            || number->number != std::floor(number->number))
        {
            error = "channel: takes a MIDI channel from 1 to 16";
            return std::nullopt;
        }
        const auto file = std::ranges::find_if(files_, [&](const BinaryResource& resource) {
            return resource.id == name.text || resource.id.ends_with("/" + name.text);
        });
        if (file == files_.end())
        {
            error = "The project has no MIDI file named " + name.text;
            return std::nullopt;
        }
        // A beat in frames that most divisions divide.
        constexpr double framesPerBeat = 1612800.0;
        const auto midiChannel = static_cast<std::uint8_t>(number->number);
        const auto read = parseStandardMidiFileChannelEventsByBeat(
            file->bytes, framesPerBeat, midiChannel, midiChannel, maximumScriptEvents, error);
        if (!read)
            return std::nullopt;
        struct Sounding
        {
            std::size_t frame = 0;
            double velocity = 0.0;
        };
        std::array<std::vector<Sounding>, 128> sounding;
        std::vector<std::tuple<std::size_t, std::size_t, std::uint8_t, double>> notes;
        for (const auto& message : *read)
        {
            if (message.bytes.size() != 3)
                continue;
            const auto status = message.bytes[0] & 0xf0U;
            const auto key = message.bytes[1] & 0x7fU;
            if (status == 0x90U && message.bytes[2] > 0)
            {
                sounding[key].push_back({message.frame, static_cast<double>(message.bytes[2]) / 127.0});
            }
            else if ((status == 0x80U || status == 0x90U) && !sounding[key].empty())
            {
                const auto on = sounding[key].front();
                sounding[key].erase(sounding[key].begin());
                if (message.frame > on.frame)
                    notes.emplace_back(on.frame, message.frame, static_cast<std::uint8_t>(key), on.velocity);
            }
        }
        const auto last = read->empty() ? std::size_t{0} : read->back().frame;
        for (std::size_t key = 0; key < sounding.size(); ++key)
        {
            for (const auto& on : sounding[key])
            {
                if (last > on.frame)
                    notes.emplace_back(on.frame, last, static_cast<std::uint8_t>(key), on.velocity);
            }
        }
        std::ranges::sort(notes);
        Event collection;
        collection.kind = Event::Kind::Timed;
        for (const auto& [on, off, key, velocity] : notes)
        {
            Event note;
            note.beats = static_cast<double>(off - on) / framesPerBeat;
            note.pitch = key;
            note.velocity = velocity;
            const auto kept = keep(std::move(note), error);
            if (!kept)
                return std::nullopt;
            collection.children.push_back(kept->index - firstEvent);
            collection.starts.push_back(static_cast<double>(on) / framesPerBeat);
        }
        return keep(std::move(collection), error);
    }

    // Each Note an event plays, with its starting beat.
    bool notesOf(std::size_t index, double start, std::vector<std::pair<double, std::size_t>>& notes, std::string& error,
        std::size_t depth = 0) const
    {
        if (depth > 256)
        {
            error = "MIDI Voice Script event collections nest too deeply";
            return false;
        }
        const auto& event = events_[index];
        switch (event.kind)
        {
        case Event::Kind::Note:
            notes.emplace_back(start, index);
            return true;
        case Event::Kind::Rest:
            return true;
        case Event::Kind::Sequence:
        {
            auto at = start;
            for (const auto child : event.children)
            {
                if (!notesOf(child, at, notes, error, depth + 1))
                    return false;
                at += duration(child);
            }
            return true;
        }
        case Event::Kind::Timed:
            for (std::size_t child = 0; child < event.children.size(); ++child)
            {
                if (!notesOf(event.children[child], start + event.starts[child], notes, error, depth + 1))
                    return false;
            }
            return true;
        case Event::Kind::Mix:
        case Event::Kind::Generic:
            for (const auto child : event.children)
            {
                if (!notesOf(child, start, notes, error, depth + 1))
                    return false;
            }
            return true;
        }
        return true;
    }

    // asMonophonicEventSequence: an EventSequence of the
    // event's Notes, one at a time. Of the Notes that start together the
    // highest plays; each lasts until the next one starts at most, and a
    // Rest fills the time between one's end and the next one's start.
    std::optional<std::size_t> monophonic(std::size_t index, std::string& error)
    {
        std::vector<std::pair<double, std::size_t>> notes;
        if (!notesOf(index, 0.0, notes, error))
            return std::nullopt;
        std::ranges::stable_sort(notes, [&](const auto& left, const auto& right) {
            return left.first != right.first ? left.first < right.first
                                             : events_[left.second].pitch > events_[right.second].pitch;
        });
        std::vector<std::pair<double, std::size_t>> heard;
        for (const auto& note : notes)
        {
            if (heard.empty() || note.first > heard.back().first)
                heard.push_back(note);
        }
        Event sequence;
        sequence.kind = Event::Kind::Sequence;
        auto cursor = 0.0;
        for (std::size_t position = 0; position < heard.size(); ++position)
        {
            const auto [start, note] = heard[position];
            if (start > cursor)
            {
                Event rest;
                rest.kind = Event::Kind::Rest;
                rest.beats = start - cursor;
                const auto kept = keep(std::move(rest), error);
                if (!kept)
                    return std::nullopt;
                sequence.children.push_back(kept->index - firstEvent);
            }
            auto played = events_[note];
            if (position + 1 < heard.size())
                played.beats = std::min(played.beats, heard[position + 1].first - start);
            cursor = start + played.beats;
            const auto kept = keep(std::move(played), error);
            if (!kept)
                return std::nullopt;
            sequence.children.push_back(kept->index - firstEvent);
        }
        const auto kept = keep(std::move(sequence), error);
        if (!kept)
            return std::nullopt;
        return kept->index - firstEvent;
    }

    [[nodiscard]] static std::string number(double value)
    {
        std::array<char, 64> buffer{};
        const auto [end, failed] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        return failed == std::errc{} ? std::string(buffer.data(), end) : std::string{};
    }

    // All arguments are real values, as opposed to Event Values.
    [[nodiscard]] static std::optional<smalltalk::Value> real(const smalltalk::Value& value, std::string& error)
    {
        const auto known = smalltalk::numberOf(value);
        if (known.kind != smalltalk::Value::Kind::Number || !std::isfinite(known.number))
        {
            error = "MIDI Script arguments must be real values, not Event Values";
            return std::nullopt;
        }
        return known;
    }

    [[nodiscard]] std::string time(const smalltalk::Value& value, std::string& error) const
    {
        const auto given = real(value, error);
        if (!given)
            return {};
        auto seconds = given->number;
        switch (given->unit)
        {
        case talas::expr::Unit::Seconds:
            break;
        case talas::expr::Unit::Milliseconds:
            seconds /= 1000.0;
            break;
        case talas::expr::Unit::Microseconds:
            seconds /= 1'000'000.0;
            break;
        case talas::expr::Unit::Minutes:
            seconds *= 60.0;
            break;
        case talas::expr::Unit::Hours:
            seconds *= 3600.0;
            break;
        case talas::expr::Unit::Samples:
            seconds /= sampleRate_;
            break;
        case talas::expr::Unit::Beats:
            if (!(bpm_ > 0.0))
            {
                error = "MIDI Voice Script has an invalid MM tempo declaration";
                return {};
            }
            seconds *= 60.0 / bpm_;
            break;
        default:
            error = "MIDI Script times need units: s, ms, beats or samp";
            return {};
        }
        const auto text = number(seconds);
        return text.empty() ? text : text + " s";
    }

    [[nodiscard]] static std::string pitch(const smalltalk::Value& value, std::string& error)
    {
        const auto given = real(value, error);
        if (!given)
            return {};
        if (given->unit != talas::expr::Unit::NoteNumber && given->unit != talas::expr::Unit::Hertz)
        {
            error = "MIDI Script frequencies need units: hz or nn";
            return {};
        }
        const auto text = number(given->number);
        return text.empty() ? text : text + (given->unit == talas::expr::Unit::Hertz ? " hz" : " nn");
    }

    [[nodiscard]] static std::string scalar(const smalltalk::Value& value, std::string& error)
    {
        const auto given = real(value, error);
        if (!given)
            return {};
        if (given->unit != talas::expr::Unit::None)
        {
            error = "MIDI Script levels are numbers without units";
            return {};
        }
        return number(given->number);
    }

    double sampleRate_ = 48'000.0;
    double bpm_ = 60.0;
    bool exactNumbers_ = true;
    talas::expr::RoundingRule rounding_ = talas::expr::RoundingRule::Smalltalk;
};

// The literal subset first; any other script runs in the interpreter.
[[nodiscard]] bool parseMidiScript(
    std::string_view source, double sampleRate, std::size_t maximumEvents, double defaultPitch,
    std::span<const BinaryResource> files, bool exactNumbers, talas::expr::RoundingRule rounding,
    std::vector<PreparedMidiScriptNoteEvent>& events,
    std::vector<PreparedMidiScriptControllerEvent>& controllers,
    std::string& error)
{
    if (parseLiteralMidiScript(source, sampleRate, maximumEvents, defaultPitch, events, controllers, error))
        return true;
    const auto literalError = error;
    events.clear();
    controllers.clear();
    if (source.empty() || source.size() > 65536 || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return false;
    const auto parsed = smalltalk::parse(source);
    if (!parsed.ok())
    {
        error = literalError;
        return false;
    }
    // A script neither form takes names both failures.
    const auto failed = [&](std::string why) {
        events.clear();
        controllers.clear();
        error = literalError + " (as a Smalltalk program: " + why + ")";
        return false;
    };
    MidiScriptStatements host(sampleRate, files, exactNumbers, rounding);
    const auto ran = smalltalk::run(*parsed.program, host);
    if (!ran.ok())
        return failed(ran.diagnostic ? ran.diagnostic->message : std::string{"the script failed"});
    if (host.statements.empty())
        return failed("MIDI Voice Script must schedule at least one event");
    std::string scheduled;
    if (!parseLiteralMidiScript(host.statements, sampleRate, maximumEvents, defaultPitch, events, controllers, scheduled,
            maximumScriptStatementBytes))
        return failed(scheduled);
    error.clear();
    return true;
}
}

std::optional<PreparedMidiInputSource> parsePreparedMidiInputSource(std::string_view text) noexcept
{
    return talas::core::parseMidiInputSource(text);
}

bool hasPreparedAnalogSequencerKeyOwner(const CompileStep& step) noexcept
{
    if (step.typeId != "talas.analogSequencer") return false;
    // A literal Polyphony above 1 plays note events on that many voices
    //, as hasProjectAnalogSequencerKeySource decides from its text.
    const auto polyphony = std::ranges::find(step.parameters, std::string_view{"polyphony"},
        &ParameterValue::id);
    if (polyphony != step.parameters.end() && polyphony->expression
        && polyphony->expression->instructionCount() == 1
        && polyphony->expression->eventReferences().empty()
        && polyphony->expression->variableReferences().empty()
        && polyphony->expression->identifiers().empty()
        && !polyphony->expression->usesTime()
        && polyphony->variableCellIndices.empty()
        && std::isfinite(polyphony->value) && polyphony->value > 1.0)
        return true;
    return std::ranges::any_of(step.parameters, [](const auto& value) {
        return (value.id == "keyDowns" || value.id == "pitches")
            && value.text && hasNonBlankAscii(*value.text);
    });
}

std::optional<std::uint64_t>
PreparedMidiFileVoiceSources::Owner::analogExtraFrameAt(std::size_t cursor) const noexcept
{
    if ((analogExtraSequences.empty() && analogGeneratedIndex.empty()) || analogKeyStages.empty()
        || (!loop && cursor >= analogKeyStages.size())) return {};
    const auto cycle = cursor / analogKeyStages.size();
    const auto local = analogKeyStages[cursor % analogKeyStages.size()].frame;
    if (cycleFrames == 0 || cycle > (std::numeric_limits<std::uint64_t>::max() - local) / cycleFrames)
        return {};
    return cycle * cycleFrames + local;
}

std::optional<std::size_t>
PreparedMidiFileVoiceSources::Owner::analogExtraCursorAtOrAfter(std::uint64_t frame) const noexcept
{
    if (analogExtraSequences.empty() && analogGeneratedIndex.empty()) return 0;
    if (analogKeyStages.empty() || cycleFrames == 0) return {};
    auto cycle = loop ? frame / cycleFrames : 0;
    const auto local = loop ? frame % cycleFrames : frame;
    auto index = static_cast<std::size_t>(std::distance(analogKeyStages.begin(),
        std::ranges::lower_bound(analogKeyStages, local, {}, &PreparedAnalogSequencerKeyStage::frame)));
    if (loop && index == analogKeyStages.size()) { ++cycle; index = 0; }
    if (cycle > (std::numeric_limits<std::size_t>::max() - index) / analogKeyStages.size()) return {};
    return cycle * analogKeyStages.size() + index;
}

std::optional<std::vector<double>> prepareAnalogSequencerConstantArray(
    std::string_view text, const RenderSettings& settings, talas::expr::ValueDomain domain)
{
    std::string error;
    auto sequence = parseSequencerArray(text, settings, domain, globalRuntimeScope, error);
    if (!sequence || std::ranges::any_of(sequence->expressions, [](const auto& expression) {
            return !expression.eventReferences().empty() || !expression.variableReferences().empty()
                || expression.isStateful() || expression.usesTime();
        })) return {};
    return std::move(sequence->values);
}

std::optional<std::vector<PreparedAnalogSequencerSequence>>
prepareAnalogSequencerExtraValues(std::string_view text,
    const RenderSettings& settings, std::string& error, RuntimeScopeId expressionScope)
{
    error.clear();
    const auto arrays = talas::core::parseAnalogSequencerExtraValues(text);
    if (!arrays)
    {
        error = "Analog Sequencer ExtraValues requires up to 32 distinct non-keyboard named arrays with 1 to 256 finite members";
        return {};
    }
    std::vector<PreparedAnalogSequencerSequence> result;
    std::vector<talas::expr::ExpressionValue> initialVariables;
    initialVariables.reserve(settings.preparedVariables.cells.size());
    for (const auto& cell : settings.preparedVariables.cells)
        initialVariables.push_back({cell.initialValue, cell.domain});
    for (const auto& array : *arrays)
    {
        PreparedAnalogSequencerSequence sequence{array.name, {}};
        sequence.expressionScope = expressionScope;
        for (const auto& source : array.expressions)
        {
            const auto parsed = talas::expr::parse(source, settings.expressionLanguage, settings.expressionDefinitions);
            if (!parsed.ok() || !parsed.expression->identifiers().empty()
                || (parsed.expression->isStateful() && !parsed.expression->usesTime()))
            {
                error = "Analog Sequencer ExtraValues requires stateless or clocked scalar expressions without runtime identifiers";
                return {};
            }
            if (!bindSequencerVariables(sequence, *parsed.expression, settings.preparedVariables, error))
                return {};
            sequence.expressions.push_back(*parsed.expression);
            double value = 0;
            talas::expr::ExpressionState state;
            if (!talas::expr::prepareExpressionState(*parsed.expression, state)
                || !evaluateAnalogSequencerValue(sequence, sequence.expressions.size() - 1,
                    settings.eventValues, settings.preparedVariables, settings.sampleRate, settings.tempoBpm,
                    value, initialVariables, &state))
            {
                error = "Analog Sequencer ExtraValues member must be a finite scalar";
                return {};
            }
            sequence.values.push_back(value);
        }
        result.push_back(std::move(sequence));
    }
    return result;
}

bool PreparedAnalogSequencerSequence::operator==(
    const PreparedAnalogSequencerSequence& other) const noexcept
{
    if (name != other.name || values != other.values || expressionScope != other.expressionScope
        || domain != other.domain || variableCellIndices != other.variableCellIndices
        || expressions.size() != other.expressions.size()) return false;
    for (std::size_t index = 0; index < expressions.size(); ++index)
        if (!expressions[index].structurallyEquivalent(other.expressions[index])) return false;
    return true;
}

bool evaluateAnalogSequencerValue(
    const PreparedAnalogSequencerSequence& sequence, std::size_t index,
    const std::vector<EventValueBinding>& bindings,
    const talas::project::PreparedProjectVariableTable& scopes,
    double sampleRate, double tempoBpm, double& value,
    std::span<const talas::expr::ExpressionValue> variableValues,
    talas::expr::ExpressionState* state, std::uint64_t elapsedSampleFrame,
    AnalogSequencerVoiceValues voiceValues) noexcept
{
    if (sequence.expressions.empty())
    {
        if (sequence.values.empty() || !sequence.variableCellIndices.empty()) return false;
        value = sequence.values[std::min(index, sequence.values.size() - 1)];
        return std::isfinite(value);
    }
    if (sequence.variableCellIndices.size() != sequence.expressions.size()) return false;
    const auto member = std::min(index, sequence.expressions.size() - 1);
    const auto& indices = sequence.variableCellIndices[member];
    if (indices.size() != sequence.expressions[member].variableReferences().size()
        || (!indices.empty() && variableValues.size() != scopes.cells.size())) return false;
    struct Resolver {
        const std::vector<EventValueBinding>& bindings;
        const talas::project::PreparedProjectVariableTable& scopes;
        RuntimeScopeId scope;
        const std::vector<std::uint32_t>& indices;
        std::span<const talas::expr::ExpressionValue> variables;
        AnalogSequencerVoiceValues voices;
    } resolver{bindings, scopes, sequence.expressionScope, indices, variableValues, voiceValues};
    talas::expr::EvaluationContext context;
    context.sampleRate = sampleRate;
    context.tempoBpm = tempoBpm;
    context.elapsedSampleFrame = elapsedSampleFrame;
    context.user = &resolver;
    context.resolveEvent = [](void* user, std::string_view name, talas::expr::ExpressionValue& result) noexcept {
        const auto& state = *static_cast<Resolver*>(user);
        if (state.voices.resolve != nullptr && state.voices.resolve(state.voices.user, state.scope, name, result))
            return true;
        const auto binding = detail::findNearestEventValue(state.bindings, state.scopes, state.scope, name);
        if (binding == state.bindings.end()) return false;
        result = {binding->value, binding->domain};
        return true;
    };
    context.resolveVariableIndex = [](void* user, std::size_t reference,
                                      talas::expr::ExpressionValue& result) noexcept {
        const auto& state = *static_cast<Resolver*>(user);
        if (reference >= state.indices.size()) return false;
        const auto cell = state.indices[reference];
        if (cell >= state.variables.size() || cell >= state.scopes.cells.size()
            || state.variables[cell].domain != state.scopes.cells[cell].domain) return false;
        result = state.variables[cell];
        return std::isfinite(result.value);
    };
    talas::expr::ExpressionValue result;
    const auto& expression = sequence.expressions[member];
    const auto evaluated = expression.isStateful()
        ? state != nullptr && talas::expr::tryEvaluateStateful(expression,
            context, *state, {sequence.domain}, result)
        : talas::expr::tryEvaluate(expression, context, {sequence.domain}, result);
    if (!evaluated || !std::isfinite(result.value)) return false;
    value = result.value;
    return true;
}

std::optional<std::uint64_t>
PreparedMidiFileVoiceSources::Owner::scriptFrameAt(
    std::size_t cursor) const noexcept
{
    if (source != PreparedMidiInputSource::Script
        || scriptEvents.empty()) return std::nullopt;
    if (!loop)
    {
        if (cursor >= scriptEvents.size()) return std::nullopt;
        return scriptEvents[cursor].frame;
    }
    if (cycleFrames == 0) return std::nullopt;
    const auto cycle = cursor / scriptEvents.size();
    const auto index = cursor % scriptEvents.size();
    const auto local = scriptEvents[index].frame;
    if (cycle > (std::numeric_limits<std::uint64_t>::max()
            - local) / cycleFrames)
        return std::nullopt;
    return static_cast<std::uint64_t>(cycle) * cycleFrames + local;
}

std::optional<std::size_t>
PreparedMidiFileVoiceSources::Owner::scriptCursorAtOrAfter(
    std::uint64_t frame) const noexcept
{
    if (source != PreparedMidiInputSource::Script)
        return std::nullopt;
    const auto lower = [&](std::uint64_t value) {
        return static_cast<std::size_t>(std::distance(
            scriptEvents.begin(),
            std::ranges::lower_bound(
                scriptEvents, value, {},
                &PreparedMidiScriptNoteEvent::frame)));
    };
    if (!loop)
        return lower(frame);
    if (scriptEvents.empty()) return std::size_t{0};
    if (cycleFrames == 0) return std::nullopt;
    auto cycle = frame / cycleFrames;
    const auto local = frame % cycleFrames;
    if (local == 0 && cycle != 0)
    {
        const auto previousEnd = lower(cycleFrames);
        if (previousEnd < scriptEvents.size()
            && scriptEvents[previousEnd].frame == cycleFrames)
        {
            const auto previousCycle = cycle - 1;
            if (previousCycle
                > (std::numeric_limits<std::size_t>::max()
                    - previousEnd) / scriptEvents.size())
                return std::nullopt;
            return static_cast<std::size_t>(previousCycle)
                * scriptEvents.size() + previousEnd;
        }
    }
    auto index = lower(local);
    if (index == scriptEvents.size())
    {
        if (cycle == std::numeric_limits<std::uint64_t>::max())
            return std::nullopt;
        ++cycle;
        index = 0;
    }
    if (cycle > (std::numeric_limits<std::size_t>::max()
            - index) / scriptEvents.size())
        return std::nullopt;
    return static_cast<std::size_t>(cycle) * scriptEvents.size()
        + index;
}

std::optional<PreparedAnalogSequencerKeyStage>
makeAnalogSequencerKeyStage(std::span<const double, 6> values,
    double sampleRate, double rate) noexcept
{
    const auto [duration, duty, down, pitch, velocity, timbre] =
        std::array{values[0], values[1], values[2], values[3], values[4], values[5]};
    const auto frames = std::round(duration * sampleRate / std::abs(rate));
    if (!std::isfinite(sampleRate) || sampleRate <= 0 || !std::isfinite(rate) || rate == 0
        || !std::isfinite(duration) || duration <= 0
        || !std::isfinite(frames) || frames < 1
        || frames >= static_cast<double>(std::numeric_limits<std::size_t>::max())
        || !std::isfinite(duty) || duty < 0 || duty > 1 || (down != 0 && down != 1)
        || !std::isfinite(pitch) || pitch < 0 || pitch > 127
        || !std::isfinite(velocity) || velocity < 0 || velocity > 1
        || !std::isfinite(timbre) || timbre < 0 || timbre > 1) return {};
    PreparedAnalogSequencerKeyStage stage;
    stage.durationFrames = static_cast<std::size_t>(frames);
    stage.durationSeconds = duration;
    stage.dutyCycle = duty;
    if (down == 1 && duty > 0)
    {
        stage.gateFrames = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::round(static_cast<double>(stage.durationFrames) * duty)),
            1, stage.durationFrames);
        stage.attack = PreparedMidiScriptNoteEvent{0, true,
            static_cast<std::uint8_t>(std::lround(pitch)),
            static_cast<std::uint8_t>(std::max<long>(1, std::lround(velocity * 127))),
            pitch, velocity, timbre};
    }
    return stage;
}

void tieFullDutyCycles(PreparedAnalogSequencerKeySchedule& schedule, bool loop)
{
    // A stage with a DutyCycle of 1 never lets its key up: the next
    // stage's note takes over its voice, and only a rest or the Gate lets it
    // go.
    const auto stageCount = schedule.stages.size();
    for (std::size_t index = 0; index < stageCount; ++index)
    {
        const auto& stage = schedule.stages[index];
        if (!stage.attack || stage.dutyCycle < 1.0)
            continue;
        const auto next = index + 1 < stageCount ? std::optional{index + 1}
            : loop ? std::optional<std::size_t>{0} : std::nullopt;
        if (next && !schedule.stages[*next].attack)
            continue;
        const auto release = std::ranges::find_if(schedule.events, [&](const auto& event) {
            return !event.noteOn && event.note == stage.attack->note
                && event.frame == stage.frame + stage.gateFrames;
        });
        if (release != schedule.events.end())
            schedule.events.erase(release);
        if (!next)
            continue;
        const auto& following = schedule.stages[*next];
        const auto attack = std::ranges::find_if(schedule.events, [&](const auto& event) {
            return event.noteOn && event.frame == following.frame
                && event.note == following.attack->note;
        });
        if (attack != schedule.events.end())
            attack->legatoFrom = stage.attack->note;
    }
}

std::optional<PreparedAnalogSequencerKeySchedule>
prepareAnalogSequencerKeySchedule(
    std::span<const double> durationsSeconds,
    std::span<const double> dutyCycles,
    std::span<const double> keyDowns,
    std::span<const double> keyPitches,
    std::span<const double> keyVelocities,
    std::span<const double> keyTimbres,
    std::size_t startIndex,
    std::size_t endIndex,
    double sampleRate,
    double rate,
    std::size_t maximumEvents,
    std::string& error)
{
    error.clear();
    const auto length = std::max({durationsSeconds.size(), dutyCycles.size(),
        keyDowns.size(), keyPitches.size(), keyVelocities.size(),
        keyTimbres.size()});
    if (length == 0 || length > 256 || durationsSeconds.empty()
        || dutyCycles.empty() || keyDowns.empty() || keyPitches.empty()
        || keyVelocities.empty() || keyTimbres.empty()
        || startIndex > endIndex
        || endIndex >= length || !std::isfinite(sampleRate)
        || sampleRate <= 0.0 || !std::isfinite(rate) || rate == 0.0)
    {
        error = "Analog Sequencer key arrays require nonempty lengths up to 256, a valid selected range, sample rate and non-zero Rate";
        return std::nullopt;
    }

    PreparedAnalogSequencerKeySchedule result;
    result.events.reserve(std::min(maximumEvents, (endIndex - startIndex + 1) * 2));
    result.stages.reserve(endIndex - startIndex + 1);
    for (std::size_t offset = 0; offset <= endIndex - startIndex; ++offset)
    {
        const auto index = rate > 0.0
            ? startIndex + offset : endIndex - offset;
        const auto duration = durationsSeconds[
            std::min(index, durationsSeconds.size() - 1)];
        const auto duty = dutyCycles[std::min(index, dutyCycles.size() - 1)];
        const auto down = keyDowns[std::min(index, keyDowns.size() - 1)];
        const auto pitch = keyPitches[std::min(index, keyPitches.size() - 1)];
        const auto velocity = keyVelocities[
            std::min(index, keyVelocities.size() - 1)];
        const auto timbre = keyTimbres[std::min(index, keyTimbres.size() - 1)];
        auto stage = makeAnalogSequencerKeyStage(
            std::array{duration, duty, down, pitch, velocity, timbre}, sampleRate, rate);
        if (!stage)
        {
            error = "Analog Sequencer key stage has an invalid duration, gate, pitch, velocity or timbre";
            return std::nullopt;
        }
        if (stage->durationFrames
            > std::numeric_limits<std::size_t>::max() - result.cycleFrames)
        {
            error = "Analog Sequencer key cycle exceeds the sample-frame range";
            return std::nullopt;
        }
        stage->frame = result.cycleFrames;
        stage->sourceIndex = index;
        if (stage->attack)
        {
            if (result.events.size() > maximumEvents
                || maximumEvents - result.events.size() < 2)
            {
                error = "Analog Sequencer key cycle exceeds the prepared event limit";
                return std::nullopt;
            }
            const auto& attack = *stage->attack;
            result.events.push_back({result.cycleFrames, true,
                attack.note, attack.allocationVelocity, pitch, velocity, timbre});
            result.events.push_back({result.cycleFrames + stage->gateFrames, false,
                attack.note, 0, pitch, velocity, timbre});
        }
        result.cycleFrames += stage->durationFrames;
        result.stages.push_back(std::move(*stage));
    }
    std::stable_sort(result.events.begin(), result.events.end(),
        [](const auto& left, const auto& right) {
            return left.frame < right.frame;
        });
    return result;
}

namespace
{
bool waitUntilCanDelayVoiceOwner(const RuntimePlan& plan, std::size_t ownerStepIndex);
bool appendOwnerControllerChanges(const PreparedMidiFileVoiceSources::Owner& owner,
    std::size_t ownerIndex, const RuntimePlan& plan, const RenderSettings& settings,
    const std::vector<EventValueBinding>& bindings,
    std::vector<EventValueChange>& changes, std::string& error);
}

std::unique_ptr<const PreparedMidiFileVoiceSources> PreparedMidiFileVoiceSources::prepare(
    const RuntimePlan& plan, const RenderSettings& settings,
    PreparedMidiFileVoiceSourceLimits limits,
    std::string& error)
{
    error.clear();
    std::unique_ptr<PreparedMidiFileVoiceSources> prepared(new PreparedMidiFileVoiceSources);
    prepared->owners_.reserve(plan.midiVoices.size());
    std::vector<std::uint8_t> claimedSteps(plan.steps.size(), 0);
    std::map<std::size_t, std::size_t> simultaneousCounts;
    std::size_t loopedSimultaneousPeak = 0;
    auto variableReferenceUses = std::size_t{0};
    const auto chargeVariableReferences = [&](std::size_t count) {
        constexpr auto maximum = talas::project::maximumPreparedProjectVariableReferenceUses;
        if (count > maximum - std::min(variableReferenceUses, maximum)) return false;
        variableReferenceUses += count;
        return true;
    };
    // Ordinary parameters/projections already own part of the shared budget.
    // Preserve their existing admission when no embedded references are added.
    for (const auto& step : plan.steps)
        for (const auto& parameter : step.parameters)
            if (!chargeVariableReferences(parameter.variableCellIndices.size()))
                variableReferenceUses = talas::project::maximumPreparedProjectVariableReferenceUses;
    for (const auto& binding : settings.eventValues)
        if (!chargeVariableReferences(binding.variableCellIndices.size()))
            variableReferenceUses = talas::project::maximumPreparedProjectVariableReferenceUses;
    for (const auto& midiPlan : plan.midiVoices)
    {
        if (midiPlan.ownerStepIndex >= plan.steps.size()
            || claimedSteps[midiPlan.ownerStepIndex] != 0
            || !isPreparedInputVoiceOwner(
                plan.steps[midiPlan.ownerStepIndex])
            || midiPlan.configuration.channel < 1 || midiPlan.configuration.channel > 16)
        {
            error = "MIDI Voice source has an invalid or duplicate owner";
            return {};
        }
        claimedSteps[midiPlan.ownerStepIndex] = 1;
        const auto& step = plan.steps[midiPlan.ownerStepIndex];
        const auto parameter = [&](std::string_view id) -> const ParameterValue* {
            const ParameterValue* result = nullptr;
            for (const auto& value : step.parameters)
            {
                if (value.id != id) continue;
                if (result != nullptr) return nullptr;
                result = &value;
            }
            return result;
        };
        Owner owner;
        owner.stepIndex = midiPlan.ownerStepIndex;
        owner.nodeId = midiPlan.ownerNodeId;
        owner.definitionId = midiPlan.ownerDefinitionId;
        owner.instancePath = midiPlan.ownerInstancePath;
        owner.parentVoiceScopeId = midiPlan.parentVoiceScopeId;
        owner.voiceAncestors = midiPlan.voiceAncestors;
        owner.channel = midiPlan.configuration.channel;
        if (hasPreparedAnalogSequencerKeyOwner(step))
        {
            const auto blank = [](const ParameterValue* value) {
                return value != nullptr && value->text
                    && !hasNonBlankAscii(*value->text);
            };
            const auto* pitches = parameter("pitches");
            const auto* velocities = parameter("velocities");
            // A Polyphony above 1 without Pitches or KeyDowns sends each
            // stage as a note of 60 nn, and empty Velocities at 1, as the
            // sequence path plays them at full amplitude.
            const auto unpitched = !(pitches != nullptr && pitches->text
                    && hasNonBlankAscii(*pitches->text))
                && blank(parameter("keyDowns"));
            const auto classic = unpitched
                || (pitches != nullptr && pitches->text
                    && hasNonBlankAscii(*pitches->text));
            if ((classic && (!blank(parameter("keyDowns"))
                    || !blank(parameter("keyPitches"))
                    || !blank(parameter("keyVelocities"))
                    || !blank(parameter("keyTimbres"))))
                || (!classic && (!blank(pitches) || !blank(velocities))))
            {
                error = "Analog Sequencer cannot mix classic Pitches/Velocities and Key arrays: "
                    + step.nodeId;
                return {};
            }
            std::array<std::string_view, 6> arrayIds{
                "durations", "dutyCycles", "keyDowns", "keyPitches",
                "keyVelocities", "keyTimbres"};
            if (classic)
            {
                arrayIds[3] = "pitches";
                arrayIds[4] = "velocities";
            }
            constexpr std::array<talas::expr::ValueDomain, 6> domains{
                talas::expr::ValueDomain::Seconds,
                talas::expr::ValueDomain::Scalar,
                talas::expr::ValueDomain::Scalar,
                talas::expr::ValueDomain::NoteNumber,
                talas::expr::ValueDomain::Scalar,
                talas::expr::ValueDomain::Scalar};
            std::array<std::vector<double>, 6> arrays;
            for (std::size_t index = 0; index < arrayIds.size(); ++index)
            {
                if (classic && (index == 2 || index == 5))
                {
                    arrays[index] = {index == 2 ? 1.0 : 0.0};
                    owner.analogKeySequences[index].values = arrays[index];
                    continue;
                }
                if (classic && (index == 3 || index == 4)
                    && blank(parameter(arrayIds[index]))
                    && (unpitched || index == 4))
                {
                    arrays[index] = {index == 3 ? 60.0 : 1.0};
                    owner.analogKeySequences[index].values = arrays[index];
                    continue;
                }
                const auto* authored = parameter(arrayIds[index]);
                if (authored == nullptr || !authored->text)
                {
                    error = "Analog Sequencer is missing the "
                        + std::string(arrayIds[index]) + " array: " + step.nodeId;
                    return {};
                }
                auto parsed = parseSequencerArray(
                    *authored->text, settings, domains[index],
                    owner.parentVoiceScopeId ? step.eventValueScopeId : step.scopeId, error);
                if (!parsed)
                {
                    error = std::string(arrayIds[index]) + ": " + error
                        + ": " + step.nodeId;
                    return {};
                }
                arrays[index] = parsed->values;
                // A live stage has no fixed timeline at preparation. These
                // finite placeholders only establish its selected indices and
                // worst-case event capacity; callback sampling owns its values.
                constexpr std::array<double, 6> placeholder{1, 1, 1, 60, 1, 0};
                for (std::size_t member = 0; member < parsed->expressions.size(); ++member)
                    if (!parsed->expressions[member].eventReferences().empty()
                        || !parsed->expressions[member].variableReferences().empty()
                        || parsed->expressions[member].usesTime())
                    {
                        owner.analogLiveArrays = true;
                        arrays[index][member] = placeholder[index];
                    }
                owner.analogKeySequences[index] = std::move(*parsed);
            }
            const auto* start = parameter("startIndex");
            const auto* end = parameter("endIndex");
            const auto* indexOrigin = parameter("indexOrigin");
            const auto* loop = parameter("loop");
            const auto* gate = parameter("gate");
            const auto* manualStep = parameter("step");
            const auto* stepZeroMode = parameter("stepZeroMode");
            const auto* rate = parameter("rate");
            const auto* extraValues = parameter("extraValues");
            auto extra = extraValues != nullptr && extraValues->text
                ? prepareAnalogSequencerExtraValues(*extraValues->text, settings, error,
                    owner.parentVoiceScopeId ? step.eventValueScopeId : step.scopeId)
                : std::nullopt;
            if (!extra)
            {
                if (error.empty()) error = "Analog Sequencer is missing ExtraValues";
                return {};
            }
            owner.analogExtraSequences = std::move(*extra);
            const auto chargeSequence = [&](const PreparedAnalogSequencerSequence& sequence) {
                for (const auto& indices : sequence.variableCellIndices)
                    if (!chargeVariableReferences(indices.size())) return false;
                return true;
            };
            if (!std::ranges::all_of(owner.analogKeySequences, chargeSequence)
                || !std::ranges::all_of(owner.analogExtraSequences, chargeSequence))
            {
                error = "Analog Sequencer exceeds the prepared variable reference limit: " + step.nodeId;
                return {};
            }
            if (const auto* generated = parameter("generatedIndex"))
            {
                const auto name = generated->text
                    ? talas::core::parseAnalogSequencerGeneratedIndex(*generated->text) : std::nullopt;
                if (!name || std::ranges::any_of(owner.analogExtraSequences,
                        [&](const auto& sequence) { return sequence.name == *name; }))
                {
                    error = "Analog Sequencer GeneratedIndex requires a distinct non-keyboard EventValue target: " + step.nodeId;
                    return {};
                }
                owner.analogGeneratedIndex = *name;
            }
            // Extra arrays participate in the same repeat-final length rule.
            for (const auto& sequence : owner.analogExtraSequences)
                if (sequence.values.size() > arrays[0].size())
                    arrays[0].resize(sequence.values.size(), arrays[0].back());
            const auto staticScalar = [](const ParameterValue* value,
                                         double expected) {
                return value != nullptr && value->value == expected
                    && std::isfinite(value->value)
                    && value->variableCellIndices.empty()
                    && (!value->expression
                        || (value->expression->eventReferences().empty()
                            && value->expression->identifiers().empty()
                            && value->expression->stateSlotCount() == 0
                            && !value->expression->usesTime()));
            };
            const auto finiteScalar = [](const ParameterValue* value) {
                return value != nullptr && std::isfinite(value->value)
                    && value->domain == talas::expr::ValueDomain::Scalar
                    && !value->choice && !value->boolean
                    && !value->resource && !value->text;
            };
            const auto hasLiveScalarClock = [](const ParameterValue* value) {
                if (value == nullptr || !value->expression) return false;
                const auto& expression = *value->expression;
                return !expression.eventReferences().empty()
                    || !expression.variableReferences().empty()
                    || expression.usesTime();
            };
            const auto hasUnclockedDynamicFeatures = [](const ParameterValue* value) {
                if (value == nullptr) return false;
                if (!value->variableCellIndices.empty()
                    && (!value->expression
                        || value->expression->variableReferences().empty()))
                    return true;
                if (!value->expression) return false;
                const auto& expression = *value->expression;
                return !expression.identifiers().empty()
                    || expression.stateSlotCount() != 0
                    || expression.isStateful()
                    || expression.usesVoiceNumber();
            };
            const auto connectedRateCount = std::ranges::count(
                step.inputs, std::string_view{"rate"}, &InputBinding::inputPortId);
            if (connectedRateCount > 1)
            {
                error = "Analog Sequencer key Rate requires at most one exact input: "
                    + step.nodeId;
                return {};
            }
            const auto hasRateClock = hasLiveScalarClock(rate);
            if (hasUnclockedDynamicFeatures(rate) && !hasRateClock)
            {
                error = "Analog Sequencer live Rate requires an EventValue/variable/time clock for stateful or unresolved expressions: "
                    + step.nodeId;
                return {};
            }
            owner.analogLiveRate = connectedRateCount == 1 || hasRateClock;
            for (const auto* bound : {start, end})
            {
                const auto hasClock = hasLiveScalarClock(bound);
                if (hasUnclockedDynamicFeatures(bound) && !hasClock)
                {
                    error = "Analog Sequencer live indices require an EventValue/variable/time clock for stateful or unresolved expressions: "
                        + step.nodeId;
                    return {};
                }
                owner.analogLiveRange |= hasClock;
            }
            if (stepZeroMode != nullptr
                && (stepZeroMode->domain != talas::expr::ValueDomain::Scalar
                    || !stepZeroMode->choice
                    || (*stepZeroMode->choice != "waitForTrigger"
                        && *stepZeroMode->choice != "legacyAutomatic")
                    || stepZeroMode->expression || stepZeroMode->boolean
                    || stepZeroMode->resource || stepZeroMode->text
                    || !stepZeroMode->variableCellIndices.empty()))
            {
                error = "Analog Sequencer requires an exact Step zero compatibility choice: "
                    + step.nodeId;
                return {};
            }
            const auto legacyZeroStep = stepZeroMode != nullptr
                && *stepZeroMode->choice == "legacyAutomatic";
            const auto* dutyCycleOne = parameter("dutyCycleOne");
            if (dutyCycleOne != nullptr
                && (dutyCycleOne->domain != talas::expr::ValueDomain::Scalar
                    || !dutyCycleOne->choice
                    || (*dutyCycleOne->choice != "holds"
                        && *dutyCycleOne->choice != "legacyRetriggers")
                    || dutyCycleOne->expression || dutyCycleOne->boolean
                    || dutyCycleOne->resource || dutyCycleOne->text
                    || !dutyCycleOne->variableCellIndices.empty()))
            {
                error = "Analog Sequencer requires an exact DutyCycle 1 compatibility choice: "
                    + step.nodeId;
                return {};
            }
            // Every compiled AnalogSequencer names the choice; a hand-made
            // step without it keeps the former retrigger.
            owner.analogKeyLegato = dutyCycleOne != nullptr
                && *dutyCycleOne->choice == "holds";
            if (start == nullptr || end == nullptr || loop == nullptr
                || indexOrigin == nullptr || !indexOrigin->choice.has_value()
                || indexOrigin->domain != talas::expr::ValueDomain::Scalar
                || indexOrigin->expression.has_value()
                || indexOrigin->boolean.has_value()
                || indexOrigin->resource.has_value()
                || indexOrigin->text.has_value()
                || !indexOrigin->variableCellIndices.empty()
                || !finiteScalar(start)
                || !finiteScalar(end)
                || std::trunc(start->value) != start->value
                || std::trunc(end->value) != end->value
                || !loop->boolean
                || !finiteScalar(gate)
                || !finiteScalar(manualStep) || manualStep->value < 0.0
                || !finiteScalar(rate) || rate->value == 0.0)
            {
                error = "Analog Sequencer key source requires a finite selected range, finite scalar gate, non-zero initial scalar Rate and non-negative scalar step: "
                    + step.nodeId;
                return {};
            }
            const auto stageCount = std::max({arrays[0].size(),
                arrays[1].size(), arrays[2].size(), arrays[3].size(),
                arrays[4].size(), arrays[5].size()});
            const auto indices = detail::normalizeAnalogSequencerIndices(
                start->value, end->value, stageCount,
                *indexOrigin->choice);
            if (!indices)
            {
                error = "Analog Sequencer key source requires selected indices inside the sequence: "
                    + step.nodeId;
                return {};
            }
            owner.analogLegacyRange = *indexOrigin->choice == "legacyOneBased";
            // A live range can later select any stage, including one outside
            // the initial pair. Reserve the complete canonical stage set.
            const auto firstStage = owner.analogLiveRange ? 0 : indices->first;
            const auto lastStage = owner.analogLiveRange ? stageCount - 1 : indices->second;
            std::vector<double> clockedDurations;
            std::span<const double> scheduleDurations{arrays[0]};
            if (owner.hasAnalogDynamicClock())
            {
                const auto placeholderDuration =
                    std::isfinite(settings.sampleRate)
                        && settings.sampleRate > 0.0
                    ? 1.0 / settings.sampleRate
                    : 1.0;
                clockedDurations.assign(
                    arrays[0].size(), placeholderDuration);
                for (std::size_t index = firstStage;
                     index <= lastStage;
                     ++index)
                {
                    const auto duration = arrays[0][
                        std::min(index, arrays[0].size() - 1)];
                    if (!std::isfinite(duration) || duration <= 0.0)
                    {
                        error = "Analog Sequencer key stage has an invalid duration, gate, pitch, velocity or timbre";
                        return {};
                    }
                }
                scheduleDurations =
                    std::span<const double>{clockedDurations};
            }
            // Dynamic traversal stays canonical. Placeholder durations bound
            // preparation; the callback samples real durations at each stage.
            auto schedule = prepareAnalogSequencerKeySchedule(
                scheduleDurations, arrays[1], arrays[2], arrays[3], arrays[4], arrays[5],
                firstStage, lastStage,
                settings.sampleRate, owner.hasAnalogDynamicClock() ? 1.0 : rate->value,
                limits.maximumTotalEvents - prepared->eventCount_, error);
            if (!schedule)
            {
                error += ": " + step.nodeId;
                return {};
            }
            owner.source = PreparedMidiInputSource::Script;
            owner.analogKeyRate = rate->value;
            owner.loop = *loop->boolean;
            owner.cycleFrames = schedule->cycleFrames;
            if (owner.analogKeyLegato)
                tieFullDutyCycles(*schedule, owner.loop);
            owner.scriptEvents = std::move(schedule->events);
            owner.analogKeyStages = std::move(schedule->stages);
            if (owner.hasAnalogDynamicClock())
            {
                for (auto& stage : owner.analogKeyStages)
                    stage.durationSeconds = arrays[0][
                        std::min(stage.sourceIndex, arrays[0].size() - 1)];
            }
            const auto extraOutputCount = owner.analogExtraSequences.size()
                + static_cast<std::size_t>(!owner.analogGeneratedIndex.empty());
            const auto extraEventCount = owner.analogKeyStages.size() * extraOutputCount;
            const auto noteEventCount = (owner.analogLiveArrays || owner.hasAnalogDynamicClock())
                ? owner.analogKeyStages.size() * 2 : owner.scriptEvents.size();
            if (extraEventCount > limits.maximumTotalEvents - prepared->eventCount_
                || noteEventCount > limits.maximumTotalEvents - prepared->eventCount_ - extraEventCount)
            {
                error = "Analog Sequencer ExtraValues exceeds the prepared event limit";
                return {};
            }
            // L1 documents constant one as automatic. Only migrated projects
            // retain the former local constant-zero alias.
            owner.analogKeyStepTriggered = std::ranges::any_of(step.inputs,
                [](const auto& input) { return input.inputPortId == "step"; })
                || (!staticScalar(manualStep, 1.0)
                    && !(legacyZeroStep && staticScalar(manualStep, 0.0)));
            // Gate can restart even a non-looping cycle at an arbitrary frame.
            // Reserve every owner's peak independently of its original offset.
            {
                std::map<std::size_t, std::size_t> localCounts;
                std::size_t localPeak = 0;
                for (const auto& event : owner.scriptEvents)
                {
                    auto& count = localCounts[owner.loop
                        ? event.frame % owner.cycleFrames : event.frame];
                    ++count;
                    localPeak = std::max(localPeak, count);
                }
                // A Gate falling edge emits at most one held-note release.
                // It replaces, rather than joins, the disabled cycle's events.
                if (owner.analogLiveArrays || owner.hasAnalogDynamicClock())
                    localPeak = std::max<std::size_t>(localPeak, 2);
                localPeak += extraOutputCount;
                if (localPeak > limits.maximumSimultaneousEvents
                    - loopedSimultaneousPeak)
                {
                    error = "Analog Sequencer key events exceed the aggregate simultaneous event limit";
                    return {};
                }
                loopedSimultaneousPeak += localPeak;
            }
            prepared->eventCount_ += noteEventCount + extraEventCount;
            prepared->hasScheduledSource_ = true;
            prepared->owners_.push_back(std::move(owner));
            continue;
        }
        const auto* sourceParameter = parameter("source");
        const auto source = sourceParameter != nullptr && sourceParameter->text
            ? parsePreparedMidiInputSource(*sourceParameter->text)
            : std::nullopt;
        if (!source)
        {
            error = "MIDI Voice owner has a missing, ambiguous or unknown source: " + step.nodeId;
            return {};
        }
        owner.source = *source;
        if (*source == PreparedMidiInputSource::Live)
        {
            // Inactive source fields are preserved authored data, not inputs.
            prepared->owners_.push_back(std::move(owner));
            continue;
        }
        prepared->hasScheduledSource_ = true;
        if (*source == PreparedMidiInputSource::Script)
        {
            const auto* script = parameter("script");
            const auto* lowPitch = parameter("lowPitch");
            const auto* notePitch = parameter("scriptNotePitch");
            const auto defaultPitch = (notePitch != nullptr && notePitch->choice == std::optional<std::string>{"legacyNoteZero"})
                    || lowPitch == nullptr || !std::isfinite(lowPitch->value)
                ? 0.0
                : lowPitch->value;
            if (script == nullptr || !script->text
                || !parseMidiScript(*script->text, settings.sampleRate,
                    limits.maximumTotalEvents - prepared->eventCount_, defaultPitch, settings.binaryResources,
                    !settings.smalltalkNumbersAreFloats, settings.expressionDefinitions.rounding,
                    owner.scriptEvents, owner.scriptControllers, error))
            {
                if (error.empty()) error = "MIDI Voice Script source requires script text";
                error += ": " + step.nodeId;
                return {};
            }
            for (const auto& event : owner.scriptEvents)
            {
                auto& count = simultaneousCounts[event.frame];
                if (count >= limits.maximumSimultaneousEvents)
                {
                    error = "MIDI Voice sources exceed the aggregate simultaneous event limit";
                    return {};
                }
                ++count;
            }
            for (const auto& event : owner.scriptControllers)
            {
                auto& count = simultaneousCounts[event.frame];
                if (count >= limits.maximumSimultaneousEvents)
                {
                    error = "MIDI Voice sources exceed the aggregate simultaneous event limit";
                    return {};
                }
                ++count;
            }
            prepared->eventCount_ += owner.scriptEvents.size()
                + owner.scriptControllers.size();
            prepared->owners_.push_back(std::move(owner));
            continue;
        }

        const auto* fileParameter = parameter("midiFile");
        if (fileParameter == nullptr || !fileParameter->resource || fileParameter->resource->empty())
        {
            error = "MIDI Voice MIDI File source requires one nonempty midiFile resource: " + step.nodeId;
            return {};
        }
        const auto& resourceId = *fileParameter->resource;
        const talas::core::ResourceSnapshot* snapshot = nullptr;
        const BinaryResource* compatibility = nullptr;
        std::size_t snapshotCount = 0;
        std::size_t compatibilityCount = 0;
        for (const auto& candidate : settings.resourceSnapshots)
            if (candidate && candidate->id == resourceId)
            {
                snapshot = candidate.get();
                ++snapshotCount;
            }
        for (const auto& candidate : settings.binaryResources)
            if (candidate.id == resourceId)
            {
                compatibility = &candidate;
                ++compatibilityCount;
            }
        const auto exactGeneration = snapshot != nullptr
            && (snapshot->generation == 0 ? snapshot->contentHash.empty()
                : snapshot->contentHash.size() == 64 && std::ranges::all_of(
                    snapshot->contentHash, [](char value) {
                        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                    }));
        if (snapshotCount != 1 || compatibilityCount != 1 || !exactGeneration
            || snapshot->kind != talas::core::ResourceSnapshotKind::Binary
            || snapshot->binaryBytes != compatibility->bytes
            || !snapshot->tableSamples.empty() || !snapshot->audioBytes.empty()
            || !snapshot->tableRightSamples.empty()
            || !snapshot->partialTrackBytes.empty())
        {
            error = "MIDI Voice requires one exact immutable binary resource: " + resourceId;
            return {};
        }
        auto events = parseStandardMidiFileChannelEvents(snapshot->binaryBytes,
            settings.sampleRate, std::numeric_limits<std::size_t>::max(),
            owner.channel, owner.channel, limits.maximumTotalEvents - prepared->eventCount_, error);
        if (!events) return {};
        std::array<bool, 17> extraChannels{};
        for (const auto& [channel, name] : taggedControllerSources(step))
            if (channel != owner.channel) extraChannels[channel] = true;
        for (std::uint8_t channel = 1; channel <= 16; ++channel)
        {
            if (!extraChannels[channel]) continue;
            auto taggedEvents = parseStandardMidiFileChannelEvents(
                snapshot->binaryBytes, settings.sampleRate,
                std::numeric_limits<std::size_t>::max(), channel, channel,
                limits.maximumTotalEvents - prepared->eventCount_
                    - events->size(), error);
            if (!taggedEvents) return {};
            for (auto& event : *taggedEvents)
                if (event.bytes.size() == 3
                    && (event.bytes[0] & 0xf0U) == 0xb0U)
                    events->push_back(std::move(event));
        }
        std::stable_sort(events->begin(), events->end(),
            [](const MidiEvent& left, const MidiEvent& right) {
                return left.frame < right.frame;
            });
        // Format 2 represents independent patterns. Echo's historical merge
        // is not an instrument-source selection policy.
        if (snapshot->binaryBytes[8] == 0 && snapshot->binaryBytes[9] == 2)
        {
            error = "MIDI Voice format 2 independent patterns require explicit pattern selection";
            return {};
        }
        for (const auto& event : *events)
        {
            auto& count = simultaneousCounts[event.frame];
            if (count >= limits.maximumSimultaneousEvents)
            {
                error = "MIDI Voice file sources exceed the aggregate simultaneous event limit";
                return {};
            }
            ++count;
        }
        // And the file's MIDI timing clock, for a
        // Sound that reads it; one that does not fit the prepared events is
        // left out, the file playing as before.
        if (std::ranges::any_of(settings.eventValues, [](const EventValueBinding& binding) {
                return binding.scopeId == globalRuntimeScope
                    && binding.name == talas::expr::midiTimingClockCountEventName;
            }))
        {
            std::string clockError;
            auto clock = parseStandardMidiFileTimingClockFrames(snapshot->binaryBytes, settings.sampleRate,
                limits.maximumTotalEvents - prepared->eventCount_ - events->size(), clockError);
            if (clock)
                owner.timingClockFrames = std::move(*clock);
        }
        owner.resourceId = resourceId;
        owner.resourceGeneration = snapshot->generation;
        owner.resourceContentHash = snapshot->contentHash;
        owner.events = std::move(*events);
        prepared->eventCount_ += owner.events.size();
        prepared->hasFileSource_ = true;
        prepared->owners_.push_back(std::move(owner));
    }
    for (const auto& [frame, count] : simultaneousCounts)
    {
        (void)frame;
        if (count > limits.maximumSimultaneousEvents
            - loopedSimultaneousPeak)
        {
            error = "Scheduled voice sources exceed the aggregate simultaneous event limit";
            return {};
        }
    }
    std::size_t controllerCount = 0;
    for (std::size_t ownerIndex = 0; ownerIndex < prepared->owners_.size(); ++ownerIndex)
    {
        auto& owner = prepared->owners_[ownerIndex];
        owner.activationControlled = owner.source != PreparedMidiInputSource::Live
            && (owner.parentVoiceScopeId.has_value()
                || waitUntilCanDelayVoiceOwner(plan, owner.stepIndex));
        if (!owner.activationControlled) continue;
        std::vector<EventValueChange> changes;
        if (!appendOwnerControllerChanges(owner, ownerIndex, plan, settings, settings.eventValues, changes, error)) return {};
        if (changes.size() > settings.maxRealtimeEventTransactions
                - std::min(controllerCount, settings.maxRealtimeEventTransactions))
        {
            error = "Activated MIDI Voice controllers exceed prepared transaction capacity";
            return {};
        }
        controllerCount += changes.size();
        std::stable_sort(changes.begin(), changes.end(), [](const auto& a, const auto& b) {
            return a.frame < b.frame;
        });
        for (const auto& change : changes)
        {
            const auto binding = std::ranges::find_if(settings.eventValues, [&](const auto& value) {
                return value.scopeId == change.scopeId && value.name == change.name;
            });
            if (binding == settings.eventValues.end()) return {};
            const auto bindingIndex = static_cast<std::size_t>(binding - settings.eventValues.begin());
            for (auto previous = owner.activationControllers.rbegin();
                 previous != owner.activationControllers.rend() && previous->frame == change.frame; ++previous)
                if (previous->bindingIndex == bindingIndex)
                {
                    error = "MIDI Voice controller conflicts with another Event Value at one frame: " + change.name;
                    return {};
                }
            owner.activationControllers.push_back({change.frame, bindingIndex, change.value});
        }
    }
    return prepared;
}

bool hasPreparedMidiFileVoiceSource(const CompileStep& step) noexcept
{
    return isPreparedInputVoiceOwner(step) && std::ranges::any_of(step.parameters,
        [](const ParameterValue& value) {
            return value.id == "source" && value.text
                && parsePreparedMidiInputSource(*value.text) == PreparedMidiInputSource::File;
        });
}

bool hasPreparedMidiFileVoiceSource(const RuntimePlan& plan) noexcept
{
    return std::ranges::any_of(plan.steps, [](const CompileStep& step) {
        return hasPreparedMidiFileVoiceSource(step);
    });
}

bool hasPreparedMidiScriptVoiceSource(const CompileStep& step) noexcept
{
    return isPreparedInputVoiceOwner(step) && std::ranges::any_of(step.parameters,
        [](const ParameterValue& value) {
            return value.id == "source" && value.text
                && parsePreparedMidiInputSource(*value.text) == PreparedMidiInputSource::Script;
        });
}

bool hasPreparedMidiScheduledVoiceSource(const RuntimePlan& plan) noexcept
{
    return std::ranges::any_of(plan.steps, [](const CompileStep& step) {
        return hasPreparedMidiFileVoiceSource(step)
            || hasPreparedMidiScriptVoiceSource(step)
            || hasPreparedAnalogSequencerKeyOwner(step);
    });
}

bool hasPreparedAnalogSequencerGeneratedIndex(const RuntimePlan& plan) noexcept
{
    return std::ranges::any_of(plan.steps, [](const CompileStep& step) {
        return step.typeId == "talas.analogSequencer"
            && std::ranges::any_of(step.parameters, [](const auto& parameter) {
                return parameter.id == "generatedIndex" && parameter.text
                    && hasNonBlankAscii(*parameter.text);
            });
    });
}

namespace
{
bool waitUntilCanDelayVoiceOwner(const RuntimePlan& plan, std::size_t ownerStepIndex)
{
    std::vector<std::uint8_t> visited(plan.steps.size());
    std::vector<std::size_t> pending;
    pending.reserve(plan.steps.size());
    const auto pushBuffer = [&](std::size_t bufferIndex) {
        if (bufferIndex >= plan.buffers.size()) return false;
        const auto& buffer = plan.buffers[bufferIndex];
        std::optional<std::size_t> producer;
        for (std::size_t index = 0; index < plan.steps.size(); ++index)
            if (plan.steps[index].nodeId == buffer.nodeId && plan.steps[index].scopeId == buffer.scopeId)
            {
                if (producer) return false;
                producer = index;
            }
        if (!producer) return false;
        if (!visited[*producer])
        {
            visited[*producer] = 1;
            pending.push_back(*producer);
        }
        return true;
    };
    for (const auto& gate : plan.steps)
    {
        if (gate.typeId != "talas.waitUntil") continue;
        const auto resumeInput = std::ranges::find(gate.inputs, "resume", &InputBinding::inputPortId);
        const auto resume = std::ranges::find(gate.parameters, "resume", &ParameterValue::id);
        const auto onZero = std::ranges::find(gate.parameters, "resumeOnZero", &ParameterValue::id);
        if (resumeInput == gate.inputs.end() && resume != gate.parameters.end()
            && std::isfinite(resume->value) && onZero != gate.parameters.end() && onZero->boolean
            && (*onZero->boolean ? resume->value == 0 : resume->value != 0)) continue;
        const auto input = std::ranges::find(gate.inputs, "audioIn", &InputBinding::inputPortId);
        if (input == gate.inputs.end()) return true;
        std::ranges::fill(visited, 0);
        pending.clear();
        if (!pushBuffer(input->bufferIndex)) return true;
        while (!pending.empty())
        {
            const auto index = pending.back(); pending.pop_back();
            if (index == ownerStepIndex) return true;
            for (const auto& dependency : plan.steps[index].inputs)
                if (!pushBuffer(dependency.bufferIndex)) return true;
            // Lowered voice mixers retain their audio Input as per-voice
            // output buffers, rather than ordinary input bindings.
            for (const auto& owner : plan.midiVoices)
                if (owner.ownerStepIndex == index)
                    for (const auto& voice : owner.voices)
                    {
                        if (!pushBuffer(voice.audioOutputBufferIndex)) return true;
                        if (voice.rightAudioOutputBufferIndex
                            && !pushBuffer(*voice.rightAudioOutputBufferIndex)) return true;
                    }
            // A feedback commit is activated by its read even though the
            // read/commit dependency is not an ordinary processor input edge.
            for (const auto& pair : plan.feedbackPairs)
                if (pair.outputStepIndex == index && !pushBuffer(pair.inputBufferIndex)) return true;
        }
    }
    return false;
}
}

bool PreparedMidiFileVoiceSources::validateNotePlayback(
    const RuntimePlan& /*plan*/, const RenderSettings& settings, std::string& error) const
{
    error.clear();
    if (!hasScheduledSource_) return true;
    if (settings.transportLoop)
    {
        error = "MIDI Voice scheduled source looping requires prepared cursor restart integration";
        return false;
    }
    if (hasFileSource_ && settings.preparedScore)
    {
        error = "MIDI File Voice Score clocks and looping require their prepared clock integration";
        return false;
    }
    for (const auto& owner : owners_)
        for (const auto& event : owner.events)
        {
            const auto status = event.bytes.empty()
                ? 0U : event.bytes[0] & 0xf0U;
            if ((event.bytes.size() == 3
                    && (status == 0x80U || status == 0x90U
                        || status == 0xb0U || status == 0xe0U))
                || (event.bytes.size() == 2
                    && (status == 0xc0U || status == 0xd0U)))
                continue;
            error = "MIDI File Voice channel message requires prepared mapping: "
                + owner.nodeId;
            return false;
        }
    return true;
}

namespace
{
bool appendOwnerControllerChanges(const PreparedMidiFileVoiceSources::Owner& owner,
    std::size_t ownerIndex, const RuntimePlan& plan, const RenderSettings& settings,
    const std::vector<EventValueBinding>& bindings,
    std::vector<EventValueChange>& changes, std::string& error)
{
    if (ownerIndex >= plan.midiVoices.size() || owner.stepIndex >= plan.steps.size())
    {
        error = "MIDI Voice controller owner is missing";
        return false;
    }
    const auto appendController = [&](std::size_t frame,
                                      std::string_view name,
                                      double value,
                                      bool required,
                                      bool midiSource) {
        std::vector<const EventValueBinding*> localBindings;
        const auto& voices = plan.midiVoices[ownerIndex].voices;
        const auto mapperInput = owner.stepIndex < plan.steps.size()
            && plan.steps[owner.stepIndex].typeId == "talas.midiMapper";
        if (mapperInput || owner.parentVoiceScopeId)
            for (const auto& voice : voices)
            {
                const auto binding = std::ranges::find_if(bindings,
                    [&](const EventValueBinding& candidate) {
                        return candidate.scopeId == voice.scopeId
                            && candidate.name == name;
                    });
                if (binding != bindings.end())
                    localBindings.push_back(std::addressof(*binding));
            }
        const auto outer = owner.parentVoiceScopeId
            ? detail::findNearestEventValue(bindings, settings.preparedVariables,
                plan.steps[owner.stepIndex].eventValueScopeId, name)
            : std::ranges::find_if(bindings,
            [&](const EventValueBinding& candidate) {
                return candidate.scopeId == globalRuntimeScope
                    && candidate.name == name;
            });
        if (!localBindings.empty() && localBindings.size() != voices.size())
        {
            error = "MIDI Voice controller has mixed local and global Event Value scopes: "
                + std::string(name);
            return false;
        }
        if (localBindings.empty() && outer == bindings.end())
        {
            if (!required) return true;
            error = "MIDI Voice Script controller has no Event Value binding: "
                + std::string(name);
            return false;
        }
        const auto append = [&](const EventValueBinding& binding) {
            if (binding.projectionExpression.has_value())
            {
                error = "MIDI Voice controller cannot assign a projected Event Value: "
                    + std::string(name);
                return false;
            }
            if (midiSource && binding.domain != talas::expr::ValueDomain::Scalar)
            {
                error = "MIDI File control source requires a scalar Event Value: "
                    + std::string(name);
                return false;
            }
            changes.push_back({frame, std::string(name),
                value, binding.domain,
                talas::core::AutomationInterpolation::Step, binding.scopeId});
            return true;
        };
        if (localBindings.empty())
        {
            if (!append(*outer)) return false;
        }
        else
        {
            for (const auto* binding : localBindings)
                if (!append(*binding)) return false;
        }
        return true;
    };
    // ADR-0024: a file's MIDI also sets the global map's names, in the
    // scalar Event Values the project leaves to it, each on its channel (the
    // default one without its own), within its range by its taper and grid.
    const auto appendGlobalMapNames = [&](std::size_t frame, std::uint8_t channel, bool pitchBend,
                                          std::uint8_t controller, double normalized) {
        for (const auto& entry : settings.globalMap)
        {
            if (entry.pitchBend != pitchBend || (!pitchBend && entry.controller != controller)
                || entry.channel.value_or(settings.defaultMidiChannel) != channel
                || std::ranges::any_of(bindings, [&](const EventValueBinding& binding) {
                       return binding.name == entry.eventName && binding.domain != talas::expr::ValueDomain::Scalar;
                   }))
                continue;
            const auto value = talas::project::globalMapValue(entry, normalized);
            if (value.has_value() && !appendController(frame, entry.eventName, *value, false, true))
                return false;
        }
        return true;
    };
    if (owner.source == PreparedMidiInputSource::Script)
        for (const auto& controller : owner.scriptControllers)
            if (!appendController(controller.frame, controller.name,
                controller.value, true, false)) return false;
    if (owner.source == PreparedMidiInputSource::File)
    {
        const auto taggedSources = taggedControllerSources(
            plan.steps[owner.stepIndex]);
        for (const auto& event : owner.events)
        {
            if (event.bytes.size() == 2
                && ((event.bytes[0] & 0xf0U) == 0xc0U
                    || (event.bytes[0] & 0xf0U) == 0xd0U))
            {
                const auto channel = static_cast<std::uint8_t>(
                    (event.bytes[0] & 0x0fU) + 1U);
                const auto program = (event.bytes[0] & 0xf0U) == 0xc0U;
                if (channel == owner.channel
                    && channel == settings.defaultMidiChannel
                    && !appendController(event.frame,
                        program ? "MIDIProgramNumber"
                            : "MIDIChannelPressure",
                        program
                            ? static_cast<double>(event.bytes[1]) + 1.0
                            : static_cast<double>(event.bytes[1]) / 127.0,
                        false, true)) return false;
                continue;
            }
            if (event.bytes.size() == 3
                && (event.bytes[0] & 0xf0U) == 0xe0U)
            {
                const auto channel = static_cast<std::uint8_t>(
                    (event.bytes[0] & 0x0fU) + 1U);
                const auto raw = static_cast<int>(event.bytes[1])
                    | (static_cast<int>(event.bytes[2]) << 7);
                if (channel == owner.channel
                    && channel == settings.defaultMidiChannel
                    && !appendController(event.frame, "MIDIPitchBend",
                            normalizeMidiPitchBend14(raw), false, true))
                    return false;
                if (channel == owner.channel
                    && !appendGlobalMapNames(event.frame, channel, true, 0, normalizeMidiPitchBend14(raw)))
                    return false;
                continue;
            }
            if (event.bytes.size() != 3
                || (event.bytes[0] & 0xf0U) != 0xb0U) continue;
            const auto channel = static_cast<std::uint8_t>(
                (event.bytes[0] & 0x0fU) + 1U);
            std::string name = "cc";
            if (event.bytes[1] < 10) name += '0';
            name += std::to_string(event.bytes[1]);
            const auto value =
                static_cast<double>(event.bytes[2]) / 127.0;
            if (channel == owner.channel
                && channel == settings.defaultMidiChannel)
            {
                if (!appendController(event.frame, name, value,
                        false, true)) return false;
                const auto binary = event.bytes[2] >= 64 ? 1.0 : 0.0;
                name.replace(0, 2, "bt");
                if (!appendController(event.frame, name, binary,
                        false, true)) return false;
                name.replace(0, 2, "sw");
                if (!appendController(event.frame, name, binary,
                        false, true)) return false;
                name.replace(0, 2, "cc");
            }
            if (channel == owner.channel
                && !appendGlobalMapNames(event.frame, channel, false, event.bytes[1], value))
                return false;
            if (taggedSources.empty()) continue;
            name += "_ch";
            if (channel < 10) name += '0';
            name += std::to_string(channel);
            if (std::ranges::any_of(taggedSources,
                    [&](const auto& tagged) {
                        return tagged.first == channel
                            && tagged.second == name;
                    })
                && !appendController(event.frame, name, value,
                    false, true)) return false;
        }
    }
    return true;
}
}

bool PreparedMidiFileVoiceSources::appendScheduledControllerChanges(
    const RuntimePlan& plan, RenderSettings& settings, std::string& error) const
{
    error.clear();
    // Publish bindings and controller changes together only after validation.
    auto bindings = settings.eventValues;
    // The first preparation resolves names against the containing environment.
    // Give each nested source its own writable Input bindings before the final
    // runtime preparation binds controller indices. All of that source's Input
    // voices share its controller stream, while sibling owners and globals keep
    // their values. Parent reuse resets these bindings with the child region.
    for (std::size_t ownerIndex = 0; ownerIndex < owners_.size(); ++ownerIndex)
    {
        const auto& owner = owners_[ownerIndex];
        if (!owner.parentVoiceScopeId || !owner.activationControlled) continue;
        std::vector<EventValueChange> sourceChanges;
        if (!appendOwnerControllerChanges(owner, ownerIndex, plan, settings, bindings, sourceChanges, error)) return false;
        std::vector<std::string> names;
        for (const auto& controller : sourceChanges)
        {
            const auto binding = std::ranges::find_if(bindings, [&](const auto& candidate) {
                return candidate.scopeId == controller.scopeId && candidate.name == controller.name;
            });
            if (binding == bindings.end())
            {
                error = "Nested MIDI Voice controller binding is missing";
                return false;
            }
            const auto source = *binding;
            if (std::ranges::find(names, source.name) != names.end()) continue;
            names.push_back(source.name);
            for (const auto& voice : plan.midiVoices[ownerIndex].voices)
            {
                if (std::ranges::none_of(bindings, [&](const auto& binding) {
                        return binding.scopeId == voice.scopeId && binding.name == source.name;
                    }))
                    bindings.push_back({source.name, source.value, source.domain, voice.scopeId});
                // Local Map projections read the newly isolated MIDI source.
                if (plan.steps[owner.stepIndex].typeId == "talas.midiMapper")
                    for (auto& binding : bindings)
                        if (binding.scopeId == voice.scopeId
                            && binding.sourceScopeId == globalRuntimeScope
                            && binding.projectionExpression
                            && std::ranges::find(binding.projectionExpression->eventReferences(), source.name)
                                != binding.projectionExpression->eventReferences().end())
                            binding.sourceScopeId = voice.scopeId;
            }
        }
    }
    auto changes = settings.eventValueChanges;
    std::size_t deferredCount = 0;
    for (std::size_t ownerIndex = 0; ownerIndex < owners_.size(); ++ownerIndex)
    {
        if (!owners_[ownerIndex].activationControlled)
        {
            if (!appendOwnerControllerChanges(owners_[ownerIndex], ownerIndex,
                    plan, settings, bindings, changes, error)) return false;
            continue;
        }
        std::vector<EventValueChange> deferred;
        if (!appendOwnerControllerChanges(owners_[ownerIndex], ownerIndex,
                plan, settings, bindings, deferred, error)) return false;
        if (deferred.size() > settings.maxRealtimeEventTransactions
                - std::min(deferredCount, settings.maxRealtimeEventTransactions))
        {
            error = "Activated MIDI Voice controllers exceed prepared transaction capacity";
            return false;
        }
        deferredCount += deferred.size();
    }
    // The MIDI timing clock of the first file played
    // from the start that holds one counts its messages for the Sound's
    // !TimingClock and `MIDITimingClock, messages at one frame counting as
    // one change. A clock that does not fit the prepared Event Value
    // transactions beside the other changes is left out, !TimingClock
    // keeping its stand-in, so that no Sound stops compiling for it.
    if (std::ranges::any_of(bindings, [](const EventValueBinding& binding) {
            return binding.scopeId == globalRuntimeScope && binding.name == talas::expr::midiTimingClockCountEventName
                && binding.domain == talas::expr::ValueDomain::Scalar && !binding.projectionExpression.has_value();
        }))
        for (const auto& owner : owners_)
        {
            if (owner.activationControlled || owner.source != PreparedMidiInputSource::File
                || owner.timingClockFrames.empty())
                continue;
            std::vector<EventValueChange> clock;
            double count = 0.0;
            for (const auto frame : owner.timingClockFrames)
            {
                ++count;
                if (!clock.empty() && clock.back().frame == frame)
                    clock.back().value = count;
                else
                    clock.push_back({frame, std::string{talas::expr::midiTimingClockCountEventName}, count});
            }
            const auto capacity = settings.maxRealtimeEventTransactions
                - std::min(deferredCount, settings.maxRealtimeEventTransactions);
            if (changes.size() <= capacity && clock.size() <= capacity - changes.size())
                changes.insert(changes.end(), clock.begin(), clock.end());
            break;
        }
    if (changes.size() > settings.maxRealtimeEventTransactions
        - std::min(deferredCount, settings.maxRealtimeEventTransactions))
    {
        error = "MIDI Voice controllers exceed prepared Event Value transaction capacity";
        return false;
    }
    if (changes.size() == settings.eventValueChanges.size())
    {
        settings.eventValues = std::move(bindings);
        return true;
    }
    std::stable_sort(changes.begin(), changes.end(), [](const auto& a, const auto& b) {
        return a.frame < b.frame;
    });
    for (std::size_t index = 0; index < changes.size(); ++index)
        for (auto prior = index; prior-- > 0;)
        {
            if (changes[prior].frame != changes[index].frame) break;
            if (changes[prior].scopeId == changes[index].scopeId
                && changes[prior].targetInstanceId == changes[index].targetInstanceId
                && changes[prior].name == changes[index].name)
            {
                error = "MIDI Voice controller conflicts with another Event Value at one frame: "
                    + changes[index].name;
                return false;
            }
        }
    settings.eventValueChanges = std::move(changes);
    settings.eventValues = std::move(bindings);
    return true;
}

bool PreparedMidiFileVoiceSources::equivalent(const PreparedMidiFileVoiceSources& other) const noexcept
{
    if (owners_.size() != other.owners_.size()) return false;
    for (std::size_t i = 0; i < owners_.size(); ++i)
    {
        const auto& a = owners_[i]; const auto& b = other.owners_[i];
        if (a.stepIndex != b.stepIndex || a.nodeId != b.nodeId || a.definitionId != b.definitionId
            || a.instancePath != b.instancePath
            || a.parentVoiceScopeId != b.parentVoiceScopeId
            || a.voiceAncestors != b.voiceAncestors
            || a.channel != b.channel || a.source != b.source
            || a.activationControlled != b.activationControlled
            || a.activationControllers != b.activationControllers
            || a.resourceId != b.resourceId || a.resourceGeneration != b.resourceGeneration
            || a.resourceContentHash != b.resourceContentHash || a.events.size() != b.events.size()
            || a.timingClockFrames != b.timingClockFrames
            || a.scriptEvents != b.scriptEvents
            || a.scriptControllers != b.scriptControllers
            || a.analogExtraSequences != b.analogExtraSequences
            || a.analogGeneratedIndex != b.analogGeneratedIndex
            || a.analogKeyStages != b.analogKeyStages
            || a.analogKeySequences != b.analogKeySequences
            || a.analogLiveArrays != b.analogLiveArrays
            || a.analogLiveRate != b.analogLiveRate
            || a.analogLiveRange != b.analogLiveRange
            || a.analogLegacyRange != b.analogLegacyRange
            || a.analogKeyRate != b.analogKeyRate
            || a.analogKeyStepTriggered != b.analogKeyStepTriggered
            || a.analogKeyLegato != b.analogKeyLegato
            || a.loop != b.loop || a.cycleFrames != b.cycleFrames) return false;
        for (std::size_t j = 0; j < a.events.size(); ++j)
            if (a.events[j].frame != b.events[j].frame || a.events[j].bytes != b.events[j].bytes) return false;
    }
    return true;
}
}
