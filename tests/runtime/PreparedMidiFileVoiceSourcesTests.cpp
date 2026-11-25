#include "../../src/runtime/PreparedMidiFileVoiceSources.h"
#include "talas/core/AnalogSequencerArray.h"

#include <array>
#include <algorithm>
#include <functional>
#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
using namespace talas::runtime;
void require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

struct Fixture
{
    RuntimePlan plan;
    RenderSettings settings;
    std::shared_ptr<talas::core::ResourceSnapshot> snapshot =
        std::make_shared<talas::core::ResourceSnapshot>();

    Fixture()
    {
        settings.sampleRate = 1000;
        settings.maxBlockFrames = 64;
        // PPQ 100, default tempo: notes at frames 0/50 on channels 1 and 2.
        // Keep a controller message and zero-velocity note-on in the stream.
        snapshot->id = "sequence";
        snapshot->kind = talas::core::ResourceSnapshotKind::Binary;
        snapshot->generation = 4;
        snapshot->contentHash = std::string(64, 'a');
        snapshot->binaryBytes = {'M','T','h','d',0,0,0,6,0,0,0,1,0,100,
            'M','T','r','k',0,0,0,24,
            0,0x90,60,100, 0,0x91,64,80, 0,0xb0,7,90,
            10,0x90,60,0, 0,0x81,64,30, 0,0xff,0x2f,0};
        settings.resourceSnapshots = {snapshot};
        settings.binaryResources = {{snapshot->id, snapshot->binaryBytes}};
        addOwner(1);
    }

    void addOwner(std::uint8_t channel)
    {
        CompileStep step;
        step.typeId = "talas.midiVoice";
        step.nodeId = "expanded." + std::to_string(plan.steps.size());
        ParameterValue source;
        source.id = "source";
        source.text = "MIDI File";
        ParameterValue resource;
        resource.id = "midiFile";
        resource.resource = "sequence";
        step.parameters = {source, resource};
        MidiVoicePlan owner;
        owner.ownerStepIndex = plan.steps.size();
        owner.ownerNodeId = "originalVoice";
        owner.ownerDefinitionId = "instrument";
        owner.ownerInstancePath = {"outer", "instrument" + std::to_string(plan.steps.size())};
        owner.configuration.channel = channel;
        plan.midiVoices.push_back(owner);
        plan.steps.push_back(step);
    }

    std::unique_ptr<const PreparedMidiFileVoiceSources> prepare(
        PreparedMidiFileVoiceSourceLimits limits = {}) const
    {
        std::string error;
        auto value = PreparedMidiFileVoiceSources::prepare(plan, settings, limits, error);
        require(value != nullptr, "preparation failed: " + error);
        require(error.empty(), "successful preparation diagnostic");
        return value;
    }

    void reject(std::string_view expected, PreparedMidiFileVoiceSourceLimits limits = {}) const
    {
        std::string error;
        const auto value = PreparedMidiFileVoiceSources::prepare(plan, settings, limits, error);
        require(value == nullptr, "invalid preparation was accepted");
        require(error.find(expected) != std::string::npos,
                "wrong diagnostic: " + error + " expected " + std::string(expected));
    }
};

void configureNestedControllerOwner(Fixture& fixture, std::size_t ownerIndex,
    RuntimeScopeId parentScope, RuntimeScopeId inputScope,
    std::vector<RuntimeScopeId> voiceScopes)
{
    auto& owner = fixture.plan.midiVoices.at(ownerIndex);
    owner.parentVoiceScopeId = parentScope;
    owner.voiceAncestors = {{"parentVoice" + std::to_string(parentScope),
        "parentSound" + std::to_string(parentScope), {"nestedInstrument"}, 1}};
    owner.voices.clear();
    for (const auto scope : voiceScopes)
        owner.voices.push_back(VoiceInstancePlan{.scopeId = scope});
    fixture.plan.steps.at(owner.ownerStepIndex).eventValueScopeId = inputScope;
}

void configureNestedFileControllerOwners(Fixture& fixture)
{
    fixture.addOwner(1);
    fixture.settings.preparedVariables.scopes = {
        {10, 0, "outerA", {"outerA"}},
        {11, 10, "outerASound", {"outerA", "sound"}},
        {12, 11, "childInputA", {"outerA", "sound", "input"}},
        {20, 0, "outerB", {"outerB"}},
        {21, 20, "outerBSound", {"outerB", "sound"}},
        {22, 21, "childInputB", {"outerB", "sound", "input"}},
    };
    configureNestedControllerOwner(fixture, 0, 10, 12, {101, 102});
    configureNestedControllerOwner(fixture, 1, 20, 22, {201, 202});
}

void setNestedFileControllerInitialValues(Fixture& fixture)
{
    fixture.settings.eventValues = {
        {"cc07", 0.125},
        {"cc07", 0.15, talas::expr::ValueDomain::Scalar, 10},
        {"cc07", 0.25, talas::expr::ValueDomain::Scalar, 11},
        {"cc07", 0.65, talas::expr::ValueDomain::Scalar, 20},
        {"cc07", 0.75, talas::expr::ValueDomain::Scalar, 21},
    };
}

void testNestedFileControllerBindingFanout()
{
    Fixture fixture;
    configureNestedFileControllerOwners(fixture);
    setNestedFileControllerInitialValues(fixture);
    fixture.settings.maxRealtimeEventTransactions = 4;

    const auto firstPreparation = fixture.prepare();
    require(firstPreparation->owners().size() == 2
        && firstPreparation->owners()[0].activationControlled
        && firstPreparation->owners()[1].activationControlled
        && firstPreparation->owners()[0].events.size() == 3
        && firstPreparation->owners()[1].events.size() == 3,
        "each nested File owner retains its own note stream and activation clock");

    std::string error;
    require(firstPreparation->appendScheduledControllerChanges(
        fixture.plan, fixture.settings, error), error);
    require(fixture.settings.eventValueChanges.empty(),
        "nested File controllers stay deferred instead of entering the global timeline");
    const auto findBinding = [&](RuntimeScopeId scope) {
        return std::ranges::find_if(fixture.settings.eventValues,
            [&](const auto& binding) {
                return binding.scopeId == scope && binding.name == "cc07";
            });
    };
    require(findBinding(0) != fixture.settings.eventValues.end()
        && findBinding(0)->value == 0.125,
        "nested controller fan-out leaves the global monitor value unchanged");
    for (const auto [scope, expected] : std::array<std::pair<RuntimeScopeId, double>, 4>{
             {{101, 0.25}, {102, 0.25}, {201, 0.75}, {202, 0.75}}})
    {
        const auto binding = findBinding(scope);
        require(binding != fixture.settings.eventValues.end()
            && binding->value == expected,
            "each child Input voice inherits the nearest visible outer value, not global state");
    }
    require(findBinding(101)->scopeId != findBinding(201)->scopeId,
        "separate parent activations receive distinct writable controller bindings");

    const auto boundPreparation = fixture.prepare();
    require(boundPreparation->owners()[0].activationControllers.size() == 2
        && boundPreparation->owners()[1].activationControllers.size() == 2,
        "the final preparation binds each controller to both child Input voice scopes");
    for (std::size_t ownerIndex = 0; ownerIndex < 2; ++ownerIndex)
    {
        const auto& owner = boundPreparation->owners()[ownerIndex];
        for (const auto& voice : fixture.plan.midiVoices[ownerIndex].voices)
        {
            const auto controller = std::ranges::find_if(owner.activationControllers,
                [&](const auto& candidate) {
                    return candidate.bindingIndex < fixture.settings.eventValues.size()
                        && fixture.settings.eventValues[candidate.bindingIndex].scopeId
                            == voice.scopeId;
                });
            require(controller != owner.activationControllers.end()
                && fixture.settings.eventValues[controller->bindingIndex].name == "cc07",
                "controller identity is rebound to its child Input voice scope");
        }
    }
}

void testNestedOptionalFileAndRequiredScriptBindings()
{
    Fixture optionalFile;
    configureNestedFileControllerOwners(optionalFile);
    optionalFile.settings.eventValues.clear();
    const auto optionalPrepared = optionalFile.prepare();
    require(optionalPrepared->owners()[0].activationControllers.empty()
        && optionalPrepared->owners()[1].activationControllers.empty(),
        "an unbound built-in File controller remains optional for nested Inputs");
    std::string error;
    require(optionalPrepared->appendScheduledControllerChanges(
        optionalFile.plan, optionalFile.settings, error), error);
    require(optionalFile.settings.eventValues.empty()
        && optionalFile.settings.eventValueChanges.empty(),
        "an optional unbound File controller creates neither local nor global bindings");

    Fixture unboundScript;
    configureNestedControllerOwner(unboundScript, 0, 10, 12, {101, 102});
    unboundScript.settings.preparedVariables.scopes = {
        {10, 0, "outer", {"outer"}},
        {11, 10, "sound", {"outer", "sound"}},
        {12, 11, "input", {"outer", "sound", "input"}},
    };
    unboundScript.plan.steps[0].parameters[0].text = "Script";
    unboundScript.plan.steps[0].parameters.push_back({.id = "script", .text =
        "self controller: !Frequency setTo: 200 atTime: 10 ms."});
    unboundScript.settings.eventValues.clear();
    unboundScript.reject("no Event Value binding");

    Fixture projectedScript;
    configureNestedControllerOwner(projectedScript, 0, 10, 12, {101, 102});
    projectedScript.settings.preparedVariables.scopes = {
        {10, 0, "outer", {"outer"}},
        {11, 10, "sound", {"outer", "sound"}},
        {12, 11, "input", {"outer", "sound", "input"}},
    };
    projectedScript.plan.steps[0].parameters[0].text = "Script";
    projectedScript.plan.steps[0].parameters.push_back({.id = "script", .text =
        "self controller: !Frequency setTo: 200 atTime: 10 ms."});
    const auto projection = talas::expr::parse("1",
        projectedScript.settings.expressionLanguage,
        projectedScript.settings.expressionDefinitions);
    require(projection.ok(), "projected Script target expression parses");
    EventValueBinding readOnly{"Frequency", 100,
        talas::expr::ValueDomain::Hertz, 11};
    readOnly.projectionExpression = *projection.expression;
    projectedScript.settings.eventValues = {std::move(readOnly)};
    projectedScript.reject("cannot assign a projected Event Value");
}

void testNestedControllerFanoutTransactionCapacity()
{
    Fixture fixture;
    configureNestedFileControllerOwners(fixture);
    setNestedFileControllerInitialValues(fixture);
    fixture.settings.maxRealtimeEventTransactions = 3;
    const auto prepared = fixture.prepare();
    const auto original = fixture.settings.eventValues;

    std::string error;
    require(!prepared->appendScheduledControllerChanges(
        fixture.plan, fixture.settings, error)
        && error.find("Activated MIDI Voice controllers exceed prepared transaction capacity")
            != std::string::npos,
        "transaction capacity counts two controllers fanned out across two child owners");
    require(fixture.settings.eventValueChanges.empty()
        && fixture.settings.eventValues.size() == original.size()
        && std::ranges::equal(fixture.settings.eventValues, original, [](const auto& a, const auto& b) {
            return a.name == b.name && a.scopeId == b.scopeId && a.value == b.value && a.domain == b.domain;
        }),
        "capacity rejection publishes neither controller bindings nor timeline changes");
    fixture.settings.maxRealtimeEventTransactions = 4;
    require(prepared->appendScheduledControllerChanges(fixture.plan, fixture.settings, error)
        && fixture.settings.eventValues.size() == original.size() + 4,
        "a corrected capacity retry creates exactly one binding per child Input slot");
}

void testSourceSelection()
{
    for (const auto text : {"", " \t\r\n", "Live MIDI", " LIVE midi "})
        require(parsePreparedMidiInputSource(text) == PreparedMidiInputSource::Live,
                "blank and historical live label");
    for (const auto text : {"MIDI File", " midi FILE\t"})
        require(parsePreparedMidiInputSource(text) == PreparedMidiInputSource::File,
                "historical MIDI File label");
    require(parsePreparedMidiInputSource("Script") == PreparedMidiInputSource::Script,
            "Script is a distinct source, never live or file");
    for (const auto text : {"file", "MIDIFile", "live midi extra", "MIDI F\xc3\xadle"})
        require(!parsePreparedMidiInputSource(text), "unknown source must not silently become live");
    require(parsePreparedMidiInputSource(std::string(100, ' ')) == PreparedMidiInputSource::Live,
            "historical arbitrary blank source stays live");
    require(!parsePreparedMidiInputSource(std::string(100, 'a')), "bounded nonblank source text");
    require(!parsePreparedMidiInputSource(std::string_view("MIDI File\0", 10)), "embedded NUL");
}

void testSelectedMessagesAndImmutableOwners()
{
    Fixture f;
    f.addOwner(2);
    auto prepared = f.prepare();
    require(prepared->hasFileSource() && prepared->eventCount() == 5, "all channel messages retained");
    const auto owners = prepared->owners();
    static_assert(std::is_const_v<std::remove_reference_t<decltype(owners[0])>>);
    require(owners.size() == 2 && owners[0].events.size() == 3 && owners[1].events.size() == 2,
            "each owner selects only its configured channel");
    require(owners[0].events[0].frame == 0 && owners[0].events[0].bytes == std::vector<std::uint8_t>{0x90,60,100}
        && owners[0].events[1].frame == 0 && owners[0].events[1].bytes == std::vector<std::uint8_t>{0xb0,7,90}
        && owners[0].events[2].frame == 50 && owners[0].events[2].bytes == std::vector<std::uint8_t>{0x90,60,0},
        "note, controller and zero-velocity bytes remain distinct and ordered");
    require(owners[1].events[0].bytes == std::vector<std::uint8_t>{0x91,64,80}
        && owners[1].events[1].frame == 50 && owners[1].events[1].bytes == std::vector<std::uint8_t>{0x81,64,30},
        "second owner note-off release velocity is preserved");
    f.snapshot->binaryBytes.clear();
    f.settings.binaryResources.clear();
    f.plan.midiVoices.clear();
    f.plan.steps.clear();
    require(owners[0].resourceId == "sequence" && owners[0].resourceGeneration == 4
        && owners[0].resourceContentHash == std::string(64, 'a')
        && owners[0].nodeId == "originalVoice" && owners[0].definitionId == "instrument"
        && owners[1].stepIndex == 1 && owners[1].instancePath.back() == "instrument1"
        && owners[0].events[2].frame == 50 && owners[1].events[1].bytes[1] == 64,
        "prepared events and original nested-owner identities must own their values");
}

void testHierarchicalSourceIdentity()
{
    Fixture fixture;
    auto& owner = fixture.plan.midiVoices.front();
    owner.parentVoiceScopeId = 10;
    owner.voiceAncestors = {{"outerVoice", "outerSound", {"instrument"}, 1}};
    const auto original = fixture.prepare();
    require(original->equivalent(*fixture.prepare()),
        "identical child source preparation must remain transferable");
    require(original->owners().front().parentVoiceScopeId == 10
        && original->owners().front().voiceAncestors == owner.voiceAncestors,
        "source preparation must retain the complete containing voice identity");

    owner.parentVoiceScopeId = 11;
    require(!original->equivalent(*fixture.prepare()),
        "identical notes under a different containing voice cannot transfer source state");
    owner.parentVoiceScopeId = 10;
    owner.voiceAncestors.front().voiceOrdinal = 0;
    require(!original->equivalent(*fixture.prepare()),
        "reused scope ordinals cannot disguise a different parent voice identity");
    owner.voiceAncestors.clear();
    owner.parentVoiceScopeId.reset();
    require(!original->equivalent(*fixture.prepare()),
        "a root source and a nested source cannot share a playback cursor");
    require(original->owners().front().parentVoiceScopeId == 10
        && original->owners().front().voiceAncestors.front().voiceOrdinal == 1,
        "prepared parent identity must own its values after the plan changes");
}

void testDefaultFileControllerChanges()
{
    Fixture f;
    f.settings.eventValues = {{"cc07", 0}};
    const auto prepared = f.prepare();
    std::string error;
    require(prepared->validateNotePlayback(f.plan, f.settings, error), error);
    require(prepared->appendScheduledControllerChanges(f.plan, f.settings, error), error);
    require(f.settings.eventValueChanges.size() == 1
        && f.settings.eventValueChanges[0].name == "cc07"
        && f.settings.eventValueChanges[0].frame == 0
        && std::abs(f.settings.eventValueChanges[0].value - 90.0 / 127.0) < 1e-15,
        "selected channel CC07 maps to the documented normalized built-in Event Value");
    f.settings.eventValueChanges.clear();
    f.settings.defaultMidiChannel = 2;
    require(prepared->appendScheduledControllerChanges(f.plan, f.settings, error)
        && f.settings.eventValueChanges.empty(),
        "built-in CC map uses the configured default MIDI channel");
    f.settings.defaultMidiChannel = 1;
    f.settings.eventValues.clear();
    require(prepared->appendScheduledControllerChanges(f.plan, f.settings, error)
        && f.settings.eventValueChanges.empty(),
        "unreferenced built-in controllers do not invent Event Value bindings");
    f.settings.eventValues = {{"cc07", 0, talas::expr::ValueDomain::Hertz}};
    require(!prepared->appendScheduledControllerChanges(f.plan, f.settings, error)
        && error.find("scalar") != std::string::npos,
        "built-in CC mapping rejects an incompatible non-scalar target");

    Fixture mapper;
    mapper.plan.steps[0].typeId = "talas.midiMapper";
    ParameterValue map;
    map.id = "map";
    map.text = "";
    ParameterValue model;
    model.id = "voiceModel";
    model.choice = "polyphonicInput";
    mapper.plan.steps[0].parameters.push_back(map);
    mapper.plan.steps[0].parameters.push_back(model);
    mapper.plan.midiVoices[0].voices = {
        VoiceInstancePlan{.scopeId = 101},
        VoiceInstancePlan{.scopeId = 102},
    };
    mapper.settings.eventValues = {
        {"cc07", 0},
        {"cc07", 0, talas::expr::ValueDomain::Scalar, 101},
        {"cc07", 0, talas::expr::ValueDomain::Scalar, 102},
    };
    const auto mapperPrepared = mapper.prepare();
    require(mapperPrepared->appendScheduledControllerChanges(
        mapper.plan, mapper.settings, error), error);
    require(mapper.settings.eventValueChanges.size() == 2
        && mapper.settings.eventValueChanges[0].scopeId == 101
        && mapper.settings.eventValueChanges[1].scopeId == 102,
        "Mapper file CC reaches every local Input voice without assigning the global binding");
}

void testInactiveResourceAndScriptArbitration()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "";
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources.clear();
    auto prepared = f.prepare();
    require(!prepared->hasFileSource() && prepared->eventCount() == 0
        && prepared->owners()[0].source == PreparedMidiInputSource::Live
        && prepared->owners()[0].resourceId.empty(), "inactive file must not resolve or activate");
    f.plan.steps[0].parameters[0].text = "Script";
    f.reject("Script");
    f.plan.steps[0].parameters[0].text = "unknown";
    f.reject("source");
}

void testLiteralScriptPreparation()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    ParameterValue script;
    script.id = "script";
    script.text = "self keyDownAt: 10 ms duration: 20 ms frequency: 445 hz velocity: 0.7 timbre: 0.375.";
    f.plan.steps[0].parameters.push_back(script);
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources.clear();
    const auto prepared = f.prepare();
    require(!prepared->hasFileSource() && prepared->hasScheduledSource()
        && prepared->eventCount() == 2, "script owns scheduled notes without loading an inactive file");
    const auto& events = prepared->owners()[0].scriptEvents;
    const auto pitch = 69.0 + 12.0 * std::log2(445.0 / 440.0);
    require(events.size() == 2 && events[0].frame == 10 && events[0].noteOn
        && events[0].note == 69 && std::abs(events[0].pitch - pitch) < 1e-12
        && events[0].velocity == 0.7 && events[0].timbre == 0.375
        && events[1].frame == 30 && !events[1].noteOn,
        "script retains fractional pitch, exact velocity and timbre, and note duration");
    require(prepared->equivalent(*f.prepare()), "same script has stable preparation identity");

    f.plan.steps[0].parameters.back().text =
        "MM := 120. self keyDownAt: 1 beats duration: 1 beat frequency: 69 nn velocity: 1.";
    const auto beats = f.prepare();
    require(beats->owners()[0].scriptEvents[0].frame == 500
        && beats->owners()[0].scriptEvents[1].frame == 1000,
        "explicit script tempo converts beat units to the composition frame clock");
    f.settings.tempoBpm = 120.0;
    f.plan.steps[0].parameters.back().text =
        "self keyDownAt: 1 beats duration: 0.25 beats frequency: 60 nn.";
    const auto defaultBeatTempo = f.prepare()->owners()[0].scriptEvents;
    require(defaultBeatTempo[0].frame == 1000 && defaultBeatTempo[1].frame == 1250,
        "an omitted MM uses the documented one-second beat rather than the render tempo");
    f.plan.steps[0].parameters.back().text =
        "MM := 60. self keyDownAt: 0.01 beats duration: 0.01 beats frequency: 60 nn. "
        "MM := 120. self keyDownAt: 0.06 beats duration: 0.02 beats frequency: 61 nn.";
    const auto changingTempo = f.prepare()->owners()[0].scriptEvents;
    require(changingTempo.size() == 4
        && changingTempo[0].frame == 10 && changingTempo[0].note == 60
        && changingTempo[1].frame == 20 && changingTempo[2].frame == 30
        && changingTempo[2].note == 61 && changingTempo[3].frame == 40,
        "each literal MM declaration applies to later beat-valued MIDI Script events");
    f.plan.steps[0].parameters.back().text =
        "self keyDownAt: 0 s. MM := 0. self keyDownAt: 1 beats.";
    f.reject("invalid MM tempo");
    f.plan.steps[0].parameters.back().text =
        "MM := 120. self keyDownAt: 0 beats duration: 10 beats frequency: 4 c. "
        "self keyDownAt: 5 beats duration: 0.25 beats frequency: 4 c + 0.5 nn velocity: 0.75.";
    const auto namedPitch = f.prepare()->owners()[0].scriptEvents;
    require(namedPitch.size() == 4
        && namedPitch[0].frame == 0 && namedPitch[0].note == 60
        && namedPitch[0].pitch == 60.0
        && namedPitch[1].frame == 2500 && namedPitch[1].note == 61
        && namedPitch[1].pitch == 60.5 && namedPitch[1].velocity == 0.75
        && namedPitch[2].frame == 2625 && namedPitch[3].frame == 5000,
        "documented octave-note spelling and fractional nn offset keep exact pitch and beat timing");
    f.plan.steps[0].parameters.back().text =
        "self keyDownAt: 0 s frequency: 4 a.";
    require(f.prepare()->owners()[0].scriptEvents[0].pitch == 69.0,
        "documented fourth-octave A matches 69 nn");
    for (const auto& [spelling, expectedPitch] :
         std::array<std::pair<std::string_view, double>, 5>{
             {{"4 c sharp", 61.0},
              {"4 c flat", 59.0},
              {"4 d flat", 61.0},
              {"4 b sharp", 72.0},
              {"4 c sharp + 0.5 nn", 61.5}}})
    {
        f.plan.steps[0].parameters.back().text =
            "self keyDownAt: 0 s frequency: "
            + std::string(spelling) + ".";
        const auto accidental = f.prepare()->owners()[0].scriptEvents;
        require(accidental.size() == 2
            && accidental[0].pitch == expectedPitch,
            "octave pitch accidental and fractional offset must retain exact nn");
    }
    f.plan.steps[0].parameters.back().text =
        "self keyDownAt: 0 s frequency: 4.5 c.";
    f.reject("octave note");
    // As a Smalltalk program, a pitch plus a frequency adds in hertz.
    f.plan.steps[0].parameters.back().text =
        "self keyDownAt: 0 s frequency: 4 c + 0.5 hz.";
    const auto mixed = f.prepare()->owners()[0].scriptEvents;
    require(mixed.size() == 2
        && std::abs(mixed[0].pitch - (69.0 + 12.0 * std::log2((440.0 * std::exp2(-9.0 / 12.0) + 0.5) / 440.0))) < 1e-9,
        "a pitch plus a frequency should add in hertz, as Smalltalk does");
    // The keywords after keyDownAt: in either order.
    f.plan.steps[0].parameters.back().text = "self keyDownAt: 1 s frequency: 60.2 nn duration: 0.25 s.";
    const auto reordered = f.prepare()->owners()[0].scriptEvents;
    require(reordered.size() == 2 && reordered[0].frame == 1000 && std::abs(reordered[0].pitch - 60.2) < 1e-12
            && reordered[1].frame == 1250,
        "frequency: may come before duration:");
    f.plan.steps[0].parameters.back().text = "self keyDownAt: 1 s duration: 0.25 s velocity: 0.8.";
    const auto unpitched = f.prepare()->owners()[0].scriptEvents;
    require(unpitched.size() == 2 && unpitched[0].velocity == 0.8 && unpitched[1].frame == 1250,
        "a note may leave out its frequency");
    // A note without a frequency plays the owner's LowPitch;
    // a project before format 101 (legacyNoteZero) keeps note 0.
    auto& parameters = f.plan.steps[0].parameters;
    parameters.insert(parameters.end() - 1, {.id = "lowPitch", .domain = talas::expr::ValueDomain::NoteNumber, .value = 48.0});
    const auto lowest = f.prepare()->owners()[0].scriptEvents;
    require(lowest.size() == 2 && lowest[0].note == 48 && lowest[0].pitch == 48.0,
        "a note without a frequency should play LowPitch");
    parameters.insert(parameters.end() - 1, {.id = "scriptNotePitch", .choice = "legacyNoteZero"});
    require(f.prepare()->owners()[0].scriptEvents[0].note == 0, "legacyNoteZero should keep note 0");
    parameters.erase(parameters.end() - 3, parameters.end() - 1);
    f.plan.steps[0].parameters.back().text = "self keyDownAt: 0 s.";
    const auto defaults = f.prepare();
    require(defaults->owners()[0].scriptEvents[0].note == 0
        && defaults->owners()[0].scriptEvents[0].velocity == 1.0
        && defaults->owners()[0].scriptEvents[1].frame == 10,
        "documented script selector defaults retain note zero, full velocity and 10 ms duration");
    f.plan.steps[0].parameters.back().text =
        "self keyDownAt: 0 s duration: 20 ms frequency: 445 hz velocity: 1.2.";
    f.reject("velocity");
    f.plan.steps[0].parameters.back().text =
        "self keyDownAt: 0 s timbre: 1.2.";
    f.reject("timbre");
    f.plan.steps[0].parameters.back().text =
        "self keyDownAt: 0 s timbre: 0.2 timbre: 0.8.";
    f.reject("duplicate selector");
    f.plan.steps[0].parameters.back().text =
        "self keyDownAt: 0 s duration: 20 ms frequency: 445 hz. 1 to: 4 do: []";
    f.reject("loop");
    // Two notes overlapping on one key each take a key of their own, the
    // second ending with its own note-off; each keeps its pitch.
    f.plan.steps[0].parameters.back().text =
        "self keyDownAt: 0 s duration: 30 ms frequency: 445 hz. "
        "self keyDownAt: 10 ms duration: 10 ms frequency: 446 hz.";
    const auto overlapping = f.prepare()->owners()[0].scriptEvents;
    require(overlapping.size() == 4 && overlapping[0].noteOn && overlapping[1].noteOn
            && overlapping[0].note != overlapping[1].note && overlapping[2].frame == 20 && !overlapping[2].noteOn
            && overlapping[2].note == overlapping[1].note && overlapping[3].frame == 30
            && overlapping[3].note == overlapping[0].note
            && std::abs(overlapping[1].pitch - (69.0 + 12.0 * std::log2(446.0 / 440.0))) < 1e-12,
        "overlapping notes on one key should each end with their own note-off and keep their pitch");
    f.plan.steps[0].parameters.back().text =
        "self keyDownAt: 0 s duration: 20 ms frequency: 445 hz. "
        "self keyDownAt: 0 s duration: 20 ms frequency: 660 hz.";
    f.reject("simultaneous event limit", {4, 1});
    f.reject("event limit", {3, 4});
}

// Notes, Rests,
// EventSequences, EventMixes and TimedEventCollections, transformed by
// dim:, trsp:, dbl:, inv:, freqScale:, retrograde and totalBeats:, and
// played on the voice with playOnVoice:bpm:, schedule the notes the
// literal keyDownAt: statements they stand for would.
void testEventCollectionsPlayTheirNotes()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    f.plan.steps[0].parameters.push_back({.id = "script", .text = ""});
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources.clear();
    const auto events = [&](std::string script) {
        f.plan.steps[0].parameters.back().text = std::move(script);
        return f.prepare()->owners()[0].scriptEvents;
    };
    const auto same = [&](const std::string& collection, const std::string& literal, const std::string& what) {
        const auto made = events(collection);
        const auto expected = events(literal);
        auto equal = made.size() == expected.size();
        for (std::size_t index = 0; equal && index < made.size(); ++index)
        {
            equal = made[index].frame == expected[index].frame && made[index].noteOn == expected[index].noteOn
                && made[index].note == expected[index].note
                && std::abs(made[index].pitch - expected[index].pitch) < 1e-9
                && std::abs(made[index].velocity - expected[index].velocity) < 1e-12;
        }
        require(equal, what);
    };
    // A measure at 6000 bpm, a beat in 10 ms: 2 g, 3 d, 2 d.
    same("(EventSequence events: #( {Note durationInBeats: 2 frequency: 2 g} {Note durationInBeats: 1 frequency: 3 d} "
         "{Note durationInBeats: 2 frequency: 2 d})) playOnVoice: self bpm: 6000.",
        "self keyDownAt: 0 ms duration: 20 ms frequency: 43 nn. self keyDownAt: 20 ms duration: 10 ms frequency: 50 nn. "
        "self keyDownAt: 30 ms duration: 20 ms frequency: 38 nn.",
        "an EventSequence should play its events one after another");
    same("| n | n := Note durationInBeats: 2 frequency: 4 c velocity: 0.5. (EventSequence events: (Array with: n "
         "with: ((n trsp: 7) dim: 0.5) with: (n trsp: -5))) playOnVoice: self bpm: 6000.",
        "self keyDownAt: 0 ms duration: 20 ms frequency: 60 nn velocity: 0.5. "
        "self keyDownAt: 20 ms duration: 10 ms frequency: 67 nn velocity: 0.5. "
        "self keyDownAt: 30 ms duration: 20 ms frequency: 55 nn velocity: 0.5.",
        "trsp: and dim: should transpose and scale the durations");
    same("(EventSequence events: (Array with: (Rest durationInBeats: 1) with: (Note durationInBeats: 1 frequency: 4 a))) "
         "playOnVoice: self onBeat: 2 bpm: 6000.",
        "self keyDownAt: 30 ms duration: 10 ms frequency: 69 nn.",
        "a Rest should take its time in a sequence, and onBeat: should start it later");
    same("(EventMix events: (Array with: (Note durationInBeats: 2 frequency: 3 d) "
         "with: (Note durationInBeats: 1 frequency: 3 d hz * 4/3))) playOnVoice: self bpm: 6000.",
        "self keyDownAt: 0 ms duration: 20 ms frequency: 50 nn. "
        "self keyDownAt: 0 ms duration: 10 ms frequency: " + [] {
            std::array<char, 64> buffer{};
            const auto [end, failed] = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                69.0 + 12.0 * std::log2(440.0 * std::exp2(-19.0 / 12.0) * 4.0 / 3.0 / 440.0));
            return std::string(buffer.data(), end);
        }() + " nn.",
        "an EventMix should start all its events at once, a frequency in hz too");
    // An EventMix is recursively defined, so it may hold an
    // EventMix and an EventSequence, all starting together.
    same("(EventMix events: (Array with: (EventMix events: (Array with: (Note durationInBeats: 1 frequency: 60 nn) "
         "with: (Note durationInBeats: 1 frequency: 64 nn))) with: (EventSequence events: (Array with: "
         "(Rest durationInBeats: 1) with: (Note durationInBeats: 1 frequency: 67 nn))))) playOnVoice: self bpm: 6000.",
        "self keyDownAt: 0 ms duration: 10 ms frequency: 60 nn. self keyDownAt: 0 ms duration: 10 ms frequency: 64 nn. "
        "self keyDownAt: 10 ms duration: 10 ms frequency: 67 nn.",
        "an EventMix of an EventMix and an EventSequence should start them together");
    same("(TimedEventCollection startingBeats: #(0 3) events: (Array with: (Note durationInBeats: 1 frequency: 60 nn) "
         "with: (Note durationInBeats: 2 frequency: 62 nn))) retrograde playOnVoice: self bpm: 6000.",
        "self keyDownAt: 0 ms duration: 20 ms frequency: 62 nn. self keyDownAt: 40 ms duration: 10 ms frequency: 60 nn.",
        "a TimedEventCollection should start each event at its beat, and retrograde mirror them");
    same("((EventSequence events: (Array with: (Note durationInBeats: 1 frequency: 60 nn) "
         "with: (Note durationInBeats: 1 frequency: 64 nn))) retrograde inv: 62 nn) playOnVoice: self bpm: 6000.",
        "self keyDownAt: 0 ms duration: 10 ms frequency: 60 nn. self keyDownAt: 10 ms duration: 10 ms frequency: 64 nn.",
        "retrograde should reverse a sequence and inv: invert its pitches");
    // Events collected within a loop.
    same("(EventSequence events: ((1 to: 3) collect: [ :i | Note durationInBeats: 2 frequency: 4 c + (i * 2) nn])) "
         "playOnVoice: self bpm: 6000.",
        "self keyDownAt: 0 ms duration: 20 ms frequency: 62 nn. self keyDownAt: 20 ms duration: 20 ms frequency: 64 nn. "
        "self keyDownAt: 40 ms duration: 20 ms frequency: 66 nn.",
        "an EventSequence should play the Notes a loop collects");
    same("((Note durationInBeats: 1 frequency: 60 nn) dbl: 12) playOnVoice: self bpm: 6000.",
        "self keyDownAt: 0 ms duration: 10 ms frequency: 60 nn. self keyDownAt: 0 ms duration: 10 ms frequency: 72 nn.",
        "dbl: should double a Note the interval away");
    same("(((EventSequence events: (Array with: (Note durationInBeats: 1 frequency: 60 nn) "
         "with: (Note durationInBeats: 3 frequency: 61 nn))) totalBeats: 2) freqScale: 2) playOnVoice: self bpm: 6000.",
        "self keyDownAt: 0 ms duration: 5 ms frequency: 72 nn. self keyDownAt: 5 ms duration: 15 ms frequency: 73 nn.",
        "totalBeats: should fit the durations to the beats and freqScale: scale the frequencies");
    // A growing algorithm, shortened: each pass sequences the last
    // phrase, its half-length fifth above and its fourth below.
    const auto grown = events("| thing | thing := EventSequence events: #( {Note durationInBeats: 2 frequency: 3 c} "
                              "{Note durationInBeats: 1 frequency: 3 g}). "
                              "2 timesRepeat: [thing := EventSequence events: (Array with: thing "
                              "with: ((thing dim: 0.5) trsp: 7) with: (thing trsp: -5))]. thing playOnVoice: self bpm: 6000.");
    require(grown.size() == 2 * 2 * 9 && grown.front().frame == 0 && grown.front().pitch == 48.0,
        "sequences of transformed sequences should play every note they hold");
    // RandomizeUsing:totalBeats:quantizeTo:
    // maxSpacing: picks an event for each start, the first on beat 0 and
    // each next a whole number of the shortest spacings later, up to the
    // longest, while within the total beats; for each start the event is
    // picked before the spacing is drawn.
    const std::string two = "(Array with: (Note durationInBeats: 0.5 frequency: 60 nn) "
                            "with: (Note frequency: 64 nn durationInBeats: 0.5))";
    same("((EventCollection events: " + two + ") randomizeUsing: (Random newForTalasWithSeed: 92) totalBeats: 4 "
         "quantizeTo: 0.5 maxSpacing: 1) playOnVoice: self bpm: 6000.",
        "| r t | r := Random newForTalasWithSeed: 92. t := 0. [t < 4] whileTrue: [self keyDownAt: (t * 10) ms "
        "duration: 5 ms frequency: (#(60 64) at: (r next * 2) floor + 1) nn. t := t + (0.5 * ((r next * 2) floor + 1))].",
        "randomizeUsing: should pick events at random spaced starts within the total beats");
    same("((EventSequence events: " + two + ") randomizeTimesUsing: (Random newForTalasWithSeed: 561) "
         "pickingEventsUsing: (OneOverF newForTalasWithSeed: 17661) totalBeats: 3 quantizeTo: 0.5 maxSpacing: 1.5) "
         "playOnVoice: self bpm: 6000.",
        "| times picks t | times := Random newForTalasWithSeed: 561. picks := OneOverF newForTalasWithSeed: 17661. "
        "t := 0. [t < 3] whileTrue: [self keyDownAt: (t * 10) ms duration: 5 ms frequency: "
        "(#(60 64) at: (picks next * 2) floor + 1) nn. t := t + (0.5 * ((times next * 3) floor + 1))].",
        "randomizeTimesUsing:pickingEventsUsing: should draw the starts and the events from two streams");
    // A randomized collection lasts its total beats in a sequence.
    const auto after = events("(EventSequence events: (Array with: ((EventCollection events: " + two
        + ") randomizeUsing: (Random new seed: 92) totalBeats: 4 quantizeTo: 0.5 maxSpacing: 1) "
          "with: (Note durationInBeats: 1 frequency: 72 nn))) playOnVoice: self bpm: 6000.");
    const auto expectedAfter = events("self keyDownAt: 40 ms duration: 10 ms frequency: 72 nn.");
    require(after.size() >= 2 && after[after.size() - 2].noteOn && after[after.size() - 2].pitch == 72.0
            && after[after.size() - 2].frame == expectedAfter.front().frame,
        "a randomized collection should last its totalBeats: in a sequence");
    // randomOrder: the events in an order drawn from Random new's stream.
    std::uint32_t state = 1u;
    const auto draw = [&] {
        state = static_cast<std::uint32_t>((static_cast<std::uint64_t>(state) * 16807u) % 2147483647u);
        return static_cast<double>(state - 1u) / 2147483646.0;
    };
    std::array<int, 3> order{60, 62, 64};
    for (std::size_t position = order.size(); position > 1; --position)
        std::swap(order[position - 1], order[std::min(position - 1, static_cast<std::size_t>(draw() * position))]);
    same("(EventSequence events: #( {Note durationInBeats: 1 frequency: 60 nn} {Note durationInBeats: 1 frequency: 62 nn} "
         "{Note durationInBeats: 1 frequency: 64 nn})) randomOrder playOnVoice: self bpm: 6000.",
        "self keyDownAt: 0 ms duration: 10 ms frequency: " + std::to_string(order[0]) + " nn. "
        "self keyDownAt: 10 ms duration: 10 ms frequency: " + std::to_string(order[1]) + " nn. "
        "self keyDownAt: 20 ms duration: 10 ms frequency: " + std::to_string(order[2]) + " nn.",
        "randomOrder should play a sequence's events in a random order");
    same("(TimedEventCollection startingBeats: #(0 3) events: (Array with: (Note durationInBeats: 1 frequency: 60 nn) "
         "with: (Note durationInBeats: 1 frequency: 62 nn))) randomOrder playOnVoice: self bpm: 6000.",
        [&] {
            std::uint32_t local = 1u;
            local = static_cast<std::uint32_t>((static_cast<std::uint64_t>(local) * 16807u) % 2147483647u);
            const auto swapped = static_cast<double>(local - 1u) / 2147483646.0 * 2.0 < 1.0;
            return swapped ? std::string{"self keyDownAt: 0 ms duration: 10 ms frequency: 62 nn. "
                                         "self keyDownAt: 30 ms duration: 10 ms frequency: 60 nn."}
                           : std::string{"self keyDownAt: 0 ms duration: 10 ms frequency: 60 nn. "
                                         "self keyDownAt: 30 ms duration: 10 ms frequency: 62 nn."};
        }(),
        "randomOrder should keep a timed collection's starting beats");
    f.plan.steps[0].parameters.back().text = "(Note durationInBeats: 1) randomOrder playOnVoice: self.";
    f.reject("EventSequence, EventMix");
    f.plan.steps[0].parameters.back().text = "(EventCollection events: " + two + ") playOnVoice: self.";
    f.reject("no times");
    f.plan.steps[0].parameters.back().text = "((EventCollection events: " + two
        + ") randomizeUsing: 3 totalBeats: 4 quantizeTo: 0.5 maxSpacing: 1) playOnVoice: self.";
    f.reject("random stream");
    f.plan.steps[0].parameters.back().text = "((EventCollection events: " + two
        + ") randomizeUsing: Random new totalBeats: 4 quantizeTo: 1 maxSpacing: 0.5) playOnVoice: self.";
    f.reject("maxSpacing:");
}

void testDocumentedSmalltalkNoteLoop()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    f.plan.steps[0].parameters.push_back({.id = "script", .text =
        "MM := 120. 1 to: 12 do: [ :i | "
        "self keyDownAt: (i - 1) beats duration: 0.25 beats "
        "frequency: 4 c + i nn velocity: (i / 12.0)]."});
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources.clear();
    const auto prepared = f.prepare();
    const auto& events = prepared->owners()[0].scriptEvents;
    require(events.size() == 24 && prepared->eventCount() == 24,
        "documented loop emits twelve bounded note-on/off pairs");
    for (std::size_t i = 0; i < 12; ++i)
    {
        const auto& on = events[2 * i];
        const auto& off = events[2 * i + 1];
        require(on.noteOn && on.frame == 500 * i
            && on.note == 61 + i && on.pitch == 61.0 + i
            && std::abs(on.velocity - static_cast<double>(i + 1) / 12.0) < 1e-12
            && !off.noteOn && off.frame == on.frame + 125
            && off.note == on.note,
            "loop index computes each note's time, duration, pitch and velocity");
    }
    require(prepared->equivalent(*f.prepare()),
        "loop expansion has a stable prepared identity");
    f.plan.steps[0].parameters.back().text =
        "1 to: 12 do: [ :i | self keyDownAt: i beats velocity: (i / 0)].";
    f.reject("loop");
    // As a Smalltalk program a loop may run past the literal subset's 1024
    // iterations; the prepared event limit still bounds what it schedules.
    f.plan.steps[0].parameters.back().text =
        "1 to: 2048 do: [ :i | self keyDownAt: i beats].";
    require(f.prepare()->eventCount() == 4096, "a 2048-note Smalltalk loop should schedule its 4096 events");
    f.reject("event limit", {4000, 256});
}

// The modified Markov chains script trains a chain of
// length 2 on offset and duration pairs and plays ten sequences of it, one
// after another, sequence i with seed i * 26, its durations divided by i,
// i half steps above 1 g and at velocity i / 10, at MM 160: each sequence
// faster and higher than the one before, every two notes in a row a pair
// the training sequence has.
void testMarkovChainScriptPlaysItsSequences()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    f.plan.steps[0].parameters.push_back({.id = "script", .text =
        "| t chain dur pitchOffset seq |\n"
        "MM := 160.\n"
        "chain := MarkovChain trainingSequence: #(\n"
        "    #(0 4) #(3 2) #(-5 2) #(2 4) #(3 2) #(-5 2) #(2 4)\n"
        "    #(3 2) #(-5 4) #(2 2) #(3 2) #(-5 4) #(2 2) #(-5 2)\n"
        "    #(5 2) #(-7 4) #(1 2) #(-2 2) #(1 4) #(0 0.5) #(12 4)\n"
        "    ) chainLength: 2.\n"
        "t := 0.\n"
        "1 to: 10 do: [ :i |\n"
        "    seq := chain newSequenceOfLength: 100 seed: i * 26.\n"
        "    seq do: [ :offDur |\n"
        "        dur := (offDur at: 2) / i.\n"
        "        pitchOffset := offDur at: 1.\n"
        "        self keyDownAt: t beats duration: dur beats frequency: 1 g + pitchOffset nn + i nn velocity: i/10.\n"
        "        t := t + dur]]."});
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources.clear();
    const std::vector<std::pair<double, double>> training{{0, 4}, {3, 2}, {-5, 2}, {2, 4}, {3, 2}, {-5, 2},
        {2, 4}, {3, 2}, {-5, 4}, {2, 2}, {3, 2}, {-5, 4}, {2, 2}, {-5, 2}, {5, 2}, {-7, 4}, {1, 2}, {-2, 2},
        {1, 4}, {0, 0.5}, {12, 4}};
    const auto prepared = f.prepare();
    std::vector<PreparedMidiScriptNoteEvent> notes;
    for (const auto& event : prepared->owners()[0].scriptEvents)
    {
        if (event.noteOn)
            notes.push_back(event);
    }
    // A beat at MM 160 is 375 frames at the fixture's 1 kHz.
    const auto framesPerBeat = 375.0;
    std::vector<std::vector<std::size_t>> sequences(10);
    for (std::size_t index = 0; index + 1 < notes.size(); ++index)
    {
        const auto& note = notes[index];
        const auto i = static_cast<int>(std::lround(note.velocity * 10.0));
        require(i >= 1 && i <= 10 && std::abs(note.velocity - i / 10.0) < 1e-12,
            "each note's velocity should be its sequence's i / 10");
        require(sequences[static_cast<std::size_t>(i - 1)].empty() || i == static_cast<int>(std::lround(notes[index - 1].velocity * 10.0)),
            "the sequences should play one after another");
        // The pair the note plays: its offset above 1 g + i nn, and its
        // duration, to the next note's start, times i.
        const auto offset = note.pitch - (31.0 + i);
        const auto beats = static_cast<double>(notes[index + 1].frame - note.frame) / framesPerBeat * i;
        const auto pair = std::ranges::find_if(training, [&](const auto& candidate) {
            return std::abs(candidate.first - offset) < 1e-9 && std::abs(candidate.second - beats) < 1.5 * i / framesPerBeat;
        });
        require(pair != training.end(), "each note should play a pair of the training sequence, "
            "its duration divided by i and i half steps above 1 g");
        sequences[static_cast<std::size_t>(i - 1)].push_back(static_cast<std::size_t>(pair - training.begin()));
    }
    for (std::size_t i = 0; i < 10; ++i)
    {
        const auto& sequence = sequences[i];
        require(!sequence.empty() || i == 9, "each of the ten sequences should play");
        for (std::size_t index = 0; index + 1 < sequence.size(); ++index)
        {
            const auto& first = training[sequence[index]];
            const auto& second = training[sequence[index + 1]];
            auto occurs = false;
            for (std::size_t at = 0; at + 1 < training.size(); ++at)
                occurs = occurs || (training[at] == first && training[at + 1] == second);
            require(occurs, "every two notes in a row should be a pair the training sequence has");
        }
    }
    std::set<std::vector<std::pair<double, double>>> distinct;
    for (const auto& sequence : sequences)
    {
        std::vector<std::pair<double, double>> pairs;
        for (const auto index : sequence)
            pairs.push_back(training[index]);
        distinct.insert(pairs);
    }
    require(distinct.size() >= 8, "each seed should generate a new sequence");
}

// The self similar melody script's block calls itself
// four times deep, each call making an EventSequence of its event three
// times, transposed by 0, 7 and -12 half steps and its duration scaled by
// 2/5, 1/5 and 2/5, so that middle C for 16 beats becomes 81 notes in the
// same 16 beats at MM 45.
void testRecursiveBlockPlaysASelfSimilarMelody()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    f.plan.steps[0].parameters.push_back({.id = "script", .text =
        "| pitchShape durationShape shapingFunction selfSimMel |\n"
        "pitchShape := #(0 7 -12).\n"
        "durationShape := #({2/5} {1/5} {2/5}).\n"
        "shapingFunction := [ :seq :count |\n"
        "    count = 0\n"
        "        ifTrue: [seq]\n"
        "        ifFalse: [\n"
        "            EventSequence events:\n"
        "                ((1 to: pitchShape size) collect: [ :i |\n"
        "                    shapingFunction\n"
        "                        value: ((seq trsp: (pitchShape at: i)) dim: (durationShape at: i))\n"
        "                        value: count - 1])]].\n"
        "selfSimMel := shapingFunction value: (Note frequency: 4 c durationInBeats: 16) value: 4.\n"
        "selfSimMel playOnVoice: self onBeat: 0 bpm: 45."});
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources.clear();
    const auto prepared = f.prepare();
    std::vector<PreparedMidiScriptNoteEvent> notes;
    for (const auto& event : prepared->owners()[0].scriptEvents)
    {
        if (event.noteOn)
            notes.push_back(event);
    }
    require(notes.size() == 81, "four levels of three calls should play 81 notes");
    // A beat at MM 45 is 4000 / 3 frames at the fixture's 1 kHz.
    const std::array<double, 3> pitchShape{0.0, 7.0, -12.0};
    const std::array<double, 3> durationShape{0.4, 0.2, 0.4};
    auto beat = 0.0;
    for (std::size_t index = 0; index < notes.size(); ++index)
    {
        auto pitch = 60.0;
        auto beats = 16.0;
        for (std::size_t level = 0, rest = index; level < 4; ++level, rest /= 3)
        {
            pitch += pitchShape[rest % 3];
            beats *= durationShape[rest % 3];
        }
        require(std::abs(notes[index].pitch - pitch) < 1e-9
                && std::abs(static_cast<double>(notes[index].frame) - beat * 4000.0 / 3.0) <= 1.0,
            "note " + std::to_string(index) + " should be the event transposed and scaled at each level");
        beat += beats;
    }
    require(std::abs(beat - 16.0) < 1e-9, "the 81 notes should last the 16 beats of the first note");
    // Iteration gets the same result.
    const auto recursed = prepared->owners()[0].scriptEvents;
    f.plan.steps[0].parameters.back().text =
        "| pitchShape durationShape c1 |\n"
        "pitchShape := #(0 7 -12).\n"
        "durationShape := #({2/5} {1/5} {2/5}).\n"
        "c1 := Note frequency: 4 c durationInBeats: 16.\n"
        "4 timesRepeat: [ c1 := EventSequence events:\n"
        "    ((1 to: 3) collect: [ :i | (c1 trsp: (pitchShape at: i)) dim: (durationShape at: i)])].\n"
        "c1 playOnVoice: self onBeat: 0 bpm: 45.";
    require(f.prepare()->owners()[0].scriptEvents == recursed,
        "iteration should play the same melody as the recursion");
}

// A three-note sequence made six times over into itself,
// then itself half as long and a fifth up, then a fourth down, plays
// 3 * 3^6 notes, the first three the sequence as written, the whole
// lasting 2.5^6 times its five beats.
void testSequencesMadeOfThemselves()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    f.plan.steps[0].parameters.push_back({.id = "script", .text =
        "| thing |\n"
        "thing := EventSequence events: #( {Note durationInBeats: 2 frequency: 3 c} {Note durationInBeats: 1 frequency: 3 g}\n"
        "    {Note durationInBeats: 2 frequency: 2 g}).\n"
        "6 timesRepeat: [ thing := EventSequence events: (Array with: thing with: ((thing dim: 0.5) trsp: 7)\n"
        "    with: (thing trsp: -5))].\n"
        "thing playOnVoice: self bpm: 160."});
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources.clear();
    const auto prepared = f.prepare();
    std::vector<PreparedMidiScriptNoteEvent> notes;
    std::size_t last = 0;
    for (const auto& event : prepared->owners()[0].scriptEvents)
    {
        if (event.noteOn)
            notes.push_back(event);
        last = std::max(last, event.frame);
    }
    require(notes.size() == 2187 && notes[0].pitch == 48.0 && notes[1].pitch == 55.0 && notes[2].pitch == 43.0
            && notes[1].frame == 750 && notes[2].frame == 1125,
        "the sequence should hold 3 * 3^6 notes, the first three as written");
    // A beat at MM 160 is 375 frames at the fixture's 1 kHz.
    const auto beats = 5.0 * std::pow(2.5, 6.0);
    require(std::abs(static_cast<double>(last) - beats * 375.0) <= 2.0,
        "each pass should make the sequence 2.5 times as long");
    // Pat1 lasts eight beats, 8000 frames at MM 60.
    f.plan.steps[0].parameters.back().text =
        "| pat1 | pat1 := EventSequence events: #( {Note durationInBeats: 4 frequency: 4 g}\n"
        "    {Note durationInBeats: (8/5) frequency: 4 b flat} {Note durationInBeats: (12/5) frequency: 5 c}).\n"
        "pat1 playOnVoice: self bpm: 60.";
    std::size_t end = 0;
    std::vector<std::size_t> starts;
    const auto pat1 = f.prepare();
    for (const auto& event : pat1->owners()[0].scriptEvents)
    {
        end = std::max(end, event.frame);
        if (event.noteOn)
            starts.push_back(event.frame);
    }
    require(starts == std::vector<std::size_t>{0, 4000, 5600} && end == 8000,
        "pat1 should last 8 beats, its notes 4, 8/5 and 12/5 beats long");
}

// A format 0 MIDI file at 100 ticks a quarter note holding the given
// channel messages, {tick, status, key, velocity}, in time order.
std::vector<std::uint8_t> midiFileOf(const std::vector<std::array<unsigned, 4>>& messages)
{
    std::vector<std::uint8_t> track;
    unsigned tick = 0;
    const auto quantity = [&](unsigned value) {
        std::vector<std::uint8_t> bytes{static_cast<std::uint8_t>(value & 0x7fU)};
        while ((value >>= 7U) != 0)
            bytes.insert(bytes.begin(), static_cast<std::uint8_t>(0x80U | (value & 0x7fU)));
        track.insert(track.end(), bytes.begin(), bytes.end());
    };
    for (const auto& [at, status, key, velocity] : messages)
    {
        quantity(at - tick);
        tick = at;
        track.insert(track.end(), {static_cast<std::uint8_t>(status), static_cast<std::uint8_t>(key),
                                      static_cast<std::uint8_t>(velocity)});
    }
    quantity(0);
    track.insert(track.end(), {0xff, 0x2f, 0});
    std::vector<std::uint8_t> bytes{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 100, 'M', 'T', 'r', 'k'};
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<std::uint8_t>(track.size() >> static_cast<unsigned>(shift)));
    bytes.insert(bytes.end(), track.begin(), track.end());
    return bytes;
}

// Serial time points, a TimedEventCollection of a
// twelve-tone set's Notes at the beats its intervals sum to, played at a
// quarter of its velocities and again from beat 11, inverted about 3 a at
// three quarters; and TimedEventCollections read from a MIDI file's
// channel, inverted, transposed and played backwards.
void testTimedEventCollectionsFromSetsAndMidiFiles()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    f.plan.steps[0].parameters.push_back({.id = "script", .text = ""});
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources = {{"Files/omino.mid",
        midiFileOf({{0, 0x92, 60, 100}, {0, 0x90, 40, 64}, {100, 0x82, 60, 0}, {100, 0x92, 64, 100}, {100, 0x92, 67, 100},
            {200, 0x82, 64, 0}, {200, 0x82, 67, 0}, {200, 0x92, 62, 100}, {300, 0x92, 65, 100}, {350, 0x82, 62, 0},
            {400, 0x82, 65, 0}, {500, 0x92, 60, 100}, {600, 0x82, 60, 0}, {600, 0x80, 40, 0}})}};
    const auto onsets = [&](std::string script) {
        f.plan.steps[0].parameters.back().text = std::move(script);
        const auto prepared = f.prepare();
        std::vector<std::tuple<std::size_t, double, double, std::size_t>> notes;
        const auto& events = prepared->owners()[0].scriptEvents;
        for (std::size_t index = 0; index < events.size(); ++index)
        {
            if (!events[index].noteOn)
                continue;
            const auto off = std::find_if(events.begin() + static_cast<std::ptrdiff_t>(index) + 1, events.end(),
                [&](const auto& event) { return !event.noteOn && event.note == events[index].note; });
            notes.emplace_back(events[index].frame, events[index].pitch, events[index].velocity,
                off == events.end() ? 0 : off->frame - events[index].frame);
        }
        std::ranges::sort(notes);
        return notes;
    };
    using Notes = std::vector<std::tuple<std::size_t, double, double, std::size_t>>;
    const auto velocity = 100.0 / 127.0;
    // A beat at MM 60 is 1000 frames at the fixture's 1 kHz.
    require(onsets("(TimedEventCollection timesAndEventsFromMIDIFile: 'omino.mid' channel: 3) playOnVoice: self bpm: 60.")
            == Notes{{0, 60.0, velocity, 1000}, {1000, 64.0, velocity, 1000}, {1000, 67.0, velocity, 1000},
                {2000, 62.0, velocity, 1500}, {3000, 65.0, velocity, 1000}, {5000, 60.0, velocity, 1000}},
        "a TimedEventCollection read from a MIDI file should hold one channel's notes at their beats");
    require(onsets("((TimedEventCollection timesAndEventsFromMIDIFile: 'omino.mid' channel: 3) asMonophonicEventSequence) "
                   "playOnVoice: self bpm: 60.")
            == Notes{{0, 60.0, velocity, 1000}, {1000, 67.0, velocity, 1000}, {2000, 62.0, velocity, 1000},
                {3000, 65.0, velocity, 1000}, {5000, 60.0, velocity, 1000}},
        "asMonophonicEventSequence should play one note at a time");
    // Transform events from MIDI file, at MM 120 (500 frames a beat).
    Notes transformed;
    for (const auto& [start, pitch, beats] : std::vector<std::tuple<double, double, double>>{
             {0, 60, 1}, {1, 64, 1}, {1, 67, 1}, {2, 62, 1.5}, {3, 65, 1}, {5, 60, 1}})
    {
        transformed.emplace_back(static_cast<std::size_t>((5.0 + start) * 500.0), 120.0 - pitch, velocity,
            static_cast<std::size_t>(beats * 500.0));
        transformed.emplace_back(static_cast<std::size_t>((6.0 - start - beats) * 500.0), pitch + 31.0, velocity,
            static_cast<std::size_t>(beats * 500.0));
    }
    std::ranges::sort(transformed);
    require(onsets("| pat | pat := TimedEventCollection timesAndEventsFromMIDIFile: 'omino.mid' channel: 3. "
                   "(pat inv: 60 nn) playOnVoice: self onBeat: 5 bpm: 120. "
                   "(pat trsp: 31) retrograde playOnVoice: self bpm: 120.")
            == transformed,
        "a MIDI file's TimedEventCollection should invert, transpose and play backwards");
    f.plan.steps[0].parameters.back().text =
        "(TimedEventCollection timesAndEventsFromMIDIFile: 'missing.mid' channel: 3) playOnVoice: self bpm: 60.";
    f.reject("no MIDI file named missing.mid");

    // Serial time points, at MM 400 (150 frames a beat).
    const std::array<int, 12> set{0, 1, 11, 5, 2, 4, 3, 9, 10, 6, 8, 7};
    Notes serial;
    auto beat = 0;
    for (const auto interval : set)
    {
        beat += interval;
        if (interval == 0)
            continue;
        serial.emplace_back(static_cast<std::size_t>(beat * 150), 57.0 + interval, 0.25, static_cast<std::size_t>(interval * 150));
        serial.emplace_back(static_cast<std::size_t>((11 + beat) * 150), 57.0 - interval, 0.75,
            static_cast<std::size_t>(interval * 150));
    }
    std::ranges::sort(serial);
    require(onsets("| set evs beats prime t |\n"
                   "set := #(0 1 11 5 2 4 3 9 10 6 8 7).\n"
                   "evs := set collect: [ :int | Note durationInBeats: int frequency: 3 a + int nn].\n"
                   "t := 0.\n"
                   "beats := set collect: [ :int | t := t + int].\n"
                   "prime := TimedEventCollection startingBeats: beats events: evs.\n"
                   "(prime velScale: 0.25) playOnVoice: self onBeat: 0 bpm: 400.\n"
                   "((prime inv: 3 a) velScale: 0.75) playOnVoice: self onBeat: 11 bpm: 400.")
            == serial,
        "serial time points should play the set and its inversion, velocities scaled");
}

// A MarkovChain trained on the events of a MIDI file's
// channel made monophonic, its Notes alike as = finds them, makes a new
// sequence that an EventSequence plays at MM 90: here the file's melody
// has one way on from each pair of events, so the sequence is the
// training sequence from a place drawn at random to its end, where the
// chain backs into a corner.
void testMarkovChainTrainsOnAMidiFile()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    f.plan.steps[0].parameters.push_back({.id = "script", .text =
        "| pat chain seq |\n"
        "pat := TimedEventCollection timesAndEventsFromMIDIFile: 'DESAFIN1.MID' channel: 3.\n"
        "pat := pat asMonophonicEventSequence.\n"
        "chain := MarkovChain trainingSequence: pat events chainLength: 3.\n"
        "seq := chain newSequenceOfLength: 200 seed: 3.\n"
        "(EventSequence events: seq) playOnVoice: self onBeat: 0 bpm: 90."});
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources = {{"Files/DESAFIN1.MID",
        midiFileOf({{0, 0x92, 60, 100}, {100, 0x82, 60, 0}, {100, 0x92, 64, 100}, {100, 0x92, 67, 100}, {200, 0x82, 64, 0},
            {200, 0x82, 67, 0}, {200, 0x92, 62, 100}, {300, 0x92, 65, 100}, {350, 0x82, 62, 0}, {400, 0x82, 65, 0},
            {500, 0x92, 60, 100}, {600, 0x82, 60, 0}})}};
    const auto prepared = f.prepare();
    std::vector<double> pitches;
    std::vector<std::size_t> frames;
    for (const auto& event : prepared->owners()[0].scriptEvents)
    {
        if (event.noteOn)
        {
            pitches.push_back(event.pitch);
            frames.push_back(event.frame);
        }
    }
    // The monophonic training sequence: 60, 67, 62, 65, a Rest, 60, a beat
    // each (667 frames at MM 90 and 1 kHz).
    const std::vector<double> melody{60, 67, 62, 65, 60};
    const std::vector<double> beats{0, 1, 2, 3, 5};
    require(!pitches.empty() && pitches.size() <= melody.size(),
        "the MarkovChain's sequence should play");
    const auto from = melody.size() - pitches.size();
    require(std::equal(pitches.begin(), pitches.end(), melody.begin() + static_cast<std::ptrdiff_t>(from)),
        "the sequence should follow the training's transitions to the corner it backs into");
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        const auto expected = (beats[from + index] - beats[from]) * 60000.0 / 90.0;
        require(frames.front() == 0 && std::abs(static_cast<double>(frames[index]) - expected) <= 1.0,
            "the EventSequence should play the Notes and Rests a beat each at MM 90");
    }
}

// A hundred notes of random pitch, duration, velocity and
// placement in time from a Random, and two hundred from OneOverF streams,
// their pitches rounded to whole note numbers.
void testRandomNoteScriptsPlayTheirNotes()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    f.plan.steps[0].parameters.push_back({.id = "script", .text =
        "| r t |\n"
        "r := Random newForTalasWithSeed: 52.\n"
        "t := 0.\n"
        "100 timesRepeat: [\n"
        "    self keyDownAt: t s duration: (r next + 1) s frequency: 3 c + (r next * 36 nn) velocity: r next.\n"
        "    t := t + r next]."});
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources.clear();
    const auto check = [&](std::size_t count, bool whole, const std::string& what) {
        const auto prepared = f.prepare();
        std::vector<PreparedMidiScriptNoteEvent> notes;
        for (const auto& event : prepared->owners()[0].scriptEvents)
        {
            if (event.noteOn)
                notes.push_back(event);
        }
        require(notes.size() == count, what + ": every note should play, not " + std::to_string(notes.size()));
        for (const auto& note : notes)
        {
            require(note.pitch >= 48.0 && note.pitch <= 84.0 && note.velocity >= 0.0 && note.velocity <= 1.0
                    && (!whole || note.pitch == std::round(note.pitch)),
                what + ": each pitch should lie between 3 c and 6 c and each velocity between 0 and 1");
        }
    };
    f.plan.steps[0].parameters.back().text =
        "| rPch rDur rVel rStart t |\n"
        "rPch := OneOverF newForTalasWithSeed: 52 states: 128.\n"
        "rDur := OneOverF newForTalasWithSeed: 52 states: 128.\n"
        "rVel := OneOverF newForTalasWithSeed: 52 states: 128.\n"
        "rStart := OneOverF newForTalasWithSeed: 52 states: 128.\n"
        "t := 0.\n"
        "200 timesRepeat: [\n"
        "    self keyDownAt: t s duration: rDur next s frequency: 3 c + ((rPch next * 36) roundTo: 1) nn velocity: rVel next.\n"
        "    t := t + (rStart next * 0.5)].";
    check(200, true, "the OneOverF notes");
    f.plan.steps[0].parameters.back().text =
        "| r t |\n"
        "r := Random newForTalasWithSeed: 52.\n"
        "t := 0.\n"
        "100 timesRepeat: [\n"
        "    self keyDownAt: t s duration: (r next + 1) s frequency: 3 c + (r next * 36 nn) velocity: r next.\n"
        "    t := t + r next].";
    check(100, false, "the Random notes");
}

// 100 uniform + ctrl sets !Morph to 0 at 0 s,
// then plays 100 random notes, each at least a second long, between 3 c
// and 6 c, and with each slides !Morph to a random value in 10 steps by
// the next note's start.
void testRandomNotesSlideAController()
{
    Fixture f;
    f.settings.sampleRate = 48000;
    f.plan.steps[0].parameters[0].text = "Script";
    f.plan.steps[0].parameters.push_back({.id = "script", .text =
        "| r t |\n"
        "r := Random newForTalasWithSeed: 2.\n"
        "t := 0.\n"
        "self controller: !Morph setTo: 0 atTime: 0 s.\n"
        "100 timesRepeat: [\n"
        "    self keyDownAt: t s duration: (r next + 1) s frequency: 3 c + (r next * 36 nn) velocity: r next.\n"
        "    t := t + r next.\n"
        "    self controller: !Morph slideTo: r next steps: 10 byTime: t s]."});
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources.clear();
    const auto prepared = f.prepare();
    const auto& owner = prepared->owners()[0];
    std::vector<PreparedMidiScriptNoteEvent> starts;
    for (const auto& event : owner.scriptEvents)
    {
        if (event.noteOn)
            starts.push_back(event);
    }
    require(starts.size() == 100, "the script should play 100 notes");
    for (const auto& note : starts)
    {
        const auto ends = std::ranges::find_if(owner.scriptEvents, [&](const PreparedMidiScriptNoteEvent& event) {
            return !event.noteOn && event.note == note.note && event.frame >= note.frame + 48000;
        });
        require(note.pitch >= 48.0 && note.pitch <= 84.0 && note.velocity >= 0.0 && note.velocity <= 1.0
                && ends != owner.scriptEvents.end(),
            "each note should last at least a second, between 3 c and 6 c");
    }
    const auto& morph = owner.scriptControllers;
    require(morph.size() == 1 + 100 * 10 && morph.front() == PreparedMidiScriptControllerEvent{0, "Morph", 0.0},
        "!Morph should start at 0 and slide in 10 steps with each note");
    for (std::size_t slide = 0; slide < 100; ++slide)
    {
        const auto& last = morph[10 * (slide + 1)];
        require(last.name == "Morph" && last.value >= 0.0 && last.value < 1.0
                && last.frame == (slide + 1 < starts.size() ? starts[slide + 1].frame : last.frame),
            "each slide should reach its random value by the next note's start");
    }
}

// A MIDI Script locates a text file by its name,
// streams over it and plays it. Text to Pitch takes each character's ASCII
// number as a pitch, a beat after a capital or other character below 97 and
// a quarter beat after a lower case one; the CSound reader takes each i line
// of its instrument, and the . lines after it, as a note from its start,
// duration, octave point pitch class and amplitude.
void testScriptsReadScoresFromTextFiles()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    f.settings.resourceSnapshots.clear();
    const auto bytes = [](std::string_view text) { return std::vector<std::uint8_t>(text.begin(), text.end()); };
    f.settings.binaryResources = {{"Texts/words.txt", bytes("Hi ab")},
        {"Texts/b1.txt", bytes("f 1 0 4096 10 1\ni 1 0 2 8.09 2500\n. 2 1 9.00 1250\ni 2 0 4 7.00 2500\n"
                                "i 1 3 1 8.00 500\ne\ni 1 9 1 8.00 2500\n")}};
    const auto locate = [](std::string_view name) {
        return "fileName := '" + std::string{name} + "'.\n"
            "file := Preferences locateFileNamed: fileName ofType: HostDriverInterface textFileType\n"
            "    locateMessage: ' locate the text file named '.\n"
            "file == nil ifTrue: [^ self abortForTalas].\n"
            "f := file asFilename textFileStream.\n";
    };
    const auto notes = [&](std::string script) {
        f.plan.steps[0].parameters.resize(1);
        f.plan.steps[0].parameters.push_back({.id = "script", .text = std::move(script)});
        const auto prepared = f.prepare();
        std::vector<PreparedMidiScriptNoteEvent> starts;
        for (const auto& event : prepared->owners()[0].scriptEvents)
        {
            if (event.noteOn)
                starts.push_back(event);
        }
        return starts;
    };
    // A beat at MM 60 is 1000 frames at the fixture's 1 kHz.
    const auto pitches = notes("| fileName file f nbr t |\n" + locate("words.txt")
        + "t := 0.\n"
          "[f atEnd] whileFalse: [ nbr := f next asInteger.\n"
          "(nbr < 97) ifTrue: [t := t + 1] ifFalse: [t := t + 0.25].\n"
          "self keyDownAt: t beats duration: 1 beats frequency: (nbr max: 20) nn velocity: 1]. f close.");
    const std::array<std::pair<std::size_t, double>, 5> expected{{{1000, 72.0}, {1250, 105.0}, {2250, 32.0},
        {2500, 97.0}, {2750, 98.0}}};
    require(pitches.size() == expected.size(), "Text to Pitch should play each character");
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        require(pitches[index].frame == expected[index].first && pitches[index].pitch == expected[index].second,
            "each character's ASCII number should be its pitch, after a beat or a quarter beat");
    }
    const auto instrument = notes("| fileName file f paramArray eCard inOurInstr insno |\n" + locate("b1.txt")
        + "insno := 1. inOurInstr := false. eCard := false.\n"
          "[f atEnd or: [eCard]] whileFalse: [\n"
          "    paramArray := f nextParameters.\n"
          "    paramArray isEmpty ifFalse: [\n"
          "        ((paramArray at: 1) == #e) ifTrue: [eCard := true].\n"
          "        ((paramArray at: 1) == #i) ifTrue: [\n"
          "            ((paramArray at: 2) == insno) ifFalse: [inOurInstr := false] ifTrue: [\n"
          "                inOurInstr := true.\n"
          "                self keyDownAt: (paramArray at: 3) beats duration: (paramArray at: 4) beats\n"
          "                    frequency: (paramArray at: 5) octavePointPitchClass velocity: (paramArray at: 6) / 2500.0]].\n"
          "        (((paramArray at: 1) == #'.') and: [inOurInstr]) ifTrue: [\n"
          "            self keyDownAt: (paramArray at: 2) beats duration: (paramArray at: 3) beats\n"
          "                frequency: (paramArray at: 4) octavePointPitchClass velocity: (paramArray at: 5) / 2500.0]]].\n"
          "f close.");
    const std::array<std::tuple<std::size_t, double, double>, 3> played{{{0, 69.0, 1.0}, {2000, 72.0, 0.5},
        {3000, 60.0, 0.2}}};
    require(instrument.size() == played.size(),
        "the reader should play instrument 1's lines and the dot line after one, up to the e card");
    for (std::size_t index = 0; index < played.size(); ++index)
    {
        const auto& [frame, pitch, velocity] = played[index];
        require(instrument[index].frame == frame && std::abs(instrument[index].pitch - pitch) < 1e-9
                && std::abs(instrument[index].velocity - velocity) < 1e-9,
            "each note should take its start, octave point pitch class and amplitude from its line");
    }
    f.plan.steps[0].parameters.resize(1);
    f.plan.steps[0].parameters.push_back({.id = "script", .text = "| fileName file f |\n" + locate("gone.txt")});
    f.reject("abortForTalas");
}

// A Note's tags may be left off for their
// defaults; trsp: leaves a Rest as it is and transposes each event of a
// sequence; and the chord example plays a chord, the chord a fourth up and
// an eighth as long (dim:), then two fourths up, while !Frequency starts
// at 0 and drops to
// -36 in 60 steps from 3 s to 4 s, a controller's values not limited to
// (0,1).
void testDocumentedEventCollections()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    f.settings.resourceSnapshots.clear();
    f.settings.binaryResources.clear();
    const auto prepared = [&](std::string script) {
        f.plan.steps[0].parameters.resize(1);
        f.plan.steps[0].parameters.push_back({.id = "script", .text = std::move(script)});
        return f.prepare();
    };
    const auto starts = [](const auto& owner) {
        std::vector<PreparedMidiScriptNoteEvent> notes;
        for (const auto& event : owner.scriptEvents)
        {
            if (event.noteOn)
                notes.push_back(event);
        }
        return notes;
    };
    // A beat at MM 60 is 1000 frames at the fixture's 1 kHz.
    const auto defaults = starts(prepared("(EventSequence events: (Array with: (Note frequency: 4 a) "
                                          "with: (Note durationInBeats: 2) with: (Note velocity: 0.5))) playOnVoice: self bpm: 60.")
                                     ->owners()[0]);
    require(defaults.size() == 3 && defaults[0].frame == 0 && defaults[0].pitch == 69.0 && defaults[1].frame == 1000
            && defaults[1].pitch == 60.0 && defaults[2].frame == 3000 && defaults[2].velocity == 0.5,
        "a Note's left-off tags should take a beat, middle c and a velocity of 1");
    const auto rested = starts(prepared("(EventSequence events: (Array with: ((Rest durationInBeats: 1) trsp: 5) "
                                        "with: (Note durationInBeats: 1 frequency: 4 c))) playOnVoice: self bpm: 60.")
                                   ->owners()[0]);
    const auto transposed = starts(prepared("((EventSequence events: (Array with: (Note durationInBeats: 1 frequency: 4 c) "
                                            "with: (Note durationInBeats: 1 frequency: 4 e))) trsp: 2) playOnVoice: self bpm: 60.")
                                       ->owners()[0]);
    require(rested.size() == 1 && rested[0].frame == 1000 && rested[0].pitch == 60.0 && transposed.size() == 2
            && transposed[0].pitch == 62.0 && transposed[1].pitch == 66.0,
        "trsp: should leave a Rest as it is and transpose each event of a sequence");
    const auto chords = prepared(
        "| chord seq | chord := EventMix events: (Array with: (Note durationInBeats: 2 frequency: 3 d)\n"
        "    with: (Note durationInBeats: 2 frequency: 3 d hz * 4/3) with: (Note durationInBeats: 2 frequency: 3 d hz * (4/3 ** 2))\n"
        "    with: (Note durationInBeats: 2 frequency: 3 d hz * (4/3 ** 3)) ).\n"
        "seq := EventSequence events: (Array with: chord with: ((chord freqScale: 4/3) dim: 0.125) with: (chord freqScale: 16/9)).\n"
        "seq playOnVoice: self bpm: 60.\n"
        "self controller: !Frequency setTo: 0 atTime: 0 s. self controller: !Frequency setTo: 0 atTime: 3 s.\n"
        "self controller: !Frequency slideTo: -36 steps: 60 byTime: 4 s.");
    const auto& owner = chords->owners()[0];
    const auto notes = starts(owner);
    const auto fourth = 12.0 * std::log2(4.0 / 3.0);
    require(notes.size() == 12, "the sequence should play three chords of four notes");
    for (std::size_t chord = 0; chord < 3; ++chord)
    {
        for (std::size_t voice = 0; voice < 4; ++voice)
        {
            const auto& note = notes[4 * chord + voice];
            require(note.frame == std::array<std::size_t, 3>{0, 2000, 2250}[chord]
                    && std::abs(note.pitch - (50.0 + fourth * static_cast<double>(voice + chord))) < 1e-6,
                "each chord should lie a fourth above the one before, the second an eighth as long");
        }
    }
    const auto& frequency = owner.scriptControllers;
    require(frequency.size() == 62 && frequency.front() == PreparedMidiScriptControllerEvent{0, "Frequency", 0.0}
            && frequency.back().frame == 4000 && frequency.back().value == -36.0
            && std::abs(frequency[2].value + 0.6) < 1e-9,
        "!Frequency should start at 0 and drop to -36 in 60 steps from 3 s to 4 s");
}

void testLiteralScriptControllerSet()
{
    Fixture f;
    f.plan.steps[0].parameters[0].text = "Script";
    f.plan.steps[0].parameters.push_back({.id = "script", .text =
        "self keyDownAt: 0 s duration: 30 ms frequency: 60 nn. "
        "self controller: !Frequency setTo: 200 atTime: 10 ms."});
    f.settings.eventValues = {{"Frequency", 100, talas::expr::ValueDomain::Hertz}};
    const auto prepared = f.prepare();
    require(prepared->eventCount() == 3
        && prepared->owners()[0].scriptControllers.size() == 1
        && prepared->owners()[0].scriptControllers[0]
            == PreparedMidiScriptControllerEvent{10, "Frequency", 200},
        "literal controller is prepared with its exact sample frame and name");
    std::string error;
    require(prepared->appendScheduledControllerChanges(f.plan, f.settings, error), error);
    require(f.settings.eventValueChanges.size() == 1
        && f.settings.eventValueChanges[0].frame == 10
        && f.settings.eventValueChanges[0].name == "Frequency"
        && f.settings.eventValueChanges[0].value == 200
        && f.settings.eventValueChanges[0].domain == talas::expr::ValueDomain::Hertz,
        "controller becomes a scoped prepared Event Value change");
    require(prepared->equivalent(*f.prepare()),
        "controller is part of the immutable prepared source identity");
    require(!prepared->appendScheduledControllerChanges(f.plan, f.settings, error)
        && error.find("conflicts") != std::string::npos,
        "same-frame controller collisions reject instead of choosing an implicit winner");
    f.settings.eventValueChanges.clear();
    f.settings.eventValues.clear();
    require(!prepared->appendScheduledControllerChanges(f.plan, f.settings, error)
        && error.find("no Event Value binding") != std::string::npos
        && f.settings.eventValueChanges.empty(),
        "unbound controllers reject without publishing partial changes");

    f.plan.steps[0].parameters.back().text =
        "self controller: !Frequency setTo: 200 atTime: 10 ms.";
    require(f.prepare()->owners()[0].scriptEvents.empty(),
        "controller-only scripts do not require an artificial note");
    f.plan.steps[0].parameters.back().text =
        "self controller: !Frequency setTo: 100 atTime: 0 ms. "
        "self controller: !Frequency slideTo: 200 steps: 5 byTime: 10 ms.";
    const auto slide = f.prepare();
    const auto& stepped = slide->owners()[0].scriptControllers;
    require(stepped.size() == 6 && slide->eventCount() == 6,
        "documented controller slide expands before the audio callback");
    for (std::size_t index = 0; index < stepped.size(); ++index)
        require(stepped[index].frame == 2 * index
            && stepped[index].name == "Frequency"
            && stepped[index].value == 100.0 + 20.0 * index,
            "five uniform steps reach the target at the declared frame");
    f.settings.eventValues = {{"Frequency", 100, talas::expr::ValueDomain::Hertz}};
    require(slide->appendScheduledControllerChanges(f.plan, f.settings, error)
        && f.settings.eventValueChanges.size() == 6,
        "prepared slide steps use the same bound Event Value timeline");
    f.settings.eventValueChanges.clear();
    f.plan.steps[0].parameters.back().text =
        "self controller: !Frequency setTo: 100 atTime: 0 ms. "
        "self controller: !Frequency slideTo: 155 stepSize: 20 byTime: 6 ms.";
    const auto sized = f.prepare();
    const auto& sizedEvents = sized->owners()[0].scriptControllers;
    require(sizedEvents.size() == 4 && sized->eventCount() == 4,
        "stepSize form expands a shorter final step to reach the target");
    for (std::size_t index = 0; index < sizedEvents.size(); ++index)
        require(sizedEvents[index].frame == 2 * index
            && sizedEvents[index].value
                == std::array{100.0, 120.0, 140.0, 155.0}[index],
            "stepSize retains requested increments and the exact target");
    f.plan.steps[0].parameters.back().text =
        "self controller: !Frequency setTo: 100 atTime: 0 ms. "
        "self controller: !Frequency slideTo: 45 stepSize: 20 byTime: 6 ms.";
    const auto descending = f.prepare()->owners()[0].scriptControllers;
    require(descending.size() == 4 && descending[1].value == 80.0
        && descending[2].value == 60.0 && descending[3].value == 45.0,
        "stepSize follows the sign of the target difference");
    // !Volume slides to 1 by 6 s in 10 steps.
    f.plan.steps[0].parameters.back().text =
        "self controller: !Volume setTo: 0 atTime: 0 s. self controller: !Volume slideTo: 1 byTime: 6 s.";
    const auto tenSteps = f.prepare()->owners()[0].scriptControllers;
    require(tenSteps.size() == 11 && tenSteps.back().frame == 6000 && tenSteps.back().value == 1.0
            && tenSteps[1].frame == 600 && std::abs(tenSteps[1].value - 0.1) < 1e-12,
        "slideTo:byTime: without steps should slide in 10 steps");
    // From 0.5 up to 1 by the time 5 seconds has been reached.
    f.plan.steps[0].parameters.back().text =
        "self controller: !cc07 setTo: 0.5 atTime: 0 s. self controller: !cc07 slideTo: 1.0 byTime: 5 s.";
    const auto gradual = f.prepare()->owners()[0].scriptControllers;
    require(gradual.size() == 11 && gradual.front().value == 0.5 && gradual.back().frame == 5000
            && gradual.back().value == 1.0 && gradual[1].frame == 500 && std::abs(gradual[1].value - 0.55) < 1e-12,
        "!cc07 slideTo: 1.0 byTime: 5 s should rise gradually from 0.5 to 1 by 5 s");
    f.plan.steps[0].parameters.back().text =
        "self controller: !Frequency slideTo: 200 steps: 10 byTime: 10 ms.";
    f.reject("prior earlier value");
    f.plan.steps[0].parameters.back().text =
        "self controller: !Frequency setTo: 100 atTime: 0 ms. "
        "self controller: !Frequency slideTo: 200 steps: 0 byTime: 10 ms.";
    f.reject("integer steps");
    f.plan.steps[0].parameters.back().text =
        "self controller: !Frequency setTo: 100 atTime: 0 ms. "
        "self controller: !Frequency slideTo: 200 steps: 11 byTime: 10 ms.";
    f.reject("exceeds prepared steps");
    f.plan.steps[0].parameters.back().text =
        "self controller: !Frequency setTo: 100 atTime: 0 ms. "
        "self controller: !Frequency slideTo: 200 stepSize: 0 byTime: 10 ms.";
    f.reject("positive stepSize");
    f.plan.steps[0].parameters.back().text =
        "self controller: !1bad setTo: 200 atTime: 10 ms.";
    f.reject("controller");
}

void testExactResourceIdentity()
{
    for (int mutation = 0; mutation < 12; ++mutation)
    {
        Fixture f;
        switch (mutation)
        {
        case 0: f.settings.resourceSnapshots.clear(); break;
        case 1: f.settings.resourceSnapshots.push_back(f.snapshot); break;
        case 2: f.settings.binaryResources.clear(); break;
        case 3: f.settings.binaryResources.push_back(f.settings.binaryResources[0]); break;
        case 4: f.settings.binaryResources[0].bytes[0] = 0; break;
        case 5: f.snapshot->kind = talas::core::ResourceSnapshotKind::Audio; break;
        case 6: f.snapshot->generation = 0; break;
        case 7: f.snapshot->contentHash.clear(); break;
        case 8: f.snapshot->contentHash[0] = 'A'; break;
        case 9: f.snapshot->tableSamples = {1}; break;
        case 10: f.snapshot->audioBytes = {1}; break;
        case 11: f.snapshot->partialTrackBytes = {1}; break;
        }
        f.reject("immutable binary");
    }
    Fixture compatibility;
    compatibility.snapshot->generation = 0;
    compatibility.snapshot->contentHash.clear();
    require(compatibility.prepare()->eventCount() == 3, "generation-zero compatibility adapter");
}

void testAggregateBudgets()
{
    Fixture f;
    f.addOwner(2);
    f.reject("event limit", {4, 4});
    f.reject("simultaneous", {5, 2});
    require(f.prepare({5, 3})->eventCount() == 5, "exact aggregate limits");
    f.plan.midiVoices[0].configuration.channel = 3;
    f.plan.midiVoices[1].configuration.channel = 4;
    require(f.prepare({0, 0})->eventCount() == 0, "filtered streams require no event capacity");
}

void testMalformedFileAndIndependentPatternPolicy()
{
    Fixture f;
    f.snapshot->binaryBytes[9] = 2;
    f.settings.binaryResources[0].bytes = f.snapshot->binaryBytes;
    f.reject("independent patterns");
    f.snapshot->binaryBytes = {0};
    f.settings.binaryResources[0].bytes = f.snapshot->binaryBytes;
    f.reject("MThd");
}

void testOwnerAndBindingValidation()
{
    for (int mutation = 0; mutation < 7; ++mutation)
    {
        Fixture f;
        switch (mutation)
        {
        case 0: f.plan.midiVoices[0].ownerStepIndex = 3; break;
        case 1: f.plan.midiVoices.push_back(f.plan.midiVoices[0]); break;
        case 2: f.plan.steps[0].typeId = "talas.oscillator"; break;
        case 3: f.plan.steps[0].parameters[0].text.reset(); break;
        case 4: f.plan.steps[0].parameters.push_back(f.plan.steps[0].parameters[0]); break;
        case 5: f.plan.midiVoices[0].configuration.channel = 0; break;
        case 6: f.plan.midiVoices[0].configuration.channel = 17; break;
        }
        f.reject("owner");
    }
    Fixture missing;
    missing.plan.steps[0].parameters[1].resource = "";
    missing.reject("midiFile");
}

void testAnalogSequencerKeyScheduleRetainsEventVectors()
{
    const std::array durations{1.0, 1.0, 1.0, 1.0, 1.0};
    const std::array duties{1.0, 1.0, 1.0, 1.0, 1.0};
    const std::array downs{1.0, 1.0, 1.0, 0.0, 0.0};
    const std::array pitches{60.0, 62.5, 65.0, 67.0, 69.0};
    const std::array velocities{0.2, 0.0, 0.8, 0.9, 0.9};
    const std::array timbres{0.1, 0.3, 0.7, 0.8, 0.9};
    std::string error;
    const auto schedule = prepareAnalogSequencerKeySchedule(
        durations, duties, downs, pitches, velocities, timbres,
        0, 4, 10.0, 1.0, 10, error);
    require(schedule.has_value() && error.empty()
            && schedule->cycleFrames == 50
            && schedule->events.size() == 6,
        "three active and two inactive stages should prepare three exact key pairs");
    const auto& events = schedule->events;
    require(schedule->stages.size() == 5
            && schedule->stages[0].durationFrames == 10
            && schedule->stages[0].gateFrames == 10
            && schedule->stages[1].attack->pitch == 62.5
            && schedule->stages[3].durationFrames == 10
            && !schedule->stages[3].attack,
        "Step preparation retains every stage's mask, including silent stages");
    require(events[0].frame == 0 && events[0].noteOn
            && events[0].pitch == 60.0
            && events[0].velocity == 0.2
            && events[0].timbre == 0.1
            && events[1].frame == 10 && !events[1].noteOn
            && events[2].frame == 10 && events[2].noteOn
            && events[2].pitch == 62.5
            && events[2].velocity == 0.0
            && events[2].timbre == 0.3
            && events[3].frame == 20 && !events[3].noteOn
            && events[4].frame == 20 && events[4].noteOn
            && events[4].pitch == 65.0
            && events[4].velocity == 0.8
            && events[4].timbre == 0.7
            && events[5].frame == 30 && !events[5].noteOn,
        "key vectors must preserve pitch, velocity and timbre with releases before new attacks");
    require(events[2].allocationVelocity == 1,
        "KeyDown one with zero velocity must remain a key event");

    const auto faster = prepareAnalogSequencerKeySchedule(
        durations, duties, downs, pitches, velocities, timbres,
        0, 4, 10.0, 2.0, 10, error);
    const auto slower = prepareAnalogSequencerKeySchedule(
        durations, duties, downs, pitches, velocities, timbres,
        0, 4, 10.0, 0.5, 10, error);
    require(faster && slower && faster->cycleFrames == 25
            && faster->events[1].frame == 5
            && slower->cycleFrames == 100
            && slower->events[1].frame == 20,
        "static positive Rate scales key-event stage duration");
    const auto reverse = prepareAnalogSequencerKeySchedule(
        durations, duties, downs, pitches, velocities, timbres,
        0, 4, 10.0, -1.0, 10, error);
    require(reverse && reverse->cycleFrames == 50
            && reverse->events.size() == 6
            && reverse->events[0].frame == 20
            && reverse->events[0].noteOn
            && reverse->events[0].pitch == 65.0
            && reverse->events[2].frame == 30
            && reverse->events[2].noteOn
            && reverse->events[2].pitch == 62.5
            && reverse->events[4].frame == 40
            && reverse->events[4].noteOn
            && reverse->events[4].pitch == 60.0,
        "negative Rate should reverse key stages and preserve their vectors");
    require(!prepareAnalogSequencerKeySchedule(
                durations, duties, downs, pitches, velocities,
                timbres, 0, 4, 10.0, 0.0, 10, error),
        "zero Rate cannot prepare a key-event schedule");

    auto invalidDowns = downs;
    invalidDowns[0] = 0.5;
    require(!prepareAnalogSequencerKeySchedule(
                durations, duties, invalidDowns, pitches, velocities,
                timbres, 0, 4, 10.0, 1.0, 10, error)
            && error.find("invalid") != std::string::npos,
        "fractional KeyDown must not silently become a gate");
    require(!prepareAnalogSequencerKeySchedule(
                durations, duties, downs, pitches, velocities,
                timbres, 0, 4, 10.0, 1.0, 5, error)
            && error.find("event limit") != std::string::npos,
        "prepared key-event capacity must reject incomplete cycles");
}

void testAnalogSequencerKeyShortArraysRepeatFinalValues()
{
    constexpr std::array durations{0.2, 0.2};
    constexpr std::array duties{0.5};
    constexpr std::array downs{1.0, 1.0, 1.0};
    constexpr std::array pitches{60.0, 62.0};
    constexpr std::array velocities{0.25};
    constexpr std::array timbres{0.75};
    std::string error;
    const auto schedule = prepareAnalogSequencerKeySchedule(
        durations, duties, downs, pitches, velocities, timbres,
        0, 2, 10.0, 1.0, 6, error);
    require(schedule.has_value() && error.empty()
            && schedule->cycleFrames == 6
            && schedule->events.size() == 6,
        "short key arrays should extend through the longest stage sequence");
    for (std::size_t stage = 0; stage < 3; ++stage)
    {
        const auto& on = schedule->events[stage * 2];
        const auto& off = schedule->events[stage * 2 + 1];
        require(on.noteOn && on.frame == stage * 2
                && on.pitch == (stage == 0 ? 60.0 : 62.0)
                && on.velocity == 0.25 && on.timbre == 0.75
                && !off.noteOn && off.frame == stage * 2 + 1,
            "each short key vector should hold its final authored value");
    }
}

void testAnalogSequencerAuthoredArraysPrepareAsVoiceSource()
{
    Fixture f;
    auto& step = f.plan.steps[0];
    step.typeId = "talas.analogSequencer";
    step.parameters.clear();
    const auto textValue = [&](std::string id, std::string value) {
        ParameterValue parameter;
        parameter.id = std::move(id);
        parameter.text = std::move(value);
        step.parameters.push_back(std::move(parameter));
    };
    textValue("durations", "{10 ms} {10 ms} {10 ms} {10 ms} {10 ms}");
    textValue("dutyCycles", "#(1 1 1 1 1)");
    textValue("keyDowns", "#(1 1 1 0 0)");
    textValue("keyPitches", "#(60 62.5 65 67 69)");
    textValue("keyVelocities", "#(0.2 0 0.8 0.9 0.9)");
    textValue("keyTimbres", "#(0.1 0.3 0.7 0.8 0.9)");
    ParameterValue start;
    start.id = "startIndex";
    start.value = 0.0;
    step.parameters.push_back(start);
    ParameterValue end;
    end.id = "endIndex";
    end.value = 4.0;
    step.parameters.push_back(end);
    ParameterValue origin;
    origin.id = "indexOrigin";
    origin.choice = "zeroBased";
    step.parameters.push_back(origin);
    ParameterValue loop;
    loop.id = "loop";
    loop.boolean = false;
    step.parameters.push_back(loop);
    for (const auto& [id, value] :
         std::array<std::pair<std::string_view, double>, 3>{{
             {"gate", 1.0}, {"step", 1.0}, {"rate", 1.0}}})
    {
        ParameterValue scalar;
        scalar.id = id;
        scalar.value = value;
        step.parameters.push_back(scalar);
    }
    textValue("pitches", "");
    textValue("velocities", "");
    textValue("extraValues", "");
    f.settings.binaryResources.clear();
    f.settings.resourceSnapshots.clear();

    require(hasPreparedAnalogSequencerKeyOwner(step)
            && hasPreparedMidiScheduledVoiceSource(f.plan),
        "authored KeyDowns select a scheduled voice owner");
    const auto prepared = f.prepare();
    require(prepared->hasScheduledSource() && !prepared->hasFileSource()
            && prepared->eventCount() == 6
            && prepared->owners()[0].source == PreparedMidiInputSource::Script,
        "one authored cycle is a prepared scheduled voice source");
    const auto& events = prepared->owners()[0].scriptEvents;
    require(events[0].frame == 0 && events[0].noteOn
            && events[0].pitch == 60.0 && events[0].velocity == 0.2
            && events[0].timbre == 0.1
            && events[1].frame == 10 && !events[1].noteOn
            && events[2].frame == 10 && events[2].noteOn
            && events[2].pitch == 62.5 && events[2].velocity == 0.0
            && events[2].timbre == 0.3,
        "authored arrays preserve exact key vectors and same-frame ordering");
    require(prepared->equivalent(*f.prepare()),
        "same authored arrays retain exact prepared source identity");
    InputBinding connectedStep;
    connectedStep.inputPortId = "step";
    step.inputs.push_back(connectedStep);
    for (const auto fallback : {0.0, 1.0})
    {
        std::ranges::find(step.parameters, "step", &ParameterValue::id)->value = fallback;
        const auto connected = f.prepare();
        require(connected->owners()[0].analogKeyStepTriggered
                && !connected->equivalent(*prepared),
            "connected Step selects edge triggering even with an automatic-clock fallback");
    }
    step.inputs.clear();
    std::ranges::find(step.parameters, "step", &ParameterValue::id)->value = 1.0;
    const auto modernStep = step;
    textValue("generatedIndex", " !Stage ");
    const auto indexed = f.prepare();
    require(hasPreparedAnalogSequencerGeneratedIndex(f.plan)
            && indexed->owners()[0].analogGeneratedIndex == "Stage"
            && indexed->eventCount() == prepared->eventCount() + 5
            && indexed->owners()[0].analogExtraFrameAt(4) == 40
            && indexed->owners()[0].analogExtraCursorAtOrAfter(11) == 2
            && !indexed->equivalent(*prepared),
        "GeneratedIndex alone retains silent-stage deadlines and charges event capacity");
    f.reject("event limit", {10, 256});
    for (const auto invalid : {"Stage", "!Stage + 1", "!KeyNumber"})
    {
        std::ranges::find(step.parameters, "generatedIndex", &ParameterValue::id)->text = invalid;
        f.reject("GeneratedIndex");
    }
    std::ranges::find(step.parameters, "generatedIndex", &ParameterValue::id)->text = "!Stage";
    std::ranges::find(step.parameters, "extraValues", &ParameterValue::id)->text = "#(!Stage 1)";
    f.reject("GeneratedIndex");
    step = modernStep;
    const auto previousLanguage = f.settings.expressionLanguage;
    f.settings.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV4;
    f.settings.eventValues.push_back({"Shift", 200});
    std::ranges::find(step.parameters, "keyPitches", &ParameterValue::id)->text = "{60 nn + !Shift nn}";
    const auto live = f.prepare();
    require(live->owners()[0].analogLiveArrays && live->eventCount() == 10
            && live->owners()[0].analogKeySequences[3].expressions.size() == 1
            && !live->equivalent(*prepared),
        "live arrays retain expressions and reserve the worst-case note count without running inactive stages");
    f.reject("simultaneous", {65536, 1});
    f.reject("event limit", {9, 256});
    for (const auto invalid : {"{!Missing nn}", "{1 hz}", "{previous(!Shift, 0)}"})
    {
        std::ranges::find(step.parameters, "keyPitches", &ParameterValue::id)->text = invalid;
        f.reject("Analog Sequencer");
    }
    f.settings.expressionLanguage = previousLanguage;
    step = modernStep;
    std::ranges::find(step.parameters, "extraValues", &ParameterValue::id)->text =
        "#(!Morph 0 0.25 1) #(!Pan {-0.5} {0.5 + 0.5})";
    const auto extra = f.prepare();
    require(extra->eventCount() == 16
            && extra->owners()[0].scriptEvents == prepared->owners()[0].scriptEvents
            && extra->owners()[0].analogExtraSequences[1].values == std::vector<double>{-0.5, 1}
            && extra->owners()[0].analogExtraFrameAt(4) == 40
            && !extra->owners()[0].analogExtraFrameAt(5)
            && extra->owners()[0].analogExtraCursorAtOrAfter(11) == 2
            && !extra->equivalent(*prepared),
        "ExtraValues keeps named scalar arrays, stage deadlines and source identity");
    for (const auto invalid : {"#(!Morph)", "#(!Morph 0 nan)", "#(!Morph 0) #(!Morph 1)",
             "#(!KeyVelocity 0.5)", "#(!Morph 0junk)", "#(!Morph {1 hz})", "#(!Morph {!Live})"})
    {
        std::ranges::find(step.parameters, "extraValues", &ParameterValue::id)->text = invalid;
        f.reject("ExtraValues");
    }
    step = modernStep;
    for (const auto id : {"keyDowns", "keyPitches", "keyVelocities", "keyTimbres"})
        std::ranges::find(step.parameters, id, &ParameterValue::id)->text = "";
    std::ranges::find(step.parameters, "pitches", &ParameterValue::id)->text =
        "{60 nn} {62.5 nn}";
    std::ranges::find(step.parameters, "velocities", &ParameterValue::id)->text =
        "#(0.2 0)";
    const auto classic = f.prepare();
    require(hasPreparedAnalogSequencerKeyOwner(step)
            && classic->eventCount() == 10
            && classic->owners()[0].scriptEvents[0].pitch == 60
            && classic->owners()[0].scriptEvents[0].velocity == 0.2
            && classic->owners()[0].scriptEvents[8].pitch == 62.5
            && classic->owners()[0].scriptEvents[8].velocity == 0
            && classic->owners()[0].scriptEvents[8].timbre == 0,
        "classic arrays create notes and repeat the final pitch and exact zero velocity");
    for (const auto id : {"keyDowns", "keyPitches", "keyVelocities", "keyTimbres"})
    {
        auto& value = *std::ranges::find(step.parameters, id, &ParameterValue::id);
        value.text = "#(1)";
        f.reject("cannot mix");
        value.text = "";
    }
    for (const auto invalid : {"#(128)", "#(-1)", "{1 hz}", "#(bad)"})
    {
        std::ranges::find(step.parameters, "pitches", &ParameterValue::id)->text = invalid;
        f.reject("Analog Sequencer");
    }
    std::ranges::find(step.parameters, "pitches", &ParameterValue::id)->text = "#(60)";
    // Empty Velocities play at 1 (2026-10-19).
    for (const auto invalid : {"#(-0.1)", "#(1.1)"})
    {
        std::ranges::find(step.parameters, "velocities", &ParameterValue::id)->text = invalid;
        f.reject("Analog Sequencer");
    }
    step = modernStep;
    auto* manualStep = &*std::ranges::find(step.parameters, "step", &ParameterValue::id);
    manualStep->value = 1.0;
    require(!f.prepare()->owners()[0].analogKeyStepTriggered,
        "constant one selects the historical automatic clock");
    manualStep->value = 0.25;
    const auto triggered = f.prepare();
    require(triggered->owners()[0].analogKeyStepTriggered
            && !triggered->equivalent(*prepared),
        "a fractional positive constant selects one edge and changes schedule identity");
    manualStep->value = -1.0;
    f.reject("non-negative scalar step");
    manualStep->value = 0.0;
    auto* gate = &*std::ranges::find(step.parameters, "gate", &ParameterValue::id);
    gate->value = 0.0;
    require(f.prepare()->eventCount() == prepared->eventCount(),
        "an initially closed Gate retains its cycle for later runtime restart");
    gate->text = "invalid";
    f.reject("finite scalar gate");
    gate->text.reset();
    gate->value = 1.0;
    auto* rate = &*std::ranges::find(step.parameters, "rate", &ParameterValue::id);
    rate->value = 0.0;
    f.reject("non-zero initial scalar Rate");
    rate->value = 1.0;
    step.parameters[2].text = "#(1 0.5 1 0 0)";
    f.reject("invalid");
    step.parameters[2].text = "#(1 1 1 0 0)";
    std::ranges::find(step.parameters, "loop", &ParameterValue::id)->boolean = true;
    const auto looped = f.prepare();
    const auto& owner = looped->owners()[0];
    require(owner.loop && owner.cycleFrames == 50
            && owner.scriptFrameAt(6) == 50
            && owner.scriptFrameAt(12) == 100
            && owner.scriptCursorAtOrAfter(49) == 6
            && owner.scriptCursorAtOrAfter(50) == 6,
        "looped owner repeats the prepared cycle without expanding event storage");
    step.parameters[2].text = "#(1 0 0 0 1)";
    const auto boundary = f.prepare();
    const auto& boundaryOwner = boundary->owners()[0];
    require(boundaryOwner.scriptFrameAt(3) == 50
            && boundaryOwner.scriptFrameAt(4) == 50
            && boundaryOwner.scriptCursorAtOrAfter(50) == 3,
        "cycle-end note-off precedes the next cycle's first attack on seek");
    f.reject("simultaneous", {65536, 1});
    step.parameters[2].text = "#(0 0 0 0 0)";
    const auto silentLoop = f.prepare();
    require(silentLoop->hasScheduledSource()
            && silentLoop->eventCount() == 0
            && !silentLoop->owners()[0].scriptFrameAt(0),
        "an all-inactive loop remains silent without a zero-time event spin");

    std::ranges::find(step.parameters, "dutyCycles", &ParameterValue::id)->text =
        "#(0.5 0.5 0.5 0.5 0.5)";
    step.parameters[2].text = "#(1 0 0 0 0)";
    const auto first = step;
    f.addOwner(2);
    auto& second = f.plan.steps[1];
    second = first;
    second.nodeId = "looped.2";
    std::ranges::find(second.parameters, "durations", &ParameterValue::id)->text =
        "#(0.01 0.01 0.01)";
    std::ranges::find(second.parameters, "dutyCycles", &ParameterValue::id)->text =
        "#(0.5 0.5 0.5)";
    std::ranges::find(second.parameters, "keyDowns", &ParameterValue::id)->text =
        "#(0 1 0)";
    for (const auto id : {"keyPitches", "keyVelocities", "keyTimbres"})
        std::ranges::find(second.parameters, id, &ParameterValue::id)->text =
            "#(1 1 1)";
    std::ranges::find(second.parameters, "endIndex", &ParameterValue::id)->value = 2;
    const auto overlappingLoops = f.prepare({65536, 2});
    require(overlappingLoops->owners()[0].scriptFrameAt(4) == 100
            && overlappingLoops->owners()[1].scriptFrameAt(6) == 100,
        "different cycle lengths align at a later callback frame");
    f.reject("simultaneous", {65536, 1});
}

Fixture makeAnalogSequencerRateFixture()
{
    Fixture fixture;
    auto& step = fixture.plan.steps[0];
    step.typeId = "talas.analogSequencer";
    step.parameters.clear();
    const auto textValue = [&](std::string id, std::string value) {
        ParameterValue parameter;
        parameter.id = std::move(id);
        parameter.text = std::move(value);
        step.parameters.push_back(std::move(parameter));
    };
    const auto scalarValue = [&](std::string id, double value) {
        ParameterValue parameter;
        parameter.id = std::move(id);
        parameter.value = value;
        step.parameters.push_back(std::move(parameter));
    };
    textValue("durations", "#(0.01 0.02 0.03 0.04)");
    textValue("dutyCycles", "#(0.5 0.25 0.5 0.75)");
    textValue("keyDowns", "#(0 1 0 1)");
    textValue("keyPitches", "#(60 61 62 63)");
    textValue("keyVelocities", "#(1 1 1 1)");
    textValue("keyTimbres", "#(0 0 0 0)");
    scalarValue("startIndex", 1.0);
    scalarValue("endIndex", 3.0);
    ParameterValue origin;
    origin.id = "indexOrigin";
    origin.choice = "zeroBased";
    step.parameters.push_back(origin);
    ParameterValue loop;
    loop.id = "loop";
    loop.boolean = false;
    step.parameters.push_back(loop);
    scalarValue("gate", 1.0);
    scalarValue("step", 1.0);
    scalarValue("rate", -2.0);
    textValue("pitches", "");
    textValue("velocities", "");
    textValue("extraValues", "");
    fixture.settings.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV4;
    fixture.settings.eventValues = {
        {"RateControl", -2.0, talas::expr::ValueDomain::Scalar},
    };
    fixture.settings.binaryResources.clear();
    fixture.settings.resourceSnapshots.clear();
    return fixture;
}

void testAnalogSequencerZeroStepPolicyPreparation()
{
    auto fixture = makeAnalogSequencerRateFixture();
    auto& step = fixture.plan.steps[0];
    const auto setStep = [&](double value) {
        std::ranges::find(step.parameters, "step", &ParameterValue::id)->value = value;
    };
    setStep(0);
    const auto waiting = fixture.prepare();
    require(waiting->owners()[0].analogKeyStepTriggered,
        "missing compatibility choice uses the new zero-Step waiting policy");
    ParameterValue mode;
    mode.id = "stepZeroMode";
    mode.choice = "waitForTrigger";
    step.parameters.push_back(mode);
    require(fixture.prepare()->equivalent(*waiting), "explicit waiting policy matches its default");
    step.parameters.back().choice = "legacyAutomatic";
    const auto legacy = fixture.prepare();
    require(!legacy->owners()[0].analogKeyStepTriggered && !legacy->equivalent(*waiting),
        "legacy zero-Step automatic clock has distinct prepared source identity");
    for (const auto policy : {"waitForTrigger", "legacyAutomatic"})
    {
        step.parameters.back().choice = policy;
        setStep(1);
        require(!fixture.prepare()->owners()[0].analogKeyStepTriggered,
            "constant one remains automatic under both compatibility policies");
        InputBinding input;
        input.inputPortId = "step";
        step.inputs.push_back(input);
        for (const auto value : {0.0, 1.0})
        {
            setStep(value);
            require(fixture.prepare()->owners()[0].analogKeyStepTriggered,
                "connected Step always uses edge detection");
        }
        step.inputs.clear();
    }
    setStep(0);
    for (const auto invalid : {"", "automatic", "zeroBased"})
    {
        step.parameters.back().choice = invalid;
        fixture.reject("Step zero compatibility choice");
    }
    for (const auto& mutate : std::vector<std::function<void(ParameterValue&)>>{
             [](auto& p) { p.choice.reset(); },
             [](auto& p) { p.domain = talas::expr::ValueDomain::Seconds; },
             [](auto& p) { p.text = "waitForTrigger"; },
             [](auto& p) { p.boolean = false; },
             [](auto& p) { p.resource = "resource"; },
             [](auto& p) { p.variableCellIndices = {0}; },
             [](auto& p) { p.expression = *talas::expr::parse("0").expression; }})
    {
        step.parameters.back() = mode;
        mutate(step.parameters.back());
        fixture.reject("Step zero compatibility choice");
    }
}

void testAnalogSequencerBareAndMixedArrayAuthoring()
{
    auto fixture = makeAnalogSequencerRateFixture();
    const auto reference = fixture.prepare();
    for (const auto [id, source] : {
             std::pair{"durations", "10 ms 20ms {30 ms} 4e-2"},
             {"dutyCycles", ".5 .25 {.5} .75"}, {"keyDowns", "0 1 {0} 1"},
             {"keyPitches", "60 nn 61 {62 nn} 63nn"},
             {"keyVelocities", "+1 1 {1} 1"}, {"keyTimbres", "0 0 {0} 0"}})
        std::ranges::find(fixture.plan.steps[0].parameters, id, &ParameterValue::id)->text = source;
    const auto authored = fixture.prepare();
    require(authored->owners()[0].scriptEvents == reference->owners()[0].scriptEvents
            && authored->owners()[0].analogKeyStages == reference->owners()[0].analogKeyStages,
        "mixed bare numbers, attached/spaced units and brace compounds retain exact numeric schedules");

    fixture.settings.eventValues.push_back({"Duration", 20});
    std::ranges::find(fixture.plan.steps[0].parameters, "durations", &ParameterValue::id)->text =
        "10 ms !Duration ms {30 ms} 40 ms";
    const auto live = fixture.prepare();
    require(live->owners()[0].analogLiveArrays, "a bare HotValue remains a live member");
    const auto& duration = live->owners()[0].analogKeySequences[0];
    double sampled = 0;
    require(evaluateAnalogSequencerValue(duration, 1, fixture.settings.eventValues,
                fixture.settings.preparedVariables, 1000, 120, sampled, {})
            && std::abs(sampled - 0.02) < 1e-12,
        "bare typed HotValues use the array's expected domain");
    fixture.settings.eventValues.back().value = 35;
    require(evaluateAnalogSequencerValue(duration, 1, fixture.settings.eventValues,
                fixture.settings.preparedVariables, 1000, 120, sampled, {})
            && std::abs(sampled - 0.035) < 1e-12,
        "bare HotValues sample updated stage bindings");

    const std::string source = " \t10 ms  !Length {20 + !Offset} @Length\n-3.5e-2s";
    const auto members = talas::core::parseAnalogSequencerArray(source);
    require(members && members->members.size() == 5 && !members->numericLiteral,
        "mixed authoring retains five separate expression members");
    for (const auto& member : members->members)
        require(source.substr(member.offset, member.source.size()) == member.source,
            "member offsets address the exact authored source for diagnostics");
    for (const auto invalid : {"", "{}", "{1", "{{1}}", "1+2", "!Gain + 1", "1, 2",
             "!", "@", "nan", "inf", "1e999", "#()", "#(1 ms)", "#(1) 2", "{1}2"})
        require(!talas::core::parseAnalogSequencerArray(invalid),
            "malformed member lists must fail structural admission");
    std::string maximum;
    for (int index = 0; index < 256; ++index) maximum += "1 ";
    require(talas::core::parseAnalogSequencerArray(maximum).has_value()
            && !talas::core::parseAnalogSequencerArray(maximum + "1"),
        "bare arrays retain the 256-member capacity");

    using Domain = talas::expr::ValueDomain;
    for (const auto invalid : {"10 unknownUnit", "{1 / 0}", "{1 ramp: 4 ms}", "!Duration"})
        require(!prepareAnalogSequencerConstantArray(invalid, fixture.settings, Domain::Seconds),
            "amplitude arrays retain constant, finite, typed admission");
    fixture.settings.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV2;
    fixture.reject("Analog Sequencer");
}

void testAnalogSequencerClockedArrayMembersRetainStateAndSoundFrame()
{
    auto fixture = makeAnalogSequencerRateFixture();
    auto& step = fixture.plan.steps[0];
    std::ranges::find(step.parameters, "keyDowns", &ParameterValue::id)->text =
        "{!Arm ramp: 4 ms} {!Arm ramp: 4 ms} {!Arm ramp: 4 ms} {!Arm ramp: 4 ms}";
    std::ranges::find(step.parameters, "extraValues", &ParameterValue::id)->text =
        "#(!Morph {!Arm ramp: 4 ms})";
    fixture.settings.eventValues.push_back(
        {"Arm", 0.0, talas::expr::ValueDomain::Scalar});

    const auto prepared = fixture.prepare();
    const auto& owner = prepared->owners()[0];
    const auto& keyDowns = owner.analogKeySequences[2];
    const auto& extra = owner.analogExtraSequences[0];
    require(owner.analogLiveArrays
            && keyDowns.expressions.size() == 4
            && keyDowns.values == std::vector<double>(4, 1.0)
            && extra.expressions.size() == 1
            && extra.values == std::vector<double>{1.0},
        "clocked key and ExtraValues members retain aligned initial values and live identity");

    std::vector<talas::expr::ExpressionValue> variables;
    talas::expr::ExpressionState state;
    require(talas::expr::prepareExpressionState(keyDowns.expressions[0], state),
        "a clocked array member prepares its bounded expression state");
    double value = -1.0;
    require(evaluateAnalogSequencerValue(
                keyDowns, 0, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, variables, &state, 0)
            && value == 1.0,
        "timed array evaluation uses the current local Sound frame (a ramp rests at 1)");
    fixture.settings.eventValues.back().value = 1.0;
    require(evaluateAnalogSequencerValue(
                keyDowns, 0, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, variables, &state, 1)
            && value == 0.0
            && evaluateAnalogSequencerValue(
                keyDowns, 0, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, variables, &state, 4)
            && value == 0.75
            && evaluateAnalogSequencerValue(
                keyDowns, 0, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, variables, &state, 5)
            && value == 1.0,
        "clocked array Ramp state observes EventValue edges and elapsed Sound frames");
    const auto committed = state;
    require(!evaluateAnalogSequencerValue(
                keyDowns, 0, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, variables, &state, 4)
            && state == committed,
        "a regressing array-member Sound clock rejects without committing state");

    talas::expr::ExpressionState extraState;
    require(talas::expr::prepareExpressionState(extra.expressions[0], extraState)
            && evaluateAnalogSequencerValue(
                extra, 0, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, variables, &extraState, 1)
            && value == 0.0
            && evaluateAnalogSequencerValue(
                extra, 0, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, variables, &extraState, 5)
            && value == 1.0,
        "ExtraValues timed members retain their own state and sample the same local clock");

    auto unclocked = makeAnalogSequencerRateFixture();
    std::ranges::find(unclocked.plan.steps[0].parameters,
        "keyDowns", &ParameterValue::id)->text =
        "{previous(!Arm, 0)} {!Arm ramp: 4 ms} {!Arm ramp: 4 ms} {!Arm ramp: 4 ms}";
    unclocked.settings.eventValues.push_back(
        {"Arm", 0.0, talas::expr::ValueDomain::Scalar});
    unclocked.reject("clocked expressions");

    auto wrongDomain = makeAnalogSequencerRateFixture();
    std::ranges::find(wrongDomain.plan.steps[0].parameters,
        "keyPitches", &ParameterValue::id)->text =
        "{1 ramp: 4 ms} {1 ramp: 4 ms} {1 ramp: 4 ms} {1 ramp: 4 ms}";
    wrongDomain.reject("valid units");

    auto oldProfile = makeAnalogSequencerRateFixture();
    oldProfile.settings.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    std::ranges::find(oldProfile.plan.steps[0].parameters,
        "keyDowns", &ParameterValue::id)->text =
        "{1 ramp: 4 ms} {1 ramp: 4 ms} {1 ramp: 4 ms} {1 ramp: 4 ms}";
    oldProfile.reject("Analog Sequencer");
}

void testAnalogSequencerProjectVariableArraysBindAndSampleSnapshots()
{
    const auto configure = [](Fixture& fixture) {
        auto& step = fixture.plan.steps[0];
        step.scopeId = 20;
        const auto textValue = [&](std::string_view id, std::string value) {
            std::ranges::find(step.parameters, id, &ParameterValue::id)->text =
                std::move(value);
        };
        textValue("durations", "@duration {@duration} @duration {@duration}");
        textValue("dutyCycles", "{@duty} {@duty} {@duty} {@duty}");
        textValue("keyDowns", "{@down} {@down} {@down} {@down}");
        textValue("keyPitches", "@tone {@tone} @tone {@tone}");
        textValue("keyVelocities", "{@velocity} {@velocity} {@velocity} {@velocity}");
        textValue("keyTimbres", "{@timbre} {@timbre} {@timbre} {@timbre}");
        textValue("extraValues", "#(!morph {@parentLevel} {@rootLevel})");

        auto& variables = fixture.settings.preparedVariables;
        variables.scopes = {
            {0, 0, {}, {}},
            {10, 0, "Parent", {"outer"}},
            {20, 10, "Child", {"outer", "inner"}},
        };
        const auto addCell = [&](std::string id, talas::expr::ValueDomain domain,
                                 double initialValue, std::uint32_t scope) {
            talas::project::PreparedProjectVariableCell cell;
            cell.identity.ownerKind =
                talas::project::ProjectVariableOwnerKind::Project;
            cell.identity.variableId = std::move(id);
            cell.domain = domain;
            cell.initialValue = initialValue;
            cell.scopeOrdinal = scope;
            variables.cells.push_back(std::move(cell));
        };
        variables.cells.clear();
        addCell("down", talas::expr::ValueDomain::Scalar, 1.0, 0);
        addCell("tone", talas::expr::ValueDomain::NoteNumber, 61.0, 10);
        addCell("tone", talas::expr::ValueDomain::NoteNumber, 62.0, 20);
        addCell("duration", talas::expr::ValueDomain::Seconds, 0.01, 10);
        addCell("duty", talas::expr::ValueDomain::Scalar, 0.5, 20);
        addCell("velocity", talas::expr::ValueDomain::Scalar, 0.75, 10);
        addCell("timbre", talas::expr::ValueDomain::Scalar, 0.25, 0);
        addCell("morph", talas::expr::ValueDomain::Scalar, 0.4, 0);
        addCell("parentLevel", talas::expr::ValueDomain::Scalar, 0.3, 10);
        addCell("rootLevel", talas::expr::ValueDomain::Scalar, 0.2, 0);
    };

    auto fixture = makeAnalogSequencerRateFixture();
    configure(fixture);
    const auto prepared = fixture.prepare();
    const auto& owner = prepared->owners()[0];
    require(owner.analogLiveArrays
            && owner.analogKeySequences[0].variableCellIndices
                == std::vector<std::vector<std::uint32_t>>(
                    4, std::vector<std::uint32_t>{3})
            && owner.analogKeySequences[1].variableCellIndices
                == std::vector<std::vector<std::uint32_t>>(
                    4, std::vector<std::uint32_t>{4})
            && owner.analogKeySequences[2].variableCellIndices
                == std::vector<std::vector<std::uint32_t>>(
                    4, std::vector<std::uint32_t>{0})
            && owner.analogKeySequences[3].variableCellIndices
                == std::vector<std::vector<std::uint32_t>>(
                    4, std::vector<std::uint32_t>{2})
            && owner.analogKeySequences[4].variableCellIndices
                == std::vector<std::vector<std::uint32_t>>(
                    4, std::vector<std::uint32_t>{5})
            && owner.analogKeySequences[5].variableCellIndices
                == std::vector<std::vector<std::uint32_t>>(
                    4, std::vector<std::uint32_t>{6}),
        "variable-only note arrays are live and bind shadowed, parent and global cells by owner scope");
    require(owner.analogExtraSequences.size() == 1
            && owner.analogExtraSequences[0].values
                == std::vector<double>{0.3, 0.2}
            && owner.analogExtraSequences[0].variableCellIndices
                == std::vector<std::vector<std::uint32_t>>{{8}, {9}},
        "ExtraValues keeps finite initial values and exact nearest parent/global cell bindings");

    std::vector<talas::expr::ExpressionValue> snapshot;
    snapshot.reserve(fixture.settings.preparedVariables.cells.size());
    for (const auto& cell : fixture.settings.preparedVariables.cells)
        snapshot.push_back({cell.initialValue, cell.domain});
    double value = 0.0;
    require(evaluateAnalogSequencerValue(
                owner.analogKeySequences[3], 2, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, snapshot)
            && value == 62.0,
        "stage sampling resolves the currently bound shadowing cell");
    snapshot[2].value = 67.5;
    require(evaluateAnalogSequencerValue(
                owner.analogKeySequences[3], 2, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, snapshot)
            && value == 67.5,
        "re-evaluating the same stage observes a newer variable snapshot");
    snapshot[5].value = 0.25;
    require(evaluateAnalogSequencerValue(
                owner.analogKeySequences[4], 1, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, snapshot)
            && value == 0.25,
        "velocity expressions sample their bound variable cell rather than preparation values");
    require(evaluateAnalogSequencerValue(
                owner.analogExtraSequences[0], 0, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, snapshot)
            && value == 0.3,
        "ExtraValues are re-evaluated from the accepted stage's variable snapshot");
    snapshot[8].value = 0.8;
    require(evaluateAnalogSequencerValue(
                owner.analogExtraSequences[0], 0, fixture.settings.eventValues,
                fixture.settings.preparedVariables, fixture.settings.sampleRate,
                fixture.settings.tempoBpm, value, snapshot)
            && value == 0.8,
        "ExtraValues observe subsequent cell changes at the same stage index");

    auto reordered = makeAnalogSequencerRateFixture();
    configure(reordered);
    talas::project::PreparedProjectVariableCell unrelated;
    unrelated.identity.ownerKind = talas::project::ProjectVariableOwnerKind::Project;
    unrelated.identity.variableId = "unrelated";
    unrelated.domain = talas::expr::ValueDomain::Scalar;
    unrelated.initialValue = 0.0;
    unrelated.scopeOrdinal = 0;
    reordered.settings.preparedVariables.cells.insert(
        reordered.settings.preparedVariables.cells.begin(), unrelated);
    require(!prepared->equivalent(*reordered.prepare()),
        "prepared-source identity includes the exact bound project-variable cell indices");

    auto unbound = makeAnalogSequencerRateFixture();
    configure(unbound);
    std::ranges::find(unbound.plan.steps[0].parameters,
        "keyPitches", &ParameterValue::id)->text =
        "{@missing} {@missing} {@missing} {@missing}";
    unbound.reject("unbound project variable");

    auto wrongNoteDomain = makeAnalogSequencerRateFixture();
    configure(wrongNoteDomain);
    wrongNoteDomain.settings.preparedVariables.cells[2].domain =
        talas::expr::ValueDomain::Hertz;
    const auto wrongDomainPrepared = wrongNoteDomain.prepare();
    std::vector<talas::expr::ExpressionValue> wrongDomainSnapshot;
    for (const auto& cell : wrongNoteDomain.settings.preparedVariables.cells)
        wrongDomainSnapshot.push_back({cell.initialValue, cell.domain});
    require(!evaluateAnalogSequencerValue(
                wrongDomainPrepared->owners()[0].analogKeySequences[3], 0,
                wrongNoteDomain.settings.eventValues,
                wrongNoteDomain.settings.preparedVariables,
                wrongNoteDomain.settings.sampleRate, wrongNoteDomain.settings.tempoBpm,
                value, wrongDomainSnapshot),
        "stage sampling rejects a changed variable cell domain for a note array");

    auto wrongExtraDomain = makeAnalogSequencerRateFixture();
    configure(wrongExtraDomain);
    wrongExtraDomain.settings.preparedVariables.cells[8].domain =
        talas::expr::ValueDomain::Hertz;
    wrongExtraDomain.reject("finite scalar");
}

void testAnalogSequencerVariableReferenceCapacityIncludesExistingBindings()
{
    const auto makeExtraValues = [](std::size_t fullArrays,
                                    std::size_t finalArraySize) {
        std::string text;
        const auto arrayCount = fullArrays + 1;
        for (std::size_t array = 0; array < arrayCount; ++array)
        {
            if (!text.empty()) text.push_back(' ');
            text += "#(!a" + std::to_string(array);
            const auto members = array < fullArrays ? 256 : finalArraySize;
            for (std::size_t member = 0; member < members; ++member)
                text += " {@morph}";
            text.push_back(')');
        }
        return text;
    };
    const auto configure = [](Fixture& fixture) {
        fixture.settings.preparedVariables.scopes = {{0, 0, {}, {}}};
        talas::project::PreparedProjectVariableCell cell;
        cell.identity.ownerKind = talas::project::ProjectVariableOwnerKind::Project;
        cell.identity.variableId = "morph";
        cell.domain = talas::expr::ValueDomain::Scalar;
        cell.initialValue = 0.5;
        cell.scopeOrdinal = 0;
        fixture.settings.preparedVariables.cells = {cell};

        auto& parameter = fixture.plan.steps[0].parameters.emplace_back();
        parameter.id = "preexistingVariableParameter";
        parameter.variableCellIndices = {0};
        auto projection = talas::expr::parse(
            "@morph", fixture.settings.expressionLanguage,
            fixture.settings.expressionDefinitions);
        require(projection.ok(), "projected EventValue reference should parse");
        EventValueBinding projected;
        projected.name = "unrelatedProjection";
        projected.value = 0.5;
        projected.projectionExpression = *projection.expression;
        projected.variableCellIndices = {0};
        fixture.settings.eventValues.push_back(std::move(projected));
    };

    auto exact = makeAnalogSequencerRateFixture();
    configure(exact);
    std::ranges::find(exact.plan.steps[0].parameters,
        "extraValues", &ParameterValue::id)->text =
        makeExtraValues(15, 254);
    require(exact.prepare()->owners()[0].analogExtraSequences.size() == 16,
        "4094 embedded uses plus one plan parameter and one projection use fit the shared 4096-use budget");

    auto exceeded = makeAnalogSequencerRateFixture();
    configure(exceeded);
    std::ranges::find(exceeded.plan.steps[0].parameters,
        "extraValues", &ParameterValue::id)->text =
        makeExtraValues(15, 256);
    exceeded.reject("prepared variable reference limit");
}

void testAnalogSequencerLiveRatePreparation()
{
    auto fixture = makeAnalogSequencerRateFixture();
    auto& step = fixture.plan.steps[0];
    auto* rate = &*std::ranges::find(step.parameters, "rate", &ParameterValue::id);
    const auto parsedRate = talas::expr::parse(
        "!RateControl", fixture.settings.expressionLanguage,
        fixture.settings.expressionDefinitions);
    require(parsedRate.ok(), "live Rate expression should parse in VariablesV4");
    rate->expression = *parsedRate.expression;
    const auto prepared = fixture.prepare();
    const auto& owner = prepared->owners()[0];
    require(owner.analogLiveRate && owner.analogKeyRate == -2.0
            && owner.cycleFrames == 3 && prepared->eventCount() == 6,
        "live Rate retains its finite initial value and prepares a one-frame capacity skeleton");
    require(owner.analogKeyStages.size() == 3
            && owner.analogKeyStages[0].sourceIndex == 1
            && owner.analogKeyStages[1].sourceIndex == 2
            && owner.analogKeyStages[2].sourceIndex == 3
            && owner.analogKeyStages[0].durationFrames == 1
            && owner.analogKeyStages[1].durationFrames == 1
            && owner.analogKeyStages[2].durationFrames == 1
            && owner.analogKeyStages[0].durationSeconds == 0.02
            && owner.analogKeyStages[1].durationSeconds == 0.03
            && owner.analogKeyStages[2].durationSeconds == 0.04
            && owner.analogKeyStages[0].dutyCycle == 0.25
            && owner.analogKeyStages[1].dutyCycle == 0.5
            && owner.analogKeyStages[2].dutyCycle == 0.75,
        "live Rate preparation keeps ascending selected indices and unscaled duration and duty");
    require(owner.scriptEvents.size() == 4
            && owner.scriptEvents[0].frame == 0 && owner.scriptEvents[0].noteOn
            && owner.scriptEvents[1].frame == 1 && !owner.scriptEvents[1].noteOn
            && owner.scriptEvents[2].frame == 2 && owner.scriptEvents[2].noteOn
            && owner.scriptEvents[3].frame == 3 && !owner.scriptEvents[3].noteOn,
        "live Rate's capacity skeleton uses one-frame placeholder timing");
    require(prepared->equivalent(*fixture.prepare()),
        "identical live Rate source retains exact prepared identity");

    for (const auto source : {
             std::string_view{"@RateControl"},
             std::string_view{"1 ramp: 2 s"}})
    {
        auto candidate = makeAnalogSequencerRateFixture();
        auto& candidateRate = *std::ranges::find(
            candidate.plan.steps[0].parameters, "rate", &ParameterValue::id);
        const auto expression = talas::expr::parse(
            source, candidate.settings.expressionLanguage,
            candidate.settings.expressionDefinitions);
        require(expression.ok(), "dynamic Rate feature expression should parse");
        candidateRate.expression = *expression.expression;
        if (source.starts_with("@")) candidateRate.variableCellIndices = {0};
        require(candidate.prepare()->owners()[0].analogLiveRate,
            "variable and timed Rate expressions have a supported live clock");
    }
    for (const auto source : {
             std::string_view{"[1, rateIdentifier][0]"},
             std::string_view{"previous(1, -2)"}})
    {
        auto candidate = makeAnalogSequencerRateFixture();
        auto& candidateRate = *std::ranges::find(
            candidate.plan.steps[0].parameters, "rate", &ParameterValue::id);
        const auto expression = talas::expr::parse(
            source, candidate.settings.expressionLanguage,
            candidate.settings.expressionDefinitions);
        require(expression.ok(), "unclocked dynamic Rate feature should parse");
        candidateRate.expression = *expression.expression;
        candidate.reject("EventValue/variable/time clock");
    }

    auto unspecializedVoiceNumber = makeAnalogSequencerRateFixture();
    unspecializedVoiceNumber.settings.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VoiceV1;
    auto& voiceNumberRate = *std::ranges::find(
        unspecializedVoiceNumber.plan.steps[0].parameters,
        "rate", &ParameterValue::id);
    const auto voiceNumberExpression = talas::expr::parse(
        "?VoiceNumber",
        unspecializedVoiceNumber.settings.expressionLanguage,
        unspecializedVoiceNumber.settings.expressionDefinitions);
    require(voiceNumberExpression.ok(),
        "unspecialized VoiceNumber Rate feature should parse");
    voiceNumberRate.expression = *voiceNumberExpression.expression;
    unspecializedVoiceNumber.reject("EventValue/variable/time clock");

    auto connected = makeAnalogSequencerRateFixture();
    auto& connectedStep = connected.plan.steps[0];
    InputBinding rateInput;
    rateInput.inputPortId = "rate";
    rateInput.sourceNodeId = "rateSource";
    rateInput.sourcePortId = "value";
    connectedStep.inputs.push_back(rateInput);
    const auto connectedPrepared = connected.prepare();
    const auto& connectedOwner = connectedPrepared->owners()[0];
    require(connectedOwner.analogLiveRate && connectedOwner.analogKeyRate == -2.0
            && connectedOwner.analogKeyStages[0].sourceIndex == 1
            && connectedOwner.analogKeyStages[2].sourceIndex == 3,
        "a connected Rate input uses live scheduling with canonical ascending source indices");
    auto staticNegative = makeAnalogSequencerRateFixture();
    const auto staticPrepared = staticNegative.prepare();
    require(!staticPrepared->owners()[0].analogLiveRate
            && staticPrepared->owners()[0].analogKeyStages[0].sourceIndex == 3
            && staticPrepared->owners()[0].analogKeyStages[2].sourceIndex == 1
            && !connectedPrepared->equivalent(*staticPrepared),
        "connected live Rate remains distinguishable from a static negative schedule");
    auto connectedLargeFallback = makeAnalogSequencerRateFixture();
    std::ranges::find(connectedLargeFallback.plan.steps[0].parameters,
        "rate", &ParameterValue::id)->value = 1.0e300;
    InputBinding connectedLargeRate;
    connectedLargeRate.inputPortId = "rate";
    connectedLargeRate.sourceNodeId = "rateSource";
    connectedLargeRate.sourcePortId = "value";
    connectedLargeFallback.plan.steps[0].inputs.push_back(connectedLargeRate);
    const auto connectedLargePrepared = connectedLargeFallback.prepare();
    require(connectedLargePrepared->owners()[0].analogLiveRate
            && connectedLargePrepared->owners()[0].analogKeyRate == 1.0e300
            && connectedLargePrepared->owners()[0].cycleFrames == 3,
        "connected Rate preparation does not scale the skeleton by its inactive fallback magnitude");

    auto staticSubsample = makeAnalogSequencerRateFixture();
    std::ranges::find(staticSubsample.plan.steps[0].parameters,
        "durations", &ParameterValue::id)->text =
        "#(0.01 0.0003 0.03 0.04)";
    std::ranges::find(staticSubsample.plan.steps[0].parameters,
        "rate", &ParameterValue::id)->value = -0.5;
    const auto staticSubsamplePrepared = staticSubsample.prepare();
    require(staticSubsamplePrepared->owners()[0].analogKeyStages[2].sourceIndex == 1
            && staticSubsamplePrepared->owners()[0].analogKeyStages[2].durationFrames == 1,
        "static slow Rate can represent a sub-sample authored duration");

    auto liveSubsample = makeAnalogSequencerRateFixture();
    std::ranges::find(liveSubsample.plan.steps[0].parameters,
        "durations", &ParameterValue::id)->text =
        "#(0.01 0.0003 0.03 0.04)";
    std::ranges::find(liveSubsample.plan.steps[0].parameters,
        "rate", &ParameterValue::id)->value = -0.5;
    liveSubsample.settings.eventValues[0].value = -0.5;
    auto& liveSubsampleRate = *std::ranges::find(
        liveSubsample.plan.steps[0].parameters, "rate", &ParameterValue::id);
    const auto liveSubsampleExpression = talas::expr::parse(
        "!RateControl", liveSubsample.settings.expressionLanguage,
        liveSubsample.settings.expressionDefinitions);
    require(liveSubsampleExpression.ok(),
        "sub-sample live Rate expression should parse");
    liveSubsampleRate.expression = *liveSubsampleExpression.expression;
    const auto liveSubsamplePrepared = liveSubsample.prepare();
    require(liveSubsamplePrepared->owners()[0].analogLiveRate
            && liveSubsamplePrepared->owners()[0].cycleFrames == 3
            && liveSubsamplePrepared->owners()[0].analogKeyStages[0].sourceIndex == 1
            && liveSubsamplePrepared->owners()[0].analogKeyStages[0].durationFrames == 1
            && liveSubsamplePrepared->owners()[0].analogKeyStages[0].durationSeconds == 0.0003,
        "live Rate skeleton admits a duration representable at its slow initial Rate");

    auto changedRate = fixture;
    std::ranges::find(changedRate.plan.steps[0].parameters, "rate", &ParameterValue::id)->value = -3.0;
    require(!prepared->equivalent(*changedRate.prepare()),
        "live Rate initial value participates in prepared source identity");
    auto changedOwner = fixture;
    changedOwner.plan.midiVoices[0].ownerDefinitionId = "otherInstrument";
    require(!prepared->equivalent(*changedOwner.prepare()),
        "live Rate source retains exact owner definition identity");

    auto changedDuration = fixture;
    std::ranges::find(changedDuration.plan.steps[0].parameters, "durations", &ParameterValue::id)->text =
        "#(0.01 0.0201 0.03 0.04)";
    const auto durationPrepared = changedDuration.prepare();
    require(durationPrepared->owners()[0].analogKeyStages[0].durationFrames == 1
            && durationPrepared->owners()[0].cycleFrames == owner.cycleFrames
            && durationPrepared->owners()[0].scriptEvents == owner.scriptEvents
            && durationPrepared->owners()[0].analogKeyStages[0].durationSeconds == 0.0201
            && !prepared->equivalent(*durationPrepared),
        "prepared identity retains raw seconds when frame-rounded schedules are equal");
    auto changedDuty = fixture;
    std::ranges::find(changedDuty.plan.steps[0].parameters, "dutyCycles", &ParameterValue::id)->text =
        "#(0.5 0.251 0.5 0.75)";
    const auto dutyPrepared = changedDuty.prepare();
    require(dutyPrepared->owners()[0].analogKeyStages[0].gateFrames
                == owner.analogKeyStages[0].gateFrames
            && dutyPrepared->owners()[0].analogKeyStages[0].dutyCycle == 0.251
            && !prepared->equivalent(*dutyPrepared),
        "prepared identity retains authored duty when rounded gate frames are equal");

    fixture.reject("event limit", {4, 256});
    fixture.reject("simultaneous", {65536, 1});
    auto invalidDynamicDuration = fixture;
    std::ranges::find(invalidDynamicDuration.plan.steps[0].parameters,
        "durations", &ParameterValue::id)->text =
        "#(0.01 0 0.03 0.04)";
    invalidDynamicDuration.reject("invalid duration");
    rate->value = 0.0;
    fixture.reject("non-zero initial scalar Rate");
    rate->value = std::numeric_limits<double>::infinity();
    fixture.reject("non-zero initial scalar Rate");
    rate->value = -2.0;
    rate->domain = talas::expr::ValueDomain::Hertz;
    fixture.reject("non-zero initial scalar Rate");
    auto invalidStaticRate = makeAnalogSequencerRateFixture();
    auto& invalidStatic = *std::ranges::find(
        invalidStaticRate.plan.steps[0].parameters, "rate", &ParameterValue::id);
    invalidStatic.value = std::numeric_limits<double>::infinity();
    invalidStaticRate.reject("non-zero initial scalar Rate");
    invalidStatic.value = 1.0;
    invalidStatic.domain = talas::expr::ValueDomain::Hertz;
    invalidStaticRate.reject("non-zero initial scalar Rate");
    auto zeroConnectedRate = makeAnalogSequencerRateFixture();
    auto& zeroConnectedStep = zeroConnectedRate.plan.steps[0];
    std::ranges::find(zeroConnectedStep.parameters, "rate", &ParameterValue::id)->value = 0.0;
    InputBinding zeroRateInput;
    zeroRateInput.inputPortId = "rate";
    zeroRateInput.sourceNodeId = "rateSource";
    zeroRateInput.sourcePortId = "value";
    zeroConnectedStep.inputs.push_back(zeroRateInput);
    zeroConnectedRate.reject("non-zero initial scalar Rate");
    auto duplicateRateInput = makeAnalogSequencerRateFixture();
    for (int index = 0; index < 2; ++index)
    {
        InputBinding duplicate;
        duplicate.inputPortId = "rate";
        duplicate.sourceNodeId = "rateSource" + std::to_string(index);
        duplicate.sourcePortId = "value";
        duplicateRateInput.plan.steps[0].inputs.push_back(std::move(duplicate));
    }
    duplicateRateInput.reject("at most one exact input");
}

void testAnalogSequencerLiveRangePreparation()
{
    const auto setExpression = [](
        Fixture& fixture,
        std::string_view parameterId,
        std::string_view source,
        double initialValue) {
        auto& parameter = *std::ranges::find(
            fixture.plan.steps[0].parameters,
            parameterId,
            &ParameterValue::id);
        const auto parsed = talas::expr::parse(
            source,
            fixture.settings.expressionLanguage,
            fixture.settings.expressionDefinitions);
        require(parsed.ok(), "live range expression should parse");
        parameter.expression = *parsed.expression;
        parameter.value = initialValue;
        parameter.domain = talas::expr::ValueDomain::Scalar;
    };
    const auto makeEventRange = [&]() {
        auto fixture = makeAnalogSequencerRateFixture();
        fixture.settings.eventValues.push_back(
            {"StartControl", 1.0, talas::expr::ValueDomain::Scalar});
        fixture.settings.eventValues.push_back(
            {"EndControl", 3.0, talas::expr::ValueDomain::Scalar});
        setExpression(fixture, "startIndex", "!StartControl", 1.0);
        setExpression(fixture, "endIndex", "!EndControl", 3.0);
        return fixture;
    };

    const auto staticPrepared = makeAnalogSequencerRateFixture().prepare();
    const auto& staticOwner = staticPrepared->owners()[0];
    require(!staticOwner.analogLiveRange
            && !staticOwner.analogLegacyRange
            && !staticOwner.hasAnalogDynamicClock()
            && staticOwner.analogKeyStages.size() == 3
            && staticOwner.analogKeyStages[0].sourceIndex == 3
            && staticOwner.analogKeyStages[1].sourceIndex == 2
            && staticOwner.analogKeyStages[2].sourceIndex == 1,
        "static ranges retain their selected subset and negative-Rate traversal");

    auto live = makeEventRange();
    auto& step = live.plan.steps[0];
    std::ranges::find(step.parameters, "extraValues", &ParameterValue::id)->text =
        "#(!Morph 1 2 3 4)";
    ParameterValue generated;
    generated.id = "generatedIndex";
    generated.text = "!CurrentStep";
    step.parameters.push_back(generated);
    const auto prepared = live.prepare();
    const auto& owner = prepared->owners()[0];
    require(owner.analogLiveRange && !owner.analogLegacyRange
            && owner.hasAnalogDynamicClock()
            && owner.analogKeyStages.size() == 4
            && owner.cycleFrames == 4
            && owner.analogKeyStages[0].sourceIndex == 0
            && owner.analogKeyStages[1].sourceIndex == 1
            && owner.analogKeyStages[2].sourceIndex == 2
            && owner.analogKeyStages[3].sourceIndex == 3
            && std::ranges::all_of(owner.analogKeyStages,
                [](const auto& stage) {
                    return stage.durationFrames == 1;
                }),
        "live key ranges prepare all stage indices in canonical ascending order");
    require(owner.analogExtraSequences.size() == 1
            && owner.analogGeneratedIndex == "CurrentStep"
            && prepared->eventCount() == 16,
        "live ranges reserve two note events and both ExtraValues outputs per possible stage");
    require(prepared->equivalent(*live.prepare()),
        "identical live ranges retain prepared source identity");
    require(live.prepare({16, 256})->eventCount() == 16,
        "the exact dynamic range capacity reservation fits its event limit");
    live.reject("event limit", {15, 256});

    auto timed = makeAnalogSequencerRateFixture();
    setExpression(timed, "startIndex", "1 ramp: 4 ms", 1.0);
    require(timed.prepare()->owners()[0].analogLiveRange,
        "a timed scalar start index receives the Sound clock");

    auto variable = makeAnalogSequencerRateFixture();
    variable.settings.preparedVariables.scopes = {{0, 0, {}, {}}};
    const auto addVariable = [&](std::string id, double initialValue) {
        talas::project::PreparedProjectVariableCell cell;
        cell.identity.ownerKind =
            talas::project::ProjectVariableOwnerKind::Project;
        cell.identity.variableId = std::move(id);
        cell.domain = talas::expr::ValueDomain::Scalar;
        cell.initialValue = initialValue;
        cell.scopeOrdinal = 0;
        variable.settings.preparedVariables.cells.push_back(std::move(cell));
    };
    addVariable("start", 1.0);
    addVariable("end", 3.0);
    setExpression(variable, "startIndex", "@start", 1.0);
    setExpression(variable, "endIndex", "@end", 3.0);
    std::ranges::find(variable.plan.steps[0].parameters,
        "startIndex", &ParameterValue::id)->variableCellIndices = {0};
    std::ranges::find(variable.plan.steps[0].parameters,
        "endIndex", &ParameterValue::id)->variableCellIndices = {1};
    require(variable.prepare()->owners()[0].analogLiveRange,
        "project-variable start and end indices receive the variable clock");

    auto legacy = live;
    std::ranges::find(legacy.plan.steps[0].parameters,
        "indexOrigin", &ParameterValue::id)->choice = "legacyOneBased";
    const auto legacyPrepared = legacy.prepare();
    const auto& legacyOwner = legacyPrepared->owners()[0];
    require(legacyOwner.analogLiveRange && legacyOwner.analogLegacyRange
            && legacyOwner.analogKeyStages.size() == 4
            && legacyOwner.analogKeyStages[0].sourceIndex == 0
            && legacyOwner.analogKeyStages[3].sourceIndex == 3
            && !legacyPrepared->equivalent(*prepared),
        "changing only the one-based origin remains visible in prepared identity");

    auto legacyUpperBound = legacy;
    std::ranges::find(legacyUpperBound.plan.steps[0].parameters,
        "endIndex", &ParameterValue::id)->value = 4.0;
    legacyUpperBound.settings.eventValues[2].value = 4.0;
    const auto legacyUpperBoundPrepared = legacyUpperBound.prepare();
    require(legacyUpperBoundPrepared->owners()[0].analogLegacyRange
            && legacyUpperBoundPrepared->owners()[0].analogKeyStages.size() == 4,
        "the inclusive upper bound remains valid in a legacy one-based range");
}

void testAnalogSequencerLiveRangeRejectsInvalidInitialControls()
{
    const auto setEventExpression = [](
        Fixture& fixture,
        std::string_view parameterId,
        std::string_view source,
        double initialValue) {
        auto& parameter = *std::ranges::find(
            fixture.plan.steps[0].parameters,
            parameterId,
            &ParameterValue::id);
        const auto parsed = talas::expr::parse(
            source,
            fixture.settings.expressionLanguage,
            fixture.settings.expressionDefinitions);
        require(parsed.ok(), "range control expression should parse");
        parameter.expression = *parsed.expression;
        parameter.value = initialValue;
    };
    const auto makeRange = [&]() {
        auto fixture = makeAnalogSequencerRateFixture();
        fixture.settings.eventValues.push_back(
            {"StartControl", 1.0, talas::expr::ValueDomain::Scalar});
        fixture.settings.eventValues.push_back(
            {"EndControl", 3.0, talas::expr::ValueDomain::Scalar});
        setEventExpression(fixture, "startIndex", "!StartControl", 1.0);
        setEventExpression(fixture, "endIndex", "!EndControl", 3.0);
        return fixture;
    };

    auto fractional = makeRange();
    std::ranges::find(fractional.plan.steps[0].parameters,
        "startIndex", &ParameterValue::id)->value = 1.5;
    fractional.settings.eventValues[1].value = 1.5;
    fractional.reject("finite selected range");

    auto nonFinite = makeRange();
    std::ranges::find(nonFinite.plan.steps[0].parameters,
        "endIndex", &ParameterValue::id)->value =
        std::numeric_limits<double>::infinity();
    nonFinite.settings.eventValues[2].value =
        std::numeric_limits<double>::infinity();
    nonFinite.reject("finite selected range");

    auto wrongDomain = makeRange();
    std::ranges::find(wrongDomain.plan.steps[0].parameters,
        "startIndex", &ParameterValue::id)->domain =
        talas::expr::ValueDomain::Hertz;
    wrongDomain.reject("finite selected range");

    auto outOfRange = makeRange();
    std::ranges::find(outOfRange.plan.steps[0].parameters,
        "endIndex", &ParameterValue::id)->value = 4.0;
    outOfRange.settings.eventValues[2].value = 4.0;
    outOfRange.reject("selected indices inside the sequence");

    auto reversed = makeRange();
    std::ranges::find(reversed.plan.steps[0].parameters,
        "startIndex", &ParameterValue::id)->value = 3.0;
    std::ranges::find(reversed.plan.steps[0].parameters,
        "endIndex", &ParameterValue::id)->value = 1.0;
    reversed.settings.eventValues[1].value = 3.0;
    reversed.settings.eventValues[2].value = 1.0;
    reversed.reject("selected indices inside the sequence");

    auto invalidLegacyOrigin = makeRange();
    std::ranges::find(invalidLegacyOrigin.plan.steps[0].parameters,
        "indexOrigin", &ParameterValue::id)->choice = "legacyOneBased";
    std::ranges::find(invalidLegacyOrigin.plan.steps[0].parameters,
        "startIndex", &ParameterValue::id)->value = 0.0;
    invalidLegacyOrigin.settings.eventValues[1].value = 0.0;
    invalidLegacyOrigin.reject("selected indices inside the sequence");

    for (const auto source : {
             std::string_view{"previous(1, 0)"},
             std::string_view{"[1, indexIdentifier][0]"}})
    {
        auto unsupported = makeRange();
        setEventExpression(unsupported, "startIndex", source, 1.0);
        unsupported.reject("EventValue/variable/time clock");
    }

    auto malformedForm = makeRange();
    std::ranges::find(malformedForm.plan.steps[0].parameters,
        "startIndex", &ParameterValue::id)->text = "!StartControl";
    malformedForm.reject("finite selected range");
}
}

int main()
{
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"source-selection", testSourceSelection},
        {"immutable-selected-streams", testSelectedMessagesAndImmutableOwners},
        {"hierarchical-source-identity", testHierarchicalSourceIdentity},
        {"default-file-controller-changes", testDefaultFileControllerChanges},
        {"nested-file-controller-binding-fanout", testNestedFileControllerBindingFanout},
        {"nested-optional-file-required-script-bindings", testNestedOptionalFileAndRequiredScriptBindings},
        {"nested-controller-fanout-capacity", testNestedControllerFanoutTransactionCapacity},
        {"source-arbitration", testInactiveResourceAndScriptArbitration},
        {"literal-script-preparation", testLiteralScriptPreparation},
        {"documented-smalltalk-note-loop", testDocumentedSmalltalkNoteLoop},
        {"markov-chain-script-plays-its-sequences", testMarkovChainScriptPlaysItsSequences},
        {"recursive-block-plays-a-self-similar-melody", testRecursiveBlockPlaysASelfSimilarMelody},
        {"timed-event-collections-from-sets-and-midi-files", testTimedEventCollectionsFromSetsAndMidiFiles},
        {"markov-chain-trains-on-a-midi-file", testMarkovChainTrainsOnAMidiFile},
        {"random-note-scripts-play-their-notes", testRandomNoteScriptsPlayTheirNotes},
        {"random-notes-slide-a-controller", testRandomNotesSlideAController},
        {"scripts-read-scores-from-text-files", testScriptsReadScoresFromTextFiles},
        {"documented-event-collections", testDocumentedEventCollections},
        {"sequences-made-of-themselves", testSequencesMadeOfThemselves},
        {"event-collections-play-their-notes", testEventCollectionsPlayTheirNotes},
        {"literal-script-controller-set", testLiteralScriptControllerSet},
        {"exact-resources", testExactResourceIdentity},
        {"aggregate-budgets", testAggregateBudgets},
        {"malformed-pattern-policy", testMalformedFileAndIndependentPatternPolicy},
        {"owner-bindings", testOwnerAndBindingValidation},
        {"analog-sequencer-key-schedule", testAnalogSequencerKeyScheduleRetainsEventVectors},
        {"analog-sequencer-short-arrays", testAnalogSequencerKeyShortArraysRepeatFinalValues},
        {"analog-sequencer-authored-arrays", testAnalogSequencerAuthoredArraysPrepareAsVoiceSource},
        {"analog-sequencer-clocked-arrays", testAnalogSequencerClockedArrayMembersRetainStateAndSoundFrame},
        {"analog-sequencer-bare-array-authoring", testAnalogSequencerBareAndMixedArrayAuthoring},
        {"analog-sequencer-zero-step-policy", testAnalogSequencerZeroStepPolicyPreparation},
        {"analog-sequencer-project-variable-arrays", testAnalogSequencerProjectVariableArraysBindAndSampleSnapshots},
        {"analog-sequencer-variable-reference-capacity", testAnalogSequencerVariableReferenceCapacityIncludesExistingBindings},
        {"analog-sequencer-live-rate", testAnalogSequencerLiveRatePreparation},
        {"analog-sequencer-live-range", testAnalogSequencerLiveRangePreparation},
        {"analog-sequencer-live-range-validation", testAnalogSequencerLiveRangeRejectsInvalidInitialControls},
    };
    std::size_t failures = 0;
    for (const auto& [name, run] : tests)
    {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
