#include "talas/app/LiveGridWorkbenchModel.h"
#include "talas/core/BuiltinNodes.h"
#include <stdexcept>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectDocument fixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"noise", "talas.noise", {}});
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back(
        {"noise", "audioOut", "out", "audioIn"});
    talas::project::ProjectLiveSet liveSet;
    liveSet.liveSetId = "main";
    liveSet.displayName = "Main";
    talas::project::ProjectLiveSetCell sound;
    sound.cellId = "sound";
    sound.displayName = "Sound";
    sound.row = 2;
    sound.column = 3;
    sound.target = talas::project::ProjectLiveSetStopTarget{};
    sound.mappings.push_back(talas::project::ProjectLiveSetWorkbenchMapping{});
    sound.mappings.push_back(talas::project::ProjectLiveSetMidiProgramMapping{
        1, 12, "device-a"});
    sound.mappings.push_back(talas::project::ProjectLiveSetOscMapping{
        "/live/sound"});
    liveSet.cells.push_back(std::move(sound));
    document.liveSetLibrary.liveSets.push_back(std::move(liveSet));
    return document;
}

talas::project::ProjectDocument overlappingSetsFixture()
{
    auto document = fixture();
    talas::project::ProjectLiveSet secondary;
    secondary.liveSetId = "secondary";
    secondary.displayName = "Secondary";
    talas::project::ProjectLiveSetCell cell;
    cell.cellId = "same-position";
    cell.displayName = "Same Position";
    cell.row = 2;
    cell.column = 3;
    cell.target = talas::project::ProjectLiveSetStopTarget{};
    cell.mappings.push_back(talas::project::ProjectLiveSetWorkbenchMapping{});
    secondary.cells.push_back(std::move(cell));
    document.liveSetLibrary.liveSets.push_back(std::move(secondary));
    return document;
}

void testMissingPreparationAndMappingTruth()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(fixture(), registry);
    talas::app::LiveGridSourceContext source;
    source.sourceGeneration = 1;
    source.routeGeneration = 1;
    const std::array<std::string, 1> connected{"device-a"};
    const auto action = talas::app::liveGridCellAction(
        bridge,
        "main",
        "sound",
        source,
        {connected, false});
    require(action.state == talas::app::LiveGridCellState::Preparing,
        "an unstaged valid cell should project Preparing");
    require(action.workbenchMapped && action.midiMapped && action.oscMapped,
        "projection should expose every persisted mapping");
    require(action.midiSourceAvailable && !action.oscSourceAvailable,
        "projection should distinguish connected MIDI from unavailable OSC");
    require(action.componentId == "live-grid.cell:main:sound",
        "cell focus identity should be stable and content-derived");
}

void testEverySourceUsesTheSameExactMappingAuthority()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(fixture(), registry);
    talas::app::LiveGridSourceContext midi;
    midi.kind = talas::runtime::RealtimeLiveSetSourceKind::MidiProgram;
    midi.sourceIdentifier = "device-a";
    midi.midiChannel = 1;
    midi.midiProgram = 12;
    midi.sourceGeneration = 2;
    midi.routeGeneration = 3;
    const auto mapped = talas::app::liveGridCellAction(
        bridge, "main", "sound", midi);
    require(mapped.state == talas::app::LiveGridCellState::Preparing,
        "exact MIDI mapping should reach the same preparation authority");
    midi.midiProgram = 13;
    const auto rejected = talas::app::liveGridCellAction(
        bridge, "main", "sound", midi);
    require(rejected.state == talas::app::LiveGridCellState::Unavailable
            && !rejected.canActivate,
        "nonmatching MIDI mapping should fail before runtime dispatch");
}

void testReadyDispatchUsesProjectedAuthority()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(fixture(), registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48'000.0;
    settings.maxBlockFrames = 128;
    require(bridge.compileAndStage(settings).ok(),
        "live-grid fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 128> samples{};
    std::array<float*, 1> outputs{samples.data()};
    (void)bridge.render(outputs.data(), outputs.size(), samples.size());
    const auto stage = bridge.latestStageGeneration();
    const auto route = bridge.editor().documentAuthorityGeneration();
    bridge.publishLiveSetSourceAuthority(
        talas::runtime::RealtimeLiveSetSourceKind::Workbench, stage, route);
    talas::app::LiveGridSourceContext source;
    source.sourceGeneration = stage;
    source.routeGeneration = route;
    const auto projected = talas::app::liveGridCellAction(
        bridge, "main", "sound", source);
    require(projected.state == talas::app::LiveGridCellState::Ready
            && projected.canActivate,
        "exact staged cell should be Ready through shared authority");
    const auto dispatched = talas::app::dispatchLiveGridCell(
        bridge, "main", "sound", source);
    require(dispatched.activation.ok()
            && dispatched.activation.requestGeneration != 0,
        "shared dispatch should enqueue the exact projected cell");

    std::array<float, 128> terminalSamples{};
    std::array<float*, 1> terminalOutputs{terminalSamples.data()};
    static_cast<void>(bridge.render(
        terminalOutputs.data(), terminalOutputs.size(), terminalSamples.size()));
    const auto terminal = bridge.realtimeLiveSetTelemetry();
    require(terminal.terminalRequestGeneration
                >= dispatched.activation.requestGeneration,
        "completed Stop must publish its terminal request identity");
    const auto afterStop = talas::app::liveGridCellAction(
        bridge,
        "main",
        "sound",
        source,
        {{}, false, "main", "sound",
         dispatched.activation.requestGeneration});
    require(afterStop.state != talas::app::LiveGridCellState::Queued,
        "terminal Stop must not remain projected as Queued");
}

void testProjectionSelectsOneLiveSetWithoutCoordinateOverlap()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(overlappingSetsFixture(), registry);
    talas::app::LiveGridSourceContext source;
    source.sourceGeneration = 1;
    source.routeGeneration = 1;
    talas::app::LiveGridProjectionEnvironment environment;
    environment.projectedLiveSetId = "secondary";
    const auto projection = talas::app::liveGridProjection(
        bridge, source, environment);
    require(projection.size() == 1
            && projection.front().liveSetId == "secondary"
            && projection.front().cellId == "same-position"
            && projection.front().row == 2
            && projection.front().column == 3,
        "one selected Live Set must own the complete 8 x 16 projection even when another set uses the same coordinate");
}

void testParallelSoundColumnsAndTrackMuteProjectTogether()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"root", "talas.noise", {}});
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back(
        {"root", "audioOut", "out", "audioIn"});
    for (const auto* id : {"voiceA", "voiceB"})
    {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = id;
        sound.graph.nodes.push_back({"tone", "talas.noise", {}});
        sound.outputs.push_back({
            "audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        document.soundDefinitions.push_back(std::move(sound));
    }
    talas::project::ProjectLiveSet set;
    set.liveSetId = "parallel";
    set.displayName = "Parallel";
    set.cells = {
        {"a", "A", 0, 0, {},
         talas::project::ProjectLiveSetSoundTarget{"voiceA", {}},
         {talas::project::ProjectLiveSetWorkbenchMapping{}}},
        {"b", "B", 0, 1, {},
         talas::project::ProjectLiveSetSoundTarget{"voiceB", {}},
         {talas::project::ProjectLiveSetWorkbenchMapping{}}},
        {"muteA", "Mute A", 1, 0, {},
         talas::project::ProjectLiveSetTrackMuteTarget{true},
         {talas::project::ProjectLiveSetWorkbenchMapping{}}},
    };
    document.liveSetLibrary.liveSets.push_back(std::move(set));
    auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48'000.0;
    settings.maxBlockFrames = 128;
    require(bridge.compileAndStage(settings).ok(),
            "parallel Grid projection fixture should stage");
    bridge.setPlaying(true);
    std::array<float, 128> samples{};
    std::array<float*, 1> outputs{samples.data()};
    const auto render = [&] {
        return bridge.render(outputs.data(), outputs.size(), samples.size());
    };
    require(render(), "parallel Grid projection should adopt its stage");
    const auto stage = bridge.latestStageGeneration();
    const auto route = bridge.editor().documentAuthorityGeneration();
    bridge.publishLiveSetSourceAuthority(
        talas::runtime::RealtimeLiveSetSourceKind::Workbench, stage, route);
    talas::app::LiveGridSourceContext source;
    source.sourceGeneration = stage;
    source.routeGeneration = route;
    const auto earlyMute = talas::app::liveGridCellAction(
        bridge, "parallel", "muteA", source);
    require(earlyMute.state == talas::app::LiveGridCellState::Unavailable
                && !earlyMute.canActivate,
            "track mute should wait for a Sound in its own column");
    require(talas::app::dispatchLiveGridCell(bridge, "parallel", "a", source)
                    .activation.ok()
                && render(),
            "first Grid source should launch");
    require(talas::app::dispatchLiveGridCell(bridge, "parallel", "b", source)
                    .activation.ok()
                && render(),
            "second Grid source should launch");
    const auto first = talas::app::liveGridCellAction(
        bridge, "parallel", "a", source);
    const auto second = talas::app::liveGridCellAction(
        bridge, "parallel", "b", source);
    const auto mute = talas::app::liveGridCellAction(
        bridge, "parallel", "muteA", source);
    require(first.state == talas::app::LiveGridCellState::Active
                && second.state == talas::app::LiveGridCellState::Active
                && mute.targetSummary == "Mute track",
            "Grid should show both Sound cells active and expose its mute control");
    require(talas::app::dispatchLiveGridCell(
                bridge, "parallel", "muteA", source).activation.ok()
                && render(),
            "mapped track mute should execute through Grid dispatch");
    const auto mutedFirst = talas::app::liveGridCellAction(
        bridge, "parallel", "a", source);
    const auto runningSecond = talas::app::liveGridCellAction(
        bridge, "parallel", "b", source);
    require(mutedFirst.state == talas::app::LiveGridCellState::Active
                && mutedFirst.reason == "Active program, muted track"
                && runningSecond.state == talas::app::LiveGridCellState::Active,
            "Grid should retain both active identities while marking one muted");
}
}

int main()
{
    testMissingPreparationAndMappingTruth();
    testEverySourceUsesTheSameExactMappingAuthority();
    testReadyDispatchUsesProjectedAuthority();
    testProjectionSelectsOneLiveSetWithoutCoordinateOverlap();
    testParallelSoundColumnsAndTrackMuteProjectTogether();
}
