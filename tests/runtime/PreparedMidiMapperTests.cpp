#include "talas/runtime/PreparedMidiMapper.h"
#include "talas/runtime/PreparedGraphSummary.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/RuntimeConformance.h"
#include "talas/project/ProjectExpressionAudit.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/MidiPitchBounds.h"
#include <numbers>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace talas::runtime
{
struct PreparedMidiMapperStateTestAccess
{
    static void exhaustSlotGeneration(
        PreparedMidiMapperState& state,
        std::size_t index)
    {
        state.slots_[index].generation =
            std::numeric_limits<std::uint64_t>::max();
    }

    static void exhaustActivationSequence(
        PreparedMidiMapperState& state)
    {
        state.nextActivationSequence_ =
            std::numeric_limits<std::uint64_t>::max();
    }

    static void exhaustMutationSequence(
        PreparedMidiMapperState& state)
    {
        state.mutationSequence_ =
            std::numeric_limits<std::uint64_t>::max();
    }
};
}

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

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
using talas::runtime::PreparedMidiMapperConfiguration;
using talas::runtime::PreparedMidiMapperEvent;
using talas::runtime::PreparedMidiMapperEventKind;
using talas::runtime::PreparedMidiMapperEventStatus;
using talas::runtime::PreparedMidiMapperMode;
using talas::runtime::PreparedMidiMapperParseError;
using talas::runtime::PreparedMidiMapperState;

struct ReferenceVoice
{
    std::uint8_t channel = 1;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    std::uint64_t activationSequence = 0;
    bool held = false;
};

struct ReferenceMapper
{
    PreparedMidiMapperConfiguration configuration;
    std::array<ReferenceVoice,
        talas::runtime::maximumPreparedMidiMapperPolyphony> voices{};
    std::size_t heldVoiceCount = 0;
    std::uint64_t nextActivationSequence = 1;

    [[nodiscard]] PreparedMidiMapperEventStatus apply(
        const PreparedMidiMapperEvent& event)
    {
        const auto validEvent = event.channel >= 1
            && event.channel <= 16
            && event.note <= 127
            && event.velocity <= 127;
        if (!validEvent)
            return PreparedMidiMapperEventStatus::Rejected;
        const auto matches = (configuration.channel == 0
                || configuration.channel == event.channel)
            && event.note >= configuration.lowPitch
            && event.note <= configuration.highPitch;
        if (!matches)
            return PreparedMidiMapperEventStatus::Filtered;

        const auto noteOff =
            event.kind == PreparedMidiMapperEventKind::NoteOff
            || event.velocity == 0;
        auto selected = static_cast<std::size_t>(configuration.polyphony);
        auto oldest = std::numeric_limits<std::uint64_t>::max();
        if (noteOff)
        {
            for (std::size_t index = 0;
                 index < configuration.polyphony;
                 ++index)
            {
                const auto& voice = voices[index];
                if (voice.held && voice.channel == event.channel
                    && voice.note == event.note
                    && voice.activationSequence < oldest)
                {
                    selected = index;
                    oldest = voice.activationSequence;
                }
            }
            if (selected == configuration.polyphony)
                return PreparedMidiMapperEventStatus::Handled;
            voices[selected].held = false;
            --heldVoiceCount;
            return PreparedMidiMapperEventStatus::Handled;
        }

        for (std::size_t index = 0;
             index < configuration.polyphony;
             ++index)
        {
            const auto& voice = voices[index];
            if (!voice.held && voice.activationSequence < oldest)
            {
                selected = index;
                oldest = voice.activationSequence;
            }
        }
        if (selected == configuration.polyphony)
        {
            for (std::size_t index = 0;
                 index < configuration.polyphony;
                 ++index)
            {
                const auto& voice = voices[index];
                if (voice.held && voice.activationSequence < oldest)
                {
                    selected = index;
                    oldest = voice.activationSequence;
                }
            }
        }
        auto& voice = voices[selected];
        if (!voice.held)
            ++heldVoiceCount;
        voice.channel = event.channel;
        voice.note = event.note;
        voice.velocity = event.velocity;
        voice.activationSequence = nextActivationSequence++;
        voice.held = true;
        return PreparedMidiMapperEventStatus::Handled;
    }

    [[nodiscard]] double gain() const
    {
        if (configuration.mode == PreparedMidiMapperMode::Gate)
            return heldVoiceCount == 0 ? 0.0 : 1.0;
        std::uint8_t maximum = 0;
        for (std::size_t index = 0;
             index < configuration.polyphony;
             ++index)
        {
            if (voices[index].held)
                maximum = std::max(maximum, voices[index].velocity);
        }
        return static_cast<double>(maximum) / 127.0;
    }
};

static_assert(talas::runtime::maximumPreparedMidiMapperMapBytes == 512);
static_assert(talas::runtime::maximumPreparedMidiMapperAssociations == 16);
static_assert(talas::runtime::maximumPreparedMidiMapperInstances == 64);
static_assert(talas::runtime::maximumPreparedMidiMapperVoices == 512);
static_assert(talas::runtime::maximumPreparedMidiMapperPolyphony == 128);
static_assert(std::is_nothrow_move_constructible_v<PreparedMidiMapperState>);
static_assert(std::is_nothrow_move_assignable_v<PreparedMidiMapperState>);
static_assert(noexcept(talas::runtime::parsePreparedMidiMapperMode({})));
static_assert(noexcept(std::declval<PreparedMidiMapperState&>().apply(
    std::declval<const PreparedMidiMapperEvent&>())));
static_assert(noexcept(std::declval<PreparedMidiMapperState&>().rollback(
    std::declval<const talas::runtime::PreparedMidiMapperAction&>())));
static_assert(noexcept(std::declval<PreparedMidiMapperState&>().reset()));

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(double actual, double expected, const std::string& message)
{
    if (std::abs(actual - expected) > 1.0e-12)
        throw std::runtime_error(message);
}

PreparedMidiMapperEvent noteOn(
    std::uint8_t channel,
    std::uint8_t note,
    std::uint8_t velocity,
    std::uint64_t frame)
{
    return {
        PreparedMidiMapperEventKind::NoteOn,
        channel,
        note,
        velocity,
        frame,
    };
}

PreparedMidiMapperEvent noteOff(
    std::uint8_t channel,
    std::uint8_t note,
    std::uint64_t frame)
{
    return {
        PreparedMidiMapperEventKind::NoteOff,
        channel,
        note,
        0,
        frame,
    };
}

void testParserAcceptsOnlyTheClosedModes()
{
    const auto blank = talas::runtime::parsePreparedMidiMapperMode("");
    require(blank.ok()
            && blank.mode == PreparedMidiMapperMode::Passthrough,
        "blank map should retain passthrough mode");

    const auto whitespace =
        talas::runtime::parsePreparedMidiMapperMode(" \t\r\n\f\v ");
    require(whitespace.ok()
            && whitespace.mode == PreparedMidiMapperMode::Passthrough,
        "ASCII whitespace should retain passthrough mode");

    const auto gate =
        talas::runtime::parsePreparedMidiMapperMode("\n gate\t");
    require(gate.ok() && gate.mode == PreparedMidiMapperMode::Gate,
        "gate token should parse after ASCII trimming");

    const auto velocity =
        talas::runtime::parsePreparedMidiMapperMode(" velocity ");
    require(velocity.ok()
            && velocity.mode == PreparedMidiMapperMode::Velocity,
        "velocity token should parse after ASCII trimming");

    const auto local = talas::runtime::parsePreparedMidiMapperMode(
        " !LocalLevel is: `AltLevel ");
    require(local.ok()
            && local.mode == PreparedMidiMapperMode::Passthrough
            && local.localAssociationCount == 1
            && local.localAssociations[0].localEventValue == "LocalLevel"
            && local.localAssociations[0].localEventSource == "AltLevel",
        "one documented local Event Value association should parse exactly");

    const auto virtualOnly = talas::runtime::parsePreparedMidiMapperMode(
        "!LocalLevel is: `LocalLevel");
    require(virtualOnly.ok() && virtualOnly.localAssociationCount == 1
            && virtualOnly.localAssociations[0].localEventValue
                == virtualOnly.localAssociations[0].localEventSource,
        "self-named virtual-only Map association must parse exactly");

    const auto controller = talas::runtime::parsePreparedMidiMapperMode(
        "!LocalLevel is: `MIDIController18");
    require(controller.ok() && controller.localAssociationCount == 1
            && controller.localAssociations[0].localEventSource
                == "MIDIController18",
        "one direct MIDI controller source should retain its exact identifier");
    const auto bentPitch = talas::runtime::parsePreparedMidiMapperMode(
        "!Pitch is: (`MIDIKeyNumber + (`MIDIPitchBend displayAs: #nothing)) nn.");
    const auto octaveBend = talas::runtime::parsePreparedMidiMapperMode(
        "!Pitch is: (`MIDIKeyNumber + (12 * `MIDIPitchBend)) nn.");
    require(bentPitch.ok() && octaveBend.ok()
            && bentPitch.localAssociationCount == 1
            && bentPitch.localAssociations[0].localEventSource
                == "MIDIKeyNumber"
            && bentPitch.localAssociations[0].pitchBendSemitones == 1.0
            && bentPitch.localAssociations[0].hidePitchBend
            && octaveBend.localAssociations[0].pitchBendSemitones == 12.0
            && !octaveBend.localAssociations[0].hidePitchBend,
        "both documented note-number plus pitch-bend Maps must retain their coefficient and nested visibility");
    for (const auto* invalidBend : {
             "!Pitch is: (`MIDIKeyNumber + (`MIDIPitchBend displayAs: #fader)) nn.",
             "!Pitch is: (`MIDIKeyNumber + (0 * `MIDIPitchBend)) nn.",
             "!Pitch is: (`MIDIKeyNumber + (129 * `MIDIPitchBend)) nn.",
             "!Pitch is: (`MIDIKeyNumber + (`MIDIChannelPressure)) nn."})
        require(!talas::runtime::parsePreparedMidiMapperMode(invalidBend).ok(),
            "unbounded or different Map arithmetic must reject instead of silently changing sources");
    require(talas::runtime::resolvePreparedMidiMapperLocalSource(
                "MIDIController0").value_or("") == "cc00"
            && talas::runtime::resolvePreparedMidiMapperLocalSource(
                "MIDIController00").value_or("") == "cc00"
            && talas::runtime::resolvePreparedMidiMapperLocalSource(
                "MIDIController07").value_or("") == "cc07"
            && talas::runtime::resolvePreparedMidiMapperLocalSource(
                "MIDIController127").value_or("") == "cc127"
            && talas::runtime::resolvePreparedMidiMapperLocalSource(
                "AltLevel").value_or("") == "AltLevel",
        "direct controller endpoints must resolve to canonical built-in names");
    require(!talas::runtime::resolvePreparedMidiMapperLocalSource(
                 "MIDIController007")
            && !talas::runtime::resolvePreparedMidiMapperLocalSource(
                 "MIDIController018")
            && !talas::runtime::resolvePreparedMidiMapperLocalSource(
                 "MIDIController128"),
        "noncanonical and out-of-range direct controller identifiers must reject");

    const auto tagged = talas::runtime::parsePreparedMidiMapperMode(
        "!LocalLevel is: (`MIDIController07 channel: 5)");
    require(tagged.ok() && tagged.localAssociationCount == 1
            && tagged.localAssociations[0].channel == 5
            && !tagged.localAssociations[0].hasRange()
            && talas::runtime::resolvePreparedMidiMapperLocalSource(
                tagged.localAssociations[0].localEventSource,
                tagged.localAssociations[0].channel).value_or("")
                == "cc07_ch05",
        "channel: tag must retain the association channel and a separate CC binding");
    const auto taggedRange = talas::runtime::parsePreparedMidiMapperMode(
        "!Frequency is: (`MIDIController07 channel: 5; min: 100 max: 1000)");
    const auto reversedRange = talas::runtime::parsePreparedMidiMapperMode(
        "!Frequency is: (`MIDIController07 min: 100 max: 1000; channel: 5)");
    require(taggedRange.ok() && reversedRange.ok()
            && taggedRange.localAssociationCount == 1
            && taggedRange.localAssociations[0].channel == 5
            && taggedRange.localAssociations[0].minimumText == "100"
            && taggedRange.localAssociations[0].maximumText == "1000"
            && reversedRange.localAssociations[0].channel == 5
            && reversedRange.localAssociations[0].hasRange(),
        "channel and range option groups must compose in either order");
    const std::array displayTypes{
        std::pair{"#fader", talas::project::MidiMapperMapDisplayAs::Fader},
        std::pair{"#smallFader", talas::project::MidiMapperMapDisplayAs::SmallFader},
        std::pair{"#toggle", talas::project::MidiMapperMapDisplayAs::Toggle},
        std::pair{"#gate", talas::project::MidiMapperMapDisplayAs::Gate},
        std::pair{"#nothing", talas::project::MidiMapperMapDisplayAs::Nothing},
    };
    for (const auto& [tag, expected] : displayTypes)
    {
        const auto parsed = talas::runtime::parsePreparedMidiMapperMode(
            std::string("!LocalLevel is: (`MIDIController07 displayAs: ")
                + tag + ")");
        require(parsed.ok() && parsed.localAssociationCount == 1
                && parsed.localAssociations[0].displayAs == expected
                && !parsed.localAssociations[0].hasRange(),
            "documented displayAs: styles must parse without a range");
    }
    // Any source takes a range.
    const auto glide = talas::runtime::parsePreparedMidiMapperMode("!Glide is: (`Glide min: -0.01 max: 0.01)");
    require(glide.ok() && glide.localAssociationCount == 1 && glide.localAssociations[0].localEventSource == "Glide"
            && glide.localAssociations[0].minimumText == "-0.01" && glide.localAssociations[0].maximumText == "0.01",
        "`Glide min: -0.01 max: 0.01 should give a fader a range");
    // type: is accepted as a spelling of displayAs:.
    const auto bypass = talas::runtime::parsePreparedMidiMapperMode("!Bypass is: (`Bypass type: #gate)");
    require(bypass.ok() && bypass.localAssociationCount == 1
            && bypass.localAssociations[0].displayAs == talas::project::MidiMapperMapDisplayAs::Gate,
        "`Bypass type: #gate should show !Bypass as a gate");
    const auto combinedDisplay = talas::runtime::parsePreparedMidiMapperMode(
        "!LocalLevel is: (`MIDIController07 channel: 5; min: 0.2 max: 0.8; displayAs: #toggle)");
    require(combinedDisplay.ok()
            && combinedDisplay.localAssociations[0].channel == 5
            && combinedDisplay.localAssociations[0].hasRange()
            && combinedDisplay.localAssociations[0].displayAs
                == talas::project::MidiMapperMapDisplayAs::Toggle,
        "displayAs: must compose with channel and range groups");
    const auto grid = talas::runtime::parsePreparedMidiMapperMode(
        "!LocalLevel is: (`MIDIController07 min: 0.5 max: 0.75 grid: 0.05)");
    const auto separateGrid = talas::runtime::parsePreparedMidiMapperMode(
        "!LocalLevel is: (`MIDIController07 channel: 5; min: 0.5 max: 0.75; grid: 0.05; displayAs: #smallFader)");
    require(grid.ok() && grid.localAssociations[0].gridText == "0.05"
            && separateGrid.ok()
            && separateGrid.localAssociations[0].channel == 5
            && separateGrid.localAssociations[0].gridText == "0.05"
            && separateGrid.localAssociations[0].displayAs
                == talas::project::MidiMapperMapDisplayAs::SmallFader,
        "grid must compose with the documented range and other option groups");
    const auto logTaper = talas::runtime::parsePreparedMidiMapperMode(
        "!LocalLevel is: (`MIDIController07 taper: #log)");
    const auto allOptions = talas::runtime::parsePreparedMidiMapperMode(
        "!LocalLevel is: (`MIDIController07 channel: 5; min: 100 max: 1000 grid: 50; taper: #log; displayAs: #smallFader)");
    require(logTaper.ok() && logTaper.localAssociations[0].taper
                == talas::project::MidiMapperMapTaper::Log
            && !logTaper.localAssociations[0].hasRange()
            && allOptions.ok()
            && allOptions.localAssociations[0].channel == 5
            && allOptions.localAssociations[0].minimumText == "100"
            && allOptions.localAssociations[0].maximumText == "1000"
            && allOptions.localAssociations[0].gridText == "50"
            && allOptions.localAssociations[0].taper
                == talas::project::MidiMapperMapTaper::Log
            && allOptions.localAssociations[0].displayAs
                == talas::project::MidiMapperMapDisplayAs::SmallFader,
        "log taper and every documented source option must compose");
    for (const auto* invalidTaper : {
             "!LocalLevel is: (`MIDIController07 taper: #exp)",
             "!LocalLevel is: (`MIDIController07 taper: #log; taper: #linear)",
             "!LocalLevel is: (`MIDIController07 min: -1 max: 1; taper: #log)",
             "!LocalLevel is: (`MIDIController07 min: 2 max: 1; taper: #log)"})
        require(!talas::runtime::parsePreparedMidiMapperMode(invalidTaper).ok(),
            "unknown, repeated, negative or reversed log ranges must reject");
    const auto midpoint = talas::project::projectMidiMapperTaperValue(
        0.5, 100.0, 1000.0, talas::project::MidiMapperMapTaper::Log);
    const auto inverse = talas::project::projectMidiMapperTaperInverse(
        std::sqrt(100.0 * 1000.0), 100.0, 1000.0,
        talas::project::MidiMapperMapTaper::Log);
    require(midpoint && std::abs(*midpoint - std::sqrt(100000.0)) < 1.0e-9
            && inverse && std::abs(*inverse - 0.5) < 1.0e-12,
        "positive logarithmic mapping and inverse must use geometric spacing");
    for (const auto* invalidChannel : {
             "!LocalLevel is: (`MIDIController07 channel: 0)",
             "!LocalLevel is: (`MIDIController07 channel: 17)",
             "!LocalLevel is: (`MIDIController07 channel: 5; grid: 0.1)",
             "!LocalLevel is: (`MIDIController07 channel: 5; channel: 6)",
             "!LocalLevel is: (`MIDIController07 channel: 5;)",
        })
        require(!talas::runtime::parsePreparedMidiMapperMode(invalidChannel).ok(),
            "unsupported or out-of-range channel tags must reject");
    for (const auto* invalidDisplay : {
             "!LocalLevel is: (`MIDIController07 displayAs: #meter)",
             "!LocalLevel is: (`MIDIController07 displayAs: #gate; displayAs: #fader)",
             "!LocalLevel is: (`MIDIController07 displayAs: #gate extra)"})
        require(!talas::runtime::parsePreparedMidiMapperMode(invalidDisplay).ok(),
            "unknown, duplicate and malformed displayAs: groups must reject");
    for (const auto* invalidGrid : {
             "!LocalLevel is: (`MIDIController07 grid: 0.05)",
             "!LocalLevel is: (`MIDIController07 min: 0.5 max: 0.75 grid: 0)",
             "!LocalLevel is: (`MIDIController07 min: 0.5 max: 0.75 grid: -0.05)",
             "!LocalLevel is: (`MIDIController07 min: 0.5 max: 0.75 grid: 0.04)",
             "!LocalLevel is: (`MIDIController07 min: 0.5 max: 0.75 grid:)",
             "!LocalLevel is: (`MIDIController07 min: 0.5 max: 0.75 grid: 0.05; grid: 0.05)"})
        require(!talas::runtime::parsePreparedMidiMapperMode(invalidGrid).ok(),
            "grid requires a positive step dividing an ascending authored range");

    const auto ranged = talas::runtime::parsePreparedMidiMapperMode(
        "!Frequency is: (`MIDIController18 min: 100 max: 1000)");
    require(ranged.ok() && ranged.localAssociationCount == 1
            && ranged.localAssociations[0].localEventSource
                == "MIDIController18"
            && ranged.localAssociations[0].minimumText == "100"
            && ranged.localAssociations[0].maximumText == "1000",
        "documented direct-controller min/max Map must retain both bounds");
    for (const auto* invalidRange : {
             "!Frequency is: (`MIDIController18 min: 100)",
             "!Frequency is: (`MIDIController18 min: nan max: 1000)",
             "!Frequency is: (`MIDIController18 min: -1e308 max: 1e308)",
             "!Frequency is: (`MIDIController18 min: 100 max: 1000 extra)",
             "!Frequency is: (`AltLevel channel: 2; min: 100 max: 1000)"})
    {
        require(!talas::runtime::parsePreparedMidiMapperMode(invalidRange).ok(),
            "a malformed ranged Map, or a channel for a source other than a MIDI controller, must reject");
    }

    const auto multiple = talas::runtime::parsePreparedMidiMapperMode(
        "!GainA is: `AltA\r\n\n !GainB is: `AltB ");
    require(multiple.ok()
            && multiple.localAssociationCount == 2
            && multiple.localAssociations[0].localEventValue == "GainA"
            && multiple.localAssociations[0].localEventSource == "AltA"
            && multiple.localAssociations[1].localEventValue == "GainB"
            && multiple.localAssociations[1].localEventSource == "AltB",
        "independent local association lines must parse in authored order");

    const auto duplicate = talas::runtime::parsePreparedMidiMapperMode(
        "!GainA is: `AltA\n!GainA is: `AltB");
    require(!duplicate.ok()
            && duplicate.error == PreparedMidiMapperParseError::DuplicateTarget,
        "duplicate local targets must reject without ambiguous precedence");

    const auto uppercase =
        talas::runtime::parsePreparedMidiMapperMode("Gate");
    require(!uppercase.ok()
            && uppercase.error == PreparedMidiMapperParseError::UnknownMode,
        "mode tokens should remain exact and case-sensitive");

    const auto unknown =
        talas::runtime::parsePreparedMidiMapperMode("note 60 -> sound");
    require(!unknown.ok()
            && unknown.error == PreparedMidiMapperParseError::UnknownMode,
        "arbitrary map syntax must remain rejected");
}

talas::project::ProjectDocument midiPitchDocument(
    std::string mode, std::string lowPitch, std::string highPitch,
    bool nested);

void testLocalMapIsScopedToOneNestedInputSound()
{
    using namespace talas::project;
    ProjectDocument document;
    ProjectSoundDefinition tone;
    tone.definitionId = "mapTone";
    tone.displayName = "Map Tone";
    tone.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "7 hz"}, {"amplitude", "!LocalLevel"}}}};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    for (const auto* id : {"sourceA", "sourceB"})
    {
        ProjectNode source;
        source.instanceId = id;
        source.soundDefinitionId = "mapTone";
        document.graph.nodes.push_back(std::move(source));
    }
    const auto mapperTemplate = midiPitchDocument(
        "", "0 nn", "127 nn", false).graph.nodes[1];
    auto mapped = mapperTemplate;
    mapped.instanceId = "mapperA";
    std::ranges::find(mapped.parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!LocalLevel is: `AltLevel";
    auto global = mapperTemplate;
    global.instanceId = "mapperB";
    std::ranges::find(global.parameters, "right",
        &ProjectParameterExpression::parameterId)->expression = "1";
    document.graph.nodes.push_back(std::move(mapped));
    document.graph.nodes.push_back(std::move(global));
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections = {
        {"sourceA", "audioOut", "mapperA", "audioIn"},
        {"sourceB", "audioOut", "mapperB", "audioIn"},
        {"mapperA", "leftOut", "out", "audioIn"},
        {"mapperB", "rightOut", "out", "rightIn"},
    };

    talas::runtime::RenderSettings settings{1000.0, 16};
    settings.eventValues = {{"LocalLevel", 0.2}, {"AltLevel", 0.8}};
    talas::runtime::EventValueChange alternate;
    alternate.frame = 8;
    alternate.name = "AltLevel";
    alternate.value = 0.4;
    settings.eventValueChanges.push_back(alternate);
    talas::runtime::EventValueChange globalChange;
    globalChange.frame = 12;
    globalChange.name = "LocalLevel";
    globalChange.value = 0.5;
    settings.eventValueChanges.push_back(globalChange);

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(
        document, registry, settings);
    require(compiled.ok(), "nested local map must compile: "
        + (compiled.diagnostics.empty() ? std::string{}
                                        : compiled.diagnostics.front().message));
    const auto& bindings = compiled.graph->renderSettings().eventValues;
    require(std::ranges::count_if(bindings,
                [](const talas::runtime::EventValueBinding& binding) {
                    return binding.name == "LocalLevel";
                }) == 2,
        "local map must add one scoped projection without replacing the global binding");
    require(compiled.graph->plan().midiMappers.empty(),
        "local association must retain passthrough MIDI Mapper audio");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 24);
    require(offline.ok() && offline.mono.size() == 24
            && offline.right.size() == 24,
        "both mapped and global inputs must render offline");
    for (const auto frame : {1U, 9U, 13U})
    {
        const auto toneSample = std::sin(
            2.0 * std::numbers::pi_v<double> * 7.0
            * static_cast<double>(frame) / 1000.0);
        const auto mappedLevel = frame < 8 ? 0.8 : 0.4;
        const auto globalLevel = frame < 12 ? 0.2 : 0.5;
        requireNear(offline.mono[frame], mappedLevel * toneSample,
            "local mapping must follow only the alternate source");
        requireNear(offline.right[frame], globalLevel * toneSample,
            "sibling input must retain the global Event Value");
    }

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "nested local map must prepare in realtime");
    std::array<double, 24> left{};
    std::array<double, 24> right{};
    std::size_t offset = 0;
    for (const auto frames : {5U, 3U, 7U, 9U})
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            std::span(left).subspan(offset, frames),
            std::span(right).subspan(offset, frames));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "partitioned local map callback must render without allocation");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < left.size(); ++frame)
    {
        requireNear(left[frame], offline.mono[frame],
            "local map realtime left output must match offline");
        requireNear(right[frame], offline.right[frame],
            "global sibling realtime right output must match offline");
    }

    auto flat = document;
    flat.graph.nodes[0] = {"flat", "talas.oscillator",
        {{"frequency", "7 hz"}, {"amplitude", "!LocalLevel"}}};
    flat.graph.connections[0].fromNode = "flat";
    const auto rejected = talas::runtime::compileProject(
        flat, registry, settings);
    require(!rejected.ok(),
        "a flat input graph cannot claim a local map without an isolated scope");

    auto sharedInput = document;
    sharedInput.graph.connections[1].fromNode = "sourceA";
    require(!talas::runtime::compileProject(
                sharedInput, registry, settings).ok(),
        "one nested input Sound cannot share its local mapping with another mapper");

    auto missingSource = document;
    std::ranges::find(missingSource.graph.nodes[2].parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!LocalLevel is: `Absent";
    require(!talas::runtime::compileProject(
                missingSource, registry, settings).ok(),
        "unbound local Map sources must fail at preparation");

    auto forged = *compiled.graph;
    auto& forgedSettings = const_cast<talas::runtime::RenderSettings&>(
        forged.renderSettings());
    std::erase_if(forgedSettings.eventValues,
        [](const talas::runtime::EventValueBinding& binding) {
            return binding.scopeId != talas::runtime::globalRuntimeScope
                && binding.name == "LocalLevel";
        });
    talas::runtime::RealtimeRenderSession forgedSession(forged);
    require(!forgedSession.ready(),
        "prepared realtime must reject a local Map whose scoped binding was removed");
    require(!talas::runtime::renderOffline(forged, 4).ok(),
        "offline render must reject a local Map whose scoped binding was removed");
}

void testLocalMapResolvesDirectMidiControllerSource()
{
    using namespace talas::project;
    ProjectDocument document;
    ProjectSoundDefinition tone;
    tone.definitionId = "controllerTone";
    tone.displayName = "Controller Tone";
    tone.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "7 hz"}, {"amplitude", "!LocalLevel"}}}};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "controllerTone";
    document.graph.nodes.push_back(std::move(source));
    auto mapper = midiPitchDocument("", "0 nn", "127 nn", false)
        .graph.nodes[1];
    std::ranges::find(mapper.parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!LocalLevel is: `MIDIController18";
    document.graph.nodes.push_back(std::move(mapper));
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections = {
        {"source", "audioOut", "mapper", "audioIn"},
        {"mapper", "leftOut", "out", "audioIn"},
    };

    talas::runtime::RenderSettings settings{1000.0, 16};
    settings.eventValues = {{"LocalLevel", 0.25}};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(document, registry, settings);
    require(compiled.ok(), "direct local MIDI controller source must compile: "
        + (compiled.diagnostics.empty() ? std::string{}
                                        : compiled.diagnostics.front().message));
    const auto& bindings = compiled.graph->renderSettings().eventValues;
    require(std::ranges::any_of(bindings,
                [](const talas::runtime::EventValueBinding& binding) {
                    return binding.name == "cc18"
                        && binding.scopeId == talas::runtime::globalRuntimeScope
                        && binding.value == 0.0;
                }),
        "Map controller source must acquire one implicit global cc18 binding");
    require(std::ranges::any_of(bindings,
                [](const talas::runtime::EventValueBinding& binding) {
                    return binding.name == "LocalLevel"
                        && binding.scopeId != talas::runtime::globalRuntimeScope
                        && binding.projectionExpression.has_value();
                }),
        "Map controller source must project only the nested Input target");
    require(std::ranges::any_of(bindings,
                [](const talas::runtime::EventValueBinding& binding) {
                    return binding.name == "LocalLevel"
                        && binding.scopeId == talas::runtime::globalRuntimeScope
                        && binding.value == 0.25;
                }),
        "direct controller Map must preserve the unrelated global target value");

    auto scheduledSettings = settings;
    scheduledSettings.eventValues.push_back({"cc18", 0.0});
    talas::runtime::EventValueChange change;
    change.frame = 8;
    change.name = "cc18";
    change.value = 1.0;
    scheduledSettings.eventValueChanges.push_back(change);
    const auto scheduled = talas::runtime::compileProject(
        document, registry, scheduledSettings);
    require(scheduled.ok(), "scheduled controller Map must compile: "
        + (scheduled.diagnostics.empty() ? std::string{}
                                         : scheduled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*scheduled.graph, 16);
    require(offline.ok() && offline.mono.size() == 16,
        "direct controller Map must render offline");
    for (const auto frame : {1U, 7U, 9U, 13U})
    {
        const auto sample = std::sin(
            2.0 * std::numbers::pi_v<double> * 7.0
            * static_cast<double>(frame) / 1000.0);
        requireNear(offline.mono[frame], (frame < 8 ? 0.0 : 1.0) * sample,
            "direct controller Map must change on the scheduled frame");
    }

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "direct controller Map must prepare in realtime");
    require(session.scheduleEventValue(
                "cc18", 1.0, talas::expr::ValueDomain::Scalar, 8),
        "implicit controller binding must accept a scheduled live value");
    std::array<double, 16> left{};
    std::array<double, 16> right{};
    std::size_t offset = 0;
    for (const auto frames : {5U, 3U, 7U, 1U})
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            std::span(left).subspan(offset, frames),
            std::span(right).subspan(offset, frames));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "direct controller Map render must be allocation-free");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < left.size(); ++frame)
        requireNear(left[frame], offline.mono[frame],
            "direct controller Map realtime output must match offline");

    auto invalid = document;
    std::ranges::find(invalid.graph.nodes[1].parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!LocalLevel is: `MIDIController128";
    require(!talas::runtime::compileProject(invalid, registry, settings).ok(),
        "out-of-range direct MIDI controller source must reject");

    auto padded = document;
    std::ranges::find(padded.graph.nodes[1].parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!LocalLevel is: `MIDIController07";
    auto paddedSettings = settings;
    paddedSettings.eventValues.push_back({"cc07", 0.0});
    paddedSettings.eventValueChanges.push_back({8, "cc07", 1.0});
    const auto paddedCompiled = talas::runtime::compileProject(
        padded, registry, paddedSettings);
    require(paddedCompiled.ok(),
        "a zero-padded MIDIController07 local Map must compile: "
        + (paddedCompiled.diagnostics.empty() ? std::string{}
                                              : paddedCompiled.diagnostics.front().message));
    const auto paddedOffline = talas::runtime::renderOffline(
        *paddedCompiled.graph, 16);
    require(paddedOffline.ok() && paddedOffline.mono.size() == 16,
        "zero-padded controller Map must render offline");
    for (const auto frame : {1U, 7U, 9U, 13U})
    {
        const auto sample = std::sin(
            2.0 * std::numbers::pi_v<double> * 7.0
            * static_cast<double>(frame) / 1000.0);
        requireNear(paddedOffline.mono[frame], (frame < 8 ? 0.0 : 1.0) * sample,
            "zero-padded controller Map must use the cc07 value on its frame");
    }

    auto taggedDocument = document;
    std::ranges::find(taggedDocument.graph.nodes[1].parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!LocalLevel is: (`MIDIController07 channel: 5)";
    auto taggedSettings = settings;
    taggedSettings.eventValues.push_back({"cc07", 0.0});
    taggedSettings.eventValues.push_back({"cc07_ch05", 0.0});
    taggedSettings.eventValueChanges.push_back({4, "cc07", 1.0});
    taggedSettings.eventValueChanges.push_back({8, "cc07_ch05", 1.0});
    const auto taggedCompiled = talas::runtime::compileProject(
        taggedDocument, registry, taggedSettings);
    require(taggedCompiled.ok(),
        "channel-tagged local Map must compile: "
        + (taggedCompiled.diagnostics.empty() ? std::string{}
                                              : taggedCompiled.diagnostics.front().message));
    const auto taggedOffline = talas::runtime::renderOffline(
        *taggedCompiled.graph, 16);
    require(taggedOffline.ok() && taggedOffline.mono.size() == 16,
        "channel-tagged controller Map must render offline");
    for (const auto frame : {1U, 5U, 7U, 9U, 13U})
    {
        const auto sample = std::sin(
            2.0 * std::numbers::pi_v<double> * 7.0
            * static_cast<double>(frame) / 1000.0);
        requireNear(taggedOffline.mono[frame],
            (frame < 8 ? 0.0 : 1.0) * sample,
            "only the tagged channel binding may change the local Input");
    }

    auto griddedDocument = document;
    std::ranges::find(griddedDocument.graph.nodes[1].parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!LocalLevel is: (`MIDIController07 min: 0.5 max: 0.75 grid: 0.05)";
    auto gridSettings = settings;
    gridSettings.eventValues.push_back({"cc07", 0.0});
    const auto gridded = talas::runtime::compileProject(
        griddedDocument, registry, gridSettings);
    require(gridded.ok(), "gridded controller Map must compile: "
        + (gridded.diagnostics.empty() ? std::string{}
                                       : gridded.diagnostics.front().message));
    auto gridOfflineSettings = gridSettings;
    gridOfflineSettings.eventValueChanges = {
        {4, "cc07", 32.0 / 127.0},
        {8, "cc07", 64.0 / 127.0},
        {12, "cc07", 1.0},
    };
    const auto gridOfflineGraph = talas::runtime::compileProject(
        griddedDocument, registry, gridOfflineSettings);
    require(gridOfflineGraph.ok(),
        "scheduled gridded controller Map must compile");
    const auto gridOffline = talas::runtime::renderOffline(
        *gridOfflineGraph.graph, 16);
    require(gridOffline.ok() && gridOffline.mono.size() == 16,
        "gridded controller Map must render offline");
    for (const auto frame : {1U, 5U, 9U, 13U})
    {
        const auto gain = frame < 4 ? 0.5
            : frame < 8 ? 0.55
            : frame < 12 ? 0.65 : 0.75;
        const auto sample = std::sin(
            2.0 * std::numbers::pi_v<double> * 7.0
            * static_cast<double>(frame) / 1000.0);
        requireNear(gridOffline.mono[frame], gain * sample,
            "grid must keep the documented six allowed values at scheduled frames");
    }
    auto sweepSettings = gridSettings;
    for (std::size_t controller = 0; controller <= 127; ++controller)
    {
        sweepSettings.eventValueChanges.push_back({
            controller, "cc07", static_cast<double>(controller) / 127.0});
    }
    const auto sweepGraph = talas::runtime::compileProject(
        griddedDocument, registry, sweepSettings);
    require(sweepGraph.ok(), "all 128 gridded CC positions must compile");
    const auto sweep = talas::runtime::renderOffline(*sweepGraph.graph, 128);
    require(sweep.ok() && sweep.mono.size() == 128,
        "all 128 gridded CC positions must render");
    constexpr std::array allowedGridValues{
        0.5, 0.55, 0.6, 0.65, 0.7, 0.75};
    for (std::size_t frame = 1; frame < sweep.mono.size(); ++frame)
    {
        const auto carrier = std::sin(
            2.0 * std::numbers::pi_v<double> * 7.0
            * static_cast<double>(frame) / 1000.0);
        if (std::abs(carrier) < 1.0e-9) continue;
        const auto gain = sweep.mono[frame] / carrier;
        require(std::ranges::any_of(allowedGridValues,
                [&](double allowed) {
                    return std::abs(gain - allowed) < 1.0e-8;
                }),
            "every CC7 byte must land on one of the six documented grid values");
    }
    talas::runtime::RealtimeRenderSession gridSession(*gridded.graph);
    require(gridSession.ready()
            && gridSession.scheduleEventValue(
                "cc07", 32.0 / 127.0, talas::expr::ValueDomain::Scalar, 4)
            && gridSession.scheduleEventValue(
                "cc07", 64.0 / 127.0, talas::expr::ValueDomain::Scalar, 8)
            && gridSession.scheduleEventValue(
                "cc07", 1.0, talas::expr::ValueDomain::Scalar, 12),
        "gridded CC changes must schedule in prepared realtime");
    std::array<double, 16> gridLeft{};
    std::array<double, 16> gridRight{};
    offset = 0;
    for (const auto frames : {3U, 5U, 3U, 5U})
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = gridSession.render(
            std::span(gridLeft).subspan(offset, frames),
            std::span(gridRight).subspan(offset, frames));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "gridded Map realtime blocks must not allocate");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < gridLeft.size(); ++frame)
        requireNear(gridLeft[frame], gridOffline.mono[frame],
            "gridded Map realtime and offline PCM must agree");

    auto logarithmicDocument = document;
    std::ranges::find(logarithmicDocument.graph.nodes[1].parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!LocalLevel is: (`MIDIController07 taper: #log)";
    auto logarithmicSettings = settings;
    logarithmicSettings.eventValues.push_back({"cc07", 0.0});
    logarithmicSettings.eventValueChanges = {
        {4, "cc07", 0.5}, {8, "cc07", 1.0},
    };
    const auto logarithmic = talas::runtime::compileProject(
        logarithmicDocument, registry, logarithmicSettings);
    require(logarithmic.ok(), "unranged log taper must compile: "
        + (logarithmic.diagnostics.empty() ? std::string{}
                                           : logarithmic.diagnostics.front().message));
    const auto logOffline = talas::runtime::renderOffline(
        *logarithmic.graph, 16);
    require(logOffline.ok(), "unranged log taper must render offline");
    const auto logMidpoint = std::expm1(std::log(1000.0) * 0.5) / 999.0;
    require(logMidpoint > 0.0 && logMidpoint < 0.5,
        "the bounded zero-anchored log curve must differ from linear");
    for (std::size_t frame = 0; frame < 16; ++frame)
    {
        const auto gain = frame < 4 ? 0.0
            : frame < 8 ? logMidpoint : 1.0;
        const auto carrier = std::sin(
            2.0 * std::numbers::pi_v<double> * 7.0
            * static_cast<double>(frame) / 1000.0);
        requireNear(logOffline.mono[frame], gain * carrier,
            "unranged log taper must change the Event Value at scheduled frames");
    }
    talas::runtime::RealtimeRenderSession logSession(*logarithmic.graph);
    require(logSession.ready(), std::string(logSession.error()));
    std::array<double, 16> logLeft{}, logRight{};
    offset = 0;
    for (const auto frames : {1U, 7U, 8U})
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = logSession.render(
            std::span(logLeft).subspan(offset, frames),
            std::span(logRight).subspan(offset, frames));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "unranged log taper callback must not allocate");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < logLeft.size(); ++frame)
        requireNear(logLeft[frame], logOffline.mono[frame],
            "unranged log taper realtime and offline PCM must agree");

    auto combinedDocument = document;
    std::ranges::find(combinedDocument.graph.nodes[1].parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!LocalLevel is: (`MIDIController07 channel: 5; min: 100 max: 1000 grid: 50; taper: #log; displayAs: #smallFader)";
    combinedDocument.soundDefinitions[0].graph.nodes[0]
        .parameters[1].expression = "!LocalLevel * 0.001";
    auto combinedSettings = settings;
    combinedSettings.defaultMidiChannel = 2;
    combinedSettings.eventValues.push_back({"cc07", 0.0});
    combinedSettings.eventValues.push_back({"cc07_ch05", 0.0});
    combinedSettings.eventValueChanges = {
        {4, "cc07", 1.0},
        {8, "cc07_ch05", 0.5},
        {12, "cc07_ch05", 1.0},
    };
    const auto combined = talas::runtime::compileProject(
        combinedDocument, registry, combinedSettings);
    require(combined.ok(), "combined Map options must compile: "
        + (combined.diagnostics.empty() ? std::string{}
                                        : combined.diagnostics.front().message));
    const auto combinedOffline = talas::runtime::renderOffline(
        *combined.graph, 16);
    require(combinedOffline.ok(), "combined Map options must render offline");
    std::array<double, 16> combinedExpected{};
    for (std::size_t frame = 0; frame < combinedExpected.size(); ++frame)
    {
        const auto gain = frame < 8 ? 0.1 : frame < 12 ? 0.3 : 1.0;
        combinedExpected[frame] = gain * std::sin(
            2.0 * std::numbers::pi_v<double> * 7.0
            * static_cast<double>(frame) / 1000.0);
        requireNear(combinedOffline.mono[frame], combinedExpected[frame],
            "combined tagged log/grid Map must ignore the default channel");
    }
    talas::runtime::RealtimeRenderSession combinedSession(*combined.graph);
    require(combinedSession.ready(), std::string(combinedSession.error()));
    std::array<double, 16> combinedLeft{}, combinedRight{};
    offset = 0;
    for (const auto frames : {3U, 5U, 3U, 5U})
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = combinedSession.render(
            std::span(combinedLeft).subspan(offset, frames),
            std::span(combinedRight).subspan(offset, frames));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "combined tagged log/grid callback must not allocate");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < combinedLeft.size(); ++frame)
        requireNear(combinedLeft[frame], combinedExpected[frame],
            "combined tagged log/grid realtime PCM must match offline");

    for (const auto* timeSource : {"MIDITimeCode", "RealTime"})
    {
        auto timeDocument = document;
        std::ranges::find(timeDocument.graph.nodes[1].parameters, "map",
            &ProjectParameterExpression::parameterId)->text =
            "!LocalLevel is: `" + std::string(timeSource);
        auto timeSettings = settings;
        timeSettings.eventValues = {
            {"LocalLevel", 0.0, talas::expr::ValueDomain::Scalar},
            {timeSource, 0.0, talas::expr::ValueDomain::Scalar},
        };
        timeSettings.eventValueChanges = {
            {4, timeSource, 1.25, talas::expr::ValueDomain::Scalar},
            {8, timeSource, 2.0, talas::expr::ValueDomain::Scalar},
        };
        const auto timeCompiled = talas::runtime::compileProject(
            timeDocument, registry, timeSettings);
        require(timeCompiled.ok(),
            std::string(timeSource) + " local Map must compile: "
            + (timeCompiled.diagnostics.empty() ? std::string{}
                                                : timeCompiled.diagnostics.front().message));
        const auto timeOffline = talas::runtime::renderOffline(
            *timeCompiled.graph, 16);
        require(timeOffline.ok(),
            std::string(timeSource) + " local Map must render offline");
        talas::runtime::RealtimeRenderSession timeSession(*timeCompiled.graph);
        require(timeSession.ready(), std::string(timeSession.error()));
        std::array<double, 16> timeLeft{}, timeRight{};
        offset = 0;
        for (const auto frames : {5U, 3U, 7U, 1U})
        {
            allocationCount.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            const auto rendered = timeSession.render(
                std::span(timeLeft).subspan(offset, frames),
                std::span(timeRight).subspan(offset, frames));
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered && allocationCount.load(
                    std::memory_order_relaxed) == 0,
                std::string(timeSource)
                    + " local Map callback must not allocate");
            offset += frames;
        }
        for (std::size_t frame = 0; frame < timeLeft.size(); ++frame)
        {
            const auto gain = frame < 4 ? 0.0
                : frame < 8 ? 1.25 : 2.0;
            const auto expected = gain * std::sin(
                2.0 * std::numbers::pi_v<double> * 7.0
                * static_cast<double>(frame) / 1000.0);
            requireNear(timeOffline.mono[frame], expected,
                std::string(timeSource)
                    + " must project the seconds-domain source to Input audio");
            requireNear(timeLeft[frame], expected,
                std::string(timeSource)
                    + " offline and realtime projections must agree");
        }

        auto typedDocument = timeDocument;
        typedDocument.soundDefinitions[0].graph.nodes[0]
            .parameters[1].expression = "1";
        auto typedSettings = settings;
        typedSettings.eventValues = {{
            "LocalLevel", 0.0, talas::expr::ValueDomain::Seconds}};
        const auto typed = talas::runtime::compileProject(
            typedDocument, registry, typedSettings);
        require(typed.ok(),
            std::string(timeSource)
                + " must preserve an authored seconds-domain target: "
                + (typed.diagnostics.empty() ? std::string{}
                                             : typed.diagnostics.front().message));
        require(std::ranges::any_of(
                typed.graph->renderSettings().eventValues,
                [&](const talas::runtime::EventValueBinding& binding) {
                    return binding.scopeId == talas::runtime::globalRuntimeScope
                        && binding.name == timeSource
                        && binding.domain == talas::expr::ValueDomain::Seconds;
                }),
            std::string(timeSource)
                + " must infer a matching seconds-domain source binding");
    }
}

void testSelfNamedLocalMapFollowsVirtualEventValue()
{
    using namespace talas::project;
    ProjectDocument document;
    ProjectSoundDefinition tone;
    tone.definitionId = "selfMappedTone";
    tone.displayName = "Self Mapped Tone";
    tone.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "7 hz"}, {"amplitude", "!LocalLevel"}}}};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "selfMappedTone";
    document.graph.nodes.push_back(std::move(source));
    auto mapper = midiPitchDocument("", "0 nn", "127 nn", false)
        .graph.nodes[1];
    std::ranges::find(mapper.parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!LocalLevel is: `LocalLevel";
    document.graph.nodes.push_back(std::move(mapper));
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections = {
        {"source", "audioOut", "mapper", "audioIn"},
        {"mapper", "leftOut", "out", "audioIn"},
    };

    talas::runtime::RenderSettings settings{1000.0, 16};
    settings.eventValues = {{"LocalLevel", 0.25}};
    talas::runtime::EventValueChange change;
    change.frame = 8;
    change.name = "LocalLevel";
    change.value = 0.75;
    settings.eventValueChanges.push_back(change);
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "self-named local Map must compile: "
        + (compiled.diagnostics.empty() ? std::string{}
                                        : compiled.diagnostics.front().message));
    const auto& bindings = compiled.graph->renderSettings().eventValues;
    require(std::ranges::count_if(bindings,
                [](const talas::runtime::EventValueBinding& binding) {
                    return binding.name == "LocalLevel";
                }) == 2,
        "self-named Map must keep one global and one local Event Value");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 16);
    require(offline.ok() && offline.mono.size() == 16,
        "self-named local Map must render offline");
    for (const auto frame : {1U, 7U, 9U, 13U})
    {
        const auto sample = std::sin(
            2.0 * std::numbers::pi_v<double> * 7.0
            * static_cast<double>(frame) / 1000.0);
        requireNear(offline.mono[frame],
            (frame < 8 ? 0.25 : 0.75) * sample,
            "self-named local Map must follow the global virtual value at its exact frame");
    }

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "self-named local Map must prepare in realtime");
    std::array<double, 16> left{};
    std::array<double, 16> right{};
    std::size_t offset = 0;
    for (const auto frames : {5U, 3U, 7U, 1U})
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            std::span(left).subspan(offset, frames),
            std::span(right).subspan(offset, frames));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "self-named local Map callback must not allocate");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < left.size(); ++frame)
        requireNear(left[frame], offline.mono[frame],
            "self-named local Map realtime output must match offline");
}

// A Map gives any source a range, so !Glide is:
// (`Glide min: -0.01 max: 0.01) scales the Glide fader's 0 to 1 into it
// for the MIDIMapper's Input.
void testRangedLocalMapScalesAFaderIntoItsRange()
{
    using namespace talas::project;
    ProjectDocument document;
    ProjectSoundDefinition held;
    held.definitionId = "heldGlide";
    held.displayName = "Held Glide";
    held.graph.nodes = {{"glide", "talas.constant", {{"value", "!Glide"}}}};
    held.outputs = {{"value", talas::core::PortType::Control, "glide", "value"}};
    document.soundDefinitions.push_back(std::move(held));
    ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "heldGlide";
    document.graph.nodes.push_back(std::move(source));
    auto mapper = midiPitchDocument("", "0 nn", "127 nn", false)
        .graph.nodes[1];
    std::ranges::find(mapper.parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!Glide is: (`Glide min: -0.01 max: 0.01)";
    document.graph.nodes.push_back(std::move(mapper));
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections = {
        {"source", "value", "mapper", "audioIn"},
        {"mapper", "leftOut", "out", "audioIn"},
    };
    talas::runtime::RenderSettings settings{4000.0, 32};
    settings.eventValues = {{"Glide", 0.75, talas::expr::ValueDomain::Scalar}};
    const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "a ranged fader Map must compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 64);
    require(rendered.ok() && std::abs(rendered.mono.back() - 0.005) < 1.0e-12,
        "the Glide fader at 0.75 should read 0.005 in its range of -0.01 to 0.01");
}

void testRangedLocalMapScalesControllerIntoFrequency()
{
    using namespace talas::project;
    ProjectDocument document;
    ProjectSoundDefinition tone;
    tone.definitionId = "rangedControllerTone";
    tone.displayName = "Ranged Controller Tone";
    tone.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "!Frequency"}, {"amplitude", "1"}}}};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "rangedControllerTone";
    document.graph.nodes.push_back(std::move(source));
    auto mapper = midiPitchDocument("", "0 nn", "127 nn", false)
        .graph.nodes[1];
    std::ranges::find(mapper.parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!Frequency is: (`MIDIController18 min: 100 max: 1000)";
    document.graph.nodes.push_back(std::move(mapper));
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections = {
        {"source", "audioOut", "mapper", "audioIn"},
        {"mapper", "leftOut", "out", "audioIn"},
    };

    talas::runtime::RenderSettings settings{4000.0, 32};
    settings.eventValues = {
        {"Frequency", 200.0, talas::expr::ValueDomain::Hertz}};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto compiled = talas::runtime::compileProject(document, registry, settings);
    require(compiled.ok(), "ranged local controller Map must compile: "
        + (compiled.diagnostics.empty() ? std::string{}
                                        : compiled.diagnostics.front().message));
    const auto& bindings = compiled.graph->renderSettings().eventValues;
    require(std::ranges::any_of(bindings,
                [](const talas::runtime::EventValueBinding& binding) {
                    return binding.name == "Frequency"
                        && binding.scopeId == talas::runtime::globalRuntimeScope
                        && binding.domain == talas::expr::ValueDomain::Hertz
                        && binding.value == 200.0;
                })
            && std::ranges::any_of(bindings,
                [](const talas::runtime::EventValueBinding& binding) {
                    return binding.name == "Frequency"
                        && binding.scopeId != talas::runtime::globalRuntimeScope
                        && binding.domain == talas::expr::ValueDomain::Hertz
                        && binding.value == 100.0
                        && binding.projectionExpression.has_value();
                }),
        "ranged Map must initialize its local Hertz target at min and preserve the global value");

    auto scheduledSettings = settings;
    scheduledSettings.eventValues.push_back({"cc18", 0.0});
    talas::runtime::EventValueChange midpoint;
    midpoint.frame = 10;
    midpoint.name = "cc18";
    midpoint.value = 0.5;
    scheduledSettings.eventValueChanges.push_back(midpoint);
    talas::runtime::EventValueChange maximum = midpoint;
    maximum.frame = 20;
    maximum.value = 1.0;
    scheduledSettings.eventValueChanges.push_back(maximum);
    const auto scheduled = talas::runtime::compileProject(
        document, registry, scheduledSettings);
    require(scheduled.ok(), "scheduled ranged controller Map must compile: "
        + (scheduled.diagnostics.empty() ? std::string{}
                                         : scheduled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*scheduled.graph, 32);
    require(offline.ok() && offline.mono.size() == 32,
        "ranged controller Map must render offline");
    double phase = 0.0;
    for (std::size_t frame = 0; frame < offline.mono.size(); ++frame)
    {
        requireNear(offline.mono[frame],
            std::sin(2.0 * std::numbers::pi_v<double> * phase),
            "ranged Map must output analytic min/mid/max frequency PCM");
        const auto frequency = frame < 10 ? 100.0
            : frame < 20 ? 550.0 : 1000.0;
        phase = std::fmod(phase + frequency / 4000.0, 1.0);
    }

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "ranged controller Map must prepare in realtime");
    require(session.scheduleEventValue(
                "cc18", 0.5, talas::expr::ValueDomain::Scalar, 10)
            && session.scheduleEventValue(
                "cc18", 1.0, talas::expr::ValueDomain::Scalar, 20),
        "ranged controller Map must schedule midpoint and maximum values");
    std::array<double, 32> left{};
    std::array<double, 32> right{};
    std::size_t offset = 0;
    for (const auto frames : {7U, 5U, 4U, 9U, 7U})
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            std::span(left).subspan(offset, frames),
            std::span(right).subspan(offset, frames));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "ranged controller Map callback must not allocate");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < left.size(); ++frame)
        requireNear(left[frame], offline.mono[frame],
            "ranged controller Map realtime output must match offline");

    auto taggedDocument = document;
    std::ranges::find(taggedDocument.graph.nodes[1].parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!Frequency is: (`MIDIController07 channel: 5; min: 100 max: 1000)";
    auto taggedSettings = settings;
    taggedSettings.eventValues.push_back({"cc07", 0.0});
    taggedSettings.eventValues.push_back({"cc07_ch05", 0.0});
    taggedSettings.eventValueChanges = {
        {5, "cc07", 1.0},
        {10, "cc07_ch05", 0.5},
        {20, "cc07_ch05", 1.0},
    };
    const auto taggedCompiled = talas::runtime::compileProject(
        taggedDocument, registry, taggedSettings);
    require(taggedCompiled.ok(),
        "channel and range Map must compile: "
        + (taggedCompiled.diagnostics.empty() ? std::string{}
                                              : taggedCompiled.diagnostics.front().message));
    const auto taggedOffline = talas::runtime::renderOffline(
        *taggedCompiled.graph, 32);
    require(taggedOffline.ok() && taggedOffline.mono.size() == 32,
        "channel and range Map must render offline");
    for (std::size_t frame = 0; frame < taggedOffline.mono.size(); ++frame)
        requireNear(taggedOffline.mono[frame], offline.mono[frame],
            "default-channel CC must not move the tagged ranged projection");
    talas::runtime::RealtimeRenderSession taggedSession(*taggedCompiled.graph);
    require(taggedSession.ready(),
        "channel and range Map must prepare in realtime");
    std::array<double, 32> taggedLeft{};
    std::array<double, 32> taggedRight{};
    offset = 0;
    for (const auto frames : {7U, 5U, 4U, 9U, 7U})
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = taggedSession.render(
            std::span(taggedLeft).subspan(offset, frames),
            std::span(taggedRight).subspan(offset, frames));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "channel and range Map callback must not allocate");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < taggedLeft.size(); ++frame)
        requireNear(taggedLeft[frame], taggedOffline.mono[frame],
            "channel and range Map realtime output must match offline");

    auto forged = *compiled.graph;
    auto& forgedBindings = const_cast<talas::runtime::RenderSettings&>(
        forged.renderSettings()).eventValues;
    const auto local = std::ranges::find_if(forgedBindings,
        [](const talas::runtime::EventValueBinding& binding) {
            return binding.name == "Frequency"
                && binding.scopeId != talas::runtime::globalRuntimeScope;
        });
    require(local != forgedBindings.end(),
        "ranged Map fixture must retain the local projection");
    const auto wrongProjection = talas::expr::parse("!cc18",
        document.expressionLanguage, document.expressionDefinitions);
    require(wrongProjection.ok() && wrongProjection.expression.has_value(),
        "forged projection fixture must parse");
    local->projectionExpression = *wrongProjection.expression;
    talas::runtime::RealtimeRenderSession forgedSession(forged);
    require(!forgedSession.ready(),
        "prepared summary must reject a forged unscaled local projection");
}

void testLocalMapCoversBothNestedStereoInputBranches()
{
    using namespace talas::project;
    ProjectDocument document;
    ProjectSoundDefinition tone;
    tone.definitionId = "stereoMapTone";
    tone.displayName = "Stereo Map Tone";
    tone.graph.nodes = {
        {"leftOsc", "talas.oscillator",
            {{"frequency", "7 hz"}, {"amplitude", "!LocalLevel"}}},
        {"rightOsc", "talas.oscillator",
            {{"frequency", "11 hz"}, {"amplitude", "!LocalLevel"}}},
    };
    tone.outputs = {
        {"leftOut", talas::core::PortType::Audio, "leftOsc", "audioOut"},
        {"rightOut", talas::core::PortType::Audio, "rightOsc", "audioOut"},
    };
    document.soundDefinitions.push_back(std::move(tone));
    ProjectNode input;
    input.instanceId = "input";
    input.soundDefinitionId = "stereoMapTone";
    document.graph.nodes.push_back(std::move(input));
    auto mapper = midiPitchDocument(
        "", "0 nn", "127 nn", false).graph.nodes[1];
    std::ranges::find(mapper.parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!LocalLevel is: `AltLevel";
    document.graph.nodes.push_back(std::move(mapper));
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections = {
        {"input", "leftOut", "mapper", "audioIn"},
        {"input", "rightOut", "mapper", "rightIn"},
        {"mapper", "leftOut", "out", "audioIn"},
        {"mapper", "rightOut", "out", "rightIn"},
    };

    talas::runtime::RenderSettings settings{1000.0, 16};
    settings.eventValues = {{"LocalLevel", 0.2}, {"AltLevel", 0.8}};
    talas::runtime::EventValueChange change;
    change.frame = 8;
    change.name = "AltLevel";
    change.value = 0.4;
    settings.eventValueChanges.push_back(change);
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "stereo local Map must cover both input branches: "
        + (compiled.diagnostics.empty() ? std::string{}
                                        : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 24);
    require(offline.ok(), "stereo local Map must render offline");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "stereo local Map must prepare in realtime");
    std::array<double, 24> left{}, right{};
    std::size_t offset = 0;
    for (const auto frames : {5U, 3U, 7U, 9U})
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            std::span(left).subspan(offset, frames),
            std::span(right).subspan(offset, frames));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "stereo local Map callback must not allocate");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < left.size(); ++frame)
    {
        const auto level = frame < 8 ? 0.8 : 0.4;
        const auto leftExpected = level * std::sin(
            2.0 * std::numbers::pi * 7.0 * frame / 1000.0);
        const auto rightExpected = -0.5 * level * std::sin(
            2.0 * std::numbers::pi * 11.0 * frame / 1000.0);
        requireNear(offline.mono[frame], leftExpected,
            "local Map must reach the left input branch offline");
        requireNear(offline.right[frame], rightExpected,
            "local Map must reach the right input branch offline");
        requireNear(left[frame], leftExpected,
            "local Map must reach the left input branch in realtime");
        requireNear(right[frame], rightExpected,
            "local Map must reach the right input branch in realtime");
    }

    auto crossScope = document;
    ProjectNode foreign;
    foreign.instanceId = "foreign";
    foreign.soundDefinitionId = "stereoMapTone";
    crossScope.graph.nodes.push_back(std::move(foreign));
    crossScope.graph.connections[1].fromNode = "foreign";
    const auto rejected = talas::runtime::compileProject(
        crossScope, talas::core::makeBuiltinNodeRegistry(), settings);
    require(!rejected.ok()
            && std::ranges::any_of(rejected.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.message.find(
                        "distinct nested input Sound scope")
                        != std::string::npos;
                }),
        "local Map must not combine input channels from different Sound scopes");

    auto polyphonic = document;
    std::ranges::find(polyphonic.graph.nodes[1].parameters,
        "voiceModel", &ProjectParameterExpression::parameterId)->choice
        = "polyphonicInput";
    const auto polyphonicCompiled = talas::runtime::compileProject(
        polyphonic, talas::core::makeBuiltinNodeRegistry(), settings);
    require(polyphonicCompiled.ok()
            && polyphonicCompiled.graph->plan().midiVoices.size() == 1,
        "stereo local Map must compile as a polyphonic Input");
    const auto& voices = polyphonicCompiled.graph->plan().midiVoices.front().voices;
    require(voices.size() == 2
            && voices[0].rightAudioOutputBufferIndex.has_value()
            && voices[1].rightAudioOutputBufferIndex.has_value(),
        "each mapped voice must own both stereo Input roots");
    std::vector<std::vector<double>> expected{offline.mono, offline.right};
    for (auto& channel : expected)
        for (auto& sample : channel)
            sample *= 2.0;
    using Kind = talas::runtime::ConformanceMidiNoteKind;
    const std::array notes{
        talas::runtime::ConformanceMidiNote{0, Kind::NoteOn, 1, 60, 127},
        talas::runtime::ConformanceMidiNote{0, Kind::NoteOn, 1, 64, 127},
    };
    const std::array<std::size_t, 4> partition{5, 3, 7, 9};
    const auto conformance = talas::runtime::runRuntimeConformance(
        *polyphonicCompiled.graph, 24, partition,
        {.midiNotes = notes,
         .referenceChannels = expected,
         .verifyResetReplay = true});
    require(conformance.ok() && conformance.resetReplayVerified,
        "two mapped stereo voices must preserve independent Input lanes: "
            + conformance.message);
}

void testMultipleLocalMapsAndGlobalFallback()
{
    using namespace talas::project;
    ProjectDocument document;
    ProjectSoundDefinition tone;
    tone.definitionId = "multiMapTone";
    tone.displayName = "Multiple Map Tone";
    tone.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "7 hz"},
         {"amplitude", "!GainA * !GainB * !GlobalTrim"}}}};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    ProjectNode input;
    input.instanceId = "input";
    input.soundDefinitionId = "multiMapTone";
    document.graph.nodes.push_back(std::move(input));
    auto mapper = midiPitchDocument(
        "", "0 nn", "127 nn", false).graph.nodes[1];
    std::ranges::find(mapper.parameters, "map",
        &ProjectParameterExpression::parameterId)->text =
        "!GainA is: `AltA\n!GainB is: `AltB";
    document.graph.nodes.push_back(std::move(mapper));
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections = {
        {"input", "audioOut", "mapper", "audioIn"},
        {"mapper", "leftOut", "out", "audioIn"},
        {"mapper", "rightOut", "out", "rightIn"},
    };

    talas::runtime::RenderSettings settings{1000.0, 16};
    settings.eventValues = {
        {"GainA", 0.2}, {"GainB", 0.3},
        {"AltA", 0.8}, {"AltB", 0.6},
        {"GlobalTrim", 0.5},
    };
    for (const auto [frame, name, value] : {
             std::tuple{8U, "AltA", 0.4},
             std::tuple{12U, "AltB", 0.2},
             std::tuple{16U, "GlobalTrim", 0.25}})
    {
        talas::runtime::EventValueChange change;
        change.frame = frame;
        change.name = name;
        change.value = value;
        settings.eventValueChanges.push_back(std::move(change));
    }
    const auto saved = serializeProjectDocument(document);
    const auto reopened = parseProjectDocument(saved);
    require(reopened.ok() && serializeProjectDocument(reopened.document) == saved,
        "multiple local Map lines must survive project persistence");
    const auto compiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "multiple local Maps must compile: "
        + (compiled.diagnostics.empty() ? std::string{}
                                        : compiled.diagnostics.front().message));
    const auto& bindings = compiled.graph->renderSettings().eventValues;
    const auto scoped = [&](std::string_view name) {
        return std::ranges::count_if(bindings,
            [&](const talas::runtime::EventValueBinding& binding) {
                return binding.scopeId != talas::runtime::globalRuntimeScope
                    && binding.name == name;
            });
    };
    require(scoped("GainA") == 1 && scoped("GainB") == 1
            && scoped("GlobalTrim") == 0,
        "two local projections must leave the unmapped Event Value global");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 24);
    require(offline.ok(), "multiple local Maps must render offline");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "multiple local Maps must prepare in realtime");
    std::array<double, 24> left{}, right{};
    std::size_t offset = 0;
    for (const auto frames : {5U, 3U, 7U, 9U})
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            std::span(left).subspan(offset, frames),
            std::span(right).subspan(offset, frames));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "multiple local Map callback must not allocate");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < left.size(); ++frame)
    {
        const auto gainA = frame < 8 ? 0.8 : 0.4;
        const auto gainB = frame < 12 ? 0.6 : 0.2;
        const auto trim = frame < 16 ? 0.5 : 0.25;
        const auto expected = gainA * gainB * trim * std::sin(
            2.0 * std::numbers::pi * 7.0 * frame / 1000.0);
        requireNear(offline.mono[frame], expected,
            "offline local projections and global fallback must combine");
        requireNear(offline.right[frame], -0.5 * expected,
            "offline multiple local Maps must retain right attenuation");
        requireNear(left[frame], expected,
            "realtime local projections and global fallback must combine");
        requireNear(right[frame], -0.5 * expected,
            "realtime multiple local Maps must retain right attenuation");
    }

    auto forged = *compiled.graph;
    auto& forgedSettings = const_cast<talas::runtime::RenderSettings&>(
        forged.renderSettings());
    std::erase_if(forgedSettings.eventValues,
        [](const talas::runtime::EventValueBinding& binding) {
            return binding.scopeId != talas::runtime::globalRuntimeScope
                && binding.name == "GainB";
        });
    talas::runtime::RealtimeRenderSession forgedSession(forged);
    require(!forgedSession.ready(),
        "realtime must reject a missing second local Map projection");
    require(!talas::runtime::renderOffline(forged, 4).ok(),
        "offline must reject a missing second local Map projection");

    auto polyphonic = document;
    std::ranges::find(polyphonic.graph.nodes[1].parameters,
        "voiceModel", &ProjectParameterExpression::parameterId)->choice
        = "polyphonicInput";
    const auto polyphonicSaved = serializeProjectDocument(polyphonic);
    const auto polyphonicReopened = parseProjectDocument(polyphonicSaved);
    require(polyphonicReopened.ok()
            && serializeProjectDocument(polyphonicReopened.document)
                == polyphonicSaved,
        "polyphonic local Maps must survive project save and reopen");
    const auto polyphonicCompiled = talas::runtime::compileProject(
        polyphonicReopened.document, talas::core::makeBuiltinNodeRegistry(),
        settings);
    require(polyphonicCompiled.ok(),
        "polyphonic local Maps must compile: "
            + (polyphonicCompiled.diagnostics.empty() ? std::string{}
                : polyphonicCompiled.diagnostics.front().message));
    const auto& polyphonicPlan = polyphonicCompiled.graph->plan();
    require(polyphonicPlan.midiVoices.size() == 1
            && polyphonicPlan.midiVoices.front().voices.size() == 2
            && polyphonicPlan.midiMappers.empty(),
        "polyphonic local Map must use the voice allocator alone");
    const auto polyphonicSummary = talas::runtime::makePreparedGraphSummary(
        *polyphonicCompiled.graph);
    require(polyphonicSummary.ok()
            && polyphonicSummary.summary->midiMappers.size() == 1
            && polyphonicSummary.summary->midiMappers.front().polyphonicInput,
        "prepared graph status must report the local Mapper's voice authority");
    const auto& polyphonicBindings =
        polyphonicCompiled.graph->renderSettings().eventValues;
    const auto projectedCount = [&](std::string_view name) {
        return std::ranges::count_if(polyphonicBindings,
            [&](const talas::runtime::EventValueBinding& binding) {
                return binding.scopeId != talas::runtime::globalRuntimeScope
                    && binding.name == name;
            });
    };
    require(projectedCount("GainA") == 2
            && projectedCount("GainB") == 2
            && projectedCount("GlobalTrim") == 0,
        "each voice must receive both local Maps and share the global fallback");
    const auto polyphonicSilent = talas::runtime::renderOffline(
        *polyphonicCompiled.graph, 24);
    require(polyphonicSilent.ok()
            && std::ranges::all_of(polyphonicSilent.mono,
                [](double sample) { return sample == 0.0; }),
        "mapped polyphonic Input must remain silent without a note");
    std::vector<std::vector<double>> polyphonicExpected{
        offline.mono, offline.right};
    for (auto& channel : polyphonicExpected)
        for (auto& sample : channel)
            sample *= 2.0;
    using Kind = talas::runtime::ConformanceMidiNoteKind;
    const std::array notes{
        talas::runtime::ConformanceMidiNote{0, Kind::NoteOn, 1, 60, 127},
        talas::runtime::ConformanceMidiNote{0, Kind::NoteOn, 1, 64, 127},
    };
    const std::array<std::size_t, 4> polyphonicPartition{5, 3, 7, 9};
    const auto polyphonicConformance = talas::runtime::runRuntimeConformance(
        *polyphonicCompiled.graph, 24, polyphonicPartition,
        {.midiNotes = notes,
         .referenceChannels = polyphonicExpected,
         .verifyResetReplay = true});
    require(polyphonicConformance.ok()
            && polyphonicConformance.resetReplayVerified,
        "two mapped voices must retain changing local/global sources in offline and realtime: "
            + polyphonicConformance.message
            + " actual="
            + (polyphonicConformance.offlineChannels.empty()
                ? std::string{"missing"}
                : std::to_string(
                    polyphonicConformance.offlineChannels[0][1]))
            + " expected=" + std::to_string(polyphonicExpected[0][1]));

    auto missingVoiceProjection = *polyphonicCompiled.graph;
    const auto secondVoiceScope =
        polyphonicPlan.midiVoices.front().voices[1].scopeId;
    auto& missingSettings = const_cast<talas::runtime::RenderSettings&>(
        missingVoiceProjection.renderSettings());
    std::erase_if(missingSettings.eventValues,
        [&](const talas::runtime::EventValueBinding& binding) {
            return binding.scopeId == secondVoiceScope
                && binding.name == "GainB";
        });
    talas::runtime::RealtimeRenderSession missingSession(
        missingVoiceProjection);
    require(!missingSession.ready()
            && !talas::runtime::renderOffline(
                missingVoiceProjection, 4).ok(),
        "prepared renderers must reject a missing second-voice Map projection");
}

void testParserRejectsOversizedAndNonAsciiInput()
{
    const std::string exact(
        talas::runtime::maximumPreparedMidiMapperMapBytes,
        ' ');
    require(talas::runtime::parsePreparedMidiMapperMode(exact).ok(),
        "the exact map byte limit should be admitted");

    const std::string oversized(
        talas::runtime::maximumPreparedMidiMapperMapBytes + 1,
        ' ');
    const auto tooLong =
        talas::runtime::parsePreparedMidiMapperMode(oversized);
    require(!tooLong.ok()
            && tooLong.error == PreparedMidiMapperParseError::TooLong,
        "the first over-bound map must fail closed");

    std::string tooMany;
    for (std::size_t index = 0;
         index <= talas::runtime::maximumPreparedMidiMapperAssociations;
         ++index)
    {
        tooMany += "!Value" + std::to_string(index)
            + " is: `Source\n";
    }
    const auto associations =
        talas::runtime::parsePreparedMidiMapperMode(tooMany);
    require(!associations.ok()
            && associations.error
                == PreparedMidiMapperParseError::TooManyAssociations,
        "the first association over the prepared bound must fail closed");

    const std::string nonAscii{"gate\xc2\xa0", 6};
    const auto invalid =
        talas::runtime::parsePreparedMidiMapperMode(nonAscii);
    require(!invalid.ok()
            && invalid.error
                == PreparedMidiMapperParseError::NonAscii,
        "non-ASCII map bytes must not alias ASCII whitespace");
}

void testConfigurationBoundsAndAnyChannel()
{
    PreparedMidiMapperState passthrough({});
    require(passthrough.ready(),
        "legacy passthrough configuration should be ready");
    requireNear(passthrough.currentGain(), 1.0,
        "legacy passthrough should retain unity mapper gain");
    require(!passthrough.matches(1, 60),
        "legacy passthrough must not claim MIDI ownership");
    require(passthrough.apply(noteOn(1, 60, 100, 0)).status
            == PreparedMidiMapperEventStatus::Filtered,
        "legacy passthrough must preserve the existing MIDI fallback");
    require(passthrough.heldVoiceCount() == 0,
        "legacy passthrough must not retain hidden MIDI state");

    PreparedMidiMapperConfiguration anyChannel;
    anyChannel.mode = PreparedMidiMapperMode::Gate;
    anyChannel.channel = 0;
    anyChannel.lowPitch = 48;
    anyChannel.highPitch = 72;
    anyChannel.polyphony = 4;
    PreparedMidiMapperState state(anyChannel);
    require(state.ready(), "channel zero mapper should be ready");
    require(state.matches(1, 48) && state.matches(16, 72),
        "channel zero should match all valid channels at range boundaries");
    require(!state.matches(0, 60) && !state.matches(17, 60),
        "invalid event channels must not match");
    require(!state.matches(1, 47) && !state.matches(1, 73),
        "notes outside the configured range must not match");

    auto invalid = anyChannel;
    invalid.channel = 17;
    require(!PreparedMidiMapperState(invalid).ready(),
        "configured channel above sixteen must reject");
    invalid = anyChannel;
    invalid.lowPitch = 73;
    invalid.highPitch = 72;
    require(!PreparedMidiMapperState(invalid).ready(),
        "reversed pitch bounds must reject");
    invalid = anyChannel;
    invalid.polyphony = 0;
    require(!PreparedMidiMapperState(invalid).ready(),
        "zero polyphony must reject");
    invalid.polyphony = 129;
    require(!PreparedMidiMapperState(invalid).ready(),
        "polyphony above fixed capacity must reject");
    invalid = anyChannel;
    invalid.mode = static_cast<PreparedMidiMapperMode>(255);
    require(!PreparedMidiMapperState(invalid).ready(),
        "unknown prepared mode must reject");
}

void testGateAndVelocityReduction()
{
    PreparedMidiMapperConfiguration gateConfiguration;
    gateConfiguration.mode = PreparedMidiMapperMode::Gate;
    gateConfiguration.polyphony = 2;
    PreparedMidiMapperState gate(gateConfiguration);
    requireNear(gate.currentGain(), 0.0,
        "mapped gate should begin closed");
    require(gate.apply(noteOn(1, 60, 80, 10)).status
            == PreparedMidiMapperEventStatus::Handled,
        "matching note-on should be handled");
    requireNear(gate.currentGain(), 1.0,
        "matching note-on should open gate");
    require(gate.apply(noteOff(1, 60, 11)).status
            == PreparedMidiMapperEventStatus::Handled,
        "matching note-off should be handled");
    requireNear(gate.currentGain(), 0.0,
        "last matching note-off should close gate");

    PreparedMidiMapperConfiguration velocityConfiguration;
    velocityConfiguration.mode = PreparedMidiMapperMode::Velocity;
    velocityConfiguration.polyphony = 3;
    PreparedMidiMapperState velocity(velocityConfiguration);
    require(velocity.apply(noteOn(1, 60, 64, 20)).status
            == PreparedMidiMapperEventStatus::Handled,
        "first velocity note should be handled");
    requireNear(velocity.currentGain(), 64.0 / 127.0,
        "velocity mode should publish normalized velocity");
    require(velocity.apply(noteOn(1, 61, 100, 21)).status
            == PreparedMidiMapperEventStatus::Handled,
        "second velocity note should be handled");
    requireNear(velocity.currentGain(), 100.0 / 127.0,
        "velocity mode should publish maximum held velocity");
    require(velocity.apply(noteOff(1, 61, 22)).status
            == PreparedMidiMapperEventStatus::Handled,
        "higher-velocity release should be handled");
    requireNear(velocity.currentGain(), 64.0 / 127.0,
        "release should reveal the remaining held velocity");
    require(velocity.apply(noteOn(1, 60, 0, 23)).status
            == PreparedMidiMapperEventStatus::Handled,
        "velocity-zero note-on should normalize to note-off");
    requireNear(velocity.currentGain(), 0.0,
        "velocity-zero note-on should close the final held note");
}

void testAnyChannelRetainsExactNoteOwnership()
{
    PreparedMidiMapperConfiguration configuration;
    configuration.mode = PreparedMidiMapperMode::Velocity;
    configuration.channel = 0;
    configuration.polyphony = 2;
    PreparedMidiMapperState state(configuration);

    require(state.apply(noteOn(2, 60, 100, 0)).status
            == PreparedMidiMapperEventStatus::Handled,
        "any-channel mapper should accept channel-two note-on");
    require(state.apply(noteOn(1, 60, 50, 1)).status
            == PreparedMidiMapperEventStatus::Handled,
        "any-channel mapper should independently accept the same pitch on channel one");
    require(state.apply(noteOff(1, 60, 2)).status
            == PreparedMidiMapperEventStatus::Handled,
        "channel-one note-off should be handled");
    require(state.heldVoiceCount() == 1,
        "channel-one note-off must retain the channel-two voice");
    requireNear(state.currentGain(), 100.0 / 127.0,
        "channel-one note-off must retain the channel-two velocity");
    require(state.apply(noteOff(2, 60, 3)).status
            == PreparedMidiMapperEventStatus::Handled,
        "channel-two note-off should release its exact voice");
    require(state.heldVoiceCount() == 0,
        "exact channel-two release should close the remaining voice");

    configuration.polyphony = 1;
    PreparedMidiMapperState rollback(configuration);
    require(rollback.apply(noteOn(2, 60, 100, 4)).status
            == PreparedMidiMapperEventStatus::Handled,
        "rollback fixture should hold channel-two ownership");
    const auto stolen = rollback.apply(noteOn(1, 60, 50, 5));
    require(stolen.status == PreparedMidiMapperEventStatus::Handled
            && stolen.action.valid,
        "rollback fixture should expose the channel-one steal");
    require(rollback.rollback(stolen.action),
        "rollback should restore the exact stolen channel-two voice");
    require(rollback.apply(noteOff(2, 60, 6)).status
            == PreparedMidiMapperEventStatus::Handled
            && rollback.heldVoiceCount() == 0,
        "rollback must restore the channel identity used by later note-off");
}

void testRepeatedNotesStealingAndFiltering()
{
    PreparedMidiMapperConfiguration configuration;
    configuration.mode = PreparedMidiMapperMode::Velocity;
    configuration.channel = 4;
    configuration.lowPitch = 48;
    configuration.highPitch = 72;
    configuration.polyphony = 2;
    PreparedMidiMapperState state(configuration);

    require(state.apply(noteOn(3, 60, 127, 1)).status
            == PreparedMidiMapperEventStatus::Filtered,
        "other channel should be filtered without mutation");
    require(state.heldVoiceCount() == 0,
        "filtered event must preserve state");

    require(state.apply(noteOn(4, 60, 80, 2)).status
            == PreparedMidiMapperEventStatus::Handled,
        "first repeated note should allocate");
    require(state.apply(noteOn(4, 60, 100, 3)).status
            == PreparedMidiMapperEventStatus::Handled,
        "second repeated note should allocate independently");
    require(state.heldVoiceCount() == 2,
        "repeated notes should occupy fixed independent slots");
    require(state.apply(noteOff(4, 60, 4)).status
            == PreparedMidiMapperEventStatus::Handled,
        "first repeated release should release oldest assignment");
    requireNear(state.currentGain(), 100.0 / 127.0,
        "newer repeated assignment should remain active");

    require(state.apply(noteOn(4, 61, 64, 5)).status
            == PreparedMidiMapperEventStatus::Handled,
        "released slot should be reused");
    require(state.apply(noteOn(4, 62, 32, 6)).status
            == PreparedMidiMapperEventStatus::Handled,
        "full allocator should deterministically steal oldest held slot");
    require(state.heldVoiceCount() == 2,
        "steal should preserve exact configured polyphony");
    requireNear(state.currentGain(), 64.0 / 127.0,
        "stolen maximum should be removed from velocity reduction");
    require(state.apply(noteOff(4, 60, 7)).status
            == PreparedMidiMapperEventStatus::Handled,
        "stale note-off inside owned range should remain handled");
    requireNear(state.currentGain(), 64.0 / 127.0,
        "stale note-off must not alter replacement state");
}

void testRollbackResetAndAuthorityExhaustion()
{
    PreparedMidiMapperConfiguration configuration;
    configuration.mode = PreparedMidiMapperMode::Velocity;
    configuration.polyphony = 2;
    PreparedMidiMapperState state(configuration);
    const auto first = state.apply(noteOn(1, 60, 50, 1));
    const auto second = state.apply(noteOn(1, 61, 100, 2));
    require(first.action.valid && second.action.valid,
        "handled note-ons should expose rollback actions");
    require(!state.rollback(first.action),
        "older action must not leapfrog the latest mapper mutation");
    require(state.rollback(second.action),
        "latest unpublished mapper mutation should roll back");
    requireNear(state.currentGain(), 50.0 / 127.0,
        "rollback should restore exact previous gain");
    require(state.heldVoiceCount() == 1,
        "rollback should restore exact previous held count");
    require(state.rollback(first.action),
        "reverse-order batch rollback should restore the earlier mutation");
    require(state.heldVoiceCount() == 0,
        "complete reverse rollback should restore the initial state");
    state.reset();
    require(state.heldVoiceCount() == 0,
        "reset should clear all held assignments");
    requireNear(state.currentGain(), 0.0,
        "reset should restore mapped closed gain");

    PreparedMidiMapperState generation(configuration);
    talas::runtime::PreparedMidiMapperStateTestAccess::
        exhaustSlotGeneration(generation, 0);
    require(generation.apply(noteOn(1, 60, 100, 0)).status
            == PreparedMidiMapperEventStatus::Handled,
        "one retired slot should allow another fixed slot");
    talas::runtime::PreparedMidiMapperStateTestAccess::
        exhaustSlotGeneration(generation, 1);
    require(generation.apply(noteOn(1, 61, 100, 1)).status
            == PreparedMidiMapperEventStatus::Rejected,
        "all exhausted slots must reject instead of wrapping generation");

    PreparedMidiMapperState activation(configuration);
    talas::runtime::PreparedMidiMapperStateTestAccess::
        exhaustActivationSequence(activation);
    require(activation.apply(noteOn(1, 60, 100, 0)).status
            == PreparedMidiMapperEventStatus::Rejected,
        "activation sequence exhaustion must fail closed");

    PreparedMidiMapperState mutation(configuration);
    require(mutation.apply(noteOn(1, 60, 100, 0)).status
            == PreparedMidiMapperEventStatus::Handled,
        "mutation exhaustion setup should succeed");
    talas::runtime::PreparedMidiMapperStateTestAccess::
        exhaustMutationSequence(mutation);
    require(mutation.apply(noteOff(1, 60, 1)).status
            == PreparedMidiMapperEventStatus::Rejected,
        "mutation sequence exhaustion must fail closed");
}

void testPreparedOperationsDoNotAllocate()
{
    PreparedMidiMapperConfiguration configuration;
    configuration.mode = PreparedMidiMapperMode::Velocity;
    configuration.polyphony = 4;
    PreparedMidiMapperState state(configuration);
    require(state.ready(), "allocation fixture should be ready");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto parsed =
        talas::runtime::parsePreparedMidiMapperMode(" velocity ");
    const auto first = state.apply(noteOn(1, 60, 90, 10));
    const auto second = state.apply(noteOn(1, 61, 91, 11));
    const auto release = state.apply(noteOff(1, 60, 12));
    const auto rolledBack = state.rollback(release.action);
    const auto gain = state.currentGain();
    state.reset();
    countAllocations.store(false, std::memory_order_relaxed);

    require(parsed.ok(), "allocation parser fixture should parse");
    require(first.status == PreparedMidiMapperEventStatus::Handled
            && second.status == PreparedMidiMapperEventStatus::Handled
            && release.status == PreparedMidiMapperEventStatus::Handled
            && rolledBack && gain > 0.0,
        "allocation fixture operations should succeed");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "prepared mapper parser/state operations must not allocate");
}

void testFixedSeedMidiEventPropertyReplay()
{
    PreparedMidiMapperConfiguration configuration;
    configuration.mode = PreparedMidiMapperMode::Velocity;
    configuration.channel = 5;
    configuration.lowPitch = 36;
    configuration.highPitch = 84;
    configuration.polyphony = 8;
    PreparedMidiMapperState state(configuration);
    ReferenceMapper reference{configuration};

    auto randomState = std::uint64_t{0x4331304d494449ULL};
    const auto nextRandom = [&randomState]() {
        randomState ^= randomState << 13U;
        randomState ^= randomState >> 7U;
        randomState ^= randomState << 17U;
        return randomState;
    };
    for (std::uint64_t frame = 0; frame < 4096; ++frame)
    {
        const auto random = nextRandom();
        PreparedMidiMapperEvent event;
        event.kind = (random & 1U) == 0
            ? PreparedMidiMapperEventKind::NoteOn
            : PreparedMidiMapperEventKind::NoteOff;
        event.channel = static_cast<std::uint8_t>(
            nextRandom() % 18U);
        event.note = static_cast<std::uint8_t>(
            nextRandom() % 130U);
        event.velocity = static_cast<std::uint8_t>(
            nextRandom() % 130U);
        event.absoluteFrame = frame;

        const auto expectedStatus = reference.apply(event);
        const auto actual = state.apply(event);
        require(actual.status == expectedStatus,
            "fixed-seed replay status must match the independent model");
        require(state.heldVoiceCount() == reference.heldVoiceCount,
            "fixed-seed replay held count must match the independent model");
        requireNear(state.currentGain(), reference.gain(),
            "fixed-seed replay gain must match the independent model");
    }
}
talas::project::ProjectDocument midiPitchDocument(
    std::string mode, std::string lowPitch, std::string highPitch, bool nested)
{
    using namespace talas::project;
    ProjectDocument document;
    ProjectGraph body;
    body.nodes = {
        {"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "1"}}},
        {"mapper", "talas.midiMapper", {
            {"channel", "1"}, {"polyphony", "2"}, {"left", "1"}, {"right", "-0.5"},
            {"lowPitch", std::move(lowPitch)}, {"highPitch", std::move(highPitch)},
        }},
    };
    auto& parameters = body.nodes.back().parameters;
    for (const auto* name : {"map", "source", "script"})
    {
        ProjectParameterExpression parameter;
        parameter.parameterId = name;
        parameter.text = std::string_view{name} == "map" ? mode : "";
        parameters.push_back(std::move(parameter));
    }
    ProjectParameterExpression file;
    file.parameterId = "midiFile"; file.resource = "";
    parameters.push_back(std::move(file));
    ProjectParameterExpression shared;
    shared.parameterId = "shared"; shared.boolean = false;
    parameters.push_back(std::move(shared));
    ProjectParameterExpression voiceModel;
    voiceModel.parameterId = "voiceModel";
    voiceModel.choice = "legacyPassthrough";
    parameters.push_back(std::move(voiceModel));
    body.connections = {{"osc", "audioOut", "mapper", "audioIn"}};
    std::string outputNode = "mapper";
    if (nested)
    {
        ProjectSoundDefinition definition;
        definition.definitionId = "mappedTone";
        definition.displayName = "Mapped Tone";
        definition.graph = std::move(body);
        definition.outputs = {
            {"leftOut", talas::core::PortType::Audio, "mapper", "leftOut"},
            {"rightOut", talas::core::PortType::Audio, "mapper", "rightOut"},
        };
        document.soundDefinitions.push_back(std::move(definition));
        ProjectNode instance;
        instance.instanceId = "cell"; instance.soundDefinitionId = "mappedTone";
        document.graph.nodes.push_back(std::move(instance));
        outputNode = "cell";
    }
    else document.graph = std::move(body);
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({outputNode, "leftOut", "out", "audioIn"});
    document.graph.connections.push_back({outputNode, "rightOut", "out", "rightIn"});
    return document;
}

void testStereoInputLanesFollowIndependentMidiMapperSignals()
{
    using Kind = talas::runtime::ConformanceMidiNoteKind;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    for (const auto* mode : {"", "gate", "velocity"})
    {
        auto document = midiPitchDocument(mode, "0 nn", "127 nn", false);
        document.graph.nodes.push_back({"rightOsc", "talas.oscillator",
            {{"frequency", "11 hz"}, {"amplitude", "1"}}});
        document.graph.connections.push_back({
            "rightOsc", "audioOut", "mapper", "rightIn"});
        const auto compiled = talas::runtime::compileProject(
            document, registry, {1000.0, 16});
        require(compiled.ok(), std::string{"stereo MIDI Mapper must compile: "}
            + (compiled.diagnostics.empty() ? ""
                                            : compiled.diagnostics.front().message));

        std::vector<talas::runtime::ConformanceMidiNote> notes;
        if (std::string_view{mode} != "")
        {
            notes = {
                {4, Kind::NoteOn, 1, 60, 64},
                {16, Kind::NoteOff, 1, 60, 0},
            };
        }
        std::vector<std::vector<double>> expected(
            2, std::vector<double>(32));
        for (std::size_t frame = 0; frame < 32; ++frame)
        {
            const auto gain = std::string_view{mode} == "" ? 1.0
                : (frame >= 4 && frame < 16)
                ? (std::string_view{mode} == "gate" ? 1.0 : 64.0 / 127.0)
                : 0.0;
            expected[0][frame] = gain * std::sin(
                2.0 * std::numbers::pi * 7.0 * frame / 1000.0);
            expected[1][frame] = -0.5 * gain * std::sin(
                2.0 * std::numbers::pi * 11.0 * frame / 1000.0);
        }
        const std::array<std::size_t, 4> partition{3, 7, 11, 11};
        const auto result = talas::runtime::runRuntimeConformance(
            *compiled.graph, 32, partition,
            {.offlineBlockPartition = std::array<std::size_t, 2>{16, 16},
             .midiNotes = notes,
             .referenceChannels = expected,
             .verifyResetReplay = true});
        require(result.ok() && result.resetReplayVerified,
            std::string{"independent stereo MIDI Mapper input must match "}
                + mode + ": " + result.message);
    }
}

void testDocumentedMapperVoiceGapIsVisible()
{
    // This probe records the current mismatch. Replace the negative assertions
    // with the source-derived voice oracle when polyphonic Mapper Input lands.
    const auto document = midiPitchDocument(
        "", "0 nn", "127 nn", false);
    const auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {1000.0, 8});
    require(compiled.ok(),
        "historical MIDIMapper voice-gap probe must compile");

    const auto& plan = compiled.graph->plan();
    require(plan.midiVoices.empty() && plan.midiMappers.empty()
            && std::ranges::count_if(plan.steps,
                [](const auto& step) {
                    return step.typeId == "talas.oscillator";
                }) == 1,
        "the current blank Map has one shared Input step and no voice plan");

    const auto offline = talas::runtime::renderOffline(
        *compiled.graph, 8);
    require(offline.ok() && offline.mono.size() == 8
            && std::abs(offline.mono[1]) > 0.01,
        "the current offline Mapper audibly renders before a note");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(),
        "historical MIDIMapper voice-gap probe must prepare in realtime");
    require(session.scheduleMidiNote(
                {talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, 60, 127}, 0)
                == talas::runtime::RealtimeMidiDispatchResult::NotHandled,
        "the current blank Map has no MIDI note owner");
    std::array<double, 8> left{}, right{};
    require(session.render(left, right)
            && std::abs(left[1]) > 0.01,
        "the current realtime Mapper audibly renders before a note");

    auto voiceDocument = document;
    voiceDocument.graph.nodes[1].typeId = "talas.midiVoice";
    std::erase_if(voiceDocument.graph.nodes[1].parameters,
        [](const auto& parameter) {
            return parameter.parameterId == "map"
                || parameter.parameterId == "voiceModel";
        });
    const auto voiceCompiled = talas::runtime::compileProject(
        voiceDocument,
        talas::core::makeBuiltinNodeRegistry(),
        {1000.0, 8});
    require(voiceCompiled.ok(),
        "same Input graph must compile through the MIDI Voice path");
    const auto& voicePlan = voiceCompiled.graph->plan();
    require(voicePlan.midiVoices.size() == 1
            && voicePlan.midiVoices.front().voices.size() == 2
            && std::ranges::count_if(voicePlan.steps,
                [](const auto& step) {
                    return step.typeId == "talas.oscillator";
                }) == 2,
        "the existing MIDI Voice path prepares two independent Input steps");
    talas::runtime::RealtimeRenderSession voiceSession(*voiceCompiled.graph);
    require(voiceSession.ready()
            && voiceSession.scheduleMidiNote(
                   {talas::runtime::RealtimeMidiNoteKind::NoteOn,
                       1, 60, 127}, 0)
                == talas::runtime::RealtimeMidiDispatchResult::Handled
            && voiceSession.scheduleMidiNote(
                   {talas::runtime::RealtimeMidiNoteKind::NoteOn,
                       1, 64, 127}, 2)
                == talas::runtime::RealtimeMidiDispatchResult::Handled,
        "the existing MIDI Voice path admits two overlapping notes");
}

void testPolyphonicMapperInputOptInClonesAndGatesVoices()
{
    auto document = midiPitchDocument("", "0 nn", "127 nn", false);
    std::ranges::find(document.graph.nodes[1].parameters,
        "voiceModel",
        &talas::project::ProjectParameterExpression::parameterId)->choice
        = "polyphonicInput";
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 16});
    require(compiled.ok(), "polyphonic Mapper Input must compile: "
        + (compiled.diagnostics.empty() ? std::string{}
                                        : compiled.diagnostics.front().message));
    const auto& plan = compiled.graph->plan();
    require(plan.midiVoices.size() == 1 && plan.midiMappers.empty()
            && plan.midiVoices.front().voices.size() == 2
            && std::ranges::count_if(plan.steps, [](const auto& step) {
                return step.typeId == "talas.oscillator";
            }) == 2,
        "opted-in Mapper must prepare two independent oscillator steps");
    auto defaultDocument = document;
    std::erase_if(defaultDocument.graph.nodes[1].parameters,
        [](const auto& parameter) {
            return parameter.parameterId == "voiceModel";
        });
    const auto defaultCompiled = talas::runtime::compileProject(
        defaultDocument, talas::core::makeBuiltinNodeRegistry(),
        {1000.0, 16});
    require(defaultCompiled.ok()
            && defaultCompiled.graph->plan().midiVoices.size() == 1,
        "a new format-38 Mapper must default to polyphonic Input");

    const auto silent = talas::runtime::renderOffline(*compiled.graph, 16);
    require(silent.ok() && silent.mono.size() == 16
            && std::ranges::all_of(silent.mono, [](double value) {
                return value == 0.0;
            })
            && std::ranges::all_of(silent.right, [](double value) {
                return value == 0.0;
            }),
        "polyphonic Mapper must not emit Input before a MIDI note");

    talas::runtime::RealtimeRenderSession one(*compiled.graph);
    talas::runtime::RealtimeRenderSession two(*compiled.graph);
    require(one.ready() && two.ready(),
        "polyphonic Mapper must prepare realtime voice sessions: "
            + std::string{one.error()} + " / "
            + std::string{two.error()});
    const talas::runtime::RealtimeMidiNoteEvent first{
        talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, 60, 127};
    const talas::runtime::RealtimeMidiNoteEvent second{
        talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, 64, 127};
    require(one.scheduleMidiNote(first, 0)
            == talas::runtime::RealtimeMidiDispatchResult::Handled
            && two.scheduleMidiNote(first, 0)
                == talas::runtime::RealtimeMidiDispatchResult::Handled
            && two.scheduleMidiNote(second, 0)
                == talas::runtime::RealtimeMidiDispatchResult::Handled,
        "polyphonic Mapper must accept two overlapping notes");
    std::array<double, 16> oneLeft{}, oneRight{}, twoLeft{}, twoRight{};
    require(one.render(oneLeft, oneRight)
            && two.render(twoLeft, twoRight)
            && std::abs(oneLeft[1]) > 0.01,
        "polyphonic Mapper must render an audible note");
    requireNear(twoLeft[1], 2.0 * oneLeft[1],
        "simultaneous Mapper notes must mix two Input copies");
    requireNear(twoRight[1], 2.0 * oneRight[1],
        "simultaneous Mapper notes must mix two right Input copies");

    talas::runtime::RealtimeRenderSession lateOnly(*compiled.graph);
    talas::runtime::RealtimeRenderSession staggered(*compiled.graph);
    require(lateOnly.ready() && staggered.ready()
            && lateOnly.scheduleMidiNote(second, 3)
                == talas::runtime::RealtimeMidiDispatchResult::Handled
            && staggered.scheduleMidiNote(first, 0)
                == talas::runtime::RealtimeMidiDispatchResult::Handled
            && staggered.scheduleMidiNote(second, 3)
                == talas::runtime::RealtimeMidiDispatchResult::Handled,
        "staggered Mapper voices must accept independent onset frames");
    std::array<double, 16> lateLeft{}, lateRight{}, staggeredLeft{},
        staggeredRight{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto renderedStaggered = lateOnly.render(lateLeft, lateRight)
        && staggered.render(staggeredLeft, staggeredRight);
    countAllocations.store(false, std::memory_order_relaxed);
    require(renderedStaggered
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "overlapping Mapper voice callback must render without allocation");
    for (std::size_t frame = 0; frame < staggeredLeft.size(); ++frame)
    {
        requireNear(staggeredLeft[frame],
            oneLeft[frame] + lateLeft[frame],
            "overlapping Mapper voices must sum independently triggered left states");
        requireNear(staggeredRight[frame],
            oneRight[frame] + lateRight[frame],
            "overlapping Mapper voices must sum independently triggered right states");
    }

    auto numbered = document;
    numbered.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VoiceV1;
    numbered.graph.nodes[0].parameters[0].expression =
        "?VoiceNumber * 7 hz";
    const auto numberedCompiled = talas::runtime::compileProject(
        numbered, talas::core::makeBuiltinNodeRegistry(), {1000.0, 16});
    require(numberedCompiled.ok(),
        "Mapper Input must specialize each voice's stateful source: "
            + (numberedCompiled.diagnostics.empty() ? std::string{}
                : numberedCompiled.diagnostics.front().message));
    const auto& numberedPlan = numberedCompiled.graph->plan();
    require(numberedPlan.midiVoices.size() == 1
            && numberedPlan.midiVoices.front().voices.size() == 2,
        "numbered Mapper must retain two exact voice instances");
    const auto firstIndex =
        numberedPlan.midiVoices.front().voices[0].firstStepIndex;
    const auto secondIndex =
        numberedPlan.midiVoices.front().voices[1].firstStepIndex;
    const auto firstFrequency = std::ranges::find(
        numberedPlan.steps[firstIndex].parameters, "frequency",
        &talas::runtime::ParameterValue::id);
    const auto secondFrequency = std::ranges::find(
        numberedPlan.steps[secondIndex].parameters, "frequency",
        &talas::runtime::ParameterValue::id);
    require(firstFrequency != numberedPlan.steps[firstIndex].parameters.end()
            && secondFrequency != numberedPlan.steps[secondIndex].parameters.end()
            && firstFrequency->value == 7.0
            && secondFrequency->value == 14.0,
        "Mapper voice copies must specialize independent source frequencies");
    talas::runtime::RealtimeRenderSession numberedSession(*numberedCompiled.graph);
    require(numberedSession.ready()
            && numberedSession.scheduleMidiNote(first, 0)
                == talas::runtime::RealtimeMidiDispatchResult::Handled
            && numberedSession.scheduleMidiNote(second, 0)
                == talas::runtime::RealtimeMidiDispatchResult::Handled,
        "numbered Mapper must accept two notes into distinct voice slots");
    std::array<double, 16> numberedLeft{}, numberedRight{};
    require(numberedSession.render(numberedLeft, numberedRight),
        "numbered Mapper must render its two source frequencies");
    for (std::size_t frame = 0; frame < numberedLeft.size(); ++frame)
    {
        const auto expected = std::sin(2.0 * std::numbers::pi
            * 7.0 * frame / 1000.0)
            + std::sin(2.0 * std::numbers::pi
                * 14.0 * frame / 1000.0);
        requireNear(numberedLeft[frame], expected,
            "Mapper voice mixer must add two distinct source frequencies");
        requireNear(numberedRight[frame], -0.5 * expected,
            "Mapper voice mixer must retain the right output gain");
    }

    auto nested = midiPitchDocument("", "0 nn", "127 nn", true);
    std::ranges::find(nested.soundDefinitions.front().graph.nodes[1].parameters,
        "voiceModel",
        &talas::project::ProjectParameterExpression::parameterId)->choice
        = "polyphonicInput";
    const auto saved = talas::project::serializeProjectDocument(nested);
    const auto reopened = talas::project::parseProjectDocument(saved);
    require(reopened.ok(),
        "nested polyphonic Mapper must survive save and reopen");
    const auto nestedCompiled = talas::runtime::compileProject(
        reopened.document, talas::core::makeBuiltinNodeRegistry(),
        {1000.0, 16});
    require(nestedCompiled.ok()
            && nestedCompiled.graph->plan().midiVoices.size() == 1
            && nestedCompiled.graph->plan().midiVoices.front().voices.size() == 2,
        "nested polyphonic Mapper must retain exact owner and two voices");
    talas::runtime::RealtimeRenderSession nestedSession(*nestedCompiled.graph);
    require(nestedSession.ready()
            && nestedSession.scheduleMidiNote(first, 0)
                == talas::runtime::RealtimeMidiDispatchResult::Handled
            && nestedSession.scheduleMidiNote(second, 0)
                == talas::runtime::RealtimeMidiDispatchResult::Handled,
        "nested Mapper owner must route two overlapping MIDI notes");
}

void testFormatThirtySevenMapperMigrationPreservesLegacyInput()
{
    for (const bool nested : {false, true})
    {
        auto document = midiPitchDocument("", "0 nn", "127 nn", nested);
        auto& body = nested
            ? document.soundDefinitions.front().graph
            : document.graph;
        std::erase_if(body.nodes[1].parameters,
            [](const auto& parameter) {
                return parameter.parameterId == "voiceModel";
            });
        auto source = talas::project::serializeProjectDocument(document);
        const auto version = source.find("\"version\": 156");
        require(version != std::string::npos,
            "migration fixture must start in the current project format");
        source.replace(version, std::string_view{"\"version\": 156"}.size(),
            "\"version\":  37");
        const auto loaded = talas::project::parseProjectDocument(source);
        require(loaded.ok() && loaded.document.formatVersion == talas::project::currentProjectFormatVersion,
            "format-37 Mapper must migrate to format 38");
        const auto& loadedBody = nested
            ? loaded.document.soundDefinitions.front().graph
            : loaded.document.graph;
        const auto model = std::ranges::find(
            loadedBody.nodes[1].parameters,
            "voiceModel",
            &talas::project::ProjectParameterExpression::parameterId);
        require(model != loadedBody.nodes[1].parameters.end()
                && model->choice == "legacyPassthrough",
            "old root and nested Mapper instances must gain explicit legacy behavior");
        const auto canonical = talas::project::serializeProjectDocument(
            loaded.document);
        require(canonical.find("\"version\": 156") != std::string::npos
                && canonical.find("\"choice\": \"legacyPassthrough\"")
                    != std::string::npos,
            "migrated Mapper must persist its explicit legacy choice");
        const auto reopened = talas::project::parseProjectDocument(canonical);
        require(reopened.ok()
                && talas::project::serializeProjectDocument(reopened.document)
                    == canonical,
            "migrated Mapper must reopen byte-stably");
        const auto compiled = talas::runtime::compileProject(
            reopened.document, talas::core::makeBuiltinNodeRegistry(),
            {1000.0, 16});
        require(compiled.ok() && compiled.graph->plan().midiVoices.empty(),
            "migrated blank Map must preserve its shared Input plan");
        const auto rendered = talas::runtime::renderOffline(
            *compiled.graph, 16);
        require(rendered.ok() && std::abs(rendered.mono[1]) > 0.01,
            "migrated blank Map must preserve audible no-note PCM");

        auto prematureModel = source;
        const auto mapId = prematureModel.find(
            R"json("id": "map")json");
        const auto mapParameter = mapId == std::string::npos
            ? std::string::npos
            : prematureModel.rfind('{', mapId);
        require(mapParameter != std::string::npos,
            "old Mapper fixture must expose its Map parameter");
        prematureModel.insert(mapParameter,
            R"json({"id": "voiceModel", "choice": "polyphonicInput"}, )json");
        require(!talas::project::parseProjectDocument(prematureModel).ok(),
            "format 37 must reject a premature voiceModel choice");
    }
}

void testPublicMidiPitchBounds()
{
    struct Bounds { const char* low; const char* high; std::uint8_t first; std::uint8_t last; };
    const std::array cases{
        Bounds{"69 nn", "70 nn", 69, 70},
        Bounds{"442 hz", "470 hz", 69, 70},
        Bounds{"449 hz", "475 hz", 69, 70},
        Bounds{"453 hz", "480 hz", 70, 71},
        Bounds{"69.5 nn", "70.5 nn", 70, 71},
        Bounds{"0 nn", "127 nn", 0, 127},
        Bounds{"69.49 nn", "69.51 nn", 69, 70},
    };
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    for (const auto& bounds : cases)
    for (const auto* mode : {"gate", "velocity"})
    for (const bool nested : {false, true})
    {
        const auto document = midiPitchDocument(mode, bounds.low, bounds.high, nested);
        require(talas::project::auditProjectExpressions(document, registry, document.expressionLanguage).ok(),
            "MIDI pitch units must pass the project authoring audit");
        const auto& mapper = nested ? document.soundDefinitions.front().graph.nodes[1]
                                   : document.graph.nodes[1];
        const auto* descriptor = registry.findNodeType("talas.midiMapper");
        for (const auto* id : {"lowPitch", "highPitch"})
        {
            const auto parameter = std::ranges::find(mapper.parameters, id,
                &talas::project::ProjectParameterExpression::parameterId);
            const auto schema = std::ranges::find(descriptor->parameters, id,
                &talas::core::ParameterDescriptor::id);
            require(parameter != mapper.parameters.end() && schema != descriptor->parameters.end(),
                "test pitch parameters must exist");
            require(talas::project::auditProjectParameterExpression(document, mapper, *parameter,
                *schema, document.expressionLanguage).ok(), "MIDI pitch editor audit must accept both units");
        }
        const auto compiled = talas::runtime::compileProject(document, registry, {1000.0, 16});
        require(compiled.ok(), std::string{"MIDI pitch bounds must compile: "} + bounds.low
            + " / " + bounds.high + ": "
            + (compiled.diagnostics.empty() ? "" : compiled.diagnostics.front().message));
        const auto silent = talas::runtime::renderOffline(*compiled.graph, 32);
        require(silent.ok() && silent.mono == std::vector<double>(32),
            "native no-MIDI render must retain a silent closed mapper");
        require(compiled.graph->plan().midiMappers.size() == 1,
            "rounded bounds retain one exact mapper");
        const auto& configuration = compiled.graph->plan().midiMappers.front().configuration;
        require(configuration.lowPitch == bounds.first && configuration.highPitch == bounds.last,
            "both pitch bounds round to their nearest MIDI note");
        using Kind = talas::runtime::ConformanceMidiNoteKind;
        std::vector<talas::runtime::ConformanceMidiNote> notes;
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), "rounded MIDI pitch session must prepare");
        std::vector<std::vector<double>> expected(2, std::vector<double>(32));
        const auto gain = std::string_view{mode} == "gate" ? 1.0 : 64.0 / 127.0;
        for (std::size_t index = 0; index < 4; ++index)
        {
            const auto pitch = static_cast<std::uint8_t>(index == 0
                ? (bounds.first == 0 ? 1 : bounds.first - 1)
                : index == 1 ? bounds.first
                : index == 2 ? bounds.last
                : bounds.last == 127 ? 126 : bounds.last + 1);
            const auto channel = static_cast<std::uint8_t>(
                (index == 0 && bounds.first == 0) || (index == 3 && bounds.last == 127) ? 2 : 1);
            if (index != 1 && index != 2)
            {
                require(session.scheduleMidiNote({talas::runtime::RealtimeMidiNoteKind::NoteOn,
                        channel, pitch, 64}, 1) == talas::runtime::RealtimeMidiDispatchResult::NotHandled,
                    "notes outside rounded bounds or on another channel must remain unhandled");
                talas::runtime::OfflineRenderSession offline(*compiled.graph);
                talas::runtime::OfflineMidiVoiceEvent event;
                event.frame = 1; event.channel = channel; event.note = pitch; event.velocity = 64;
                event.kind = talas::runtime::OfflineMidiVoiceEventKind::NoteOn;
                require(!offline.scheduleMidiVoiceEvents(std::span{&event, 1}),
                    "offline schedules without a matching rounded MIDI target must reject");
                continue;
            }
            notes.push_back({8 * index + 1, Kind::NoteOn, channel, pitch, 64});
            notes.push_back({8 * index + 6, Kind::NoteOff, channel, pitch, 0});
            for (std::size_t frame = 8 * index + 1; frame < 8 * index + 6; ++frame)
            {
                expected[0][frame] = gain * std::sin(2.0 * std::numbers::pi * 7.0 * frame / 1000.0);
                expected[1][frame] = -0.5 * expected[0][frame];
            }
        }
        const std::array<std::size_t, 4> irregular{3, 7, 11, 11};
        const std::array<std::size_t, 2> regular{16, 16};
        const auto result = talas::runtime::runRuntimeConformance(*compiled.graph, 32, irregular,
            {.offlineBlockPartition = regular, .midiNotes = notes,
             .referenceChannels = expected, .verifyResetReplay = true});
        require(result.ok() && result.resetReplayVerified,
            "rounded MIDI pitch boundaries must match timed stereo oracle and reset: " + result.message);
        const auto uniform = talas::runtime::runRuntimeConformance(*compiled.graph, 32, regular,
            {.midiNotes = notes, .referenceChannels = expected});
        require(uniform.ok() && uniform.realtimeChannels == result.realtimeChannels,
            "rounded MIDI pitch filtering must be callback-partition independent: " + uniform.message);
        require(session.scheduleMidiNote({talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, bounds.first, 64}, 1)
                == talas::runtime::RealtimeMidiDispatchResult::Handled,
            "rounded lower endpoint must accept note-on");
        std::array<double, 8> output{};
        allocationCount = 0; countAllocations = true;
        const bool rendered = session.render(output);
        countAllocations = false;
        require(rendered && allocationCount == 0, "rounded MIDI pitch callbacks must not allocate");
    }
}

void testMidiPitchBoundsRejectInvalidAndForgedInputs()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    for (const auto& bounds : std::array{
        std::pair{"-0.1 nn", "69 nn"}, std::pair{"69 nn", "127.1 nn"},
        std::pair{"69.8 nn", "69.2 nn"}, std::pair{"0 hz", "440 hz"},
        std::pair{"-440 hz", "440 hz"}, std::pair{"8 hz", "440 hz"},
        std::pair{"440 hz", "13000 hz"}, std::pair{"1 s", "70 nn"},
        std::pair{"69 nn", "2 samples"}, std::pair{"1 / 0 nn", "70 nn"}})
    {
        require(!talas::runtime::compileProject(midiPitchDocument("gate", bounds.first,
            bounds.second, false), registry, {1000.0, 16}).ok(),
            "invalid pitch domains, reversed and out-of-range bounds must reject");
    }
    const auto parsed = talas::expr::parse("442 hz");
    require(parsed.ok() && !talas::expr::evaluate(*parsed.expression, {},
        {talas::expr::ValueDomain::NoteNumber}).ok(),
        "MIDI's frequency conversion must not change generic expression unit rules");
    for (const auto value : {std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        require(!talas::project::roundedMidiPitchBounds(value, 70).has_value()
            && !talas::project::roundedMidiPitchBounds(69, value).has_value(),
            "nonfinite bounds must reject before rounding or integer conversion");
    }
    const auto compiled = talas::runtime::compileProject(
        midiPitchDocument("gate", "442 hz", "470 hz", false), registry, {1000.0, 16});
    require(compiled.ok(), "forgery fixture must compile");
    for (int mutation = 0; mutation < 7; ++mutation)
    {
        auto forged = *compiled.graph;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        auto& mapper = plan.midiMappers.front();
        auto& step = plan.steps[mapper.ownerStepIndex];
        if (mutation < 3)
        {
            auto& parameter = *std::ranges::find(step.parameters,
                mutation == 1 ? "highPitch" : "lowPitch", &talas::runtime::ParameterValue::id);
            parameter.value = mutation == 0 ? 70.1 : mutation == 1 ? 128.0
                : std::numeric_limits<double>::quiet_NaN();
        }
        else if (mutation == 3) mapper.configuration.lowPitch = 68;
        else if (mutation == 4) mapper.configuration.highPitch = 71;
        else if (mutation == 5) plan.midiMappers.push_back(mapper);
        else mapper.ownerStepIndex = plan.steps.size();
        talas::runtime::RealtimeRenderSession session(forged);
        require(!session.ready(), "forged or inconsistent rounded MIDI plans must reject");
    }
}

void testMidiVoiceFrequencyBounds()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    for (const bool nested : {false, true})
    {
        auto document = midiPitchDocument("gate", "442 hz", "470 hz", nested);
        auto& body = nested ? document.soundDefinitions.front().graph : document.graph;
        auto& voice = body.nodes[1];
        voice.typeId = "talas.midiVoice";
        std::erase_if(voice.parameters, [](const auto& p) {
            return p.parameterId == "map" || p.parameterId == "voiceModel";
        });
        body.nodes[0].parameters[1].expression = "(!KeyDown > 0) * !KeyVelocity";
        talas::runtime::RenderSettings settings{1000.0, 16};
        settings.eventValues = {{"Pitch", 69, talas::expr::ValueDomain::NoteNumber},
            {"KeyVelocity", 0}, {"KeyDown", 0}};
        const auto compiled = talas::runtime::compileProject(document, registry, settings);
        require(compiled.ok(), "MIDI Voice must accept the same frequency pitch bounds");
        require(compiled.graph->plan().midiVoices.front().configuration.lowPitch == 69
            && compiled.graph->plan().midiVoices.front().configuration.highPitch == 70,
            "MIDI Voice frequency bounds must round before allocation");
        using Kind = talas::runtime::ConformanceMidiNoteKind;
        const std::array<talas::runtime::ConformanceMidiNote, 4> notes{{
            {1, Kind::NoteOn, 1, 69, 127}, {10, Kind::NoteOff, 1, 69, 0},
            {17, Kind::NoteOn, 1, 70, 127}, {26, Kind::NoteOff, 1, 70, 0},
        }};
        std::vector<std::vector<double>> expected(2, std::vector<double>(32));
        for (std::size_t frame = 0; frame < 32; ++frame)
        {
            if (!(frame >= 6 && frame < 10) && !(frame >= 22 && frame < 26)) continue;
            expected[0][frame] = std::sin(2.0 * std::numbers::pi * 7.0 * frame / 1000.0);
            expected[1][frame] = -0.5 * expected[0][frame];
        }
        const std::array<std::size_t, 4> partition{3, 7, 11, 11};
        const auto result = talas::runtime::runRuntimeConformance(*compiled.graph, 32, partition,
            {.midiNotes = notes, .referenceChannels = expected, .verifyResetReplay = true});
        require(result.ok() && result.resetReplayVerified,
            "MIDI Voice frequency bounds must retain attack, release and reset behavior: " + result.message);
    }
}

void testVoiceProfilePreservesOrdinaryNonMidiRendering()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"osc", "talas.oscillator", {{"frequency", "7 hz"}, {"amplitude", "0.25"}}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {{"osc", "audioOut", "out", "audioIn"},
        {"osc", "audioOut", "out", "rightIn"}};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    std::vector<std::vector<double>> expected(2, std::vector<double>(32));
    for (std::size_t frame = 0; frame < 32; ++frame)
        expected[0][frame] = expected[1][frame]
            = 0.25 * std::sin(2.0 * std::numbers::pi * 7.0 * frame / 1000.0);
    const std::array<std::size_t, 4> partition{3, 7, 11, 11};
    for (const auto language : {talas::expr::ExpressionLanguageVersion::VariablesV2,
             talas::expr::ExpressionLanguageVersion::VoiceV1})
    {
        document.expressionLanguage = language;
        const auto compiled = talas::runtime::compileProject(document, registry, {1000.0, 16});
        require(compiled.ok() && compiled.graph->plan().midiVoices.empty(),
            "upgrading an ordinary graph must not require a MIDI Voice owner");
        const auto result = talas::runtime::runRuntimeConformance(*compiled.graph, 32, partition,
            {.referenceChannels = expected, .verifyResetReplay = true});
        require(result.ok() && result.resetReplayVerified,
            "ordinary VoiceV1 expressions must retain native/realtime PCM and reset: " + result.message);
    }
}

void testVoiceNumberCompilesDistinctVoiceParameters()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    for (const bool nested : {false, true})
    for (const bool timedControls : {false, true})
    {
        auto document = midiPitchDocument("gate", "0 nn", "127 nn", nested);
        document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VoiceV1;
        auto& body = nested ? document.soundDefinitions.front().graph : document.graph;
        body.nodes[1].typeId = "talas.midiVoice";
        std::erase_if(body.nodes[1].parameters,
            [](const auto& p) {
                return p.parameterId == "map"
                    || p.parameterId == "voiceModel";
            });
        const auto authoredFrequency = timedControls
            ? "?VoiceNumber * !Rate * 1 hz" : "?VoiceNumber * 7 hz";
        body.nodes[0].parameters[0].expression = authoredFrequency;
        body.nodes[0].parameters[1].expression = "(!KeyDown > 0) * !KeyVelocity";
        talas::runtime::RenderSettings settings{1000.0, 16};
        settings.eventValues = {{"Pitch", 69, talas::expr::ValueDomain::NoteNumber},
            {"KeyVelocity", 0}, {"KeyDown", 0}, {"Rate", 7}};
        const auto compiled = talas::runtime::compileProject(document, registry, settings);
        require(compiled.ok(), "VoiceNumber instrument must compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto& plan = compiled.graph->plan();
        require(plan.midiVoices.size() == 1 && plan.midiVoices.front().voices.size() == 2,
            "VoiceNumber instrument must retain its two slots");
        for (std::size_t index = 0; index < 2; ++index)
        {
            const auto& voice = plan.midiVoices.front().voices[index];
            const auto& step = plan.steps[voice.firstStepIndex];
            const auto frequency = std::ranges::find(step.parameters,
                "frequency", &talas::runtime::ParameterValue::id);
            require(frequency != step.parameters.end() && frequency->expression.has_value()
                && !frequency->expression->usesVoiceNumber()
                && frequency->expression->identifiers().empty()
                && frequency->value == 7.0 * (index + 1),
                "each prepared voice must own its one-based literal frequency");
        }
        require(body.nodes[0].parameters[0].expression == authoredFrequency,
            "preparation must preserve the authored template");
        const talas::runtime::PreparedRuntimeGraph unsealed(compiled.graph->plan(), compiled.graph->renderSettings());
        talas::runtime::RealtimeRenderSession unsealedSession(unsealed);
        require(!unsealedSession.ready() && !talas::runtime::renderOffline(unsealed, 8).ok(),
            "copied public plan and settings cannot mint voice specialization authority");
        auto downgradedSettings = compiled.graph->renderSettings();
        downgradedSettings.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV2;
        const talas::runtime::PreparedRuntimeGraph downgradedPublic(
            compiled.graph->plan(), downgradedSettings);
        talas::runtime::RealtimeRenderSession downgradedSession(downgradedPublic);
        require(!downgradedSession.ready(),
            "public reconstruction and language downgrade cannot erase voice specialization authority");
        require(!talas::runtime::renderOffline(downgradedPublic, 8).ok(),
            "native offline must reject reconstructed and downgraded voice specialization authority");
        for (int mutation = 0; mutation < 5; ++mutation)
        {
            auto forged = *compiled.graph;
            auto& forgedPlan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
            auto& voice = forgedPlan.midiVoices.front().voices.front();
            auto& step = forgedPlan.steps[voice.firstStepIndex];
            auto frequency = std::ranges::find(step.parameters, "frequency", &talas::runtime::ParameterValue::id);
            if (mutation == 0) frequency->value = 14;
            else if (mutation == 1)
                frequency->expression = talas::expr::parse("?VoiceNumber * 7 hz",
                    talas::expr::ExpressionLanguageVersion::VoiceV1).expression;
            else if (mutation == 2)
                frequency->expression = talas::expr::parse("14 hz",
                    talas::expr::ExpressionLanguageVersion::VoiceV1).expression;
            else if (mutation == 3) voice.voiceOrdinal = 1;
            else const_cast<talas::runtime::RenderSettings&>(forged.renderSettings()).expressionLanguage
                = talas::expr::ExpressionLanguageVersion::VariablesV2;
            talas::runtime::RealtimeRenderSession rejected(forged);
            require(!rejected.ready(), "forged voice specialization authority must reject: " + std::to_string(mutation));
            require(!talas::runtime::renderOffline(forged, 8).ok(),
                "native offline must reject forged voice specialization authority");
        }
        using Kind = talas::runtime::ConformanceMidiNoteKind;
        const std::array<talas::runtime::ConformanceMidiNote, 7> notes{{
            {1, Kind::NoteOn, 1, 60, 127}, {2, Kind::NoteOn, 1, 61, 64},
            {10, Kind::NoteOff, 1, 60, 0}, {12, Kind::NoteOn, 1, 62, 100},
            {18, Kind::NoteOn, 1, 63, 80}, {26, Kind::NoteOff, 1, 62, 0},
            {29, Kind::NoteOff, 1, 63, 0},
        }};
        std::vector<std::vector<double>> expected(2, std::vector<double>(32));
        for (std::size_t frame = 0; frame < 32; ++frame)
        {
            const auto firstGain = frame >= 6 && frame < 10 ? 1.0
                : frame >= 17 && frame < 26 ? 100.0 / 127.0 : 0.0;
            const auto secondGain = frame >= 7 && frame < 18 ? 64.0 / 127.0
                : frame >= 23 && frame < 29 ? 80.0 / 127.0 : 0.0;
            const auto phaseTime = timedControls
                ? 7.0 * std::min(frame, std::size_t{9}) + 11.0 * (frame > 9 ? frame - 9 : 0)
                : 7.0 * frame;
            expected[0][frame] = firstGain * std::sin(2.0 * std::numbers::pi * phaseTime / 1000.0)
                + secondGain * std::sin(4.0 * std::numbers::pi * phaseTime / 1000.0);
            expected[1][frame] = -0.5 * expected[0][frame];
        }
        const std::array<std::size_t, 4> partition{3, 7, 11, 11};
        const std::array<std::size_t, 2> uniformPartition{16, 16};
        std::vector<talas::runtime::ConformanceInputEvent> inputs(notes.begin(), notes.end());
        if (timedControls)
        {
            inputs.emplace_back(talas::runtime::ConformanceEventValueAssignment{0, "Rate", 7});
            inputs.emplace_back(talas::runtime::ConformanceEventValueAssignment{9, "Rate", 11});
            const auto frameOf = [](const auto& event) {
                return std::visit([](const auto& value) { return value.frame; }, event);
            };
            std::stable_sort(inputs.begin(), inputs.end(), [&](const auto& left, const auto& right) {
                return frameOf(left) < frameOf(right);
            });
        }
        const auto result = talas::runtime::runRuntimeConformance(*compiled.graph, 32, partition,
            {.offlineBlockPartition = uniformPartition,
             .referenceChannels = expected, .verifyResetReplay = true, .inputEvents = inputs});
        require(result.ok() && result.resetReplayVerified,
            "one-based voice frequencies must survive reuse, stealing and reset: " + result.message);
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), "specialized voice session must prepare");
        require(session.scheduleMidiNote({talas::runtime::RealtimeMidiNoteKind::NoteOn, 1, 60, 127}, 1)
                == talas::runtime::RealtimeMidiDispatchResult::Handled,
            "specialized voice must accept its note");
        std::array<double, 16> output{};
        allocationCount = 0; countAllocations = true;
        const auto rendered = session.render(output);
        countAllocations = false;
        require(rendered && allocationCount == 0,
            "specialized voice callback must not allocate");

        // A negative frequency reads backwards; one that is
        // not finite for the second voice stays refused.
        body.nodes[0].parameters[0].expression = "7 hz / (2 - ?VoiceNumber)";
        const auto invalid = talas::runtime::compileProject(document, registry, settings);
        require(!invalid.ok() && std::ranges::any_of(invalid.diagnostics, [](const auto& d) {
                return d.parameterId == "frequency";
            }), "a frequency valid only for the first voice must reject the whole instrument");
        body.nodes[0].parameters[0].expression = "?VoiceNumber * 7 hz";
        body.nodes[1].parameters.push_back({"map", {}, {}, {}, {}, "gate"});
        body.nodes[1].typeId = "talas.midiMapper";
        const auto outside = talas::runtime::compileProject(document, registry, settings);
        require(!outside.ok(), "a mapper must not supply an implicit polyphonic voice number");
    }
}

void testVoiceNumberStaticNoiseSeedsMatchLiteralInstruments()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto makeDocument = [](int nesting, std::string initialState, int polyphony,
                                 talas::expr::ExpressionLanguageVersion language) {
        auto document = midiPitchDocument("gate", "0 nn", "127 nn", nesting == 1);
        document.expressionLanguage = language;
        auto& body = nesting == 1 ? document.soundDefinitions.front().graph : document.graph;
        body.nodes[1].typeId = "talas.midiVoice";
        std::erase_if(body.nodes[1].parameters,
            [](const auto& p) {
                return p.parameterId == "map"
                    || p.parameterId == "voiceModel";
            });
        std::ranges::find(body.nodes[1].parameters, "polyphony",
            &talas::project::ProjectParameterExpression::parameterId)->expression = std::to_string(polyphony);
        body.nodes[0].typeId = "talas.noise";
        body.nodes[0].parameters = {{"initialState", std::move(initialState)},
            {"frequency", "37 hz"}, {"scale", "1"}};
        if (nesting >= 2)
        {
            const auto authoredSeed = *body.nodes[0].parameters[0].expression;
            talas::project::ProjectSoundDefinition definition;
            definition.definitionId = "noiseSource";
            definition.displayName = "Noise Source";
            definition.graph.nodes.push_back(std::move(body.nodes[0]));
            definition.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
            body.nodes[0] = {};
            body.nodes[0].instanceId = "osc";
            body.nodes[0].soundDefinitionId = "noiseSource";
            if (nesting >= 3)
            {
                talas::project::ProjectSoundExposedParameter exposed;
                exposed.parameterId = "seed"; exposed.displayName = "Seed";
                exposed.domain = talas::expr::ValueDomain::Scalar;
                exposed.defaultValue = {"seed", nesting == 3 ? authoredSeed : "0"};
                exposed.bindings.push_back({talas::project::ProjectSoundParameterTarget::NodeParameter,
                    "osc", "initialState"});
                definition.exposedParameters.push_back(std::move(exposed));
                definition.graph.nodes[0].parameters[0].expression = "0";
                if (nesting == 4) body.nodes[0].parameters = {{"seed", authoredSeed}};
            }
            document.soundDefinitions.push_back(std::move(definition));
        }
        return document;
    };
    for (const int nesting : {0, 1, 2, 3, 4})
    {
        auto document = makeDocument(nesting, "?VoiceNumber * 0.1 + 0.001", 3,
            talas::expr::ExpressionLanguageVersion::VoiceV1);
        const auto canonical = talas::project::serializeProjectDocument(document);
        const auto reopened = talas::project::parseProjectDocument(canonical);
        require(reopened.ok() && talas::project::serializeProjectDocument(reopened.document) == canonical,
            "voice seed template must survive stable canonical persistence");
        const auto compiled = talas::runtime::compileProject(reopened.document, registry, {1000.0, 16});
        require(compiled.ok(), "static per-voice noise seeds must prepare: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        std::vector<std::vector<double>> expected(2, std::vector<double>(64));
        std::vector<double> firstVoice;
        for (const int ordinal : {1, 2, 3})
        {
            talas::project::ProjectDocument literal;
            literal.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV2;
            literal.graph.nodes = {
                {"noise", "talas.noise", {{"initialState", std::to_string(ordinal) + " * 0.1 + 0.001"},
                    {"frequency", "37 hz"}, {"scale", "1"}}},
                {"out", "talas.audioOutput", {}},
            };
            literal.graph.connections = {{"noise", "audioOut", "out", "audioIn"}};
            const auto literalGraph = talas::runtime::compileProject(literal, registry, {1000.0, 16});
            require(literalGraph.ok(), "independent literal noise instrument must compile");
            const auto rendered = talas::runtime::renderOffline(*literalGraph.graph, 64);
            require(rendered.ok() && rendered.mono.size() == 64,
                "independent literal noise generator must render");
            if (ordinal == 1) firstVoice = rendered.mono;
            else require(firstVoice != rendered.mono, "each later seed oracle must differ from the first");
            for (std::size_t frame = 0; frame < 64; ++frame)
            {
                expected[0][frame] += rendered.mono[frame];
                expected[1][frame] -= 0.5 * rendered.mono[frame];
            }
        }
        const std::array<std::size_t, 6> partition{3, 7, 11, 13, 14, 16};
        using Kind = talas::runtime::ConformanceMidiNoteKind;
        const std::array<talas::runtime::ConformanceMidiNote, 3> notes{{
            {0, Kind::NoteOn, 1, 60, 127}, {0, Kind::NoteOn, 1, 61, 127},
            {0, Kind::NoteOn, 1, 62, 127},
        }};
        const auto result = talas::runtime::runRuntimeConformance(*compiled.graph, 64, partition,
            {.midiNotes = notes, .referenceChannels = expected, .verifyResetReplay = true});
        require(result.ok() && result.resetReplayVerified,
            "per-voice static seeds must match the sum of literal instruments: " + result.message);
        const auto invalidSeed = makeDocument(nesting, "?VoiceNumber * 0.75", 2,
            talas::expr::ExpressionLanguageVersion::VoiceV1);
        const auto rejected = talas::runtime::compileProject(invalidSeed, registry, {1000.0, 16});
        require(!rejected.ok() && std::ranges::any_of(rejected.diagnostics, [](const auto& diagnostic) {
                return diagnostic.parameterId == "initialState";
            }), "a static seed valid only for ordinal one must reject every template placement");
    }
}

void testVoiceNumberRestartsForEachIndependentOwner()
{
    auto document = midiPitchDocument("gate", "0 nn", "127 nn", false);
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VoiceV1;
    document.graph.nodes[1].typeId = "talas.midiVoice";
    std::erase_if(document.graph.nodes[1].parameters,
        [](const auto& p) {
            return p.parameterId == "map"
                || p.parameterId == "voiceModel";
        });
    document.graph.nodes[0].parameters[0].expression = "?VoiceNumber * 7 hz";
    auto otherSource = document.graph.nodes[0];
    otherSource.instanceId = "otherSource";
    otherSource.parameters[0].expression = "?VoiceNumber * 11 hz";
    auto otherOwner = document.graph.nodes[1];
    otherOwner.instanceId = "otherOwner";
    document.graph.nodes.push_back(std::move(otherSource));
    document.graph.nodes.push_back(std::move(otherOwner));
    document.graph.connections.back() = {"otherOwner", "leftOut", "out", "rightIn"};
    document.graph.connections.push_back({"otherSource", "audioOut", "otherOwner", "audioIn"});
    document.expressionDefinitions.definitions.push_back(
        {"voiceRate", {{"base", talas::expr::ValueDomain::Scalar}}, talas::expr::ValueDomain::Hertz,
            "?VoiceNumber * base * @Multiplier * 1 hz"});
    document.graph.nodes[0].parameters[0].expression = "voiceRate(7)";
    document.graph.nodes[3].parameters[0].expression = "voiceRate(11)";
    using OwnerKind = talas::project::ProjectVariableOwnerKind;
    document.variableLibrary.variables = {
        {"Multiplier", "Multiplier", talas::expr::ValueDomain::Scalar, 99, {}},
        {"Multiplier", "Multiplier", talas::expr::ValueDomain::Scalar, 1, {OwnerKind::MidiVoice, {}, "mapper"}, false},
        {"Multiplier", "Multiplier", talas::expr::ValueDomain::Scalar, 1, {OwnerKind::MidiVoice, {}, "otherOwner"}, false},
    };
    const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {1000.0, 16});
    require(compiled.ok(), "independent VoiceNumber owners must compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto& plan = compiled.graph->plan();
    require(plan.midiVoices.size() == 2, "both independent MIDI owners must remain present");
    for (const auto& owner : plan.midiVoices)
    {
        for (std::size_t index = 0; index < owner.voices.size(); ++index)
        {
            const auto& step = plan.steps[owner.voices[index].firstStepIndex];
            const auto frequency = std::ranges::find(step.parameters, "frequency", &talas::runtime::ParameterValue::id);
            require(frequency != step.parameters.end()
                && frequency->value == (owner.ownerNodeId == "mapper" ? 7.0 : 11.0) * (index + 1),
                "voice numbers must restart at one for each owner independently of runtime scope IDs");
            require(frequency->variableCellIndices.size() == 1,
                "specialization must retain the ordinary variable binding");
            const auto& cell = compiled.graph->renderSettings().preparedVariables.cells[frequency->variableCellIndices.front()];
            require(cell.identity.ownerNodeId == owner.ownerNodeId
                && cell.identity.voiceOrdinal == std::optional<std::uint16_t>{static_cast<std::uint16_t>(index)},
                "definition variables must retain exact owner and voice shadowing after specialization");
        }
    }
}

}

int main()
{
    testFormatThirtySevenMapperMigrationPreservesLegacyInput();
    testPolyphonicMapperInputOptInClonesAndGatesVoices();
    testVoiceProfilePreservesOrdinaryNonMidiRendering();
    testVoiceNumberStaticNoiseSeedsMatchLiteralInstruments();
    testVoiceNumberRestartsForEachIndependentOwner();
    testVoiceNumberCompilesDistinctVoiceParameters();
    testMidiPitchBoundsRejectInvalidAndForgedInputs();
    testMidiVoiceFrequencyBounds();
    testStereoInputLanesFollowIndependentMidiMapperSignals();
    testDocumentedMapperVoiceGapIsVisible();
    testPublicMidiPitchBounds();
    testParserAcceptsOnlyTheClosedModes();
    testLocalMapIsScopedToOneNestedInputSound();
    testLocalMapResolvesDirectMidiControllerSource();
    testSelfNamedLocalMapFollowsVirtualEventValue();
    testRangedLocalMapScalesControllerIntoFrequency();
    testRangedLocalMapScalesAFaderIntoItsRange();
    testLocalMapCoversBothNestedStereoInputBranches();
    testMultipleLocalMapsAndGlobalFallback();
    testParserRejectsOversizedAndNonAsciiInput();
    testConfigurationBoundsAndAnyChannel();
    testGateAndVelocityReduction();
    testAnyChannelRetainsExactNoteOwnership();
    testRepeatedNotesStealingAndFiltering();
    testRollbackResetAndAuthorityExhaustion();
    testPreparedOperationsDoNotAllocate();
    testFixedSeedMidiEventPropertyReplay();
}
