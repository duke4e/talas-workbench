// ADR-0030: the transport's built-in Event Values (!Tempo, !SongPosition,
// !BeatPhase, !TransportPlaying) offline, from the project's own transport,
// and from a plugin host's, which can also start, stop and place the Sound.

#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/TransportEventValues.h"
#include "talas/runtime/ProjectRenderSettings.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
using Output = std::array<float, 4>;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

const talas::core::NodeRegistry& registry()
{
    static const auto builtins = talas::core::makeBuiltinNodeRegistry();
    return builtins;
}

talas::project::ProjectParameterExpression expression(std::string id, std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(text);
    return parameter;
}

// A constant 1 through a VCA whose gain is `gain`, so each frame is the
// gain's value, in a project whose musical map runs at `tempoBpm`.
talas::project::ProjectDocument levelDocument(std::string gain, double tempoBpm = 120.0)
{
    talas::project::ProjectParameterExpression type;
    type.parameterId = "type";
    type.choice = "hotPink";
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"source", "talas.noise", {
            expression("initialState", "1"),
            expression("frequency", "0 hz"),
            expression("centerValue", "0"),
            expression("scale", "1"),
            std::move(type),
        }},
        {"gain", "talas.constant", {expression("value", std::move(gain))}},
    };
    document.graph.connections = {
        {"source", "audioOut", "vca", "audioIn"},
        {"gain", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    document.timeline.musicalTime.tempo = {
        {0, static_cast<std::uint32_t>(std::llround(60000000.0 / tempoBpm))}};
    return document;
}

// Four frames a second, so a frame is a quarter of a beat at 60 BPM.
talas::runtime::RenderSettings settings()
{
    talas::runtime::RenderSettings result;
    result.sampleRate = 4.0;
    result.maxBlockFrames = 4;
    return result;
}

Output render(talas::runtime::ProjectRuntimeBridge& bridge)
{
    Output output{};
    std::array<float*, 1> channels{output.data()};
    (void)bridge.render(channels.data(), channels.size(), output.size());
    return output;
}

[[nodiscard]] bool every(const Output& output, double value)
{
    return std::ranges::all_of(output, [&](float sample) { return std::abs(sample - value) < 1.0e-5; });
}

[[nodiscard]] std::string shown(const Output& output)
{
    std::string text;
    for (const auto sample : output)
        text += " " + std::to_string(sample);
    return text;
}

[[nodiscard]] talas::runtime::HostMusicalTransport host(
    double tempoBpm,
    double quarterPosition,
    bool playing,
    bool relocated = false,
    bool drivesSound = false)
{
    return {tempoBpm, quarterPosition, playing, relocated, drivesSound};
}

void staged(talas::runtime::ProjectRuntimeBridge& bridge)
{
    const auto result = bridge.compileAndStage(settings());
    require(result.ok(), "the project stages: " + result.message
        + (result.diagnostics.empty() ? std::string{} : " " + result.diagnostics.front().message));
    bridge.setPlaying(true);
}

[[nodiscard]] const talas::runtime::EventValueBinding* binding(
    const talas::runtime::RenderSettings& prepared,
    std::string_view name)
{
    const auto found = std::ranges::find(prepared.eventValues, name, &talas::runtime::EventValueBinding::name);
    return found == prepared.eventValues.end() ? nullptr : &*found;
}

void testOfflineTheEventValuesGiveTheProjectsTempo()
{
    const auto prepared = talas::runtime::prepareProjectRenderSettings(
        levelDocument("!Tempo + !SongPosition + !BeatPhase + !TransportPlaying", 90.0), registry(), settings());
    require(prepared.ok(), "a project reading the transport prepares: " + prepared.error);
    const auto* tempo = binding(*prepared.settings, "Tempo");
    require(tempo != nullptr && std::abs(tempo->value - 90.0) < 1.0e-3
            && tempo->domain == talas::expr::ValueDomain::Scalar,
        "!Tempo starts at the tempo the project's musical map starts with");
    require(binding(*prepared.settings, "SongPosition")->value == 0.0
            && binding(*prepared.settings, "BeatPhase")->value == 0.0
            && binding(*prepared.settings, "TransportPlaying")->value == 1.0,
        "the song is at its start and the transport plays");

    const auto unread = talas::runtime::prepareProjectRenderSettings(levelDocument("0.5"), registry(), settings());
    require(unread.ok() && binding(*unread.settings, "Tempo") == nullptr,
        "a project that reads none of them binds none");

    // A Virtual Control of the same name is the project's, as an older
    // arpeggiator's Tempo fader is.
    auto declared = levelDocument("!Tempo / 1000", 90.0);
    declared.virtualControls.push_back(talas::project::makeDefaultVirtualControl("Tempo", talas::expr::ValueDomain::Scalar));
    declared.virtualControls.back().value = 0.5;
    const auto own = talas::runtime::prepareProjectRenderSettings(declared, registry(), settings());
    require(own.ok() && binding(*own.settings, "Tempo")->value == 0.5, "the project's own Tempo control wins");

    // No control is discovered for them: they are not faders.
    const auto discovered = talas::project::discoverProjectEventValueControls(
        levelDocument("!Tempo * !Cutoff + !BeatPhase"), registry());
    require(discovered.ok(), "discovery succeeds");
    require(std::ranges::any_of(discovered.missingControls, [](const auto& control) { return control.eventName == "Cutoff"; })
            && std::ranges::none_of(discovered.missingControls, [](const auto& control) {
                   return talas::project::transportEventValueNamed(control.eventName).has_value();
               }),
        "the transport's Event Values get no control, a project's own do");
    require(talas::project::projectReferencesEventValue(levelDocument("!SongPosition"), registry(), "SongPosition"),
        "the project reads !SongPosition");

    // Rendered offline, !Tempo is the project's.
    auto offline = levelDocument("!Tempo / 1000", 75.0);
    auto offlineSettings = talas::runtime::prepareProjectRenderSettings(offline, registry(), settings());
    require(offlineSettings.ok(), "the offline project prepares");
    auto compiled = talas::runtime::compileProject(offline, registry(), *offlineSettings.settings);
    require(compiled.ok(), "the offline project compiles");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 4);
    require(rendered.ok() && !rendered.mono.empty() && std::abs(rendered.mono.front() - 0.075) < 1.0e-6,
        "offline !Tempo is the project's tempo");
}

void testTheProjectsTransportDrivesThemInTheWorkbench()
{
    {
        talas::runtime::ProjectRuntimeBridge bridge(levelDocument("!Tempo / 1000", 90.0), registry());
        staged(bridge);
        const auto output = render(bridge);
        require(every(output, 0.09), "!Tempo is the project's tempo:" + shown(output));
    }
    {
        // 90 BPM, four frames a second: a block of four frames is a beat
        // and a half.
        talas::runtime::ProjectRuntimeBridge bridge(levelDocument("!SongPosition", 90.0), registry());
        staged(bridge);
        const auto first = render(bridge);
        const auto second = render(bridge);
        const auto third = render(bridge);
        require(every(first, 0.0) && every(second, 1.5) && every(third, 3.0),
            "!SongPosition counts the quarter notes the transport has played:" + shown(first) + shown(second)
                + shown(third));
    }
    {
        talas::runtime::ProjectRuntimeBridge bridge(levelDocument("!BeatPhase", 90.0), registry());
        staged(bridge);
        const auto first = render(bridge);
        const auto second = render(bridge);
        const auto third = render(bridge);
        require(every(first, 0.0) && every(second, 0.5) && every(third, 0.0),
            "!BeatPhase is how far through its beat the song is:" + shown(second));
    }
    {
        talas::runtime::ProjectRuntimeBridge bridge(levelDocument("!TransportPlaying * 0.5"), registry());
        staged(bridge);
        require(every(render(bridge), 0.5), "!TransportPlaying is 1 while the transport plays");
    }
    {
        // A tempo change in the musical map: 60 BPM for a beat, a block of
        // four frames, then 120.
        auto document = levelDocument("!Tempo / 1000", 60.0);
        document.timeline.musicalTime.tempo.push_back({960, 500000});
        talas::runtime::ProjectRuntimeBridge bridge(document, registry());
        staged(bridge);
        const auto first = render(bridge);
        const auto second = render(bridge);
        require(every(first, 0.06) && every(second, 0.12),
            "!Tempo follows the map's tempo changes:" + shown(first) + shown(second));
    }
}

void testAHostDrivesThemInAPlugin()
{
    talas::runtime::ProjectRuntimeBridge tempo(levelDocument("!Tempo / 1000", 100.0), registry());
    staged(tempo);
    require(every(render(tempo), 0.1), "without a host the project's tempo plays");
    tempo.followHostTransport(host(140.0, 7.25, true));
    require(every(render(tempo), 0.14), "the host's tempo, at the render it is given for");
    tempo.followHostTransport(host(90.0, 7.5, true));
    require(every(render(tempo), 0.09), "a host tempo change takes effect at its render");
    require(every(render(tempo), 0.1), "a render without the host is the project's again");

    talas::runtime::ProjectRuntimeBridge position(levelDocument("!SongPosition"), registry());
    staged(position);
    position.followHostTransport(host(140.0, 7.25, true));
    require(every(render(position), 7.25), "!SongPosition is the host's");
    position.followHostTransport(host(140.0, -2.0, true));
    require(every(render(position), -2.0), "including a count-in before the song");

    talas::runtime::ProjectRuntimeBridge phase(levelDocument("!BeatPhase"), registry());
    staged(phase);
    phase.followHostTransport(host(140.0, 7.25, true));
    require(every(render(phase), 0.25), "!BeatPhase is the host's");
    phase.followHostTransport(host(140.0, -1.75, true));
    require(every(render(phase), 0.25), "a count-in's phase counts towards the next beat");

    // The Sound runs on when the host stops: an effect keeps processing.
    talas::runtime::ProjectRuntimeBridge playing(levelDocument("0.25 + !TransportPlaying * 0.5"), registry());
    staged(playing);
    playing.followHostTransport(host(120.0, 4.0, true));
    require(every(render(playing), 0.75), "!TransportPlaying is 1 while the host plays");
    playing.followHostTransport(host(120.0, 4.0, false));
    const auto stopped = render(playing);
    require(every(stopped, 0.25), "0 while it is stopped, the Sound running on:" + shown(stopped));

    // A name the project declares is not the transport's.
    auto declared = levelDocument("!Tempo");
    declared.virtualControls.push_back(talas::project::makeDefaultVirtualControl("Tempo", talas::expr::ValueDomain::Scalar));
    declared.virtualControls.back().value = 0.5;
    talas::runtime::ProjectRuntimeBridge own(declared, registry());
    staged(own);
    own.followHostTransport(host(140.0, 0.0, true));
    require(every(render(own), 0.5), "the project's own Tempo control is left to it");
}

void testAHostStartsStopsAndPlacesATimeline()
{
    // 60 BPM at four frames a second: a quarter note is four frames.
    talas::runtime::ProjectRuntimeBridge bridge(levelDocument("1", 60.0), registry());
    staged(bridge);
    bridge.followHostTransport(host(60.0, 0.0, false, true, true));
    require(every(render(bridge), 0.0) && bridge.transportFramePosition() == 0,
        "a stopped host holds the Sound it drives, silent");
    bridge.followHostTransport(host(60.0, 2.0, true, false, true));
    require(every(render(bridge), 1.0) && bridge.transportFramePosition() == 12,
        "Play starts it at the frame of the host's position, quarter 2 being frame 8");
    bridge.followHostTransport(host(60.0, 3.0, true, false, true));
    (void)render(bridge);
    require(bridge.transportFramePosition() == 16, "it plays on with the host");
    bridge.followHostTransport(host(60.0, 10.0, true, true, true));
    (void)render(bridge);
    require(bridge.transportFramePosition() == 44, "a locate or a loop's return moves it to the host's position");
    bridge.followHostTransport(host(60.0, 11.0, false, false, true));
    require(every(render(bridge), 0.0) && bridge.transportFramePosition() == 44, "Stop stops it where it is");
    bridge.followHostTransport(host(60.0, -1.0, true, true, true));
    require(every(render(bridge), 0.0), "a count-in waits at the song's start");
    bridge.followHostTransport(host(60.0, 0.0, true, false, true));
    require(every(render(bridge), 1.0) && bridge.transportFramePosition() == 4, "and the song starts at quarter 0");

    // Staged again with another musical map, as a plugin does at the host's
    // tempo: the next render puts the transport at the host's position in
    // the new map.
    auto edited = bridge.editor().setMusicalTime(
        {960, {{0, 500000}}, {{0, 4, 4}}});
    require(edited.ok(), "the musical map changes");
    require(bridge.compileAndStage(settings()).ok(), "and the project stages again");
    bridge.followHostTransport(host(120.0, 1.0, true, false, true));
    (void)render(bridge);
    require(bridge.transportFramePosition() == 6,
        "quarter 1 at 120 BPM is frame 2 of the new map, then the block's four: "
            + std::to_string(bridge.transportFramePosition()));

    // A render without the host lets the Sound run on its own again.
    bridge.followHostTransport(host(120.0, 3.0, false, false, true));
    require(every(render(bridge), 0.0), "stopped by the host");
    require(every(render(bridge), 1.0), "and running again once the host no longer drives it");
}

// A Score clip of a constant 1 from quarter 4 for four quarters.
talas::project::ProjectDocument scoreDocument(double tempoBpm)
{
    talas::project::ProjectParameterExpression type;
    type.parameterId = "type";
    type.choice = "hotPink";
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition level;
    level.definitionId = "level";
    level.displayName = "Level";
    level.graph.nodes = {{"one", "talas.noise", {
        expression("initialState", "1"),
        expression("frequency", "0 hz"),
        expression("centerValue", "0"),
        expression("scale", "1"),
        std::move(type),
    }}};
    level.outputs = {{"audioOut", talas::core::PortType::Audio, "one", "audioOut"}};
    document.soundDefinitions.push_back(std::move(level));
    const auto ticks = [](std::uint64_t tick) {
        return talas::project::ScoreTime{talas::project::ScoreTimeDomain::MusicalTicks, 0.0, tick};
    };
    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::MusicalTicks,
            "master", 1, talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::MusicalTicks,
            {}, 1, talas::project::ScoreOverlapMode::Layer},
    };
    document.timeline.score.clips = {
        {"clip", "track", "level", ticks(4 * 960), ticks(4 * 960), ticks(0), ticks(0), ticks(0), 1},
    };
    document.timeline.musicalTime.tempo = {
        {0, static_cast<std::uint32_t>(std::llround(60000000.0 / tempoBpm))}};
    return document;
}

// Score clips under way when the host starts: Play after Stop continues
// them, and a relocation prepared while the host is stopped rebuilds them.
void testAHostStartInsideAClipSoundsIt()
{
    // 60 BPM, four frames a second: the clip is frames 16 to 32.
    talas::runtime::ProjectRuntimeBridge bridge(scoreDocument(60.0), registry());
    staged(bridge);
    bridge.followHostTransport(host(60.0, 3.0, true, true, true));
    require(every(render(bridge), 0.0), "the Timeline starts a beat before the clip");
    bridge.followHostTransport(host(60.0, 4.0, true, false, true));
    require(every(render(bridge), 1.0), "and the clip sounds on its beat");
    bridge.followHostTransport(host(60.0, 5.0, false, false, true));
    require(every(render(bridge), 0.0) && bridge.transportFramePosition() == 20, "Stop in the clip holds it");
    bridge.followHostTransport(host(60.0, 5.0, true, false, true));
    const auto resumed = render(bridge);
    require(every(resumed, 1.0), "Play from where it stopped sounds the clip at once:" + shown(resumed));
    bridge.followHostTransport(host(60.0, 6.0, false, false, true));
    (void)render(bridge);

    // A locate while stopped, prepared on the control thread as a plugin's
    // worker prepares it.
    require(bridge.requestTransportRelocation(26), "a relocation into the clip is prepared");
    bridge.followHostTransport(host(60.0, 6.5, false, true, true));
    require(every(render(bridge), 0.0) && bridge.transportFramePosition() == 26,
        "the stopped Timeline moves to the host's position");
    bridge.followHostTransport(host(60.0, 6.5, true, false, true));
    const auto located = render(bridge);
    require(every(located, 1.0), "Play there sounds the clip under way at once:" + shown(located));
}

void testFollowingTheHostAllocatesNothing()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        levelDocument("!Tempo / 1000 + !SongPosition + !BeatPhase + !TransportPlaying", 100.0), registry());
    staged(bridge);
    Output output{};
    std::array<float*, 1> channels{output.data()};
    for (int warmup = 0; warmup < 4; ++warmup)
    {
        bridge.followHostTransport(host(120.0, warmup, true));
        (void)bridge.render(channels.data(), channels.size(), output.size());
    }
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    for (int block = 0; block < 16; ++block)
    {
        bridge.followHostTransport(host(120.0 + block, 4.0 + block * 0.5, block % 3 != 0, block == 7));
        (void)bridge.render(channels.data(), channels.size(), output.size());
    }
    for (int block = 0; block < 4; ++block)
        (void)bridge.render(channels.data(), channels.size(), output.size());
    countAllocations.store(false, std::memory_order_relaxed);
    require(allocationCount.load() == 0,
        "following the host allocates nothing: " + std::to_string(allocationCount.load()) + " allocations");
}
}

int main()
{
    try
    {
        testOfflineTheEventValuesGiveTheProjectsTempo();
        testTheProjectsTransportDrivesThemInTheWorkbench();
        testAHostDrivesThemInAPlugin();
        testAHostStartsStopsAndPlacesATimeline();
        testFollowingTheHostAllocatesNothing();
        testAHostStartInsideAClipSoundsIt();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "talas_transport_event_value_tests passed\n";
    return 0;
}
