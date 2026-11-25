#include "../../src/runtime/LiveFieldRanges.h"
#include "../../src/runtime/StartOnlyFields.h"
#include "../support/PrototypeHarness.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/ConstantFields.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// A parameter that follows an Event Value or a Val function of time
// changes while the Sound plays. Offline rendering and the realtime renderer
// must follow the change alike, or one of them must refuse the parameter:
// never may one follow it while the other plays its starting value. Every
// catalog node the prototype harness plays gets, in each expression
// parameter, its default moved by a repeating ramp, by a smoothed Event
// Value and by a stepped Event Value, and wherever both renderers play the
// result they must agree to the sample.
namespace
{
using namespace talas::test::prototypes;

constexpr std::size_t frames = 2048;
constexpr std::size_t blockFrames = 256;
constexpr std::size_t changeFrame = 1024;

// Parameters whose stepped Event Value the realtime renderer does not yet
// follow, though offline rendering does. Each one refuses a function of
// time in realtime. Remove a parameter once it plays alike.
const std::set<std::string_view> realtimeIgnoresEventValueChanges{};

// Parameters that play differently offline and in realtime. Remove a
// parameter once it plays alike.
const std::set<std::string_view> playsDifferently{};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct Played
{
    // Empty when both renderers played; otherwise which one refused.
    std::string refusal;
    talas::runtime::OfflineRenderResult offline;
    std::vector<double> left = std::vector<double>(frames);
    std::vector<double> right = std::vector<double>(frames);
};

// Event Value X steps from 0 to 1 at changeFrame in both renderers.
Played play(
    const talas::core::NodeTypeDescriptor& type,
    const std::vector<Parameter>& parameters,
    bool withAudioOutput)
{
    Played played;
    auto [document, error] = patch(type, parameters, withAudioOutput);
    if (!error.empty())
    {
        played.refusal = error;
        return played;
    }
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    auto settings = talas::test::prototypes::settings(frames);
    settings.eventValues.push_back({"X", 0.0});
    talas::runtime::EventValueChange change;
    change.frame = changeFrame;
    change.name = "X";
    change.value = 1.0;
    settings.eventValueChanges.push_back(change);
    const auto compiled = talas::runtime::compileProject(document, registry(), settings);
    if (!compiled.ok())
    {
        played.refusal = "compile: " + compiled.diagnostics.front().message;
        return played;
    }
    played.offline = talas::runtime::renderOffline(*compiled.graph, frames);
    if (!played.offline.ok())
    {
        played.refusal = "offline: " + played.offline.diagnostics.front().message;
        return played;
    }
    if (!withAudioOutput
        || type.capabilities.preparedRealtime == talas::core::ExecutionSupport::Unsupported)
    {
        played.refusal = "no realtime";
        return played;
    }
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    if (!session.ready())
    {
        played.refusal = "realtime: " + std::string{session.error()};
        return played;
    }
    const std::vector<float> ones(blockFrames, 1.0F);
    const float* const channels[] = {ones.data(), ones.data()};
    for (std::size_t offset = 0; offset < frames; offset += blockFrames)
    {
        require(offset != changeFrame
                || session.updateEventValue("X", 1.0, talas::expr::ValueDomain::Scalar),
            type.typeId + " should take Event Value X in realtime");
        require(session.render({channels, 2},
                    std::span{played.left}.subspan(offset, blockFrames),
                    std::span{played.right}.subspan(offset, blockFrames)),
            type.typeId + " prepared for realtime but failed while playing "
                + parameters.front().parameterId + " = "
                + *parameters.front().expression + ": " + std::string{session.error()});
    }
    return played;
}

Played play(const talas::core::NodeTypeDescriptor& type, const std::vector<Parameter>& parameters)
{
    auto played = play(type, parameters, true);
    if (played.refusal.find("multiple audio output nodes") != std::string::npos)
        played = play(type, parameters, false);
    return played;
}

// The first frame at which the renderers differ, or none.
std::string difference(const Played& played)
{
    const auto& offline = played.offline;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        if (offline.mono[frame] != played.left[frame]
            || (!offline.right.empty() && offline.right[frame] != played.right[frame]))
        {
            return "from frame " + std::to_string(frame) + ": "
                + std::to_string(offline.mono[frame]) + " offline versus "
                + std::to_string(played.left[frame]) + " in realtime";
        }
    }
    return {};
}

void requireAlike(const talas::core::NodeTypeDescriptor& type, const Played& played)
{
    const auto different = difference(played);
    require(different.empty(),
        type.typeId + " plays differently offline and in realtime " + different);
}

Parameter expression(std::string id, std::string text)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(text);
    return parameter;
}

enum class Outcome
{
    Refused,
    Alike,
    Different,
};

Outcome compare(
    const talas::core::NodeTypeDescriptor& type,
    const std::string& parameterId,
    const std::string& text,
    std::string& different)
{
    const auto played = play(type, {expression(parameterId, text)});
    if (!played.refusal.empty())
        return Outcome::Refused;
    different = difference(played);
    return different.empty() ? Outcome::Alike : Outcome::Different;
}

// A step in the parameter's own unit, small and odd enough that the scaled
// and the added change never cancel for a catalog default.
std::string step(talas::expr::ValueDomain domain)
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Hertz:
        return "3.71 hz";
    case talas::expr::ValueDomain::Seconds:
        return "0.000371 s";
    case talas::expr::ValueDomain::NoteNumber:
        return "0.371 nn";
    case talas::expr::ValueDomain::Samples:
        return "0.371 samp";
    case talas::expr::ValueDomain::Scalar:
        break;
    }
    return "0.0371";
}

// Whether both renderers take the node with these parameters, Event Value X
// at 0.5 and not moving: empty, or which one refused it.
std::string refusal(
    const talas::core::NodeTypeDescriptor& type,
    const std::vector<Parameter>& parameters,
    bool withAudioOutput = true)
{
    auto [document, error] = patch(type, parameters, withAudioOutput);
    if (!error.empty())
        return error;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    auto settings = talas::test::prototypes::settings(frames);
    settings.eventValues.push_back({"X", 0.5});
    const auto compiled = talas::runtime::compileProject(document, registry(), settings);
    if (!compiled.ok())
    {
        const auto message = "compile: " + compiled.diagnostics.front().message;
        return withAudioOutput && message.find("multiple audio output nodes") != std::string::npos
            ? refusal(type, parameters, false) : message;
    }
    const auto offline = talas::runtime::renderOffline(*compiled.graph, frames);
    if (!offline.ok())
        return "offline: " + offline.diagnostics.front().message;
    if (!withAudioOutput || type.capabilities.preparedRealtime == talas::core::ExecutionSupport::Unsupported)
        return "no realtime";
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    if (!session.ready())
        return "realtime: " + std::string{session.error()};
    const std::vector<float> ones(blockFrames, 1.0F);
    const float* const channels[] = {ones.data(), ones.data()};
    std::vector<double> left(blockFrames);
    std::vector<double> right(blockFrames);
    if (!session.render({channels, 2}, left, right))
        return "realtime: " + std::string{session.error()};
    return {};
}

// Any hot field of any Sound in the signal flow
// path can hold Val. Every hot field (an expression field that is not
// a constant field, nor fed by a signal) of every Sound the harness plays
// takes an expression of Event Value X, wherever the Sound plays with its
// defaults: neither renderer refuses it. CDP8's ports are not native Sounds
// and keep CDP's own ranges, so they are left out. Whether a field follows
// X as it moves is the sweep's to check.
void testEveryHotFieldTakesVal()
{
    // A MIDIFileEcho's channels take Val offline, but the realtime
    // renderer picks the file's events for a fixed range of channels as it
    // prepares, so it refuses one that reads an Event Value.
    const std::set<std::string_view> offlineOnly{
        "talas.midiFileEcho.lowChannel",
        "talas.midiFileEcho.highChannel",
        "talas.midiFileEchoTap.lowChannel",
        "talas.midiFileEchoTap.highChannel",
    };
    std::size_t hot = 0;
    std::string refused;
    for (const auto& type : registry().nodeTypes())
    {
        if (!playable(type) || type.typeId.starts_with("talas.cdp"))
            continue;
        std::vector<const talas::core::ParameterDescriptor*> fields;
        for (const auto& parameter : type.parameters)
        {
            bool fedByInput = false;
            for (const auto& input : type.inputs)
                fedByInput |= input.id == parameter.id && fed(type, input);
            if (parameter.kind == talas::core::ParameterKind::Expression
                && !talas::runtime::isConstantField(type.typeId, parameter.id) && !fedByInput)
                fields.push_back(&parameter);
        }
        // The default, a pitch in a frequency field taken as its frequency.
        const auto base = [](const talas::core::ParameterDescriptor& parameter) {
            return parameter.defaultExpression.empty() ? std::string{"0"}
                : parameter.domain == talas::expr::ValueDomain::Hertz
                    && parameter.defaultExpression.find("hz") == std::string::npos
                    && parameter.defaultExpression != "default"
                ? "(" + parameter.defaultExpression + ") hz"
                : parameter.defaultExpression;
        };
        if (fields.empty())
            continue;
        const auto steady = refusal(type, {expression(fields.front()->id, base(*fields.front()))});
        if (!steady.empty() && steady != "no realtime")
            continue;
        for (const auto* parameter : fields)
        {
            // The default again, as X is a half.
            const auto text = "(" + base(*parameter) + ") * (2 * !X)";
            const auto taken = refusal(type, {expression(parameter->id, text)});
            ++hot;
            const auto name = type.typeId + "." + parameter->id;
            if (offlineOnly.contains(name) ? !taken.starts_with("realtime: ") : taken != steady)
                refused += "\n  " + name + " = " + text + ": " + (taken.empty() ? "now taken in realtime" : taken);
        }
    }
    std::cout << hot << " hot fields take Val\n";
    require(refused.empty(), "every hot field should take Val; refused:" + refused);
    require(hot > 400, "the harness should reach the hot fields of the Sounds it plays");
}

void testLiveParametersPlayAlikeOrAreRefused()
{
    std::size_t variants = 0;
    std::size_t playedAlike = 0;
    for (const auto& type : registry().nodeTypes())
    {
        if (!playable(type))
            continue;
        for (const auto& parameter : type.parameters)
        {
            if (parameter.kind != talas::core::ParameterKind::Expression)
                continue;
            bool fedByInput = false;
            for (const auto& input : type.inputs)
                fedByInput |= input.id == parameter.id && fed(type, input);
            if (fedByInput)
                continue;
            // The default, a pitch in a frequency field (a GenericSource's
            // 4 c) taken as the frequency it names.
            const auto base = parameter.defaultExpression.empty() ? std::string{"0"}
                : parameter.domain == talas::expr::ValueDomain::Hertz
                    && parameter.defaultExpression.find("hz") == std::string::npos
                    && parameter.defaultExpression != "default"
                ? "(" + parameter.defaultExpression + ") hz"
                : parameter.defaultExpression;
            // The default scaled down or up by the change and moved by a
            // step, so that a zero default changes too.
            const auto moved = [&](const std::string& change, const char* direction) {
                return "(" + base + ") * (1 " + direction + " (" + change + " * 0.2)) + (" + change
                    + " * " + step(parameter.domain) + ")";
            };
            const auto name = type.typeId + "." + parameter.id;
            const auto listed = realtimeIgnoresEventValueChanges.contains(name);
            const auto different = playsDifferently.contains(name);
            bool eventValueDiffers = false;
            bool anyDiffers = false;
            for (const std::string change : {"(1 repeatingRamp: 0.005 s)", "(!X smoothed)", "!X"})
            {
                for (const auto* direction : {"-", "+"})
                {
                    const auto text = moved(change, direction);
                    std::string where;
                    const auto outcome = compare(type, parameter.id, text, where);
                    ++variants;
                    playedAlike += outcome == Outcome::Alike;
                    anyDiffers |= outcome == Outcome::Different;
                    if (change == "!X" && listed)
                    {
                        eventValueDiffers |= outcome == Outcome::Different;
                        continue;
                    }
                    require(different || outcome != Outcome::Different,
                        name + " = " + text + " plays differently offline and in realtime "
                            + where);
                }
            }
            require(!listed || eventValueDiffers,
                name + " now follows its Event Value alike or refuses it; update the list");
            // The default dropped to zero as X steps to 1, as a gate
            // closes: a field realtime held at its starting value would
            // show only when the change crosses zero.
            {
                const auto text = "(" + base + ") * (1 - !X)";
                std::string where;
                const auto outcome = compare(type, parameter.id, text, where);
                ++variants;
                playedAlike += outcome == Outcome::Alike;
                anyDiffers |= outcome == Outcome::Different;
                require(different || outcome != Outcome::Different,
                    name + " = " + text + " plays differently offline and in realtime "
                        + where);
            }
            require(!different || anyDiffers,
                name + " now plays alike or is refused; remove it from playsDifferently");
        }
    }
    std::cout << playedAlike << " of " << variants
              << " live parameter variants play alike offline and in realtime\n";
    // Raise the floor as more parameters follow their changes in both
    // renderers. Spectrum sources count since the harness hears their
    // spectra, where it used to play a tone beside them.
    require(playedAlike >= 7367,
        "only " + std::to_string(playedAlike) + " live parameter variants play in both renderers");
}

void testIgnoredEventValueChangesNameCatalogParameters()
{
    std::set<std::string_view> names{realtimeIgnoresEventValueChanges};
    names.insert(playsDifferently.begin(), playsDifferently.end());
    for (const auto name : names)
    {
        const auto dot = name.rfind('.');
        const auto* type = registry().findNodeType(name.substr(0, dot));
        require(type != nullptr
                && std::ranges::any_of(type->parameters,
                    [&](const talas::core::ParameterDescriptor& parameter) {
                        return parameter.id == name.substr(dot + 1);
                    }),
            std::string{name} + " is not a catalog parameter");
    }
}

// The saturating ranges and the constant fields name catalog parameters.
void testFieldListsNameCatalogParameters()
{
    const auto named = [](std::string_view typeId, std::string_view parameterId) {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr
                && std::ranges::any_of(type->parameters,
                    [&](const talas::core::ParameterDescriptor& parameter) { return parameter.id == parameterId; }),
            std::string{typeId} + "." + std::string{parameterId} + " is not a catalog parameter");
    };
    for (const auto& range : talas::runtime::live_field_ranges::ranges)
        named(range.typeId, range.parameterId);
    for (const auto& [typeId, parameterId] : talas::runtime::start_only_fields::fields)
        named(typeId, parameterId);
}

// A GraphicEQ band at unity adds nothing, but when its level follows an
// Event Value its filter must keep running: offline it restarted from
// silence where realtime kept its state.
void testGraphicEqBandLeavesUnityLikeRealtime()
{
    const auto* type = registry().findNodeType("talas.graphicEq");
    require(type != nullptr, "GraphicEQ should be a catalog node");
    const auto played = play(*type, {expression("cf1000Hz", "1 - (!X * 0.2)")});
    require(played.refusal.empty(),
        "GraphicEQ should play an Event Value band level: " + played.refusal);
    requireAlike(*type, played);
}

// Offline, a timed HarmonicResonator Frequency was taken for a fixed one:
// each block prepared its delay line afresh and the resonator fell silent.
void testHarmonicResonatorFollowsATimedFrequency()
{
    const auto* type = registry().findNodeType("talas.harmonicResonator");
    require(type != nullptr, "HarmonicResonator should be a catalog node");
    const auto played = play(*type,
        {expression("frequency", "220 hz * (1 - ((1 repeatingRamp: 0.005 s) * 0.2))")});
    require(played.refusal.empty(),
        "HarmonicResonator should play a timed Frequency: " + played.refusal);
    double late = 0.0;
    for (std::size_t frame = frames / 2; frame < frames; ++frame)
        late = std::max(late, std::abs(played.offline.mono[frame]));
    require(late > 0.01, "HarmonicResonator should keep resonating offline");
    requireAlike(*type, played);
}

// A parameter the realtime renderer reads only when it starts cannot take a
// time function there that offline follows: it names the parameter rather
// than freezing it.
// A MemoryWriter records for its CaptureDuration, which follows a
// function of time or an Event Value in realtime as offline.
void testRealtimeFollowsAMemoryWritersCaptureDuration()
{
    const auto* type = registry().findNodeType("talas.memoryWriter");
    require(type != nullptr, "MemoryWriter should be a catalog node");
    for (const auto* duration : {"(1 repeatingRamp: 0.005 s) * 0.01 s", "(0.002 s) * (1 + !X)"})
    {
        const auto played = play(*type, {expression("captureDuration", duration)});
        require(played.refusal.empty(),
            std::string{"realtime MemoryWriter should follow its captureDuration "} + duration + ": "
                + played.refusal);
        requireAlike(*type, played);
    }
}

// A field both renderers read only when the Sound starts plays a time
// function's starting value in realtime as offline: a Noise's InitialState
// seeds its first frame.
void testRealtimePlaysAStartOnlyFieldsStartingValue()
{
    const auto* type = registry().findNodeType("talas.noise");
    require(type != nullptr, "Noise should be a catalog node");
    const auto timed = play(*type,
        {expression("initialState", "0.25 + ((1 repeatingRamp: 0.005 s) * 0.5)")});
    const auto steady = play(*type, {expression("initialState", "0.25")});
    require(timed.refusal.empty() && steady.refusal.empty(),
        "Noise should play a timed initialState: " + timed.refusal);
    requireAlike(*type, timed);
    require(timed.offline.mono == steady.offline.mono,
        "a timed initialState should play its starting value");
}

// A Constant's program with EventVariables plays alike
// in both renderers, counting X's change once, or each millisecond when a
// variable is read every millisecond.
void testEventVariablesPlayAlike()
{
    const auto* type = registry().findNodeType("talas.constant");
    require(type != nullptr, "Constant should be a catalog node");
    for (const auto* program : {"| count | count := EventVariable new initialValue: -1. "
                                "!X true: (count <~ (count + 0.1)) false: (count)",
             "| count | count := EventVariable new initialValue: 0. count readEveryMillisecond. "
             "count <~ (count + (0.001 * !X))",
             "| pos vel a f v | pos := EventVariable new initialValue: 0. pos readEveryMillisecond. "
             "vel := EventVariable new initialValue: 0. a := 0.05 * (!X - pos). f := 0.01 * vel. "
             "v := vel <~ (vel + a - f). (pos <~ (pos + (0.01 * v)))"})
    {
        const auto played = play(*type, {expression("value", program)});
        require(played.refusal.empty(), std::string{""} + program + " should play: " + played.refusal);
        requireAlike(*type, played);
        require(played.offline.mono.back() != played.offline.mono.front(),
            std::string{""} + program + " should change after X does");
    }
}

// A field both renderers read only when the Sound starts plays its
// starting value, so offline no longer refuses a later
// value it never reads, such as a pitch bound or a scale limit that an
// Event Value moves out of its range.
void testStartOnlyFieldsCheckOnlyTheirStartingValue()
{
    for (const auto& [typeId, field, steady, live] :
        {std::tuple{"talas.midiVoice", "highPitch", "127 nn", "(127 nn) * (1 + (!X * 0.2))"},
            std::tuple{"talas.timeFrequencyScale", "maxFreqScale", "1.0", "(1.0) * (1 - !X)"},
            std::tuple{"talas.simplePitchShifter", "maxInputPitch", "2000 hz", "(2000 hz) * (1 - !X)"},
            std::tuple{"talas.frequencyScale", "maxScale", "1.0", "(1.0) * (1 - !X)"}})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, live)});
        const auto held = play(*type, {expression(field, steady)});
        require(played.refusal.empty() && held.refusal.empty(),
            std::string{typeId} + " should play a moving " + field + " from its starting value: "
                + played.refusal);
        requireAlike(*type, played);
        require(played.offline.mono == held.offline.mono,
            std::string{typeId} + " should play its starting " + field);
    }
}

// Constant fields are set at compile time and cannot be changed while
// the Sound is playing: given a time function
// or an Event Value, each plays its starting value in both renderers, a later
// value neither followed nor checked, though it is not a whole number.
void testConstantFieldsPlayTheirStartingValue()
{
    for (const auto& [typeId, field, steady, live] :
        {std::tuple{"talas.filter", "order", "4", "(4) * (1 + ((1 repeatingRamp: 0.005 s) * 0.2))"},
            std::tuple{"talas.vocoder", "nbrBands", "16", "(16) * (1 - (!X * 0.2))"},
            std::tuple{"talas.euSeq", "pulses", "3", "(3) * (1 + ((!X smoothed) * 0.2))"},
            std::tuple{"talas.multisegmentEnvelope", "startLoop", "0", "(1 repeatingRamp: 0.005 s) * 2"},
            std::tuple{"talas.sumOfSines", "analysisFrames0", "1", "1 + (!X * 0.5)"},
            // The fields that set a structure up as
            // it compiles take Val at its starting value.
            std::tuple{"talas.replicator", "number", "1", "1 + (!X * 2)"},
            std::tuple{"talas.rhythmicCellularAutomaton", "rule", "30", "(30) * (1 + (!X * 0.2))"},
            std::tuple{"talas.rhythmicCellularAutomaton", "generations", "4", "4 + ((!X smoothed) * 2)"},
            std::tuple{"talas.rhythmicCellularAutomaton", "generationLength", "8", "(8) * (1 - (!X * 0.5))"}})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, live)});
        const auto held = play(*type, {expression(field, steady)});
        require(played.refusal.empty() && held.refusal.empty(),
            std::string{typeId} + " should play a moving " + field + " from its starting value: "
                + played.refusal);
        requireAlike(*type, played);
        require(played.offline.mono == held.offline.mono,
            std::string{typeId} + " should play its starting " + field);
    }
}

// A hot field, cyan in the Sound editor, follows a moving value, and a
// constant one, plain, plays its starting value; the inspector marks each
// by talas::runtime::isConstantField.
void testHotAndConstantFieldsAreMarked()
{
    for (const auto& [typeId, field, steady, live] :
        {std::tuple{"talas.oscillator", "frequency", "440", "440 * (1 + ((1 repeatingRamp: 0.005 s) * 0.5))"},
            std::tuple{"talas.filter", "order", "4", "(4) * (1 + ((1 repeatingRamp: 0.005 s) * 0.2))"}})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, live)});
        const auto held = play(*type, {expression(field, steady)});
        require(played.refusal.empty() && held.refusal.empty(), std::string{typeId} + " should play: " + played.refusal);
        require((played.offline.mono == held.offline.mono) == talas::runtime::isConstantField(typeId, field),
            std::string{""} + typeId + "'s " + field + " should be marked as it plays");
    }
    require(talas::runtime::isConstantField("talas.cloudBank", "seed") && talas::runtime::isConstantField("talas.analogSequencer", "polyphony")
            && !talas::runtime::isConstantField("talas.oscillator", "frequency") && !talas::runtime::isConstantField("talas.filter", "frequency"),
        "seeds and counts should be constant and frequencies hot");
}

// The setup of a bank or a spectrum is constant too (whatever determines
// something about the memory or DSP requirements): a
// bank's size and count, a
// spectrum's partials and analysis, a selected track
//, a terrain's size, and the seeds that assign a CloudBank's tracks
// to grains and pick a SpectrumModifier's tracks. Each plays its starting value in both renderers, where
// both refused a later value that was not a whole number.
void testBankAndSpectrumSetupPlaysItsStartingValue()
{
    // Each field's default, moved as the survey moves it.
    for (const auto& [typeId, field, change] :
        {std::tuple{"talas.oscillatorBank", "nbrOscillators", " * (1 + ((1 repeatingRamp: 0.005 s) * 0.2))"},
            std::tuple{"talas.filterBank", "nbrFilters", " * (1 - (!X * 0.2)) + (!X * 0.0371)"},
            std::tuple{"talas.cloudBank", "seed", " * (1 + ((1 repeatingRamp: 0.005 s) * 0.2))"},
            std::tuple{"talas.spectrumInRam", "firstPartial", " * (1 + (!X * 0.2)) + (!X * 0.0371)"},
            std::tuple{"talas.spectrumOnDisk", "nbrPartials", " * (1 - ((!X smoothed) * 0.2))"},
            std::tuple{"talas.vocoderChannelBank", "count", " * (1 - (!X * 0.2))"},
            std::tuple{"talas.waveTerrainOscillator", "width", " * (1 + (!X * 0.2)) + (!X * 0.0371)"},
            std::tuple{"talas.frameSynchronizer", "frameLength", " * (1 - (!X * 0.2))"},
            std::tuple{"talas.spectrumModifier", "seed", " * (1 + ((1 repeatingRamp: 0.005 s) * 0.2))"}})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto descriptor = std::ranges::find(type->parameters, std::string_view{field},
            &talas::core::ParameterDescriptor::id);
        require(descriptor != type->parameters.end(), std::string{typeId} + "." + field + " should be a field");
        const auto steady = descriptor->defaultExpression;
        const auto played = play(*type, {expression(field, "(" + steady + ")" + change)});
        const auto held = play(*type, {expression(field, steady)});
        require(played.refusal.empty() && held.refusal.empty(),
            std::string{typeId} + " should play a moving " + field + " from its starting value: "
                + played.refusal + held.refusal);
        requireAlike(*type, played);
        require(played.offline.mono == held.offline.mono,
            std::string{typeId} + " should play its starting " + field);
    }
}

// Fields documented as hot that realtime held at their starting value,
// refusing a function of time, while offline followed them block by block: a
// CloudBank's CyclesPerGrain, which GrainDurMod modulates as it does GrainDur
//, a FormantBank's Frequency, set "from a MIDI keyboard or
// continuous controller", a LiveSpectralAnalysis's AmpScale and
// FreqScale (controlled continuously with !Freq * 10), and a
// SpectrumModifier's criteria and changes, applied "on each frame"
//, its track numbers rounded, and a Multisample's loop points,
// held within the sample. Realtime now follows each as a
// block starts, as offline does. A VocoderChannelBank sets its TimeConstant
// as it starts in both renderers, as it does its envelope times.
void testRealtimeFollowsHotSpectralFields()
{
    struct Case
    {
        const char* typeId;
        const char* field;
        const char* text;
        bool heard;
    };
    for (const auto& [typeId, field, text, heard] : {
             Case{"talas.cloudBank", "cyclesPerGrain", "(1) * (1 + ((1 repeatingRamp: 0.005 s) * 0.2))", false},
             Case{"talas.cloudBankElement", "cyclesPerGrain", "(1) * (1 - ((!X smoothed) * 0.2))", false},
             Case{"talas.formantBank", "frequency", "(110 hz) * (1 + (!X * 0.2))", true},
             Case{"talas.formantBankOscillator", "frequency", "(110 hz) * (1 - ((1 repeatingRamp: 0.005 s) * 0.2))", true},
             Case{"talas.liveSpectralAnalysis", "ampScale", "(1.0) * (1 - (!X * 0.5))", true},
             Case{"talas.liveSpectralAnalysis", "freqScale", "(1.0) * (1 + ((!X smoothed) * 0.2))", true},
             Case{"talas.spectrumModifier", "ampScale", "(1.0) * (1 - (!X * 0.5))", true},
             Case{"talas.spectrumModifier", "loTrack", "(1) * (1 + (!X * 0.2)) + (!X * 0.0371)", false},
             Case{"talas.spectrumModifier", "probability", "(1.0) * (1 - ((1 repeatingRamp: 0.005 s) * 0.2))", false},
             Case{"talas.spectrumModifier", "freqSmoothing", "(0.0) * (1 + (!X * 0.2)) + (!X * 0.0371)", false},
             Case{"talas.multisample", "loopStart", "(0.0) * (1 + ((1 repeatingRamp: 0.005 s) * 0.2)) + ((1 repeatingRamp: 0.005 s) * 0.0371)", false},
             Case{"talas.multisample", "loopEnd", "(1.0) * (1 + (!X * 0.2)) + (!X * 0.0371)", false},
             Case{"talas.vocoderChannelBank", "timeConstant", "(5 ms) * (1 + ((1 repeatingRamp: 0.005 s) * 0.2))", false},
         })
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, text)});
        require(played.refusal.empty(),
            std::string{typeId} + " should play a moving " + field + " in both renderers: " + played.refusal);
        requireAlike(*type, played);
        if (heard)
        {
            const auto descriptor = std::ranges::find(type->parameters, std::string_view{field},
                &talas::core::ParameterDescriptor::id);
            const auto steady = play(*type, {expression(field, descriptor->defaultExpression)});
            require(steady.refusal.empty() && steady.offline.mono != played.offline.mono,
                std::string{typeId} + " should be heard to follow its " + field);
        }
    }
}

// Owner decision 2026-10-12: a whole-number field's live value is rounded
// after saturating, and a live value past a field's range saturates at its
// edge. A sample index (which file plays, an integer that selects which
// of the disk files should be played, an index below the first
// selecting the first) moved to a fraction rounds to the nearest file, and a
// FormantBank's or StereoDispersiveResonator's Frequency dropped to 0 Hz
// plays at the lowest it takes, and a WaveTerrainOscillator's orbit stays on
// its terrain; each stopped the Sound offline or in realtime.
void testLiveIndexesRoundAndFrequenciesSaturate()
{
    for (const auto& [typeId, field, text, steady] : {
             std::tuple{"talas.multisample", "index", "(0) * (1 + (!X * 0.2)) + (!X * 0.0371)", "0"},
             std::tuple{"talas.multifileDiskPlayer", "index", "(0) * (1 - ((1 repeatingRamp: 0.005 s) * 0.2)) + ((1 repeatingRamp: 0.005 s) * 0.0371)", "0"},
             std::tuple{"talas.timeIndexForSamples", "index", "(0) * (1 + ((!X smoothed) * 0.2)) + ((!X smoothed) * 0.0371)", "0"},
             std::tuple{"talas.timeIndexForFileNames", "index", "(0) * (1 + (!X * 0.2)) + (!X * 0.0371)", "0"},
             std::tuple{"talas.formantBank", "frequency", "(110 hz) * (1 - !X)", ""},
             std::tuple{"talas.formantBankOscillator", "frequency", "(110 hz) * (1 - !X)", ""},
             std::tuple{"talas.stereoDispersiveResonator", "frequency", "(220 hz) * (1 - !X)", ""},
             std::tuple{"talas.waveTerrainOscillator", "xRadius", "(0.9) * (1 + (!X * 0.2)) + (!X * 0.0371)", ""},
             std::tuple{"talas.waveTerrainOscillator", "yRadius", "(0.9) * (1 + ((!X smoothed) * 0.2))", ""},
             std::tuple{"talas.waveTerrainOscillator", "yRatio", "(2.0) * (1 - !X)", ""},
         })
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, text)});
        require(played.refusal.empty() || played.refusal.starts_with("realtime: Realtime renderer requires a prepared Audio snapshot"),
            std::string{typeId} + " should play a live " + field + " " + text + ": " + played.refusal);
        if (played.refusal.empty())
            requireAlike(*type, played);
        if (*steady != '\0')
        {
            const auto held = play(*type, {expression(field, steady)});
            require(held.offline.mono == played.offline.mono,
                std::string{typeId} + " should round a live " + field + " to the nearest whole one");
        }
    }
}

// A live value that passes the bound another field sets saturates against
// it (owner, 2026-10-12, as for the Monotonizer and Vocoder bounds): a
// DynamicRangeController's Ratio stays within what its Type takes, at or
// above 1 for a compressor and above 0 up to 1 for an expander, and a
// FrequencyScale's FrequencyScale at or below the
// MaxScale its latency is set for. Each stopped the Sound offline.
void testRatioAndScaleSaturateAgainstTheirBound()
{
    for (const auto& [typeId, field, text, steady] : {
             std::tuple{"talas.dynamicRangeController", "ratio", "(1.0) * (1 - !X)", "1.0"},
             std::tuple{"talas.dynamicRangeController", "ratio",
                 "(1.0) * (1 - ((1 repeatingRamp: 0.005 s) * 0.2)) + ((1 repeatingRamp: 0.005 s) * 0.0371)", "1.0"},
             // Held by an Event Value too, so that both take the live path.
             std::tuple{"talas.frequencyScale", "frequencyScale", "(1.0) * (1 + (!X * 0.2)) + (!X * 0.0371)",
                 "(1.0) * (1 + (!X * 0))"},
         })
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, text)});
        const auto held = play(*type, {expression(field, steady)});
        require(played.refusal.empty() && held.refusal.empty(),
            std::string{typeId} + " should play a live " + field + " " + text + ": " + played.refusal);
        requireAlike(*type, played);
        require(played.offline.mono == held.offline.mono,
            std::string{typeId} + " should saturate a live " + field + " at its bound");
    }
}

// An AnalogSequencer's Left, Right, Gate, Step and Rate are hot (an Event
// Value such as !KeyDown restarts and stops the sequence interactively),
// and realtime followed functions of time and
// smoothed Event Values in them where offline refused them; levels past -1
// to 1 and a Rate of zero stopped the Sound in both. Both renderers now
// follow them block by block, a level saturating at 1 and a Rate of zero
// playing at a fixed-point step (owner, 2026-10-12). Polyphony sets the
// sequence up and plays its starting value.
void testAnalogSequencerFollowsItsControlsAndSaturates()
{
    for (const auto& [field, text, steady] : {
             std::tuple{"left", "(1.0) * (1 - ((1 repeatingRamp: 0.005 s) * 0.2))", ""},
             std::tuple{"rate", "(1.0) * (1 + ((!X smoothed) * 0.2))", ""},
             std::tuple{"rate", "(1.0) * (1 - !X)", ""},
             std::tuple{"left", "(1.0) * (1 + (!X * 0.2)) + (!X * 0.0371)", "(1.0) * (1 + (!X * 0))"},
             std::tuple{"polyphony", "(1) * (1 - !X)", "1"},
         })
    {
        const auto* type = registry().findNodeType("talas.analogSequencer");
        require(type != nullptr, "AnalogSequencer should be a catalog node");
        const auto played = play(*type, {expression(field, text)});
        require(played.refusal.empty(),
            std::string{"AnalogSequencer should play a live "} + field + " " + text + ": " + played.refusal);
        requireAlike(*type, played);
        if (*steady != '\0')
        {
            const auto held = play(*type, {expression(field, steady)});
            require(held.refusal.empty() && held.offline.mono == played.offline.mono,
                std::string{"AnalogSequencer should saturate or start from its "} + field);
        }
    }
}

// An AnalogSequencer's StartIndex and EndIndex select the stages it plays
//, and its key arrays followed them live while
// the sequence itself refused a moving one in both renderers. Both now read
// them as each block starts, a moving one saturating within the sequence and
// a moving EndIndex below StartIndex rising to it (owner, 2026-10-12 and
// 2026-10-13); a static one past the sequence is still refused.
void testAnalogSequencerFollowsItsSelectedStages()
{
    const auto* type = registry().findNodeType("talas.analogSequencer");
    require(type != nullptr, "AnalogSequencer should be a catalog node");
    // Four looping stages of 48 frames at distinct levels, timed by their
    // Durations (Step 1) or stepped every 96 frames.
    const auto sequencer = [](std::string start, std::string end, bool stepped) {
        auto parameters = std::vector<Parameter>{expression("startIndex", std::move(start)),
            expression("endIndex", std::move(end)),
            expression("step", stepped ? "(1 repeatingRamp: 0.002 s) > 0.5" : "1")};
        Parameter loop;
        loop.parameterId = "loop";
        loop.boolean = true;
        parameters.push_back(loop);
        for (const auto& [id, value] : {std::pair{"durations", "{0.001 s}"}, std::pair{"dutyCycles", "{1}"},
                 std::pair{"velocities", "{0.25} {0.5} {0.75} {1}"}})
        {
            Parameter list;
            list.parameterId = id;
            list.text = value;
            parameters.push_back(list);
        }
        return parameters;
    };
    const auto after = [](const Played& played) {
        return std::vector<double>(played.offline.mono.begin() + changeFrame, played.offline.mono.end());
    };
    for (const auto stepped : {false, true})
    {
        const auto whole = play(*type, sequencer("0", "3", stepped));
        require(whole.refusal.empty(), "AnalogSequencer should play its four stages: " + whole.refusal);
        for (const auto& [start, end, heldStart, heldEnd] : {
                 std::tuple{"0", "3 * (1 - !X)", "0", "0"},
                 std::tuple{"3 * !X", "3", "3", "3"},
                 // Past the last stage it stays on it.
                 std::tuple{"0", "3 + (!X * 2.4)", "0", "3"},
                 // EndIndex below StartIndex rises to it.
                 std::tuple{"2", "3 * (1 - !X)", "2", "2"},
                 std::tuple{"0", "(3 * (1 - ((1 repeatingRamp: 0.005 s) * 0.2))) + (!X * 0.0371)", "", ""},
             })
        {
            const auto played = play(*type, sequencer(start, end, stepped));
            require(played.refusal.empty(), std::string{"AnalogSequencer should play StartIndex "} + start
                    + " and EndIndex " + end + ": " + played.refusal);
            requireAlike(*type, played);
            if (*heldStart == '\0')
                continue;
            const auto saturates = std::string_view{end} == "3 + (!X * 2.4)";
            require(saturates == (after(whole) == after(played)),
                std::string{"AnalogSequencer should follow StartIndex "} + start + " and EndIndex " + end
                    + (stepped ? " as it steps" : ""));
            if (stepped)
                continue;
            const auto held = play(*type, sequencer(heldStart, heldEnd, stepped));
            require(held.refusal.empty() && after(held) == after(played),
                std::string{"AnalogSequencer should select stages "} + heldStart + " to " + heldEnd
                    + " once StartIndex is " + start + " and EndIndex " + end);
        }
    }
    require(!play(*type, sequencer("0", "4", false)).refusal.empty(),
        "AnalogSequencer should still refuse a static EndIndex past its sequence");
}

// A HarmonicSpectrum's Frequency and Amplitude and a
// SpectrumFromSingleCycle's Frequency (our extensions, so no documented
// case makes them hot)
// set up the static spectrum each computes, and both renderers refused a
// moving one. They are constant fields now: a moving one plays its starting
// value in both renderers, and inside a MIDIVoice it is refused by name
// (taken as recommended, 2026-10-13).
void testStaticSpectraReadTheirStartingSetup()
{
    for (const auto& [typeId, field, text, steady] : {
             std::tuple{"talas.harmonicSpectrum", "frequency",
                 "(440 hz) * (1 - ((1 repeatingRamp: 0.005 s) * 0.2)) + ((1 repeatingRamp: 0.005 s) * 3.71 hz)",
                 "440 hz"},
             std::tuple{"talas.harmonicSpectrum", "frequency", "(440 hz) * (1 + (!X * 0.2))", "440 hz"},
             std::tuple{"talas.harmonicSpectrum", "amplitude", "(1.0) * (1 - !X)", "1.0"},
             std::tuple{"talas.harmonicSpectrum", "amplitude", "(0.5) + ((!X smoothed) * 0.2)", "0.5"},
             std::tuple{"talas.spectrumFromSingleCycle", "frequency", "(440 hz) * (1 - (!X * 0.2))", "440 hz"},
         })
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, text)});
        const auto held = play(*type, {expression(field, steady)});
        require(played.refusal.empty() && held.refusal.empty(),
            std::string{typeId} + " should play a moving " + field + " " + text + ": " + played.refusal);
        requireAlike(*type, played);
        require(played.offline.mono == held.offline.mono && played.left == held.left,
            std::string{typeId} + " should play its starting " + field);
    }
}

// A GAOscillators's TimeIndex moves through its analysis's amplitude and
// frequency envelopes, -1 at their beginning and 1 at their end, and both
// renderers refused any TimeIndex but 0. The
// analyses we compute hold constant envelopes, so every TimeIndex plays as 0
// does, a moving one followed in both renderers and saturating at -1 and 1;
// a static one past them is still refused.
void testGaOscillatorsReadsItsTimeIndex()
{
    const auto* type = registry().findNodeType("talas.gaOscillators");
    require(type != nullptr, "GAOscillators should be a catalog node");
    const auto middle = play(*type, {expression("timeIndex", "0")});
    require(middle.refusal.empty(), "GAOscillators should play TimeIndex 0: " + middle.refusal);
    for (const auto* text : {"-1", "1", "0.5", "((1 repeatingRamp: 0.005 s) * 2) - 1", "(!X smoothed) * 3",
             "-1 + (!X * 0.5)"})
    {
        const auto played = play(*type, {expression("timeIndex", text)});
        require(played.refusal.empty(),
            std::string{"GAOscillators should play TimeIndex "} + text + ": " + played.refusal);
        requireAlike(*type, played);
        require(played.offline.mono == middle.offline.mono,
            std::string{"GAOscillators should read TimeIndex "} + text + " in its constant envelopes");
    }
    require(!play(*type, {expression("timeIndex", "1.5")}).refusal.empty(),
        "GAOscillators should still refuse a static TimeIndex past 1");
}

// Inside a MIDIVoice a MultisegmentEnvelope's loop points cannot take their
// starting value from the compile, so a moving one is named as such rather
// than refused for want of the envelope's compiled authority.
void testMultisegmentEnvelopeNamesAMovingLoopInAVoice()
{
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    document.graph.nodes.push_back({"env", "talas.multisegmentEnvelope", {expression("startLoop", "1 + !X")}});
    document.graph.nodes.push_back(talas::test::prototypes::source("tone", PortType::Audio));
    document.graph.nodes.push_back({"vca", "talas.vca", {}});
    document.graph.nodes.push_back({"voice", "talas.midiVoice", {}});
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({"tone", "audioOut", "vca", "audioIn"});
    document.graph.connections.push_back({"env", "value", "vca", "gain"});
    document.graph.connections.push_back({"vca", "audioOut", "voice", "audioIn"});
    document.graph.connections.push_back({"voice", "leftOut", "out", "audioIn"});
    auto settings = talas::test::prototypes::settings(frames);
    settings.eventValues.push_back({"X", 0.0});
    const auto compiled = talas::runtime::compileProject(document, registry(), settings);
    require(!compiled.ok(), "a moving loop point in a MIDIVoice should be refused");
    require(std::ranges::any_of(compiled.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.message.find("StartLoop and EndLoop are read once") != std::string::npos;
                }),
        "a moving loop point in a MIDIVoice should be named: " + compiled.diagnostics.front().message);
}

// Owner decision 2026-10-12: a live value past a field's range saturates
// at its edge in both renderers, as the documented cases do: a StereoMix2
// Scale pushed above 1 plays as 1, an ADSR's AttackTime
// and a VCF's Cutoff dropped to zero play one step inside their range. A
// static value outside the range is still refused.
void testLiveValuesSaturateAtTheirRange()
{
    const auto* mix = registry().findNodeType("talas.stereoMix2");
    require(mix != nullptr, "StereoMix2 should be a catalog node");
    const auto pushed = play(*mix, {expression("scale1", "(1.0) * (1 + (!X * 0.2))")});
    const auto full = play(*mix, {expression("scale1", "1.0")});
    require(pushed.refusal.empty() && full.refusal.empty(),
        "StereoMix2 should play a Scale1 pushed past 1: " + pushed.refusal);
    requireAlike(*mix, pushed);
    require(pushed.offline.mono == full.offline.mono, "a Scale1 pushed past 1 should play as 1");
    const auto fixed = play(*mix, {expression("scale1", "1.2")});
    require(!fixed.refusal.empty(), "a static Scale1 of 1.2 should still be refused");
    for (const auto& [typeId, field, text] :
        {std::tuple{"talas.adsr", "attackTime", "(10 ms) * (1 - !X)"},
            std::tuple{"talas.vcf", "cutoff", "(1000 hz) * (1 - !X)"},
            std::tuple{"talas.midiVoice", "left", "(1.0) * (1 + ((1 repeatingRamp: 0.005 s) * 0.2))"}})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, text)});
        require(played.refusal.empty(),
            std::string{typeId} + " should play a " + field + " past its range: " + played.refusal);
        requireAlike(*type, played);
    }
}

// Owner decision 2026-10-12: a whole-number field's live value is rounded to
// the nearest whole number in both renderers, after it saturates to the
// range: Iterations moving to 1.6 play as 2, a ClockDivider's Division
// moving to 2.4 plays as 2.
void testWholeNumberFieldsRoundTheirLiveValue()
{
    for (const auto& [typeId, field, rounded, live] :
        {std::tuple{"talas.iteratedWaveshaper", "iterations", "1 + !X", "1 + (!X * 0.6)"},
            std::tuple{"talas.clockDivider", "division", "2", "2 + (!X * 0.4)"}})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, live)});
        const auto whole = play(*type, {expression(field, rounded)});
        require(played.refusal.empty() && whole.refusal.empty(),
            std::string{typeId} + " should play a fractional live " + field + ": " + played.refusal);
        requireAlike(*type, played);
        require(played.offline.mono == whole.offline.mono,
            std::string{typeId} + " should round its live " + field);
    }
}

// Owner decision 2026-10-12: a live field that starts past its range
// saturates from the start: a StereoMix2 Scale1 of 1.2 that follows an Event
// Value plays as 1, an ADSR attack ramping up from zero starts at one sample.
// A static 1.2 is still refused.
void testLiveFieldsStartingPastTheirRangeSaturate()
{
    const auto* mix = registry().findNodeType("talas.stereoMix2");
    require(mix != nullptr, "StereoMix2 should be a catalog node");
    const auto past = play(*mix, {expression("scale1", "1.2 + (!X * 0.1)")});
    const auto full = play(*mix, {expression("scale1", "1.0")});
    require(past.refusal.empty() && full.refusal.empty(),
        "StereoMix2 should play a live Scale1 that starts past 1: " + past.refusal);
    requireAlike(*mix, past);
    require(past.offline.mono == full.offline.mono, "a live Scale1 starting past 1 should play as 1");
    const auto* adsr = registry().findNodeType("talas.adsr");
    require(adsr != nullptr, "ADSR should be a catalog node");
    const auto ramped = play(*adsr, {expression("attackTime", "(1 repeatingRamp: 0.005 s) * (10 ms)")});
    require(ramped.refusal.empty(), "ADSR should play an attack ramping up from zero: " + ramped.refusal);
    requireAlike(*adsr, ramped);
}

// The node patched as the harness plays it, heard inside a MIDIVoice of one
// voice when a key is given. Its audio sources are the live input, a tone
// of single-precision samples that starts `shift` frames into its cycle, so that a node playing alone can
// hear what a voice hears a frame later.
struct VoicePlayed
{
    std::string refusal;
    std::vector<double> realtime = std::vector<double>(frames);
    std::vector<double> offline;
};

std::vector<double> liveTone(std::size_t shift)
{
    std::vector<double> tone(frames + 1);
    for (std::size_t frame = 0; frame < tone.size(); ++frame)
        tone[frame] = static_cast<float>(
            0.5 * std::sin(2.0 * 3.141592653589793 * 437.5 * static_cast<double>(frame + shift) / 48000.0));
    return tone;
}

struct Voicing
{
    std::optional<std::uint8_t> key;
    std::uint8_t velocity = 100;
    std::optional<std::uint8_t> secondKey;
    double x = 0.0;
    std::size_t shift = 0;
};

VoicePlayed playInVoice(
    const talas::core::NodeTypeDescriptor& type, const std::vector<Parameter>& parameters, const Voicing& voicing)
{
    VoicePlayed played;
    auto [document, error] = patch(type, parameters, true);
    if (!error.empty())
    {
        played.refusal = error;
        return played;
    }
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    // The harness's tones become the live input, which a frame's shift
    // moves.
    for (auto& node : document.graph.nodes)
    {
        if (node.instanceId != "n" && node.typeId == "talas.oscillator")
        {
            node.typeId = "talas.audioInput";
            node.parameters.clear();
        }
    }
    if (voicing.key.has_value())
    {
        const auto heard = std::ranges::find_if(document.graph.connections, [](const auto& wire) {
            return wire.toNode == "out" && wire.toPort == "audioIn";
        });
        if (heard == document.graph.connections.end())
        {
            played.refusal = "no heard output";
            return played;
        }
        Parameter polyphony;
        polyphony.parameterId = "polyphony";
        polyphony.expression = "1";
        document.graph.nodes.push_back({"voice", "talas.midiVoice", {polyphony}});
        auto into = *heard;
        heard->fromNode = "voice";
        heard->fromPort = "leftOut";
        into.toNode = "voice";
        document.graph.connections.push_back(std::move(into));
    }
    const auto input = liveTone(voicing.shift);
    auto settings = talas::test::prototypes::settings(frames);
    for (auto& channel : settings.audioInputChannels)
        channel.samples.assign(input.begin(), input.begin() + frames);
    settings.eventValues.push_back({"KeyVelocity", 0.0});
    settings.eventValues.push_back({"KeyNumber", 60.0, talas::expr::ValueDomain::NoteNumber});
    settings.eventValues.push_back({"X", voicing.x});
    const auto compiled = talas::runtime::compileProject(document, registry(), settings);
    if (!compiled.ok())
    {
        played.refusal = "compile: " + compiled.diagnostics.front().message;
        return played;
    }
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    if (!session.ready())
    {
        played.refusal = "realtime: " + std::string{session.error()};
        return played;
    }
    if (voicing.key.has_value()
        && session.scheduleMidiNote({talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, *voicing.key, voicing.velocity}, 0)
            != talas::runtime::RealtimeMidiDispatchResult::Handled)
    {
        played.refusal = "realtime: the voice did not take the note";
        return played;
    }
    std::vector<float> block(blockFrames);
    const float* const channels[] = {block.data(), block.data()};
    std::vector<double> right(frames);
    for (std::size_t offset = 0; offset < frames; offset += blockFrames)
    {
        for (std::size_t frame = 0; frame < blockFrames; ++frame)
            block[frame] = static_cast<float>(input[offset + frame]);
        if (voicing.secondKey.has_value() && offset == frames / 2)
        {
            require(session.scheduleMidiNote({talas::runtime::RealtimeMidiNoteKind::NoteOff, 1, *voicing.key, 0}, 0)
                        == talas::runtime::RealtimeMidiDispatchResult::Handled
                    && session.scheduleMidiNote(
                           {talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, *voicing.secondKey, voicing.velocity}, 1)
                        == talas::runtime::RealtimeMidiDispatchResult::Handled,
                "the voice should take the second note");
        }
        if (!session.render({channels, 2}, std::span{played.realtime}.subspan(offset, blockFrames),
                std::span{right}.subspan(offset, blockFrames)))
        {
            played.refusal = "realtime: " + std::string{session.error()};
            return played;
        }
    }
    talas::runtime::OfflineRenderSession offline(*compiled.graph);
    std::vector<talas::runtime::OfflineMidiVoiceEvent> events;
    if (voicing.key.has_value())
    {
        talas::runtime::OfflineMidiVoiceEvent on;
        on.frame = 0;
        on.batchId = 1;
        on.sequence = 1;
        on.kind = talas::runtime::OfflineMidiVoiceEventKind::NoteOn;
        on.note = *voicing.key;
        on.velocity = voicing.velocity;
        events.push_back(on);
        if (voicing.secondKey.has_value())
        {
            auto off = on;
            off.frame = frames / 2;
            off.batchId = 2;
            off.sequence = 2;
            off.kind = talas::runtime::OfflineMidiVoiceEventKind::NoteOff;
            off.velocity = 0;
            events.push_back(off);
            auto second = on;
            second.frame = frames / 2 + 1;
            second.batchId = 3;
            second.sequence = 3;
            second.note = *voicing.secondKey;
            events.push_back(second);
        }
        require(offline.scheduleMidiVoiceEvents(events), type.typeId + " should take the notes offline");
    }
    auto rendered = offline.render(frames);
    if (!rendered.ok())
    {
        played.refusal = "offline: " + rendered.diagnostics.front().message;
        return played;
    }
    played.offline = std::move(rendered.mono);
    return played;
}

// A voice starts 5 ms after its note arrives, its KeyDown at -1 until then
//, and its Sound is set up afresh as it
// starts: from then on it plays what the node alone plays from its first
// frame, hearing the live input that far on.
constexpr std::size_t voiceStart = 240;

bool startsAsAlone(const std::vector<double>& voiced, const std::vector<double>& alone, std::size_t start = voiceStart)
{
    return std::equal(voiced.begin() + static_cast<std::ptrdiff_t>(start), voiced.end(), alone.begin());
}

// Owner decision 2026-10-12: a constant field inside a MIDIVoice is read
// once as each voice starts, with that voice's Event Values. A Low-Pass
// Gate's OpenFrequency of (!KeyNumber removeUnits) * 20 hz opens to 800 Hz
// for key 40 and to 2000 Hz for key 100, as it does alone with those
// frequencies, offline and in realtime; it played the compile's key 60
// (1200 Hz) for every note. A second note on the same voice reads its own
// key.
void testLowPassGateReadsEachVoicesKeyNumber()
{
    const auto* type = registry().findNodeType("talas.lowPassGate");
    require(type != nullptr, "LowPassGate should be a catalog node");
    const auto open = [](std::string frequency) {
        return std::vector<Parameter>{expression("control", "1"), expression("openFrequency", std::move(frequency))};
    };
    const auto keyed = open("(!KeyNumber removeUnits) * 20 hz");
    std::vector<std::vector<double>> heard;
    for (const auto& [key, frequency] : {std::pair{std::uint8_t{40}, "800 hz"}, std::pair{std::uint8_t{100}, "2000 hz"}})
    {
        const auto voiced = playInVoice(*type, keyed, {.key = key});
        const auto alone = playInVoice(*type, open(frequency), {.shift = voiceStart});
        require(voiced.refusal.empty() && alone.refusal.empty(),
            "a Low-Pass Gate should play its key's OpenFrequency in a voice: " + voiced.refusal + alone.refusal);
        require(voiced.offline == voiced.realtime,
            "a Low-Pass Gate in a voice should play alike offline and in realtime");
        require(startsAsAlone(voiced.realtime, alone.realtime),
            "key " + std::to_string(key) + " should open the Low-Pass Gate to " + frequency);
        heard.push_back(voiced.realtime);
    }
    require(heard[0] != heard[1], "keys 40 and 100 should sound different through the Low-Pass Gate");
    const auto lowThenHigh = playInVoice(*type, keyed, {.key = 40, .secondKey = 100});
    const auto highTwice = playInVoice(*type, keyed, {.key = 100, .secondKey = 100});
    require(lowThenHigh.refusal.empty() && highTwice.refusal.empty(),
        "a voice should play two notes through its Low-Pass Gate: " + lowThenHigh.refusal + highTwice.refusal);
    require(lowThenHigh.offline == lowThenHigh.realtime,
        "two notes through a Low-Pass Gate should play alike offline and in realtime");
    // The second note, a frame after the first note's half, starts its voice
    // afresh as the first of key 100 did.
    constexpr auto secondStart = frames / 2 + 1 + voiceStart;
    require(!std::equal(lowThenHigh.realtime.begin(), lowThenHigh.realtime.begin() + secondStart,
                highTwice.realtime.begin()),
        "the voice's first notes should differ");
    require(std::equal(lowThenHigh.realtime.begin() + secondStart, lowThenHigh.realtime.end(),
                highTwice.realtime.begin() + secondStart),
        "the voice's second note should read its own key");
}

// 2026-10-13: a vocoder's NbrBands, a SumOfSines's AnalysisFrames and an
// AntialiasedOscillator's DutyCycle are read as each voice starts, within
// what the voice prepared: the bank keeps room for 64 bands, the analysis
// is kept whole, the cycle's transform is planned. A voice's second note
// that grows or shrinks the band count, the frames or the duty cycle plays
// as the same note played twice, offline and in realtime alike.
void testAVoiceResizesWhatItPrepared()
{
    for (const auto& [typeId, parameters] :
        {std::pair{"talas.vocoder", std::vector<Parameter>{expression("nbrBands", "(!KeyNumber removeUnits) / 5")}},
            std::pair{"talas.tunableVocoder", std::vector<Parameter>{expression("nbrBands", "(!KeyNumber removeUnits) / 5")}},
            std::pair{"talas.scaleVocoder", std::vector<Parameter>{expression("nbrBands", "(!KeyNumber removeUnits) / 10")}},
            std::pair{"talas.sumOfSines", std::vector<Parameter>{expression("nbrPartials", "4"),
                expression("analysisFrames0", "(!KeyNumber removeUnits) / 50")}},
            std::pair{"talas.antialiasedOscillator", std::vector<Parameter>{expression("dutyCycle", "(!KeyNumber removeUnits) / 200")}}})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        constexpr auto secondStart = frames / 2 + 1 + voiceStart;
        for (const auto& [first, second] : {std::pair{std::uint8_t{40}, std::uint8_t{100}}, std::pair{std::uint8_t{100}, std::uint8_t{40}}})
        {
            const auto changed = playInVoice(*type, parameters, {.key = first, .secondKey = second});
            const auto twice = playInVoice(*type, parameters, {.key = second, .secondKey = second});
            require(changed.refusal.empty() && twice.refusal.empty(),
                std::string{typeId} + " should play two notes in a voice: " + changed.refusal + twice.refusal);
            require(changed.offline == changed.realtime && twice.offline == twice.realtime,
                std::string{typeId} + " should play two notes alike offline and in realtime");
            require(!std::equal(changed.realtime.begin(), changed.realtime.begin() + secondStart,
                        twice.realtime.begin()),
                std::string{typeId} + "'s first notes should differ");
            require(std::equal(changed.realtime.begin() + secondStart, changed.realtime.end(),
                        twice.realtime.begin() + secondStart),
                std::string{typeId} + "'s second note should set the voice up for its own key");
        }
    }
}

// Projects before format 91 read a constant field inside a MIDIVoice once,
// as the Sound compiles: their MIDIVoice acquires constantFieldsRead
// legacyWhenCompiled, and keys 40 and 100 open the Low-Pass Gate alike.
void testOlderProjectsReadConstantFieldsWhenCompiled()
{
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    Parameter polyphony;
    polyphony.parameterId = "polyphony";
    polyphony.expression = "1";
    document.graph.nodes.push_back({"in", "talas.audioInput", {}});
    document.graph.nodes.push_back({"gate", "talas.lowPassGate",
        {expression("control", "1"), expression("openFrequency", "(!KeyNumber removeUnits) * 20 hz")}});
    document.graph.nodes.push_back({"voice", "talas.midiVoice", {polyphony}});
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({"in", "audioOut", "gate", "audioIn"});
    document.graph.connections.push_back({"gate", "audioOut", "voice", "audioIn"});
    document.graph.connections.push_back({"voice", "leftOut", "out", "audioIn"});
    auto json = talas::project::serializeProjectDocument(document);
    const auto version = json.find("\"version\": 156");
    require(version != std::string::npos, "a project should save as format 91");
    json.replace(version, std::string_view{"\"version\": 156"}.size(), "\"version\":  90");
    const auto migrated = talas::project::parseProjectDocument(json);
    require(migrated.ok(), "a format 90 project should open");
    const auto& voice = *std::ranges::find(migrated.document.graph.nodes, std::string{"voice"},
        &talas::project::ProjectNode::instanceId);
    require(std::ranges::any_of(voice.parameters, [](const Parameter& parameter) {
                return parameter.parameterId == "constantFieldsRead"
                    && parameter.choice == std::optional<std::string>{"legacyWhenCompiled"};
            }),
        "a format 90 MIDIVoice should read its constant fields when the Sound compiles");
    auto settings = talas::test::prototypes::settings(frames);
    settings.eventValues.push_back({"KeyNumber", 60.0, talas::expr::ValueDomain::NoteNumber});
    const auto play = [&](const talas::project::ProjectDocument& project, std::uint8_t key) {
        const auto compiled = talas::runtime::compileProject(project, registry(), settings);
        require(compiled.ok(), "the voiced Low-Pass Gate should compile: "
                + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), "the voiced Low-Pass Gate should prepare: " + std::string{session.error()});
        require(session.scheduleMidiNote({talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, key, 100}, 0)
                == talas::runtime::RealtimeMidiDispatchResult::Handled,
            "the voice should take the key");
        const auto input = liveTone(0);
        std::vector<float> block(blockFrames);
        const float* const channels[] = {block.data(), block.data()};
        std::vector<double> left(frames);
        std::vector<double> right(frames);
        for (std::size_t offset = 0; offset < frames; offset += blockFrames)
        {
            for (std::size_t frame = 0; frame < blockFrames; ++frame)
                block[frame] = static_cast<float>(input[offset + frame]);
            require(session.render({channels, 2}, std::span{left}.subspan(offset, blockFrames),
                        std::span{right}.subspan(offset, blockFrames)),
                "the voiced Low-Pass Gate should play: " + std::string{session.error()});
        }
        return left;
    };
    require(play(migrated.document, 40) == play(migrated.document, 100),
        "a format 90 voice should open its Low-Pass Gate to the compile's key for every note");
    require(play(document, 40) != play(document, 100),
        "a format 91 voice should open its Low-Pass Gate to each note's key");
}

// A callback that fails after a voice started leaves the voice's setup as
// it was: played again, the voice starts afresh with its key, as a callback
// that never failed does. The voice plays through a Level of exactly 1 whose
// gain overflows for the failed callback.
void testAFailedCallbackStartsTheVoiceAgain()
{
    const auto played = [](bool failFirst) {
        talas::project::ProjectDocument document;
        document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
        Parameter polyphony;
        polyphony.parameterId = "polyphony";
        polyphony.expression = "1";
        document.graph.nodes.push_back({"in", "talas.audioInput", {}});
        document.graph.nodes.push_back({"gate", "talas.lowPassGate",
            {expression("control", "1"), expression("openFrequency", "(!KeyNumber removeUnits) * 20 hz"),
                expression("attackTime", "(!KeyNumber removeUnits) * 0.0001 s")}});
        document.graph.nodes.push_back({"voice", "talas.midiVoice", {polyphony}});
        document.graph.nodes.push_back({"gain", "talas.level",
            {expression("left", "!Gain * 4"), expression("right", "!Gain * 4")}});
        document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
        document.graph.connections.push_back({"in", "audioOut", "gate", "audioIn"});
        document.graph.connections.push_back({"gate", "audioOut", "voice", "audioIn"});
        document.graph.connections.push_back({"voice", "leftOut", "gain", "audioIn"});
        document.graph.connections.push_back({"gain", "leftOut", "out", "audioIn"});
        auto settings = talas::test::prototypes::settings(frames);
        settings.eventValues.push_back({"Gain", 0.25});
        settings.eventValues.push_back({"KeyNumber", 60.0, talas::expr::ValueDomain::NoteNumber});
        const auto compiled = talas::runtime::compileProject(document, registry(), settings);
        require(compiled.ok(), "a voiced Low-Pass Gate should compile: "
                + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), "a voiced Low-Pass Gate should prepare: " + std::string{session.error()});
        const auto input = liveTone(0);
        std::vector<float> block(blockFrames);
        const float* const channels[] = {block.data(), block.data()};
        std::vector<double> left(frames);
        std::vector<double> right(frames);
        for (std::size_t offset = 0; offset < frames; offset += blockFrames)
        {
            for (std::size_t frame = 0; frame < blockFrames; ++frame)
                block[frame] = static_cast<float>(input[offset + frame]);
            if (offset == blockFrames)
            {
                require(session.scheduleMidiNote({talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, 40, 100}, 0)
                        == talas::runtime::RealtimeMidiDispatchResult::Handled,
                    "the voice should take the key");
                if (failFirst)
                {
                    require(session.updateEventValue("Gain", std::numeric_limits<double>::max(),
                                talas::expr::ValueDomain::Scalar)
                            && !session.render({channels, 2}, std::span{left}.subspan(offset, blockFrames),
                                std::span{right}.subspan(offset, blockFrames))
                            && session.updateEventValue("Gain", 0.25, talas::expr::ValueDomain::Scalar),
                        "an overflowing gain should fail the callback in which the voice starts");
                }
            }
            require(session.render({channels, 2}, std::span{left}.subspan(offset, blockFrames),
                        std::span{right}.subspan(offset, blockFrames)),
                "the voiced Low-Pass Gate should play: " + std::string{session.error()});
        }
        return left;
    };
    const auto clean = played(false);
    require(std::ranges::any_of(clean, [](double sample) { return sample != 0.0; }),
        "the voiced Low-Pass Gate should sound");
    require(played(true) == clean,
        "a failed callback should leave the voice to start afresh with its key when played again");
}

// Owner decision 2026-10-12: a constant field inside a MIDIVoice is read
// once as each voice starts, with that voice's
// Event Values. Each constant field moved by !KeyVelocity plays, for notes
// struck at velocities 32 and 96 (the default and twice it; the compile reads
// half of it), as the node plays alone with the same value from an Event
// Value it starts with, offline and in realtime alike. A field that fixes the
// Sound's latency, structure or processor when it compiles, or a count our
// renderers hold, is refused by name instead.
void testConstantFieldsReadEachVoicesEventValues()
{
    // Fields set so that the constant field is heard, by node or by field;
    // a time function repeats every 5 ms, so a voice hears it as alone.
    const std::map<std::string_view, std::vector<Parameter>> heardWith{
        {"talas.lowPassGate", {expression("control", "1")}},
        {"talas.euSeq", {expression("stepRate", "2000 hz")}},
        {"talas.grainCloud", {expression("density", "5000")}},
        {"talas.sampleCloud", {expression("density", "5000")}},
        {"talas.pluckedWaveguide", {expression("trigger", "1")}},
        {"talas.sumOfSines.analysisFrequency1", {expression("pchMorph", "1")}},
        // Four partials fit a frame of the eight-harmonic analysis halved.
        {"talas.sumOfSines.analysisFrames0", {expression("nbrPartials", "4")}},
        {"talas.sumOfSines.analysisFrames1", {expression("dbMorph", "1"), expression("nbrPartials", "4")}},
    };
    // A field whose default is zero moves from another value.
    const std::map<std::string_view, std::string_view> movedFrom{
        {"talas.noise.initialState", "0.25"},
        // Within the four partials its arrays hold.
        {"talas.syntheticSpectrumFromArray.nbrPartials", "2"},
    };
    // Fields the harness's patch does not hear, each for a reason: they
    // still read the voice's value and set the voice up afresh.
    const std::set<std::string_view> notHeardHere{
        // Only scales a modulator, which the harness does not connect.
        "talas.oscillator.maxMI",
        // Only bound the pitch the Sound expects of its input.
        "talas.simplePitchShifter.maxInputPitch",
        "talas.simplePitchShifter.minInputPitch",
        "talas.timeFrequencyScale.maxInputFreq",
        // The harness holds the gate open.
        "talas.lowPassGate.releaseTime",
        // Only shape the frequencies, which the harness does not hear.
        "talas.euSeq.scaleSteps",
        "talas.euSeq.stepsPerOctave",
        // Its 4095-frame latency outlasts the test.
        "talas.diffuseSpectralStretch.diffusion",
        "talas.diffuseSpectralStretch.seed",
        "talas.diffuseSpectralStretch.smear",
        "talas.diffuseSpectralStretch.stretch",
        // Only spreads the bank over the signal processors.
        "talas.scaleVocoder.bankSize",
        "talas.tunableVocoder.bankSize",
        "talas.vocoder.bankSize",
    };
    // Fields that fix a latency, a structure or a processor when the Sound
    // compiles, or a count our renderers hold.
    const std::set<std::string_view> setWhenCompiled{
        "talas.cdpLowerResolution.sampleRateDivision",
        "talas.cellularAutomaton.iterations",
        "talas.cellularAutomaton.rule",
        "talas.cloudBank.bankSize",
        "talas.cloudBank.grainsPerTrack",
        "talas.cloudBank.nbrTracks",
        "talas.cloudBank.seed",
        "talas.cloudBankElement.count",
        "talas.cloudBankElement.first",
        "talas.cloudBankElement.initialDelay",
        "talas.cloudBankElement.maxGrains",
        "talas.cloudBankElement.seed",
        "talas.contextFreeGrammar.left",
        "talas.contextFreeGrammar.maxRewrites",
        "talas.contextFreeGrammar.maxSize",
        "talas.contextFreeGrammar.right",
        "talas.contextFreeGrammar.seed",
        "talas.feedbackLoopOutput.delay",
        "talas.filterBank.bankSize",
        "talas.filterBank.nbrFilters",
        "talas.filterBankElement.count",
        "talas.filterBankElement.first",
        "talas.forcedProcessorAssignment.processor",
        "talas.formantBank.bankSize",
        "talas.formantBank.nbrFormants",
        "talas.formantBank.nbrImpulses",
        "talas.formantBankOscillator.firstFormant",
        "talas.formantBankOscillator.nbrFormants",
        "talas.formantBankOscillator.nbrImpulses",
        "talas.frameSynchronizer.frameLength",
        "talas.frequencyScale.maxScale",
        "talas.frequencyTracker.detectors",
        "talas.harmonicSpectrum.amplitude",
        "talas.harmonicSpectrum.frequency",
        "talas.harmonicSpectrum.nbrPartials",
        "talas.liveSpectralAnalysis.highFreq",
        "talas.liveSpectralAnalysis.initFreq",
        "talas.liveSpectralAnalysis.lowFreq",
        "talas.liveSpectralAnalysis.trackedHarmonic",
        "talas.liveSpectralAnalysis.unpitchedOnly",
        "talas.liveSpectralAnalysis.unpitchedThreshold",
        "talas.oscillatorBank.bankSize",
        "talas.oscillatorBank.nbrOscillators",
        "talas.oscilloscopeDisplay.initialSweepFrequency",
        "talas.oscilloscopeDisplay.level",
        "talas.oscilloscopeDisplay.sweepFreq",
        "talas.overlappingMixer.overlapTime",
        "talas.partialSpectrumTrackSelectorV1.track",
        "talas.replicator.number",
        "talas.rhythmicCellularAutomaton.generationLength",
        "talas.rhythmicCellularAutomaton.generations",
        "talas.rhythmicCellularAutomaton.rule",
        "talas.sosOscillators.firstOscillator",
        "talas.sosOscillators.nbrOscillators",
        "talas.spectralShape.nbrPartials",
        "talas.spectrumFromSingleCycle.frequency",
        "talas.spectrumInRam.analysisFrames",
        "talas.spectrumInRam.analysisFrequency",
        "talas.spectrumInRam.firstPartial",
        "talas.spectrumInRam.nbrPartials",
        "talas.spectrumModifier.seed",
        "talas.spectrumOnDisk.analysisFrames",
        "talas.spectrumOnDisk.analysisFrequency",
        "talas.spectrumOnDisk.firstPartial",
        "talas.spectrumOnDisk.nbrPartials",
        "talas.spectrumTrackSelector.track",
        "talas.syntheticSpectrumFromSounds.nbrPartials",
        "talas.timeFrequencyScale.detectors",
        "talas.timeFrequencyScale.maxFreqScale",
        "talas.timeFrequencyScale.minInputFreq",
        "talas.vocoderChannelBank.count",
        "talas.vocoderChannelBank.first",
        "talas.waveTerrainOscillator.height",
        "talas.waveTerrainOscillator.width",
    };
    std::string playing;
    std::string refusals;
    std::string failures;
    std::size_t perVoice = 0;
    std::size_t refused = 0;
    for (const auto& [typeId, field] : talas::runtime::start_only_fields::fields)
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        // A MIDIVoice, MIDIMapper or AnalogSequencer inside a live MIDIVoice
        // is refused, and a MultisegmentEnvelope names its loop points
        // (below).
        if (!playable(*type) || typeId == "talas.midiVoice" || typeId == "talas.midiMapper"
            || typeId == "talas.analogSequencer" || typeId == "talas.multisegmentEnvelope")
            continue;
        const auto descriptor = std::ranges::find(type->parameters, field, &talas::core::ParameterDescriptor::id);
        const auto name = std::string{typeId} + "." + std::string{field};
        const auto base = "("
            + (movedFrom.contains(name) ? std::string{movedFrom.at(name)}
                    : descriptor->defaultExpression.empty() ? std::string{"1"}
                                                            : descriptor->defaultExpression)
            + ")";
        const auto fixed = setWhenCompiled.contains(name);
        const auto withField = [&](std::string text) {
            auto parameters = heardWith.contains(name) ? heardWith.at(name)
                : heardWith.contains(typeId)           ? heardWith.at(typeId)
                                                       : std::vector<Parameter>{};
            parameters.push_back(expression(std::string{field}, std::move(text)));
            return parameters;
        };
        std::vector<std::vector<double>> heard;
        bool fieldFailed = false;
        for (const std::uint8_t velocity : {std::uint8_t{32}, std::uint8_t{96}})
        {
            const auto voiced = playInVoice(*type, withField(base + " * (0.5 + (!KeyVelocity * 127 / 64))"),
                {.key = 60, .velocity = velocity});
            if (fixed)
            {
                // The harness cannot hear a FeedbackLoopOutput beside its
                // FeedbackLoopInput's own output.
                if (voiced.refusal.find("is set when the Sound compiles") == std::string::npos
                    && typeId != "talas.feedbackLoopOutput")
                {
                    failures += "  " + name + " should be refused by name in a voice: "
                        + (voiced.refusal.empty() ? std::string{"it played"} : voiced.refusal) + "\n";
                }
                else
                {
                    ++refused;
                    refusals += " " + name;
                }
                fieldFailed = true;
                break;
            }
            const auto alone = playInVoice(*type, withField(base + " * (0.5 + (!X * 127 / 64))"),
                {.x = static_cast<double>(velocity) / 127.0, .shift = voiceStart});
            if (!voiced.refusal.empty())
                failures += "  " + name + " should play in a voice: " + voiced.refusal + "\n";
            else if (!alone.refusal.empty())
                failures += "  " + name + " should play its note's value alone: " + alone.refusal + "\n";
            else if (voiced.offline != voiced.realtime)
                failures += "  " + name + " should play alike offline and in realtime in a voice\n";
            else if (!startsAsAlone(voiced.realtime, alone.realtime))
                failures += "  " + name + " should read velocity " + std::to_string(velocity) + " as its voice starts\n";
            else
            {
                heard.push_back(alone.realtime);
                continue;
            }
            fieldFailed = true;
            break;
        }
        if (fieldFailed)
            continue;
        if (heard[0] == heard[1] && !notHeardHere.contains(name))
            failures += "  " + name + " should sound different for the two notes\n";
        else if (heard[0] != heard[1] && notHeardHere.contains(name))
            failures += "  " + name + " is heard now; remove it from notHeardHere\n";
        else
        {
            ++perVoice;
            playing += " " + name;
        }
    }
    std::cout << perVoice << " constant fields read each voice's Event Values:" << playing << "\n"
              << refused << " are refused by name in a voice:" << refusals << "\n";
    require(failures.empty(), "constant fields inside a MIDIVoice should read each voice's Event Values:\n" + failures);
}

// The fields CDP8 takes as fixed values, not breakpoint files, are constant
// fields too (2026-10-19, taken as recommended by analogy with the owner's
// 2026-10-12 decision): given a time function or an Event Value, one plays
// its starting value in both renderers, where both used to refuse it; inside
// a MIDIVoice a moving one is refused by name; and a starting value outside
// CDP8's range is still refused.
void testCdpFixedFieldsPlayTheirStartingValue()
{
    std::string failures;
    std::size_t played = 0;
    for (const auto& [typeId, field] : talas::runtime::start_only_fields::cdpFixedFields)
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto descriptor = std::ranges::find(type->parameters, field, &talas::core::ParameterDescriptor::id);
        require(descriptor != type->parameters.end(), std::string{typeId} + "." + std::string{field} + " should be a field");
        if (!playable(*type))
            continue;
        const auto name = std::string{typeId} + "." + std::string{field};
        const auto base = "(" + (descriptor->defaultExpression.empty() ? std::string{"0"} : descriptor->defaultExpression) + ")";
        const auto held = play(*type, {expression(std::string{field}, base)});
        for (const std::string change : {"(!X * 0.2)", "((1 repeatingRamp: 0.005 s) * 0.2)"})
        {
            const auto moving = play(*type, {expression(std::string{field}, base + " * (1 + " + change + ")")});
            if (!moving.refusal.empty() || !held.refusal.empty())
                failures += "  " + name + " should play a moving value: " + moving.refusal + held.refusal + "\n";
            else if (!difference(moving).empty())
                failures += "  " + name + " should play alike offline and in realtime: " + difference(moving) + "\n";
            else if (moving.offline.mono != held.offline.mono || moving.left != held.left)
                failures += "  " + name + " should play its starting value\n";
            else
                ++played;
        }
        const auto voiced = playInVoice(*type, {expression(std::string{field}, base + " * (0.5 + (!KeyVelocity * 127 / 64))")},
            {.key = 60, .velocity = 32});
        if (voiced.refusal.find("is set when the Sound compiles") == std::string::npos)
            failures += "  " + name + " should be refused by name in a voice: "
                + (voiced.refusal.empty() ? std::string{"it played"} : voiced.refusal) + "\n";
    }
    std::cout << played << " moving CDP fixed fields play their starting value\n";
    require(failures.empty(), "CDP fixed fields should play their starting value:\n" + failures);
    // A starting value past CDP8's range is still refused: a Tap Delay's
    // Mix from 0 to 1.
    const auto* tapDelay = registry().findNodeType("talas.cdpTapDelay");
    require(tapDelay != nullptr && !play(*tapDelay, {expression("mix", "1.5 * (1 + (!X * 0.2))")}).refusal.empty()
            && play(*tapDelay, {expression("mix", "0.75 * (1 + (!X * 0.2))")}).refusal.empty(),
        "a CDP field whose starting value is past CDP8's range should still be refused");
}

// Owner decision 2026-10-12, taken as recommended: a vocoder's AttackTime
// or ReleaseTime of -1 inherits its TimeConstant, and a live
// value off -1 saturates at zero rather than stopping the Sound. Both
// renderers set the envelope times as the vocoder starts: one that starts
// at -1 and moves later inherits, one that starts below zero plays as 0 s.
void testVocoderEnvelopeTimesInheritOrSaturate()
{
    for (const auto* typeId : {"talas.vocoder", "talas.tunableVocoder", "talas.scaleVocoder", "talas.vocoderChannelBank"})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        for (const auto* field : {"attackTime", "releaseTime"})
        {
            const auto moved = play(*type, {expression(field, "(-1 s) * (1 + (!X * 0.2))")});
            const auto inherited = play(*type, {expression(field, "-1 s")});
            const auto below = play(*type, {expression(field, "(-1.2 s) + (!X * (0.1 s))")});
            const auto zero = play(*type, {expression(field, "0 s")});
            require(moved.refusal.empty() && inherited.refusal.empty() && below.refusal.empty()
                    && zero.refusal.empty(),
                std::string{typeId} + " should play a live " + field + " off -1: " + moved.refusal + below.refusal);
            requireAlike(*type, moved);
            requireAlike(*type, below);
            require(moved.offline.mono == inherited.offline.mono,
                std::string{typeId} + " should inherit a " + field + " that starts at -1");
            require(below.offline.mono == zero.offline.mono,
                std::string{typeId} + " should saturate a " + field + " below zero at 0 s");
        }
    }
}

// Owner decision 2026-10-12, taken as recommended: a live value whose range
// depends on another field saturates against that field's current value.
// A TimeIndex's End falling below its ReleaseStart stops there, and a
// SetRange's OldMax reaching OldMin stays a fixed-point step above it, in
// both renderers alike; both stopped the Sound.
void testCrossFieldRangesSaturate()
{
    for (const auto& [typeId, field, text] :
        {std::tuple{"talas.timeIndex", "end", "(1.0) * (1 - (!X * 0.8))"},
            std::tuple{"talas.timeIndex", "start", "(1 repeatingRamp: 0.005 s) * 1.5"},
            std::tuple{"talas.setRange", "oldMax", "(1.0) * (1 - !X)"}})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, text)});
        require(played.refusal.empty(),
            std::string{typeId} + " should play a " + field + " that passes another field: " + played.refusal);
        requireAlike(*type, played);
    }
}

// Owner decision 2026-10-12, taken as recommended on 2026-10-13: bounds that
// pass what they are bound by saturate against it in both renderers. A
// Monotonizer's MaxInputPitch below MinInputPitch closes the range on that
// pitch, its MinInputPitch rises to the lowest the analysis tracks and its
// Frequency stays within an octave of a known pitch; a FrequencyTracker's
// MinFrequency rises to the lowest its history holds and MaxFrequency stays
// above it; a Vocoder's HighCF stays above LowCF and HiCutoff stops at
// LoCutoff; a Gate's HighThreshold stays a fixed-point step above
// LowThreshold; an AnalysisFilter's Fundamental and an AveragingLowPass
// Cutoff rise to the lowest whose period the history holds; a DiskPlayer's
// RateScale of zero plays at the smallest step. All of these stopped the
// Sound.
void testBoundsThatPassAnotherFieldSaturate()
{
    for (const auto& [typeId, field, text] :
        {std::tuple{"talas.monotonizer", "maxInputPitch", "(440 hz) * (1 - !X)"},
            std::tuple{"talas.monotonizer", "minInputPitch", "(440 hz) * (1 + (!X * 0.2)) + (!X * 3.71 hz)"},
            std::tuple{"talas.monotonizer", "frequency", "(440 hz) * (1 - !X)"},
            std::tuple{"talas.frequencyTracker", "minFrequency", "(20 hz) * (1 - !X)"},
            std::tuple{"talas.frequencyTracker", "maxFrequency", "(2000 hz) * (1 - !X)"},
            std::tuple{"talas.vocoder", "highCF", "(5000 hz) * (1 - !X)"},
            std::tuple{"talas.vocoder", "hiCutoff", "(10000 hz) * (1 - !X)"},
            std::tuple{"talas.gate", "highThreshold", "(0.75) * (1 - !X)"},
            std::tuple{"talas.analysisFilter", "fundamental", "(440 hz) * (1 - !X)"},
            std::tuple{"talas.averagingLowPassFilter", "cutoff", "(1000 hz) * (1 - !X)"},
            std::tuple{"talas.diskPlayer", "rateScale", "(1.0) * (1 - !X)"},
            std::tuple{"talas.multifileDiskPlayer", "rateScale", "(1.0) * (1 - !X)"}})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, text)});
        require(played.refusal.empty(),
            std::string{typeId} + " should play a " + field + " that passes its bound: " + played.refusal);
        requireAlike(*type, played);
    }
    // A HiCutoff below LoCutoff plays as one at LoCutoff, and a HighThreshold
    // below LowThreshold as one a fixed-point step above it.
    for (const auto& [typeId, field, below, at] :
        {std::tuple{"talas.vocoder", "hiCutoff", "(!X * (0 hz)) + 1 hz", "50 hz"},
            std::tuple{"talas.gate", "highThreshold", "(!X * 0) + 0.1", "0.25 + (2 / 16777216)"}})
    {
        const auto* type = registry().findNodeType(typeId);
        const auto saturated = play(*type, {expression(field, below)});
        const auto edge = play(*type, {expression(field, at)});
        require(saturated.refusal.empty() && edge.refusal.empty(),
            std::string{typeId} + " should play a " + field + " below its bound: " + saturated.refusal + edge.refusal);
        requireAlike(*type, saturated);
        require(saturated.offline.mono == edge.offline.mono,
            std::string{typeId} + " should play a " + field + " below its bound as one at it");
    }
}

// Without VariableDutyCycle a PulseTrain is 1 for one sample a
// period, whatever its DutyCycle; realtime ignores a time function there as
// offline does, and follows it once VariableDutyCycle is checked.
void testPulseTrainIgnoresDutyCycleUntilVariable()
{
    const auto* type = registry().findNodeType("talas.pulseTrain");
    require(type != nullptr, "PulseTrain should be a catalog node");
    for (const auto variable : {false, true})
    {
        Parameter variableDutyCycle;
        variableDutyCycle.parameterId = "variableDutyCycle";
        variableDutyCycle.boolean = variable;
        const auto played = play(*type, {expression("dutyCycle", "(1 repeatingRamp: 0.005 s) * 0.5"),
                                            variableDutyCycle, expression("period", "1 ms")});
        require(played.refusal.empty(),
            std::string{"PulseTrain should play a timed DutyCycle "} + (variable ? "with" : "without")
                + " VariableDutyCycle: " + played.refusal);
        requireAlike(*type, played);
    }
}

// The envelope, Trigger and Frequency of a GenericSource shape
// sample sources only, so one playing the live input ignores a time function
// in them in realtime as offline.
void testLiveGenericSourceIgnoresItsSampleFields()
{
    const auto* type = registry().findNodeType("talas.genericSource");
    require(type != nullptr, "GenericSource should be a catalog node");
    for (const auto* field : {"trigger", "attackTime", "releaseTime"})
    {
        const auto played = play(*type, {expression(field,
            std::string{field} == "trigger" ? "1 repeatingRamp: 0.005 s" : "(1 repeatingRamp: 0.005 s) * (0.01 s)")});
        require(played.refusal.empty(),
            std::string{"a live-input GenericSource should play a timed "} + field + ": " + played.refusal);
        requireAlike(*type, played);
    }
}

// A SumOfSines's BankSize only spreads its oscillators over the
// signal processor, and its loop points matter only when it loops; realtime
// ignores a time function in them as offline does.
void testSumOfSinesIgnoresBankSizeAndIdleLoopPoints()
{
    const auto* type = registry().findNodeType("talas.sumOfSines");
    require(type != nullptr, "SumOfSines should be a catalog node");
    for (const auto& [field, text] : {std::pair{"bankSize", "8 + (1 repeatingRamp: 0.005 s)"},
             std::pair{"loopStart", "(1 repeatingRamp: 0.005 s) * 0.5"},
             std::pair{"loopEnd", "1 - ((1 repeatingRamp: 0.005 s) * 0.5)"}})
    {
        const auto played = play(*type, {expression(field, text)});
        require(played.refusal.empty(),
            std::string{"SumOfSines should play a timed "} + field + ": " + played.refusal);
        requireAlike(*type, played);
    }
}

// CDP8's `modify brassage` mode 1 (pitchshift) takes only a pitch, so a
// Brassage in that mode ignores a time function in its density, amp,
// grainsize and range in realtime as offline.
void testBrassageIgnoresTimedFieldsItsModeDoesNotTake()
{
    const auto* type = registry().findNodeType("talas.cdpBrassage");
    require(type != nullptr, "CDP Brassage should be a catalog node");
    for (const auto* field : {"density", "amp", "grainsize", "range"})
    {
        const auto parameter = std::ranges::find(type->parameters, std::string_view{field},
            &talas::core::ParameterDescriptor::id);
        require(parameter != type->parameters.end(), std::string{"CDP Brassage should have a "} + field);
        const auto played = play(*type,
            {expression(field, "(" + parameter->defaultExpression + ") * (1 - ((1 repeatingRamp: 0.005 s) * 0.2))")});
        require(played.refusal.empty(),
            std::string{"a pitchshift Brassage should play a timed "} + field + ": " + played.refusal);
        requireAlike(*type, played);
    }
}

// Only CDP8's `distort reform` mode 8 (exaggerate) reads its exaggeration
// as it plays; in the other modes a change after the start is ignored in
// realtime as offline, even one that leaves CDP's range.
void testDistortReformIgnoresAnExaggerationItsModeDoesNotRead()
{
    const auto* type = registry().findNodeType("talas.cdpDistortReform");
    require(type != nullptr, "CDP Distort Reform should be a catalog node");
    const auto played = play(*type, {expression("exaggeration", "2 * (1 - !X)")});
    require(played.refusal.empty(),
        "a square Distort Reform should ignore a changed exaggeration: " + played.refusal);
    requireAlike(*type, played);
}

// An IteratedWaveshaper's Iterations and a SumOfSines's NbrPartials take a
// Sound as well as a number, and offline follows a change to
// either; realtime played the count it started with.
void testRealtimeFollowsALiveCount()
{
    for (const auto& [typeId, field, steady, live] :
        {std::tuple{"talas.iteratedWaveshaper", "iterations", "1", "1 + !X"},
            std::tuple{"talas.sumOfSines", "nbrPartials", "4", "4 + (!X * 4)"}})
    {
        const auto* type = registry().findNodeType(typeId);
        require(type != nullptr, std::string{typeId} + " should be a catalog node");
        const auto played = play(*type, {expression(field, live)});
        const auto held = play(*type, {expression(field, steady)});
        require(played.refusal.empty() && held.refusal.empty(),
            std::string{typeId} + " should play a live " + field + ": " + played.refusal);
        require(played.offline.mono != held.offline.mono,
            std::string{typeId} + " should follow its " + field + " offline");
        requireAlike(*type, played);
    }
}

// A SumOfSines's live NbrPartials past the eight partials of its analysis
// (the computed HarmonicAmplitudes) plays all eight, in both renderers, as a
// live value past its range saturates.
void testSumOfSinesPlaysTheAnalysisPartialsAtMost()
{
    const auto* type = registry().findNodeType("talas.sumOfSines");
    require(type != nullptr, "SumOfSines should be a catalog node");
    const auto past = play(*type, {expression("nbrPartials", "8 + (!X * 4)"), expression("frequency0", "220 hz")});
    const auto all = play(*type, {expression("nbrPartials", "8"), expression("frequency0", "220 hz")});
    require(past.refusal.empty() && all.refusal.empty(),
        "SumOfSines should play a NbrPartials past its analysis's partials: " + past.refusal + all.refusal);
    requireAlike(*type, all);
    requireAlike(*type, past);
    require(past.offline.mono == all.offline.mono, "a NbrPartials past the analysis should play all its partials");
}

// The metronome idiom: `1 bpm: 3000` gates an ADSR
// every 20 ms, half of it on, its first 5 ms the -1 that steals the voice
//, and the envelope rises and releases on every beat in both
// renderers.
void testAdsrFollowsAMetronomeGate()
{
    const auto* type = registry().findNodeType("talas.adsr");
    require(type != nullptr, "ADSR should be a catalog node");
    const auto played = play(*type, {
        expression("attackTime", "1 ms"),
        expression("decayTime", "1 ms"),
        expression("sustainLevel", "0.5"),
        expression("releaseTime", "1 ms"),
        expression("gate", "1 bpm: 3000"),
    });
    require(played.refusal.empty(), "ADSR should play a metronome gate: " + played.refusal);
    requireAlike(*type, played);
    constexpr std::size_t beat = 960;
    for (std::size_t start = 0; start + beat <= frames; start += beat)
    {
        require(played.offline.mono[start + 312] > 0.45,
            "the ADSR should sustain 1.5 ms after the beat's -1 at frame " + std::to_string(start));
        require(played.offline.mono[start + 696] < 0.01,
            "the ADSR should have released 4.5 ms after the beat at frame " + std::to_string(start));
    }
}

// ValToSound turns a Val expression into a signal: a repeating
// ramp becomes a sawtooth, sampled on millisecond ticks and interpolated.
void testValToSoundPlaysARepeatingRamp()
{
    const auto* type = registry().findNodeType("talas.valToSound");
    require(type != nullptr, "ValToSound should be a catalog node");
    const auto played = play(*type, {expression("value", "1 repeatingRamp: 0.01 s")});
    require(played.refusal.empty(),
        "ValToSound should play a repeating ramp: " + played.refusal);
    requireAlike(*type, played);
    const auto& signal = played.offline.mono;
    require(signal[240] > 0.3 && signal[240] < 0.7,
        "the sawtooth should be halfway up 5 ms into its cycle");
    require(signal[480 + 48] < 0.2,
        "the sawtooth should start again after 10 ms");
}
}

int main()
{
    try
    {
        testIgnoredEventValueChangesNameCatalogParameters();
        testFieldListsNameCatalogParameters();
        testGraphicEqBandLeavesUnityLikeRealtime();
        testHarmonicResonatorFollowsATimedFrequency();
        testRealtimeFollowsAMemoryWritersCaptureDuration();
        testRealtimePlaysAStartOnlyFieldsStartingValue();
        testEventVariablesPlayAlike();
        testStartOnlyFieldsCheckOnlyTheirStartingValue();
        testConstantFieldsPlayTheirStartingValue();
        testHotAndConstantFieldsAreMarked();
        testBankAndSpectrumSetupPlaysItsStartingValue();
        testCdpFixedFieldsPlayTheirStartingValue();
        testRealtimeFollowsHotSpectralFields();
        testLiveIndexesRoundAndFrequenciesSaturate();
        testRatioAndScaleSaturateAgainstTheirBound();
        testAnalogSequencerFollowsItsControlsAndSaturates();
        testAnalogSequencerFollowsItsSelectedStages();
        testStaticSpectraReadTheirStartingSetup();
        testGaOscillatorsReadsItsTimeIndex();
        testMultisegmentEnvelopeNamesAMovingLoopInAVoice();
        testLiveValuesSaturateAtTheirRange();
        testWholeNumberFieldsRoundTheirLiveValue();
        testLiveFieldsStartingPastTheirRangeSaturate();
        testPulseTrainIgnoresDutyCycleUntilVariable();
        testLiveGenericSourceIgnoresItsSampleFields();
        testSumOfSinesIgnoresBankSizeAndIdleLoopPoints();
        testAdsrFollowsAMetronomeGate();
        testBrassageIgnoresTimedFieldsItsModeDoesNotTake();
        testDistortReformIgnoresAnExaggerationItsModeDoesNotRead();
        testRealtimeFollowsALiveCount();
        testSumOfSinesPlaysTheAnalysisPartialsAtMost();
        testValToSoundPlaysARepeatingRamp();
        testVocoderEnvelopeTimesInheritOrSaturate();
        testCrossFieldRangesSaturate();
        testBoundsThatPassAnotherFieldSaturate();
        testLowPassGateReadsEachVoicesKeyNumber();
        testAVoiceResizesWhatItPrepared();
        testAFailedCallbackStartsTheVoiceAgain();
        testOlderProjectsReadConstantFieldsWhenCompiled();
        testConstantFieldsReadEachVoicesEventValues();
        testEveryHotFieldTakesVal();
        testLiveParametersPlayAlikeOrAreRefused();
        std::cout << "Live parameter parity tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
