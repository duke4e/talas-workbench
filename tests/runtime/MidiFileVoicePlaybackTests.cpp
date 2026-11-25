#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <tuple>

namespace { std::atomic<bool> countAllocations{false}; std::atomic<std::size_t> allocations{0}, deallocations{0}; }
void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept
{
    if (p && countAllocations.load(std::memory_order_relaxed)) deallocations.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

namespace
{
using namespace talas::runtime;
void require(bool value, std::string_view message)
{
    if (!value) throw std::runtime_error(std::string(message));
}
talas::project::ProjectParameterExpression text(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression p;
    p.parameterId = std::move(id); p.text = std::move(value); return p;
}
talas::project::ProjectParameterExpression choice(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression p;
    p.parameterId = std::move(id); p.choice = std::move(value); return p;
}
talas::project::ProjectDocument instrument(bool file)
{
    talas::project::ProjectDocument document;
    talas::project::ProjectParameterExpression resource;
    resource.parameterId = "midiFile"; resource.resource = file ? "notes" : "";
    talas::project::ProjectParameterExpression shared;
    shared.parameterId = "shared"; shared.boolean = false;
    document.graph.nodes = {
        {"osc", "talas.oscillator", {{"frequency", "!Pitch"}, {"amplitude", "(!KeyDown > 0) * !KeyVelocity"}}},
        {"voice", "talas.midiVoice", {{"left", "1"}, {"right", "0.5"},
            {"channel", "1"}, {"polyphony", "1"}, {"lowPitch", "0 nn"}, {"highPitch", "127 nn"},
            text("source", file ? "MIDI File" : ""), resource, text("script", ""), shared}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {{"osc", "audioOut", "voice", "audioIn"},
        {"voice", "leftOut", "out", "audioIn"}, {"voice", "rightOut", "out", "rightIn"}};
    return document;
}
talas::project::ProjectDocument polyphonicMapperInstrument(bool file)
{
    auto document = instrument(file);
    auto& mapper = document.graph.nodes[1];
    mapper.typeId = "talas.midiMapper";
    mapper.parameters.push_back(text("map", ""));
    talas::project::ProjectParameterExpression model;
    model.parameterId = "voiceModel";
    model.choice = "polyphonicInput";
    mapper.parameters.push_back(std::move(model));
    return document;
}
RenderSettings settings()
{
    RenderSettings s;
    s.sampleRate = 1000; s.maxBlockFrames = 64;
    s.eventValues = {{"Pitch",60,talas::expr::ValueDomain::NoteNumber}, {"KeyDown",0}, {"KeyVelocity",0}};
    // 500 PPQ at default tempo and 1000 Hz gives one sample per tick.
    s.binaryResources = {{"notes", {'M','T','h','d',0,0,0,6,0,0,0,1,1,0xf4,
        'M','T','r','k',0,0,0,20,
        0,0x90,60,100, 20,0x80,60,0, 10,0x90,64,80, 20,0x90,64,0, 0,0xff,0x2f,0}}};
    return s;
}
void track(RenderSettings& s, std::initializer_list<std::uint8_t> events)
{
    auto& bytes = s.binaryResources.front().bytes;
    bytes.resize(22);
    const auto length = events.size() + 4;
    for (std::size_t i = 0; i < 4; ++i) bytes[18+i] = static_cast<std::uint8_t>(length >> (24-8*i));
    bytes.insert(bytes.end(), events);
    bytes.insert(bytes.end(), {0,0xff,0x2f,0});
}
PreparedRuntimeGraph compile(const talas::project::ProjectDocument& document, const RenderSettings& s)
{
    auto result = compileProject(document, talas::core::makeBuiltinNodeRegistry(), s);
    require(result.ok(), result.diagnostics.empty() ? "compile" : result.diagnostics.front().message);
    return std::move(*result.graph);
}
std::vector<double> oracle()
{
    std::vector<double> result(64);
    double phase = 0;
    auto pitch = 60;
    for (std::size_t frame = 0; frame < result.size(); ++frame)
    {
        // This instrument has no oscillator reset input. Retrigger changes
        // KeyDown; phase continues until an explicit transport reset.
        if (frame == 35) pitch = 64;
        const auto frequency = 440 * std::pow(2.0, (pitch - 69) / 12.0);
        const auto gain = frame >= 5 && frame < 20 ? 100.0/127.0
            : frame >= 35 && frame < 50 ? 80.0/127.0 : 0;
        result[frame] = gain * std::sin(2 * std::numbers::pi * phase);
        phase += frequency / 1000;
        phase -= std::floor(phase);
    }
    return result;
}
void expect(std::span<const double> values, std::span<const double> expected, double gain = 1)
{
    require(values.size() == expected.size(), "PCM size");
    for (std::size_t i = 0; i < values.size(); ++i)
        require(std::abs(values[i] - expected[i] * gain) < 1e-10,
            "PCM mismatch at frame " + std::to_string(i) + ": " + std::to_string(values[i])
                + " expected " + std::to_string(expected[i] * gain));
}
void testFileVoiceAudio()
{
    const auto compiled = compileProject(instrument(true), talas::core::makeBuiltinNodeRegistry(), settings());
    require(compiled.ok(), compiled.diagnostics.empty() ? "file instrument compile" : compiled.diagnostics.front().message);
    RealtimeRenderSession prepared(*compiled.graph);
    require(prepared.ready(), prepared.error());
    require(validateOfflineRuntimeGraph(*compiled.graph).ok(), "public offline validation must select file backend");
    const auto expected = oracle();
    auto offline = OfflineRenderSession(*compiled.graph);
    const auto audio = offline.render(64);
    require(audio.ok(), audio.diagnostics.empty() ? "offline file render" : audio.diagnostics.front().message);
    expect(audio.mono, expected); expect(audio.right, expected, 0.5);
    offline.reset();
    const auto repeated = offline.render(64);
    require(repeated.ok(), "offline file reset"); expect(repeated.mono, expected);
    for (const std::size_t block : {1, 7, 64})
    {
        RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), session.error());
        require(session.scheduleMidiNote({RealtimeMidiNoteKind::NoteOn,1,72,127}, 0)
            == RealtimeMidiDispatchResult::NotHandled, "live notes must not reach a file-only owner");
        std::vector<double> left(64), right(64);
        for (std::size_t start = 0; start < 64; start += block)
        {
            const auto count = std::min(block, 64-start);
            require(session.render(std::span(left).subspan(start,count), std::span(right).subspan(start,count)),
                    "partitioned file voice render");
        }
        expect(left, expected); expect(right, expected, 0.5);
        session.resetTransport();
        require(session.render(left,right), "file voice reset render");
        expect(left, expected); expect(right, expected, 0.5);
    }
}

void testZeroVelocityNoteOnMatchesNoteOffAndRetainsAttackVelocity()
{
    constexpr std::size_t frames = 32;
    constexpr double attackVelocity = 100.0 / 127.0;
    std::array<double, frames> expected{};
    double phase = 0.0;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const double gate = frame < 5 ? -1.0 : frame < 20 ? 1.0 : 0.0;
        const double velocity = frame < 5 ? 0.0 : attackVelocity;
        expected[frame] = (velocity + gate * 0.1)
            * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + 7.0 / 1000.0, 1.0);
    }

    for (const bool mapper : {false, true})
        for (const bool zeroVelocityNoteOn : {false, true})
        {
            auto document = mapper
                ? polyphonicMapperInstrument(true) : instrument(true);
            document.graph.nodes[0].parameters[0].expression = "7 hz";
            document.graph.nodes[0].parameters[1].expression =
                "!KeyVelocity + (!KeyDown * 0.1)";
            auto renderSettings = settings();
            if (zeroVelocityNoteOn)
                track(renderSettings, {0,0x90,60,100, 20,0x90,60,0});
            else
                track(renderSettings, {0,0x90,60,100, 20,0x80,60,0});
            auto graph = compile(document, renderSettings);
            const auto offline = OfflineRenderSession(graph).render(frames);
            require(offline.ok(), "zero-velocity offline render");
            expect(offline.mono, expected);
            for (const std::size_t block : std::array<std::size_t, 3>{1, 7, frames})
            {
                RealtimeRenderSession session(graph);
                require(session.ready(), session.error());
                std::array<double, frames> left{}, right{};
                for (std::size_t start = 0; start < frames; start += block)
                {
                    const auto count = std::min(block, frames - start);
                    require(session.render(
                        std::span(left).subspan(start, count),
                        std::span(right).subspan(start, count)),
                        "zero-velocity partitioned render");
                }
                expect(left, expected);
            }
        }
}

void testMidiKeyboardSourcesMapPerVoiceWithDelayedAttack()
{
    auto document = polyphonicMapperInstrument(true);
    talas::project::ProjectSoundDefinition tone;
    tone.definitionId = "mappedKeyboardInput";
    tone.displayName = "Mapped Keyboard Input";
    tone.graph.nodes = {
        {"gateOsc", "talas.oscillator",
            {{"frequency", "7 hz"}, {"amplitude", "!LocalGate"}}},
        {"pitchOsc", "talas.oscillator",
            {{"frequency", "!LocalPitch"},
             {"amplitude", "!LocalVelocity * (!LocalGate > 0)"}}},
    };
    tone.outputs = {
        {"audioOut", talas::core::PortType::Audio,
            "gateOsc", "audioOut"},
        {"rightOut", talas::core::PortType::Audio,
            "pitchOsc", "audioOut"},
    };
    document.soundDefinitions.push_back(std::move(tone));
    talas::project::ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "mappedKeyboardInput";
    document.graph.nodes[0] = std::move(source);
    document.graph.connections[0] = {
        "source", "audioOut", "voice", "audioIn"};
    document.graph.connections.push_back({
        "source", "rightOut", "voice", "rightIn"});
    for (auto& parameter : document.graph.nodes[1].parameters)
    {
        if (parameter.parameterId == "polyphony")
            parameter.expression = "2";
        if (parameter.parameterId == "map")
            parameter.text =
                "!LocalGate is: `MIDIKeyDown\n"
                "!LocalPitch is: `MIDIKeyNumber\n"
                "!LocalVelocity is: `MIDIKeyVelocity";
    }
    auto renderSettings = settings();
    renderSettings.eventValues.push_back({"LocalGate", 0.0});
    renderSettings.eventValues.push_back({
        "LocalPitch", 60.0, talas::expr::ValueDomain::NoteNumber});
    renderSettings.eventValues.push_back({"LocalVelocity", 0.0});
    track(renderSettings, {0,0x90,60,64, 10,0x90,64,127,
        10,0x80,60,0, 10,0x80,64,0});
    const auto graph = compile(document, renderSettings);
    for (const auto name : {"MIDIKeyDown", "MIDIKeyNumber",
             "MIDIKeyVelocity"})
        require(std::ranges::count_if(
                    graph.renderSettings().eventValues,
                    [&](const auto& binding) {
                        return binding.name == name
                            && binding.scopeId != globalRuntimeScope
                            && !binding.projectionExpression;
                    }) == 2,
            "keyboard Event Source must have one binding per voice");

    std::array<double, 40> expectedLeft{}, expectedRight{};
    const auto hz60 = 440.0 * std::pow(2.0, (60.0 - 69.0) / 12.0);
    const auto hz64 = 440.0 * std::pow(2.0, (64.0 - 69.0) / 12.0);
    double gatePhase = 0.0;
    double firstPitchPhase = 0.0;
    double secondPitchPhase = 0.0;
    for (std::size_t frame = 0; frame < expectedLeft.size(); ++frame)
    {
        const auto firstGate = frame < 5 ? -1.0 : frame < 20 ? 1.0 : 0.0;
        const auto secondGate = frame < 10 ? 0.0
            : frame < 15 ? -1.0 : frame < 30 ? 1.0 : 0.0;
        expectedLeft[frame] = (firstGate + secondGate)
            * std::sin(2.0 * std::numbers::pi * gatePhase);
        if (firstGate > 0.0)
            expectedRight[frame] += 64.0 / 127.0
                * std::sin(2.0 * std::numbers::pi * firstPitchPhase);
        if (secondGate > 0.0)
            expectedRight[frame] +=
                std::sin(2.0 * std::numbers::pi * secondPitchPhase);
        expectedRight[frame] *= 0.5;
        gatePhase = std::fmod(gatePhase + 7.0 / 1000.0, 1.0);
        firstPitchPhase = std::fmod(
            firstPitchPhase + hz60 / 1000.0, 1.0);
        secondPitchPhase = std::fmod(
            secondPitchPhase + (frame < 15 ? hz60 : hz64) / 1000.0, 1.0);
    }
    const auto offline = OfflineRenderSession(graph).render(expectedLeft.size());
    require(offline.ok(), "keyboard source Map must render offline");
    expect(offline.mono, expectedLeft);
    expect(offline.right, expectedRight);
    for (const std::size_t block : {std::size_t{1}, std::size_t{7}, std::size_t{40}})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 40> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += block)
        {
            const auto count = std::min(block, left.size() - start);
            allocations.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            const auto rendered = session.render(
                std::span(left).subspan(start, count),
                std::span(right).subspan(start, count));
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered && allocations.load(std::memory_order_relaxed) == 0,
                "keyboard source Map callback must not allocate");
        }
        expect(left, expectedLeft);
        expect(right, expectedRight);
    }
    auto forged = graph;
    auto& forgedBindings = const_cast<RenderSettings&>(
        forged.renderSettings()).eventValues;
    const auto localGate = std::ranges::find_if(
        forgedBindings, [](const auto& binding) {
            return binding.name == "LocalGate"
                && binding.scopeId != globalRuntimeScope
                && binding.projectionExpression;
        });
    require(localGate != forgedBindings.end(),
        "keyboard Map must contain a local gate projection");
    localGate->sourceScopeId = globalRuntimeScope;
    RealtimeRenderSession forgedSession(forged);
    require(!forgedSession.ready(),
        "repointing a voice-local keyboard source to global must reject");
}

void testMidiFileDefaultController()
{
    std::vector<double> expected(40);
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        if (frame >= 5 && frame < 30)
            expected[frame] = std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + (frame < 10 ? 100.0 : 200.0) / 1000.0, 1.0);
    }
    for (const bool mapper : {false, true})
    {
        auto document = mapper ? polyphonicMapperInstrument(true) : instrument(true);
        document.graph.nodes[0].parameters[0].expression =
            "!cc18 * 100 hz + 100 hz";
        auto renderSettings = settings();
        track(renderSettings, {0,0x90,60,127, 10,0xb0,18,127, 20,0x80,60,0});
        renderSettings.eventValues.push_back({"cc18", 0});
        auto graph = compile(document, renderSettings);
        require(graph.renderSettings().eventValueChanges.size() == 1
            && graph.renderSettings().eventValueChanges[0].frame == 10
            && graph.renderSettings().eventValueChanges[0].value == 1.0,
            "file CC18 must enter the prepared normalized Event Value timeline");
        const auto offline = OfflineRenderSession(graph).render(40);
        require(offline.ok(), "file CC must render offline");
        expect(offline.mono, expected);
        expect(offline.right, expected, 0.5);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 40> left{}, right{};
        allocations.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        bool ok = true;
        for (std::size_t start = 0; start < left.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, left.size() - start);
            ok = session.render(std::span(left).subspan(start, count),
                std::span(right).subspan(start, count)) && ok;
        }
        countAllocations.store(false, std::memory_order_relaxed);
        require(ok && allocations.load(std::memory_order_relaxed) == 0,
            "file CC callback must be allocation-free");
        expect(left, expected);
        expect(right, expected, 0.5);
        if (!mapper) continue;
        const auto voiceScope = graph.plan().midiVoices.front().voices.front().scopeId;
        renderSettings.eventValues.push_back({"cc18", 0.5,
            talas::expr::ValueDomain::Scalar, voiceScope});
        auto localGraph = compile(document, renderSettings);
        std::vector<double> localExpected(40);
        double localPhase = 0.0;
        for (std::size_t frame = 0; frame < localExpected.size(); ++frame)
        {
            if (frame >= 5 && frame < 30)
                localExpected[frame] = std::sin(2.0 * std::numbers::pi * localPhase);
            localPhase = std::fmod(localPhase
                + (frame < 10 ? 150.0 : 200.0) / 1000.0, 1.0);
        }
        const auto localOffline = OfflineRenderSession(localGraph).render(40);
        require(localOffline.ok(), "local Mapper CC must render offline");
        expect(localOffline.mono, localExpected);
        expect(localOffline.right, localExpected, 0.5);
        RealtimeRenderSession localSession(localGraph);
        require(localSession.ready(), localSession.error());
        std::array<double, 40> localLeft{}, localRight{};
        for (std::size_t start = 0; start < localLeft.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, localLeft.size() - start);
            require(localSession.render(std::span(localLeft).subspan(start, count),
                std::span(localRight).subspan(start, count)),
                "local Mapper CC must render across prepared realtime blocks");
        }
        expect(localLeft, localExpected);
        expect(localRight, localExpected, 0.5);
    }
}

// ADR-0024: a file's controllers and pitch bend also set the default map
// names, !DamperPedal following controller 64 and !PitchBend the bend.
void testMidiFileDefaultMapNames()
{
    const auto play = [](std::string frequency, std::initializer_list<std::uint8_t> events,
                          std::string_view name, double before) {
        std::vector<double> expected(40);
        double phase = 0.0;
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
        {
            if (frame >= 5 && frame < 30)
                expected[frame] = std::sin(2.0 * std::numbers::pi * phase);
            phase = std::fmod(phase + (frame < 10 ? before : 200.0) / 1000.0, 1.0);
        }
        auto document = instrument(true);
        document.graph.nodes[0].parameters[0].expression = std::move(frequency);
        auto renderSettings = settings();
        track(renderSettings, events);
        auto graph = compile(document, renderSettings);
        const auto& changes = graph.renderSettings().eventValueChanges;
        require(changes.size() == 1 && changes[0].frame == 10 && changes[0].name == name
                && changes[0].value == 1.0,
            "the file's MIDI should set !" + std::string(name));
        const auto offline = OfflineRenderSession(graph).render(40);
        require(offline.ok(), "a default map name should render offline");
        expect(offline.mono, expected);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 40> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, left.size() - start);
            require(session.render(std::span(left).subspan(start, count),
                        std::span(right).subspan(start, count)),
                "a default map name should render in realtime");
        }
        expect(left, expected);
    };
    play("!DamperPedal * 100 hz + 100 hz", {0,0x90,60,127, 10,0xb0,64,127, 20,0x80,60,0},
        "DamperPedal", 100.0);
    // !PitchBend rests at 0 until the bend reaches its top.
    play("!PitchBend * 50 hz + 150 hz", {0,0x90,60,127, 10,0xe0,0x7f,0x7f, 20,0x80,60,0},
        "PitchBend", 150.0);
}

// ADR-0024 amendment: a file's MIDI sets the names of
// the project's own global map, scaled by their ranges, and no longer the
// Default map's.
void testMidiFileProjectGlobalMap()
{
    auto document = instrument(true);
    document.graph.nodes[0].parameters[0].expression = "!Brightness * 1 hz + !Volume * 1 hz + 100 hz";
    document.globalMap = "!Brightness is: (`MIDIController74 min: 100 max: 300).\n";
    auto renderSettings = settings();
    track(renderSettings, {0,0x90,60,127, 10,0xb0,74,127, 12,0xb0,7,127, 20,0x80,60,0});
    auto graph = compile(document, renderSettings);
    const auto& changes = graph.renderSettings().eventValueChanges;
    require(std::ranges::any_of(changes, [](const auto& change) {
                return change.frame == 10 && change.name == "Brightness" && change.value == 300.0;
            }),
        "controller 74 should set !Brightness within its range");
    require(std::ranges::none_of(changes, [](const auto& change) { return change.name == "Volume"; }),
        "the project's global map should replace the Default map's !Volume");
}

void testWaitUntilDefersMidiSourceClock()
{
    for (const bool script : {false, true})
    for (const bool mapper : {false, true})
    {
        auto document = mapper ? polyphonicMapperInstrument(!script) : instrument(!script);
        if (script)
        {
            document.graph.nodes[1].parameters[6].text = "Script";
            document.graph.nodes[1].parameters[8].text =
                "self keyDownAt: 0 s duration: 30 ms frequency: 60 nn. "
                "self controller: !cc18 setTo: 1 atTime: 10 ms.";
        }
        document.graph.nodes[0].parameters[0].expression = "100 hz + !cc18 * 100 hz";
        document.graph.nodes.push_back({"resume", "talas.constant", {{"value", "!Resume"}}});
        document.graph.nodes.push_back({"leftWait", "talas.waitUntil", {{"resume", "0"}}});
        document.graph.nodes.push_back({"rightWait", "talas.waitUntil", {{"resume", "0"}}});
        document.graph.connections[1] = {"voice", "leftOut", "leftWait", "audioIn"};
        document.graph.connections[2] = {"voice", "rightOut", "rightWait", "audioIn"};
        document.graph.connections.insert(document.graph.connections.end(), {
            {"resume", "value", "leftWait", "resume"}, {"resume", "value", "rightWait", "resume"},
            {"leftWait", "audioOut", "out", "audioIn"}, {"rightWait", "audioOut", "out", "rightIn"}});
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"Resume", 0}, {"cc18", 0}});
        s.eventValueChanges.push_back({15, "Resume", 1});
        track(s, {0,0x90,60,127, 10,0xb0,18,127, 20,0x80,60,0});
        const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
        require(reopened.ok(), "held MIDI source must survive save/reopen");
        const auto graph = compile(reopened.document, s);
        std::array<double, 56> expected{};
        double phase = 0;
        for (std::size_t frame = 15; frame < expected.size(); ++frame)
        {
            const auto local = frame - 15;
            if (local >= 5 && local < 30) expected[frame] = std::sin(2 * std::numbers::pi * phase);
            phase = std::fmod(phase + (local < 10 ? 100.0 : 200.0) / 1000.0, 1.0);
        }
        const auto offline = OfflineRenderSession(graph).render(expected.size());
        require(offline.ok(), "WaitUntil must defer a MIDI File owner's notes and controllers");
        expect(offline.mono, expected); expect(offline.right, expected, 0.5);
        for (const auto block : {1U, 7U, 56U})
        {
            RealtimeRenderSession session(graph);
            require(session.ready(), session.error());
            for (int pass = 0; pass < 2; ++pass)
            {
                std::array<double, 56> left{}, right{};
                allocations.store(0); countAllocations.store(true);
                bool rendered = true;
                for (std::size_t offset = 0; offset < left.size(); offset += block)
                {
                    const auto count = std::min<std::size_t>(block, left.size() - offset);
                    rendered &= session.render(std::span(left).subspan(offset, count),
                        std::span(right).subspan(offset, count));
                }
                countAllocations.store(false);
                require(rendered && allocations.load() == 0, "deferred MIDI source callback must not allocate");
                expect(left, expected); expect(right, expected, 0.5);
                allocations.store(0); countAllocations.store(true);
                session.resetTransport();
                countAllocations.store(false);
                require(allocations.load() == 0, "deferred MIDI source reset must not allocate");
            }
        }
        auto shared = document;
        std::erase_if(shared.graph.connections, [](const auto& edge) {
            return edge.fromNode == "rightWait" || edge.toNode == "rightWait";
        });
        std::erase_if(shared.graph.nodes, [](const auto& node) { return node.instanceId == "rightWait"; });
        shared.graph.connections.push_back({"voice", "rightOut", "out", "rightIn"});
        const auto sharedGraph = compile(shared, s);
        const auto sharedAudio = OfflineRenderSession(sharedGraph).render(expected.size());
        require(sharedAudio.ok(), "an active consumer starts the shared source despite another held edge");
        std::array<double, 56> unheld{}, held{};
        phase = 0;
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
        {
            if (frame >= 5 && frame < 30) unheld[frame] = std::sin(2 * std::numbers::pi * phase);
            if (frame >= 15) held[frame] = unheld[frame];
            phase = std::fmod(phase + (frame < 10 ? 100.0 : 200.0) / 1000.0, 1.0);
        }
        expect(sharedAudio.mono, held); expect(sharedAudio.right, unheld, 0.5);

        auto faultDocument = document;
        faultDocument.graph.nodes[0].parameters[1].expression =
            "(!KeyDown > 0) * !KeyVelocity / (1 - (!KeyDown > 0) * (!cc18 > 0) * !Fault)";
        auto faultSettings = s;
        faultSettings.eventValues.push_back({"Fault", 0});
        const auto faultGraph = compile(faultDocument, faultSettings);
        RealtimeRenderSession failed(faultGraph), reference(faultGraph);
        require(failed.ready() && reference.ready() && failed.scheduleEventValue("Fault", 1,
                talas::expr::ValueDomain::Scalar, 24), "fault arms before the deferred CC reaches the active voice");
        std::array<double, 56> left{}, right{}, referenceLeft{}, referenceRight{};
        allocations.store(0); countAllocations.store(true);
        const auto rendered = failed.render(left, right);
        countAllocations.store(false);
        require(!rendered, "late deferred-source fault must fail the callback");
        require(allocations.load() == 0, "deferred-source failure allocated " + std::to_string(allocations.load()));
        require(std::ranges::all_of(left, [](double value) { return value == 0; })
                && std::ranges::all_of(right, [](double value) { return value == 0; }),
            "deferred notes, controls, gate and DSP roll back together without allocation");
        require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
                && reference.render(referenceLeft, referenceRight), "failed source can be retried after correction");
        allocations.store(0); countAllocations.store(true);
        const auto recovered = failed.render(left, right);
        countAllocations.store(false);
        require(recovered && allocations.load() == 0, "deferred source retry must not allocate");
        expect(left, expected); expect(right, expected, 0.5);
        expect(left, referenceLeft); expect(right, referenceRight);

        RealtimeRenderSession firstAtLaterFrame(graph);
        firstAtLaterFrame.setTransportFrameBeforeFirstRender(100);
        require(firstAtLaterFrame.render(left, right), "unstarted source can activate at a nonzero host frame");
        expect(left, unheld); expect(right, unheld, 0.5);

        auto repairDocument = document;
        auto repairSettings = s;
        if (script)
            *repairDocument.graph.nodes[1].parameters[8].text +=
                " self keyDownAt: 40 ms duration: 20 ms frequency: 64 nn.";
        else
            track(repairSettings, {0,0x90,60,127, 10,0xb0,18,127, 20,0x80,60,0,
                10,0x90,64,127, 20,0x80,64,0});
        const auto repairGraph = compile(repairDocument, repairSettings);
        RealtimeRenderSession repaired(repairGraph);
        std::array<double, 24> prefixLeft{}, prefixRight{};
        require(repaired.render(prefixLeft, prefixRight), "render before skipping an activated source interval");
        std::array<double, 40> repairLeft{}, repairRight{}, repairExpected{};
        phase = 0.9; // Nine oscillator frames ran after opening at sample 15.
        for (std::size_t frame = 0; frame < repairExpected.size(); ++frame)
        {
            if (frame >= 20 && frame < 35) repairExpected[frame] = std::sin(2 * std::numbers::pi * phase);
            phase = std::fmod(phase + 0.2, 1.0);
        }
        allocations.store(0); countAllocations.store(true);
        repaired.synchronizeTransportFrame(40);
        const auto repairRendered = repaired.render(repairLeft, repairRight);
        countAllocations.store(false);
        require(repairRendered && allocations.load() == 0,
            "forward repair preserves source origin and skipped controller state without allocation");
        expect(repairLeft, repairExpected); expect(repairRight, repairExpected, 0.5);
    }
}

void testMidiFileBuiltInControllerBinding()
{
    std::vector<double> expected(40);
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        if (frame >= 5 && frame < 30)
            expected[frame] = std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + 200.0 / 1000.0, 1.0);
    }
    for (const std::uint8_t controller : {std::uint8_t{0}, std::uint8_t{18},
             std::uint8_t{127}})
    for (const auto* prefix : {"cc", "bt", "sw"})
    for (const bool mapper : {false, true})
    {
        std::string name = prefix;
        if (controller < 10) name += '0';
        name += std::to_string(controller);
        auto document = mapper ? polyphonicMapperInstrument(true) : instrument(true);
        document.graph.nodes[0].parameters[0].expression =
            "!" + name + " * 100 hz + 100 hz";
        auto renderSettings = settings();
        track(renderSettings, {0,0xb0,controller,127, 0,0x90,60,127, 30,0x80,60,0});
        const auto graph = compile(document, renderSettings);
        require(std::ranges::any_of(graph.renderSettings().eventValues,
            [&](const auto& binding) { return binding.name == name; }),
            "referenced built-in MIDI alias must resolve without a caller-authored binding");
        require(std::ranges::any_of(
                    graph.renderSettings().eventValueChanges,
                    [&](const auto& change) {
                        return change.name == name && change.frame == 0
                            && change.value == 1.0;
                    }),
            "MIDI File CC must feed the corresponding built-in alias");
        const auto offline = OfflineRenderSession(graph).render(40);
        require(offline.ok(), "built-in CC binding renders offline");
        expect(offline.mono, expected);
        expect(offline.right, expected, 0.5);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 40> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, left.size() - start);
            require(session.render(std::span(left).subspan(start, count),
                std::span(right).subspan(start, count)),
                "built-in CC binding renders across prepared realtime blocks");
        }
        expect(left, expected);
        expect(right, expected, 0.5);
    }
}

void testMidiFileDirectLocalMapController()
{
    auto document = polyphonicMapperInstrument(true);
    talas::project::ProjectSoundDefinition tone;
    tone.definitionId = "fileMappedControllerInput";
    tone.displayName = "File Mapped Controller Input";
    tone.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "100 hz"},
         {"amplitude", "!LocalLevel * (!KeyDown > 0)"}}}};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    talas::project::ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "fileMappedControllerInput";
    document.graph.nodes[0] = std::move(source);
    document.graph.connections[0].fromNode = "source";
    auto& parameters = document.graph.nodes[1].parameters;
    const auto map = std::ranges::find(parameters, "map",
        &talas::project::ProjectParameterExpression::parameterId);
    require(map != parameters.end(), "Mapper fixture must contain Map");
    map->text = "!LocalLevel is: `MIDIController18";

    auto renderSettings = settings();
    renderSettings.eventValues.push_back({"LocalLevel", 0.25});
    track(renderSettings, {0,0x90,60,127, 10,0xb0,18,127,
        20,0x80,60,0});
    const auto graph = compile(document, renderSettings);
    require(std::ranges::any_of(graph.renderSettings().eventValues,
                [](const auto& binding) {
                    return binding.name == "cc18"
                        && binding.scopeId == globalRuntimeScope;
                }),
        "MIDI File local Map must acquire implicit global cc18");
    require(graph.renderSettings().eventValueChanges.size() == 1
            && graph.renderSettings().eventValueChanges.front().frame == 10
            && graph.renderSettings().eventValueChanges.front().name == "cc18"
            && graph.renderSettings().eventValueChanges.front().value == 1.0,
        "MIDI File CC18 must reach the direct local Map through global cc18");

    std::vector<double> expected(40);
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        if (frame >= 10 && frame < 30)
            expected[frame] = std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + 0.1, 1.0);
    }
    const auto offline = OfflineRenderSession(graph).render(40);
    require(offline.ok(), "MIDI File direct local Map must render offline");
    expect(offline.mono, expected);
    expect(offline.right, expected, 0.5);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 40> left{}, right{};
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min(std::size_t{7}, left.size() - start);
        require(session.render(std::span(left).subspan(start, count),
                    std::span(right).subspan(start, count)),
            "MIDI File direct local Map must render across realtime blocks");
    }
    expect(left, expected);
    expect(right, expected, 0.5);

    map->text = "!LocalLevel is: (`MIDIController18 min: 0.2 max: 0.8)";
    const auto rangedGraph = compile(document, renderSettings);
    std::vector<double> rangedExpected(40);
    phase = 0.0;
    for (std::size_t frame = 0; frame < rangedExpected.size(); ++frame)
    {
        if (frame >= 5 && frame < 30)
            rangedExpected[frame] = (frame < 10 ? 0.2 : 0.8)
                * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + 0.1, 1.0);
    }
    const auto rangedOffline = OfflineRenderSession(rangedGraph).render(40);
    require(rangedOffline.ok(), "ranged MIDI File Map must render offline");
    expect(rangedOffline.mono, rangedExpected);
    expect(rangedOffline.right, rangedExpected, 0.5);
    RealtimeRenderSession rangedSession(rangedGraph);
    require(rangedSession.ready(), rangedSession.error());
    left.fill(0.0);
    right.fill(0.0);
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min(std::size_t{7}, left.size() - start);
        require(rangedSession.render(std::span(left).subspan(start, count),
                    std::span(right).subspan(start, count)),
            "ranged MIDI File Map must render across realtime blocks");
    }
    expect(left, rangedExpected);
    expect(right, rangedExpected, 0.5);

    map->text = "!LocalLevel is: (`MIDIController07 channel: 5)\n"
        "!DefaultLevel is: `MIDIController07";
    for (auto& parameter : document.graph.nodes[1].parameters)
        if (parameter.parameterId == "channel")
            parameter.expression = "2";
    for (auto& parameter :
         document.soundDefinitions[0].graph.nodes[0].parameters)
        if (parameter.parameterId == "amplitude")
            parameter.expression =
                "(!LocalLevel + !DefaultLevel) * (!KeyDown > 0) / 2";
    auto taggedSettings = renderSettings;
    taggedSettings.defaultMidiChannel = 2;
    std::ranges::find(taggedSettings.eventValues, "LocalLevel",
        &EventValueBinding::name)->value = 0.0;
    taggedSettings.eventValues.push_back({"DefaultLevel", 0.0});
    track(taggedSettings, {
        0, 0x91, 60, 127,
        5, 0x94, 72, 127,
        5, 0xb1, 7, 127,
        5, 0xb5, 7, 127,
        5, 0xb4, 7, 127,
        10, 0x81, 60, 0,
    });
    const auto taggedGraph = compile(document, taggedSettings);
    const auto& changes = taggedGraph.renderSettings().eventValueChanges;
    require(changes.size() == 2
            && changes[0].frame == 10 && changes[0].name == "cc07"
            && changes[1].frame == 20
            && changes[1].name == "cc07_ch05",
        "MIDI File must route only the default and tagged CC7 channels");
    std::vector<double> taggedExpected(40);
    phase = 0.0;
    for (std::size_t frame = 0; frame < taggedExpected.size(); ++frame)
    {
        if (frame >= 10 && frame < 30)
            taggedExpected[frame] = (frame < 20 ? 0.5 : 1.0)
                * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + 0.1, 1.0);
    }
    const auto taggedOffline = OfflineRenderSession(taggedGraph).render(40);
    require(taggedOffline.ok(),
        "channel-tagged MIDI File Map must render offline");
    expect(taggedOffline.mono, taggedExpected);
    expect(taggedOffline.right, taggedExpected, 0.5);
    RealtimeRenderSession taggedSession(taggedGraph);
    require(taggedSession.ready(), taggedSession.error());
    left.fill(0.0);
    right.fill(0.0);
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min(std::size_t{7}, left.size() - start);
        allocations.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = taggedSession.render(
            std::span(left).subspan(start, count),
            std::span(right).subspan(start, count));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocations.load(std::memory_order_relaxed) == 0,
            "channel-tagged MIDI File callback must render without allocation");
    }
    expect(left, taggedExpected);
    expect(right, taggedExpected, 0.5);

    map->text =
        "!LocalLevel is: (`MIDIController07 channel: 5; min: 0.2 max: 0.8)\n"
        "!DefaultLevel is: `MIDIController07";
    const auto scaledGraph = compile(document, taggedSettings);
    std::vector<double> scaledExpected(40);
    phase = 0.0;
    for (std::size_t frame = 0; frame < scaledExpected.size(); ++frame)
    {
        const auto gain = frame < 5 || frame >= 30 ? 0.0
            : frame < 10 ? 0.1 : frame < 20 ? 0.6 : 0.9;
        scaledExpected[frame] = gain
            * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + 0.1, 1.0);
    }
    const auto scaledOffline = OfflineRenderSession(scaledGraph).render(40);
    require(scaledOffline.ok(),
        "channel and range MIDI File Map must render offline");
    expect(scaledOffline.mono, scaledExpected);
    expect(scaledOffline.right, scaledExpected, 0.5);
    RealtimeRenderSession scaledSession(scaledGraph);
    require(scaledSession.ready(), scaledSession.error());
    left.fill(0.0);
    right.fill(0.0);
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min(std::size_t{7}, left.size() - start);
        require(scaledSession.render(std::span(left).subspan(start, count),
                    std::span(right).subspan(start, count)),
            "channel and range MIDI File Map must render across blocks");
    }
    expect(left, scaledExpected);
    expect(right, scaledExpected, 0.5);
}

void testMidiFileProgramNumberUsesOneBasedLocalMap()
{
    auto document = polyphonicMapperInstrument(true);
    talas::project::ProjectSoundDefinition tone;
    tone.definitionId = "fileProgramInput";
    tone.displayName = "File Program Input";
    tone.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "100 hz"},
         {"amplitude", "!LocalProgram / 128 * (!KeyDown > 0)"}}}};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    talas::project::ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "fileProgramInput";
    document.graph.nodes[0] = std::move(source);
    document.graph.connections[0].fromNode = "source";
    auto& parameters = document.graph.nodes[1].parameters;
    const auto map = std::ranges::find(parameters, "map",
        &talas::project::ProjectParameterExpression::parameterId);
    require(map != parameters.end(), "program Mapper fixture must contain Map");
    map->text = "!LocalProgram is: `MIDIProgramNumber";

    auto renderSettings = settings();
    renderSettings.eventValues.push_back({"LocalProgram", 0.0});
    track(renderSettings, {0,0x90,60,127, 10,0xc1,127,
        0,0xc0,0, 10,0xc0,127, 10,0x80,60,0});
    const auto graph = compile(document, renderSettings);
    require(std::ranges::any_of(graph.renderSettings().eventValues,
                [](const auto& binding) {
                    return binding.name == "MIDIProgramNumber"
                        && binding.scopeId == globalRuntimeScope;
                }),
        "MIDI program Map must prepare its source binding");
    const auto& changes = graph.renderSettings().eventValueChanges;
    require(changes.size() == 2
            && changes[0].frame == 10 && changes[0].value == 1.0
            && changes[1].frame == 20 && changes[1].value == 128.0,
        "MIDI file program bytes must become one-based Event Values");

    std::array<double, 40> expected{};
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        const auto gain = frame < 10 || frame >= 30 ? 0.0
            : frame < 20 ? 1.0 / 128.0 : 1.0;
        expected[frame] = gain
            * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + 0.1, 1.0);
    }
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), "MIDI file program Map must render offline");
    expect(offline.mono, expected);
    expect(offline.right, expected, 0.5);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 40> left{}, right{};
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min(std::size_t{7}, left.size() - start);
        allocations.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            std::span(left).subspan(start, count),
            std::span(right).subspan(start, count));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocations.load(std::memory_order_relaxed) == 0,
            "MIDI file program Map callback must not allocate");
    }
    expect(left, expected);
    expect(right, expected, 0.5);
}

void testMidiFileChannelPressureNormalizesLocalMap()
{
    auto document = polyphonicMapperInstrument(true);
    talas::project::ProjectSoundDefinition tone;
    tone.definitionId = "filePressureInput";
    tone.displayName = "File Pressure Input";
    tone.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "100 hz"},
         {"amplitude", "!LocalPressure * (!KeyDown > 0)"}}}};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    talas::project::ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "filePressureInput";
    document.graph.nodes[0] = std::move(source);
    document.graph.connections[0].fromNode = "source";
    auto& parameters = document.graph.nodes[1].parameters;
    const auto map = std::ranges::find(parameters, "map",
        &talas::project::ProjectParameterExpression::parameterId);
    require(map != parameters.end(), "pressure Mapper fixture must contain Map");
    map->text = "!LocalPressure is: `MIDIChannelPressure";

    auto renderSettings = settings();
    renderSettings.eventValues.push_back({"LocalPressure", 0.0});
    track(renderSettings, {0,0x90,60,127, 10,0xd0,0,
        10,0xd0,64, 5,0xd1,127, 0,0xd0,127, 5,0x80,60,0});
    const auto graph = compile(document, renderSettings);
    require(std::ranges::any_of(graph.renderSettings().eventValues,
                [](const auto& binding) {
                    return binding.name == "MIDIChannelPressure"
                        && binding.scopeId == globalRuntimeScope;
                }),
        "MIDI channel-pressure Map must prepare its source binding");
    const auto& changes = graph.renderSettings().eventValueChanges;
    require(changes.size() == 3
            && changes[0].frame == 10 && changes[0].value == 0.0
            && changes[1].frame == 20
            && std::abs(changes[1].value - 64.0 / 127.0) < 1.0e-12
            && changes[2].frame == 25 && changes[2].value == 1.0,
        "MIDI file pressure bytes must normalize to scalar Event Values");

    std::array<double, 40> expected{};
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        const auto gain = frame < 20 || frame >= 30 ? 0.0
            : frame < 25 ? 64.0 / 127.0 : 1.0;
        expected[frame] = gain
            * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + 0.1, 1.0);
    }
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), "MIDI file pressure Map must render offline");
    expect(offline.mono, expected);
    expect(offline.right, expected, 0.5);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 40> left{}, right{};
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min(std::size_t{7}, left.size() - start);
        allocations.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            std::span(left).subspan(start, count),
            std::span(right).subspan(start, count));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocations.load(std::memory_order_relaxed) == 0,
            "MIDI file pressure Map callback must not allocate");
    }
    expect(left, expected);
    expect(right, expected, 0.5);
}

void testMidiFilePitchBendNormalizesLocalMap()
{
    auto document = polyphonicMapperInstrument(true);
    talas::project::ProjectSoundDefinition tone;
    tone.definitionId = "fileBendInput";
    tone.displayName = "File Bend Input";
    tone.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "100 hz"},
         {"amplitude", "!LocalBend * (!KeyDown > 0)"}}}};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    talas::project::ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "fileBendInput";
    document.graph.nodes[0] = std::move(source);
    document.graph.connections[0].fromNode = "source";
    auto& parameters = document.graph.nodes[1].parameters;
    const auto map = std::ranges::find(parameters, "map",
        &talas::project::ProjectParameterExpression::parameterId);
    require(map != parameters.end(), "bend Mapper fixture must contain Map");
    map->text = "!LocalBend is: `MIDIPitchBend";

    auto renderSettings = settings();
    renderSettings.eventValues.push_back({"LocalBend", 0.0});
    track(renderSettings, {0,0x90,60,127,
        10,0xe0,0,0, 5,0xe1,127,127,
        5,0xe0,0,64, 10,0xe0,127,127,
        10,0x80,60,0});
    const auto graph = compile(document, renderSettings);
    require(std::ranges::any_of(graph.renderSettings().eventValues,
                [](const auto& binding) {
                    return binding.name == "MIDIPitchBend"
                        && binding.scopeId == globalRuntimeScope;
                }),
        "MIDI bend Map must prepare its source binding");
    const auto& changes = graph.renderSettings().eventValueChanges;
    require(changes.size() == 3
            && changes[0].frame == 10 && changes[0].value == -1.0
            && changes[1].frame == 20 && changes[1].value == 0.0
            && changes[2].frame == 30 && changes[2].value == 1.0,
        "MIDI file bend bytes must preserve negative, center and positive endpoints");

    std::array<double, 50> expected{};
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        const auto gain = frame >= 10 && frame < 20 ? -1.0
            : frame >= 30 && frame < 40 ? 1.0 : 0.0;
        expected[frame] = gain
            * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + 0.1, 1.0);
    }
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), "MIDI file bend Map must render offline");
    expect(offline.mono, expected);
    expect(offline.right, expected, 0.5);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 50> left{}, right{};
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min(std::size_t{7}, left.size() - start);
        allocations.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            std::span(left).subspan(start, count),
            std::span(right).subspan(start, count));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocations.load(std::memory_order_relaxed) == 0,
            "MIDI file bend Map callback must not allocate");
    }
    expect(left, expected);
    expect(right, expected, 0.5);
}

void testMidiMapperCombinesVoiceKeyNumberAndSharedBend()
{
    auto document = polyphonicMapperInstrument(true);
    talas::project::ProjectSoundDefinition tone;
    tone.definitionId = "bentKeyboardInput";
    tone.displayName = "Bent Keyboard Input";
    tone.graph.nodes = {{"osc", "talas.oscillator",
        {{"frequency", "!Pitch"},
         {"amplitude", "(!KeyDown > 0)"}}}};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio,
        "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(tone));
    talas::project::ProjectNode source;
    source.instanceId = "source";
    source.soundDefinitionId = "bentKeyboardInput";
    document.graph.nodes[0] = std::move(source);
    document.graph.connections[0].fromNode = "source";
    for (auto& parameter : document.graph.nodes[1].parameters)
    {
        if (parameter.parameterId == "polyphony")
            parameter.expression = "2";
    }
    auto& parameters = document.graph.nodes[1].parameters;
    const auto map = std::ranges::find(parameters, "map",
        &talas::project::ProjectParameterExpression::parameterId);
    require(map != parameters.end(), "bend composition needs Mapper Map");

    auto renderSettings = settings();
    track(renderSettings, {0,0x90,60,127,
        10,0xe0,127,127, 10,0x90,64,127,
        10,0xe0,0,0, 10,0x80,60,0,
        10,0x80,64,0});
    for (const auto [mapText, semitones] : {
             std::pair{
                 "!Pitch is: (`MIDIKeyNumber + (`MIDIPitchBend displayAs: #nothing)) nn.",
                 1.0},
             std::pair{
                 "!Pitch is: (`MIDIKeyNumber + (12 * `MIDIPitchBend)) nn.",
                 12.0}})
    {
        map->text = mapText;
        const auto graph = compile(document, renderSettings);
        const auto& bindings = graph.renderSettings().eventValues;
        require(std::ranges::count_if(bindings,
                    [](const auto& binding) {
                        return binding.name == "Pitch"
                            && binding.scopeId != globalRuntimeScope
                            && binding.projectionExpression.has_value();
                    }) == 2
                && std::ranges::any_of(bindings,
                    [](const auto& binding) {
                        return binding.name == "MIDIPitchBend"
                            && binding.scopeId == globalRuntimeScope
                            && binding.domain == talas::expr::ValueDomain::Scalar;
                    }),
            "each voice needs its own Pitch projection and one shared bend source");
        const auto& changes = graph.renderSettings().eventValueChanges;
        require(changes.size() == 2
                && changes[0].frame == 10
                && changes[0].name == "MIDIPitchBend"
                && changes[0].value == 1.0
                && changes[1].frame == 30
                && changes[1].value == -1.0,
            "MIDI File bend must update the shared source without a new note");

        auto forged = graph;
        auto& forgedEvents = const_cast<talas::runtime::RenderSettings&>(
            forged.renderSettings()).eventValues;
        std::erase_if(forgedEvents, [](const auto& binding) {
            return binding.name == "MIDIPitchBend"
                && binding.scopeId == globalRuntimeScope;
        });
        RealtimeRenderSession rejected(forged);
        require(!rejected.ready()
                && !OfflineRenderSession(forged).render(4).ok(),
            "both render paths must reject a composed Map without its shared bend binding");

        std::array<double, 60> expected{};
        double firstPhase = 0.0;
        double secondPhase = 0.0;
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
        {
            const auto bend = frame < 10 ? 0.0
                : frame < 30 ? 1.0 : -1.0;
            const auto firstPitch = 60.0 + semitones * bend;
            const auto secondPitch = (frame < 25 ? 60.0 : 64.0)
                + semitones * bend;
            if (frame >= 5 && frame < 40)
                expected[frame] += std::sin(
                    2.0 * std::numbers::pi * firstPhase);
            if (frame >= 25 && frame < 50)
                expected[frame] += std::sin(
                    2.0 * std::numbers::pi * secondPhase);
            firstPhase = std::fmod(firstPhase
                + 440.0 * std::pow(2.0, (firstPitch - 69.0) / 12.0)
                    / 1000.0, 1.0);
            secondPhase = std::fmod(secondPhase
                + 440.0 * std::pow(2.0, (secondPitch - 69.0) / 12.0)
                    / 1000.0, 1.0);
        }
        const auto offline = OfflineRenderSession(graph).render(expected.size());
        require(offline.ok(), "composed pitch Map must render offline");
        expect(offline.mono, expected);
        expect(offline.right, expected, 0.5);
        for (const auto block : {std::size_t{1}, std::size_t{7},
                 std::size_t{60}})
        {
            RealtimeRenderSession session(graph);
            require(session.ready(), session.error());
            std::array<double, 60> left{}, right{};
            for (std::size_t start = 0; start < left.size(); start += block)
            {
                const auto count = std::min(block, left.size() - start);
                allocations.store(0, std::memory_order_relaxed);
                countAllocations.store(true, std::memory_order_relaxed);
                const auto rendered = session.render(
                    std::span(left).subspan(start, count),
                    std::span(right).subspan(start, count));
                countAllocations.store(false, std::memory_order_relaxed);
                require(rendered && allocations.load(std::memory_order_relaxed) == 0,
                    "composed pitch Map callback must not allocate");
            }
            expect(left, expected);
            expect(right, expected, 0.5);
        }
    }
}

void testPolyphonicMapperFileSourceAudio()
{
    const auto document = polyphonicMapperInstrument(true);
    auto graph = compile(document, settings());
    require(graph.plan().midiVoices.size() == 1
            && graph.plan().midiMappers.empty(),
        "file-backed Mapper must use one prepared Input voice owner");
    const auto validation = validateOfflineRuntimeGraph(graph);
    require(validation.ok(), validation.diagnostics.empty()
        ? "public offline validation must select Mapper's MIDI file source"
        : validation.diagnostics.front().message);
    const auto expected = oracle();
    auto offline = OfflineRenderSession(graph);
    const auto rendered = offline.render(64);
    require(rendered.ok(), "Mapper MIDI file must render offline");
    expect(rendered.mono, expected);
    expect(rendered.right, expected, 0.5);
    offline.reset();
    const auto replay = offline.render(64);
    require(replay.ok(), "Mapper MIDI file must replay after reset");
    expect(replay.mono, expected);

    for (const auto block : {std::size_t{1}, std::size_t{7}, std::size_t{64}})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        require(session.scheduleMidiNote(
                    {RealtimeMidiNoteKind::NoteOn, 1, 72, 127}, 0)
                == RealtimeMidiDispatchResult::NotHandled,
            "live notes must not enter a file-only Mapper");
        std::array<double, 64> left{}, right{};
        allocations.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        bool ok = true;
        for (std::size_t start = 0; start < left.size(); start += block)
        {
            const auto count = std::min(block, left.size() - start);
            ok = session.render(std::span(left).subspan(start, count),
                     std::span(right).subspan(start, count)) && ok;
        }
        countAllocations.store(false, std::memory_order_relaxed);
        require(ok && allocations.load(std::memory_order_relaxed) == 0,
            "Mapper MIDI file callback must be allocation-free");
        expect(left, expected);
        expect(right, expected, 0.5);
    }
}

void testMidiVoiceAttenuationEndpoints()
{
    const auto expected = oracle();
    for (const auto [leftGain, rightGain] :
         std::array{std::pair{-1.0, 1.0}, std::pair{0.0, 1.0},
                    std::pair{1.0, 0.0}, std::pair{1.0, -1.0}})
    {
        auto document = instrument(true);
        document.graph.nodes[1].parameters[0].expression =
            std::to_string(leftGain);
        document.graph.nodes[1].parameters[1].expression =
            std::to_string(rightGain);
        auto graph = compile(document, settings());
        const auto offline = OfflineRenderSession(graph).render(64);
        require(offline.ok(), "MIDI Voice endpoint offline render");
        expect(offline.mono, expected, leftGain);
        expect(offline.right, expected, rightGain);

        RealtimeRenderSession realtime(graph);
        require(realtime.ready(), realtime.error());
        std::array<double, 64> left{}, right{};
        allocations.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        bool rendered = true;
        for (std::size_t start = 0; start < left.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, left.size() - start);
            rendered = realtime.render(std::span(left).subspan(start, count),
                                       std::span(right).subspan(start, count))
                && rendered;
        }
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered && allocations.load(std::memory_order_relaxed) == 0,
                "MIDI Voice endpoint realtime render must be allocation-free");
        expect(left, expected, leftGain);
        expect(right, expected, rightGain);
    }
}

void testMidiVoiceConnectedGainModulation()
{
    auto document = instrument(true);
    document.graph.nodes[1].parameters[0].expression = "1";
    document.graph.nodes[1].parameters[1].expression = "-1";
    const auto addGainTable = [&](std::string nodeId, std::string resourceId) {
        talas::project::ProjectParameterExpression resource;
        resource.parameterId = "wavetable";
        resource.resource = std::move(resourceId);
        talas::project::ProjectParameterExpression ignoreLoops;
        ignoreLoops.parameterId = "ignoreLoops";
        ignoreLoops.boolean = true;
        talas::project::ProjectParameterExpression fromMemoryWriter;
        fromMemoryWriter.parameterId = "fromMemoryWriter";
        fromMemoryWriter.boolean = false;
        document.graph.nodes.push_back({
            std::move(nodeId), "talas.triggeredTableRead",
            {{"trigger", "1"}, resource, {"gate", "1"},
             ignoreLoops, fromMemoryWriter}});
    };
    addGainTable("leftGain", "Tables/LeftGain");
    addGainTable("rightGain", "Tables/RightGain");
    document.graph.connections.push_back({"leftGain", "value", "voice", "left"});
    document.graph.connections.push_back({"rightGain", "value", "voice", "right"});

    auto renderSettings = settings();
    const std::array leftPattern{-1.0, 0.0, 0.5, 1.0};
    const std::array rightPattern{1.0, 0.25, -0.5, 0.0};
    std::vector<double> leftGains(64), rightGains(64);
    std::vector<double> expectedLeft(64), expectedRight(64);
    const auto input = oracle();
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        leftGains[frame] = leftPattern[(frame / 4) % leftPattern.size()];
        rightGains[frame] = rightPattern[(frame / 5) % rightPattern.size()];
        expectedLeft[frame] = input[frame] * leftGains[frame];
        expectedRight[frame] = input[frame] * rightGains[frame];
    }
    renderSettings.tableResources.push_back({"Tables/LeftGain", leftGains});
    renderSettings.tableResources.push_back({"Tables/RightGain", rightGains});
    auto graph = compile(document, renderSettings);
    const auto offline = OfflineRenderSession(graph).render(64);
    require(offline.ok(), offline.diagnostics.empty()
        ? "MIDI Voice connected-gain offline render"
        : offline.diagnostics.front().message);
    expect(offline.mono, expectedLeft);
    expect(offline.right, expectedRight);

    RealtimeRenderSession realtime(graph);
    require(realtime.ready(), realtime.error());
    std::array<double, 64> left{}, right{};
    allocations.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    bool rendered = true;
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min(std::size_t{7}, left.size() - start);
        rendered = realtime.render(std::span(left).subspan(start, count),
                                   std::span(right).subspan(start, count))
            && rendered;
    }
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocations.load(std::memory_order_relaxed) == 0,
            "MIDI Voice connected-gain realtime render must be allocation-free");
    expect(left, expectedLeft);
    expect(right, expectedRight);
}

void testMidiVoiceStereoInputRouting()
{
    auto document = instrument(true);
    document.graph.nodes[0].parameters[0].expression = "7 hz";
    document.graph.nodes[1].parameters[1].expression = "1";
    document.graph.nodes.push_back({
        "rightOsc", "talas.oscillator",
        {{"frequency", "11 hz"},
         {"amplitude", "(!KeyDown > 0) * !KeyVelocity"}}});
    document.graph.connections.push_back({
        "rightOsc", "audioOut", "voice", "rightIn"});
    auto graph = compile(document, settings());

    std::vector<double> expectedLeft(64), expectedRight(64);
    for (std::size_t frame = 0; frame < expectedLeft.size(); ++frame)
    {
        const auto gain = frame >= 5 && frame < 20 ? 100.0 / 127.0
            : frame >= 35 && frame < 50 ? 80.0 / 127.0 : 0.0;
        expectedLeft[frame] = gain * std::sin(
            2.0 * std::numbers::pi * 7.0 * static_cast<double>(frame) / 1000.0);
        expectedRight[frame] = gain * std::sin(
            2.0 * std::numbers::pi * 11.0 * static_cast<double>(frame) / 1000.0);
    }
    const auto offline = OfflineRenderSession(graph).render(64);
    require(offline.ok(), offline.diagnostics.empty()
        ? "MIDI Voice stereo-input offline render"
        : offline.diagnostics.front().message);
    expect(offline.mono, expectedLeft);
    expect(offline.right, expectedRight);

    RealtimeRenderSession realtime(graph);
    require(realtime.ready(), realtime.error());
    std::array<double, 64> left{}, right{};
    allocations.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    bool rendered = true;
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min(std::size_t{7}, left.size() - start);
        rendered = realtime.render(std::span(left).subspan(start, count),
                                   std::span(right).subspan(start, count))
            && rendered;
    }
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocations.load(std::memory_order_relaxed) == 0,
            "MIDI Voice stereo-input realtime render must be allocation-free");
    expect(left, expectedLeft);
    expect(right, expectedRight);

    auto leaked = document;
    leaked.graph.nodes.push_back({"leakMix", "talas.mixer", {}});
    auto& leftOutput = leaked.graph.connections[1];
    leftOutput.toNode = "leakMix";
    leftOutput.toPort = "in1";
    leaked.graph.connections.push_back({
        "rightOsc", "audioOut", "leakMix", "in2"});
    leaked.graph.connections.push_back({
        "leakMix", "audioOut", "out", "audioIn"});
    const auto rejected = compileProject(
        leaked, talas::core::makeBuiltinNodeRegistry(), settings());
    require(!rejected.ok()
                && std::ranges::any_of(rejected.diagnostics,
                    [](const auto& diagnostic) {
                        return diagnostic.message.find(
                            "cannot share upstream outputs outside its voice island")
                            != std::string::npos;
                    }),
            "a right-channel voice root must not escape its isolated voice scope");
}

// A voice's Input may pass through any processor: each is expanded per
// voice, prepares in realtime and plays as it does offline.
void testProcessorsPlayInsideVoices()
{
    struct Processor
    {
        std::string typeId;
        std::string output;
        std::vector<talas::project::ProjectParameterExpression> parameters;
    };
    const std::vector<Processor> processors{
        {"talas.vcf", "audioOut", {{"cutoff", "100 hz"}, {"cutoffModRange", "10 hz"}}},
        {"talas.reverbSection", "audioOut", {{"delay", "5 ms"}, {"decayTime", "50 ms"}}},
        {"talas.delayWithFeedback", "audioOut",
            {{"delay", "4 samples"}, {"feedback", "0.5"}, {"mix", "0.5"}}},
        {"talas.firFilter", "audioOut", {}},
        {"talas.timeOffset", "audioOut", {{"offset", "3 samples"}}},
        {"talas.pan", "leftOut", {}},
        {"talas.channeller", "leftOut", {}},
    };
    for (const auto& processor : processors)
    {
        auto document = instrument(true);
        document.graph.nodes.push_back(
            {"processor", processor.typeId, processor.parameters});
        document.graph.connections.front() = {"osc", "audioOut", "processor", "audioIn"};
        document.graph.connections.push_back(
            {"processor", processor.output, "voice", "audioIn"});
        const auto graph = compile(document, settings());
        const auto offline = OfflineRenderSession(graph).render(64);
        require(offline.ok(), processor.typeId + " in a voice should render offline");
        require(std::ranges::any_of(offline.mono, [](double value) { return value != 0.0; }),
            processor.typeId + " in a voice should be heard");
        RealtimeRenderSession realtime(graph);
        require(realtime.ready(), processor.typeId + " in a voice should prepare: "
            + std::string{realtime.error()});
        std::array<double, 64> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, left.size() - start);
            require(realtime.render(std::span(left).subspan(start, count),
                        std::span(right).subspan(start, count)),
                processor.typeId + " in a voice should render in realtime");
        }
        expect(left, offline.mono);
        expect(right, offline.right);
    }

    // A voice's Input may itself be a source, here at a working sample
    // rate; the file's ticks then last 48 samples.
    const std::vector<Processor> sources{
        {"talas.pluckedWaveguide", "audioOut", {{"trigger", "!KeyDown"}}},
        {"talas.twoFormantVoiceElement", "audioOut", {}},
        {"talas.dualMorphOscillator", "audioOut", {}},
        {"talas.pulseGenerator", "audioOut", {}},
        {"talas.scannedMassOscillator", "audioOut", {{"excite", "!KeyDown"}}},
    };
    auto sourceSettings = settings();
    sourceSettings.sampleRate = 48000;
    constexpr std::size_t frames = 3072;
    for (const auto& [sourceType, sourceOutput, sourceParameters] : sources)
    {
        auto document = instrument(true);
        document.graph.nodes.front() = {"osc", sourceType, sourceParameters};
        document.graph.connections.front() = {"osc", sourceOutput, "voice", "audioIn"};
        const auto graph = compile(document, sourceSettings);
        const auto offline = OfflineRenderSession(graph).render(frames);
        require(offline.ok(), sourceType + " as a voice should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
        require(std::ranges::any_of(offline.mono, [](double value) { return value != 0.0; }),
            sourceType + " as a voice should be heard");
        RealtimeRenderSession realtime(graph);
        require(realtime.ready(), sourceType + " as a voice should prepare: "
            + std::string{realtime.error()});
        std::vector<double> left(frames), right(frames);
        for (std::size_t start = 0; start < frames; start += 64)
        {
            require(realtime.render(std::span(left).subspan(start, 64),
                        std::span(right).subspan(start, 64)),
                sourceType + " as a voice should render in realtime");
        }
        expect(left, offline.mono);
        expect(right, offline.right);
    }
}

// A MIDIVoice's Input runs from the MIDIVoice's start, not
// from each note, so a TimeOffset in it starts the Input SilentTime after
// the MIDIVoice starts and later notes are heard as they come (ADR-0023,
// 2026-10-10). Projects saved before format 87 delay each note's samples.
void testTimeOffsetInAVoiceCountsFromTheVoicesStart()
{
    auto document = instrument(true);
    document.graph.nodes.push_back({"offset", "talas.timeOffset", {{"offset", "10 samples"}}});
    document.graph.connections.front() = {"osc", "audioOut", "offset", "audioIn"};
    document.graph.connections.push_back({"offset", "audioOut", "voice", "audioIn"});
    const auto graph = compile(document, settings());
    const auto offline = OfflineRenderSession(graph).render(64);
    require(offline.ok(), offline.diagnostics.empty() ? "a TimeOffset in a voice should render offline"
        : offline.diagnostics.front().message);
    RealtimeRenderSession realtime(graph);
    require(realtime.ready(), "a TimeOffset in a voice should prepare: " + std::string{realtime.error()});
    std::array<double, 64> left{}, right{};
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min(std::size_t{7}, left.size() - start);
        require(realtime.render(std::span(left).subspan(start, count), std::span(right).subspan(start, count)),
            "a TimeOffset in a voice should render in realtime");
    }
    expect(left, offline.mono);
    expect(right, offline.right);
    const auto silent = [&](std::size_t from, std::size_t to) {
        return std::all_of(offline.mono.begin() + static_cast<std::ptrdiff_t>(from),
            offline.mono.begin() + static_cast<std::ptrdiff_t>(to), [](double value) { return value == 0.0; });
    };
    // The first note (KeyDown from frame 5 to 20) is heard from frame 10,
    // the second (from 35 to 50) as it comes.
    require(silent(0, 10) && offline.mono[12] != 0.0 && silent(20, 35) && offline.mono[40] != 0.0
            && silent(50, 64),
        "a TimeOffset in a voice should start its Input SilentTime after the MIDIVoice starts");

    auto json = talas::project::serializeProjectDocument(document);
    json.replace(json.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\":  86");
    const auto migrated = talas::project::parseProjectDocument(json);
    require(migrated.ok(), "a format 86 voice should open");
    const auto legacyGraph = compile(migrated.document, settings());
    const auto legacy = OfflineRenderSession(legacyGraph).render(64);
    require(legacy.ok() && legacy.mono[25] != 0.0 && legacy.mono[55] != 0.0,
        "a format 86 TimeOffset in a voice should keep delaying each note's samples");
}

// A MIDIMapper or MIDIVoice
// multiplies its left and right channels by Left and Right, an Event Value
// such as !Fader1 setting them; it responds only to notes on its Channel
// from LowPitch to HighPitch, given as note numbers or frequencies rounded
// to the nearest one, so that voices can split the keyboard.
void testVoiceLevelsAndPitchRange()
{
    // The same for a MIDIVoice and a MIDIMapper.
    for (const auto* typeId : {"talas.midiVoice", "talas.midiMapper"})
    {
        const std::string type{typeId};
        const auto played = [&](std::vector<std::pair<std::string, std::string>> fields, RenderSettings s = settings()) {
            auto document = type == "talas.midiMapper" ? polyphonicMapperInstrument(true) : instrument(true);
            for (const auto& [id, value] : fields)
            {
                auto& parameters = document.graph.nodes[1].parameters;
                const auto found = std::ranges::find(parameters, id, &talas::project::ProjectParameterExpression::parameterId);
                require(found != parameters.end(), "the voice should have " + id);
                found->expression = value;
            }
            const auto graph = compile(document, s);
            const auto offline = OfflineRenderSession(graph).render(64);
            require(offline.ok(), offline.diagnostics.empty() ? "voice render" : offline.diagnostics.front().message);
            return offline;
        };
        const auto plain = played({});
        const auto heard = [](const std::vector<double>& samples, std::size_t from, std::size_t to) {
            return std::any_of(samples.begin() + static_cast<std::ptrdiff_t>(from),
                samples.begin() + static_cast<std::ptrdiff_t>(to), [](double value) { return value != 0.0; });
        };
        require(heard(plain.mono, 0, 20) && heard(plain.mono, 30, 50), type + ": both of the file's notes should play");
        const auto halved = played({{"left", "0.5"}, {"right", "-1"}});
        for (std::size_t frame = 0; frame < 64; ++frame)
        {
            require(std::abs(halved.mono[frame] - 0.5 * plain.mono[frame]) < 1e-12
                    && std::abs(halved.right[frame] + 2.0 * plain.right[frame]) < 1e-12,
                type + ": Left and Right should multiply the voice's channels");
        }
        auto faded = settings();
        faded.eventValues.push_back({"Fader1", 0.25});
        const auto fader = played({{"left", "!Fader1"}}, faded);
        for (std::size_t frame = 0; frame < 64; ++frame)
            require(std::abs(fader.mono[frame] - 0.25 * plain.mono[frame]) < 1e-12, type + ": !Fader1 should set Left");
        // Notes 60 and 64: a voice from 62 nn up hears only the second, one up
        // to 293.66 hz (4 d, rounded to 62 nn) only the first.
        const auto upper = played({{"lowPitch", "62 nn"}});
        const auto lower = played({{"highPitch", "293.66 hz"}});
        require(!heard(upper.mono, 0, 20) && heard(upper.mono, 30, 50) && heard(lower.mono, 0, 20) && !heard(lower.mono, 30, 50),
            type + ": a voice should respond only to notes from LowPitch to HighPitch, splitting the keyboard");
        // The file's notes are on channel 1: a voice on channel 2 hears
        // nothing.
        const auto elsewhere = played({{"channel", "2"}});
        require(!heard(elsewhere.mono, 0, 64), type + ": a voice should only hear its own MIDI channel");
    }
}

// A MIDIMapper's and a MIDIVoice's Script:
// the field is a Smalltalk program, so temporaries, timesRepeat:, Random
// newForTalasWithSeed: and arithmetic generate the events algorithmically;
// a tag may be dropped; arguments must be real values, not Event Values.
void testSmalltalkScriptGeneratesEvents()
{
    const auto played = [](std::string typeId, std::string script) -> std::optional<std::vector<double>> {
        auto document = typeId == "talas.midiMapper" ? polyphonicMapperInstrument(false) : instrument(false);
        document.graph.nodes[1].parameters[6].text = "Script";
        document.graph.nodes[1].parameters[8].text = std::move(script);
        auto s = settings();
        s.binaryResources.clear();
        const auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), s);
        if (!compiled.ok())
            return std::nullopt;
        const auto offline = OfflineRenderSession(*compiled.graph).render(256);
        require(offline.ok(), offline.diagnostics.empty() ? "script render" : offline.diagnostics.front().message);
        return offline.mono;
    };
    for (const auto* typeId : {"talas.midiVoice", "talas.midiMapper"})
    {
        const auto literal = played(typeId,
            "self keyDownAt: 10 ms duration: 5 ms frequency: 61 nn velocity: 0.5. "
            "self keyDownAt: 20 ms duration: 5 ms frequency: 62 nn velocity: 0.5. "
            "self keyDownAt: 30 ms duration: 5 ms frequency: 63 nn velocity: 0.5.");
        const auto generated = played(typeId,
            "| t p | t := 0. p := 60. 3 timesRepeat: [t := t + 10. p := p + 1. "
            "self keyDownAt: t ms duration: 5 ms frequency: p nn velocity: 0.5].");
        require(literal.has_value() && generated.has_value() && *generated == *literal,
            std::string{typeId} + ": a Smalltalk Script should generate the events its statements describe");
        // An example, shortened: notes at random times, frequencies and
        // velocities from a Random seeded with 66508, the same each time.
        const auto randomScript = "| r t | r := Random newForTalasWithSeed: 66508. t := 0. "
                                  "6 timesRepeat: [t := t + 0.03 + (r next * 0.01). "
                                  "self keyDownAt: t s duration: 0.02 beats frequency: (r next * 400 + 60) hz velocity: r next].";
        const auto drawn = played(typeId, randomScript);
        require(drawn.has_value() && *played(typeId, randomScript) == *drawn
                && std::ranges::any_of(*drawn, [](double value) { return value != 0.0; }),
            std::string{typeId} + ": a seeded Random Script should play the same notes each time");
        // A tag may be dropped: the defaults stand for the rest.
        require(played(typeId, "| t | t := 10. self keyDownAt: t ms.").has_value(),
            std::string{typeId} + ": a keyDownAt: without its other tags should be valid");
        require(!played(typeId, "| t | t := 1. self keyDownAt: t s duration: !Dur s.").has_value(),
            std::string{typeId} + ": Script arguments must be real values, not Event Values");
    }
}

void testLiteralScriptKeepsContinuousFrequency()
{
    auto document = instrument(false);
    document.graph.nodes[1].parameters[6].text = "Script";
    document.graph.nodes[1].parameters[8].text =
        "self keyDownAt: 10 ms duration: 20 ms frequency: 445 hz velocity: 0.7.";
    auto s = settings();
    s.binaryResources.clear();
    auto graph = compile(document, s);
    const auto validated = validateOfflineRuntimeGraph(graph);
    require(validated.ok(), validated.diagnostics.empty() ? "script offline validation"
        : validated.diagnostics.front().message);
    PreparedRuntimeGraph forged(graph.plan(), graph.renderSettings());
    require(!RealtimeRenderSession(forged).ready()
        && !OfflineRenderSession(forged).render(64).ok(),
        "unsealed Script plan cannot bypass compiler preparation");
    std::vector<double> expected(64);
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        const auto frequency = frame >= 15 ? 445.0
            : 440.0 * std::pow(2.0, (60.0 - 69.0) / 12.0);
        if (frame >= 15 && frame < 30)
            expected[frame] = 0.7 * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + frequency / 1000.0, 1.0);
    }
    const auto offline = OfflineRenderSession(graph).render(64);
    require(offline.ok(), offline.diagnostics.empty() ? "script offline render"
        : offline.diagnostics.front().message);
    expect(offline.mono, expected); expect(offline.right, expected, 0.5);
    for (const auto block : {std::size_t{1}, std::size_t{7}, std::size_t{64}})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        require(session.scheduleMidiNote({RealtimeMidiNoteKind::NoteOn, 1, 72, 127}, 0)
            == RealtimeMidiDispatchResult::NotHandled,
            "live MIDI must not reach a script owner");
        std::vector<double> left(64), right(64);
        for (std::size_t start = 0; start < 64; start += block)
        {
            const auto count = std::min(block, 64 - start);
            require(session.render(std::span(left).subspan(start, count),
                std::span(right).subspan(start, count)), "partitioned script render");
        }
        expect(left, expected); expect(right, expected, 0.5);
    }
    document.graph.nodes[1].parameters[8].text = "1 to: 4 do: [ :i | i ].";
    require(!compileProject(document, talas::core::makeBuiltinNodeRegistry(), s).ok(),
        "unsupported Smalltalk must fail during preparation");
}

void testLiteralScriptTimbreReachesVoice()
{
    auto document = instrument(false);
    document.graph.nodes[0].parameters[1].expression =
        "(!KeyDown > 0) * !KeyTimbre";
    document.graph.nodes[1].parameters[6].text = "Script";
    document.graph.nodes[1].parameters[8].text =
        "self keyDownAt: 0 ms duration: 10 ms frequency: 60 nn velocity: 0.7 timbre: 0.25. "
        "self keyDownAt: 20 ms duration: 10 ms frequency: 60 nn velocity: 0.7 timbre: 0.75.";
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto saved = talas::project::serializeProjectDocument(
        document, registry);
    const auto reopened = talas::project::parseProjectDocument(saved);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(
                   reopened.document, registry) == saved,
        "script timbre Sound saves and reopens byte-stably");
    auto s = settings();
    s.binaryResources.clear();
    const auto graph = compile(reopened.document, s);

    std::vector<double> expected(40);
    double phase = 0.0;
    const auto frequency = 440.0
        * std::pow(2.0, (60.0 - 69.0) / 12.0);
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        const auto timbre = frame >= 5 && frame < 10 ? 0.25
            : frame >= 25 && frame < 30 ? 0.75 : 0.0;
        expected[frame] = timbre
            * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + frequency / 1000.0, 1.0);
    }
    const auto offline = OfflineRenderSession(graph).render(40);
    require(offline.ok(), offline.diagnostics.empty()
        ? "script timbre offline render"
        : offline.diagnostics.front().message);
    expect(offline.mono, expected);
    expect(offline.right, expected, 0.5);
    for (const auto block : {std::size_t{3}, std::size_t{7}})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 40> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += block)
        {
            const auto count = std::min(block, left.size() - start);
            allocations.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            const auto rendered = session.render(
                std::span(left).subspan(start, count),
                std::span(right).subspan(start, count));
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered
                    && allocations.load(std::memory_order_relaxed) == 0,
                "script timbre callback renders without allocation");
        }
        expect(left, expected);
        expect(right, expected, 0.5);
    }
}

void testLiteralScriptRetainsFineVelocityAndTimbre()
{
    for (const bool velocityProfile : {true, false})
    {
        auto document = instrument(false);
        document.graph.nodes[0].parameters[1].expression =
            velocityProfile
                ? "(!KeyDown > 0) * !KeyVelocity"
                : "(!KeyDown > 0) * !KeyTimbre";
        document.graph.nodes[1].parameters[6].text = "Script";
        document.graph.nodes[1].parameters[8].text =
            velocityProfile
                ? "self keyDownAt: 0 ms duration: 10 ms frequency: 60 nn velocity: 0.5 timbre: 0.5. "
                  "self keyDownAt: 20 ms duration: 10 ms frequency: 60 nn velocity: 0.5005 timbre: 0.5."
                : "self keyDownAt: 0 ms duration: 10 ms frequency: 60 nn velocity: 0.5 timbre: 0.5. "
                  "self keyDownAt: 20 ms duration: 10 ms frequency: 60 nn velocity: 0.5 timbre: 0.5005.";
        auto s = settings();
        s.binaryResources.clear();
        const auto graph = compile(document, s);
        const auto offline = OfflineRenderSession(graph).render(40);
        require(offline.ok(), "fine MIDI Script values render offline");

        std::vector<double> expected(40);
        double phase = 0.0;
        const auto frequency = 440.0
            * std::pow(2.0, (60.0 - 69.0) / 12.0);
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
        {
            const auto value = frame >= 5 && frame < 10 ? 0.5
                : frame >= 25 && frame < 30 ? 0.5005 : 0.0;
            expected[frame] = value
                * std::sin(2.0 * std::numbers::pi * phase);
            phase = std::fmod(phase + frequency / 1000.0, 1.0);
        }
        expect(offline.mono, expected);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 40> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7},
                left.size() - start);
            allocations.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            const auto rendered = session.render(
                std::span(left).subspan(start, count),
                std::span(right).subspan(start, count));
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered
                    && allocations.load(std::memory_order_relaxed) == 0,
                "fine MIDI Script callback renders without allocation");
        }
        expect(left, expected);
        expect(right, expected, 0.5);
    }
}

void testScriptOctavePitchWithFractionalOffset()
{
    std::vector<double> expected(64);
    double phase = 0.0;
    const auto fractionalFrequency = 440.0 * std::pow(2.0, (60.5 - 69.0) / 12.0);
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        const auto frequency = frame >= 15 ? fractionalFrequency
            : 440.0 * std::pow(2.0, (60.0 - 69.0) / 12.0);
        if (frame >= 15 && frame < 30)
            expected[frame] = 0.75 * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + frequency / 1000.0, 1.0);
    }
    for (const bool mapper : {false, true})
    {
        auto document = mapper ? polyphonicMapperInstrument(false) : instrument(false);
        document.graph.nodes[1].parameters[6].text = "Script";
        document.graph.nodes[1].parameters[8].text =
            "self keyDownAt: 10 ms duration: 20 ms frequency: 4 c + 0.5 nn velocity: 0.75.";
        auto renderSettings = settings();
        renderSettings.binaryResources.clear();
        auto graph = compile(document, renderSettings);
        const auto offline = OfflineRenderSession(graph).render(64);
        require(offline.ok(), "octave-spelled Script must render offline");
        expect(offline.mono, expected);
        expect(offline.right, expected, 0.5);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 64> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, left.size() - start);
            require(session.render(std::span(left).subspan(start, count),
                std::span(right).subspan(start, count)),
                "octave-spelled Script prepared render");
        }
        expect(left, expected);
        expect(right, expected, 0.5);
    }
}

void testScriptAccidentalPitchSpelling()
{
    for (const auto& [spelling, pitch] :
         std::array<std::pair<std::string_view, double>, 2>{
             {{"4 c sharp", 61.0}, {"4 c flat", 59.0}}})
    {
        std::vector<double> expected(64);
        double phase = 0.0;
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
        {
            const auto activePitch = frame >= 15 ? pitch : 60.0;
            const auto frequency = 440.0
                * std::pow(2.0, (activePitch - 69.0) / 12.0);
            if (frame >= 15 && frame < 30)
            {
                expected[frame] = 0.75
                    * std::sin(2.0 * std::numbers::pi * phase);
            }
            phase = std::fmod(phase + frequency / 1000.0, 1.0);
        }
        for (const bool mapper : {false, true})
        {
            auto document = mapper
                ? polyphonicMapperInstrument(false) : instrument(false);
            document.graph.nodes[1].parameters[6].text = "Script";
            document.graph.nodes[1].parameters[8].text =
                "self keyDownAt: 10 ms duration: 20 ms frequency: "
                + std::string(spelling) + " velocity: 0.75.";
            auto renderSettings = settings();
            renderSettings.binaryResources.clear();
            auto graph = compile(document, renderSettings);
            const auto offline = OfflineRenderSession(graph).render(64);
            require(offline.ok(),
                "accidental-spelled Script must render offline");
            expect(offline.mono, expected);
            expect(offline.right, expected, 0.5);
            RealtimeRenderSession session(graph);
            require(session.ready(), session.error());
            std::array<double, 64> left{}, right{};
            for (std::size_t start = 0; start < left.size(); start += 7)
            {
                const auto count =
                    std::min(std::size_t{7}, left.size() - start);
                require(session.render(
                    std::span(left).subspan(start, count),
                    std::span(right).subspan(start, count)),
                    "accidental-spelled Script prepared render");
            }
            expect(left, expected);
            expect(right, expected, 0.5);
        }
    }
}

void testScriptBeatTimeWithoutMetronome()
{
    std::vector<double> expected(64);
    double phase = 0.0;
    const auto frequency = 440.0 * std::pow(2.0, (60.0 - 69.0) / 12.0);
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        if (frame >= 15 && frame < 30)
            expected[frame] = 0.5 * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + frequency / 1000.0, 1.0);
    }
    for (const bool mapper : {false, true})
    {
        auto document = mapper ? polyphonicMapperInstrument(false) : instrument(false);
        document.graph.nodes[1].parameters[6].text = "Script";
        document.graph.nodes[1].parameters[8].text =
            "self keyDownAt: 0.01 beats duration: 0.02 beats frequency: 60 nn velocity: 0.5.";
        auto renderSettings = settings();
        renderSettings.tempoBpm = 120.0;
        renderSettings.binaryResources.clear();
        auto graph = compile(document, renderSettings);
        const auto offline = OfflineRenderSession(graph).render(64);
        require(offline.ok(), "implicit-metronome Script must render offline");
        expect(offline.mono, expected);
        expect(offline.right, expected, 0.5);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 64> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, left.size() - start);
            require(session.render(std::span(left).subspan(start, count),
                std::span(right).subspan(start, count)),
                "implicit-metronome Script prepared render");
        }
        expect(left, expected);
        expect(right, expected, 0.5);
    }
}

void testScriptMetronomeRedefinition()
{
    std::vector<double> expected(64);
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        const auto pitch = frame >= 35 ? 61.0 : 60.0;
        const auto frequency = 440.0 * std::pow(2.0, (pitch - 69.0) / 12.0);
        const auto gain = frame >= 15 && frame < 20 ? 0.5
            : frame >= 35 && frame < 40 ? 0.7 : 0.0;
        expected[frame] = gain * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + frequency / 1000.0, 1.0);
    }
    for (const bool mapper : {false, true})
    {
        auto document = mapper ? polyphonicMapperInstrument(false) : instrument(false);
        document.graph.nodes[1].parameters[6].text = "Script";
        document.graph.nodes[1].parameters[8].text =
            "MM := 60. self keyDownAt: 0.01 beats duration: 0.01 beats frequency: 60 nn velocity: 0.5. "
            "MM := 120. self keyDownAt: 0.06 beats duration: 0.02 beats frequency: 61 nn velocity: 0.7.";
        auto renderSettings = settings();
        renderSettings.binaryResources.clear();
        auto graph = compile(document, renderSettings);
        const auto offline = OfflineRenderSession(graph).render(64);
        require(offline.ok(), "redefined-metronome Script must render offline");
        expect(offline.mono, expected);
        expect(offline.right, expected, 0.5);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 64> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, left.size() - start);
            require(session.render(std::span(left).subspan(start, count),
                std::span(right).subspan(start, count)),
                "redefined-metronome Script prepared render");
        }
        expect(left, expected);
        expect(right, expected, 0.5);
    }
}

void testScriptGeneratedNoteLoop()
{
    constexpr std::size_t frameCount = 6000;
    std::vector<double> expected(frameCount);
    double phase = 0.0;
    double pitch = 60.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        if (frame >= 5 && (frame - 5) % 500 == 0 && (frame - 5) / 500 < 12)
            pitch = 61.0 + static_cast<double>((frame - 5) / 500);
        const auto noteIndex = frame / 500;
        const auto withinBeat = frame % 500;
        const auto gain = noteIndex < 12 && withinBeat >= 5 && withinBeat < 125
            ? static_cast<double>(noteIndex + 1) / 12.0 : 0.0;
        expected[frame] = gain * std::sin(2.0 * std::numbers::pi * phase);
        const auto frequency = 440.0 * std::pow(2.0, (pitch - 69.0) / 12.0);
        phase = std::fmod(phase + frequency / 1000.0, 1.0);
    }
    for (const bool mapper : {false, true})
    {
        auto document = mapper ? polyphonicMapperInstrument(false) : instrument(false);
        document.graph.nodes[1].parameters[6].text = "Script";
        document.graph.nodes[1].parameters[8].text =
            "MM := 120. 1 to: 12 do: [ :i | "
            "self keyDownAt: (i - 1) beats duration: 0.25 beats "
            "frequency: 4 c + i nn velocity: (i / 12.0)].";
        auto renderSettings = settings();
        renderSettings.binaryResources.clear();
        auto graph = compile(document, renderSettings);
        const auto offline = OfflineRenderSession(graph).render(frameCount);
        require(offline.ok(), "generated-note Script must render offline");
        expect(offline.mono, expected);
        expect(offline.right, expected, 0.5);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::vector<double> left(frameCount), right(frameCount);
        for (std::size_t start = 0; start < frameCount; start += 61)
        {
            const auto count = std::min(std::size_t{61}, frameCount - start);
            require(session.render(std::span(left).subspan(start, count),
                std::span(right).subspan(start, count)),
                "generated-note Script prepared render");
        }
        expect(left, expected);
        expect(right, expected, 0.5);
    }
}

void testScriptControllerSet()
{
    std::vector<double> expected(40);
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        if (frame >= 5 && frame < 30)
            expected[frame] = std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + (frame < 10 ? 100.0 : 200.0) / 1000.0, 1.0);
    }
    for (const bool mapper : {false, true})
    {
        auto document = mapper ? polyphonicMapperInstrument(false) : instrument(false);
        document.graph.nodes[0].parameters[0].expression = "!Frequency";
        document.graph.nodes[1].parameters[6].text = "Script";
        document.graph.nodes[1].parameters[8].text =
            "self keyDownAt: 0 s duration: 30 ms frequency: 60 nn. "
            "self controller: !Frequency setTo: 200 atTime: 10 ms.";
        auto renderSettings = settings();
        renderSettings.binaryResources.clear();
        renderSettings.eventValues.push_back(
            {"Frequency", 100, talas::expr::ValueDomain::Hertz});
        auto graph = compile(document, renderSettings);
        const auto offline = OfflineRenderSession(graph).render(40);
        require(offline.ok(), "script controller must render offline");
        expect(offline.mono, expected);
        expect(offline.right, expected, 0.5);
        for (const auto block : {std::size_t{1}, std::size_t{7}, std::size_t{40}})
        {
            RealtimeRenderSession session(graph);
            require(session.ready(), session.error());
            std::array<double, 40> left{}, right{};
            for (std::size_t start = 0; start < left.size(); start += block)
            {
                const auto count = std::min(block, left.size() - start);
                require(session.render(std::span(left).subspan(start, count),
                    std::span(right).subspan(start, count)),
                    "script controller must render across irregular prepared blocks");
            }
            expect(left, expected);
            expect(right, expected, 0.5);
            session.resetTransport();
            require(session.render(left, right), "script controller replay after reset");
            expect(left, expected);
        }
        if (!mapper) continue;
        const auto voiceScope = graph.plan().midiVoices.front().voices.front().scopeId;
        renderSettings.eventValues.push_back(
            {"Frequency", 150, talas::expr::ValueDomain::Hertz, voiceScope});
        auto localGraph = compile(document, renderSettings);
        std::vector<double> localExpected(40);
        double localPhase = 0.0;
        for (std::size_t frame = 0; frame < localExpected.size(); ++frame)
        {
            if (frame >= 5 && frame < 30)
                localExpected[frame] = std::sin(2.0 * std::numbers::pi * localPhase);
            localPhase = std::fmod(localPhase
                + (frame < 10 ? 150.0 : 200.0) / 1000.0, 1.0);
        }
        const auto localOffline = OfflineRenderSession(localGraph).render(40);
        require(localOffline.ok(), "local Script controller must render offline");
        expect(localOffline.mono, localExpected);
        RealtimeRenderSession localSession(localGraph);
        require(localSession.ready(), localSession.error());
        std::array<double, 40> localLeft{}, localRight{};
        for (std::size_t start = 0; start < localLeft.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, localLeft.size() - start);
            require(localSession.render(std::span(localLeft).subspan(start, count),
                std::span(localRight).subspan(start, count)),
                "local Script controller must render prepared blocks");
        }
        expect(localLeft, localExpected);
    }
}

void testScriptControllerSlide()
{
    std::vector<double> expected(40);
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        if (frame >= 5 && frame < 30)
            expected[frame] = std::sin(2.0 * std::numbers::pi * phase);
        const auto step = std::min<std::size_t>(frame / 2, 5);
        phase = std::fmod(phase + (100.0 + 20.0 * step) / 1000.0, 1.0);
    }
    for (const bool mapper : {false, true})
    {
        auto document = mapper ? polyphonicMapperInstrument(false) : instrument(false);
        document.graph.nodes[0].parameters[0].expression = "!Frequency";
        document.graph.nodes[1].parameters[6].text = "Script";
        document.graph.nodes[1].parameters[8].text =
            "self keyDownAt: 0 s duration: 30 ms frequency: 60 nn. "
            "self controller: !Frequency setTo: 100 atTime: 0 ms. "
            "self controller: !Frequency slideTo: 200 steps: 5 byTime: 10 ms.";
        auto renderSettings = settings();
        renderSettings.binaryResources.clear();
        renderSettings.eventValues.push_back(
            {"Frequency", 100, talas::expr::ValueDomain::Hertz});
        auto graph = compile(document, renderSettings);
        const auto offline = OfflineRenderSession(graph).render(40);
        require(offline.ok(), "stepped Script controller must render offline");
        expect(offline.mono, expected);
        expect(offline.right, expected, 0.5);
        for (const auto block : {std::size_t{1}, std::size_t{7}, std::size_t{40}})
        {
            RealtimeRenderSession session(graph);
            require(session.ready(), session.error());
            std::array<double, 40> left{}, right{};
            allocations.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            bool ok = true;
            for (std::size_t start = 0; start < left.size(); start += block)
            {
                const auto count = std::min(block, left.size() - start);
                ok = session.render(std::span(left).subspan(start, count),
                    std::span(right).subspan(start, count)) && ok;
            }
            countAllocations.store(false, std::memory_order_relaxed);
            require(ok && allocations.load(std::memory_order_relaxed) == 0,
                "stepped Script controller callback must be allocation-free");
            expect(left, expected);
            expect(right, expected, 0.5);
        }
    }
}

void testScriptControllerStepSize()
{
    std::vector<double> expected(30);
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        if (frame >= 5 && frame < 20)
            expected[frame] = std::sin(2.0 * std::numbers::pi * phase);
        const auto frequency = frame < 2 ? 100.0
            : frame < 4 ? 120.0 : frame < 6 ? 140.0 : 155.0;
        phase = std::fmod(phase + frequency / 1000.0, 1.0);
    }
    for (const bool mapper : {false, true})
    {
        auto document = mapper ? polyphonicMapperInstrument(false) : instrument(false);
        document.graph.nodes[0].parameters[0].expression = "!Frequency";
        document.graph.nodes[1].parameters[6].text = "Script";
        document.graph.nodes[1].parameters[8].text =
            "self keyDownAt: 0 s duration: 20 ms frequency: 60 nn. "
            "self controller: !Frequency setTo: 100 atTime: 0 ms. "
            "self controller: !Frequency slideTo: 155 stepSize: 20 byTime: 6 ms.";
        auto renderSettings = settings();
        renderSettings.binaryResources.clear();
        renderSettings.eventValues.push_back(
            {"Frequency", 100, talas::expr::ValueDomain::Hertz});
        auto graph = compile(document, renderSettings);
        const auto offline = OfflineRenderSession(graph).render(30);
        require(offline.ok(), "stepSize Script controller must render offline");
        expect(offline.mono, expected);
        expect(offline.right, expected, 0.5);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 30> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, left.size() - start);
            require(session.render(std::span(left).subspan(start, count),
                std::span(right).subspan(start, count)),
                "stepSize Script controller must render prepared blocks");
        }
        expect(left, expected);
        expect(right, expected, 0.5);
        if (!mapper) continue;
        const auto voiceScope = graph.plan().midiVoices.front().voices.front().scopeId;
        renderSettings.eventValues.push_back(
            {"Frequency", 150, talas::expr::ValueDomain::Hertz, voiceScope});
        auto localGraph = compile(document, renderSettings);
        const auto localOffline = OfflineRenderSession(localGraph).render(30);
        require(localOffline.ok(), "local stepSize Script controller must render offline");
        expect(localOffline.mono, expected);
        RealtimeRenderSession localSession(localGraph);
        require(localSession.ready(), localSession.error());
        std::array<double, 30> localLeft{}, localRight{};
        for (std::size_t start = 0; start < localLeft.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, localLeft.size() - start);
            require(localSession.render(std::span(localLeft).subspan(start, count),
                std::span(localRight).subspan(start, count)),
                "local stepSize Script controller must render prepared blocks");
        }
        expect(localLeft, expected);
    }
}

void testPolyphonicMapperLiteralScriptSource()
{
    auto document = polyphonicMapperInstrument(false);
    document.graph.nodes[0].parameters[1].expression =
        "(!KeyDown > 0) * !KeyVelocity * !KeyTimbre";
    document.graph.nodes[1].parameters[6].text = "Script";
    document.graph.nodes[1].parameters[8].text =
        "self keyDownAt: 10 ms duration: 20 ms frequency: 445 hz velocity: 0.7 timbre: 0.5.";
    auto renderSettings = settings();
    renderSettings.binaryResources.clear();
    auto graph = compile(document, renderSettings);
    const auto validation = validateOfflineRuntimeGraph(graph);
    require(validation.ok(), validation.diagnostics.empty()
        ? "public offline validation must select Mapper's Script source"
        : validation.diagnostics.front().message);
    std::vector<double> expected(64);
    double phase = 0.0;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        const auto frequency = frame >= 15 ? 445.0
            : 440.0 * std::pow(2.0, (60.0 - 69.0) / 12.0);
        if (frame >= 15 && frame < 30)
            expected[frame] = 0.35 * std::sin(2.0 * std::numbers::pi * phase);
        phase = std::fmod(phase + frequency / 1000.0, 1.0);
    }
    const auto offline = OfflineRenderSession(graph).render(64);
    require(offline.ok(), "Mapper Script must render offline");
    expect(offline.mono, expected);
    expect(offline.right, expected, 0.5);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    require(session.scheduleMidiNote(
                {RealtimeMidiNoteKind::NoteOn, 1, 72, 127}, 0)
            == RealtimeMidiDispatchResult::NotHandled,
        "live notes must not enter a Script-only Mapper");
    std::array<double, 64> left{}, right{};
    allocations.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    bool ok = true;
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min(std::size_t{7}, left.size() - start);
        ok = session.render(std::span(left).subspan(start, count),
                 std::span(right).subspan(start, count)) && ok;
    }
    countAllocations.store(false, std::memory_order_relaxed);
    require(ok && allocations.load(std::memory_order_relaxed) == 0,
        "Mapper Script callback must be allocation-free");
    expect(left, expected);
    expect(right, expected, 0.5);

    auto legacy = document;
    legacy.graph.nodes[1].parameters.back().choice = "legacyPassthrough";
    const auto legacyCompiled = compileProject(
        legacy, talas::core::makeBuiltinNodeRegistry(), renderSettings);
    require(!legacyCompiled.ok(),
        "legacy Mapper mode must not silently change to Script scheduling");
}

void testPolyphonicMapperSourceSelection()
{
    auto live = polyphonicMapperInstrument(false);
    live.graph.nodes[1].parameters[6].text = "Live MIDI";
    live.graph.nodes[1].parameters[7].resource = "inactive-file";
    auto renderSettings = settings();
    renderSettings.binaryResources.clear();
    auto compiled = compileProject(
        live, talas::core::makeBuiltinNodeRegistry(), renderSettings);
    require(compiled.ok(),
        "live Mapper must not load its inactive MIDI file field");
    RealtimeRenderSession liveSession(*compiled.graph);
    require(liveSession.ready()
            && liveSession.scheduleMidiNote(
                {RealtimeMidiNoteKind::NoteOn, 1, 60, 127}, 0)
                == RealtimeMidiDispatchResult::Handled,
        "live Mapper must accept a scheduled live note");

    auto unknown = live;
    unknown.graph.nodes[1].parameters[6].text = "MIDIFile";
    require(!compileProject(unknown, talas::core::makeBuiltinNodeRegistry(),
                renderSettings).ok(),
        "unknown Mapper source labels must fail closed");
    auto legacyFile = polyphonicMapperInstrument(true);
    legacyFile.graph.nodes[1].parameters.back().choice = "legacyPassthrough";
    require(!compileProject(legacyFile, talas::core::makeBuiltinNodeRegistry(),
                settings()).ok(),
        "legacy Mapper mode must not silently change to MIDI File scheduling");
    auto missingFile = polyphonicMapperInstrument(true);
    missingFile.graph.nodes[1].parameters[7].resource = "";
    require(!compileProject(missingFile,
                talas::core::makeBuiltinNodeRegistry(), renderSettings).ok(),
        "selected Mapper MIDI File must identify a resource");
    auto missingScript = polyphonicMapperInstrument(false);
    missingScript.graph.nodes[1].parameters[6].text = "Script";
    require(!compileProject(missingScript,
                talas::core::makeBuiltinNodeRegistry(), renderSettings).ok(),
        "selected Mapper Script must contain program text");
}

void testPolyphonyAndFiltering()
{
    auto document = instrument(true);
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VoiceV1;
    document.graph.nodes[0].parameters[0].expression = "?VoiceNumber * 7 hz";
    document.graph.nodes.push_back({
        "rightOsc", "talas.oscillator",
        {{"frequency", "?VoiceNumber * 11 hz"},
         {"amplitude", "(!KeyDown > 0) * !KeyVelocity"}}});
    document.graph.connections.push_back({
        "rightOsc", "audioOut", "voice", "rightIn"});
    document.graph.nodes[1].parameters[3].expression = "2";
    document.graph.nodes[1].parameters[4].expression = "60 nn";
    document.graph.nodes[1].parameters[5].expression = "72 nn";
    auto s = settings();
    track(s, {0,0x91,60,127, 0,0x90,48,127, 0,0x90,60,127, 0,0x90,64,64,
        2,0x80,60,0, 4,0x90,67,32, 6,0x90,69,100, 6,0x80,64,0,
        2,0x80,67,0, 5,0x90,69,0});
    auto graph = compile(document,s);
    std::vector<double> expected(64), expectedRight(64);
    for (std::size_t f=0; f<64; ++f)
    {
        if (f>=11 && f<20)
        {
            expected[f] += 32.0/127 * std::sin(2*std::numbers::pi*7*f/1000);
            expectedRight[f] += 0.5 * 32.0/127 * std::sin(2*std::numbers::pi*11*f/1000);
        }
        const auto gain = f>=5 && f<12 ? 64.0/127 : f>=17 && f<25 ? 100.0/127 : 0;
        expected[f] += gain * std::sin(2*std::numbers::pi*14*f/1000);
        expectedRight[f] += 0.5 * gain * std::sin(2*std::numbers::pi*22*f/1000);
    }
    const auto audio = OfflineRenderSession(graph).render(64);
    require(audio.ok(), "polyphonic offline render");
    expect(audio.mono,expected);
    expect(audio.right,expectedRight);
    for (const std::size_t block : {1,7,64})
    {
        RealtimeRenderSession session(graph); require(session.ready(),session.error());
        std::vector<double> left(64),right(64);
        for (std::size_t f=0; f<64; f+=block)
            require(session.render(std::span(left).subspan(f,std::min(block,64-f)),
                std::span(right).subspan(f,std::min(block,64-f))), "polyphonic realtime render");
        expect(left,expected); expect(right,expectedRight);
    }
}

void testMixedOwners()
{
    auto document = instrument(true);
    auto live = instrument(false);
    live.graph.nodes[0].instanceId = "liveOsc"; live.graph.nodes[1].instanceId = "liveVoice";
    live.graph.nodes[0].parameters[0].expression = "7 hz";
    document.graph.nodes.push_back(live.graph.nodes[0]);
    document.graph.nodes.push_back(live.graph.nodes[1]);
    document.graph.connections.back() = {"liveVoice","rightOut","out","rightIn"};
    document.graph.connections.push_back({"liveOsc","audioOut","liveVoice","audioIn"});
    auto graph = compile(document,settings());
    RealtimeRenderSession session(graph); require(session.ready(),session.error());
    require(session.scheduleMidiNote({RealtimeMidiNoteKind::NoteOn,1,72,127},2)
        == RealtimeMidiDispatchResult::Handled, "mixed live note accepted");
    require(session.scheduleMidiNote({RealtimeMidiNoteKind::NoteOff,1,72,0},16)
        == RealtimeMidiDispatchResult::Handled, "mixed live release accepted");
    std::vector<double> left(64),right(64),liveExpected(64);
    for (std::size_t f=7; f<16; ++f) liveExpected[f]=0.5*std::sin(2*std::numbers::pi*7*f/1000);
    require(session.render(left,right), "mixed owners render");
    expect(left,oracle()); expect(right,liveExpected);
    OfflineRenderSession offline(graph);
    std::array<OfflineMidiVoiceEvent,2> events;
    events[0].frame=2; events[0].batchId=1; events[0].sequence=1;
    events[0].kind=OfflineMidiVoiceEventKind::NoteOn; events[0].note=72; events[0].velocity=127;
    events[1].frame=16; events[1].batchId=2; events[1].sequence=2;
    events[1].kind=OfflineMidiVoiceEventKind::NoteOff; events[1].note=72;
    require(offline.scheduleMidiVoiceEvents(events), "offline mixed schedule");
    auto audio=offline.render(64); require(audio.ok(), "offline mixed render");
    expect(audio.mono,oracle()); expect(audio.right,liveExpected);
}

void testCallbackRollbackAndAllocation()
{
    auto document=instrument(true);
    document.graph.nodes[0].parameters[1].expression =
        "(!KeyDown > 0) * !KeyVelocity / (1 - (!KeyDown > 0) * !Fault)";
    auto s=settings(); s.eventValues.push_back({"Fault",1});
    auto graph=compile(document,s);
    RealtimeRenderSession session(graph); require(session.ready(),session.error());
    std::vector<double> left(64),right(64);
    allocations.store(0); countAllocations.store(true);
    const auto failed=session.render(left,right);
    countAllocations.store(false);
    require(!failed, "attack expression must fail");
    require(allocations.load()==0, "failing file callback must not allocate");
    require(session.updateEventValue("Fault",0,talas::expr::ValueDomain::Scalar), "repair failed callback");
    allocations.store(0); countAllocations.store(true);
    const auto recovered=session.render(left,right);
    countAllocations.store(false);
    require(recovered, "repaired callback replay");
    require(allocations.load()==0, "file event consumption and replay must not allocate");
    expect(left,oracle()); expect(right,oracle(),0.5);
}

void testScriptCallbackRollbackAndAllocation()
{
    auto document = instrument(false);
    document.graph.nodes[0].parameters[0].expression = "!Pitch";
    document.graph.nodes[0].parameters[1].expression =
        "(!KeyDown > 0) * !KeyVelocity / (1 - (!KeyDown > 0) * !Fault)";
    document.graph.nodes[1].parameters[6].text = "Script";
    document.graph.nodes[1].parameters[8].text =
        "self keyDownAt: 0 ms duration: 20 ms frequency: 445 hz velocity: 0.7.";
    auto configured = settings();
    configured.binaryResources.clear();
    configured.eventValues.push_back({"Fault", 1});
    const auto graph = compile(document, configured);
    RealtimeRenderSession session(graph), reference(graph);
    require(session.ready() && reference.ready(), "script rollback sessions prepare");
    std::vector<double> left(64), right(64), expectedLeft(64), expectedRight(64);
    allocations.store(0); countAllocations.store(true);
    const auto failed = session.render(left, right);
    countAllocations.store(false);
    require(!failed && allocations.load() == 0,
        "script attack failure rolls back without callback allocation");
    require(session.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
        && reference.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar),
        "repair script fault event");
    require(reference.render(expectedLeft, expectedRight), "fresh repaired script reference");
    allocations.store(0); countAllocations.store(true);
    const auto recovered = session.render(left, right);
    countAllocations.store(false);
    require(recovered && allocations.load() == 0,
        "script same-frame batch retry renders without callback allocation");
    expect(left, expectedLeft); expect(right, expectedRight);
}

void testSourceAuthorityAndExplicitLimits()
{
    for (const auto* source : {"", "Live MIDI"})
    {
        auto live=instrument(false);
        live.graph.nodes[1].parameters[6].text=source;
        live.graph.nodes[1].parameters[7].resource="inactive-missing-file";
        auto liveGraph=compile(live,settings());
        require(validateOfflineRuntimeGraph(liveGraph).ok(), "inactive file must not invalidate live offline source");
        auto audio=OfflineRenderSession(liveGraph).render(64);
        require(audio.ok(), "explicit live source must render offline");
        std::array<double,64> silence{};
        expect(audio.mono,silence); expect(audio.right,silence);
        RealtimeRenderSession liveSession(liveGraph);
        require(liveSession.ready(),liveSession.error());
        std::array<double,64> left{},right{};
        require(liveSession.render(left,right), "explicit live realtime render");
        expect(left,silence); expect(right,silence);
    }
    auto graph=compile(instrument(true),settings());
    PreparedRuntimeGraph forged(graph.plan(),graph.renderSettings());
    RealtimeRenderSession realtime(forged);
    require(!realtime.ready(), "unsealed file plan must fail realtime admission");
    require(!OfflineRenderSession(forged).render(64).ok(), "unsealed file plan must fail offline admission");
    auto s=settings(); track(s,{0,0xb0,1,64});
    auto unboundController = compile(instrument(true), s);
    require(unboundController.renderSettings().eventValueChanges.empty(),
        "unreferenced file CC must not invent a bound Event Value");
    s=settings(); track(s,{0,0xa0,60,64});
    require(!compileProject(instrument(true),talas::core::makeBuiltinNodeRegistry(),s).ok(),
        "file channel pressure still requires an explicit prepared mapping");
    s=settings(); s.maxRealtimeEventTransactions=1;
    track(s,{0,0x90,60,100, 0,0x90,64,100});
    require(!compileProject(instrument(true),talas::core::makeBuiltinNodeRegistry(),s).ok(),
        "same-frame overflow must reject during preparation");
}

void testTransportForwardRepair()
{
    auto graph=compile(instrument(true),settings());
    RealtimeRenderSession session(graph);
    std::array<double,10> initialLeft{},initialRight{};
    require(session.render(initialLeft,initialRight), "render before host repair");
    // The host skipped the failed/absent interval [10,24). Release the file
    // voice rather than retaining the missed note-off at frame 20 forever.
    session.synchronizeTransportFrame(24);
    std::array<double,16> left{},right{},expected{};
    auto phase=std::fmod(10 * 440*std::pow(2.0,(60-69)/12.0)/1000,1.0);
    for (std::size_t f=0; f<16; ++f)
    {
        if (f>=11) expected[f]=80.0/127*std::sin(2*std::numbers::pi*phase);
        const auto pitch=f>=11 ? 64 : 60;
        phase=std::fmod(phase+440*std::pow(2.0,(pitch-69)/12.0)/1000,1.0);
    }
    require(session.render(left,right), "file cursor must survive forward host repair");
    expect(left,expected); expect(right,expected,0.5);
    RealtimeRenderSession seek(graph);
    RealtimeTransportSnapshot snapshot;
    snapshot.epoch=7; snapshot.sampleRate=1000; snapshot.playing=true;
    seek.setTransportSnapshotBeforeRender(snapshot);
    require(seek.render(initialLeft,initialRight), "render before host discontinuity");
    snapshot.epoch=8; snapshot.absoluteFrame=snapshot.playheadFrame=24;
    seek.setTransportSnapshotBeforeRender(snapshot);
    std::array<double,6> silentLeft{},silentRight{},silence{};
    require(seek.render(silentLeft,silentRight), "render after host discontinuity");
    expect(silentLeft,silence); expect(silentRight,silence);
}

void testIndependentNestedFileOwners()
{
    talas::project::ProjectDocument document;
    for (const auto* id : {"first", "second"})
    {
        auto body=instrument(true).graph;
        body.nodes.pop_back(); body.connections.resize(1);
        if (std::string_view(id)=="second")
        {
            body.nodes[0].parameters[0].expression="7 hz";
            body.nodes[1].parameters[7].resource="otherNotes";
        }
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId=id; definition.displayName=id;
        definition.graph=std::move(body);
        definition.outputs={{"leftOut",talas::core::PortType::Audio,"voice","leftOut"},
            {"rightOut",talas::core::PortType::Audio,"voice","rightOut"}};
        document.soundDefinitions.push_back(std::move(definition));
        talas::project::ProjectNode instance;
        instance.instanceId=id; instance.soundDefinitionId=id;
        document.graph.nodes.push_back(std::move(instance));
    }
    document.graph.nodes.push_back({"out","talas.audioOutput",{}});
    document.graph.connections={{"first","leftOut","out","audioIn"},
        {"second","rightOut","out","rightIn"}};
    auto s=settings(); auto other=settings();
    track(other,{10,0x90,67,80, 30,0x80,67,0});
    other.binaryResources.front().id="otherNotes";
    s.binaryResources.push_back(other.binaryResources.front());
    auto graph=compile(document,s);
    require(graph.plan().midiVoices.size()==2, "two nested file owners");
    std::vector<double> expectedRight(64);
    for (std::size_t f=15; f<40; ++f)
        expectedRight[f]=40.0/127*std::sin(2*std::numbers::pi*7*f/1000);
    auto offline=OfflineRenderSession(graph).render(64);
    require(offline.ok(), "nested offline render");
    expect(offline.mono,oracle()); expect(offline.right,expectedRight);
    for (const std::size_t block : {1,7,64})
    {
        RealtimeRenderSession session(graph); require(session.ready(),session.error());
        std::vector<double> left(64),right(64);
        for (std::size_t f=0; f<64; f+=block)
            require(session.render(std::span(left).subspan(f,std::min(block,64-f)),
                std::span(right).subspan(f,std::min(block,64-f))), "nested realtime render");
        expect(left,oracle()); expect(right,expectedRight);
    }
}

void testOfflineResetPreservesSoundSelection()
{
    auto document=instrument(true);
    for (const auto& [id,amplitude] : std::array{std::pair{"toneA","0"},std::pair{"toneB","1"}})
    {
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId=id; definition.displayName=id;
        definition.graph.nodes={{"osc","talas.oscillator",{{"frequency","7 hz"},{"amplitude",amplitude}}}};
        definition.outputs={{"audioOut",talas::core::PortType::Audio,"osc","audioOut"}};
        document.soundDefinitions.push_back(std::move(definition));
    }
    talas::project::ProjectSoundVariableDeclaration selected;
    selected.variableId="selected"; selected.displayName="Selected";
    selected.candidateDefinitionIds={"toneA","toneB"};
    document.soundVariableLibrary.variables={selected};
    document.graph.nodes.push_back({"selector","talas.variable",{text("variableId","selected")}});
    document.graph.connections.back()={"selector","audioOut","out","rightIn"};
    auto graph=compile(document,settings());
    OfflineMidiVoiceEvent selection;
    selection.batchId=selection.sequence=1;
    selection.kind=OfflineMidiVoiceEventKind::SoundVariableAssignment;
    selection.soundTarget=graph.preparedSoundVariables().cells.front().identity;
    selection.selectedDefinitionId="toneB";
    OfflineRenderSession offline(graph);
    require(offline.scheduleMidiVoiceEvents(std::span{&selection,1}), "schedule non-voice Sound selection");
    require(offline.render(8).ok(), "render Sound selection");
    require(offline.soundVariableValues().front().selectedDefinitionId=="toneB", "publish selected Sound");
    offline.reset();
    require(offline.soundVariableValues().front().selectedDefinitionId=="toneB", "file backend reset must preserve selected Sound");
    auto audio=offline.render(8); require(audio.ok(), "render preserved Sound after reset");
    std::array<double,8> expected{};
    for (std::size_t f=0; f<8; ++f) expected[f]=std::sin(2*std::numbers::pi*7*f/1000);
    expect(audio.right,expected);
}

void testPerformanceTransferKeepsFilePosition()
{
    auto document=instrument(true);
    document.graph.nodes[0].parameters[0].expression="7 hz";
    auto graph=compile(document,settings());
    RealtimeRenderSession source(graph),target(graph);
    RealtimeTransportSnapshot snapshot;
    snapshot.epoch=7; snapshot.sampleRate=1000; snapshot.playing=true;
    source.setTransportSnapshotBeforeRender(snapshot);
    std::array<double,10> prefixLeft{},prefixRight{};
    require(source.render(prefixLeft,prefixRight), "transfer source prefix");
    target.resetTransport();
    require(target.transferPerformanceStateFrom(source), "transfer file performance state");
    target.synchronizeTransportFrame(10);
    snapshot.absoluteFrame=snapshot.playheadFrame=10;
    target.setTransportSnapshotBeforeRender(snapshot);
    std::array<double,16> left{},right{},expected{};
    for (std::size_t f=0; f<10; ++f) expected[f]=100.0/127*std::sin(2*std::numbers::pi*7*f/1000);
    require(target.render(left,right), "render transferred file voice");
    expect(left,expected); expect(right,expected,0.5);
    auto changed=settings(); track(changed,{0,0x90,72,100});
    auto differentGraph=compile(document,changed);
    RealtimeRenderSession incompatible(differentGraph);
    require(!incompatible.performanceStateCompatibleWith(source), "different file stream cannot inherit a cursor");
}

void useClassicAnalogSequencerArrays(talas::project::ProjectDocument& document)
{
    auto& parameters = document.graph.nodes[1].parameters;
    for (auto& parameter : parameters)
    {
        if (parameter.parameterId == "keyPitches") parameter.parameterId = "pitches";
        if (parameter.parameterId == "keyVelocities") parameter.parameterId = "velocities";
    }
    std::erase_if(parameters, [](const auto& parameter) {
        return parameter.parameterId == "keyDowns" || parameter.parameterId == "keyTimbres";
    });
}

void testAnalogSequencerClassicArraysBindUpstreamVoice()
{
    auto document = instrument(false);
    document.graph.nodes[0].typeId = "talas.valToSound";
    document.graph.nodes[0].parameters = {{"value",
        "(!KeyDown > 0) * (0.1 + !KeyVelocity + !KeyTimbre + "
        "0.1 * (!Pitch > 61 nn) + 0.01 * (!KeyNumber > 62.25 nn) + "
        "0.001 * (!KeyPitch > 64 nn))"}};
    document.graph.nodes[1].typeId = "talas.analogSequencer";
    document.graph.nodes[1].parameters = {
        {"left", "1"}, {"right", "0.5"},
        {"gate", "1"}, {"step", "1"}, {"rate", "1"},
        {"startIndex", "0"}, {"endIndex", "2"}, {"polyphony", "2"},
        text("durations", "{10 ms} {20 ms} {10 ms}"),
        text("dutyCycles", "#(1 0.5 1)"),
        text("pitches", "#(60 62.5 65)"), text("velocities", "#(0.2 0)"),
        // A DutyCycle of 1 retriggers here, as before format 76.
        choice("dutyCycleOne", "legacyRetriggers"),
    };
    auto s = settings();
    s.eventValues.push_back({"KeyNumber", 99, talas::expr::ValueDomain::NoteNumber});
    s.eventValues.push_back({"KeyTimbre", 0.8});
    for (const bool reverse : {false, true})
    {
        auto selected = document;
        std::ranges::find(selected.graph.nodes[1].parameters, "rate",
            &talas::project::ProjectParameterExpression::parameterId)->expression =
                reverse ? "-0.5" : "1";
        std::array<double, 96> expected{};
        if (reverse)
        {
            std::fill(expected.begin() + 6, expected.begin() + 21, 0.211);
            std::fill(expected.begin() + 26, expected.begin() + 41, 0.21);
            std::fill(expected.begin() + 66, expected.begin() + 81, 0.3);
        }
        else
        {
            std::fill(expected.begin() + 6, expected.begin() + 11, 0.3);
            std::fill(expected.begin() + 16, expected.begin() + 21, 0.21);
            std::fill(expected.begin() + 36, expected.begin() + 41, 0.211);
        }
        for (const bool nested : {false, true})
        {
            auto authored = selected;
            if (nested)
            {
                talas::project::ProjectSoundDefinition definition;
                definition.definitionId = "classic";
                definition.displayName = "Classic sequence";
                definition.graph = authored.graph;
                definition.graph.nodes.pop_back();
                definition.graph.connections.resize(1);
                definition.outputs = {
                    {"leftOut", talas::core::PortType::Audio, "voice", "leftOut"},
                    {"rightOut", talas::core::PortType::Audio, "voice", "rightOut"}};
                authored.soundDefinitions.push_back(std::move(definition));
                talas::project::ProjectNode instance;
                instance.instanceId = "sequence";
                instance.soundDefinitionId = "classic";
                authored.graph.nodes = {instance, {"out", "talas.audioOutput", {}}};
                authored.graph.connections = {
                    {"sequence", "leftOut", "out", "audioIn"},
                    {"sequence", "rightOut", "out", "rightIn"}};
            }
            const auto reopened = talas::project::parseProjectDocument(
                talas::project::serializeProjectDocument(authored));
            require(reopened.ok(), "classic arrays save and reopen in root and reusable Sounds");
            auto configured = s;
            if (nested)
                std::erase_if(configured.eventValues, [](const auto& binding) {
                    return binding.name == "KeyNumber";
                });
            const auto graph = compile(reopened.document, configured);
            require(graph.plan().midiVoices.size() == 1
                    && graph.plan().midiVoices[0].voices.size() == 2,
                "classic Pitches select the two-voice input environment");
            const auto offline = OfflineRenderSession(graph).render(expected.size());
            require(offline.ok(), "classic note arrays render offline");
            expect(offline.mono, expected);
            expect(offline.right, expected, 0.5);
            for (const auto block : {1U, 7U, 64U})
            {
                RealtimeRenderSession session(graph);
                require(session.ready(), session.error());
                std::array<double, 96> left{}, right{};
                for (int pass = 0; pass < 2; ++pass)
                {
                    allocations.store(0);
                    countAllocations.store(true);
                    bool rendered = true;
                    for (std::size_t start = 0; start < left.size(); start += block)
                    {
                        const auto count = std::min<std::size_t>(block, left.size() - start);
                        rendered = session.render(std::span(left).subspan(start, count),
                            std::span(right).subspan(start, count)) && rendered;
                    }
                    countAllocations.store(false);
                    require(rendered && allocations.load() == 0,
                        "classic arrays preserve callback partitions and reset without allocation");
                    expect(left, expected);
                    expect(right, expected, 0.5);
                    session.resetTransport();
                }
            }
        }
    }
    auto& pitches = *std::ranges::find(document.graph.nodes[1].parameters, "pitches",
        &talas::project::ProjectParameterExpression::parameterId);
    for (const auto invalid : {"#(60 bad)", "{1 hz}"})
    {
        pitches.text = invalid;
        bool rejected = false;
        try { (void)talas::project::serializeProjectDocument(document); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "saving classic Pitches rejects malformed numbers and wrong units");
    }
}

void testAnalogSequencerKeyArraysBindUpstreamVoice()
{
    auto document = instrument(false);
    auto& oscillator = document.graph.nodes[0];
    oscillator.parameters[0].expression = "!KeyPitch";
    oscillator.parameters[1].expression =
        "(!KeyDown > 0) * (0.1 + !KeyVelocity + !KeyTimbre)";
    auto& sequencer = document.graph.nodes[1];
    sequencer.typeId = "talas.analogSequencer";
    sequencer.parameters = {
        {"left", "1"}, {"right", "0.5"},
        {"gate", "1"}, {"step", "1"}, {"rate", "1"},
        {"startIndex", "0"}, {"endIndex", "4"}, {"polyphony", "2"},
        text("durations", "{10 ms} {10 ms} {10 ms} {10 ms} {10 ms}"),
        text("dutyCycles", "#(1 1 1 1 1)"),
        text("keyDowns", "#(1 1 1 0 0)"),
        text("keyPitches", "#(60 62.5 65 67 69)"),
        text("keyVelocities", "#(0.2 0 0.8 0.9 0.9)"),
        text("keyTimbres", "#(0.1 0.3 0.7 0.8 0.9)"),
        // These fixtures retrigger at a DutyCycle of 1, as projects before
        // format 76 do.
        choice("dutyCycleOne", "legacyRetriggers"),
    };
    const auto saved = talas::project::serializeProjectDocument(
        document, talas::core::makeBuiltinNodeRegistry());
    const auto reopened = talas::project::parseProjectDocument(saved);
    require(reopened.ok(), reopened.errors.empty()
        ? "Analog Sequencer saved project failed to reopen"
        : reopened.errors.front().message);
    auto malformed = document;
    auto& malformedParameters = malformed.graph.nodes[1].parameters;
    std::ranges::find(malformedParameters, "keyDowns",
        &talas::project::ProjectParameterExpression::parameterId)->text =
            "#(1 invalid 0)";
    bool rejectedMalformed = false;
    try
    {
        (void)talas::project::serializeProjectDocument(
            malformed, talas::core::makeBuiltinNodeRegistry());
    }
    catch (const std::invalid_argument&)
    {
        rejectedMalformed = true;
    }
    require(rejectedMalformed,
        "saving an Analog Sequencer rejects malformed numeric key arrays");
    const auto graph = compile(reopened.document, settings());
    require(graph.plan().midiVoices.size() == 1
            && graph.plan().midiVoices[0].voices.size() == 2,
        "Analog Sequencer key source clones its upstream Sound twice");
    const auto offline = OfflineRenderSession(graph).render(60);
    require(offline.ok(), offline.diagnostics.empty()
        ? "Analog Sequencer offline render" : offline.diagnostics.front().message);
    require(std::abs(offline.mono[7]) > 0.01
            && std::abs(offline.mono[17]) > 0.01
            && std::abs(offline.mono[27]) > 0.01
            && std::all_of(offline.mono.begin() + 35, offline.mono.end(),
                [](double value) { return std::abs(value) < 1e-12; }),
        "three active stages sound, including zero velocity, while two inactive stages stay silent");
    for (const std::size_t block : {std::size_t{1}, std::size_t{7}, std::size_t{60}})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 60> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += block)
        {
            const auto count = std::min(block, left.size() - start);
            require(session.render(
                std::span(left).subspan(start, count),
                std::span(right).subspan(start, count)),
                "Analog Sequencer partitioned key render");
        }
        expect(left, offline.mono);
        expect(right, offline.mono, 0.5);
    }

    oscillator.typeId = "talas.valToSound";
    oscillator.parameters = {{"value",
        "(!KeyDown > 0) * (!KeyVelocity + !KeyTimbre + ((!KeyPitch > 62.25 nn) * 0.05))"}};
    const auto vectorGraph = compile(document, settings());
    const auto vectorOffline = OfflineRenderSession(vectorGraph).render(60);
    require(vectorOffline.ok(), vectorOffline.diagnostics.empty()
        ? "Analog Sequencer key-vector render"
        : vectorOffline.diagnostics.front().message);
    std::array<double, 60> vectorOracle{};
    // ValToSound publishes each changed key vector one sample later.
    for (std::size_t frame = 6; frame <= 10; ++frame)
        vectorOracle[frame] = 0.3;
    for (std::size_t frame = 16; frame <= 20; ++frame)
        vectorOracle[frame] = 0.35;
    for (std::size_t frame = 26; frame <= 30; ++frame)
        vectorOracle[frame] = 1.55;
    expect(vectorOffline.mono, vectorOracle);
    expect(vectorOffline.right, vectorOracle, 0.5);
    RealtimeRenderSession vectorSession(vectorGraph);
    require(vectorSession.ready(), vectorSession.error());
    std::array<double, 60> vectorLeft{}, vectorRight{};
    allocations.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = vectorSession.render(vectorLeft, vectorRight);
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocations.load(std::memory_order_relaxed) == 0,
        "Analog Sequencer realtime key-vector render");
    expect(vectorLeft, vectorOracle);
    expect(vectorRight, vectorOracle, 0.5);
    vectorSession.resetTransport();
    require(vectorSession.render(vectorLeft, vectorRight),
        "Analog Sequencer key-vector transport restart");
    expect(vectorLeft, vectorOracle);

    std::ranges::find(sequencer.parameters, "rate",
        &talas::project::ProjectParameterExpression::parameterId)->expression =
            "-1";
    const auto reverseSaved = talas::project::serializeProjectDocument(
        document, talas::core::makeBuiltinNodeRegistry());
    const auto reverseReopened = talas::project::parseProjectDocument(
        reverseSaved);
    require(reverseReopened.ok(),
        "reverse Analog Sequencer should save and reopen");
    const auto reverseGraph = compile(reverseReopened.document, settings());
    const auto reverseOffline = OfflineRenderSession(reverseGraph).render(60);
    require(reverseOffline.ok(),
        reverseOffline.diagnostics.empty()
            ? "reverse Analog Sequencer key-vector render"
            : reverseOffline.diagnostics.front().message);
    std::array<double, 60> reverseOracle{};
    for (std::size_t frame = 26; frame <= 30; ++frame)
        reverseOracle[frame] = 1.55;
    for (std::size_t frame = 36; frame <= 40; ++frame)
        reverseOracle[frame] = 0.35;
    for (std::size_t frame = 46; frame <= 50; ++frame)
        reverseOracle[frame] = 0.3;
    expect(reverseOffline.mono, reverseOracle);
    expect(reverseOffline.right, reverseOracle, 0.5);
    RealtimeRenderSession reverseSession(reverseGraph);
    require(reverseSession.ready(), reverseSession.error());
    std::array<double, 60> reverseLeft{}, reverseRight{};
    allocations.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto reverseRendered = reverseSession.render(
        reverseLeft, reverseRight);
    countAllocations.store(false, std::memory_order_relaxed);
    require(reverseRendered
            && allocations.load(std::memory_order_relaxed) == 0,
        "reverse Analog Sequencer key-vector callback should not allocate");
    expect(reverseLeft, reverseOracle);
    expect(reverseRight, reverseOracle, 0.5);
}

void testAnalogSequencerShortArraysRepeatFinalValuesAcrossVoices()
{
    auto shortDocument = instrument(false);
    shortDocument.graph.nodes[0].typeId = "talas.valToSound";
    shortDocument.graph.nodes[0].parameters = {
        {"value", "(!KeyDown > 0) * (!KeyVelocity + !KeyTimbre + ((!KeyPitch > 61 nn) * 0.1))"}};
    auto& sequencer = shortDocument.graph.nodes[1];
    sequencer.typeId = "talas.analogSequencer";
    sequencer.parameters = {
        {"left", "1"}, {"right", "0.5"},
        {"gate", "1"}, {"step", "1"}, {"rate", "1"},
        {"startIndex", "0"}, {"endIndex", "2"},
        {"polyphony", "2"},
        text("durations", "{10 ms} {10 ms} {10 ms}"),
        text("dutyCycles", "#(1)"),
        text("keyDowns", "#(1 1 1)"),
        text("keyPitches", "#(60 62)"),
        text("keyVelocities", "#(0.2)"),
        text("keyTimbres", "#(0.1)"),
        // These fixtures retrigger at a DutyCycle of 1, as projects before
        // format 76 do.
        choice("dutyCycleOne", "legacyRetriggers"),
    };
    const auto saved = talas::project::serializeProjectDocument(
        shortDocument, talas::core::makeBuiltinNodeRegistry());
    const auto reopened = talas::project::parseProjectDocument(saved);
    require(reopened.ok(),
        "short-array Analog Sequencer Sound should save and reopen");

    auto expanded = reopened.document;
    auto& expandedParameters = expanded.graph.nodes[1].parameters;
    for (auto& parameter : expandedParameters)
    {
        if (parameter.parameterId == "dutyCycles")
            parameter.text = "#(1 1 1)";
        else if (parameter.parameterId == "keyPitches")
            parameter.text = "#(60 62 62)";
        else if (parameter.parameterId == "keyVelocities")
            parameter.text = "#(0.2 0.2 0.2)";
        else if (parameter.parameterId == "keyTimbres")
            parameter.text = "#(0.1 0.1 0.1)";
    }
    const auto shortGraph = compile(reopened.document, settings());
    const auto expandedGraph = compile(expanded, settings());
    const auto shortOffline = OfflineRenderSession(shortGraph).render(36);
    const auto expandedOffline = OfflineRenderSession(expandedGraph).render(36);
    require(shortOffline.ok() && expandedOffline.ok(),
        "both short and explicit Analog Sequencer arrays should render");
    expect(shortOffline.mono, expandedOffline.mono);
    require(shortOffline.mono[7] > 0.1
            && shortOffline.mono[17] > shortOffline.mono[7]
            && std::abs(shortOffline.mono[27] - shortOffline.mono[17]) < 1e-12,
        "third stage should repeat the second pitch and final velocity/timbre");

    RealtimeRenderSession session(shortGraph);
    require(session.ready(), session.error());
    std::array<double, 36> left{}, right{};
    std::size_t offset = 0;
    for (const auto count : {7U, 5U, 11U, 13U})
    {
        allocations.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        const auto rendered = session.render(
            std::span{left}.subspan(offset, count),
            std::span{right}.subspan(offset, count));
        countAllocations.store(false, std::memory_order_relaxed);
        require(rendered
                && allocations.load(std::memory_order_relaxed) == 0,
            "short key-array callbacks should not allocate");
        offset += count;
    }
    expect(left, shortOffline.mono);
    expect(right, shortOffline.right);
}

void testAnalogSequencerPolyphonicReleaseOverlap()
{
    const auto makeDocument = [](int polyphony) {
        talas::project::ProjectDocument document;
        document.graph.nodes = {
            {"tone", "talas.valToSound", {{"value", "1"}}},
            {"envelope", "talas.ar", {{"attackTime", "1 ms"},
                {"releaseTime", "30 ms"}, {"gate", "!KeyDown"}}},
            {"gain", "talas.vca", {{"gain", "1"}}},
            {"seq", "talas.analogSequencer", {
                {"left", "1"}, {"right", "0"},
                {"gate", "1"}, {"step", "1"}, {"rate", "1"},
                {"startIndex", "0"}, {"endIndex", "1"},
                {"polyphony", std::to_string(polyphony)},
                text("durations", "#(0.01 0.01)"),
                text("dutyCycles", "#(1 1)"),
                text("keyDowns", "#(1 1)"),
                text("keyPitches", "#(60 64)"),
                text("keyVelocities", "#(0.8 0.6)"),
                text("keyTimbres", "#(0.1 0.7)"),
                // A DutyCycle of 1 retriggers here, as before format 76.
                choice("dutyCycleOne", "legacyRetriggers"),
            }},
            {"out", "talas.audioOutput", {}},
        };
        document.graph.connections = {
            {"tone", "audioOut", "gain", "audioIn"},
            {"envelope", "value", "gain", "gain"},
            {"gain", "audioOut", "seq", "audioIn"},
            {"seq", "leftOut", "out", "audioIn"},
        };
        return document;
    };
    const auto monoGraph = compile(makeDocument(1), settings());
    const auto polyGraph = compile(makeDocument(2), settings());
    const auto mono = OfflineRenderSession(monoGraph).render(48);
    const auto poly = OfflineRenderSession(polyGraph).render(48);
    require(mono.ok() && poly.ok(),
        mono.ok() ? (poly.diagnostics.empty()
            ? "polyphonic Analog Sequencer render failed"
            : poly.diagnostics.front().message)
        : (mono.diagnostics.empty()
            ? "monophonic Analog Sequencer render failed"
            : mono.diagnostics.front().message));
    require(polyGraph.plan().midiVoices[0].voices.size() == 2
            && poly.mono[17] > mono.mono[17] + 0.1
            && poly.mono[25] > mono.mono[25] + 0.1,
        "polyphonic voices retain independent release tails after later attacks");
    RealtimeRenderSession session(polyGraph);
    require(session.ready(), session.error());
    std::array<double, 48> left{}, right{};
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min<std::size_t>(7, left.size() - start);
        require(session.render(std::span(left).subspan(start, count),
            std::span(right).subspan(start, count)),
            "Analog Sequencer overlapping prepared render");
    }
    expect(left, poly.mono);
}

// Projects before format 76 cannot name dutyCycleOne; migration supplies it.
std::string withoutDutyCycleOne(talas::project::ProjectDocument document)
{
    const auto drop = [](talas::project::ProjectGraph& graph) {
        for (auto& node : graph.nodes)
            std::erase_if(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "dutyCycleOne";
            });
    };
    drop(document.graph);
    for (auto& definition : document.soundDefinitions)
        drop(definition.graph);
    return talas::project::serializeProjectDocument(document);
}

void testAnalogSequencerZeroBasedRangeAndLegacyMigration()
{
    auto document = instrument(false);
    document.graph.nodes[0].typeId = "talas.valToSound";
    document.graph.nodes[0].parameters = {
        {"value", "(!KeyDown > 0) * !KeyVelocity"}};
    auto& sequencer = document.graph.nodes[1];
    sequencer.typeId = "talas.analogSequencer";
    sequencer.parameters = {
        {"left", "1"}, {"right", "0.5"},
        {"gate", "1"}, {"step", "1"}, {"rate", "1"},
        {"startIndex", "1"}, {"endIndex", "2"}, {"polyphony", "1"},
        text("durations", "{10 ms} {10 ms} {10 ms} {10 ms}"),
        text("dutyCycles", "#(1)"), text("keyDowns", "#(1)"),
        text("keyPitches", "#(60)"),
        text("keyVelocities", "#(0.1 0.3 0.7 0.9)"),
        text("keyTimbres", "#(0)"),
        // These fixtures retrigger at a DutyCycle of 1, as projects before
        // format 76 do.
        choice("dutyCycleOne", "legacyRetriggers"),
    };
    const auto current = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document));
    require(current.ok(), "zero-based key range should save and reopen");

    std::ranges::find(sequencer.parameters, "startIndex",
        &talas::project::ProjectParameterExpression::parameterId)->expression = "2";
    std::ranges::find(sequencer.parameters, "endIndex",
        &talas::project::ProjectParameterExpression::parameterId)->expression = "3";
    auto historical = withoutDutyCycleOne(document);
    const auto version = historical.find("\"version\": 156");
    require(version != std::string::npos,
        "migration source should begin with the current project version");
    historical.replace(version, std::string_view{"\"version\": 156"}.size(),
        "\"version\":  38");
    const auto migrated = talas::project::parseProjectDocument(historical);
    require(migrated.ok(), "format-38 key range should migrate");
    const auto legacy = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(migrated.document));
    require(legacy.ok(), "migrated key range should save and reopen");

    std::vector<double> expected;
    for (const auto* source : {&current.document, &legacy.document})
    {
        const auto graph = compile(*source, settings());
        const auto offline = OfflineRenderSession(graph).render(26);
        require(offline.ok(), "selected key range should render offline");
        require(std::abs(offline.mono[7] - 0.3) < 1e-12
                && std::abs(offline.mono[17] - 0.7) < 1e-12,
            "selected range must emit the middle two velocities");
        if (expected.empty())
            expected = offline.mono;
        else
            expect(offline.mono, expected);

        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 26> left{}, right{};
        std::size_t offset = 0;
        for (const auto count : {3U, 8U, 2U, 13U})
        {
            allocations.store(0, std::memory_order_relaxed);
            countAllocations.store(true, std::memory_order_relaxed);
            const auto rendered = session.render(
                std::span{left}.subspan(offset, count),
                std::span{right}.subspan(offset, count));
            countAllocations.store(false, std::memory_order_relaxed);
            require(rendered
                    && allocations.load(std::memory_order_relaxed) == 0,
                "selected key-range callbacks should not allocate");
            offset += count;
        }
        expect(left, expected);
        expect(right, expected, 0.5);
    }
}

talas::project::ProjectDocument analogGateInstrument(bool looping)
{
    auto document = instrument(false);
    document.graph.nodes[0].typeId = "talas.valToSound";
    document.graph.nodes[0].parameters = {
        {"value", "(!KeyDown > 0) * !KeyVelocity"}};
    auto& sequencer = document.graph.nodes[1];
    sequencer.typeId = "talas.analogSequencer";
    talas::project::ProjectParameterExpression loop;
    loop.parameterId = "loop";
    loop.boolean = looping;
    sequencer.parameters = {
        {"left", "1"}, {"right", "0.5"},
        {"gate", "!SeqGate"}, {"step", "1"}, {"rate", "1"},
        {"startIndex", "0"}, {"endIndex", "1"}, {"polyphony", "1"}, loop,
        text("durations", "#(0.01 0.01)"), text("dutyCycles", "#(1)"),
        text("keyDowns", "#(1)"), text("keyPitches", "#(60 64)"),
        text("keyVelocities", "#(0.2 0.7)"), text("keyTimbres", "#(0)"),
        // These fixtures retrigger at a DutyCycle of 1, as projects before
        // format 76 do.
        choice("dutyCycleOne", "legacyRetriggers"),
    };
    return document;
}

void testAnalogSequencerLiveGateRestartsAndReleases()
{
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(analogGateInstrument(true)));
    require(reopened.ok(), "Gate-controlled key Sound should save and reopen");
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 0});
    s.eventValueChanges = {
        {3, "SeqGate", 1}, {10, "SeqGate", 0},
        {19, "SeqGate", 1}, {40, "SeqGate", 2}, {42, "SeqGate", -1},
        {52, "SeqGate", 0}, {60, "SeqGate", 1}, {62, "SeqGate", 0},
        {70, "SeqGate", 1}, {90, "SeqGate", 0},
    };
    const auto graph = compile(reopened.document, s);
    std::array<double, 96> expected{};
    // Five-frame voice retrigger plus the existing one-frame ValToSound
    // delay. Gate releases and stage transitions reach its output one frame later.
    for (const auto [first, last, value] : {
             std::tuple{9U, 10U, 0.2}, {25U, 29U, 0.2}, {35U, 39U, 0.7},
             {45U, 49U, 0.2}, {76U, 80U, 0.2}, {86U, 90U, 0.7}})
        std::fill(expected.begin() + first, expected.begin() + last + 1, value);
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), offline.diagnostics.empty()
        ? "live Gate offline render" : offline.diagnostics.front().message);
    expect(offline.mono, expected);
    expect(offline.right, expected, 0.5);

    for (const auto blockSize : {1U, 7U, 64U})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 96> left{}, right{};
        for (int pass = 0; pass < 2; ++pass)
        {
            allocations.store(0);
            countAllocations.store(true);
            std::array<double*, 2> channels{left.data(), right.data()};
            bool rendered = session.render({}, channels.data(), channels.size(), 0);
            auto failedOffset = rendered ? -1 : -2;
            for (std::size_t offset = 0; offset < left.size(); offset += blockSize)
            {
                const auto count = std::min<std::size_t>(blockSize, left.size() - offset);
                const auto blockRendered = session.render(std::span(left).subspan(offset, count),
                    std::span(right).subspan(offset, count));
                if (!blockRendered && failedOffset == -1)
                    failedOffset = static_cast<int>(offset);
                rendered = blockRendered && rendered;
            }
            countAllocations.store(false);
            require(rendered && allocations.load() == 0,
                "Gate callbacks: block=" + std::to_string(blockSize)
                    + " pass=" + std::to_string(pass)
                    + " rendered=" + std::to_string(rendered)
                    + " firstFailure=" + std::to_string(failedOffset)
                    + " allocations=" + std::to_string(allocations.load()));
            expect(left, expected);
            expect(right, expected, 0.5);
            session.resetTransport();
        }
    }

    RealtimeRenderSession source(graph), transferred(graph);
    std::array<double, 47> prefixLeft{}, prefixRight{};
    require(source.render(prefixLeft, prefixRight)
            && transferred.transferPerformanceStateFrom(source),
        "performance transfer retains the Gate epoch, held note and cursor");
    transferred.synchronizeTransportFrame(47);
    std::array<double, 49> left{}, right{};
    require(transferred.render(left, right), "transferred Gate sequence should continue");
    expect(left, std::span(expected).subspan(47));
    expect(right, std::span(expected).subspan(47), 0.5);
    auto differentTimeline = s;
    differentTimeline.eventValueChanges.front().frame = 4;
    const auto changedGraph = compile(reopened.document, differentTimeline);
    RealtimeRenderSession changed(changedGraph);
    require(!changed.performanceStateCompatibleWith(source),
        "a different Gate automation timeline cannot inherit the consumed edge cursor");

    // Loop off must finish once, stay finished while Gate is held, then restart.
    const auto singleGraph = compile(analogGateInstrument(false), s);
    auto singleExpected = expected;
    std::fill(singleExpected.begin() + 45, singleExpected.begin() + 50, 0.0);
    const auto single = OfflineRenderSession(singleGraph).render(singleExpected.size());
    require(single.ok(), "non-looping Gate restart should render");
    expect(single.mono, singleExpected);
}

void testAnalogSequencerGateCallbackRollback()
{
    auto document = analogGateInstrument(true);
    document.graph.nodes[0].parameters[0].expression =
        "(!KeyDown > 0) * !KeyVelocity / (1 - (!KeyDown > 0) * (!KeyVelocity > 0.5) * !Fault)";
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 0});
    s.eventValues.push_back({"Fault", 1});
    s.eventValueChanges = {
        {3, "SeqGate", 1}, {10, "SeqGate", 0}, {19, "SeqGate", 1}};
    const auto graph = compile(document, s);
    RealtimeRenderSession session(graph), reference(graph);
    require(session.ready() && reference.ready(), "Gate rollback sessions prepare");
    std::array<double, 64> left{}, right{}, expectedLeft{}, expectedRight{};
    allocations.store(0);
    countAllocations.store(true);
    const auto failed = session.render(left, right);
    countAllocations.store(false);
    require(!failed && allocations.load() == 0,
        "fault after Gate restart must roll back the callback without allocation");
    require(std::ranges::all_of(left, [](double value) { return value == 0; })
            && std::ranges::all_of(right, [](double value) { return value == 0; }),
        "rejected callback must silence both output channels");
    require(session.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
            && reference.render(expectedLeft, expectedRight),
        "repair the Gate voice expression and render a fresh reference");
    allocations.store(0);
    countAllocations.store(true);
    const auto recovered = session.render(left, right);
    countAllocations.store(false);
    require(recovered && allocations.load() == 0,
        "Gate epoch, held note and cursor must replay after rollback without allocation");
    expect(left, expectedLeft);
    expect(right, expectedRight);
}

void testAnalogSequencerGateLivePublication()
{
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 0});
    const auto graph = compile(analogGateInstrument(true), s);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 32> left{}, right{}, expected{};
    for (std::size_t frame = 10; frame <= 12; ++frame) expected[frame] = 0.2;
    for (std::size_t frame = 22; frame <= 26; ++frame) expected[frame] = 0.2;
    auto offset = std::size_t{0};
    for (const auto [gate, count] : {
             std::pair{0.0, 4U}, {1.0, 8U}, {0.0, 4U}, {1.0, 16U}})
    {
        require(session.updateEventValue("SeqGate", gate, talas::expr::ValueDomain::Scalar),
            "publish a live Gate value");
        allocations.store(0);
        countAllocations.store(true);
        const auto rendered = session.render(std::span(left).subspan(offset, count),
            std::span(right).subspan(offset, count));
        countAllocations.store(false);
        require(rendered && allocations.load() == 0,
            "live Gate publication applies at the callback boundary without allocation");
        offset += count;
    }
    expect(left, expected);
    expect(right, expected, 0.5);
}

void testAnalogSequencerGateKeepsReleaseTail()
{
    auto document = analogGateInstrument(true);
    document.graph.nodes[0].parameters = {{"value", "1"}};
    document.graph.nodes.push_back({"envelope", "talas.ar", {
        {"attackTime", "1 ms"}, {"releaseTime", "30 ms"}, {"gate", "!KeyDown"}}});
    document.graph.nodes.push_back({"gain", "talas.vca", {{"gain", "1"}}});
    document.graph.connections[0] = {"osc", "audioOut", "gain", "audioIn"};
    document.graph.connections.push_back({"envelope", "value", "gain", "gain"});
    document.graph.connections.push_back({"gain", "audioOut", "voice", "audioIn"});
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 1});
    s.eventValueChanges = {{8, "SeqGate", 0}};
    const auto graph = compile(document, s);
    const auto offline = OfflineRenderSession(graph).render(60);
    require(offline.ok(), "Gate release-tail Sound should render");
    require(offline.mono[7] > 0.9
            && offline.mono[9] > offline.mono[20]
            && offline.mono[20] > 0.0
            && std::abs(offline.mono[50]) < 1e-12,
        "closing Gate must release the upstream envelope and allow its finite tail to finish");
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 60> left{}, right{};
    allocations.store(0);
    countAllocations.store(true);
    bool rendered = true;
    for (std::size_t offset = 0; offset < left.size(); offset += 7)
    {
        const auto count = std::min<std::size_t>(7, left.size() - offset);
        rendered = session.render(std::span(left).subspan(offset, count),
            std::span(right).subspan(offset, count)) && rendered;
    }
    countAllocations.store(false);
    require(rendered && allocations.load() == 0,
        "Gate release tails should render without callback allocation");
    expect(left, offline.mono);
    expect(right, offline.right);
}

talas::project::ProjectDocument analogStepInstrument(bool looping = true)
{
    auto document = analogGateInstrument(looping);
    auto& parameters = document.graph.nodes[1].parameters;
    for (const auto [id, expression] : {
             std::pair{"step", "!SeqStep"}, {"endIndex", "2"}})
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->expression = expression;
    for (const auto [id, value] : {
             std::pair{"durations", "#(0.01 0.02 0.01)"},
             {"dutyCycles", "#(1 0.5 1)"}, {"keyPitches", "#(60 64 67)"},
             {"keyVelocities", "#(0.2 0.7 0.9)"}})
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->text = value;
    return document;
}

void connectAnalogKeyControl(talas::project::ProjectDocument& document, std::string_view id)
{
    auto& parameters = document.graph.nodes[1].parameters;
    auto& parameter = *std::ranges::find(parameters, id,
        &talas::project::ProjectParameterExpression::parameterId);
    const auto source = *parameter.expression;
    // The connected stream must override both disabled Gate and automatic Step.
    parameter.expression = id == "gate" ? "0" : "1";
    const auto producer = "connected-" + std::string{id};
    document.graph.nodes.push_back({producer, "talas.constant", {{"value", source}}});
    document.graph.connections.push_back({producer, "value", "voice", std::string{id}});
}

void wrapAnalogInstrument(talas::project::ProjectDocument& document)
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "connectedSequence";
    definition.displayName = "Connected sequence";
    definition.graph = document.graph;
    std::erase_if(definition.graph.nodes, [](const auto& node) { return node.instanceId == "out"; });
    std::erase_if(definition.graph.connections, [](const auto& input) { return input.toNode == "out"; });
    definition.outputs = {{"leftOut", talas::core::PortType::Audio, "voice", "leftOut"},
        {"rightOut", talas::core::PortType::Audio, "voice", "rightOut"}};
    document.soundDefinitions.push_back(std::move(definition));
    talas::project::ProjectNode instance;
    instance.instanceId = "sequence";
    instance.soundDefinitionId = "connectedSequence";
    document.graph.nodes = {instance, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"sequence", "leftOut", "out", "audioIn"},
        {"sequence", "rightOut", "out", "rightIn"}};
}

void verifyAnalogSequencerStepDurationMask(bool connected = false,
    bool classic = false, bool reusable = false)
{
    auto document = analogStepInstrument();
    if (classic) useClassicAnalogSequencerArrays(document);
    if (connected)
    {
        connectAnalogKeyControl(document, "gate");
        connectAnalogKeyControl(document, "step");
    }
    if (reusable) wrapAnalogInstrument(document);
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document));
    require(reopened.ok(), "Step-controlled Sound should save and reopen");
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 1});
    s.eventValues.push_back({"SeqStep", 0});
    s.eventValueChanges = {
        {3, "SeqStep", 0.25}, {4, "SeqStep", 0},
        {6, "SeqStep", 2}, {7, "SeqStep", 0},
        {12, "SeqStep", 1}, {14, "SeqStep", 0},
        {17, "SeqStep", 0.5}, {18, "SeqStep", 0},
        {27, "SeqStep", 2}, {28, "SeqStep", 0},
        {37, "SeqStep", 1}, {38, "SeqStep", 0},
        {47, "SeqStep", 1}, {48, "SeqStep", 0},
        {60, "SeqStep", 1}, {61, "SeqStep", 0},
        {65, "SeqGate", 0}, {66, "SeqGate", 1},
        {67, "SeqStep", 0.125}, {68, "SeqStep", 0},
        {79, "SeqGate", 0}, {82, "SeqGate", 1}, {82, "SeqStep", 1},
    };
    const auto graph = compile(reopened.document, s);
    std::array<double, 96> expected{};
    // Onsets: 3, 17, 37, 47, 60, 67, 82. The pulse at 12 is discarded,
    // including while it stays high across the mask expiry at 13. The pulse at
    // 27 is discarded even though DutyCycle has already released that note.
    // Gate at 65 cancels the attack due at that same frame; 82 resets then steps.
    for (const auto [first, last, value] : {
             std::tuple{9U, 13U, 0.2}, {23U, 27U, 0.7}, {43U, 47U, 0.9},
             {53U, 57U, 0.2}, {73U, 77U, 0.2}, {88U, 92U, 0.2}})
        std::fill(expected.begin() + first, expected.begin() + last + 1, value);
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), offline.diagnostics.empty()
        ? "Step duration-mask offline render" : offline.diagnostics.front().message);
    expect(offline.mono, expected);
    expect(offline.right, expected, 0.5);
    for (const auto blockSize : {1U, 7U, 64U})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 96> left{}, right{};
        for (int pass = 0; pass < 2; ++pass)
        {
            allocations.store(0);
            countAllocations.store(true);
            bool rendered = true;
            for (std::size_t offset = 0; offset < left.size(); offset += blockSize)
            {
                const auto count = std::min<std::size_t>(blockSize, left.size() - offset);
                std::array<double*, 2> channels{left.data() + offset, right.data() + offset};
                rendered = session.render({}, channels.data(), channels.size(), 0) && rendered;
                rendered = session.render(std::span(left).subspan(offset, count),
                    std::span(right).subspan(offset, count)) && rendered;
            }
            countAllocations.store(false);
            require(rendered && allocations.load() == 0,
                "Step masks, edges and reset must remain allocation-free across partitions");
            expect(left, expected);
            expect(right, expected, 0.5);
            session.resetTransport();
        }
    }
    RealtimeRenderSession source(graph), transferred(graph);
    std::array<double, 13> prefixLeft{}, prefixRight{};
    require(source.render(prefixLeft, prefixRight)
            && transferred.transferPerformanceStateFrom(source),
        "transfer while an ignored Step remains high across its duration mask");
    transferred.synchronizeTransportFrame(13);
    std::array<double, 83> left{}, right{};
    require(transferred.render(std::span(left).first(32), std::span(right).first(32))
            && transferred.render(std::span(left).subspan(32), std::span(right).subspan(32)),
        "transferred Step state should continue without replaying an ignored pulse");
    std::array<double, 83> transferredExpected{};
    std::copy(expected.begin() + 13, expected.end(), transferredExpected.begin());
    // Performance transfer retains event state. The new converter has fresh
    // DSP history, so the release at its first frame has no preceding sample.
    transferredExpected.front() = 0.0;
    expect(left, transferredExpected);
    expect(right, transferredExpected, 0.5);
}

void testAnalogSequencerStepDurationMask()
{
    verifyAnalogSequencerStepDurationMask();
}

void testAnalogSequencerConnectedControls()
{
    for (const bool classic : {false, true})
        for (const bool reusable : {false, true})
            verifyAnalogSequencerStepDurationMask(true, classic, reusable);
}

void testAnalogSequencerConnectedControlAuthority()
{
    for (const auto control : {"gate", "step", "rate"})
    {
        auto document = analogStepInstrument();
        connectAnalogKeyControl(document, control);
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"SeqStep", 0}});
        const auto original = compile(document, s);
        for (const int mutation : {0, 1, 2, 3})
        {
            auto forged = original;
            auto& plan = const_cast<RuntimePlan&>(forged.plan());
            auto& owner = plan.midiVoices.front();
            auto& inputs = plan.steps[owner.ownerStepIndex].inputs;
            auto& binding = *std::ranges::find(inputs, control, &InputBinding::inputPortId);
            if (mutation == 0) inputs.push_back(binding);
            else if (mutation == 1) plan.buffers[binding.bufferIndex].type = talas::core::PortType::Audio;
            else if (mutation == 2) binding.sourceScopeId = owner.voices.front().scopeId;
            else
            {
                const auto producer = std::ranges::find(plan.steps, binding.sourceNodeId, &CompileStep::nodeId);
                require(producer != plan.steps.end(), "connected key control has a prepared producer");
                std::swap(*producer, plan.steps[owner.ownerStepIndex]);
            }
            RealtimeRenderSession realtime(forged);
            require(!realtime.ready() && !OfflineRenderSession(forged).render(8).ok(),
                "duplicate, mistyped, rescoped or reordered key-control authority must reject");
        }
    }
}

void verifyAnalogSequencerConnectedAudioTriggers(bool chained, bool heldVoice)
{
    auto document = analogStepInstrument();
    std::ranges::find(document.graph.nodes[1].parameters, "step",
        &talas::project::ProjectParameterExpression::parameterId)->expression = "1";
    document.graph.nodes.insert(document.graph.nodes.end(), {
        {"clock", "talas.oscillator", {{"frequency", "25 hz"}, {"amplitude", "1"}}},
        {"follower", "talas.amplitudeFollower", {{"timeConstant", "1 ms"}, {"scale", "1"}}},
        {"threshold", "talas.threshold", {{"threshold", "0.6"}, {"hysteresis", "0.1"}}},
        {"tapCarrier", "talas.valToSound", {{"value", "1"}}},
        {"tap", "talas.vca", {}},
    });
    std::erase_if(document.graph.connections, [](const auto& input) { return input.toPort == "rightIn"; });
    document.graph.connections.insert(document.graph.connections.end(), {
        {"clock", "audioOut", "follower", "audioIn"},
        {"follower", "value", "threshold", "value"},
        {"threshold", "value", "voice", "step"},
        {"threshold", "value", "tap", "gain"},
        {"tapCarrier", "audioOut", "tap", "audioIn"},
        {"tap", "audioOut", "out", "rightIn"},
    });
    if (chained)
    {
        auto trigger = analogGateInstrument(true);
        trigger.graph.nodes[0].instanceId = "clock";
        trigger.graph.nodes[0].parameters = {{"value", "!KeyDown > 0"}};
        trigger.graph.nodes[1].instanceId = "triggerVoice";
        std::ranges::find(trigger.graph.nodes[1].parameters, "gate",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "0";
        document.graph.nodes[3] = std::move(trigger.graph.nodes[0]);
        document.graph.nodes.push_back(std::move(trigger.graph.nodes[1]));
        document.graph.nodes.push_back({"triggerGate", "talas.constant", {{"value", "1"}}});
        auto& followerInput = *std::ranges::find(document.graph.connections, "follower",
            &talas::core::Connection::toNode);
        followerInput.fromNode = "triggerVoice";
        followerInput.fromPort = "leftOut";
        document.graph.connections.push_back({"clock", "audioOut", "triggerVoice", "audioIn"});
        document.graph.connections.push_back({"triggerGate", "value", "triggerVoice", "gate"});
    }
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 1});
    auto probe = document;
    std::erase_if(probe.graph.nodes, [](const auto& node) {
        return node.instanceId == "voice" || node.instanceId == "osc";
    });
    std::erase_if(probe.graph.connections, [](const auto& input) {
        return input.fromNode == "voice" || input.toNode == "voice" || input.fromNode == "osc"
            || input.toNode == "out";
    });
    probe.graph.connections.push_back({"tap", "audioOut", "out", "audioIn"});
    const auto probeGraph = compile(probe, s);
    const auto isolatedControl = OfflineRenderSession(probeGraph).render(96);
    require(isolatedControl.ok(), "isolated audio follower and threshold should render");
    // The independent control graph supplies an edge trace, not a sequencer
    // output oracle. Apply the documented duration mask and existing 5 ms voice
    // retrigger plus one-sample converter delay to those observed edges.
    std::array<double, 96> expected{};
    const std::array<std::size_t, 3> durations{10, 20, 10}, releases{10, 10, 10};
    const std::array<double, 3> velocities{0.2, 0.7, 0.9};
    std::size_t stage = 0, nextAccepted = 0, accepted = 0;
    bool high = false;
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
    {
        const bool nextHigh = isolatedControl.mono[frame] > 0;
        if (nextHigh && !high && frame >= nextAccepted)
        {
            ++accepted;
            nextAccepted = frame + durations[stage];
            // WaitUntil leaves the converter uninitialized until the first
            // attack. Its first sample starts at that input value immediately;
            // subsequent value changes retain the one-sample interpolation.
            const auto attackDelay = heldVoice && accepted == 1 ? 5U : 6U;
            for (std::size_t audible = frame + attackDelay;
                 audible < std::min(expected.size(), frame + releases[stage] + 1); ++audible)
                expected[audible] = velocities[stage];
            stage = (stage + 1) % durations.size();
        }
        high = nextHigh;
    }
    require(accepted >= 4, "audio trigger fixture must traverse and loop the sequence");
    if (heldVoice)
    {
        document.graph.nodes.push_back({"voiceReady", "talas.constant", {{"value", "!KeyDown > 0"}}});
        document.graph.nodes.push_back({"held", "talas.waitUntil", {{"resume", "0"}}});
        auto& voiceInput = *std::ranges::find(document.graph.connections, "osc",
            &talas::core::Connection::fromNode);
        voiceInput.toNode = "held";
        document.graph.connections.push_back({"held", "audioOut", "voice", "audioIn"});
        document.graph.connections.push_back({"voiceReady", "value", "held", "resume"});
    }
    const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
    require(reopened.ok(), "audio-triggered sequence should save and reopen");
    const auto graph = compile(reopened.document, s);
    if (heldVoice)
    {
        auto forged = graph;
        auto& plan = const_cast<RuntimePlan&>(forged.plan());
        auto& held = *std::ranges::find(plan.steps, "talas.waitUntil", &CompileStep::typeId);
        held.structuralScopeId = held.scopeId;
        RealtimeRenderSession rejected(forged);
        require(!rejected.ready(), "voice WaitUntil retains its sealed authored structural scope");
    }
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), offline.diagnostics.empty() ? "audio-triggered sequence should render"
        : offline.diagnostics.front().message);
    expect(offline.right, isolatedControl.mono);
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
        require(std::abs(offline.mono[frame] - expected[frame]) < 1e-10,
            "audio-triggered offline PCM held=" + std::to_string(heldVoice)
                + " frame=" + std::to_string(frame) + " actual=" + std::to_string(offline.mono[frame])
                + " expected=" + std::to_string(expected[frame]));
    for (const std::size_t block : {1U, 9U, 64U})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        for (int pass = 0; pass < 2; ++pass)
        {
            std::array<double, 96> left{}, right{};
            allocations.store(0); countAllocations.store(true);
            bool rendered = session.render(std::span(left).first(0), std::span(right).first(0));
            for (std::size_t offset = 0; offset < left.size(); offset += block)
            {
                const auto count = std::min(block, left.size() - offset);
                rendered = session.render(std::span(left).subspan(offset, count),
                    std::span(right).subspan(offset, count)) && rendered;
            }
            countAllocations.store(false);
            require(rendered && allocations.load() == 0,
                "shared audio control producers must execute once without allocation; block="
                    + std::to_string(block) + " pass=" + std::to_string(pass)
                    + " rendered=" + std::to_string(rendered)
                    + " allocations=" + std::to_string(allocations.load()));
            expect(left, expected);
            expect(right, isolatedControl.mono);
            session.resetTransport();
        }
    }
    document.graph.nodes[0].parameters[0].expression =
        "(!KeyDown > 0) * !KeyVelocity / (1 - (!KeyDown > 0) * (!KeyVelocity > 0.5) * !Fault)";
    s.eventValues.push_back({"Fault", 1});
    RealtimeRenderSession failed(compile(document, s));
    require(failed.ready(), failed.error());
    std::array<double, 64> left{}, right{};
    allocations.store(0); countAllocations.store(true);
    const auto bad = failed.render(left, right);
    countAllocations.store(false);
    require(!bad && allocations.load() == 0
            && std::ranges::all_of(left, [](double value) { return value == 0; }),
        "failed connected note activation must silence and roll back the whole callback");
    require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar),
        "repair the connected voice expression");
    allocations.store(0); countAllocations.store(true);
    const auto recovered = failed.render(left, right);
    countAllocations.store(false);
    require(recovered && allocations.load() == 0,
        "retry must restore oscillator, follower, threshold, notes and the edge cursor");
    expect(left, std::span(expected).first(64));
    expect(right, std::span(isolatedControl.mono).first(64));
}

void testAnalogSequencerConnectedAudioTriggersAndRollback()
{
    verifyAnalogSequencerConnectedAudioTriggers(false, false);
}

void testAnalogSequencerChainedConnectedControls()
{
    verifyAnalogSequencerConnectedAudioTriggers(true, false);
    verifyAnalogSequencerConnectedAudioTriggers(true, true);
}

void verifyAnalogSequencerLiveRate(bool classic, bool connected, bool manual,
    bool looping, bool reusable)
{
    auto document = analogStepInstrument(looping);
    auto& parameters = document.graph.nodes[1].parameters;
    for (const auto [id, value] : {std::pair{"durations", "#(0.02)"},
             {"dutyCycles", "#(0.75)"}})
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->text = value;
    std::ranges::find(parameters, "rate",
        &talas::project::ProjectParameterExpression::parameterId)->expression = "!SeqRate";
    if (!manual)
        std::ranges::find(parameters, "step",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "1";
    parameters.push_back(text("extraValues", "#(!Morph 0.2 0.7 0.9)"));
    document.graph.nodes[0].parameters[0].expression =
        "(!KeyDown > 0) * (!KeyVelocity + !Morph) / 2";
    if (classic) useClassicAnalogSequencerArrays(document);
    if (connected) connectAnalogKeyControl(document, "rate");
    if (reusable) wrapAnalogInstrument(document);
    const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
    require(reopened.ok(), "live Rate Sound must save and reopen");
    auto s = settings();
    s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"SeqRate", 1}, {"SeqStep", 1}});
    s.eventValueChanges = {{8, "SeqRate", 2}, {20, "SeqRate", 0.5}, {60, "SeqRate", -1}};
    if (manual)
    {
        s.eventValueChanges.push_back({1, "SeqStep", 0});
        for (const auto frame : {5U, 8U, 12U, 14U, 30U, 36U, 60U, 68U, 88U, 108U})
        {
            s.eventValueChanges.push_back({frame, "SeqStep", 1});
            s.eventValueChanges.push_back({frame + 1, "SeqStep", 0});
        }
        std::ranges::stable_sort(s.eventValueChanges,
            [](const auto& a, const auto& b) { return a.frame < b.frame; });
    }
    const auto graph = compile(reopened.document, s);
    // Rate changes during active notes retain elapsed duration. At frames
    // 14/36/68/88/108 the clock reaches successive 20-unit stage budgets;
    // the sign change at 60 traverses 2 -> 1 -> 0 without repitching stage 2.
    std::array<double, 128> expected{};
    for (const auto [first, last, value] : {
             std::tuple{6U, 12U, 0.2}, {20U, 28U, 0.7}, {42U, 63U, 0.9},
             {74U, 83U, 0.7}, {94U, 103U, 0.2}})
        std::fill(expected.begin() + first, expected.begin() + last + 1, value);
    if (looping) std::fill(expected.begin() + 114, expected.begin() + 124, 0.9);
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), offline.diagnostics.empty() ? "live Rate offline render"
        : offline.diagnostics.front().message);
    expect(offline.mono, expected);
    expect(offline.right, expected, 0.5);
    for (const auto block : {1U, 7U, 64U})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        for (int pass = 0; pass < 2; ++pass)
        {
            std::array<double, 128> left{}, right{};
            allocations.store(0); countAllocations.store(true);
            bool rendered = session.render(std::span(left).first(0), std::span(right).first(0));
            for (std::size_t offset = 0; offset < left.size(); offset += block)
            {
                const auto count = std::min<std::size_t>(block, left.size() - offset);
                rendered = session.render(std::span(left).subspan(offset, count),
                    std::span(right).subspan(offset, count)) && rendered;
            }
            countAllocations.store(false);
            require(rendered && allocations.load() == 0,
                "live Rate clocks and direction changes render without allocation");
            expect(left, expected); expect(right, expected, 0.5);
            session.resetTransport();
        }
    }
    RealtimeRenderSession source(graph), transferred(graph);
    std::array<double, 37> prefixLeft{}, prefixRight{};
    require(source.render(prefixLeft, prefixRight)
            && transferred.transferPerformanceStateFrom(source),
        "transfer retains the partially elapsed Rate clock and automation cursor");
    transferred.synchronizeTransportFrame(37);
    std::array<double, 91> tailLeft{}, tailRight{};
    for (std::size_t offset = 0; offset < tailLeft.size(); offset += 7)
    {
        const auto count = std::min<std::size_t>(7, tailLeft.size() - offset);
        require(transferred.render(std::span(tailLeft).subspan(offset, count),
            std::span(tailRight).subspan(offset, count)), "transferred Rate clock should continue");
    }
    expect(tailLeft, std::span(expected).subspan(37));
    expect(tailRight, std::span(expected).subspan(37), 0.5);
    auto& faultNodes = reusable ? document.soundDefinitions.front().graph.nodes : document.graph.nodes;
    auto& oscillator = *std::ranges::find(faultNodes, "osc", &talas::project::ProjectNode::instanceId);
    oscillator.parameters[0].expression =
        "(!KeyDown > 0) * (!KeyVelocity + !Morph) / (2 * (1 - (!KeyDown > 0) * (!KeyVelocity > 0.5) * !Fault))";
    s.eventValues.push_back({"Fault", 1});
    RealtimeRenderSession failed(compile(document, s));
    require(failed.ready(), failed.error());
    std::array<double, 64> left{}, right{};
    allocations.store(0); countAllocations.store(true);
    const auto bad = failed.render(left, right);
    countAllocations.store(false);
    require(!bad && allocations.load() == 0
            && std::ranges::all_of(left, [](double value) { return value == 0; }),
        "failed note activation rolls back rate phase, sign and control automation");
    require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar), "repair live Rate voice");
    allocations.store(0); countAllocations.store(true);
    const auto recovered = failed.render(left, right);
    countAllocations.store(false);
    require(recovered && allocations.load() == 0, "live Rate retries without callback allocation");
    expect(left, std::span(expected).first(64));
    expect(right, std::span(expected).first(64), 0.5);
}

void testAnalogSequencerLiveRateAndDirection()
{
    verifyAnalogSequencerLiveRate(false, false, false, true, false);
    verifyAnalogSequencerLiveRate(true, true, false, false, true);
    verifyAnalogSequencerLiveRate(false, true, true, true, true);
    verifyAnalogSequencerLiveRate(true, false, true, false, false);
}

void testAnalogSequencerTimedRate()
{
    auto document = analogStepInstrument();
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
    auto& parameters = document.graph.nodes[1].parameters;
    std::ranges::find(parameters, "step", &talas::project::ProjectParameterExpression::parameterId)->expression = "1";
    std::ranges::find(parameters, "rate", &talas::project::ProjectParameterExpression::parameterId)->expression =
        "1 + (1 ramp: 20 ms)";
    std::ranges::find(parameters, "durations", &talas::project::ProjectParameterExpression::parameterId)->text = "#(0.04)";
    std::ranges::find(parameters, "dutyCycles", &talas::project::ProjectParameterExpression::parameterId)->text = "#(0.5)";
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 1});
    const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
    require(reopened.ok(), "timed Rate must save and reopen");
    const auto graph = compile(reopened.document, s);
    std::array<double, 64> expected{};
    for (const auto [first, last, value] : {
             std::tuple{6U, 15U, 0.2}, {32U, 36U, 0.7}, {52U, 56U, 0.9}})
        std::fill(expected.begin() + first, expected.begin() + last + 1, value);
    const auto offline = OfflineRenderSession(graph).render(64);
    require(offline.ok(), "continuous timed Rate should render offline");
    expect(offline.mono, expected);
    for (const auto block : {1U, 7U, 64U})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 64> left{}, right{};
        for (std::size_t offset = 0; offset < left.size(); offset += block)
        {
            const auto count = std::min<std::size_t>(block, left.size() - offset);
            allocations.store(0); countAllocations.store(true);
            const auto rendered = session.render(std::span(left).subspan(offset, count),
                std::span(right).subspan(offset, count));
            countAllocations.store(false);
            require(rendered && allocations.load() == 0, "timed Rate callback must not allocate");
        }
        expect(left, expected); expect(right, expected, 0.5);
    }
}

void testAnalogSequencerLiveRateRestartsAndRejectsInvalidValues()
{
    for (const auto source : {"previous(1, -2)", "[1, rateIdentifier][0]"})
    {
        auto document = analogGateInstrument(false);
        document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
        std::ranges::find(document.graph.nodes[1].parameters, "rate",
            &talas::project::ProjectParameterExpression::parameterId)->expression = source;
        auto s = settings();
        s.eventValues.push_back({"SeqGate", 1});
        const auto rejected = compileProject(document, talas::core::makeBuiltinNodeRegistry(), s);
        require(!rejected.ok(), "unclocked stateful or unresolved Rate must not silently freeze at its initial value");
    }
    for (const bool connected : {false, true})
    {
        auto document = analogGateInstrument(false);
        auto& parameters = document.graph.nodes[1].parameters;
        std::ranges::find(parameters, "rate", &talas::project::ProjectParameterExpression::parameterId)->expression = "!SeqRate";
        std::ranges::find(parameters, "endIndex", &talas::project::ProjectParameterExpression::parameterId)->expression = "0";
        std::ranges::find(parameters, "durations", &talas::project::ProjectParameterExpression::parameterId)->text = "#(0.02)";
        std::ranges::find(parameters, "dutyCycles", &talas::project::ProjectParameterExpression::parameterId)->text = "#(0.75)";
        if (connected) connectAnalogKeyControl(document, "rate");
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"SeqRate", 1}});
        RealtimeRenderSession session(compile(document, s));
        require(session.ready(), session.error());
        std::array<double, 7> initial{}, initialRight{};
        require(session.render(initial, initialRight), "live Rate initial note renders");
        require(std::abs(initial.back() - 0.2) < 1e-10, "initial Rate note reaches its voice");
        require(session.updateEventValue("SeqGate", 0, talas::expr::ValueDomain::Scalar), "close live Rate Gate");
        std::array<double, 3> closing{}, closingRight{}, closingExpected{0.2, 0, 0};
        require(session.render(closing, closingRight), "Gate releases a dynamic-clock note");
        expect(closing, closingExpected);
        require(!session.updateEventValue("SeqRate", 1, talas::expr::ValueDomain::Hertz),
            "Rate retains its scalar domain");
        require(session.updateEventValue("SeqRate", 0, talas::expr::ValueDomain::Scalar),
            "MIDI callback validation owns deferred Rate publication");
        {
            std::array<double, 1> badLeft{99}, badRight{99};
            allocations.store(0); countAllocations.store(true);
            const auto bad = session.render(badLeft, badRight);
            countAllocations.store(false);
            // A field's live Rate of zero plays at a fixed-point step
            // (2026-10-13), silent while the Gate is closed; one a Sound
            // feeds still fails without advancing.
            require((connected ? !bad : bad) && badLeft[0] == 0 && badRight[0] == 0 && allocations.load() == 0,
                connected ? "a Sound-fed Rate of zero fails without advancing even while Gate is closed"
                          : "a field's Rate of zero saturates and stays silent while Gate is closed");
        }
        require(session.updateEventValue("SeqRate", -2, talas::expr::ValueDomain::Scalar)
                && session.updateEventValue("SeqGate", 1, talas::expr::ValueDomain::Scalar),
            "reopen Gate with a different Rate and sign");
        std::array<double, 16> left{}, right{}, expected{};
        std::fill(expected.begin() + 6, expected.begin() + 9, 0.2);
        require(session.render(left, right), "Gate restart gets a fresh Rate clock");
        expect(left, expected); expect(right, expected, 0.5);
    }
}

void testAnalogSequencerFixedLiveRatePreservesStaticRounding()
{
    for (const auto rate : {0.7, -0.7, 1.3, -1.3, 0.007, -0.007})
    {
        const auto shortDurations = std::abs(rate) < 0.01;
        auto document = analogStepInstrument();
        auto& parameters = document.graph.nodes[1].parameters;
        std::ranges::find(parameters, "step", &talas::project::ProjectParameterExpression::parameterId)->expression = "1";
        std::ranges::find(parameters, "durations", &talas::project::ProjectParameterExpression::parameterId)->text =
            shortDurations ? "#(0.000197 0.000273 0.000411)" : "#(0.0197 0.0273 0.0411)";
        std::ranges::find(parameters, "dutyCycles", &talas::project::ProjectParameterExpression::parameterId)->text = "#(0.61 0.83 0.57)";
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"SeqRate", rate}});
        std::array<double, 320> expected{};
        const std::array<double, 3> durations{19.7, 27.3, 41.1}, duties{0.61, 0.83, 0.57}, velocities{0.2, 0.7, 0.9};
        for (std::size_t frame = 0, ordinal = 0; frame < expected.size(); ++ordinal)
        {
            const auto stage = rate > 0 ? ordinal % 3 : 2 - ordinal % 3;
            const auto duration = static_cast<std::size_t>(std::round(
                durations[stage] / (shortDurations ? 100 : 1) / std::abs(rate)));
            const auto gate = static_cast<std::size_t>(std::round(duration * duties[stage]));
            for (auto audible = frame + 6; audible < std::min(expected.size(), frame + gate + 1); ++audible)
                expected[audible] = velocities[stage];
            frame += duration;
        }
        for (const int mode : {0, 1, 2, 3})
        {
            auto candidate = document;
            std::ranges::find(candidate.graph.nodes[1].parameters, "rate",
                &talas::project::ProjectParameterExpression::parameterId)->expression =
                    mode == 0 ? std::to_string(rate) : mode == 3 ? "@SeqRate" : "!SeqRate";
            if (mode == 2) connectAnalogKeyControl(candidate, "rate");
            if (mode == 3)
                candidate.variableLibrary.variables.push_back({"SeqRate", "Sequence rate",
                    talas::expr::ValueDomain::Scalar, rate,
                    {talas::project::ProjectVariableOwnerKind::Project, {}}, true});
            if (rate < 0) useClassicAnalogSequencerArrays(candidate);
            const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(candidate));
            require(reopened.ok(), "fractional fixed Rate fixture saves and reopens");
            const auto graph = compile(reopened.document, s);
            const auto offline = OfflineRenderSession(graph).render(expected.size());
            require(offline.ok(), "fixed static/EventValue/connected/variable Rate renders offline");
            expect(offline.mono, expected); expect(offline.right, expected, 0.5);
            for (const auto block : {1U, 7U, 64U})
            {
                RealtimeRenderSession session(graph);
                require(session.ready(), session.error());
                std::array<double, 320> left{}, right{};
                for (std::size_t offset = 0; offset < left.size(); offset += block)
                {
                    const auto count = std::min<std::size_t>(block, left.size() - offset);
                    allocations.store(0); countAllocations.store(true);
                    const auto rendered = session.render(std::span(left).subspan(offset, count),
                        std::span(right).subspan(offset, count));
                    countAllocations.store(false);
                    require(rendered && allocations.load() == 0,
                        "fractional fixed Rate preserves frame rounding without callback allocations");
                }
                expect(left, expected); expect(right, expected, 0.5);
            }
        }
    }
}

void testAnalogSequencerLiveRateSamplesArraysAtAcceptedStages()
{
    auto document = analogStepInstrument();
    auto& parameters = document.graph.nodes[1].parameters;
    std::ranges::find(parameters, "step", &talas::project::ProjectParameterExpression::parameterId)->expression = "1";
    std::ranges::find(parameters, "rate", &talas::project::ProjectParameterExpression::parameterId)->expression = "!SeqRate";
    std::ranges::find(parameters, "durations", &talas::project::ProjectParameterExpression::parameterId)->text = "{20 ms * !Length}";
    std::ranges::find(parameters, "dutyCycles", &talas::project::ProjectParameterExpression::parameterId)->text = "#(0.75)";
    std::ranges::find(parameters, "keyVelocities", &talas::project::ProjectParameterExpression::parameterId)->text =
        "{0.2 * !VelocityScale} {0.7 * !VelocityScale} {0.9 * !VelocityScale}";
    connectAnalogKeyControl(document, "rate");
    auto s = settings();
    s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"SeqRate", 1}, {"Length", 1}, {"VelocityScale", 1}});
    s.eventValueChanges = {{8, "SeqRate", 2}, {10, "Length", 2}, {25, "VelocityScale", 0.5}};
    const auto graph = compile(document, s);
    std::array<double, 64> expected{};
    for (const auto [first, last, value] : {
             std::tuple{6U, 12U, 0.2}, {20U, 29U, 0.7}, {40U, 49U, 0.45}, {60U, 63U, 0.1}})
        std::fill(expected.begin() + first, expected.begin() + last + 1, value);
    const auto offline = OfflineRenderSession(graph).render(64);
    require(offline.ok(), "live Rate and array members render together");
    expect(offline.mono, expected);
    for (const auto block : {1U, 7U, 64U})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 64> left{}, right{};
        for (std::size_t offset = 0; offset < left.size(); offset += block)
        {
            const auto count = std::min<std::size_t>(block, left.size() - offset);
            allocations.store(0); countAllocations.store(true);
            const auto rendered = session.render(std::span(left).subspan(offset, count),
                std::span(right).subspan(offset, count));
            countAllocations.store(false);
            require(rendered && allocations.load() == 0, "Rate and array sampling allocate no callback memory");
        }
        expect(left, expected); expect(right, expected, 0.5);
    }
}

void testAnalogSequencerLiveRateBurstLimitRollsBack()
{
    for (const bool silent : {false, true})
    {
        auto document = analogStepInstrument();
        auto& parameters = document.graph.nodes[1].parameters;
        std::ranges::find(parameters, "step", &talas::project::ProjectParameterExpression::parameterId)->expression = "1";
        std::ranges::find(parameters, "rate", &talas::project::ProjectParameterExpression::parameterId)->expression =
            "1 + !Burst * (!SeqRate - 1)";
        std::ranges::find(parameters, "durations", &talas::project::ProjectParameterExpression::parameterId)->text = "#(0.02)";
        std::ranges::find(parameters, "dutyCycles", &talas::project::ProjectParameterExpression::parameterId)->text = "#(0.75)";
        if (silent)
            std::ranges::find(parameters, "keyDowns", &talas::project::ProjectParameterExpression::parameterId)->text = "#(0)";
        auto s = settings();
        s.maxRealtimeEventTransactions = 8;
        s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"SeqRate", 1}, {"Burst", 1}});
        s.eventValueChanges = {{8, "SeqRate", 100000}, {9, "SeqRate", 1}};
        const auto graph = compile(document, s);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 64> left{}, right{}, expected{};
        allocations.store(0); countAllocations.store(true);
        const auto rendered = session.render(left, right);
        countAllocations.store(false);
        require(!rendered && allocations.load() == 0
                && std::ranges::all_of(left, [](double value) { return value == 0; })
                && std::ranges::all_of(right, [](double value) { return value == 0; }),
            "one-sample Rate burst is bounded even when every stage is silent");
        require(session.updateEventValue("Burst", 0, talas::expr::ValueDomain::Scalar),
            "repair the failed burst without rebuilding the session");
        if (!silent)
        {
            std::fill(expected.begin() + 6, expected.begin() + 16, 0.2);
            std::fill(expected.begin() + 26, expected.begin() + 36, 0.7);
            std::fill(expected.begin() + 46, expected.begin() + 56, 0.9);
        }
        allocations.store(0); countAllocations.store(true);
        const auto recovered = session.render(left, right);
        countAllocations.store(false);
        require(recovered && allocations.load() == 0,
            "Rate phase, stage position and pending voices roll back after a bounded burst");
        expect(left, expected); expect(right, expected, 0.5);
    }
}

void testAnalogSequencerLiveRateInsideTransportLoop()
{
    for (const bool connected : {false, true})
    {
        auto document = analogStepInstrument();
        auto& parameters = document.graph.nodes[1].parameters;
        std::ranges::find(parameters, "step", &talas::project::ProjectParameterExpression::parameterId)->expression = "1";
        std::ranges::find(parameters, "rate", &talas::project::ProjectParameterExpression::parameterId)->expression = "!SeqRate";
        std::ranges::find(parameters, "durations", &talas::project::ProjectParameterExpression::parameterId)->text = "#(0.02)";
        std::ranges::find(parameters, "dutyCycles", &talas::project::ProjectParameterExpression::parameterId)->text = "#(0.75)";
        if (connected) connectAnalogKeyControl(document, "rate");
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = "sequence"; sound.displayName = "Sequence";
        sound.graph = std::move(document.graph);
        std::erase_if(sound.graph.nodes, [](const auto& node) { return node.instanceId == "out"; });
        std::erase_if(sound.graph.connections, [](const auto& input) { return input.toNode == "out"; });
        sound.outputs = {{"leftOut", talas::core::PortType::Audio, "voice", "leftOut"},
            {"rightOut", talas::core::PortType::Audio, "voice", "rightOut"}};
        document.graph = {};
        document.soundDefinitions.push_back(std::move(sound));
        document.timeline.score.tracks = {
            {"track", "Track", talas::project::ScoreTrackKind::Track,
                talas::project::ScoreTimeDomain::Seconds, "master", 2, talas::project::ScoreOverlapMode::Layer},
            {"master", "Master", talas::project::ScoreTrackKind::Master,
                talas::project::ScoreTimeDomain::Seconds, {}, 2, talas::project::ScoreOverlapMode::Layer}};
        const auto seconds = [](double value) { return talas::project::ScoreTime{talas::project::ScoreTimeDomain::Seconds, value, 0}; };
        document.timeline.score.clips = {{"clip", "track", "sequence", seconds(0), seconds(0.16),
            seconds(0), seconds(0), seconds(0), 1}};
        const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
        require(reopened.ok(), "live Rate Score source saves and reopens");
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"SeqRate", 1}});
        s.transportLoop = TransportLoopRegion{0, 32};
        const auto graph = compile(reopened.document, s);
        require(graph.plan().midiVoices.empty(), "Score projection owns sequencer voices inside its clip source");
        std::array<double, 128> expected{};
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
        {
            const auto local = frame % 32;
            if (local >= 6 && local <= 15) expected[frame] = 0.2;
            if (local >= 26) expected[frame] = 0.7;
        }
        for (const auto block : {1U, 7U, 64U})
        {
            RealtimeRenderSession session(graph);
            require(session.ready(), session.error());
            std::array<double, 128> left{}, right{};
            for (std::size_t offset = 0; offset < left.size(); offset += block)
            {
                const auto count = std::min<std::size_t>(block, left.size() - offset);
                allocations.store(0); countAllocations.store(true);
                const auto rendered = session.render(std::span(left).subspan(offset, count),
                    std::span(right).subspan(offset, count));
                countAllocations.store(false);
                require(rendered && allocations.load() == 0,
                    "live Rate source restarts at transport wrap without allocation");
            }
            expect(left, expected); expect(right, expected, 0.5);
        }
    }
}

talas::project::ProjectDocument analogExtraInstrument(bool looping)
{
    auto document = analogGateInstrument(looping);
    document.graph.nodes[0].parameters = {{"value", "!Morph + 0.1 * !Pan"}};
    auto& parameters = document.graph.nodes[1].parameters;
    std::ranges::find(parameters, "endIndex",
        &talas::project::ProjectParameterExpression::parameterId)->expression = "2";
    for (const auto [id, value] : {
             std::pair{"durations", "#(0.01)"}, {"dutyCycles", "#(1)"},
             {"keyDowns", "#(1 0 0)"}, {"keyPitches", "#(60)"},
             {"keyVelocities", "#(1)"}, {"keyTimbres", "#(0)"}})
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->text = value;
    parameters.push_back(text("extraValues", "#(!Morph 0 {0.5 / 2} 1)\n#(!Pan 0.1)"));
    return document;
}

void testAnalogSequencerExtraValuesAutomaticAndIsolated()
{
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 1});
    s.eventValues.push_back({"Morph", 99});
    s.eventValues.push_back({"Pan", 99});
    for (const bool looping : {false, true})
        for (const bool reverse : {false, true})
        {
            auto document = analogExtraInstrument(looping);
            std::ranges::find(document.graph.nodes[1].parameters, "rate",
                &talas::project::ProjectParameterExpression::parameterId)->expression = reverse ? "-1" : "1";
            // Two reusable Sounds own identically named controllers. Their
            // distinct values must not change one another or the global values.
            talas::project::ProjectDocument nested;
            for (const auto* name : {"first", "second"})
            {
                talas::project::ProjectSoundDefinition definition;
                definition.definitionId = name;
                definition.displayName = name;
                definition.graph = document.graph;
                definition.graph.nodes.pop_back();
                definition.graph.connections.resize(1);
                if (std::string_view(name) == "second")
                    std::ranges::find(definition.graph.nodes[1].parameters, "extraValues",
                        &talas::project::ProjectParameterExpression::parameterId)->text =
                            "#(!Morph 2 3 4) #(!Pan 0.1)";
                definition.outputs = {{"audioOut", talas::core::PortType::Audio, "voice", "leftOut"}};
                nested.soundDefinitions.push_back(std::move(definition));
                talas::project::ProjectNode instance;
                instance.instanceId = name;
                instance.soundDefinitionId = name;
                nested.graph.nodes.push_back(std::move(instance));
            }
            nested.graph.nodes.push_back({"out", "talas.audioOutput", {}});
            nested.graph.connections = {{"first", "audioOut", "out", "audioIn"},
                {"second", "audioOut", "out", "rightIn"}};
            document.graph.nodes.push_back({"outside", "talas.valToSound", {{"value", "!Morph"}}});
            document.graph.connections.back() = {"outside", "audioOut", "out", "rightIn"};
            for (const bool useNested : {false, true})
            {
                const auto reopened = talas::project::parseProjectDocument(
                    talas::project::serializeProjectDocument(useNested ? nested : document));
                require(reopened.ok(), "named ExtraValues save and reopen in root and reusable Sounds");
                const auto graph = compile(reopened.document, s);
                std::array<double, 64> expectedLeft{}, expectedRight{};
                for (std::size_t frame = 0; frame < expectedLeft.size(); ++frame)
                {
                    if (!useNested) expectedRight[frame] = 99;
                    if (reverse && frame < 20) continue;
                    auto index = (frame == 0 ? 0 : frame - 1) / 10;
                    index = looping ? index % 3 : std::min<std::size_t>(index, 2);
                    if (reverse) index = 2 - index;
                    expectedLeft[frame] = std::array{0.0, 0.25, 1.0}[index] + 0.01;
                    if (useNested) expectedRight[frame] = 2.01 + static_cast<double>(index);
                }
                const auto offline = OfflineRenderSession(graph).render(64);
                require(offline.ok(), offline.diagnostics.empty() ? "ExtraValues offline render"
                    : offline.diagnostics.front().message);
                expect(offline.mono, expectedLeft);
                expect(offline.right, expectedRight);
                for (const auto block : {1U, 7U, 64U})
                {
                    RealtimeRenderSession session(graph);
                    require(session.ready(), session.error());
                    std::array<double, 64> left{}, right{};
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        allocations.store(0);
                        countAllocations.store(true);
                        bool rendered = true;
                        for (std::size_t frame = 0; frame < left.size(); frame += block)
                        {
                            const auto count = std::min<std::size_t>(block, left.size() - frame);
                            rendered = session.render(std::span(left).subspan(frame, count),
                                std::span(right).subspan(frame, count)) && rendered;
                        }
                        countAllocations.store(false);
                        require(rendered && allocations.load() == 0,
                            "ExtraValues stages without notes, loop, reverse and reset must not allocate");
                        expect(left, expectedLeft);
                        expect(right, expectedRight);
                        session.resetTransport();
                    }
                }
            }
        }
}

void testAnalogSequencerExtraValuesGateAndRollback()
{
    auto document = analogExtraInstrument(false);
    document.graph.nodes[0].parameters[0].expression =
        "(!Morph + 0.1 * !Pan) / (1 - (!Morph > 0) * !Fault)";
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 0});
    s.eventValues.push_back({"Fault", 1});
    s.eventValueChanges = {{3, "SeqGate", 1}, {18, "SeqGate", 0},
        {23, "SeqGate", 1}, {49, "SeqGate", 0}, {52, "SeqGate", 1}};
    const auto graph = compile(document, s);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 64> left{}, right{}, expected{};
    for (const auto [first, last, value] : {
             std::tuple{3U, 13U, 0.01}, {14U, 23U, 0.26}, {24U, 33U, 0.01},
             {34U, 43U, 0.26}, {44U, 52U, 1.01}, {53U, 62U, 0.01}, {63U, 63U, 0.26}})
        std::fill(expected.begin() + first, expected.begin() + last + 1, value);
    allocations.store(0);
    countAllocations.store(true);
    const auto failed = session.render(left, right);
    countAllocations.store(false);
    require(!failed && allocations.load() == 0
            && std::ranges::all_of(left, [](double value) { return value == 0; }),
        "a failing controller stage must roll back and silence the callback without allocation");
    require(session.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar),
        "repair the expression observing ExtraValues");
    allocations.store(0);
    countAllocations.store(true);
    const auto rendered = session.render(left, right);
    countAllocations.store(false);
    require(rendered && allocations.load() == 0, "controller cursor and local values replay after rollback");
    expect(left, expected);
    expect(right, expected, 0.5);
    s.eventValues.back().value = 0;
    const auto referenceGraph = compile(document, s);
    const auto reference = OfflineRenderSession(referenceGraph).render(64);
    require(reference.ok(), "Gate-controlled ExtraValues offline render");
    expect(reference.mono, expected);
}

void testAnalogSequencerExtraValuesSampleLiveExpressions()
{
    auto document = analogExtraInstrument(false);
    auto& extra = *std::ranges::find(document.graph.nodes[1].parameters, "extraValues",
        &talas::project::ProjectParameterExpression::parameterId);
    extra.text = "#(!Morph {!Spread} {1 + !Spread} {2 + !Spread}) "
        "#(!Pan {0.5 - !Spread} {0.5 + !Spread})";
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document));
    require(reopened.ok(), "ExtraValues brace expressions with live EventValues save and reopen");
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 1});
    s.eventValues.push_back({"Spread", 0.25});
    s.eventValueChanges = {{5, "Spread", 0.75}, {15, "Spread", -0.5}};
    const auto graph = compile(reopened.document, s);
    std::array<double, 48> expected{};
    std::fill(expected.begin(), expected.begin() + 11, 0.275);
    std::fill(expected.begin() + 11, expected.begin() + 21, 1.875);
    std::fill(expected.begin() + 21, expected.end(), 1.5);
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), "sample live ExtraValues expressions at accepted stage boundaries");
    expect(offline.mono, expected);
    for (const auto block : {1U, 7U, 48U})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 48> left{}, right{};
        allocations.store(0);
        countAllocations.store(true);
        bool rendered = true;
        for (std::size_t frame = 0; frame < left.size(); frame += block)
        {
            const auto count = std::min<std::size_t>(block, left.size() - frame);
            rendered = session.render(std::span(left).subspan(frame, count),
                std::span(right).subspan(frame, count)) && rendered;
        }
        countAllocations.store(false);
        require(rendered && allocations.load() == 0, "live ExtraValues expression sampling must not allocate");
        expect(left, expected);
        expect(right, expected, 0.5);
    }

    extra.text = "#(!Morph 0 {1 / (1 - !Fault)} 2) #(!Pan 0.1)";
    s.eventValueChanges.clear();
    s.eventValues.push_back({"Fault", 0});
    const auto failureGraph = compile(document, s);
    RealtimeRenderSession failed(failureGraph);
    require(failed.ready() && failed.updateEventValue("Fault", 1, talas::expr::ValueDomain::Scalar),
        "arm a failure inside the second ExtraValues expression");
    std::array<double, 48> left{}, right{};
    allocations.store(0);
    countAllocations.store(true);
    const auto rejected = failed.render(left, right);
    countAllocations.store(false);
    require(!rejected && allocations.load() == 0, "ExtraValues evaluation failure rejects the callback without allocation");
    require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar), "repair the sampled expression");
    allocations.store(0);
    countAllocations.store(true);
    const auto recovered = failed.render(left, right);
    countAllocations.store(false);
    require(recovered && allocations.load() == 0, "ExtraValues retries its failed expression and original cursor");
    expected.fill(0);
    std::fill(expected.begin(), expected.begin() + 11, 0.01);
    std::fill(expected.begin() + 11, expected.begin() + 21, 1.01);
    std::fill(expected.begin() + 21, expected.end(), 2.01);
    expect(left, expected);
    expect(right, expected, 0.5);
}

void testAnalogSequencerStepReverseRateAndSilentStages()
{
    auto document = analogStepInstrument(false);
    auto& parameters = document.graph.nodes[1].parameters;
    std::ranges::find(parameters, "rate",
        &talas::project::ProjectParameterExpression::parameterId)->expression = "-2";
    for (const auto [id, value] : {
             std::pair{"durations", "#(0.04 0.02 0.06)"},
             {"dutyCycles", "#(0.5 1 1)"}, {"keyDowns", "#(1 0 1)"}})
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->text = value;
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 1});
    s.eventValues.push_back({"SeqStep", 0});
    s.eventValueChanges = {
        {2, "SeqStep", 1}, {3, "SeqStep", 0},
        {30, "SeqStep", 1}, {31, "SeqStep", 0},
        {32, "SeqStep", 1}, {33, "SeqStep", 0},
        {40, "SeqStep", 1}, {41, "SeqStep", 0},
        {42, "SeqStep", 1}, {43, "SeqStep", 0}, {70, "SeqStep", 1},
    };
    const auto graph = compile(document, s);
    std::array<double, 80> expected{};
    std::fill(expected.begin() + 8, expected.begin() + 33, 0.9);
    std::fill(expected.begin() + 48, expected.begin() + 53, 0.2);
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), "reverse Step sequence should render");
    expect(offline.mono, expected);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 80> left{}, right{};
    allocations.store(0);
    countAllocations.store(true);
    bool rendered = true;
    for (std::size_t offset = 0; offset < left.size(); offset += 9)
    {
        const auto count = std::min<std::size_t>(9, left.size() - offset);
        rendered = session.render(std::span(left).subspan(offset, count),
            std::span(right).subspan(offset, count)) && rendered;
    }
    countAllocations.store(false);
    require(rendered && allocations.load() == 0,
        "reverse Step, silent-stage mask and non-loop exhaustion need no callback allocations");
    expect(left, expected);
    expect(right, expected, 0.5);
}

void testAnalogSequencerZeroStepCompatibility()
{
    for (const bool classic : {false, true})
        for (const bool reusable : {false, true})
        {
            auto document = analogStepInstrument();
            auto& parameters = document.graph.nodes[1].parameters;
            std::ranges::find(parameters, "step",
                &talas::project::ProjectParameterExpression::parameterId)->expression = "0";
            std::ranges::find(parameters, "startIndex",
                &talas::project::ProjectParameterExpression::parameterId)->expression = "1";
            parameters.push_back(text("extraValues", "#(!Morph 0.2 0.7 0.9)"));
            parameters.push_back(text("generatedIndex", "!Stage"));
            // ExtraValues starts at its first member during voice preparation.
            // Selecting stage one must wait, so it cannot publish 0.7 yet.
            document.graph.nodes[0].parameters[0].expression =
                "(!KeyDown > 0) * !KeyVelocity + !Morph - 0.2";
            if (classic) useClassicAnalogSequencerArrays(document);
            if (reusable) wrapAnalogInstrument(document);
            const auto saved = talas::project::serializeProjectDocument(document);
            const auto reopened = talas::project::parseProjectDocument(saved);
            require(reopened.ok(), "new zero-Step Sound saves and reopens");
            auto s = settings();
            s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"Stage", -1}});
            s.eventValueChanges = {{5, "SeqGate", 0}, {11, "SeqGate", 1}};
            const auto graph = compile(reopened.document, s);
            const std::array<double, 48> silence{};
            const auto offline = OfflineRenderSession(graph).render(silence.size());
            require(offline.ok(), "new zero-Step Sound renders while waiting");
            expect(offline.mono, silence); expect(offline.right, silence);
            for (const auto block : {1U, 7U, 64U})
            {
                RealtimeRenderSession session(graph);
                require(session.ready(), session.error());
                for (int pass = 0; pass < 2; ++pass)
                {
                    std::array<double, 48> left{}, right{};
                    std::array<double*, 2> channels{left.data(), right.data()};
                    allocations.store(0); countAllocations.store(true);
                    bool ok = session.render({}, channels.data(), channels.size(), 0);
                    for (std::size_t offset = 0; offset < left.size(); offset += block)
                    {
                        const auto count = std::min<std::size_t>(block, left.size() - offset);
                        ok = session.render(std::span(left).subspan(offset, count),
                            std::span(right).subspan(offset, count)) && ok;
                    }
                    countAllocations.store(false);
                    require(ok && allocations.load() == 0,
                        "zero-Step waiting, Gate restart and reset allocate no callback memory: classic="
                            + std::to_string(classic) + " reusable=" + std::to_string(reusable)
                            + " block=" + std::to_string(block) + " pass=" + std::to_string(pass)
                            + " ok=" + std::to_string(ok) + " allocations=" + std::to_string(allocations.load())
                            + " error=" + std::string(session.error()));
                    expect(left, silence); expect(right, silence);
                    require(session.currentGlobalEventValue("Stage", talas::expr::ValueDomain::Scalar) == -1,
                        "waiting must not publish GeneratedIndex");
                    session.resetTransport();
                }
            }
            RealtimeRenderSession source(graph), transferred(graph);
            std::array<double, 8> prefix{}, prefixRight{};
            require(source.render(prefix, prefixRight), "render waiting prefix before transfer");
            allocations.store(0); countAllocations.store(true);
            const auto moved = transferred.transferPerformanceStateFrom(source);
            countAllocations.store(false);
            require(moved && allocations.load() == 0, "transfer a waiting sequencer without allocation");
            transferred.synchronizeTransportFrame(8);
            std::array<double, 40> tail{}, tailRight{};
            require(transferred.render(tail, tailRight), "transferred zero Step keeps waiting");
            expect(tail, std::span(silence).subspan(8));
            expect(tailRight, std::span(silence).subspan(8));

            auto historical = withoutDutyCycleOne(document);
            const auto version = historical.find("\"version\": 156");
            require(version != std::string::npos, "migration playback fixture uses format 41");
            historical.replace(version, std::string_view{"\"version\": 156"}.size(), "\"version\":  40");
            const auto migrated = talas::project::parseProjectDocument(historical);
            require(migrated.ok(), "format-40 zero-Step Sound migrates");
            const auto legacy = talas::project::parseProjectDocument(
                talas::project::serializeProjectDocument(migrated.document));
            require(legacy.ok(), "legacy zero-Step policy survives save/reopen");
            const auto legacyGraph = compile(legacy.document, s);
            RealtimeRenderSession legacySession(legacyGraph);
            require(legacySession.ready() && !legacySession.performanceStateCompatibleWith(source),
                "automatic and waiting zero-Step clocks cannot exchange performance state");
            auto automatic = reopened.document;
            auto& nodes = reusable ? automatic.soundDefinitions.front().graph.nodes : automatic.graph.nodes;
            auto& sequencer = *std::ranges::find(nodes, "talas.analogSequencer", &talas::project::ProjectNode::typeId);
            std::ranges::find(sequencer.parameters, "step",
                &talas::project::ProjectParameterExpression::parameterId)->expression = "1";
            const auto referenceGraph = compile(automatic, s);
            const auto reference = OfflineRenderSession(referenceGraph).render(silence.size());
            const auto legacyAudio = OfflineRenderSession(legacyGraph).render(silence.size());
            require(reference.ok() && legacyAudio.ok()
                    && std::ranges::any_of(reference.mono, [](double value) { return value > 0.4; }),
                "migrated zero clock and documented one clock advance selected stages");
            expect(legacyAudio.mono, reference.mono); expect(legacyAudio.right, reference.right);
            std::array<double, 48> left{}, right{};
            for (std::size_t offset = 0; offset < left.size(); offset += 7)
            {
                const auto count = std::min<std::size_t>(7, left.size() - offset);
                require(legacySession.render(std::span(left).subspan(offset, count),
                        std::span(right).subspan(offset, count)), "legacy zero-Step callback renders");
            }
            expect(left, reference.mono); expect(right, reference.right);

            auto forged = graph;
            auto& plan = const_cast<RuntimePlan&>(forged.plan());
            auto& owner = *std::ranges::find(plan.steps, "talas.analogSequencer", &CompileStep::typeId);
            std::ranges::find(owner.parameters, "stepZeroMode", &ParameterValue::id)->choice = "legacyAutomatic";
            require(!RealtimeRenderSession(forged).ready() && !validateOfflineRuntimeGraph(forged).ok(),
                "mutating sealed key Step-zero policy rejects both execution paths");
        }
    for (const bool connected : {false, true})
    {
        auto document = analogStepInstrument();
        document.graph.nodes[1].parameters.push_back(choice("stepZeroMode", "legacyAutomatic"));
        if (connected) connectAnalogKeyControl(document, "step");
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"SeqStep", 0}});
        const auto waitingGraph = compile(document, s);
        const auto waiting = OfflineRenderSession(waitingGraph).render(32);
        const std::array<double, 32> silence{};
        require(waiting.ok(), "legacy policy leaves live and connected Step edge-triggered");
        expect(waiting.mono, silence);
        s.eventValueChanges = {{4, "SeqStep", 1}, {5, "SeqStep", 0}};
        const auto triggeredGraph = compile(document, s);
        const auto triggered = OfflineRenderSession(triggeredGraph).render(32);
        std::array<double, 32> expected{};
        std::fill(expected.begin() + 10, expected.begin() + 15, 0.2);
        require(triggered.ok(), "legacy live/connected zero can accept its first positive edge");
        expect(triggered.mono, expected); expect(triggered.right, expected, 0.5);
    }
}

void testAnalogSequencerStepConstantClockAndLiveEdges()
{
    auto document = analogStepInstrument();
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 1});
    s.eventValues.push_back({"SeqStep", 1});
    // A live Step that stays high is one edge, even when its initial value is 1.
    const auto edgeGraph = compile(document, s);
    const auto edgeAudio = OfflineRenderSession(edgeGraph).render(60);
    require(edgeAudio.ok(), "held live Step should render");
    std::array<double, 60> oneNote{};
    std::fill(oneNote.begin() + 6, oneNote.begin() + 11, 0.2);
    expect(edgeAudio.mono, oneNote);
    auto& step = *std::ranges::find(document.graph.nodes[1].parameters, "step",
        &talas::project::ProjectParameterExpression::parameterId);
    step.expression = "0.25";
    const auto fractionalGraph = compile(document, s);
    const auto fractional = OfflineRenderSession(fractionalGraph).render(60);
    require(fractional.ok(), "positive fractional constant is one Step edge");
    expect(fractional.mono, oneNote);
    step.expression = "1";
    const auto clockGraph = compile(document, s);
    const auto clock = OfflineRenderSession(clockGraph).render(60);
    step.expression = "0";
    const auto waitingGraph = compile(document, s);
    const auto waiting = OfflineRenderSession(waitingGraph).render(60);
    require(waiting.ok(), "constant zero waits for a Step edge in new projects");
    const std::array<double, 60> silenceAtZero{};
    expect(waiting.mono, silenceAtZero);
    document.graph.nodes[1].parameters.push_back(choice("stepZeroMode", "legacyAutomatic"));
    const auto legacyGraph = compile(document, s);
    const auto legacy = OfflineRenderSession(legacyGraph).render(60);
    require(clock.ok() && legacy.ok(), "constant-one clock and legacy-zero alias render");
    expect(clock.mono, legacy.mono);
    require(clock.mono[17] > 0.6 && clock.mono[36] > 0.8,
        "constant one must automatically traverse later stages");

    s.eventValues.back().value = 0;
    const auto graph = compile(analogStepInstrument(), s);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 32> left{}, right{}, expected{};
    std::fill(expected.begin() + 10, expected.begin() + 15, 0.2);
    std::fill(expected.begin() + 21, expected.begin() + 26, 0.7);
    std::size_t offset = 0;
    for (const auto [value, count] : {
             std::pair{0.0, 4U}, {1.0, 4U}, {0.0, 1U},
             {2.0, 5U}, {0.0, 1U}, {0.1, 17U}})
    {
        require(session.updateEventValue("SeqStep", value, talas::expr::ValueDomain::Scalar),
            "publish a non-negative Step control");
        allocations.store(0);
        countAllocations.store(true);
        const auto rendered = session.render(std::span(left).subspan(offset, count),
            std::span(right).subspan(offset, count));
        countAllocations.store(false);
        require(rendered && allocations.load() == 0,
            "callback-boundary Step publication should not allocate");
        offset += count;
    }
    expect(left, expected);
    expect(right, expected, 0.5);
    require(session.updateEventValue("SeqStep", -1, talas::expr::ValueDomain::Scalar),
        "queue a negative Step for callback-time validation");
    std::array<double, 8> rejectedLeft{}, rejectedRight{}, silence{};
    allocations.store(0);
    countAllocations.store(true);
    const auto invalidRendered = session.render(rejectedLeft, rejectedRight);
    countAllocations.store(false);
    require(!invalidRendered && allocations.load() == 0,
        "negative Step must reject the callback without allocation");
    expect(rejectedLeft, silence);
    expect(rejectedRight, silence);
    require(session.updateEventValue("SeqStep", 0, talas::expr::ValueDomain::Scalar)
            && session.render(rejectedLeft, rejectedRight),
        "repair an invalid Step without advancing the rejected callback");
    expect(rejectedLeft, silence);
    require(session.updateEventValue("SeqStep", 1, talas::expr::ValueDomain::Scalar)
            && session.render(rejectedLeft, rejectedRight),
        "next accepted Step after repair should select the third stage");
    std::array<double, 8> repairedExpected{};
    repairedExpected[6] = repairedExpected[7] = 0.9;
    expect(rejectedLeft, repairedExpected);
}

void testAnalogSequencerStepCallbackRollback()
{
    auto document = analogStepInstrument();
    document.graph.nodes[0].parameters[0].expression =
        "(!KeyDown > 0) * !KeyVelocity / (1 - (!KeyDown > 0) * (!KeyVelocity > 0.5) * !Fault)";
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 1});
    s.eventValues.push_back({"SeqStep", 0});
    s.eventValues.push_back({"Fault", 1});
    s.eventValueChanges = {
        {3, "SeqStep", 1}, {4, "SeqStep", 0},
        {12, "SeqStep", 1}, {14, "SeqStep", 0},
        {17, "SeqStep", 1}, {18, "SeqStep", 0},
    };
    const auto graph = compile(document, s);
    RealtimeRenderSession session(graph), reference(graph);
    require(session.ready() && reference.ready(), "Step rollback sessions prepare");
    std::array<double, 64> left{}, right{}, expectedLeft{}, expectedRight{};
    allocations.store(0);
    countAllocations.store(true);
    const auto failed = session.render(left, right);
    countAllocations.store(false);
    require(!failed && allocations.load() == 0,
        "Step attack failure should roll back the callback without allocating");
    require(std::ranges::all_of(left, [](double value) { return value == 0; })
            && std::ranges::all_of(right, [](double value) { return value == 0; }),
        "failed Step callback must silence both channels");
    require(session.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
            && reference.render(expectedLeft, expectedRight),
        "repair the Step voice expression");
    allocations.store(0);
    countAllocations.store(true);
    const auto recovered = session.render(left, right);
    countAllocations.store(false);
    require(recovered && allocations.load() == 0,
        "Step edge, next stage, duration mask and note release must replay after rollback");
    expect(left, expectedLeft);
    expect(right, expectedRight);
}

void verifyAnalogSequencerTimedStepAndGate(bool classic, bool extraValues = false,
    bool liveArrays = false, bool connected = false)
{
    auto document = analogStepInstrument();
    if (liveArrays)
    {
        auto& parameters = document.graph.nodes[1].parameters;
        for (const auto [id, value] : {
                 std::pair{"durations", "{10 ms * !ArrayTime} {20 ms * !ArrayTime} {10 ms * !ArrayTime}"},
                 {"dutyCycles", "{!ArrayDuty} {0.5 * !ArrayDuty} {!ArrayDuty}"},
                 {"keyDowns", "{!ArrayDown}"},
                 {"keyPitches", "{60 nn + !ArrayPitch nn} {64 nn + !ArrayPitch nn} {67 nn + !ArrayPitch nn}"},
                 {"keyVelocities", "{0.2 * !ArrayVelocity} {0.7 * !ArrayVelocity} {0.9 * !ArrayVelocity}"},
                 {"keyTimbres", "{!ArrayTimbre}"}})
            std::ranges::find(parameters, id,
                &talas::project::ProjectParameterExpression::parameterId)->text = value;
    }
    if (classic) useClassicAnalogSequencerArrays(document);
    if (extraValues)
    {
        document.graph.nodes[1].parameters.push_back(text("extraValues", "#(!Morph 0.2 0.7 0.9)"));
        document.graph.nodes[0].parameters[0].expression = "(!KeyDown > 0) * !Morph";
    }
    document.expressionLanguage = liveArrays
        ? talas::expr::ExpressionLanguageVersion::VariablesV4
        : talas::expr::ExpressionLanguageVersion::VariablesV3;
    // Thresholds lie between adjacent millisecond clock ticks, avoiding
    // binary rounding of the ramp exactly at a comparison boundary.
    auto& parameters = document.graph.nodes[1].parameters;
    std::ranges::find(parameters, "step",
        &talas::project::ProjectParameterExpression::parameterId)->expression =
        "((!StepArm ramp: 100 ms) >= 0.025) * ((!StepArm ramp: 100 ms) < 0.035) + "
        "((!StepArm ramp: 100 ms) >= 0.055) * ((!StepArm ramp: 100 ms) < 0.065) + "
        "((!StepArm ramp: 100 ms) >= 0.165) * ((!StepArm ramp: 100 ms) < 0.175) + "
        "((!StepArm ramp: 100 ms) >= 0.365) * ((!StepArm ramp: 100 ms) < 0.375) + "
        "((!StepArm ramp: 100 ms) >= 0.465) * ((!StepArm ramp: 100 ms) < 0.475)";
    std::ranges::find(parameters, "gate",
        &talas::project::ProjectParameterExpression::parameterId)->expression =
        "(1 ramp: 100 ms) < 0.465";
    if (connected)
    {
        std::ranges::find(parameters, "gate",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "!ConnectedGate";
        connectAnalogKeyControl(document, "gate");
    }
    const auto reopened = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document));
    require(reopened.ok(), "timed Step expressions should save and reopen");
    auto timedSettings = settings();
    if (connected)
    {
        timedSettings.eventValues.push_back({"ConnectedGate", 1});
        timedSettings.eventValueChanges = {{47, "ConnectedGate", 0}};
    }
    timedSettings.eventValues.push_back({"StepArm", 1});
    if (liveArrays)
        timedSettings.eventValues.insert(timedSettings.eventValues.begin(),
            {{"ArrayTime", 1}, {"ArrayDuty", 1}, {"ArrayDown", 1}, {"ArrayPitch", 0},
             {"ArrayVelocity", 1}, {"ArrayTimbre", 0}});
    const auto graph = compile(reopened.document, timedSettings);
    std::array<double, 64> expected{};
    for (const auto [first, last, value] : {
             std::tuple{9U, 13U, 0.2}, {23U, 27U, 0.7}, {43U, 47U, 0.9}})
        std::fill(expected.begin() + first, expected.begin() + last + 1, value);
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), offline.diagnostics.empty()
        ? "timed Step offline render" : offline.diagnostics.front().message);
    expect(offline.mono, expected);
    for (const auto blockSize : {1U, 7U, 64U})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 64> left{}, right{};
        for (int pass = 0; pass < 2; ++pass)
        {
            allocations.store(0);
            countAllocations.store(true);
            bool rendered = true;
            for (std::size_t offset = 0; offset < left.size(); offset += blockSize)
            {
                const auto count = std::min<std::size_t>(blockSize, left.size() - offset);
                rendered = session.render(std::span(left).subspan(offset, count),
                    std::span(right).subspan(offset, count)) && rendered;
            }
            countAllocations.store(false);
            require(rendered && allocations.load() == 0,
                "timed Step and Gate must schedule new deadlines within a callback without allocation");
            expect(left, expected);
            expect(right, expected, 0.5);
            session.resetTransport();
        }
    }
    RealtimeRenderSession source(graph), transferred(graph);
    std::array<double, 11> prefixLeft{}, prefixRight{};
    require(source.render(prefixLeft, prefixRight)
            && transferred.transferPerformanceStateFrom(source),
        "timed Step transfer keeps expression clock and edge state together");
    transferred.synchronizeTransportFrame(11);
    std::array<double, 53> left{}, right{};
    require(transferred.render(left, right), "transferred timed Step should continue");
    expect(left, std::span(expected).subspan(11));
    expect(right, std::span(expected).subspan(11), 0.5);

    auto armedSettings = timedSettings;
    armedSettings.eventValues.back().value = 0;
    armedSettings.eventValueChanges.insert(armedSettings.eventValueChanges.begin(), {10, "StepArm", 1});
    const auto armedGraph = compile(reopened.document, armedSettings);
    std::array<double, 64> armedExpected{}, armedLeft{}, armedRight{};
    std::fill(armedExpected.begin() + 19, armedExpected.begin() + 24, 0.2);
    std::fill(armedExpected.begin() + 33, armedExpected.begin() + 38, 0.7);
    const auto armedOffline = OfflineRenderSession(armedGraph).render(64);
    require(armedOffline.ok(), "an EventValue can start the timed Step expression");
    expect(armedOffline.mono, armedExpected);
    RealtimeRenderSession armed(armedGraph);
    require(armed.ready(), armed.error());
    allocations.store(0);
    countAllocations.store(true);
    const auto armedRendered = armed.render(armedLeft, armedRight);
    countAllocations.store(false);
    require(armedRendered && allocations.load() == 0,
        "starting a timed Step inside a callback must not allocate");
    expect(armedLeft, armedExpected);
    expect(armedRight, armedExpected, 0.5);

    document.graph.nodes[0].parameters[0].expression =
        extraValues
        ? "(!KeyDown > 0) * !Morph / (1 - (!KeyDown > 0) * (!Morph > 0.5) * !Fault)"
        : "(!KeyDown > 0) * !KeyVelocity / (1 - (!KeyDown > 0) * (!KeyVelocity > 0.5) * !Fault)";
    auto configured = timedSettings;
    configured.eventValues.push_back({"Fault", 1});
    const auto failureGraph = compile(document, configured);
    RealtimeRenderSession failed(failureGraph);
    require(failed.ready(), failed.error());
    std::array<double, 64> recoveredLeft{}, recoveredRight{};
    allocations.store(0);
    countAllocations.store(true);
    const auto bad = failed.render(recoveredLeft, recoveredRight);
    countAllocations.store(false);
    require(!bad && allocations.load() == 0,
        "timed Step attack failure must roll back without allocation");
    require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar),
        "repair timed Step attack expression");
    allocations.store(0);
    countAllocations.store(true);
    const auto recovered = failed.render(recoveredLeft, recoveredRight);
    countAllocations.store(false);
    require(recovered && allocations.load() == 0,
        "timed Step must replay clock state and note deadlines after failure");
    expect(recoveredLeft, expected);
    expect(recoveredRight, expected, 0.5);
}

void testAnalogSequencerTimedStepAndGate()
{
    verifyAnalogSequencerTimedStepAndGate(false);
}

void testAnalogSequencerConnectedTimedControlsAndLiveArrays()
{
    verifyAnalogSequencerTimedStepAndGate(false, true, true, true);
    verifyAnalogSequencerTimedStepAndGate(true, true, true, true);
}

void testAnalogSequencerClassicTimedStepAndGate()
{
    verifyAnalogSequencerTimedStepAndGate(true);
}

void testAnalogSequencerExtraValuesTimedControls()
{
    verifyAnalogSequencerTimedStepAndGate(true, true);
}

void testAnalogSequencerLiveArraysTimedControls()
{
    verifyAnalogSequencerTimedStepAndGate(false, true, true);
    verifyAnalogSequencerTimedStepAndGate(true, true, true);
}

talas::project::ProjectDocument analogLiveArrayInstrument(bool classic, bool bare = false)
{
    auto document = analogStepInstrument(false);
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV4;
    document.graph.nodes[0].parameters = {{"value",
        "(!KeyDown > 0) * (!KeyVelocity + !KeyTimbre + !StageMark + 0.01 * (!KeyPitch > 61 nn))"}};
    auto& parameters = document.graph.nodes[1].parameters;
    std::ranges::find(parameters, "step",
        &talas::project::ProjectParameterExpression::parameterId)->expression = "1";
    for (const auto [id, value] : {
             std::pair{"durations", bare ? "!Length ms" : "{!Length ms}"},
             {"dutyCycles", bare ? "!Duty" : "{!Duty}"},
             {"keyDowns", bare ? "!Down" : "{!Down}"},
             {"keyPitches", "{60 nn + !Interval nn} {61 nn + !Interval nn} {62 nn + !Interval nn}"},
             {"keyVelocities", "{0.5 * !VelocityScale}"},
             {"keyTimbres", bare ? "!Timbre" : "{!Timbre}"}})
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->text = value;
    parameters.push_back(text("extraValues", "#(!StageMark 0 0.1 0.2)"));
    if (classic) useClassicAnalogSequencerArrays(document);
    return document;
}

RenderSettings analogLiveArraySettings()
{
    auto s = settings();
    s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"Length", 20},
        {"Duty", 0.5}, {"Down", 1}, {"Interval", 0}, {"VelocityScale", 0.4}, {"Timbre", 0.1}});
    return s;
}

void testAnalogSequencerLiveArraysAutomaticSampling()
{
    for (const bool classic : {false, true})
        for (const bool reverse : {false, true})
            for (const bool bare : {false, true})
        {
            auto document = analogLiveArrayInstrument(classic, bare);
            std::ranges::find(document.graph.nodes[1].parameters, "rate",
                &talas::project::ProjectParameterExpression::parameterId)->expression = reverse ? "-1" : "1";
            if (reverse)
            {
                std::ranges::find(document.graph.nodes[1].parameters, "loop",
                    &talas::project::ProjectParameterExpression::parameterId)->boolean = true;
                talas::project::ProjectSoundDefinition definition;
                definition.definitionId = "liveSequence";
                definition.displayName = "Live sequence";
                definition.graph = document.graph;
                definition.graph.nodes.pop_back();
                definition.graph.connections.resize(1);
                definition.outputs = {{"leftOut", talas::core::PortType::Audio, "voice", "leftOut"},
                    {"rightOut", talas::core::PortType::Audio, "voice", "rightOut"}};
                document.soundDefinitions.push_back(std::move(definition));
                talas::project::ProjectNode instance;
                instance.instanceId = "sequence";
                instance.soundDefinitionId = "liveSequence";
                document.graph.nodes = {instance, {"out", "talas.audioOutput", {}}};
                document.graph.connections = {{"sequence", "leftOut", "out", "audioIn"},
                    {"sequence", "rightOut", "out", "rightIn"}};
            }
            const auto reopened = talas::project::parseProjectDocument(
                talas::project::serializeProjectDocument(document));
            require(reopened.ok(), "live classic and modern note arrays save and reopen");
            auto s = analogLiveArraySettings();
            s.eventValueChanges = {
                {5, "Length", 0}, {5, "Duty", 2}, {5, "Down", 0.5},
                {5, "Interval", 200}, {5, "VelocityScale", 3}, {5, "Timbre", 2},
                {19, "Length", 40}, {19, "Duty", 0.75}, {19, "Down", 1},
                {19, "Interval", 4.5}, {19, "VelocityScale", 1.4}, {19, "Timbre", 0.2},
                {25, "Length", 30}, {25, "Duty", 0.5}, {25, "Interval", 7.25},
                {25, "VelocityScale", 0}, {25, "Timbre", 0.4}};
            const auto graph = compile(reopened.document, s);
            std::array<double, 128> expected{};
            const auto first = 0.2 + (classic ? 0 : 0.1) + (reverse ? 0.21 : 0);
            const auto second = 0.81 + (classic ? 0 : 0.2);
            const auto third = 0.01 + (classic ? 0 : 0.4) + (reverse ? 0 : 0.2);
            std::fill(expected.begin() + 6, expected.begin() + 11, first);
            std::fill(expected.begin() + 26, expected.begin() + 51, second);
            std::fill(expected.begin() + 66, expected.begin() + 76, third);
            if (reverse)
            {
                // The next cycle uses the durations and values last sampled
                // from live controls, independently of preparation's timeline.
                std::fill(expected.begin() + 96, expected.begin() + 106,
                    0.21 + (classic ? 0 : 0.4));
                std::fill(expected.begin() + 126, expected.end(),
                    0.11 + (classic ? 0 : 0.4));
            }
            const auto offline = OfflineRenderSession(graph).render(expected.size());
            require(offline.ok(), offline.diagnostics.empty() ? "live note-array offline render"
                : offline.diagnostics.front().message);
            expect(offline.mono, expected);
            expect(offline.right, expected, 0.5);
            for (const auto block : {1U, 7U, 64U})
            {
                RealtimeRenderSession session(graph);
                require(session.ready(), session.error());
                std::array<double, 128> left{}, right{};
                for (int pass = 0; pass < 2; ++pass)
                {
                    allocations.store(0);
                    countAllocations.store(true);
                    bool rendered = true;
                    for (std::size_t frame = 0; frame < left.size(); frame += block)
                    {
                        const auto count = std::min<std::size_t>(block, left.size() - frame);
                        std::array<double*, 2> channels{left.data() + frame, right.data() + frame};
                        rendered = session.render({}, channels.data(), channels.size(), 0) && rendered;
                        rendered = session.render(std::span(left).subspan(frame, count),
                            std::span(right).subspan(frame, count)) && rendered;
                    }
                    countAllocations.store(false);
                    require(rendered && allocations.load() == 0,
                        "live arrays, captured note release, zero frames and reset must remain allocation-free");
                    expect(left, expected);
                    expect(right, expected, 0.5);
                    session.resetTransport();
                }
            }
            RealtimeRenderSession source(graph), transferred(graph);
            std::array<double, 32> prefixLeft{}, prefixRight{};
            require(source.render(prefixLeft, prefixRight)
                    && transferred.transferPerformanceStateFrom(source),
                "transfer the sampled duration, held pitch and future stage deadline");
            transferred.synchronizeTransportFrame(32);
            std::array<double, 96> left{}, right{};
            for (std::size_t frame = 0; frame < left.size(); frame += 32)
                require(transferred.render(std::span(left).subspan(frame, 32),
                        std::span(right).subspan(frame, 32)),
                    "live array performance transfer should continue");
            expect(left, std::span(expected).subspan(32));
            expect(right, std::span(expected).subspan(32), 0.5);
        }
}

void testAnalogSequencerLiveArraysDeferredValidationAndRollback()
{
    const auto document = analogLiveArrayInstrument(false);
    auto s = analogLiveArraySettings();
    for (auto& binding : s.eventValues)
    {
        if (binding.name == "SeqGate") binding.value = 0;
        if (binding.name == "Length") binding.value = 0;
        if (binding.name == "Down") binding.value = 0.5;
        if (binding.name == "Interval") binding.value = 200;
    }
    const auto graph = compile(document, s);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 8> silentLeft{}, silentRight{}, silence{};
    require(session.render(silentLeft, silentRight), "closed Gate does not execute invalid live members");
    expect(silentLeft, silence);
    require(session.updateEventValue("SeqGate", 1, talas::expr::ValueDomain::Scalar), "open Gate on invalid arrays");
    std::array<double, 24> left{}, right{}, expected{};
    for (const auto [name, value] : {std::pair{"Length", 20.0}, {"Down", 1.0}, {"Interval", 0.0}})
    {
        allocations.store(0);
        countAllocations.store(true);
        const auto rendered = session.render(left, right);
        countAllocations.store(false);
        require(!rendered && allocations.load() == 0
                && std::ranges::all_of(left, [](double sample) { return sample == 0; }),
            "each invalid accepted duration, key gate or pitch must reject and restore the whole callback");
        require(session.updateEventValue(name, value, talas::expr::ValueDomain::Scalar), "repair one live note member");
    }
    allocations.store(0);
    countAllocations.store(true);
    const auto rendered = session.render(left, right);
    countAllocations.store(false);
    require(rendered && allocations.load() == 0, "repaired live arrays replay the same Gate edge and stage");
    std::fill(expected.begin() + 6, expected.begin() + 11, 0.3);
    expect(left, expected);
    expect(right, expected, 0.5);
}

void testAnalogSequencerLiveArraysStepDurationMask()
{
    auto document = analogLiveArrayInstrument(false);
    std::ranges::find(document.graph.nodes[1].parameters, "step",
        &talas::project::ProjectParameterExpression::parameterId)->expression = "!SeqStep";
    auto s = analogLiveArraySettings();
    s.eventValues.push_back({"SeqStep", 0});
    s.eventValueChanges = {{3, "SeqStep", 1}, {4, "SeqStep", 0},
        {8, "Length", 5}, {8, "VelocityScale", 1}, {10, "SeqStep", 1}, {11, "SeqStep", 0},
        {22, "Length", 40}, {23, "SeqStep", 1}, {24, "SeqStep", 0},
        {30, "SeqStep", 1}, {31, "SeqStep", 0}, {65, "SeqStep", 1}, {66, "SeqStep", 0}};
    const auto graph = compile(document, s);
    std::array<double, 96> expected{};
    std::fill(expected.begin() + 9, expected.begin() + 14, 0.3);
    std::fill(expected.begin() + 29, expected.begin() + 44, 0.7);
    std::fill(expected.begin() + 71, expected.begin() + 86, 0.81);
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), "live Duration is captured on each accepted Step");
    expect(offline.mono, expected);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 96> left{}, right{};
    allocations.store(0);
    countAllocations.store(true);
    bool rendered = true;
    for (std::size_t frame = 0; frame < left.size(); frame += 7)
    {
        const auto count = std::min<std::size_t>(7, left.size() - frame);
        rendered = session.render(std::span(left).subspan(frame, count),
            std::span(right).subspan(frame, count)) && rendered;
    }
    countAllocations.store(false);
    require(rendered && allocations.load() == 0,
        "changing a live Duration does not shorten the already accepted Step mask");
    expect(left, expected);
    expect(right, expected, 0.5);
}

talas::project::ProjectDocument analogGeneratedIndexInstrument(bool classic = false, bool amplitude = false)
{
    auto document = analogStepInstrument();
    auto& parameters = document.graph.nodes[1].parameters;
    for (const auto [id, expression] : {
             std::pair{"step", "1"}, {"startIndex", "1"}})
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->expression = expression;
    for (const auto [id, value] : {
             std::pair{"durations", "#(0.0078125)"},
             {"dutyCycles", "#(1)"}, {"keyDowns", "#(1 0 1)"},
             {"keyVelocities", "#(0.2 0.7 0)"}})
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->text = value;
    if (classic)
    {
        useClassicAnalogSequencerArrays(document);
        std::ranges::find(parameters, "velocities",
            &talas::project::ProjectParameterExpression::parameterId)->text = "#(0)";
    }
    if (amplitude)
    {
        for (const auto id : {"keyDowns", "keyPitches", "keyVelocities", "keyTimbres"})
            std::ranges::find(parameters, id,
                &talas::project::ProjectParameterExpression::parameterId)->text = "";
        parameters.push_back(text("velocities", "{1} {1} {1}"));
        std::ranges::find(parameters, "durations",
            &talas::project::ProjectParameterExpression::parameterId)->text = "{0.0078125 s}";
        std::ranges::find(parameters, "dutyCycles",
            &talas::project::ProjectParameterExpression::parameterId)->text = "{1}";
        document.graph.nodes[0].parameters[0].expression = "!Stage";
    }
    parameters.push_back(text("generatedIndex", "!Stage"));
    document.graph.nodes.push_back({"indexReader", "talas.valToSound", {{"value", "!Stage"}}});
    document.graph.connections.back() = {"indexReader", "audioOut", "out", "rightIn"};
    return document;
}

void testAnalogSequencerGeneratedIndexAutomaticAndAmplitude()
{
    for (const auto mode : {0, 1, 2})
        for (const bool reverse : {false, true})
        for (const bool looping : {false, true})
        {
            auto document = analogGeneratedIndexInstrument(mode == 1, mode == 2);
            std::ranges::find(document.graph.nodes[1].parameters, "loop",
                &talas::project::ProjectParameterExpression::parameterId)->boolean = looping;
            std::ranges::find(document.graph.nodes[1].parameters, "rate",
                &talas::project::ProjectParameterExpression::parameterId)->expression = reverse ? "-1" : "1";
            const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
            require(reopened.ok(), "GeneratedIndex saves and reopens in every array family");
            auto s = settings();
            s.sampleRate = 1024;
            s.eventValues.push_back({"SeqGate", 1});
            s.eventValueChanges = {{11, "SeqGate", 0}, {16, "SeqGate", 1}};
            const auto graph = compile(reopened.document, s);
            const auto indexAt = [&](std::size_t frame) {
                const auto local = frame >= 16 ? frame - 16 : std::min<std::size_t>(frame, 10);
                const auto stage = looping ? (local / 8) % 2 : std::min<std::size_t>(local / 8, 1);
                return static_cast<double>(reverse ? 2 - stage : 1 + stage);
            };
            std::array<double, 40> expectedRight{};
            // At 1024 Hz the Val clock is slightly longer than one sample;
            // use 1000 Hz below for numeric reader checks. Here the target itself
            // is the exact stage oracle, including silent and zero-velocity steps.
            const auto offline = OfflineRenderSession(graph).render(expectedRight.size());
            require(offline.ok(), offline.diagnostics.empty() ? "GeneratedIndex offline render"
                : offline.diagnostics.front().message);
            for (const auto block : {1U, 7U, 40U})
            {
                RealtimeRenderSession session(graph);
                require(session.ready(), session.error());
                for (int pass = 0; pass < 2; ++pass)
                {
                    require(session.currentGlobalEventValue("Stage", talas::expr::ValueDomain::Scalar) == 0,
                        "new target and reset retain the prepared initial value");
                    std::array<double, 40> left{}, right{};
                    std::array<double*, 2> channels{left.data(), right.data()};
                    allocations.store(0); countAllocations.store(true);
                    const auto empty = session.render({}, channels.data(), channels.size(), 0);
                    countAllocations.store(false);
                    require(empty && allocations.load() == 0
                            && session.currentGlobalEventValue("Stage", talas::expr::ValueDomain::Scalar) == 0,
                        "zero frames do not publish GeneratedIndex: mode=" + std::to_string(mode)
                            + " ok=" + std::to_string(empty) + " allocations=" + std::to_string(allocations.load())
                            + " index=" + std::to_string(session.currentGlobalEventValue(
                                "Stage", talas::expr::ValueDomain::Scalar).value_or(-99)));
                    for (std::size_t offset = 0; offset < left.size(); offset += block)
                    {
                        const auto count = std::min<std::size_t>(block, left.size() - offset);
                        allocations.store(0); countAllocations.store(true);
                        const auto rendered = session.render(std::span(left).subspan(offset, count),
                            std::span(right).subspan(offset, count));
                        countAllocations.store(false);
                        require(rendered && allocations.load() == 0,
                            "GeneratedIndex renders without allocation");
                        require(session.currentGlobalEventValue("Stage", talas::expr::ValueDomain::Scalar)
                                == indexAt(offset + count - 1),
                            "GeneratedIndex publishes the selected source index and retains it behind closed Gate");
                    }
                    expect(left, offline.mono); expect(right, offline.right);
                    session.resetTransport();
                }
            }
        }
}

void testAnalogSequencerGeneratedIndexReaderOrdering()
{
    for (const bool amplitude : {false, true})
    {
        auto document = analogGeneratedIndexInstrument(false, amplitude);
        auto& parameters = document.graph.nodes[1].parameters;
        std::ranges::find(parameters, "durations",
            &talas::project::ProjectParameterExpression::parameterId)->text = amplitude ? "{0.01 s}" : "#(0.01)";
        auto s = settings();
        s.eventValues.push_back({"SeqGate", 1});
        const auto graph = compile(document, s);
        std::array<double, 32> expectedRight{}, expectedLeft{};
        const auto indexAt = [](std::size_t frame) { return double(1 + (frame / 10) % 2); };
        for (std::size_t frame = 0; frame < expectedRight.size(); ++frame)
        {
            expectedRight[frame] = amplitude
                ? (frame < 2 ? 0 : indexAt(frame - 2))
                : indexAt(frame == 0 ? 0 : frame - 1);
            if (amplitude && frame >= 2) expectedLeft[frame] = indexAt(frame - 2);
        }
        const auto offline = OfflineRenderSession(graph).render(expectedRight.size());
        require(offline.ok(), offline.diagnostics.empty() ? "GeneratedIndex external reader renders offline"
            : offline.diagnostics.front().message);
        expect(offline.right, expectedRight);
        expect(offline.mono, expectedLeft);
        RealtimeRenderSession source(graph), transferred(graph);
        require(source.ready() && transferred.ready(), "GeneratedIndex transfer sessions prepare");
        std::array<double, 7> prefixL{}, prefixR{};
        require(source.render(prefixL, prefixR), "GeneratedIndex transfer prefix");
        require(transferred.transferPerformanceStateFrom(source)
                && transferred.currentGlobalEventValue("Stage", talas::expr::ValueDomain::Scalar) == 1,
            "performance transfer includes the published GeneratedIndex");
        if (!amplitude)
        {
            std::array<double, 12> suffixL{}, suffixR{};
            require(transferred.render(suffixL, suffixR), "GeneratedIndex transferred stage clock");
            expect(suffixR, std::span(expectedRight).subspan(7, 12));
        }
    }
}

void testAnalogSequencerGeneratedIndexLiveControlsAndRollback()
{
    for (const bool connected : {false, true})
    {
        auto document = analogGeneratedIndexInstrument();
        auto& parameters = document.graph.nodes[1].parameters;
        std::ranges::find(parameters, "step",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "!SeqStep";
        std::ranges::find(parameters, "rate",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "!SeqRate";
        std::ranges::find(parameters, "durations",
            &talas::project::ProjectParameterExpression::parameterId)->text = "#(0.01)";
        parameters.push_back(text("extraValues", "#(!Seen {!Stage})"));
        document.graph.nodes[0].parameters[0].expression = "(!KeyDown > 0) * !Seen";
        // A downstream error is evaluated during publication, after the owner
        // has selected the new index; rollback must also undo its stage clock.
        document.graph.nodes.back().parameters[0].expression = "!Stage / (1 - (!Stage > 1) * !Fault)";
        if (connected)
        {
            connectAnalogKeyControl(document, "step");
            connectAnalogKeyControl(document, "rate");
        }
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"SeqStep", 0},
            {"SeqRate", 1}, {"Fault", 1}, {"Stage", -1}});
        s.eventValueChanges = {{2, "SeqStep", 1}, {3, "SeqStep", 0},
            {6, "SeqStep", 1}, {7, "SeqStep", 0}, {12, "SeqStep", 1},
            {13, "SeqStep", 0}, {20, "SeqRate", -1}, {22, "SeqStep", 1},
            {23, "SeqStep", 0}};
        const auto graph = compile(document, s);
        RealtimeRenderSession session(graph), reference(graph);
        require(session.ready() && reference.ready(), "GeneratedIndex live controls prepare");
        std::array<double, 32> left{}, right{}, refL{}, refR{};
        allocations.store(0); countAllocations.store(true);
        const auto rendered = session.render(left, right);
        countAllocations.store(false);
        require(!rendered && allocations.load() == 0
                && session.currentGlobalEventValue("Stage", talas::expr::ValueDomain::Scalar) == -1
                && std::ranges::all_of(right, [](double value) { return value == 0; }),
            "failed GeneratedIndex publication restores target, stage, note and whole callback");
        require(session.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
                && reference.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar),
            "repair the downstream GeneratedIndex reader");
        for (std::size_t frame = 0; frame < refL.size(); ++frame)
        {
            require(reference.render(std::span(refL).subspan(frame, 1), std::span(refR).subspan(frame, 1)),
                "GeneratedIndex per-sample reference renders");
            require(reference.currentGlobalEventValue("Stage", talas::expr::ValueDomain::Scalar)
                    == (frame < 2 ? -1 : frame < 12 ? 1 : frame < 22 ? 2 : 1),
                "accepted live Step publishes its index immediately; masked edges retain it");
        }
        allocations.store(0); countAllocations.store(true);
        const auto recovered = session.render(left, right);
        countAllocations.store(false);
        require(recovered && allocations.load() == 0, "GeneratedIndex retry is allocation-free");
        expect(left, refL); expect(right, refR);
        std::array<double, 32> expected{};
        // This independent reader precedes the connected producer phase in
        // the prepared order. It sees that publication on the following sample,
        // in addition to its own one-sample Val interpolation delay.
        const auto delay = connected ? 1 : 0;
        std::fill(expected.begin(), expected.begin() + 3 + delay, -1);
        std::fill(expected.begin() + 3 + delay, expected.begin() + 13 + delay, 1);
        std::fill(expected.begin() + 13 + delay, expected.begin() + 23 + delay, 2);
        std::fill(expected.begin() + 23 + delay, expected.end(), 1);
        expect(right, expected);
        expected.fill(0);
        std::fill(expected.begin() + 18, expected.begin() + 23, 2);
        expect(left, expected);
    }
}

void testAnalogSequencerGeneratedIndexAdmissionAndScope()
{
    auto document = analogGeneratedIndexInstrument();
    auto s = settings();
    s.eventValues.push_back({"SeqGate", 1});
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    for (const auto name : {"Stage", "!Stage + 1", "!KeyPitch"})
    {
        std::ranges::find(document.graph.nodes[1].parameters, "generatedIndex",
            &talas::project::ProjectParameterExpression::parameterId)->text = name;
        require(!compileProject(document, registry, s).ok(), "invalid GeneratedIndex target rejects compilation");
    }
    document = analogGeneratedIndexInstrument();
    s.eventValues.push_back({"Stage", 0, talas::expr::ValueDomain::Hertz});
    require(!compileProject(document, registry, s).ok(), "GeneratedIndex rejects a mistyped target");
    s.eventValues.back().domain = talas::expr::ValueDomain::Scalar;
    s.eventValues.back().projectionExpression = *talas::expr::parse("!SeqGate").expression;
    require(!compileProject(document, registry, s).ok(), "GeneratedIndex rejects a projected target");
    s.eventValues.back().projectionExpression.reset();
    document.graph.nodes[1].parameters.push_back(text("extraValues", "#(!Stage 0 1)"));
    require(!compileProject(document, registry, s).ok(), "GeneratedIndex rejects an ExtraValues name collision");
    document = analogGeneratedIndexInstrument();
    wrapAnalogInstrument(document);
    document.soundDefinitions.front().outputs[1].nodeId = "indexReader";
    document.soundDefinitions.front().outputs[1].nodePortId = "audioOut";
    s.eventValues.back().value = 7;
    const auto initial = compile(document, s);
    const auto owner = std::ranges::find(initial.plan().steps, "talas.analogSequencer", &CompileStep::typeId);
    require(owner != initial.plan().steps.end() && owner->scopeId != 0, "nested GeneratedIndex has an owner scope");
    s.eventValues.push_back({"Stage", -1, talas::expr::ValueDomain::Scalar, owner->scopeId});
    const auto graph = compile(document, s);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 4> left{}, right{};
    require(session.render(left, right)
            && session.currentGlobalEventValue("Stage", talas::expr::ValueDomain::Scalar) == 7
            && std::ranges::all_of(right, [](double value) { return value == 1; }),
        "GeneratedIndex writes the nearest explicit owner binding and leaves the parent value intact");
}

void testAnalogSequencerGeneratedIndexAmplitudeRollbackAndAuthority()
{
    auto document = analogGeneratedIndexInstrument(false, true);
    document.graph.nodes.back().parameters[0].expression = "!Stage / (1 - (!Stage > 1) * !Fault)";
    auto s = settings();
    s.sampleRate = 1024;
    s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"Stage", -1}, {"Fault", 1}});
    const auto graph = compile(document, s);
    require(validateOfflineRuntimeGraph(graph).ok(), "GeneratedIndex public offline validation admits its prepared backend");
    RealtimeRenderSession session(graph), reference(graph);
    require(session.ready() && reference.ready(), "amplitude GeneratedIndex rollback sessions prepare");
    std::array<double, 32> left{}, right{}, refL{}, refR{};
    allocations.store(0); countAllocations.store(true);
    const auto failed = session.render(left, right);
    countAllocations.store(false);
    require(!failed && allocations.load() == 0
            && session.currentGlobalEventValue("Stage", talas::expr::ValueDomain::Scalar) == -1
            && std::ranges::all_of(left, [](double value) { return value == 0; })
            && std::ranges::all_of(right, [](double value) { return value == 0; }),
            "amplitude failure restores published index, audio and clock without allocation: ok="
                + std::to_string(failed) + " allocations=" + std::to_string(allocations.load())
                + " index=" + std::to_string(session.currentGlobalEventValue(
                    "Stage", talas::expr::ValueDomain::Scalar).value_or(-99)));
    require(session.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
            && reference.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
            && reference.render(refL, refR), "repair amplitude GeneratedIndex reader");
    allocations.store(0); countAllocations.store(true);
    const auto recovered = session.render(left, right);
    countAllocations.store(false);
    require(recovered && allocations.load() == 0, "amplitude index rollback permits a clean retry");
    expect(left, refL); expect(right, refR);
    auto forged = compile(document, s);
    auto& plan = const_cast<RuntimePlan&>(forged.plan());
    auto& owner = *std::ranges::find(plan.steps, "talas.analogSequencer", &CompileStep::typeId);
    std::ranges::find(owner.parameters, "generatedIndex", &ParameterValue::id)->text = "!Different";
    require(!RealtimeRenderSession(forged).ready() && !validateOfflineRuntimeGraph(forged).ok(),
        "changing a sealed GeneratedIndex target rejects both execution paths");
}

talas::project::ProjectDocument analogVariableArrayInstrument(bool classic, bool reusable, bool liveRate)
{
    auto document = analogLiveArrayInstrument(classic);
    auto& parameters = document.graph.nodes[1].parameters;
    for (auto& parameter : parameters)
    {
        if (!parameter.text) continue;
        for (const auto name : {"Length", "Duty", "Down", "Interval", "VelocityScale", "Timbre"})
        {
            const auto event = "!" + std::string(name);
            std::size_t offset = 0;
            while ((offset = parameter.text->find(event, offset)) != std::string::npos)
                (*parameter.text)[offset++] = '@';
        }
    }
    std::ranges::find(parameters, "extraValues",
        &talas::project::ProjectParameterExpression::parameterId)->text = "#(!StageMark {0.1 * @Mark})";
    parameters.push_back(text("generatedIndex", "!SeqIndex"));
    if (liveRate)
        std::ranges::find(parameters, "rate",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "@Rate";
    for (const auto [name, initial] : {
             std::pair{"Length", 20.0}, {"Duty", 0.5}, {"Down", 1.0}, {"Interval", 0.0},
             {"VelocityScale", 0.4}, {"Timbre", 0.1}, {"Mark", 0.0}, {"Rate", 1.0}})
        document.variableLibrary.variables.push_back({name, name,
            talas::expr::ValueDomain::Scalar, initial, {talas::project::ProjectVariableOwnerKind::Project, {}}, true});
    if (reusable)
    {
        wrapAnalogInstrument(document);
        document.variableLibrary.variables.push_back({"VelocityScale", "Local velocity",
            talas::expr::ValueDomain::Scalar, 0.6,
            {talas::project::ProjectVariableOwnerKind::SoundDefinition, "connectedSequence"}, true});
    }
    return document;
}

std::vector<OfflineMidiVoiceEvent> analogVariableArrayChanges(const PreparedRuntimeGraph& graph, bool liveRate)
{
    std::vector<OfflineMidiVoiceEvent> events;
    for (const auto [frame, name, value] : {
             std::tuple{5U, "Length", 40.0}, {5U, "Duty", 0.25},
             {5U, "VelocityScale", 1.2}, {5U, "Timbre", 0.2}, {5U, "Mark", 2.0}, {5U, "Interval", 3.0},
             {25U, "Length", 10.0}, {25U, "Duty", 1.0}, {25U, "VelocityScale", 0.8},
             {25U, "Timbre", 0.3}, {25U, "Mark", 4.0}, {25U, "Interval", -2.0}, {25U, "Down", 0.0},
             {35U, "Rate", 0.5}})
    {
        if (std::string_view(name) == "Rate" && !liveRate) continue;
        const talas::project::PreparedProjectVariableCell* selected = nullptr;
        for (const auto& cell : graph.preparedVariables().cells)
            if (cell.identity.variableId == name && (!selected || cell.scopeOrdinal > selected->scopeOrdinal))
                selected = &cell;
        require(selected != nullptr, "stage-array variable target exists");
        OfflineMidiVoiceEvent event;
        event.frame = frame; event.batchId = frame + 1; event.sequence = events.size() + 1;
        event.kind = OfflineMidiVoiceEventKind::VariableAssignment;
        event.target = selected->identity; event.value = value; event.domain = selected->domain;
        events.push_back(std::move(event));
    }
    return events;
}

void queueAnalogVariableChanges(RealtimeRenderSession& session,
    std::span<const OfflineMidiVoiceEvent> events, std::size_t offset, std::size_t frames)
{
    std::array<PreparedTypedEvent, 32> typed{};
    std::array<RealtimeTypedEventDispatchResult, 32> results{};
    std::size_t count = 0;
    for (const auto& event : events)
    {
        if (event.frame < offset || event.frame >= offset + frames) continue;
        const auto target = session.variableTargetIndex(event.target, event.domain);
        require(target.has_value(), "project/instance variable has a writable target");
        TypedEventPayload payload{};
        payload.variable = {event.value, event.domain};
        typed[count++] = {TypedEventKind::VariableAssignment,
            {TypedEventTargetKind::Variable, *target, 1, 1},
            static_cast<std::size_t>(event.frame - offset), event.sequence, payload};
    }
    if (count == 0) return;
    allocations.store(0); countAllocations.store(true);
    const auto accepted = session.scheduleTypedEvents(std::span(typed).first(count), std::span(results).first(count));
    countAllocations.store(false);
    require(accepted && allocations.load() == 0
            && std::ranges::all_of(std::span(results).first(count), [](auto result) {
                return result == RealtimeTypedEventDispatchResult::Handled;
            }), "stage-array variable changes queue without allocation");
}

void testAnalogSequencerVariableArraysSampleAcceptedStages()
{
    for (const bool classic : {false, true})
        for (const bool reusable : {false, true})
            for (const bool liveRate : {false, true})
            for (const bool stepped : {false, true})
            {
                auto document = analogVariableArrayInstrument(classic, reusable, liveRate);
                if (stepped)
                {
                    auto& nodes = reusable ? document.soundDefinitions.front().graph.nodes : document.graph.nodes;
                    auto& parameters = std::ranges::find(nodes, "talas.analogSequencer",
                        &talas::project::ProjectNode::typeId)->parameters;
                    std::ranges::find(parameters, "step",
                        &talas::project::ProjectParameterExpression::parameterId)->expression = "!SeqStep";
                }
                const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
                require(reopened.ok(), "variable arrays save and reopen");
                auto s = settings(); s.eventValues.push_back({"SeqGate", 1});
                if (stepped)
                {
                    s.eventValues.push_back({"SeqStep", 0});
                    s.eventValueChanges = {{2, "SeqStep", 1}, {3, "SeqStep", 0},
                        {7, "SeqStep", 1}, {8, "SeqStep", 0}, {22, "SeqStep", 1}, {23, "SeqStep", 0},
                        {62, "SeqStep", 1}, {63, "SeqStep", 0}, {90, "SeqStep", 1}, {91, "SeqStep", 0}};
                }
                const auto graph = compile(reopened.document, s);
                const auto changes = analogVariableArrayChanges(graph, liveRate);
                std::array<double, 96> expected{};
                const auto first = (reusable ? 0.3 : 0.2) + (classic ? 0.0 : 0.1);
                const auto delay = stepped ? 2 : 0;
                std::fill(expected.begin() + 6 + delay, expected.begin() + 11 + delay, first);
                std::fill(expected.begin() + 26 + delay, expected.begin() + 31 + delay, classic ? 0.81 : 1.01);
                if (classic)
                {
                    // Classic arrays always play KeyDown=1, so the final stage
                    // remains audible after the modern Down variable becomes zero.
                    const auto third = liveRate ? (stepped ? 90U : 85U) : (60U + delay);
                    const auto end = std::min<std::size_t>(expected.size(), third + (liveRate ? 20U : 10U) + 1);
                    const auto begin = std::min<std::size_t>(expected.size(), third + 6);
                    std::fill(expected.begin() + begin, expected.begin() + end, 0.8);
                }
                OfflineRenderSession offline(graph);
                require(offline.scheduleMidiVoiceEvents(changes), "offline array variable timeline schedules");
                const auto audio = offline.render(expected.size());
                require(audio.ok(), audio.diagnostics.empty() ? "offline variable arrays render" : audio.diagnostics.front().message);
                expect(audio.mono, expected); expect(audio.right, expected, 0.5);
                for (const auto block : {1U, 7U, 64U})
                {
                    RealtimeRenderSession session(graph);
                    require(session.ready(), session.error());
                    std::array<double, 96> left{}, right{};
                    for (std::size_t offset = 0; offset < left.size(); offset += block)
                    {
                        const auto count = std::min<std::size_t>(block, left.size() - offset);
                        queueAnalogVariableChanges(session, changes, offset, count);
                        allocations.store(0); countAllocations.store(true);
                        const auto rendered = session.render(std::span(left).subspan(offset, count),
                            std::span(right).subspan(offset, count));
                        countAllocations.store(false);
                        require(rendered && allocations.load() == 0, "variable arrays render without allocation");
                        if (block == 1 && offset == 80)
                            require(session.currentGlobalEventValue("SeqIndex", talas::expr::ValueDomain::Scalar)
                                    == (liveRate ? 1 : 2),
                                "captured Duration survives assignments while live Rate advances its remaining budget");
                    }
                    expect(left, expected); expect(right, expected, 0.5);
                    session.resetTransport();
                    // Non-voice variables deliberately survive transport reset.
                    std::array<double, 32> resetL{}, resetR{}, resetExpected{};
                    if (classic)
                    {
                        const auto duration = liveRate ? 20U : 10U;
                        const auto firstStage = stepped ? 2U : 0U;
                        const auto secondStage = stepped ? 22U : duration;
                        const auto addStage = [&](std::size_t stage) {
                            const auto begin = std::min(resetExpected.size(), stage + 6);
                            const auto end = std::min(resetExpected.size(), stage + duration + 1);
                            std::fill(resetExpected.begin() + begin, resetExpected.begin() + end, 0.8);
                        };
                        addStage(firstStage); addStage(secondStage);
                        if (!stepped) addStage(2 * duration);
                    }
                    require(session.render(resetL, resetR), "variable arrays render after transport reset");
                    expect(resetL, resetExpected); expect(resetR, resetExpected, 0.5);
                }
            }
}

void testAnalogSequencerVariableArraysTransferAndRollback()
{
    {
        auto document = analogVariableArrayInstrument(false, false, false);
        std::ranges::find(document.variableLibrary.variables, "Length",
            &talas::project::ProjectVariableDeclaration::variableId)->initialValue = 0;
        auto s = settings(); s.eventValues.push_back({"SeqGate", 0});
        const auto graph = compile(document, s);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::array<double, 8> silentL{}, silentR{}, silence{};
        require(session.render(silentL, silentR), "closed Gate defers an invalid variable duration");
        expect(silentL, silence); expect(silentR, silence);
        const auto& cell = *std::ranges::find(graph.preparedVariables().cells, "Length",
            [](const auto& value) { return value.identity.variableId; });
        OfflineMidiVoiceEvent repair;
        repair.kind = OfflineMidiVoiceEventKind::VariableAssignment;
        repair.target = cell.identity; repair.value = 20; repair.domain = cell.domain;
        require(session.updateEventValue("SeqGate", 1, talas::expr::ValueDomain::Scalar),
            "open Gate in the same sample as the variable repair");
        queueAnalogVariableChanges(session, std::span(&repair, 1), 0, 24);
        std::array<double, 24> left{}, right{}, expected{};
        allocations.store(0); countAllocations.store(true);
        const auto rendered = session.render(left, right);
        countAllocations.store(false);
        require(rendered && allocations.load() == 0, "the accepted stage sees the repaired variable without allocation");
        std::fill(expected.begin() + 6, expected.begin() + 11, 0.3);
        expect(left, expected); expect(right, expected, 0.5);
    }
    for (const bool extraFailure : {false, true})
    {
        auto document = analogVariableArrayInstrument(false, false, true);
        auto& parameters = document.graph.nodes[1].parameters;
        if (extraFailure)
            std::ranges::find(parameters, "extraValues",
                &talas::project::ProjectParameterExpression::parameterId)->text =
                    "#(!StageMark {0.1 * @Mark / (1 - !Fault * (@Mark > 0))})";
        else
            std::ranges::find(parameters, "keyVelocities",
                &talas::project::ProjectParameterExpression::parameterId)->text =
                    "{0.5 * @VelocityScale / (1 - !Fault * (@VelocityScale > 0.5))}";
        auto s = settings(); s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"Fault", 1}});
        const auto graph = compile(document, s);
        const auto changes = analogVariableArrayChanges(graph, true);
        RealtimeRenderSession session(graph), reference(graph);
        require(session.ready() && reference.ready(), "variable array failure sessions prepare");
        require(reference.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar),
            "set the clean reference before queuing later variable changes");
        queueAnalogVariableChanges(session, changes, 0, 64);
        queueAnalogVariableChanges(reference, changes, 0, 64);
        std::array<double, 64> left{}, right{}, refL{}, refR{};
        allocations.store(0); countAllocations.store(true);
        const auto rendered = session.render(left, right);
        countAllocations.store(false);
        require(!rendered && allocations.load() == 0
                && session.currentGlobalEventValue("SeqIndex", talas::expr::ValueDomain::Scalar) == 0
                && std::ranges::all_of(left, [](double value) { return value == 0; }),
            "failed variable member restores stage, variable assignments and full callback output");
        const auto retained = session.preparedVariableValues();
        require(retained.publicationValid && std::ranges::all_of(retained.values, [&](const auto& value) {
                const auto cell = std::ranges::find(graph.preparedVariables().cells, value.identity,
                    &talas::project::PreparedProjectVariableCell::identity);
                return cell != graph.preparedVariables().cells.end()
                    && value.value.value == cell->initialValue
                    && value.value.domain == cell->domain;
            }), "failure restores published variable cells to the pre-callback state");
        require(session.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar),
            "repair the failed session's variable-dependent member");
        require(reference.render(refL, refR), reference.error());
        allocations.store(0); countAllocations.store(true);
        const auto recovered = session.render(left, right);
        countAllocations.store(false);
        require(recovered && allocations.load() == 0, "variable member failure retries without allocation");
        expect(left, refL); expect(right, refR);
    }
    const auto document = analogVariableArrayInstrument(false, true, false);
    auto s = settings(); s.eventValues.push_back({"SeqGate", 1});
    const auto graph = compile(document, s);
    const auto changes = analogVariableArrayChanges(graph, false);
    RealtimeRenderSession source(graph), target(graph), reference(graph);
    require(source.ready() && target.ready() && reference.ready(), "variable-array transfer sessions prepare");
    queueAnalogVariableChanges(source, changes, 0, 16);
    std::array<double, 16> prefixL{}, prefixR{};
    require(source.render(prefixL, prefixR), "variable-array transfer prefix");
    allocations.store(0); countAllocations.store(true);
    const auto transferred = target.transferPerformanceStateFrom(source);
    countAllocations.store(false);
    require(transferred && allocations.load() == 0, "variable arrays transfer cells and accepted stage without allocation");
    target.synchronizeTransportFrame(16);
    queueAnalogVariableChanges(target, changes, 16, 48);
    queueAnalogVariableChanges(reference, changes, 0, 64);
    std::array<double, 48> suffixL{}, suffixR{};
    std::array<double, 64> refL{}, refR{};
    require(target.render(suffixL, suffixR) && reference.render(refL, refR), "transferred variable arrays render");
    expect(suffixL, std::span(refL).subspan(16)); expect(suffixR, std::span(refR).subspan(16));
}

talas::project::ProjectDocument analogTimedArrayInstrument(bool classic, bool reusable,
    bool stepped, bool liveRate)
{
    auto document = analogVariableArrayInstrument(classic, reusable, liveRate);
    auto& nodes = reusable ? document.soundDefinitions.front().graph.nodes : document.graph.nodes;
    auto& parameters = std::ranges::find(nodes, "talas.analogSequencer",
        &talas::project::ProjectNode::typeId)->parameters;
    const auto set = [&](std::string_view id, std::string source) {
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->text = std::move(source);
    };
    set("durations", "{(20 + 20 * (1 ramp: 40 ms)) ms}");
    set("dutyCycles", "{0.5 + 0.5 * (1 ramp: 40 ms)}");
    const std::string pitch = "{(60 + @Interval + 4 * (1 ramp: 40 ms)) nn}";
    set(classic ? "pitches" : "keyPitches", pitch + " " + pitch + " " + pitch);
    set(classic ? "velocities" : "keyVelocities",
        "{0.5 * @VelocityScale + 0.4 * (!RampGate ramp: 20 ms)}");
    if (!classic)
    {
        set("keyDowns", "#(1 1 1)");
        set("keyTimbres", "{0.1 + 0.2 * (1 ramp: 40 ms)}");
    }
    set("extraValues", "#(!StageMark {0.1 * (1 ramp: 40 ms)})");
    if (stepped)
        std::ranges::find(parameters, "step",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "!SeqStep";
    return document;
}

RenderSettings analogTimedArraySettings(bool stepped)
{
    auto s = settings();
    s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"RampGate", 1}, {"SeqStep", 0}});
    s.eventValueChanges = {{5, "RampGate", 0}, {6, "RampGate", 1},
        {24, "RampGate", 0}, {25, "RampGate", 1}};
    if (stepped)
    {
        const std::array<talas::runtime::EventValueChange, 10> steps{{
            {2, "SeqStep", 1}, {3, "SeqStep", 0}, {7, "SeqStep", 1}, {8, "SeqStep", 0},
            {32, "SeqStep", 1}, {33, "SeqStep", 0}, {35, "SeqStep", 1}, {36, "SeqStep", 0},
            {72, "SeqStep", 1}, {73, "SeqStep", 0}}};
        s.eventValueChanges.insert(s.eventValueChanges.end(), steps.begin(), steps.end());
        std::ranges::stable_sort(s.eventValueChanges, {}, &talas::runtime::EventValueChange::frame);
    }
    return s;
}

std::array<double, 128> analogTimedArrayExpected(bool classic, bool reusable, bool stepped)
{
    std::array<double, 128> expected{};
    const auto base = reusable ? 0.3 : 0.2;
    if (stepped)
    {
        // Accepted frames 2, 32, 72 capture durations 21, 36, 40 ms and
        // duty cycles .525, .9, 1. Early pulses at 7 and 35 remain masked.
        std::fill(expected.begin() + 8, expected.begin() + 14, base + (classic ? 0.045 : 0.155));
        std::fill(expected.begin() + 38, expected.begin() + 65, base + (classic ? 0.23 : 0.49));
        std::fill(expected.begin() + 78, expected.begin() + 113, base + (classic ? 0.51 : 0.81));
    }
    else
    {
        // Accepted frames 0, 20, 50 capture durations 20, 30, 40 ms. The
        // velocity ramp sees both trigger resets between accepted stages.
        std::fill(expected.begin() + 6, expected.begin() + 11, base + (classic ? 0.0 : 0.1));
        std::fill(expected.begin() + 26, expected.begin() + 44, base + (classic ? 0.34 : 0.54));
        std::fill(expected.begin() + 56, expected.begin() + 91, base + (classic ? 0.51 : 0.81));
    }
    return expected;
}

void verifyAnalogTimedArrayAudio(const PreparedRuntimeGraph& graph, std::span<const double> expected)
{
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), offline.diagnostics.empty() ? "timed arrays render offline" : offline.diagnostics.front().message);
    expect(offline.mono, expected); expect(offline.right, expected, 0.5);
    for (const auto block : {1U, 7U, 64U})
    {
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        std::vector<double> left(expected.size()), right(expected.size());
        for (int pass = 0; pass < 2; ++pass)
        {
            for (std::size_t offset = 0; offset < expected.size(); offset += block)
            {
                const auto count = std::min<std::size_t>(block, expected.size() - offset);
                std::array<double*, 2> channels{left.data() + offset, right.data() + offset};
                allocations.store(0); countAllocations.store(true);
                const auto empty = session.render({}, channels.data(), channels.size(), 0);
                const auto rendered = session.render(std::span(left).subspan(offset, count),
                    std::span(right).subspan(offset, count));
                countAllocations.store(false);
                if (!empty || !rendered || allocations.load() != 0)
                    throw std::runtime_error("timed array callback at frame " + std::to_string(offset)
                        + ", pass " + std::to_string(pass) + ", block " + std::to_string(block)
                        + ": empty=" + std::to_string(empty) + ", rendered=" + std::to_string(rendered)
                        + ", allocations=" + std::to_string(allocations.load()) + " " + std::string(session.error()));
            }
            expect(left, expected); expect(right, expected, 0.5);
            session.resetTransport();
        }
    }
}

void testAnalogSequencerTimedArraysSampleContinuousMembers()
{
    for (const bool classic : {false, true})
        for (const bool reusable : {false, true})
            for (const bool stepped : {false, true})
                for (const bool liveRate : {false, true})
                {
                    const auto document = analogTimedArrayInstrument(classic, reusable, stepped, liveRate);
                    const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
                    require(reopened.ok(), "timed classic/modern arrays save and reopen");
                    const auto graph = compile(reopened.document, analogTimedArraySettings(stepped));
                    verifyAnalogTimedArrayAudio(graph, analogTimedArrayExpected(classic, reusable, stepped));
                }
    auto mixed = analogTimedArrayInstrument(false, false, false, false);
    std::ranges::find(mixed.graph.nodes[1].parameters, "keyPitches",
        &talas::project::ProjectParameterExpression::parameterId)->text =
        "60 {(60 + @Interval + 4 * (1 ramp: 40 ms)) nn} !LastPitch";
    auto s = analogTimedArraySettings(false);
    s.eventValues.push_back({"LastPitch", 64, talas::expr::ValueDomain::NoteNumber});
    verifyAnalogTimedArrayAudio(compile(mixed, s), analogTimedArrayExpected(false, false, false));
}

void testAnalogSequencerTimedArraysGateAndTransfer()
{
    const auto document = analogTimedArrayInstrument(false, true, false, true);
    auto s = analogTimedArraySettings(false);
    const auto graph = compile(document, s);
    const auto expected = analogTimedArrayExpected(false, true, false);
    RealtimeRenderSession source(graph), transferred(graph);
    std::array<double, 16> prefixL{}, prefixR{};
    require(source.render(prefixL, prefixR), "timed array transfer prefix");
    allocations.store(0); countAllocations.store(true);
    const auto moved = transferred.transferPerformanceStateFrom(source);
    countAllocations.store(false);
    require(moved && allocations.load() == 0, "timed array transfer retains each member's independent state");
    transferred.synchronizeTransportFrame(16);
    std::array<double, 112> left{}, right{};
    for (std::size_t offset = 0; offset < left.size(); offset += 7)
    {
        const auto count = std::min<std::size_t>(7, left.size() - offset);
        require(transferred.render(std::span(left).subspan(offset, count),
                std::span(right).subspan(offset, count)), "transferred timed arrays continue");
    }
    expect(left, std::span(expected).subspan(16)); expect(right, std::span(expected).subspan(16), 0.5);

    // The Sound clock and member ramps continue behind closed Gate and do
    // not restart when Gate restarts the sequence's stage index.
    std::ranges::find(s.eventValues, "SeqGate", &EventValueBinding::name)->value = 0;
    s.eventValueChanges = {{30, "SeqGate", 1}, {50, "SeqGate", 0}, {80, "SeqGate", 1}};
    const auto gatedGraph = compile(analogTimedArrayInstrument(false, false, false, false), s);
    std::array<double, 128> gatedExpected{};
    std::fill(gatedExpected.begin() + 36, gatedExpected.begin() + 51, 0.935);
    std::fill(gatedExpected.begin() + 86, gatedExpected.begin() + 121, 1.01);
    std::fill(gatedExpected.begin() + 126, gatedExpected.end(), 1.01);
    verifyAnalogTimedArrayAudio(gatedGraph, gatedExpected);

    auto published = analogTimedArrayInstrument(false, false, false, false);
    // The ramp starting at its first value, as projects before format 100
    // have it, so that stage 0 publishes no ramp yet.
    published.rampsStartAtFirstValue = true;
    auto& parameters = published.graph.nodes[1].parameters;
    for (const auto [id, text] : {
             std::pair{"durations", "#(0.02)"}, {"dutyCycles", "#(1)"},
             {"keyPitches", "#(60 60 60)"}, {"keyVelocities", "#(0.2)"},
             {"keyTimbres", "#(0)"}, {"extraValues", "#(!StageMark {0.1 * (!SeqIndex ramp: 20 ms)})"}})
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->text = text;
    auto publishedSettings = analogTimedArraySettings(false);
    publishedSettings.eventValueChanges.clear();
    std::array<double, 128> publishedExpected{};
    std::fill(publishedExpected.begin() + 6, publishedExpected.begin() + 21, 0.2);
    std::fill(publishedExpected.begin() + 26, publishedExpected.begin() + 41, 0.2);
    std::fill(publishedExpected.begin() + 46, publishedExpected.begin() + 61, 0.3);
    // Stage 1 publishes its index after the timed member snapshot. Its ramp
    // starts at that publication and reaches one by the accepted stage at 40.
    verifyAnalogTimedArrayAudio(compile(published, publishedSettings), publishedExpected);
}

void testAnalogSequencerTimedArraysRollback()
{
    for (const bool memberFailure : {false, true})
    {
        auto document = analogTimedArrayInstrument(false, false, false, false);
        if (memberFailure)
            std::ranges::find(document.graph.nodes[1].parameters, "extraValues",
                &talas::project::ProjectParameterExpression::parameterId)->text =
                "#(!StageMark {0.1 * (1 ramp: 40 ms) / (1 - !Fault * ((1 ramp: 20 ms) > 0.5))})";
        else
            document.graph.nodes[0].parameters[0].expression =
                "(!KeyDown > 0) * (!KeyVelocity + !KeyTimbre + !StageMark + 0.01 * (!KeyPitch > 61 nn))"
                " / (1 - !Fault * (!KeyVelocity > 0.45))";
        auto s = analogTimedArraySettings(false); s.eventValues.push_back({"Fault", 1});
        const auto graph = compile(document, s);
        RealtimeRenderSession failed(graph), reference(graph);
        require(failed.ready() && reference.ready(), "timed member rollback sessions prepare");
        require(reference.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar), "prepare repaired reference");
        std::array<double, 64> left{}, right{}, refL{}, refR{};
        allocations.store(0); countAllocations.store(true);
        const auto rendered = failed.render(left, right);
        countAllocations.store(false);
        require(!rendered && allocations.load() == 0
                && std::ranges::all_of(left, [](double value) { return value == 0; })
                && failed.currentGlobalEventValue("SeqIndex", talas::expr::ValueDomain::Scalar) == 0,
            "late member/DSP failure rolls back every timed state, publication and output sample");
        require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
                && reference.render(refL, refR), "repair timed array failure");
        allocations.store(0); countAllocations.store(true);
        const auto retried = failed.render(left, right);
        countAllocations.store(false);
        require(retried && allocations.load() == 0, "timed member retry restores the original clock without allocation");
        expect(left, refL); expect(right, refR);
        const auto expected = analogTimedArrayExpected(false, false, false);
        expect(left, std::span(expected).first(64));
    }
}

void testAnalogSequencerTimedArraysUseScoreSourceClock()
{
    auto document = analogTimedArrayInstrument(false, false, false, false);
    talas::project::ProjectSoundDefinition sound;
    sound.definitionId = "timedSequence"; sound.displayName = "Timed sequence";
    sound.graph = std::move(document.graph);
    std::erase_if(sound.graph.nodes, [](const auto& node) { return node.instanceId == "out"; });
    std::erase_if(sound.graph.connections, [](const auto& input) { return input.toNode == "out"; });
    sound.outputs = {{"leftOut", talas::core::PortType::Audio, "voice", "leftOut"},
        {"rightOut", talas::core::PortType::Audio, "voice", "rightOut"}};
    document.graph = {};
    document.soundDefinitions.push_back(std::move(sound));
    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track,
            talas::project::ScoreTimeDomain::Seconds, "master", 2, talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master,
            talas::project::ScoreTimeDomain::Seconds, {}, 2, talas::project::ScoreOverlapMode::Layer}};
    const auto seconds = [](double value) { return talas::project::ScoreTime{talas::project::ScoreTimeDomain::Seconds, value, 0}; };
    document.timeline.score.clips = {{"clip", "track", "timedSequence", seconds(0.015), seconds(0.096),
        seconds(0.020), seconds(0), seconds(0), 1}};
    const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
    require(reopened.ok(), "timed array Score source saves and reopens with a nonzero content offset");
    for (const bool loop : {false, true})
    {
        auto s = analogTimedArraySettings(false); s.eventValueChanges.clear();
        if (loop) s.transportLoop = TransportLoopRegion{0, 64};
        const auto graph = compile(reopened.document, s);
        require(graph.plan().midiVoices.empty(), "Score owns its sequencer in an independent source");
        std::array<double, 128> expected{};
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
        {
            const auto local = loop ? frame % 64 : frame;
            // Source frames 26..43 and 56..90 appear at Score frames 21..38
            // and 51..85 after the 20-frame offset and 15-frame clip start.
            if (local >= 21 && local <= 38) expected[frame] = 0.86;
            if (local >= 51 && local <= 85) expected[frame] = 1.01;
        }
        if (!loop)
            verifyAnalogTimedArrayAudio(graph, expected);
        else
        {
            for (const auto block : {1U, 7U, 64U})
            {
                RealtimeRenderSession session(graph);
                require(session.ready(), session.error());
                std::array<double, 128> left{}, right{};
                for (std::size_t offset = 0; offset < left.size(); offset += block)
                {
                    const auto count = std::min<std::size_t>(block, left.size() - offset);
                    allocations.store(0); countAllocations.store(true);
                    const auto rendered = session.render(std::span(left).subspan(offset, count),
                        std::span(right).subspan(offset, count));
                    countAllocations.store(false);
                    require(rendered && allocations.load() == 0,
                        "Score wraps restart timed member source state without allocating");
                }
                expect(left, expected); expect(right, expected, 0.5);
            }
        }
    }
}

talas::project::ProjectDocument analogLiveRangeInstrument(bool looping = true)
{
    auto document = analogGateInstrument(looping);
    auto& parameters = document.graph.nodes[1].parameters;
    for (const auto [id, expression] : {
             std::pair{"startIndex", "!RangeStart"}, {"endIndex", "!RangeEnd"}})
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->expression = expression;
    for (const auto [id, value] : {
             std::pair{"durations", "#(0.02)"}, {"dutyCycles", "#(0.75)"},
             {"keyPitches", "#(60 62 64 67)"}, {"keyVelocities", "#(0.2 0.4 0.6 0.8)"}})
        std::ranges::find(parameters, id,
            &talas::project::ProjectParameterExpression::parameterId)->text = value;
    parameters.push_back(text("extraValues", "#(!Morph 0.2 0.4 0.6 0.8)"));
    parameters.push_back(text("generatedIndex", "!SeqIndex"));
    document.graph.nodes[0].parameters[0].expression =
        "(!KeyDown > 0) * (!KeyVelocity + !Morph) / 2";
    return document;
}

void testAnalogSequencerFixedLiveRangeMatchesStatic()
{
    for (const auto rate : {0.7, -1.3})
    {
        auto document = analogLiveRangeInstrument();
        auto& parameters = document.graph.nodes[1].parameters;
        std::ranges::find(parameters, "rate",
            &talas::project::ProjectParameterExpression::parameterId)->expression = std::to_string(rate);
        std::ranges::find(parameters, "startIndex",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "1";
        std::ranges::find(parameters, "endIndex",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "2";
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"RangeStart", 1}, {"RangeEnd", 2}, {"SeqIndex", 0}});
        const auto referenceGraph = compile(document, s);
        const auto reference = OfflineRenderSession(referenceGraph).render(96);
        require(reference.ok(), "static selected range renders");
        for (const bool legacy : {false, true})
        {
            auto candidate = document;
            auto& liveParameters = candidate.graph.nodes[1].parameters;
            // A single live endpoint must preserve the other static endpoint.
            std::ranges::find(liveParameters, "startIndex",
                &talas::project::ProjectParameterExpression::parameterId)->expression = legacy ? "!RangeStart + 1" : "!RangeStart";
            std::ranges::find(liveParameters, "endIndex",
                &talas::project::ProjectParameterExpression::parameterId)->expression = legacy ? "3" : "2";
            if (legacy) liveParameters.push_back(choice("indexOrigin", "legacyOneBased"));
            if (rate < 0) useClassicAnalogSequencerArrays(candidate);
            verifyAnalogTimedArrayAudio(compile(candidate, s), reference.mono);
        }
    }
}

void testAnalogSequencerLiveRangeTraversal()
{
    for (const bool stepped : {false, true})
    {
        auto document = analogLiveRangeInstrument();
        auto& parameters = document.graph.nodes[1].parameters;
        std::ranges::find(parameters, "rate",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "!SeqRate";
        if (stepped)
        {
            std::ranges::find(parameters, "step",
                &talas::project::ProjectParameterExpression::parameterId)->expression = "!SeqStep";
            useClassicAnalogSequencerArrays(document);
            connectAnalogKeyControl(document, "rate");
            connectAnalogKeyControl(document, "step");
            wrapAnalogInstrument(document);
        }
        const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
        require(reopened.ok(), "live selected range saves and reopens");
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"SeqRate", 1}, {"SeqStep", 1},
            {"RangeStart", 0}, {"RangeEnd", 1}, {"SeqIndex", 0}});
        s.eventValueChanges = {{5, "RangeStart", 2}, {5, "RangeEnd", 3},
            {25, "RangeStart", 0}, {45, "RangeEnd", 1}, {65, "SeqRate", -1},
            {85, "RangeStart", 2}, {85, "RangeEnd", 3},
            {105, "RangeStart", 0}, {105, "RangeEnd", 1}};
        if (stepped)
        {
            s.eventValueChanges.push_back({1, "SeqStep", 0});
            for (const auto frame : {5U, 15U, 20U, 25U, 40U, 50U, 60U, 70U, 80U, 90U, 100U, 110U, 120U})
            {
                s.eventValueChanges.push_back({frame, "SeqStep", 1});
                s.eventValueChanges.push_back({frame + 1, "SeqStep", 0});
            }
        }
        std::ranges::stable_sort(s.eventValueChanges, {}, &EventValueChange::frame);
        const auto graph = compile(reopened.document, s);
        // Changes during a held stage preserve its note and 20-frame mask.
        // Forward clamping, forward wrap, reverse wrap and reverse clamping
        // select stages 0,2,3,0,1,3,1 at the accepted boundaries.
        const std::array<std::size_t, 7> stages{0, 2, 3, 0, 1, 3, 1};
        std::array<double, 128> expected{};
        for (std::size_t i = 0; i < stages.size(); ++i)
            std::fill(expected.begin() + i * 20 + 6,
                expected.begin() + std::min(expected.size(), i * 20 + 16), 0.2 * (stages[i] + 1));
        verifyAnalogTimedArrayAudio(graph, expected);
        RealtimeRenderSession indexed(graph), transferred(graph);
        std::array<double, 1> left{}, right{};
        for (std::size_t frame = 0; frame < 47; ++frame)
        {
            require(indexed.render(left, right), "live range single-frame render");
            require(indexed.currentGlobalEventValue("SeqIndex", talas::expr::ValueDomain::Scalar) == stages[frame / 20],
                "GeneratedIndex changes only on accepted stages");
        }
        require(transferred.transferPerformanceStateFrom(indexed), "live range state transfers while the active stage lies outside new bounds");
        transferred.synchronizeTransportFrame(47);
        std::array<double, 81> tailL{}, tailR{};
        for (std::size_t offset = 0; offset < tailL.size(); offset += 7)
        {
            const auto count = std::min<std::size_t>(7, tailL.size() - offset);
            require(transferred.render(std::span(tailL).subspan(offset, count),
                std::span(tailR).subspan(offset, count)), "transferred live range continues");
        }
        expect(tailL, std::span(expected).subspan(47));
        expect(tailR, std::span(expected).subspan(47), 0.5);
    }
}

void testAnalogSequencerLiveRangeExhaustionAndRollback()
{
    auto document = analogLiveRangeInstrument(false);
    auto s = settings();
    s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"RangeStart", 0}, {"RangeEnd", 0}, {"SeqIndex", 0}});
    s.eventValueChanges = {{25, "RangeEnd", 3}, {35, "SeqGate", 0},
        {36, "RangeStart", 2}, {36, "SeqGate", 1},
        {45, "RangeStart", 0}, {45, "RangeEnd", 1},
        {65, "SeqGate", 0}, {66, "SeqGate", 1}};
    std::array<double, 112> expected{};
    for (const auto [start, value] : {std::pair{0U, 0.2}, {36U, 0.6}, {66U, 0.2}, {86U, 0.4}})
        std::fill(expected.begin() + start + 6, expected.begin() + start + 16, value);
    verifyAnalogTimedArrayAudio(compile(document, s), expected);

    // Reject invalid values after a stage publication in the same callback,
    // including near-integers that other integer controls tolerate.
    for (const auto displacement : {"4", "0.0000000001", "-1"})
    {
        auto candidate = analogLiveRangeInstrument();
        candidate.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
        std::ranges::find(candidate.graph.nodes[1].parameters, "startIndex",
            &talas::project::ProjectParameterExpression::parameterId)->expression =
                "!RangeStart + !Fault * ((1 ramp: 40 ms) > 0.5) * " + std::string(displacement);
        auto faultSettings = settings();
        faultSettings.eventValues.insert(faultSettings.eventValues.end(), {{"SeqGate", 1},
            {"RangeStart", 0}, {"RangeEnd", 3}, {"SeqIndex", 0}, {"Fault", 1}});
        const auto graph = compile(candidate, faultSettings);
        RealtimeRenderSession failed(graph), reference(graph);
        require(failed.ready() && reference.ready(), "live range rollback sessions prepare");
        require(reference.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar), "repair range reference");
        std::array<double, 64> left{}, right{}, refL{}, refR{};
        allocations.store(0); countAllocations.store(true);
        const auto rendered = failed.render(left, right);
        countAllocations.store(false);
        require(!rendered && allocations.load() == 0
                && std::ranges::all_of(left, [](double value) { return value == 0; })
                && std::ranges::all_of(right, [](double value) { return value == 0; })
                && failed.currentGlobalEventValue("SeqIndex", talas::expr::ValueDomain::Scalar) == 0,
            "invalid live range rolls back notes, time, ExtraValues, GeneratedIndex and both output channels");
        require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
                && reference.render(refL, refR), "repair range after failed callback");
        allocations.store(0); countAllocations.store(true);
        const auto retried = failed.render(left, right);
        countAllocations.store(false);
        require(retried && allocations.load() == 0, "live range retry is allocation-free");
        expect(left, refL); expect(right, refR);
        require(failed.updateEventValue("SeqGate", 0, talas::expr::ValueDomain::Scalar)
                && failed.updateEventValue("RangeStart", 3, talas::expr::ValueDomain::Scalar)
                && failed.updateEventValue("RangeEnd", 1, talas::expr::ValueDomain::Scalar),
            "publish reversed range with Gate closed");
        std::array<double, 1> badL{99}, badR{99};
        require(!failed.render(badL, badR) && badL[0] == 0 && badR[0] == 0,
            "Gate closure does not bypass range validation");
    }
}

void testAnalogSequencerVariableAndTimedRanges()
{
    for (const bool reusable : {false, true})
    {
        auto document = analogLiveRangeInstrument();
        document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV4;
        auto& parameters = document.graph.nodes[1].parameters;
        std::ranges::find(parameters, "startIndex",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "@RangeStart";
        std::ranges::find(parameters, "endIndex",
            &talas::project::ProjectParameterExpression::parameterId)->expression = "@RangeEnd + ((1 ramp: 40 ms) > 0.5)";
        for (const auto [name, value] : {std::pair{"RangeStart", 0.0}, {"RangeEnd", 1.0}})
            document.variableLibrary.variables.push_back({name, name, talas::expr::ValueDomain::Scalar, value,
                {talas::project::ProjectVariableOwnerKind::Project, {}}, true});
        if (reusable)
        {
            useClassicAnalogSequencerArrays(document);
            wrapAnalogInstrument(document);
            for (const auto [name, value] : {std::pair{"RangeStart", 1.0}, {"RangeEnd", 2.0}})
                document.variableLibrary.variables.push_back({name, name, talas::expr::ValueDomain::Scalar, value,
                    {talas::project::ProjectVariableOwnerKind::SoundDefinition, "connectedSequence"}, true});
        }
        const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
        require(reopened.ok(), "variable/timed range saves and reopens");
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"SeqGate", 1}, {"SeqIndex", 0}});
        const auto graph = compile(reopened.document, s);
        std::vector<OfflineMidiVoiceEvent> changes;
        for (const auto [frame, name, value] : {std::tuple{25U, "RangeStart", 2.0},
                 {25U, "RangeEnd", 2.0}, {55U, "RangeStart", 0.0}, {55U, "RangeEnd", 0.0}})
        {
            const talas::project::PreparedProjectVariableCell* selected = nullptr;
            for (const auto& cell : graph.preparedVariables().cells)
                if (cell.identity.variableId == name && (!selected || cell.scopeOrdinal > selected->scopeOrdinal))
                    selected = &cell;
            require(selected != nullptr, "range variable has a source-scope target");
            OfflineMidiVoiceEvent event;
            event.frame = frame; event.batchId = frame + 1; event.sequence = changes.size() + 1;
            event.kind = OfflineMidiVoiceEventKind::VariableAssignment;
            event.target = selected->identity; event.value = value; event.domain = selected->domain;
            changes.push_back(std::move(event));
        }
        const std::array<std::size_t, 5> stages{reusable ? 1U : 0U, reusable ? 2U : 1U,
            reusable ? 3U : 2U, 0, 1};
        std::array<double, 96> expected{};
        for (std::size_t i = 0; i < stages.size(); ++i)
            std::fill(expected.begin() + i * 20 + 6, expected.begin() + i * 20 + 16,
                0.2 * (stages[i] + 1));
        OfflineRenderSession offline(graph);
        require(offline.scheduleMidiVoiceEvents(changes), "offline range assignments schedule");
        const auto audio = offline.render(expected.size());
        require(audio.ok(), "variable/timed ranges render offline");
        expect(audio.mono, expected); expect(audio.right, expected, 0.5);
        for (const auto block : {1U, 7U, 64U})
        {
            RealtimeRenderSession session(graph);
            require(session.ready(), session.error());
            std::array<double, 96> left{}, right{};
            for (std::size_t offset = 0; offset < left.size(); offset += block)
            {
                const auto count = std::min<std::size_t>(block, left.size() - offset);
                queueAnalogVariableChanges(session, changes, offset, count);
                allocations.store(0); countAllocations.store(true);
                const auto rendered = session.render(std::span(left).subspan(offset, count),
                    std::span(right).subspan(offset, count));
                countAllocations.store(false);
                require(rendered && allocations.load() == 0, "variable/timed ranges render without allocation");
            }
            expect(left, expected); expect(right, expected, 0.5);
        }
    }
}

void testAnalogSequencerLoopReleasesBeforeCycleAttack()
{
    auto document = instrument(false);
    document.graph.nodes[0].typeId = "talas.valToSound";
    document.graph.nodes[0].parameters = {{"value",
        "(!KeyDown > 0) * (!KeyVelocity + !KeyTimbre)"}};
    auto& sequencer = document.graph.nodes[1];
    sequencer.typeId = "talas.analogSequencer";
    talas::project::ProjectParameterExpression loop;
    loop.parameterId = "loop";
    loop.boolean = true;
    sequencer.parameters = {
        {"left", "1"}, {"right", "0.5"},
        {"gate", "1"}, {"step", "1"}, {"rate", "1"},
        {"startIndex", "0"}, {"endIndex", "1"}, {"polyphony", "1"},
        loop,
        text("durations", "#(0.01 0.01)"),
        text("dutyCycles", "#(1 1)"),
        text("keyDowns", "#(1 1)"),
        text("keyPitches", "#(60 64)"),
        text("keyVelocities", "#(0.2 0.3)"),
        text("keyTimbres", "#(0.1 0.2)"),
        // These fixtures retrigger at a DutyCycle of 1, as projects before
        // format 76 do.
        choice("dutyCycleOne", "legacyRetriggers"),
    };
    const auto graph = compile(document, settings());
    const auto offline = OfflineRenderSession(graph).render(65);
    require(offline.ok(), offline.diagnostics.empty()
        ? "looped Analog Sequencer offline render"
        : offline.diagnostics.front().message);
    std::array<double, 65> expected{};
    for (std::size_t cycle = 0; cycle < 3; ++cycle)
    {
        for (std::size_t frame = cycle * 20 + 6;
             frame <= cycle * 20 + 10; ++frame)
            expected[frame] = 0.3;
        for (std::size_t frame = cycle * 20 + 16;
             frame <= cycle * 20 + 20; ++frame)
            expected[frame] = 0.5;
    }
    expect(offline.mono, expected);
    expect(offline.right, expected, 0.5);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 65> left{}, right{};
    allocations.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    bool rendered = true;
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min<std::size_t>(7, left.size() - start);
        rendered = session.render(std::span(left).subspan(start, count),
            std::span(right).subspan(start, count)) && rendered;
    }
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocations.load(std::memory_order_relaxed) == 0,
        "looped Analog Sequencer prepared callbacks allocate no memory");
    expect(left, expected);
    expect(right, expected, 0.5);
    session.resetTransport();
    for (std::size_t start = 0; start < left.size(); start += 7)
    {
        const auto count = std::min<std::size_t>(7, left.size() - start);
        require(session.render(std::span(left).subspan(start, count),
            std::span(right).subspan(start, count)),
            "looped Analog Sequencer transport reset");
    }
    expect(left, expected);

    RealtimeRenderSession seek(graph);
    require(seek.ready(), seek.error());
    RealtimeTransportSnapshot snapshot;
    snapshot.epoch = 9;
    snapshot.sampleRate = 1000;
    snapshot.playing = true;
    snapshot.absoluteFrame = snapshot.playheadFrame = 20;
    seek.setTransportSnapshotBeforeRender(snapshot);
    std::array<double, 15> seekLeft{}, seekRight{}, seekOracle{};
    for (std::size_t frame = 6; frame <= 10; ++frame)
        seekOracle[frame] = 0.3;
    require(seek.render(seekLeft, seekRight),
        "looped Analog Sequencer seeks to a release/attack boundary");
    expect(seekLeft, seekOracle);

    RealtimeRenderSession source(graph), transferred(graph);
    require(source.ready() && transferred.ready(),
        "looped Analog Sequencer transfer sessions prepare");
    std::array<double, 17> prefixLeft{}, prefixRight{};
    require(source.render(prefixLeft, prefixRight),
        "looped Analog Sequencer transfer source prefix");
    require(transferred.transferPerformanceStateFrom(source),
        "looped Analog Sequencer transfers event cursor and voices");
    transferred.synchronizeTransportFrame(17);
    std::array<double, 20> transferredLeft{}, transferredRight{};
    require(transferred.render(transferredLeft, transferredRight),
        "looped Analog Sequencer renders after performance transfer");
    expect(transferredLeft,
        std::span<const double>(expected).subspan(17, transferredLeft.size()));

    auto scaledDocument = document;
    auto& scaledParameters = scaledDocument.graph.nodes[1].parameters;
    std::ranges::find(scaledParameters, "durations",
        &talas::project::ProjectParameterExpression::parameterId)->text =
            "#(0.02 0.02)";
    std::ranges::find(scaledParameters, "rate",
        &talas::project::ProjectParameterExpression::parameterId)->expression = "2";
    std::ranges::find(scaledParameters, "gate",
        &talas::project::ProjectParameterExpression::parameterId)->expression = "2";
    const auto scaledGraph = compile(scaledDocument, settings());
    const auto scaledOffline = OfflineRenderSession(scaledGraph).render(65);
    require(scaledOffline.ok(),
        "static positive Gate and doubled Rate render offline");
    expect(scaledOffline.mono, expected);
    RealtimeRenderSession scaledSession(scaledGraph);
    require(scaledSession.ready(), scaledSession.error());
    std::array<double, 65> scaledLeft{}, scaledRight{};
    for (std::size_t start = 0; start < scaledLeft.size(); start += 7)
    {
        const auto count = std::min<std::size_t>(7, scaledLeft.size() - start);
        require(scaledSession.render(
            std::span(scaledLeft).subspan(start, count),
            std::span(scaledRight).subspan(start, count)),
            "static positive Gate and doubled Rate render prepared");
    }
    expect(scaledLeft, expected);
}

talas::project::ProjectDocument nestedAnalogPlaybackInstrument(bool midiParent)
{
    auto loop = talas::project::ProjectParameterExpression{};
    loop.parameterId = "loop";
    loop.boolean = false;
    std::vector<talas::project::ProjectParameterExpression> child{
        {"left", "1"}, {"right", "0.5"}, {"gate", "1"},
        {"step", "1"}, {"rate", "1"}, {"startIndex", "0"},
        {"endIndex", "2"}, {"polyphony", "2"}, loop,
        text("durations", "#(0.01 0.01 0.01)"),
        text("dutyCycles", "#(1 1 1)"),
        text("keyDowns", "#(1 1 1)"),
        text("keyPitches", "#(60 62 64)"),
        text("keyVelocities", "#(0.25 0.5 0.75)"),
        text("keyTimbres", "#(0 0 0)"),
        text("extraValues", "#(!Mark 0.125 0.25 0.375)"),
        // A DutyCycle of 1 retriggers here, as before format 76.
        choice("dutyCycleOne", "legacyRetriggers"),
    };
    std::vector<talas::project::ProjectParameterExpression> parent;
    if (midiParent)
    {
        auto resource = text("midiFile", "");
        resource.text.reset();
        resource.resource = "notes";
        auto shared = talas::project::ProjectParameterExpression{};
        shared.parameterId = "shared";
        shared.boolean = false;
        parent = {{"left", "1"}, {"right", "0.5"}, {"channel", "1"},
            {"polyphony", "2"}, {"lowPitch", "0 nn"},
            {"highPitch", "127 nn"}, text("source", "MIDI File"),
            resource, text("script", ""), shared};
    }
    else
    {
        parent = {{"left", "1"}, {"right", "0.5"}, {"gate", "1"},
            {"step", "1"}, {"rate", "1"}, {"startIndex", "0"},
            {"endIndex", "2"}, {"polyphony", "2"}, loop,
            text("durations", "#(0.01 0.01 0.01)"),
            text("dutyCycles", "#(1 1 1)"),
            text("keyDowns", "#(1 1 1)"),
            text("keyPitches", "#(48 55 60)"),
            text("keyVelocities", "#(0.4 0.6 0.8)"),
            text("keyTimbres", "#(0 0 0)"),
            // A DutyCycle of 1 retriggers here, as before format 76.
            choice("dutyCycleOne", "legacyRetriggers"),
        };
    }
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"tone", "talas.valToSound", {{"value",
            "(!KeyDown > 0) * (!KeyVelocity + !Mark) / "
            "(1 - (!KeyDown > 0) * (!KeyVelocity > 0.5) * !Fault)"}}},
        {"inner", "talas.analogSequencer", std::move(child)},
        {"outer", midiParent ? "talas.midiVoice" : "talas.analogSequencer",
            std::move(parent)},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"tone", "audioOut", "inner", "audioIn"},
        {"inner", "leftOut", "outer", "audioIn"},
        {"outer", "leftOut", "out", "audioIn"},
        {"outer", "rightOut", "out", "rightIn"},
    };
    return document;
}

void testNestedAnalogInputPlaybackLifecycle()
{
    for (const bool midiParent : {false, true})
    {
        auto renderSettings = settings();
        renderSettings.eventValues.insert(renderSettings.eventValues.end(),
            {{"Fault", 0}, {"Mark", 99}});
        if (midiParent)
            track(renderSettings, {0,0x90,60,127, 10,0x90,64,127,
                10,0x80,60,0, 10,0x80,64,0, 1,0x90,67,127, 20,0x80,67,0});
        const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(
            nestedAnalogPlaybackInstrument(midiParent)));
        require(reopened.ok(), "nested voice instruments must survive save/reopen");
        const auto graph = compile(reopened.document, renderSettings);
        const auto parent = std::ranges::find_if(
            graph.plan().midiVoices, [](const auto& owner) {
                return owner.ownerNodeId == "outer"
                    && owner.voiceAncestors.empty();
            });
        require(parent != graph.plan().midiVoices.end()
                && parent->voices.size() == 2
                && std::ranges::count_if(graph.plan().midiVoices,
                    [](const auto& owner) {
                        return owner.ownerNodeId == "inner"
                            && owner.voiceAncestors.size() == 1;
                    }) == 2,
            "nested playback fixture has two independent parent slots and one child per slot");

        std::array<double, 64> expected{};
        const auto pulse = [&](std::size_t first, std::size_t last,
                               double value) {
            for (auto frame = first; frame <= last; ++frame)
                expected[frame] += value;
        };
        if (midiParent)
        {
            // Notes at 0/10 occupy slots 0/1; releases at 20/30 leave slot 0
            // oldest, so the note at 31 reuses it while its child has an attack pending.
            pulse(11, 15, 0.375); pulse(21, 25, 0.75);
            pulse(21, 25, 0.375); pulse(31, 35, 0.75);
            pulse(41, 45, 1.125); pulse(42, 46, 0.375);
            pulse(52, 56, 0.75); pulse(62, 63, 1.125);
        }
        else
        {
            // Outer stages trigger at 0/10/20: stages 0 and 1 occupy slots
            // 0/1, then stage 2 reuses released slot 0. Slot 1 keeps running.
            pulse(11, 15, 0.375); pulse(21, 25, 0.375);
            pulse(31, 35, 0.75); pulse(31, 35, 0.375);
            pulse(41, 45, 1.125); pulse(41, 45, 0.75);
            pulse(51, 55, 1.125);
        }
        std::array<double, 64> expectedRight{};
        std::ranges::transform(expected, expectedRight.begin(),
            [](double value) { return value * 0.5; });
        const auto offline = OfflineRenderSession(graph).render(expected.size());
        require(offline.ok(), "nested voice instrument must render offline");
        expect(offline.mono, expected);
        for (const auto block : {1U, 7U, 64U})
        {
            RealtimeRenderSession session(graph);
            require(session.ready(), session.error());
            std::array<double, 64> left{}, right{};
            require(session.render(std::span(left).first(0), std::span(right).first(0)),
                "zero-frame nested render must leave clocks untouched");
            for (std::size_t offset = 0; offset < left.size(); offset += block)
            {
                const auto count = std::min<std::size_t>(block, left.size() - offset);
                allocations.store(0); countAllocations.store(true);
                const auto rendered = session.render(std::span(left).subspan(offset, count),
                    std::span(right).subspan(offset, count));
                countAllocations.store(false);
                require(rendered && allocations.load() == 0,
                    "nested Analog Input renders across partitions without callback allocation");
            }
            expect(left, expected); expect(right, expectedRight);
            require(session.currentGlobalEventValue("Mark",
                        talas::expr::ValueDomain::Scalar) == 99,
                "child ExtraValues stay scoped to each nested Input");
            session.resetTransport();
            require(session.render(left, right),
                "nested Analog Input restarts after transport reset");
            expect(left, expected); expect(right, expectedRight);
        }

        RealtimeRenderSession source(graph), transferred(graph);
        std::array<double, 28> prefixLeft{}, prefixRight{};
        require(source.ready() && transferred.ready()
                && source.render(prefixLeft, prefixRight),
            "nested transfer source reaches the middle of both child clocks");
        transferred.resetTransport();
        require(transferred.transferPerformanceStateFrom(source),
            "nested parent and child clocks transfer together");
        transferred.synchronizeTransportFrame(28);
        std::array<double, 36> suffixLeft{}, suffixRight{};
        require(transferred.render(suffixLeft, suffixRight),
            "nested Analog Input continues after transfer");
        expect(suffixLeft, std::span(expected).subspan(28));
        expect(suffixRight, std::span(expectedRight).subspan(28));

        auto faultSettings = renderSettings;
        const auto faultBinding = std::ranges::find(
            faultSettings.eventValues, "Fault",
            &talas::runtime::EventValueBinding::name);
        require(faultBinding != faultSettings.eventValues.end(),
            "nested rollback fixture has its Fault binding");
        faultBinding->value = 1;
        const auto faultGraph = compile(
            nestedAnalogPlaybackInstrument(midiParent), faultSettings);
        RealtimeRenderSession failed(faultGraph), reference(graph);
        std::array<double, 64> failedLeft{}, failedRight{}, referenceLeft{}, referenceRight{};
        require(failed.ready() && reference.ready(), "nested rollback sessions prepare");
        allocations.store(0); countAllocations.store(true);
        const auto failedRender = failed.render(failedLeft, failedRight);
        countAllocations.store(false);
        require(!failedRender && allocations.load() == 0
                && std::ranges::all_of(failedLeft,
                    [](double value) { return value == 0; })
                && std::ranges::all_of(failedRight,
                    [](double value) { return value == 0; })
                && failed.updateEventValue("Fault", 0,
                    talas::expr::ValueDomain::Scalar)
                && reference.render(referenceLeft, referenceRight),
            "late child failure rolls back parent and descendant state for retry");
        allocations.store(0); countAllocations.store(true);
        const auto recovered = failed.render(failedLeft, failedRight);
        countAllocations.store(false);
        require(recovered && allocations.load() == 0, "nested failure retry must allocate nothing");
        expect(failedLeft, referenceLeft); expect(failedRight, referenceRight);
    }
}

void testNestedScheduledInputPlaybackLifecycle()
{
    for (const auto parentKind : {0, 1, 2}) // Analog, MIDI File, literal Script.
    for (const auto script : {false, true})
    for (const auto mapper : {false, true})
    {
        auto document = nestedAnalogPlaybackInstrument(parentKind != 0);
        auto child = (mapper ? polyphonicMapperInstrument(!script) : instrument(!script)).graph.nodes[1];
        child.instanceId = "inner";
        if (script)
        {
            child.parameters[6].text = "Script";
            child.parameters[8].text =
                "self keyDownAt: 0 ms duration: 12 ms frequency: 60 nn velocity: 0.25. "
                "self keyDownAt: 20 ms duration: 12 ms frequency: 67 nn velocity: 0.5. "
                "self keyDownAt: 40 ms duration: 12 ms frequency: 64 nn velocity: 0.75. "
                "self controller: !cc18 setTo: 0.25 atTime: 8 ms. "
                "self controller: !cc18 setTo: 0.75 atTime: 24 ms. "
                "self controller: !cc18 setTo: 0.5 atTime: 44 ms.";
        }
        document.graph.nodes[1] = std::move(child);
        auto& parent = document.graph.nodes[2];
        if (parentKind == 0)
        {
            for (auto& parameter : parent.parameters)
            {
                if (parameter.parameterId == "durations") parameter.text = "#(0.02 0.02 0.02)";
                if (parameter.parameterId == "dutyCycles") parameter.text = "#(0.4 0.4 0.4)";
            }
        }
        else if (parentKind == 1)
            parent.parameters[7].resource = "parent-notes";
        else
        {
            parent.parameters[6].text = "Script";
            parent.parameters[7].resource = "";
            parent.parameters[8].text =
                "self keyDownAt: 0 ms duration: 28 ms frequency: 60 nn. "
                "self keyDownAt: 20 ms duration: 16 ms frequency: 64 nn. "
                "self keyDownAt: 48 ms duration: 12 ms frequency: 67 nn.";
        }
        document.graph.nodes[0].parameters[0].expression =
            "(!KeyDown > 0) * (!KeyVelocity + !cc18) / "
            "(1 - (!KeyDown > 0) * (!cc18 > 0.7) * !Fault)";
        document.graph.nodes.push_back({"globalController", "talas.valToSound", {{"value", "!cc18"}}});
        document.graph.nodes.push_back({"rightMix", "talas.mixer", {}});
        document.graph.connections.back() = {"outer", "rightOut", "rightMix", "in1"};
        document.graph.connections.push_back({"globalController", "audioOut", "rightMix", "in2"});
        document.graph.connections.push_back({"rightMix", "audioOut", "out", "rightIn"});

        auto configured = settings(); configured.maxBlockFrames = 128;
        configured.eventValues.insert(configured.eventValues.end(), {{"cc18", 0.125}, {"Fault", 0}});
        track(configured, {0,0x90,60,64, 8,0xb0,18,32, 4,0x80,60,0,
            8,0x90,67,96, 4,0xb0,18,96, 8,0x80,67,0,
            8,0x90,64,32, 4,0xb0,18,64, 8,0x80,64,0});
        auto parentSettings = settings();
        track(parentSettings, {0,0x90,60,127, 20,0x90,64,127,
            8,0x80,60,0, 8,0x80,64,0, 12,0x90,67,127, 12,0x80,67,0});
        parentSettings.binaryResources[0].id = "parent-notes";
        configured.binaryResources.push_back(std::move(parentSettings.binaryResources[0]));
        const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
        require(reopened.ok(), "nested scheduled source fixture survives save/reopen");
        const auto graph = compile(reopened.document, configured);
        const auto context = std::string("nested source parent=") + std::to_string(parentKind)
            + " Script=" + std::to_string(script) + " Mapper=" + std::to_string(mapper);
        RealtimeRenderSession admission(graph);
        require(admission.ready(), context + ": " + std::string(admission.error()));

        // Parent Attack opens its Input at sample five. These output origins
        // include ValToSound's one-sample interpolation delay at 1000 Hz.
        // Release preserves Input; parent Retrigger replaces its child lifetime.
        const auto reuseFrame = parentKind == 0 ? 40U : 48U;
        const auto expectedWithOutputDelay = [&](std::size_t delay) {
            std::array<double, 96> audio{};
            const auto addLifetime = [&](std::size_t origin, std::size_t end) {
                for (std::size_t frame = origin; frame < std::min(end, audio.size()); ++frame)
                {
                    const auto age = frame - origin;
                    const auto note = age / 20;
                    if (note >= 3 || age % 20 < 5 || age % 20 >= 12) continue;
                    const auto velocity = script ? std::array{0.25, 0.5, 0.75}[note]
                        : std::array{64.0 / 127, 96.0 / 127, 32.0 / 127}[note];
                    const auto cc = age < 8 ? 0.125
                        : age < 24 ? (script ? 0.25 : 32.0 / 127)
                        : age < 44 ? (script ? 0.75 : 96.0 / 127)
                        : (script ? 0.5 : 64.0 / 127);
                    audio[frame] += velocity + cc;
                }
            };
            addLifetime(5 + delay, reuseFrame); addLifetime(25 + delay, audio.size());
            addLifetime(reuseFrame + 5 + delay, audio.size());
            return audio;
        };
        const auto expected = expectedWithOutputDelay(1);
        std::array<double, 96> expectedRight{};
        std::ranges::transform(expected, expectedRight.begin(), [](double value) { return value * 0.5 + 0.125; });
        const auto offline = OfflineRenderSession(graph).render(expected.size());
        require(offline.ok(), context + ": offline rendering");
        expect(offline.mono, expected); expect(offline.right, expectedRight);
        for (const auto block : {1U, 7U, 32U})
        {
            RealtimeRenderSession session(graph);
            std::array<double, 96> left{}, right{};
            for (std::size_t offset = 0; offset < left.size(); offset += block)
            {
                const auto count = std::min<std::size_t>(block, left.size() - offset);
                allocations = 0; deallocations = 0; countAllocations = true;
                const auto rendered = session.render(std::span(left).subspan(offset, count), std::span(right).subspan(offset, count));
                countAllocations = false;
                require(rendered && allocations == 0 && deallocations == 0, context + ": callback heap or render failure");
            }
            expect(left, expected); expect(right, expectedRight);
            session.resetTransport();
            require(session.render(left, right), context + ": reset source clocks");
            expect(left, expected); expect(right, expectedRight);
        }
        RealtimeRenderSession source(graph), transferred(graph);
        std::array<double, 36> prefixLeft{}, prefixRight{};
        require(source.render(prefixLeft, prefixRight) && transferred.transferPerformanceStateFrom(source),
            context + ": transfer active source clocks and controllers");
        transferred.synchronizeTransportFrame(36);
        std::array<double, 60> tailLeft{}, tailRight{};
        require(transferred.render(tailLeft, tailRight), context + ": render transferred tail");
        expect(tailLeft, std::span(expected).subspan(36)); expect(tailRight, std::span(expectedRight).subspan(36));

        // Performance transfer preserves event state. Use a direct VCA probe
        // so a newly prepared output interpolator cannot hide a clock error.
        auto transferDocument = reopened.document;
        transferDocument.graph.nodes[0].typeId = "talas.vca";
        transferDocument.graph.nodes[0].parameters[0].parameterId = "gain";
        transferDocument.graph.nodes.push_back({"unit", "talas.valToSound", {{"value", "1"}}});
        transferDocument.graph.connections.push_back({"unit", "audioOut", "tone", "audioIn"});
        const auto transferGraph = compile(transferDocument, configured);
        const auto directExpected = expectedWithOutputDelay(0);
        std::array<double, 96> directRight{};
        std::ranges::transform(directExpected, directRight.begin(), [](double value) { return value * 0.5 + 0.125; });
        const auto directOffline = OfflineRenderSession(transferGraph).render(directExpected.size());
        require(directOffline.ok(), context + ": direct transfer oracle renders offline");
        expect(directOffline.mono, directExpected); expect(directOffline.right, directRight);
        // Pending parent/child attacks, a due/consumed CC, overlapping active
        // children and a reused parent's pending Attack all cross publication.
        for (const auto boundary : {4U, 9U, 13U, 14U, 36U, reuseFrame + 1})
        {
            RealtimeRenderSession liveSource(transferGraph), destination(transferGraph);
            std::array<double, 96> sourceLeft{}, sourceRight{}, movedLeft{}, movedRight{};
            require(liveSource.ready() && destination.ready()
                    && liveSource.render(std::span(sourceLeft).first(boundary), std::span(sourceRight).first(boundary))
                    && destination.performanceStateCompatibleWith(liveSource),
                context + ": prepare source state at transfer frame " + std::to_string(boundary));
            allocations = 0; deallocations = 0; countAllocations = true;
            const auto moved = destination.transferPerformanceStateFrom(liveSource);
            destination.synchronizeTransportFrame(boundary);
            countAllocations = false;
            require(moved && allocations == 0 && deallocations == 0,
                context + ": active source transfer must not use the heap");
            for (std::size_t offset = boundary; offset < directExpected.size(); offset += 7)
            {
                const auto count = std::min<std::size_t>(7, directExpected.size() - offset);
                allocations = 0; deallocations = 0; countAllocations = true;
                const auto rendered = destination.render(std::span(movedLeft).subspan(offset, count),
                    std::span(movedRight).subspan(offset, count));
                countAllocations = false;
                require(rendered && allocations == 0 && deallocations == 0,
                    context + ": transferred callback must preserve source state without heap work");
            }
            expect(std::span(movedLeft).subspan(boundary), std::span(directExpected).subspan(boundary));
            expect(std::span(movedRight).subspan(boundary), std::span(directRight).subspan(boundary));
        }

        configured.eventValues.back().value = 1;
        const auto faultGraph = compile(document, configured);
        RealtimeRenderSession failed(faultGraph);
        std::array<double, 96> left{}, right{};
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto rendered = failed.render(left, right);
        countAllocations = false;
        require(!rendered && allocations == 0 && deallocations == 0
                && std::ranges::all_of(left, [](double value) { return value == 0; })
                && std::ranges::all_of(right, [](double value) { return value == 0; })
                && failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar),
            context + ": late child failure rolls back source cursors, controllers and parent state");
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto retried = failed.render(left, right);
        countAllocations = false;
        require(retried && allocations == 0 && deallocations == 0, context + ": corrected retry without heap activity");
        expect(left, expected); expect(right, expectedRight);
    }
}

void testNestedScheduledSiblingControllerScopes()
{
    using namespace talas::project;
    for (const bool script : {false, true})
    for (const auto mode : {0, 1, 2}) // Direct, local Map, explicit Sound value.
    {
        auto document = nestedAnalogPlaybackInstrument(false);
        document.graph.nodes[0].parameters[0].expression = "(!KeyDown > 0) * !cc18";
        auto& parent = document.graph.nodes[2];
        for (auto& parameter : parent.parameters)
        {
            if (parameter.parameterId == "endIndex") parameter.expression = "0";
            if (parameter.parameterId == "polyphony") parameter.expression = "1";
            if (parameter.parameterId == "durations") parameter.text = "#(0.1)";
            if (parameter.parameterId == "dutyCycles") parameter.text = "#(1)";
        }
        auto first = instrument(!script).graph.nodes[1];
        first.instanceId = "inner";
        first.parameters[3].expression = "2";
        auto second = (mode == 1 ? polyphonicMapperInstrument(!script) : instrument(!script)).graph.nodes[1];
        second.instanceId = "second";
        second.parameters[3].expression = "2";
        if (script)
            for (auto* child : {&first, &second})
            {
                child->parameters[6].text = "Script";
                child->parameters[8].text =
                    "self keyDownAt: 0 ms duration: 30 ms frequency: 60 nn. "
                    "self keyDownAt: 20 ms duration: 30 ms frequency: 67 nn. "
                    + std::string(child == &first
                        ? "self controller: !cc18 setTo: 0.25 atTime: 8 ms."
                        : "self controller: !cc18 setTo: 0.75 atTime: 10 ms.");
            }
        else second.parameters[7].resource = "second-notes";
        if (mode == 1)
            std::ranges::find(second.parameters, "map", &ProjectParameterExpression::parameterId)->text =
                "!LocalLevel is: (`MIDIController18 min: 0.25 max: 0.75)";
        document.graph.nodes[1] = std::move(first);
        document.graph.nodes.push_back(std::move(second));
        ProjectSoundDefinition sound;
        sound.definitionId = "siblingInput"; sound.displayName = "Sibling Input";
        sound.graph.nodes = {{"tone", "talas.valToSound", {{"value",
            mode == 1 ? "2 * (!KeyDown > 0) * !LocalLevel" : "2 * (!KeyDown > 0) * !cc18"}}}};
        sound.outputs = {{"audioOut", talas::core::PortType::Audio, "tone", "audioOut"}};
        if (mode == 2)
        {
            ProjectSoundExposedParameter local;
            local.parameterId = "level"; local.displayName = "Level";
            local.defaultValue = {"level", "0.625"};
            local.bindings.push_back({ProjectSoundParameterTarget::EventValue, {}, {}, "cc18", talas::expr::ValueDomain::Scalar});
            sound.exposedParameters.push_back(std::move(local));
        }
        document.soundDefinitions.push_back(std::move(sound));
        ProjectNode input;
        input.instanceId = "secondInput"; input.soundDefinitionId = "siblingInput";
        document.graph.nodes.push_back(std::move(input));
        document.graph.nodes.push_back({"children", "talas.mixer", {}});
        document.graph.nodes.push_back({"globalController", "talas.valToSound", {{"value", "!cc18"}}});
        document.graph.nodes.push_back({"rightMix", "talas.mixer", {}});
        document.graph.connections[1] = {"children", "audioOut", "outer", "audioIn"};
        document.graph.connections.back() = {"outer", "rightOut", "rightMix", "in1"};
        document.graph.connections.insert(document.graph.connections.end(), {
            {"inner", "leftOut", "children", "in1"}, {"second", "leftOut", "children", "in2"},
            {"secondInput", "audioOut", "second", "audioIn"},
            {"globalController", "audioOut", "rightMix", "in2"}, {"rightMix", "audioOut", "out", "rightIn"}});
        auto configured = settings(); configured.maxBlockFrames = 128;
        configured.eventValues.insert(configured.eventValues.end(), {{"cc18", 0.125}, {"LocalLevel", 0}});
        track(configured, {0,0x90,60,127, 8,0xb0,18,32, 12,0x90,67,127,
            10,0x80,60,0, 20,0x80,67,0});
        auto secondSettings = settings();
        track(secondSettings, {0,0x90,60,127, 10,0xb0,18,96, 10,0x90,67,127,
            10,0x80,60,0, 20,0x80,67,0});
        secondSettings.binaryResources[0].id = "second-notes";
        configured.binaryResources.push_back(std::move(secondSettings.binaryResources[0]));
        const auto reopened = parseProjectDocument(serializeProjectDocument(document));
        require(reopened.ok(), "sibling source fixture survives save/reopen");
        const auto graph = compile(reopened.document, configured);
        require(graph.renderSettings().eventValueChanges.empty(), "nested controllers cannot enter the composition timeline");
        std::array<double, 72> expected{}, expectedRight{}, left{}, right{};
        for (std::size_t frame = 6; frame < expected.size(); ++frame)
        {
            const auto age = frame - 6;
            const auto active = (age >= 5 && age < 30 ? 1 : 0) + (age >= 25 && age < 50 ? 1 : 0);
            const auto firstValue = age < 8 ? 0.125 : script ? 0.25 : 32.0 / 127;
            const auto secondValue = age < 10 ? 0.125 : script ? 0.75 : 96.0 / 127;
            const auto mappedValue = mode == 1 ? 0.25 + 0.5 * secondValue : mode == 2 ? 0.625 : secondValue;
            expected[frame] = active * (firstValue + 2 * mappedValue);
        }
        std::ranges::transform(expected, expectedRight.begin(), [](double value) { return value * 0.5 + 0.125; });
        const auto offline = OfflineRenderSession(graph).render(expected.size());
        require(offline.ok(), "sibling controller scopes render offline");
        expect(offline.mono, expected); expect(offline.right, expectedRight);
        RealtimeRenderSession session(graph);
        require(session.ready(), session.error());
        for (std::size_t offset = 0; offset < left.size(); offset += 7)
        {
            const auto count = std::min<std::size_t>(7, left.size() - offset);
            allocations = 0; deallocations = 0; countAllocations = true;
            const auto rendered = session.render(std::span(left).subspan(offset, count), std::span(right).subspan(offset, count));
            countAllocations = false;
            require(rendered && allocations == 0 && deallocations == 0, "sibling source callback cannot use the heap");
        }
        expect(left, expected); expect(right, expectedRight);
        session.resetTransport();
        require(session.render(left, right), "reset restores each sibling controller and projection");
        expect(left, expected); expect(right, expectedRight);
    }
}

talas::project::ProjectDocument nestedProjectedDefaultsInstrument(bool midiParent)
{
    using namespace talas::project;
    auto document = nestedAnalogPlaybackInstrument(midiParent);
    auto& child = document.graph.nodes[1];
    for (auto& parameter : child.parameters)
    {
        if (parameter.parameterId == "endIndex") parameter.expression = "0";
        if (parameter.parameterId == "durations") parameter.text = "#(0.1)";
    }
    ProjectSoundDefinition sound;
    sound.definitionId = "toneSound"; sound.displayName = "Tone Sound";
    auto tone = document.graph.nodes.front();
    tone.parameters.front().expression = "(" + *tone.parameters.front().expression + ") * @gain * !ToneGain";
    sound.graph.nodes.push_back(std::move(tone));
    sound.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
    ProjectSoundExposedParameter local;
    local.parameterId = "gain"; local.displayName = "Local Gain";
    local.defaultValue = {"gain", "!KeyVelocity + 0.5"};
    local.bindings.push_back({ProjectSoundParameterTarget::EventValue, {}, {}, "ToneGain", talas::expr::ValueDomain::Scalar});
    sound.exposedParameters.push_back(std::move(local));
    document.soundDefinitions.push_back(std::move(sound));
    document.graph.nodes.front() = {};
    document.graph.nodes.front().instanceId = "tone";
    document.graph.nodes.front().soundDefinitionId = "toneSound";
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV4;
    ProjectVariableDeclaration gain;
    gain.variableId = "gain"; gain.displayName = "Gain"; gain.initialValue = 0.5;
    gain.snapshotEligible = false;
    gain.owner.kind = ProjectVariableOwnerKind::SoundDefinition;
    gain.owner.definitionId = "toneSound";
    document.variableLibrary.variables.push_back(gain);
    return document;
}

void testNestedProjectedDefaultsAndGenerationTargets()
{
    using namespace talas::project;
    auto document = nestedProjectedDefaultsInstrument(false);
    auto renderSettings = settings();
    renderSettings.eventValues.insert(renderSettings.eventValues.end(),
        {{"Fault", 0}, {"Mark", 99}, {"ToneGain", 9}});
    const auto reopened = parseProjectDocument(serializeProjectDocument(document));
    require(reopened.ok(), "projected-default fixture must persist");
    const auto graph = compile(reopened.document, renderSettings);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 13> prefixLeft{}, prefixRight{};
    require(session.render(prefixLeft, prefixRight) && std::abs(prefixLeft[11] - 0.140625) < 1e-12,
        "projected Sound-local EventValue and variable must shadow global values during audio playback");
    const auto publication = session.preparedVariableValues();
    const auto first = std::ranges::find_if(publication.values, [](const auto& value) {
        return value.identity.voiceAncestors.size() == 2
            && value.identity.voiceAncestors.front().voiceOrdinal == 0;
    });
    require(publication.publicationValid && first != publication.values.end() && first->voiceGeneration != 0,
        "projected default must publish its exact active child generation");
    require(!session.variableTargetIndex(first->identity, first->value.domain),
        "projected default cannot use the generation-free variable target");
    const auto target = session.voiceVariableTargetIndex(first->identity, first->value.domain, first->voiceGeneration);
    require(target.has_value(), "projected default accepts only its current child generation");
    TypedEventPayload payload{}; payload.variable = {0.875, first->value.domain};
    PreparedTypedEvent assignment{TypedEventKind::VariableAssignment,
        {TypedEventTargetKind::Variable, *target, 1, 1, first->voiceGeneration}, 0, 1, payload};
    std::array<RealtimeTypedEventDispatchResult, 1> result{};
    require(session.scheduleTypedEvents(std::span(&assignment, 1), result), "projected assignment queues");
    std::array<double, 7> middleLeft{}, middleRight{};
    require(session.render(middleLeft, middleRight), "projected assignment renders");
    auto changed = session.preparedVariableValues();
    require(std::ranges::find(changed.values, first->identity, &PreparedVariableValue::identity)->value.value == 0.875,
        "generation-specific assignment updates that projected cell");
    std::array<double, 12> reusedLeft{}, reusedRight{};
    allocations.store(0); countAllocations.store(true);
    const auto reusedRender = session.render(reusedLeft, reusedRight);
    countAllocations.store(false);
    require(reusedRender && allocations.load() == 0, "parent reuse restarts projected defaults without allocation");
    changed = session.preparedVariableValues();
    const auto reused = std::ranges::find(changed.values, first->identity, &PreparedVariableValue::identity);
    require(reused != changed.values.end() && reused->value.value == 0.5
            && reused->voiceGeneration > first->voiceGeneration
            && !session.voiceVariableTargetIndex(first->identity, first->value.domain, first->voiceGeneration),
        "parent reuse restores the projected default and invalidates the old child generation");
    require(session.currentGlobalEventValue("ToneGain", talas::expr::ValueDomain::Scalar) == 9,
        "local projected expression must leave its global namesake untouched");
}

void testNestedEventValueReadsParentVoiceVariable()
{
    using namespace talas::project;
    auto document = nestedProjectedDefaultsInstrument(true);
    document.soundDefinitions.front().exposedParameters.front().defaultValue.expression = "!InheritedGain";
    ProjectSoundExposedParameter inherited;
    inherited.parameterId = "inherited"; inherited.displayName = "Inherited Gain";
    inherited.defaultValue = {"inherited", "@parentGain"};
    inherited.bindings.push_back({ProjectSoundParameterTarget::EventValue,
        {}, {}, "InheritedGain", talas::expr::ValueDomain::Scalar});
    document.soundDefinitions.front().exposedParameters.push_back(std::move(inherited));
    ProjectVariableDeclaration gain;
    gain.variableId = "parentGain"; gain.displayName = "Parent Gain";
    gain.initialValue = 0.25; gain.snapshotEligible = false;
    gain.owner.kind = ProjectVariableOwnerKind::MidiVoice;
    gain.owner.nodeId = "outer";
    document.variableLibrary.variables.push_back(gain);
    auto s = settings();
    s.eventValues.insert(s.eventValues.end(), {{"Fault", 0}, {"Mark", 99}, {"ToneGain", 9}});
    track(s, {0,0x90,60,127, 10,0x90,64,127, 10,0x80,60,0, 10,0x80,64,0});
    const auto graph = compile(document, s);
    const auto offline = OfflineRenderSession(graph).render(13);
    require(offline.ok() && std::abs(offline.mono[11] - 0.046875) < 1e-12,
        "offline nested EventValue resolves the enclosing voice variable");
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 13> prefixLeft{}, prefixRight{};
    require(session.render(prefixLeft, prefixRight)
            && std::abs(prefixLeft[11] - 0.046875) < 1e-12,
        "nested Sound EventValue must inherit its parent voice variable without a global fallback");
    const auto snapshot = session.preparedVariableValues();
    const auto parent = std::ranges::find_if(snapshot.values, [](const auto& value) {
        return value.identity.variableId == "parentGain" && value.identity.voiceOrdinal == 0;
    });
    require(parent != snapshot.values.end() && parent->voiceGeneration != 0,
        "first parent voice publishes its exact variable generation");
    const auto target = session.voiceVariableTargetIndex(parent->identity, parent->value.domain, parent->voiceGeneration);
    require(target.has_value(), "parent variable has a generation-bound target");
    TypedEventPayload payload{}; payload.variable = {0.75, parent->value.domain};
    const std::array events{PreparedTypedEvent{TypedEventKind::VariableAssignment,
        {TypedEventTargetKind::Variable, *target, 1, 1, parent->voiceGeneration}, 0, 1, payload}};
    std::array<RealtimeTypedEventDispatchResult, 1> results{};
    require(session.scheduleTypedEvents(events, results), "parent assignment queues for nested expression readers");
    std::array<double, 12> left{}, right{};
    allocations.store(0); countAllocations.store(true);
    const auto rendered = session.render(left, right);
    countAllocations.store(false);
    require(rendered && allocations.load() == 0, "parent variable propagation must not allocate");
    require(std::abs(left[1] - 0.140625) < 1e-12
            && std::abs(left[8] - 0.1875) < 1e-12,
        "each nested projection must follow its own parent while the second parent's default remains independent");
    expect(right, left, 0.5);
    require(session.currentGlobalEventValue("ToneGain", talas::expr::ValueDomain::Scalar) == 9,
        "parent inheritance must preserve the unrelated global EventValue");

    // The same authored Sound can also contain an ordinary, non-voice reader.
    // Its canonical binding must remain and must not silently use the global.
    auto mixed = document;
    mixed.soundDefinitions.front().graph.nodes.push_back(
        {"outside", "talas.valToSound", {{"value", "!ToneGain"}}});
    mixed.soundDefinitions.front().outputs.push_back(
        {"outside", talas::core::PortType::Audio, "outside", "audioOut"});
    mixed.graph.connections.back() = {"tone", "outside", "out", "rightIn"};
    const auto invalid = compileProject(mixed, talas::core::makeBuiltinNodeRegistry(), s);
    require(!invalid.ok() && std::ranges::any_of(invalid.diagnostics, [](const auto& diagnostic) {
            return diagnostic.message.find("variable references cannot resolve") != std::string::npos;
        }), "an ordinary reader cannot borrow a voice-only variable or fall through to its global namesake");
    mixed.soundDefinitions.front().exposedParameters.front().defaultValue.expression = "0.25";
    mixed.soundDefinitions.front().exposedParameters.back().defaultValue.expression = "0.25";
    const auto mixedGraph = compile(mixed, s);
    const auto mixedAudio = OfflineRenderSession(mixedGraph).render(13);
    require(mixedAudio.ok() && std::abs(mixedAudio.mono[11] - 0.046875) < 1e-12
            && std::abs(mixedAudio.right[11] - 0.25) < 1e-12,
        "mixed ordinary and nested readers retain separate canonical and projected bindings");
}


void testNestedParentReuseRejectsStaleChildAssignment()
{
    auto document = nestedProjectedDefaultsInstrument(true);
    for (auto& parameter : document.graph.nodes[2].parameters)
    {
        if (parameter.parameterId == "polyphony") parameter.expression = "1";
        if (parameter.parameterId == "source") parameter.text = "";
        if (parameter.parameterId == "midiFile") parameter.resource = "";
    }
    auto s = settings();
    s.eventValues.insert(s.eventValues.end(), {{"Fault", 0}, {"Mark", 99}, {"ToneGain", 9}});
    const auto graph = compile(document, s);
    RealtimeRenderSession session(graph), reference(graph);
    std::array<double, 13> prefixLeft{}, prefixRight{};
    for (auto* candidate : {&session, &reference})
        require(candidate->ready()
                && candidate->scheduleMidiNote({RealtimeMidiNoteKind::NoteOn, 1, 60, 127}, 0)
                    == RealtimeMidiDispatchResult::Handled
                && candidate->render(prefixLeft, prefixRight),
            "live MIDI parent activates the projected child");
    const auto before = session.preparedVariableValues();
    const auto cell = std::ranges::find_if(before.values, [](const auto& value) {
        return value.identity.isVoiceLocal() && value.voiceActive;
    });
    require(cell != before.values.end(), "live child has an active projected default");
    const auto target = session.voiceVariableTargetIndex(cell->identity, cell->value.domain, cell->voiceGeneration);
    require(target.has_value(), "current child generation resolves before parent reuse");
    TypedEventPayload note{}; note.midiNote = {1, 62, 127};
    TypedEventPayload value{}; value.variable = {0.875, cell->value.domain};
    const std::array events{
        PreparedTypedEvent{TypedEventKind::MidiNoteOn, {TypedEventTargetKind::MidiVoice, 1, 1, 1}, 0, 1, note},
        PreparedTypedEvent{TypedEventKind::VariableAssignment,
            {TypedEventTargetKind::Variable, *target, 1, 1, cell->voiceGeneration}, 0, 2, value}};
    std::array<RealtimeTypedEventDispatchResult, 2> results{};
    require(session.scheduleTypedEvents(events, results), "parent reuse and old child assignment queue together");
    std::array<double, 16> left{}, right{}, expectedLeft{}, expectedRight{};
    allocations.store(0); countAllocations.store(true);
    const auto rendered = session.render(left, right);
    countAllocations.store(false);
    require(!rendered && allocations.load() == 0,
        "a stale child assignment sharing parent reuse must reject the entire callback without allocation");
    const auto after = session.preparedVariableValues();
    const auto retained = std::ranges::find(after.values, cell->identity, &PreparedVariableValue::identity);
    require(retained != after.values.end() && retained->voiceGeneration == cell->voiceGeneration
            && retained->value == cell->value
            && std::ranges::all_of(left, [](double v) { return v == 0; })
            && std::ranges::all_of(right, [](double v) { return v == 0; }),
        "failed parent reuse must restore descendant generation, values and output");
    // Complete callback rollback retains its queued events. An unchanged retry
    // must reject again; transport reset explicitly cancels that invalid batch.
    allocations.store(0); countAllocations.store(true);
    const auto retried = session.render(left, right);
    countAllocations.store(false);
    require(!retried && allocations.load() == 0,
        "an unchanged retry must retain and reject the invalid transaction without allocation");
    const auto retriedValues = session.preparedVariableValues();
    const auto stillRetained = std::ranges::find(retriedValues.values, cell->identity, &PreparedVariableValue::identity);
    require(stillRetained != retriedValues.values.end()
            && stillRetained->voiceGeneration == cell->voiceGeneration
            && stillRetained->value == cell->value,
        "repeated rejection must preserve descendant generation and values");
    for (auto* candidate : {&session, &reference})
    {
        allocations.store(0); countAllocations.store(true);
        candidate->resetTransport();
        countAllocations.store(false);
        require(allocations.load() == 0, "nested reset after rejection must not allocate");
        require(candidate->scheduleMidiNote({RealtimeMidiNoteKind::NoteOn, 1, 62, 127}, 0)
                    == RealtimeMidiDispatchResult::Handled,
            "reset permits a new parent activation");
    }
    require(reference.render(expectedLeft, expectedRight), "reference renders after reset and parent reuse");
    allocations.store(0); countAllocations.store(true);
    const auto recovered = session.render(left, right);
    countAllocations.store(false);
    require(recovered && allocations.load() == 0,
        "reset must cancel the invalid batch and recover nested playback without allocation");
    expect(left, expectedLeft); expect(right, expectedRight);
}

void checkNestedLiveRateAndRange(bool generatedIndex)
{
    for (const auto parentKind : {0, 1, 2}) // Analog, MIDI File, literal Script.
    for (const bool connectedRate : {false, true})
    {
        auto document = nestedAnalogPlaybackInstrument(parentKind != 0);
        const auto setExpression = [](auto& parameters, std::string_view id, std::string value) {
            std::ranges::find(parameters, id, &talas::project::ProjectParameterExpression::parameterId)->expression = std::move(value);
        };
        const auto setText = [](auto& parameters, std::string_view id, std::string value) {
            std::ranges::find(parameters, id, &talas::project::ProjectParameterExpression::parameterId)->text = std::move(value);
        };
        auto& child = document.graph.nodes[1].parameters;
        setExpression(child, "polyphony", "1");
        const std::string rateExpression = "!SeqRate * (1 + (!KeyVelocity > 0.5))";
        setExpression(child, "rate", connectedRate ? "1" : rateExpression);
        setExpression(child, "startIndex", "!RangeStart + (!KeyVelocity > 0.5)");
        setExpression(child, "endIndex", "!RangeEnd + (!KeyVelocity > 0.5)");
        std::ranges::find(child, "loop", &talas::project::ProjectParameterExpression::parameterId)->boolean = true;
        setText(child, "durations", "#(0.032)");
        setText(child, "dutyCycles", "#(0.75)");
        setText(child, "keyPitches", "#(60 62 64 67)");
        setText(child, "keyVelocities", "#(0.125 0.25 0.5 1)");
        setText(child, "extraValues", "#(!Mark 0.0625 0.125 0.25 0.5)");
        if (generatedIndex) child.push_back(text("generatedIndex", "!SeqIndex"));
        auto& parent = document.graph.nodes[2].parameters;
        if (parentKind == 0)
        {
            setText(parent, "durations", "#(0.014 0.026 0.088)");
            setText(parent, "keyVelocities", "#(0.25 0.75 1)");
        }
        else if (parentKind == 2)
        {
            setText(parent, "source", "Script");
            setText(parent, "script", "self keyDownAt: 0 ms duration: 24 ms frequency: 60 nn velocity: 0.25. "
                "self keyDownAt: 14 ms duration: 24 ms frequency: 64 nn velocity: 0.75. "
                "self keyDownAt: 40 ms duration: 24 ms frequency: 67 nn velocity: 1.");
        }
        auto& probe = document.graph.nodes[0];
        probe.typeId = "talas.vca";
        probe.parameters[0].parameterId = "gain";
        probe.parameters[0].expression = std::string("(!KeyDown > 0) * (!KeyVelocity + !Mark")
            + (generatedIndex ? " + !SeqIndex / 16" : "")
            + ") / (1 - (!KeyDown > 0) * (!KeyVelocity > 0.75) * !Fault)";
        document.graph.nodes.push_back({"unit", "talas.valToSound", {{"value", "1"}}});
        document.graph.connections.push_back({"unit", "audioOut", "tone", "audioIn"});
        if (connectedRate)
        {
            document.graph.nodes.push_back({"parentRate", "talas.constant", {{"value", rateExpression}}});
            document.graph.connections.push_back({"parentRate", "value", "inner", "rate"});
        }
        auto configured = settings(); configured.maxBlockFrames = 128;
        configured.eventValues.insert(configured.eventValues.end(), {{"SeqRate", 1}, {"RangeStart", 0},
            {"RangeEnd", 1}, {"Mark", 99}, {"SeqIndex", 99}, {"Fault", 0}});
        configured.eventValueChanges = {{12, "SeqRate", 2}, {25, "RangeStart", 1}, {25, "RangeEnd", 2},
            {44, "SeqRate", -1}, {55, "RangeStart", 0}, {55, "RangeEnd", 1},
            {76, "SeqRate", 0.5}, {87, "RangeStart", 1}, {87, "RangeEnd", 2}};
        track(configured, {0,0x90,60,32, 14,0x90,64,96, 10,0x80,60,0,
            14,0x80,64,0, 2,0x90,67,127, 24,0x80,67,0});
        const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
        require(reopened.ok(), "nested live controls must survive save/reopen");
        const auto graph = compile(reopened.document, configured);
        const auto context = std::string("nested live Rate/range parent=") + std::to_string(parentKind)
            + " connected=" + std::to_string(connectedRate) + " generatedIndex=" + std::to_string(generatedIndex);
        if (generatedIndex && parentKind == 0 && !connectedRate)
        {
            auto invalid = configured;
            const auto index = std::ranges::find(invalid.eventValues, "SeqIndex", &EventValueBinding::name);
            index->projectionExpression = *talas::expr::parse("!SeqRate").expression;
            require(!compileProject(reopened.document, talas::core::makeBuiltinNodeRegistry(), invalid).ok(),
                "nested GeneratedIndex cannot turn a read-only inherited target into a writable shadow");
            index->projectionExpression.reset(); index->domain = talas::expr::ValueDomain::Hertz;
            require(!compileProject(reopened.document, talas::core::makeBuiltinNodeRegistry(), invalid).ok(),
                "nested GeneratedIndex must preserve the inherited target domain check");
        }
        // Integrate |Rate| over each physical interval. A stage consumes 32
        // units and releases at 24; accepted notes attack five samples later.
        // Parent Inputs start at 5/19/45, with slot zero replaced at frame 40.
        // Range/sign changes affect the next accepted stage, never the held one.
        std::array<double, 128> expected{};
        for (const auto [first, end, stage] : {
                 std::tuple{10U, 21U, 0U}, {30U, 37U, 1U},
                 {24U, 25U, 1U}, {32U, 33U, 2U}, {40U, 41U, 3U},
                 {48U, 54U, 2U}, {63U, 70U, 1U}, {79U, 96U, 2U}, {109U, 128U, 3U},
                 {50U, 57U, 3U}, {66U, 73U, 2U}, {83U, 102U, 1U}, {115U, 128U, 2U}})
            for (auto frame = first; frame < end; ++frame)
                expected[frame] += std::array{0.1875, 0.375, 0.75, 1.5}[stage]
                    + (generatedIndex ? stage / 16.0 : 0);
        const auto offline = OfflineRenderSession(graph).render(expected.size());
        require(offline.ok(), context + ": offline render");
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
            require(std::abs(offline.mono[frame] - expected[frame]) < 1e-12,
                context + ": offline frame " + std::to_string(frame) + ", expected="
                    + std::to_string(expected[frame]) + ", actual=" + std::to_string(offline.mono[frame]));
        expect(offline.right, expected, 0.5);
        for (const auto block : {1U, 7U, 64U})
        {
            RealtimeRenderSession session(graph);
            require(session.ready(), session.error());
            for (int pass = 0; pass < 2; ++pass)
            {
                std::array<double, 128> left{}, right{};
                allocations = 0; deallocations = 0; countAllocations = true;
                if (pass) session.resetTransport();
                const auto empty = session.render(std::span(left).first(0), std::span(right).first(0));
                countAllocations = false;
                require(empty && allocations == 0 && deallocations == 0, context + ": reset/empty callback heap work");
                for (std::size_t offset = 0; offset < left.size(); offset += block)
                {
                    const auto count = std::min<std::size_t>(block, left.size() - offset);
                    allocations = 0; deallocations = 0; countAllocations = true;
                    const auto rendered = session.render(std::span(left).subspan(offset, count), std::span(right).subspan(offset, count));
                    countAllocations = false;
                    require(rendered && allocations == 0 && deallocations == 0, context + ": live control callback heap work");
                }
                expect(left, expected); expect(right, expected, 0.5);
                require(session.currentGlobalEventValue("Mark", talas::expr::ValueDomain::Scalar) == 99
                        && session.currentGlobalEventValue("SeqIndex", talas::expr::ValueDomain::Scalar) == 99,
                    context + ": child output controls must not overwrite the enclosing environment");
            }
        }
        for (const auto boundary : {29U, 41U, 80U})
        {
            RealtimeRenderSession source(graph), transferred(graph);
            std::array<double, 128> left{}, right{};
            require(source.render(std::span(left).first(boundary), std::span(right).first(boundary)),
                context + ": reach partial child clocks");
            allocations = 0; deallocations = 0; countAllocations = true;
            const auto moved = transferred.transferPerformanceStateFrom(source);
            transferred.synchronizeTransportFrame(boundary);
            const auto rendered = transferred.render(std::span(left).subspan(boundary), std::span(right).subspan(boundary));
            countAllocations = false;
            require(moved && rendered && allocations == 0 && deallocations == 0,
                context + ": transfer live nested clocks without heap work");
            expect(left, expected); expect(right, expected, 0.5);
        }
        configured.eventValues.back().value = 1;
        RealtimeRenderSession failed(compile(reopened.document, configured));
        std::array<double, 64> left{}, right{};
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto rejected = failed.render(left, right);
        countAllocations = false;
        require(!rejected && allocations == 0 && deallocations == 0
                && std::ranges::all_of(left, [](double value) { return value == 0; })
                && std::ranges::all_of(right, [](double value) { return value == 0; }),
            context + ": late failure must roll back both dynamic clocks and parent reuse");
        require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar), context + ": repair nested voice fault");
        for (std::size_t offset = 0; offset < expected.size(); offset += left.size())
        {
            allocations = 0; deallocations = 0; countAllocations = true;
            const auto rendered = failed.render(left, right);
            countAllocations = false;
            require(rendered && allocations == 0 && deallocations == 0, context + ": retry live nested clocks without heap work");
            expect(left, std::span(expected).subspan(offset, left.size()));
            expect(right, std::span(expected).subspan(offset, right.size()), 0.5);
        }
    }
}

void testGeneratedIndexInCopiedAmplitudeSource()
{
    auto document = nestedAnalogPlaybackInstrument(true);
    auto& sequence = document.graph.nodes[1].parameters;
    for (auto& parameter : sequence)
    {
        if (parameter.parameterId == "keyDowns" || parameter.parameterId == "keyPitches"
            || parameter.parameterId == "keyVelocities" || parameter.parameterId == "keyTimbres"
            || parameter.parameterId == "extraValues") parameter.text = "";
        if (parameter.parameterId == "durations") parameter.text = "#(0.008)";
        if (parameter.parameterId == "dutyCycles") parameter.text = "#(1)";
        if (parameter.parameterId == "gate") parameter.expression = "!KeyDown > 0";
        if (parameter.parameterId == "step") parameter.expression = "1";
        if (parameter.parameterId == "polyphony") parameter.expression = "1";
        if (parameter.parameterId == "endIndex") parameter.expression = "1";
        if (parameter.parameterId == "loop") parameter.boolean = true;
    }
    sequence.push_back(text("velocities", "#(0.25 0.75)"));
    sequence.push_back(text("generatedIndex", "!SeqIndex"));
    document.graph.nodes[0].parameters[0].expression = "1";
    document.graph.nodes.push_back({"indexReader", "talas.vca",
        {{"gain", "(1 + !SeqIndex) / (1 - (!SeqIndex == 1) * !Fault)"}}});
    document.graph.connections[1] = {"inner", "leftOut", "indexReader", "audioIn"};
    document.graph.connections.push_back({"indexReader", "audioOut", "outer", "audioIn"});
    auto configured = settings();
    configured.eventValues.insert(configured.eventValues.end(), {{"SeqIndex", 99}, {"Fault", 0}});
    track(configured, {0,0x90,60,127, 4,0x90,64,127, 40,0x80,60,0, 0,0x80,64,0});
    const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
    require(reopened.ok(), "copied amplitude GeneratedIndex must survive save/reopen");
    const auto graph = compile(reopened.document, configured);
    std::array<double, 32> expected{};
    for (const auto origin : {5U, 9U})
        for (std::size_t frame = origin; frame < expected.size(); ++frame)
        {
            const auto age = frame - origin;
            // Ordinary Input DSP runs before Attack. The explicit KeyDown
            // Gate starts these two clocks at 5/9; the downstream VCA reads
            // the index published at each amplitude stage boundary.
            expected[frame] += age / 8 % 2 ? 1.5 : 0.25;
        }
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), offline.diagnostics.empty() ? "copied amplitude GeneratedIndex renders offline"
        : offline.diagnostics.front().message);
    expect(offline.mono, expected); expect(offline.right, expected, 0.5);
    for (const auto block : {1U, 7U, 32U})
    {
        RealtimeRenderSession session(graph);
        for (int pass = 0; pass < 2; ++pass)
        {
            std::array<double, 32> left{}, right{};
            allocations = 0; deallocations = 0; countAllocations = true;
            if (pass) session.resetTransport();
            countAllocations = false;
            require(allocations == 0 && deallocations == 0, "copied amplitude reset cannot use the heap");
            for (std::size_t offset = 0; offset < expected.size(); offset += block)
            {
                const auto count = std::min<std::size_t>(block, expected.size() - offset);
                allocations = 0; deallocations = 0; countAllocations = true;
                const auto rendered = session.render(std::span(left).subspan(offset, count), std::span(right).subspan(offset, count));
                countAllocations = false;
                require(rendered && allocations == 0 && deallocations == 0, "copied amplitude index callback cannot use the heap");
            }
            expect(left, expected); expect(right, expected, 0.5);
            require(session.currentGlobalEventValue("SeqIndex", talas::expr::ValueDomain::Scalar) == 99,
                "copied amplitude sequencers must keep their enclosing index unchanged");
        }
    }
    configured.eventValues.back().value = 1;
    RealtimeRenderSession failed(compile(reopened.document, configured));
    std::array<double, 32> left{}, right{};
    allocations = 0; deallocations = 0; countAllocations = true;
    const auto rejected = failed.render(left, right);
    countAllocations = false;
    require(!rejected && allocations == 0 && deallocations == 0
            && std::ranges::all_of(left, [](double value) { return value == 0; })
            && failed.currentGlobalEventValue("SeqIndex", talas::expr::ValueDomain::Scalar) == 99,
        "copied amplitude failure must restore clocks, indexes and output without heap work");
    require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar), "repair copied amplitude index reader");
    allocations = 0; deallocations = 0; countAllocations = true;
    const auto rendered = failed.render(left, right);
    countAllocations = false;
    require(rendered && allocations == 0 && deallocations == 0, "copied amplitude retry cannot use the heap");
    expect(left, expected); expect(right, expected, 0.5);
}

void checkNestedProjectedQuadOscillator(bool connectedFrequency)
{
    using namespace talas::project;
    for (const bool scriptParent : {false, true})
    {
        auto document = nestedAnalogPlaybackInstrument(true);
        ProjectSoundDefinition sound;
        sound.definitionId = "quadSound"; sound.displayName = "Quadrature Sound";
        const auto frequencyDomain = connectedFrequency ? talas::expr::ValueDomain::Scalar : talas::expr::ValueDomain::Hertz;
        sound.graph.nodes = {
            {"one", "talas.valToSound", {{"value", "1"}}},
            {"leftEnvelope", "talas.vca", {{"gain", "(!KeyDown > 0) * !LocalGain / (1 - (!cc18 > 0.5) * !Fault)"}}},
            {"rightEnvelope", "talas.vca", {{"gain", "2 * (!KeyDown > 0) * !LocalGain"}}},
            {"quad", "talas.quadOscillator", {{"frequency", connectedFrequency ? "17 hz" : "!LocalFrequency"}}},
        };
        sound.graph.connections = {{"one", "audioOut", "leftEnvelope", "audioIn"},
            {"one", "audioOut", "rightEnvelope", "audioIn"},
            {"leftEnvelope", "audioOut", "quad", "envelope"},
            {"rightEnvelope", "audioOut", "quad", "rightEnvelope"}};
        if (connectedFrequency)
        {
            sound.graph.nodes.push_back({"frequency", "talas.constant", {{"value", "!LocalFrequency"}}});
            sound.graph.connections.push_back({"frequency", "value", "quad", "frequency"});
        }
        sound.outputs = {{"audioOut", talas::core::PortType::Audio, "quad", "audioOut"},
            {"rightOut", talas::core::PortType::Audio, "quad", "rightOut"}};
        for (const auto [name, expression, domain] : {
                 std::tuple{"LocalGain", "!KeyVelocity + 0.25", talas::expr::ValueDomain::Scalar},
                 std::tuple{"LocalFrequency", connectedFrequency
                     ? "125 * !Direction * (1 + !KeyDown)"
                     : "125 hz * !Direction * (1 + !KeyDown)", frequencyDomain}})
        {
            ProjectSoundExposedParameter local;
            local.parameterId = name; local.displayName = name;
            local.domain = domain;
            local.defaultValue = {name, expression};
            local.bindings.push_back({ProjectSoundParameterTarget::EventValue, {}, {}, name, domain});
            sound.exposedParameters.push_back(std::move(local));
        }
        document.soundDefinitions.push_back(std::move(sound));
        document.graph.nodes[0] = {};
        document.graph.nodes[0].instanceId = "tone";
        document.graph.nodes[0].soundDefinitionId = "quadSound";
        auto child = instrument(false).graph.nodes[1];
        child.instanceId = "inner";
        child.parameters[1].expression = "1";
        child.parameters[6].text = "Script";
        child.parameters[8].text =
            "self keyDownAt: 0 ms duration: 28 ms frequency: 60 nn velocity: 0.5. "
            "self controller: !cc18 setTo: 1 atTime: 12 ms.";
        document.graph.nodes[1] = std::move(child);
        document.graph.connections.push_back({"tone", "rightOut", "inner", "rightIn"});
        document.graph.connections.push_back({"inner", "rightOut", "outer", "rightIn"});
        if (scriptParent)
        {
            auto& parent = document.graph.nodes[2].parameters;
            parent[6].text = "Script"; parent[7].resource = "";
            parent[8].text =
                "self keyDownAt: 0 ms duration: 30 ms frequency: 60 nn. "
                "self keyDownAt: 10 ms duration: 50 ms frequency: 64 nn. "
                "self keyDownAt: 40 ms duration: 30 ms frequency: 67 nn.";
        }
        auto configured = settings(); configured.maxBlockFrames = 128;
        configured.eventValues.insert(configured.eventValues.end(),
            {{"LocalGain", 9}, {"LocalFrequency", 9, frequencyDomain},
             {"Direction", 1}, {"cc18", 0}, {"Fault", 0}});
        configured.eventValueChanges = {{24, "Direction", -1}, {60, "Direction", 1}};
        track(configured, {0,0x90,60,127, 10,0x90,64,127, 20,0x80,60,0,
            10,0x90,67,127, 20,0x80,64,0, 10,0x80,67,0});
        const auto reopened = parseProjectDocument(serializeProjectDocument(document));
        require(reopened.ok(), "nested quadrature Sound must survive save/reopen");
        const auto graph = compile(reopened.document, configured);
        const auto context = std::string("nested quadrature Script=") + std::to_string(scriptParent)
            + " connected=" + std::to_string(connectedFrequency);
        std::vector<std::size_t> quadSteps;
        for (std::size_t index = 0; index < graph.plan().steps.size(); ++index)
            if (graph.plan().steps[index].typeId == "talas.quadOscillator") quadSteps.push_back(index);
        require(quadSteps.size() == 2, context + ": two independent quadrature Input copies");
        for (const auto index : quadSteps)
        {
            const auto& step = graph.plan().steps[index];
            require(step.eventValueScopeId != step.scopeId
                    && std::ranges::count_if(graph.plan().midiVoices, [&](const auto& owner) {
                        return owner.ownerNodeId == "inner" && owner.voiceAncestors.size() == 1
                            && owner.voices.size() == 1 && owner.voices.front().scopeId == step.scopeId;
                    }) == 1,
                context + ": projected Sound belongs to the exact nested child voice");
        }
        RealtimeRenderSession admission(graph);
        require(admission.ready(), context + ": " + std::string(admission.error()));
        std::array<double, 96> expected{};
        std::array<double, 2> phase{};
        for (std::size_t frame = 0; frame < expected.size(); ++frame)
            for (std::size_t voice = 0; voice < phase.size(); ++voice)
            {
                const auto active = voice == 0 ? ((frame >= 10 && frame < 33) || (frame >= 50 && frame < 73))
                    : (frame >= 20 && frame < 43);
                const auto pending = voice == 0 ? ((frame >= 5 && frame < 10) || (frame >= 45 && frame < 50))
                    : (frame >= 15 && frame < 20);
                if (active) expected[frame] += 0.75 * std::sin(phase[voice]) + 1.5 * std::cos(phase[voice]);
                // Ordinary Input DSP keeps its phase before Attack, after
                // Release and across parent reuse. Retrigger's KeyDown=-1
                // holds this frequency at zero until the delayed Attack.
                const auto direction = frame >= 24 && frame < 60 ? -1.0 : 1.0;
                phase[voice] += 2 * std::numbers::pi * 125 * direction * (pending ? 0 : active ? 2 : 1) / 1000;
                phase[voice] = std::fmod(phase[voice], 2 * std::numbers::pi);
            }
        const auto offline = OfflineRenderSession(graph).render(expected.size());
        require(offline.ok(), context + ": offline rendering");
        expect(offline.mono, expected); expect(offline.right, expected, 0.5);
        for (const auto block : {1U, 7U, 32U})
        {
            RealtimeRenderSession session(graph);
            for (int pass = 0; pass < 2; ++pass)
            {
                allocations = 0; deallocations = 0; countAllocations = true;
                if (pass) session.resetTransport();
                countAllocations = false;
                require(allocations == 0 && deallocations == 0, context + ": reset cannot use the heap");
                std::array<double, 96> left{}, right{};
                for (std::size_t offset = 0; offset < expected.size(); offset += block)
                {
                    const auto count = std::min<std::size_t>(block, expected.size() - offset);
                    allocations = 0; deallocations = 0; countAllocations = true;
                    const auto rendered = session.render(std::span(left).subspan(offset, count), std::span(right).subspan(offset, count));
                    countAllocations = false;
                    require(rendered && allocations == 0 && deallocations == 0, context + ": callback cannot use the heap");
                }
                expect(left, expected); expect(right, expected, 0.5);
                require(session.currentGlobalEventValue("LocalGain", talas::expr::ValueDomain::Scalar) == 9
                        && session.currentGlobalEventValue("LocalFrequency", frequencyDomain) == 9
                        && session.currentGlobalEventValue("cc18", talas::expr::ValueDomain::Scalar) == 0,
                    context + ": Sound defaults and child controller remain local");
            }
        }
        configured.eventValues.back().value = 1;
        RealtimeRenderSession failed(compile(reopened.document, configured));
        std::array<double, 96> left{}, right{};
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto rejected = failed.render(left, right);
        countAllocations = false;
        require(!rejected && allocations == 0 && deallocations == 0
                && std::ranges::all_of(left, [](double value) { return value == 0; })
                && std::ranges::all_of(right, [](double value) { return value == 0; }),
            context + ": late child failure clears both output ports without heap work");
        require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar), context + ": repair reader");
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto recovered = failed.render(left, right);
        countAllocations = false;
        require(recovered && allocations == 0 && deallocations == 0, context + ": retry restores phase without heap work");
        expect(left, expected); expect(right, expected, 0.5);
        for (const bool parameterScope : {false, true})
        {
            auto forged = graph;
            auto& plan = const_cast<RuntimePlan&>(forged.plan());
            if (parameterScope)
                plan.steps[quadSteps[0]].parameters.front().variableScopeId =
                    plan.steps[quadSteps[1]].parameters.front().variableScopeId;
            else
                plan.steps[quadSteps[0]].eventValueScopeId = plan.steps[quadSteps[1]].eventValueScopeId;
            require(!RealtimeRenderSession(forged).ready() && !validateOfflineRuntimeGraph(forged).ok(),
                context + ": sibling projected EventValue/parameter scope cannot replace compiler authority");
        }
    }
}

void checkNestedProjectedAmplitudeSource(bool generatedIndex)
{
    using namespace talas::project;
    for (const bool scriptParent : {false, true})
    {
        auto document = nestedAnalogPlaybackInstrument(true);
        auto amplitude = document.graph.nodes[1];
        amplitude.instanceId = "amplitude";
        for (auto& parameter : amplitude.parameters)
        {
            if (parameter.parameterId == "keyDowns" || parameter.parameterId == "keyPitches"
                || parameter.parameterId == "keyVelocities" || parameter.parameterId == "keyTimbres"
                || parameter.parameterId == "extraValues") parameter.text = "";
            if (parameter.parameterId == "durations") parameter.text = "#(0.008)";
            if (parameter.parameterId == "dutyCycles") parameter.text = "#(1)";
            if (parameter.parameterId == "gate") parameter.expression = "!KeyDown > 0";
            if (parameter.parameterId == "step") parameter.expression = "1";
            if (parameter.parameterId == "polyphony") parameter.expression = "1";
            if (parameter.parameterId == "endIndex") parameter.expression = "1";
            if (parameter.parameterId == "loop") parameter.boolean = true;
        }
        amplitude.parameters.push_back(text("velocities", "#(0.25 0.75)"));
        if (generatedIndex) amplitude.parameters.push_back(text("generatedIndex", "!SeqIndex"));
        ProjectSoundDefinition sound;
        sound.definitionId = "amplitudeSound"; sound.displayName = "Amplitude Sound";
        sound.graph.nodes = {
            {"one", "talas.valToSound", {{"value", "1"}}},
            std::move(amplitude),
            {"reader", "talas.vca", {{"gain", std::string("!AmpScale * ")
                + (generatedIndex ? "(1 + !SeqIndex)" : "1")
                + " / (1 - (!cc18 > 0.5) * !Fault)"}}},
        };
        sound.graph.connections = {{"one", "audioOut", "amplitude", "audioIn"},
            {"amplitude", "leftOut", "reader", "audioIn"}};
        sound.outputs.push_back({"audioOut", talas::core::PortType::Audio, "reader", "audioOut"});
        ProjectSoundExposedParameter local;
        local.parameterId = "scale"; local.displayName = "Scale";
        local.defaultValue = {"scale", "!KeyVelocity + 0.5"};
        local.bindings.push_back({ProjectSoundParameterTarget::EventValue, {}, {}, "AmpScale", talas::expr::ValueDomain::Scalar});
        sound.exposedParameters.push_back(std::move(local));
        document.soundDefinitions.push_back(std::move(sound));
        document.graph.nodes[0] = {};
        document.graph.nodes[0].instanceId = "tone";
        document.graph.nodes[0].soundDefinitionId = "amplitudeSound";
        auto child = instrument(false).graph.nodes[1];
        child.instanceId = "inner";
        child.parameters[6].text = "Script";
        child.parameters[8].text =
            "self keyDownAt: 0 ms duration: 200 ms frequency: 60 nn velocity: 0.5. "
            "self controller: !cc18 setTo: 1 atTime: 12 ms.";
        document.graph.nodes[1] = std::move(child);
        if (scriptParent)
        {
            auto& parent = document.graph.nodes[2].parameters;
            parent[6].text = "Script"; parent[7].resource = "";
            parent[8].text =
                "self keyDownAt: 0 ms duration: 200 ms frequency: 60 nn. "
                "self keyDownAt: 10 ms duration: 200 ms frequency: 64 nn. "
                "self keyDownAt: 40 ms duration: 200 ms frequency: 67 nn.";
        }
        auto configured = settings(); configured.maxBlockFrames = 128;
        configured.eventValues.insert(configured.eventValues.end(),
            {{"SeqIndex", 99}, {"AmpScale", 9}, {"cc18", 0}, {"Fault", 0}});
        track(configured, {0,0x90,60,127, 10,0x90,64,127, 30,0x90,67,127});
        const auto reopened = parseProjectDocument(serializeProjectDocument(document));
        require(reopened.ok(), "nested projected amplitude fixture must survive save/reopen");
        const auto graph = compile(reopened.document, configured);
        const auto context = std::string("nested projected amplitude Script=") + std::to_string(scriptParent)
            + " GeneratedIndex=" + std::to_string(generatedIndex);
        std::vector<std::size_t> amplitudeSteps;
        for (std::size_t index = 0; index < graph.plan().steps.size(); ++index)
            if (graph.plan().steps[index].typeId == "talas.analogSequencer") amplitudeSteps.push_back(index);
        require(amplitudeSteps.size() == 2
                && std::ranges::count_if(graph.plan().midiVoices, [](const auto& owner) {
                    return owner.ownerNodeId == "inner" && owner.voiceAncestors.size() == 1
                        && owner.voices.size() == 1;
                }) == 2,
            context + ": exactly one amplitude Input per nested child and parent slot");
        for (const auto index : amplitudeSteps)
        {
            const auto& step = graph.plan().steps[index];
            require(step.eventValueScopeId != step.scopeId,
                context + ": reusable Sound EventValues use a distinct projected child scope");
            if (generatedIndex)
                require(std::ranges::count_if(graph.renderSettings().eventValues, [&](const auto& binding) {
                    return binding.name == "SeqIndex" && binding.scopeId == step.eventValueScopeId
                        && binding.value == 99 && binding.domain == talas::expr::ValueDomain::Scalar
                        && !binding.projectionExpression.has_value();
                }) == 1, context + ": each projected index inherits its own mutable default");
        }
        require(graph.plan().steps[amplitudeSteps[0]].eventValueScopeId
                != graph.plan().steps[amplitudeSteps[1]].eventValueScopeId,
            context + ": sibling copies have independent projected cells");
        RealtimeRenderSession admission(graph);
        require(admission.ready(), context + ": " + std::string(admission.error()));

        std::array<double, 96> expected{};
        for (const auto [origin, end] : {std::pair{10U, 40U}, {20U, 96U}, {50U, 96U}})
            for (std::size_t frame = origin; frame < end; ++frame)
            {
                // Each MIDI level delays Attack by five samples. The third
                // parent note reuses slot zero at 40 and opens its child at 45.
                const auto stage = (frame - origin) / 8 % 2;
                expected[frame] += stage ? (generatedIndex ? 1.5 : 0.75) : 0.25;
            }
        const auto offline = OfflineRenderSession(graph).render(expected.size());
        require(offline.ok(), context + ": offline rendering");
        expect(offline.mono, expected); expect(offline.right, expected, 0.5);
        for (const auto block : {1U, 7U, 32U})
        {
            RealtimeRenderSession session(graph);
            for (int pass = 0; pass < 2; ++pass)
            {
                std::array<double, 96> left{}, right{};
                allocations = 0; deallocations = 0; countAllocations = true;
                if (pass) session.resetTransport();
                countAllocations = false;
                require(allocations == 0 && deallocations == 0, context + ": reset cannot use the heap");
                for (std::size_t offset = 0; offset < expected.size(); offset += block)
                {
                    const auto count = std::min<std::size_t>(block, expected.size() - offset);
                    allocations = 0; deallocations = 0; countAllocations = true;
                    const auto rendered = session.render(std::span(left).subspan(offset, count), std::span(right).subspan(offset, count));
                    countAllocations = false;
                    require(rendered && allocations == 0 && deallocations == 0, context + ": callback cannot use the heap");
                }
                expect(left, expected); expect(right, expected, 0.5);
                require(session.currentGlobalEventValue("SeqIndex", talas::expr::ValueDomain::Scalar) == 99
                        && session.currentGlobalEventValue("AmpScale", talas::expr::ValueDomain::Scalar) == 9
                        && session.currentGlobalEventValue("cc18", talas::expr::ValueDomain::Scalar) == 0,
                    context + ": generated indexes, projected defaults and child controllers remain local");
            }
        }
        configured.eventValues.back().value = 1;
        RealtimeRenderSession failed(compile(reopened.document, configured));
        std::array<double, 96> left{}, right{};
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto rejected = failed.render(left, right);
        countAllocations = false;
        require(!rejected && allocations == 0 && deallocations == 0
                && std::ranges::all_of(left, [](double value) { return value == 0; })
                && std::ranges::all_of(right, [](double value) { return value == 0; })
                && failed.currentGlobalEventValue("SeqIndex", talas::expr::ValueDomain::Scalar) == 99,
            context + ": late child failure restores output and local clocks without heap work");
        require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar), context + ": repair reader");
        allocations = 0; deallocations = 0; countAllocations = true;
        const auto recovered = failed.render(left, right);
        countAllocations = false;
        require(recovered && allocations == 0 && deallocations == 0, context + ": retry cannot use the heap");
        expect(left, expected); expect(right, expected, 0.5);

        auto forged = graph;
        auto& plan = const_cast<RuntimePlan&>(forged.plan());
        plan.steps[amplitudeSteps[0]].eventValueScopeId = plan.steps[amplitudeSteps[1]].eventValueScopeId;
        require(!RealtimeRenderSession(forged).ready() && !validateOfflineRuntimeGraph(forged).ok(),
            context + ": a sibling scope cannot replace compiler-owned amplitude authority");
    }
}

void testNestedConnectedGateAndStep()
{
    auto document = nestedAnalogPlaybackInstrument(false);
    for (auto& parameter : document.graph.nodes[2].parameters)
        if (parameter.parameterId == "durations") parameter.text = "#(0.03 0.03 0.03)";
    document.graph.nodes.push_back({"parentGate", "talas.constant", {{"value", "!KeyDown > 0"}}});
    document.graph.nodes.push_back({"parentStep", "talas.constant", {{"value", "!ChildStep"}}});
    document.graph.connections.push_back({"parentGate", "value", "inner", "gate"});
    document.graph.connections.push_back({"parentStep", "value", "inner", "step"});
    auto s = settings();
    s.eventValues.insert(s.eventValues.end(), {{"Fault", 0}, {"Mark", 99}, {"ChildStep", 0}});
    for (const auto frame : {6U, 18U, 36U, 48U})
    {
        s.eventValueChanges.push_back({frame, "ChildStep", 1});
        s.eventValueChanges.push_back({frame + 1, "ChildStep", 0});
    }
    const auto graph = compile(document, s);
    std::array<double, 64> expected{};
    for (const auto [first, value] : {std::pair{12U, 0.375}, {24U, 0.75}, {42U, 0.375}, {54U, 0.75}})
        for (std::size_t frame = first; frame < first + 5; ++frame) expected[frame] = value;
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), "connected nested Gate/Step should render offline");
    expect(offline.mono, expected);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 64> left{}, right{};
    for (std::size_t offset = 0; offset < left.size(); offset += 7)
    {
        const auto count = std::min<std::size_t>(7, left.size() - offset);
        allocations.store(0); countAllocations.store(true);
        const auto rendered = session.render(std::span(left).subspan(offset, count), std::span(right).subspan(offset, count));
        countAllocations.store(false);
        require(rendered && allocations.load() == 0, "connected nested controls must render without allocation");
    }
    expect(left, expected); expect(right, expected, 0.5);
}

void testNestedWaitUntilVoiceInputActivation()
{
    for (const bool midiParent : {false, true})
    {
        auto document = nestedAnalogPlaybackInstrument(midiParent);
        document.graph.nodes.push_back({"voiceReady", "talas.constant", {{"value", "!KeyDown > 0"}}});
        document.graph.nodes.push_back({"held", "talas.waitUntil", {{"resume", "0"}}});
        document.graph.nodes.push_back({"parentRate", "talas.constant", {{"value", "1 + !KeyVelocity * 0"}}});
        document.graph.connections.front() = {"tone", "audioOut", "held", "audioIn"};
        document.graph.connections.push_back({"held", "audioOut", "inner", "audioIn"});
        document.graph.connections.push_back({"voiceReady", "value", "held", "resume"});
        document.graph.connections.push_back({"parentRate", "value", "inner", "rate"});
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"Fault", 0}, {"Mark", 99}});
        if (midiParent)
            track(s, {0,0x90,60,127, 10,0x90,64,127, 10,0x80,60,0,
                10,0x80,64,0, 1,0x90,67,127, 20,0x80,67,0});
        const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
        require(reopened.ok(), "nested WaitUntil instrument must persist");
        const auto graph = compile(reopened.document, s);
        std::array<double, 64> expected{};
        const auto pulse = [&](std::size_t first, std::size_t last, double value) {
            for (auto frame = first; frame <= last; ++frame) expected[frame] += value;
        };
        if (midiParent)
        {
            pulse(11, 15, 0.375); pulse(21, 25, 1.125); pulse(31, 35, 0.75);
            pulse(41, 45, 1.125); pulse(42, 46, 0.375); pulse(52, 56, 0.75); pulse(62, 63, 1.125);
            expected[10] += 0.375; expected[20] += 1.125; expected[30] += 0.75;
        }
        else
        {
            pulse(11, 15, 0.375); pulse(21, 25, 0.375); pulse(31, 35, 1.125);
            pulse(41, 45, 1.875); pulse(51, 55, 1.125);
            expected[10] += 0.375; expected[20] += 0.375; expected[30] += 0.75; expected[40] += 0.75;
        }
        // Each physical input first starts on its own KeyDown, with no prior
        // converter history. Later attacks retain ordinary one-sample interpolation.
        const auto offline = OfflineRenderSession(graph).render(expected.size());
        require(offline.ok(), "WaitUntil in a nested voice Input must render offline");
        expect(offline.mono, expected); expect(offline.right, expected, 0.5);
        if (midiParent)
        {
            auto delayedFile = document;
            delayedFile.graph.nodes.push_back({"heldFile", "talas.waitUntil", {{"resume", "0"}}});
            std::erase_if(delayedFile.graph.connections, [](const auto& input) {
                return input.fromNode == "outer" && input.toNode == "out";
            });
            delayedFile.graph.connections.push_back({"outer", "leftOut", "heldFile", "audioIn"});
            delayedFile.graph.connections.push_back({"heldFile", "audioOut", "out", "audioIn"});
            const auto heldGraph = compile(delayedFile, s);
            const auto heldAudio = OfflineRenderSession(heldGraph).render(expected.size());
            require(heldAudio.ok() && std::ranges::all_of(heldAudio.mono, [](double value) { return value == 0; }),
                "a held MIDI File owner stays silent without starting its child voices");
            auto feedbackHeldFile = delayedFile;
            feedbackHeldFile.graph.nodes.push_back({"readFile", "talas.feedbackLoopOutput",
                {text("connection", "heldFileSource"), {"delay", "1 samples"}}});
            feedbackHeldFile.graph.nodes.push_back({"writeFile", "talas.feedbackLoopInput",
                {text("connection", "heldFileSource")}});
            auto& directFileInput = *std::ranges::find(feedbackHeldFile.graph.connections, "heldFile",
                &talas::core::Connection::toNode);
            directFileInput.fromNode = "readFile"; directFileInput.fromPort = "audioOut";
            feedbackHeldFile.graph.connections.push_back({"outer", "leftOut", "writeFile", "audioIn"});
            const auto feedbackGraph = compile(feedbackHeldFile, s);
            const auto feedbackAudio = OfflineRenderSession(feedbackGraph).render(expected.size());
            require(feedbackAudio.ok(), feedbackAudio.diagnostics.empty() ? "held feedback render failed"
                : feedbackAudio.diagnostics.front().message);
            require(std::ranges::all_of(feedbackAudio.mono, [](double value) { return value == 0; }),
                "a held feedback read must also defer its MIDI File commit source");
            for (const bool feedback : {false, true})
            {
                auto resumed = feedback ? feedbackHeldFile : delayedFile;
                resumed.graph.nodes.push_back({"fileResume", "talas.constant", {{"value", "!FileResume"}}});
                resumed.graph.connections.push_back({"fileResume", "value", "heldFile", "resume"});
                auto resumedSettings = s;
                resumedSettings.eventValues.push_back({"FileResume", 0});
                resumedSettings.eventValueChanges.push_back({8, "FileResume", 1});
                const auto resumedGraph = compile(resumed, resumedSettings);
                const auto resumedAudio = OfflineRenderSession(resumedGraph).render(64);
                require(resumedAudio.ok(), "a held hierarchical File owner must resume its source and child clocks");
                std::array<double, 64> shifted{};
                const auto delay = feedback ? 9U : 8U;
                for (std::size_t frame = delay; frame < shifted.size(); ++frame) shifted[frame] = expected[frame - delay];
                expect(resumedAudio.mono, shifted);
                RealtimeRenderSession resumedSession(resumedGraph);
                std::array<double, 64> resumedLeft{}, resumedRight{};
                require(resumedSession.ready(), resumedSession.error());
                allocations.store(0); countAllocations.store(true);
                const auto rendered = resumedSession.render(resumedLeft, resumedRight);
                countAllocations.store(false);
                require(rendered && allocations.load() == 0, "hierarchical source and feedback activation must not allocate");
                expect(resumedLeft, shifted);
            }
            delayedFile.graph.nodes.back().parameters.front().expression = "1";
            const auto openGraph = compile(delayedFile, s);
            const auto openAudio = OfflineRenderSession(openGraph).render(expected.size());
            require(openAudio.ok(), openAudio.diagnostics.empty()
                ? "an always-open gate does not suspend its MIDI File owner"
                : openAudio.diagnostics.front().message);
            expect(openAudio.mono, expected);
            auto forged = openGraph;
            auto& forgedPlan = const_cast<RuntimePlan&>(forged.plan());
            auto& fileOwner = *std::ranges::find(forgedPlan.steps, "outer", &CompileStep::nodeId);
            fileOwner.resolvedLatencyFrames = 0;
            fileOwner.cumulativeOutputLatencyFrames = 0;
            require(!RealtimeRenderSession(forged).ready(),
                "a forged known latency cannot replace the sealed MIDI mixer contract");
        }
        for (const auto block : {1U, 7U, 64U})
        {
            RealtimeRenderSession session(graph);
            require(session.ready(), session.error());
            for (int pass = 0; pass < 2; ++pass)
            {
                std::array<double, 64> left{}, right{};
                for (std::size_t offset = 0; offset < left.size(); offset += block)
                {
                    const auto count = std::min<std::size_t>(block, left.size() - offset);
                    allocations.store(0); countAllocations.store(true);
                    const auto rendered = session.render(std::span(left).subspan(offset, count),
                        std::span(right).subspan(offset, count));
                    countAllocations.store(false);
                    require(rendered && allocations.load() == 0, "nested WaitUntil callback must not allocate");
                }
                expect(left, expected); expect(right, expected, 0.5);
                allocations.store(0); countAllocations.store(true);
                session.resetTransport();
                countAllocations.store(false);
                require(allocations.load() == 0, "nested WaitUntil reset must not allocate");
            }
        }
    }
}

// A MIDIVoice's Input runs from the MIDIVoice's start, not
// from each note, and a nested MIDIVoice belongs where its own MIDIVoice
// does, so a TimeOffset in a nested voice's Input starts that Input
// SilentTime after the outer MIDIVoice starts and later notes are heard as
// they come (ADR-0023, 2026-10-12). Projects saved before format 89 delay
// each note's samples.
void testTimeOffsetInANestedVoiceCountsFromTheVoicesStart()
{
    for (const bool midiParent : {false, true})
    {
        const auto plain = nestedAnalogPlaybackInstrument(midiParent);
        auto offset = plain;
        offset.graph.nodes.push_back({"offset", "talas.timeOffset", {{"offset", "12 samples"}}});
        offset.graph.connections.front() = {"tone", "audioOut", "offset", "audioIn"};
        offset.graph.connections.push_back({"offset", "audioOut", "inner", "audioIn"});
        auto s = settings();
        s.eventValues.insert(s.eventValues.end(), {{"Fault", 0}, {"Mark", 99}});
        if (midiParent)
            track(s, {0,0x90,60,127, 10,0x90,64,127, 10,0x80,60,0,
                10,0x80,64,0, 1,0x90,67,127, 20,0x80,67,0});
        const auto plainGraph = compile(plain, s);
        const auto heard = OfflineRenderSession(plainGraph).render(64);
        require(heard.ok(), "the nested instrument should render offline");
        const auto graph = compile(offset, s);
        const auto offline = OfflineRenderSession(graph).render(64);
        require(offline.ok(), offline.diagnostics.empty() ? "a TimeOffset in a nested voice should render offline"
            : offline.diagnostics.front().message);
        RealtimeRenderSession realtime(graph);
        require(realtime.ready(), "a TimeOffset in a nested voice should prepare: " + std::string{realtime.error()});
        std::array<double, 64> left{}, right{};
        for (std::size_t start = 0; start < left.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, left.size() - start);
            require(realtime.render(std::span(left).subspan(start, count), std::span(right).subspan(start, count)),
                "a TimeOffset in a nested voice should render in realtime");
        }
        expect(left, offline.mono);
        expect(right, offline.right);
        // The first notes sound from frame 10; the Input starts at frame 12
        // and every later note is heard as it comes.
        require(std::any_of(heard.mono.begin(), heard.mono.begin() + 12, [](double value) { return value != 0.0; }),
            "the nested instrument should sound before frame 12");
        auto expected = heard.mono;
        std::fill(expected.begin(), expected.begin() + 12, 0.0);
        expect(offline.mono, expected);

        // ADR-0019: a TimeStopper the voices play through paces the
        // TimeOffset too, its clock running once Resume opens at frame 20.
        auto stopped = offset;
        std::erase_if(stopped.graph.connections, [](const auto& wire) { return wire.fromNode == "outer"; });
        stopped.graph.nodes.push_back({"stop", "talas.timeStopper", {{"resume", "!Go"}}});
        stopped.graph.connections.push_back({"outer", "leftOut", "stop", "audioIn"});
        stopped.graph.connections.push_back({"stop", "audioOut", "out", "audioIn"});
        auto stopping = s;
        stopping.eventValues.push_back({"Go", 0});
        stopping.eventValueChanges.push_back({20, "Go", 1});
        const auto stoppedGraph = compile(stopped, stopping);
        const auto held = OfflineRenderSession(stoppedGraph).render(64);
        require(held.ok(), held.diagnostics.empty() ? "a stopped nested voice should render offline"
            : held.diagnostics.front().message);
        RealtimeRenderSession heldRealtime(stoppedGraph);
        require(heldRealtime.ready(), "a stopped nested voice should prepare: " + std::string{heldRealtime.error()});
        std::array<double, 64> heldLeft{}, heldRight{};
        for (std::size_t start = 0; start < heldLeft.size(); start += 7)
        {
            const auto count = std::min(std::size_t{7}, heldLeft.size() - start);
            require(heldRealtime.render(std::span(heldLeft).subspan(start, count),
                        std::span(heldRight).subspan(start, count)),
                "a stopped nested voice should render in realtime");
        }
        expect(heldLeft, held.mono);
        const auto startsAt = [&](std::size_t first) {
            auto late = heard.mono;
            std::fill(late.begin(), late.begin() + static_cast<std::ptrdiff_t>(first), 0.0);
            return std::ranges::equal(held.mono, late);
        };
        require(startsAt(32) || startsAt(33),
            "ADR-0019: a TimeOffset in a nested voice should start its Input 12 frames after Resume opens");

        auto json = talas::project::serializeProjectDocument(offset);
        json.replace(json.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\":  88");
        const auto migrated = talas::project::parseProjectDocument(json);
        require(migrated.ok(), "a format 88 nested voice should open");
        const auto legacyGraph = compile(migrated.document, s);
        const auto legacy = OfflineRenderSession(legacyGraph).render(64);
        require(legacy.ok() && legacy.mono != offline.mono,
            "a format 88 TimeOffset in a nested voice should keep delaying each note's samples");
    }
}

void testNestedWaitUntilDefersChildClockAndRollsBack()
{
    auto document = nestedProjectedDefaultsInstrument(false);
    for (auto& parameter : document.graph.nodes[2].parameters)
    {
        if (parameter.parameterId == "endIndex") parameter.expression = "0";
        if (parameter.parameterId == "polyphony") parameter.expression = "1";
        if (parameter.parameterId == "durations") parameter.text = "#(0.1)";
    }
    for (auto& parameter : document.graph.nodes[1].parameters)
        if (parameter.parameterId == "durations") parameter.text = "#(0.01)";
    auto& tone = document.soundDefinitions.front().graph.nodes.front().parameters.front().expression;
    tone = "(" + *tone + ") / (1 - (!KeyDown > 0) * !Fault)";
    document.graph.nodes.push_back({"heldChild", "talas.waitUntil", {{"resume", "0"}}});
    document.graph.nodes.push_back({"resume", "talas.constant", {{"value", "!Resume"}}});
    document.graph.connections[1] = {"inner", "leftOut", "heldChild", "audioIn"};
    document.graph.connections.push_back({"heldChild", "audioOut", "outer", "audioIn"});
    document.graph.connections.push_back({"resume", "value", "heldChild", "resume"});
    auto s = settings();
    s.eventValues.insert(s.eventValues.end(), {{"Fault", 0}, {"Mark", 99}, {"ToneGain", 9}, {"Resume", 0}});
    auto scheduled = s;
    scheduled.eventValueChanges.push_back({20, "Resume", 1});
    const auto scheduledGraph = compile(document, scheduled);
    const auto offline = OfflineRenderSession(scheduledGraph).render(32);
    std::array<double, 32> expected{};
    for (std::size_t frame = 26; frame <= 30; ++frame) expected[frame] = 0.140625;
    require(offline.ok(), "WaitUntil may defer an entire nested Analog scheduler");
    expect(offline.mono, expected); expect(offline.right, expected, 0.5);

    const auto graph = compile(document, s);
    RealtimeRenderSession failed(graph), reference(graph);
    std::array<double, 12> prefixLeft{}, prefixRight{};
    for (auto* session : {&failed, &reference})
    {
        require(session->ready() && session->render(prefixLeft, prefixRight)
                && std::ranges::all_of(prefixLeft, [](double value) { return value == 0; }),
            "held child stays silent while the parent begins its own note");
        const auto snapshot = session->preparedVariableValues();
        require(std::ranges::all_of(snapshot.values, [](const auto& value) {
                return !value.identity.isVoiceLocal() || (!value.voiceActive && value.voiceGeneration == 0);
            }), "a held child has not allocated or started its projected voice cells");
        require(session->scheduleEventValue("Resume", 1, talas::expr::ValueDomain::Scalar, 8),
            "Resume queues at absolute frame 20 after the parent attack");
    }
    require(failed.scheduleEventValue("Fault", 1, talas::expr::ValueDomain::Scalar, 13),
        "a late failure queues after the child input has been activated");
    std::array<double, 20> left{}, right{}, referenceLeft{}, referenceRight{};
    allocations.store(0); countAllocations.store(true);
    const auto rendered = failed.render(left, right);
    countAllocations.store(false);
    require(!rendered && allocations.load() == 0
            && std::ranges::all_of(left, [](double value) { return value == 0; })
            && std::ranges::all_of(right, [](double value) { return value == 0; }),
        "failure after nested activation rolls back the complete callback without allocation");
    const auto retained = failed.preparedVariableValues();
    require(std::ranges::all_of(retained.values, [](const auto& value) {
            return !value.identity.isVoiceLocal() || (!value.voiceActive && value.voiceGeneration == 0);
        }), "failed activation restores the child's unstarted generation and cells");
    require(failed.updateEventValue("Fault", 0, talas::expr::ValueDomain::Scalar)
            && reference.render(referenceLeft, referenceRight),
        "failed nested activation permits a corrected retry");
    allocations.store(0); countAllocations.store(true);
    const auto recovered = failed.render(left, right);
    countAllocations.store(false);
    require(recovered && allocations.load() == 0, "nested activation retry must not allocate");
    expect(left, referenceLeft); expect(right, referenceRight);
    expect(left, std::span(expected).subspan(12));
}

void testThreeLevelAnalogInputs()
{
    auto document = nestedAnalogPlaybackInstrument(false);
    auto middle = document.graph.nodes[1];
    middle.instanceId = "middle";
    for (auto& parameter : middle.parameters)
    {
        if (parameter.parameterId == "polyphony") parameter.expression = "1";
        if (parameter.parameterId == "endIndex") parameter.expression = "0";
        if (parameter.parameterId == "durations") parameter.text = "#(0.1)";
        if (parameter.parameterId == "extraValues") parameter.text = "";
    }
    document.graph.nodes.push_back(std::move(middle));
    document.graph.connections[1] = {"inner", "leftOut", "middle", "audioIn"};
    document.graph.connections.push_back({"middle", "leftOut", "outer", "audioIn"});
    auto s = settings();
    s.eventValues.insert(s.eventValues.end(), {{"Fault", 0}, {"Mark", 99}});
    const auto graph = compile(document, s);
    std::array<double, 64> expected{};
    for (std::size_t frame = 16; frame < 20; ++frame) expected[frame] = 0.375;
    for (const auto [first, value] : {std::pair{26U, 0.375}, {36U, 1.125}, {46U, 1.875}, {56U, 1.125}})
        for (std::size_t frame = first; frame < first + 5; ++frame) expected[frame] = value;
    const auto offline = OfflineRenderSession(graph).render(expected.size());
    require(offline.ok(), "three levels of Analog Inputs should render offline");
    expect(offline.mono, expected);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::array<double, 64> left{}, right{};
    for (std::size_t offset = 0; offset < left.size(); offset += 7)
    {
        const auto count = std::min<std::size_t>(7, left.size() - offset);
        allocations.store(0); countAllocations.store(true);
        const auto rendered = session.render(std::span(left).subspan(offset, count), std::span(right).subspan(offset, count));
        countAllocations.store(false);
        require(rendered && allocations.load() == 0, "three-level callback must preserve bounded state without allocation");
    }
    expect(left, expected); expect(right, expected, 0.5);
}

// A MIDIVoice playing the file "notes" whose Input is a constant 1 through a
// VCA at the level `level`, so that the voice gives that level.
talas::project::ProjectDocument levelInstrument(std::string level)
{
    auto document = instrument(true);
    document.graph.nodes[0] = {"source", "talas.noise", {{"initialState", "1"}, {"frequency", "0 hz"},
        {"centerValue", "0"}, {"scale", "1"}, choice("type", "hotPink")}};
    document.graph.nodes.push_back({"gain", "talas.constant", {{"value", std::move(level)}}});
    document.graph.nodes.push_back({"vca", "talas.vca", {}});
    document.graph.connections[0] = {"source", "audioOut", "vca", "audioIn"};
    document.graph.connections.push_back({"gain", "value", "vca", "gain"});
    document.graph.connections.push_back({"vca", "audioOut", "voice", "audioIn"});
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    return document;
}

// A file's events, one tick a millisecond at the file's tempo: `bytes` at
// the tick `tick`, after the last event at `last`.
void appendEvent(std::vector<std::uint8_t>& events, std::size_t& last, std::size_t tick,
    std::initializer_list<std::uint8_t> bytes)
{
    const auto delta = tick - last;
    if (delta >= 128)
        events.push_back(static_cast<std::uint8_t>(0x80 | (delta >> 7)));
    events.push_back(static_cast<std::uint8_t>(delta & 0x7f));
    events.insert(events.end(), bytes);
    last = tick;
}

// MIDI timing clock messages as a file keeps them, escaped (F7 01 F8):
// `count` of them at 24 a second, as a device at 60 bpm sends them, from
// the millisecond `start`.
void appendClockMessages(std::vector<std::uint8_t>& events, std::size_t& last, std::size_t start, std::size_t count)
{
    for (std::size_t message = 0; message < count; ++message)
        appendEvent(events, last,
            start + static_cast<std::size_t>(std::lround(static_cast<double>(message) * 1000.0 / 24.0)),
            {0xf7, 0x01, 0xf8});
}

// The rising edges of a trigger in each `span` frames.
std::vector<int> risesBy(std::span<const double> level, std::size_t span)
{
    std::vector<int> rises((level.size() + span - 1) / span);
    for (std::size_t frame = 0; frame < level.size(); ++frame)
    {
        require(std::abs(level[frame]) < 1e-12 || std::abs(level[frame] - 1.0) < 1e-12,
            "a timing clock trigger should be 0 or 1, not " + std::to_string(level[frame]));
        rises[frame / span] += level[frame] > 0.5 && (frame == 0 || level[frame - 1] <= 0.5) ? 1 : 0;
    }
    return rises;
}

// The rises of `level` in each `span` frames of a MIDIVoice playing the
// file of these events, the same offline and in realtime; `counted`, the
// timing clock messages the compiler scheduled.
std::vector<int> clockRises(const std::string& level, const std::vector<std::uint8_t>& events,
    std::size_t frames, std::size_t span, std::size_t* counted = nullptr)
{
    auto renderSettings = settings();
    auto& bytes = renderSettings.binaryResources.front().bytes;
    bytes.resize(22);
    const auto length = events.size() + 4;
    for (std::size_t i = 0; i < 4; ++i) bytes[18 + i] = static_cast<std::uint8_t>(length >> (24 - 8 * i));
    bytes.insert(bytes.end(), events.begin(), events.end());
    bytes.insert(bytes.end(), {0, 0xff, 0x2f, 0});
    const auto graph = compile(levelInstrument(level), renderSettings);
    if (counted != nullptr)
        *counted = static_cast<std::size_t>(std::ranges::count_if(graph.renderSettings().eventValueChanges,
            [](const EventValueChange& change) { return change.name == talas::expr::midiTimingClockCountEventName; }));
    const auto offline = OfflineRenderSession(graph).render(frames);
    require(offline.ok(), level + " should render offline");
    const auto rises = risesBy(offline.mono, span);
    RealtimeRenderSession session(graph);
    require(session.ready(), session.error());
    std::vector<double> left(frames), right(frames);
    for (std::size_t start = 0; start < frames; start += 7)
    {
        const auto count = std::min<std::size_t>(7, frames - start);
        allocations.store(0); countAllocations.store(true);
        const auto rendered = session.render(std::span(left).subspan(start, count), std::span(right).subspan(start, count));
        countAllocations.store(false);
        require(rendered && allocations.load() == 0, level + " should render in realtime without allocation");
    }
    require(risesBy(left, span) == rises, level + " should rise in realtime where it rises offline");
    return rises;
}

// `MIDITimingClock is a trigger sequence from an
// external MIDI device or software, once for each MIDI timing message, 24 a
// second from a device at 60 bpm; here a MIDI file's clock, which a Stop
// holds until a Continue.
void testMidiFileTimingClockTriggersOncePerMessage()
{
    // A note starts the voice whose Input reads the clock.
    std::vector<std::uint8_t> events{0, 0x90, 60, 100};
    std::size_t last = 0;
    appendClockMessages(events, last, 0, 24);
    // A Stop: the 12 messages after it are not counted until a Continue.
    appendEvent(events, last, 1000, {0xf7, 0x01, 0xfc});
    appendClockMessages(events, last, 1000, 12);
    appendEvent(events, last, 1500, {0xf7, 0x01, 0xfb});
    appendClockMessages(events, last, 1500, 12);
    std::size_t counted = 0;
    const auto rises = clockRises("`MIDITimingClock", events, 2000, 500, &counted);
    require(counted == 36, "the file's 36 running clock messages should count, not " + std::to_string(counted));
    require(rises[0] + rises[1] == 24, "24 timing clock messages in a second should give 24 triggers, not "
            + std::to_string(rises[0] + rises[1]));
    require(rises[2] == 0 && rises[3] == 12, "a Stop should hold the clock until a Continue: "
            + std::to_string(rises[2]) + ", " + std::to_string(rises[3]));
    // Without clock messages it gives no trigger.
    std::vector<std::uint8_t> notes;
    last = 0;
    appendEvent(notes, last, 0, {0x90, 60, 100});
    appendEvent(notes, last, 20, {0x80, 60, 0});
    const auto silent = clockRises("`MIDITimingClock", notes, 1000, 1000);
    require(silent[0] == 0, "`MIDITimingClock should not trigger without clock messages");
}

// `MIDITimingClock extractClock, the global map's
// !TimingClock, triggers as the MIDI timing clock does, 24 a second: a
// file's clock takes over from the stand-in at the Sound's tempo (48 a
// second at 120 bpm), which a file without a clock
// keeps.
void testMidiFileTimingClockDrivesTimingClock()
{
    std::vector<std::uint8_t> notes;
    std::size_t last = 0;
    appendEvent(notes, last, 0, {0x90, 60, 100});
    appendEvent(notes, last, 100, {0x80, 60, 0});
    auto events = notes;
    // The first message comes a second in.
    appendClockMessages(events, last, 1000, 24);
    const auto clocked = clockRises("!TimingClock", events, 2000, 1000);
    require(clocked[0] == 48, "!TimingClock should stand in at 120 bpm before the clock, 48 a second, not "
            + std::to_string(clocked[0]));
    require(clocked[1] == 24, "!TimingClock should trigger once for each of 24 clock messages in a second, not "
            + std::to_string(clocked[1]));
    const auto extracted = clockRises("`MIDITimingClock extractClock", events, 2000, 1000);
    require(extracted[0] == 0 && extracted[1] == 24,
        "`MIDITimingClock extractClock should trigger 24 times in the clock's second and not before");
    const auto unclocked = clockRises("!TimingClock", notes, 2000, 1000);
    require(unclocked[0] == 48 && unclocked[1] == 48, "a file without a clock should keep !TimingClock's stand-in");
    // A clock past the prepared Event Value transactions (256 by default)
    // is left out, the file playing as before.
    auto crowded = notes;
    std::size_t crowdedLast = 100;
    for (std::size_t message = 0; message < 300; ++message)
        appendEvent(crowded, crowdedLast, 200 + 2 * message, {0xf7, 0x01, 0xf8});
    std::size_t counted = 1;
    const auto kept = clockRises("!TimingClock", crowded, 1000, 1000, &counted);
    require(counted == 0 && kept[0] == 48, "a clock past the prepared transactions should keep the stand-in");
}

}
int main()
{
    bool failed=false;
    for (const auto& [name,test] : std::array{
        std::pair{"file-voice-audio", &testFileVoiceAudio},
        std::pair{"zero-velocity-note-on-release", &testZeroVelocityNoteOnMatchesNoteOffAndRetainsAttackVelocity},
        std::pair{"keyboard-sources-per-voice", &testMidiKeyboardSourcesMapPerVoiceWithDelayedAttack},
        std::pair{"file-default-controller", &testMidiFileDefaultController},
        std::pair{"file-default-map-names", &testMidiFileDefaultMapNames},
        std::pair{"file-project-global-map", &testMidiFileProjectGlobalMap},
        std::pair{"wait-until-midi-source-clock", &testWaitUntilDefersMidiSourceClock},
        std::pair{"file-built-in-controller-binding", &testMidiFileBuiltInControllerBinding},
        std::pair{"file-direct-local-controller-map", &testMidiFileDirectLocalMapController},
        std::pair{"file-program-number-map", &testMidiFileProgramNumberUsesOneBasedLocalMap},
        std::pair{"file-channel-pressure-map", &testMidiFileChannelPressureNormalizesLocalMap},
        std::pair{"file-pitch-bend-map", &testMidiFilePitchBendNormalizesLocalMap},
        std::pair{"file-pitch-bend-composition", &testMidiMapperCombinesVoiceKeyNumberAndSharedBend},
        std::pair{"mapper-file-source-audio", &testPolyphonicMapperFileSourceAudio},
        std::pair{"midi-voice-attenuation-endpoints", &testMidiVoiceAttenuationEndpoints},
        std::pair{"midi-voice-connected-gain-modulation", &testMidiVoiceConnectedGainModulation},
        std::pair{"midi-voice-stereo-input-routing", &testMidiVoiceStereoInputRouting},
        std::pair{"literal-script-continuous-frequency", &testLiteralScriptKeepsContinuousFrequency},
        std::pair{"smalltalk-script-generates-events", &testSmalltalkScriptGeneratesEvents},
        std::pair{"voice-levels-and-pitch-range", &testVoiceLevelsAndPitchRange},
        std::pair{"literal-script-key-timbre", &testLiteralScriptTimbreReachesVoice},
        std::pair{"literal-script-fine-velocity-timbre", &testLiteralScriptRetainsFineVelocityAndTimbre},
        std::pair{"script-octave-pitch-fractional-offset", &testScriptOctavePitchWithFractionalOffset},
        std::pair{"script-accidental-pitch-spelling", &testScriptAccidentalPitchSpelling},
        std::pair{"script-beat-time-without-metronome", &testScriptBeatTimeWithoutMetronome},
        std::pair{"script-metronome-redefinition", &testScriptMetronomeRedefinition},
        std::pair{"script-generated-note-loop", &testScriptGeneratedNoteLoop},
        std::pair{"script-controller-set", &testScriptControllerSet},
        std::pair{"script-controller-slide", &testScriptControllerSlide},
        std::pair{"script-controller-step-size", &testScriptControllerStepSize},
        std::pair{"mapper-literal-script-source", &testPolyphonicMapperLiteralScriptSource},
        std::pair{"mapper-source-selection", &testPolyphonicMapperSourceSelection},
        std::pair{"polyphony-filtering", &testPolyphonyAndFiltering},
        std::pair{"mixed-owners", &testMixedOwners},
        std::pair{"callback-rollback-allocation", &testCallbackRollbackAndAllocation},
        std::pair{"script-callback-rollback-allocation", &testScriptCallbackRollbackAndAllocation},
        std::pair{"source-authority-limits", &testSourceAuthorityAndExplicitLimits},
        std::pair{"transport-forward-repair", &testTransportForwardRepair},
        std::pair{"independent-nested-file-owners", &testIndependentNestedFileOwners},
        std::pair{"performance-transfer-file-position", &testPerformanceTransferKeepsFilePosition},
        std::pair{"offline-reset-sound-selection", &testOfflineResetPreservesSoundSelection},
        std::pair{"analog-key-voice-owner", &testAnalogSequencerKeyArraysBindUpstreamVoice},
        std::pair{"analog-classic-voice-owner", &testAnalogSequencerClassicArraysBindUpstreamVoice},
        std::pair{"analog-key-short-arrays", &testAnalogSequencerShortArraysRepeatFinalValuesAcrossVoices},
        std::pair{"analog-key-index-migration", &testAnalogSequencerZeroBasedRangeAndLegacyMigration},
        std::pair{"analog-key-polyphonic-tail", &testAnalogSequencerPolyphonicReleaseOverlap},
        std::pair{"analog-key-live-gate", &testAnalogSequencerLiveGateRestartsAndReleases},
        std::pair{"analog-key-gate-rollback", &testAnalogSequencerGateCallbackRollback},
        std::pair{"analog-key-gate-publication", &testAnalogSequencerGateLivePublication},
        std::pair{"analog-key-gate-release-tail", &testAnalogSequencerGateKeepsReleaseTail},
        std::pair{"analog-key-step-mask", &testAnalogSequencerStepDurationMask},
        std::pair{"analog-key-connected-controls", &testAnalogSequencerConnectedControls},
        std::pair{"analog-key-connected-control-authority", &testAnalogSequencerConnectedControlAuthority},
        std::pair{"analog-key-connected-audio-triggers", &testAnalogSequencerConnectedAudioTriggersAndRollback},
        std::pair{"analog-key-chained-connected-controls", &testAnalogSequencerChainedConnectedControls},
        std::pair{"analog-key-connected-timed-live-arrays", &testAnalogSequencerConnectedTimedControlsAndLiveArrays},
        std::pair{"analog-key-live-rate-direction", &testAnalogSequencerLiveRateAndDirection},
        std::pair{"analog-key-fixed-live-range", &testAnalogSequencerFixedLiveRangeMatchesStatic},
        std::pair{"analog-key-live-range-traversal", &testAnalogSequencerLiveRangeTraversal},
        std::pair{"analog-key-live-range-exhaustion-rollback", &testAnalogSequencerLiveRangeExhaustionAndRollback},
        std::pair{"analog-key-variable-timed-ranges", &testAnalogSequencerVariableAndTimedRanges},
        std::pair{"analog-key-timed-rate", &testAnalogSequencerTimedRate},
        std::pair{"analog-key-live-rate-restart-validation", &testAnalogSequencerLiveRateRestartsAndRejectsInvalidValues},
        std::pair{"analog-key-fixed-live-rate-rounding", &testAnalogSequencerFixedLiveRatePreservesStaticRounding},
        std::pair{"analog-key-live-rate-array-sampling", &testAnalogSequencerLiveRateSamplesArraysAtAcceptedStages},
        std::pair{"analog-key-live-rate-burst-limit", &testAnalogSequencerLiveRateBurstLimitRollsBack},
        std::pair{"analog-key-live-rate-transport-loop", &testAnalogSequencerLiveRateInsideTransportLoop},
        std::pair{"analog-key-step-reverse-rest", &testAnalogSequencerStepReverseRateAndSilentStages},
        std::pair{"analog-key-step-constant-and-live", &testAnalogSequencerStepConstantClockAndLiveEdges},
        std::pair{"analog-key-step-zero-compatibility", &testAnalogSequencerZeroStepCompatibility},
        std::pair{"analog-key-step-rollback", &testAnalogSequencerStepCallbackRollback},
        std::pair{"analog-key-step-timed-controls", &testAnalogSequencerTimedStepAndGate},
        std::pair{"analog-classic-step-timed-controls", &testAnalogSequencerClassicTimedStepAndGate},
        std::pair{"analog-extra-automatic-isolation", &testAnalogSequencerExtraValuesAutomaticAndIsolated},
        std::pair{"analog-extra-gate-rollback", &testAnalogSequencerExtraValuesGateAndRollback},
        std::pair{"analog-extra-timed-controls", &testAnalogSequencerExtraValuesTimedControls},
        std::pair{"analog-extra-live-expression-sampling", &testAnalogSequencerExtraValuesSampleLiveExpressions},
        std::pair{"analog-live-array-automatic", &testAnalogSequencerLiveArraysAutomaticSampling},
        std::pair{"analog-live-array-validation-rollback", &testAnalogSequencerLiveArraysDeferredValidationAndRollback},
        std::pair{"analog-live-array-step-mask", &testAnalogSequencerLiveArraysStepDurationMask},
        std::pair{"analog-live-array-timed-controls", &testAnalogSequencerLiveArraysTimedControls},
        std::pair{"analog-generated-index-automatic-amplitude", &testAnalogSequencerGeneratedIndexAutomaticAndAmplitude},
        std::pair{"analog-generated-index-readers-transfer", &testAnalogSequencerGeneratedIndexReaderOrdering},
        std::pair{"analog-generated-index-live-rollback", &testAnalogSequencerGeneratedIndexLiveControlsAndRollback},
        std::pair{"analog-generated-index-admission-scope", &testAnalogSequencerGeneratedIndexAdmissionAndScope},
        std::pair{"analog-generated-index-amplitude-rollback-authority", &testAnalogSequencerGeneratedIndexAmplitudeRollbackAndAuthority},
        std::pair{"analog-variable-array-sampling", &testAnalogSequencerVariableArraysSampleAcceptedStages},
        std::pair{"analog-variable-array-transfer-rollback", &testAnalogSequencerVariableArraysTransferAndRollback},
        std::pair{"analog-timed-array-sampling", &testAnalogSequencerTimedArraysSampleContinuousMembers},
        std::pair{"analog-timed-array-gate-transfer", &testAnalogSequencerTimedArraysGateAndTransfer},
        std::pair{"analog-timed-array-rollback", &testAnalogSequencerTimedArraysRollback},
        std::pair{"analog-timed-array-score-clock", &testAnalogSequencerTimedArraysUseScoreSourceClock},
        std::pair{"analog-key-loop-boundary", &testAnalogSequencerLoopReleasesBeforeCycleAttack},
        std::pair{"nested-analog-input-playback", &testNestedAnalogInputPlaybackLifecycle},
        std::pair{"nested-scheduled-input-playback", &testNestedScheduledInputPlaybackLifecycle},
        std::pair{"nested-scheduled-sibling-scopes", &testNestedScheduledSiblingControllerScopes},
        std::pair{"nested-projected-defaults-generation", &testNestedProjectedDefaultsAndGenerationTargets},
        std::pair{"nested-parent-variable-event-value", &testNestedEventValueReadsParentVoiceVariable},
        std::pair{"nested-stale-parent-reuse", &testNestedParentReuseRejectsStaleChildAssignment},
        std::pair{"nested-live-rate-range", +[] { checkNestedLiveRateAndRange(false); }},
        std::pair{"nested-live-generated-index", +[] { checkNestedLiveRateAndRange(true); }},
        std::pair{"copied-amplitude-generated-index", &testGeneratedIndexInCopiedAmplitudeSource},
        std::pair{"nested-projected-quad-oscillator", +[] { checkNestedProjectedQuadOscillator(false); }},
        std::pair{"nested-projected-quad-oscillator-connected", +[] { checkNestedProjectedQuadOscillator(true); }},
        std::pair{"nested-projected-amplitude", +[] { checkNestedProjectedAmplitudeSource(false); }},
        std::pair{"nested-projected-amplitude-generated-index", +[] { checkNestedProjectedAmplitudeSource(true); }},
        std::pair{"nested-connected-gate-step", &testNestedConnectedGateAndStep},
        std::pair{"nested-wait-until-voice-input", &testNestedWaitUntilVoiceInputActivation},
        std::pair{"processors-inside-voices", &testProcessorsPlayInsideVoices},
        std::pair{"time-offset-in-a-voice", &testTimeOffsetInAVoiceCountsFromTheVoicesStart},
        std::pair{"time-offset-in-a-nested-voice", &testTimeOffsetInANestedVoiceCountsFromTheVoicesStart},
        std::pair{"nested-wait-until-child-clock-rollback", &testNestedWaitUntilDefersChildClockAndRollsBack},
        std::pair{"three-level-analog-inputs", &testThreeLevelAnalogInputs},
        std::pair{"file-timing-clock-event-source", &testMidiFileTimingClockTriggersOncePerMessage},
        std::pair{"file-timing-clock-drives-timing-clock", &testMidiFileTimingClockDrivesTimingClock}})
    {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) {
            failed=true; std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failed ? 1 : 0;
}
