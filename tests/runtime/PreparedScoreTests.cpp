#include "../../src/runtime/ScoreTrackPanning.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectLiveSets.h"
#include "talas/runtime/RealtimeAudioEngine.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/Runtime.h"
#include "talas/runtime/ScoreClipArtifactPublication.h"
#include "talas/runtime/ScoreClipMaterialization.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <iostream>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

talas::project::ScoreTime seconds(double value)
{
    return {talas::project::ScoreTimeDomain::Seconds, value, 0};
}

talas::project::ScoreTime ticks(std::uint64_t value)
{
    return {talas::project::ScoreTimeDomain::MusicalTicks, 0.0, value};
}

talas::project::ProjectDocument fixture()
{
    talas::project::ProjectDocument document;
    document.timeline.musicalTime.tempo = {{0, 500000}, {960, 1000000}};
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voiceDefinition";
    definition.displayName = "Voice";
    definition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    talas::project::ProjectNode voiceOscillator;
    voiceOscillator.instanceId = "osc";
    voiceOscillator.typeId = "talas.oscillator";
    voiceOscillator.parameters.push_back({"amplitude", "!Gain"});
    definition.graph.nodes.push_back(std::move(voiceOscillator));
    talas::project::ProjectSoundExposedParameter gain;
    gain.parameterId = "gain";
    gain.displayName = "Gain";
    gain.defaultValue = {"gain", "1.0"};
    gain.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::EventValue,
        {},
        {},
        "Gain",
        talas::expr::ValueDomain::Scalar,
    });
    definition.exposedParameters.push_back(std::move(gain));
    document.soundDefinitions.push_back(std::move(definition));
    document.timeline.score.tracks = {
        {"voice", "Voice", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::MusicalTicks, "music", 2,
         talas::project::ScoreOverlapMode::Layer, false, true},
        {"music", "Music", talas::project::ScoreTrackKind::Submix,
         talas::project::ScoreTimeDomain::MusicalTicks, "master", 2,
         talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds, {}, 2,
         talas::project::ScoreOverlapMode::Layer},
    };
    document.timeline.score.clips = {
        {"clipA", "voice", "voiceDefinition", ticks(480), ticks(960),
         ticks(120), ticks(120), ticks(240), 2},
    };
    document.timeline.score.automation = {
        {"clipGain", talas::project::ScoreAutomationScope::Clip, "clipA",
         "Gain", talas::expr::ValueDomain::Scalar,
         {{ticks(0), 0.0, talas::core::AutomationInterpolation::Linear},
          {ticks(960), 1.0, talas::core::AutomationInterpolation::Step}}},
        {"masterGain", talas::project::ScoreAutomationScope::Master, "master",
         "Gain", talas::expr::ValueDomain::Scalar,
         {{seconds(0.75), 0.5, talas::core::AutomationInterpolation::Step}}},
    };
    document.timeline.score.markers = {
        {"marker", "Tempo boundary", ticks(960), ticks(1920)},
    };
    return document;
}

talas::core::NodeRegistry registry()
{
    return talas::core::makeBuiltinNodeRegistry();
}

talas::runtime::PreparedScoreBinding binding()
{
    return {7, 11, 48000, 2, 3, 4, 5, 6};
}

void testPreparationResolvesExactFramesAndBindings()
{
    const auto prepared = talas::runtime::prepareProjectScore(fixture(), binding());
    require(prepared.ok(), prepared.error);
    require(prepared.schedule->binding() == binding(),
            "prepared Score should retain the complete exact generation binding");
    require(prepared.schedule->tracks().size() == 3
                && prepared.schedule->tracks()[0].audible
                && prepared.schedule->tracks()[1].audible
                && prepared.schedule->tracks()[2].audible,
            "solo resolution should retain the selected track and route ancestors");
    require(prepared.schedule->tracks()[0].outputTrackIndex == 1
                && prepared.schedule->tracks()[1].outputTrackIndex == 2,
            "prepared routes should use exact immutable track indices");

    const auto& clip = prepared.schedule->clips().front();
    require(clip.startFrame == 12000,
            "clip start should use the first tempo segment");
    require(clip.durationFrames == 36000,
            "clip duration should integrate across the tempo change at its start");
    require(clip.endFrame == 96000 && clip.repeatCount == 2,
            "clip repeats should produce one checked exclusive end frame");
    require(clip.contentOffsetFrames == 3000
                && clip.fadeInFrames == 3000
                && clip.fadeOutFrames == 12000,
            "content offset and fades should use their exact start/end tempo segments");

    require(prepared.schedule->automationLanes().size() == 2
                && prepared.schedule->automationPoints().size() == 3,
            "scoped automation should compile into bounded immutable arrays");
    require(prepared.schedule->automationPoints()[0].frame == 12000
                && prepared.schedule->automationPoints()[1].frame == 48000
                && prepared.schedule->automationPoints()[2].frame == 36000,
            "clip-local and master automation should retain their exact time origins");
    const auto iterations = prepared.schedule->clipIterations();
    require(iterations.size() == 2
                && iterations[0]
                    == talas::runtime::PreparedScoreClipIteration{0, 12000, 48000}
                && iterations[1]
                    == talas::runtime::PreparedScoreClipIteration{0, 48000, 96000},
            "repeat boundaries should each integrate the active tempo segment");
    require(prepared.schedule->markers().front().startFrame == 24000
                && prepared.schedule->markers().front().endFrame
                    == std::optional<std::uint64_t>{72000},
            "marker ranges should integrate the canonical tempo map");
}

void testScheduleOrderingAndSeekArePartitionIndependent()
{
    const auto prepared = talas::runtime::prepareProjectScore(fixture(), binding());
    require(prepared.ok(), prepared.error);
    const auto events = prepared.schedule->events();
    require(events.size() == 6,
            "one repeated clip and three automation points should author six prepared events");
    for (std::size_t index = 1; index < events.size(); ++index)
        require(events[index - 1].frame <= events[index].frame,
                "prepared events should be globally frame ordered");
    require(prepared.schedule->firstEventAtOrAfter(0) == 0
                && prepared.schedule->firstEventAtOrAfter(12001) == 2
                && prepared.schedule->firstEventAtOrAfter(96001) == events.size(),
            "binary seek should locate the same event independent of callback partitioning");

    talas::runtime::PreparedScorePlaybackCursor partitioned(prepared.schedule);
    require(partitioned.ready() && partitioned.relocate(0)
                && !partitioned.clipActive(0),
            "playback cursor should begin before the clip");
    require(partitioned.advanceTo(12000) && partitioned.clipActive(0),
            "clip should activate on its exact first frame");
    require(partitioned.advanceTo(30000)
                && partitioned.automationValue(0, 30000).has_value()
                && std::abs(*partitioned.automationValue(0, 30000) - 0.5)
                    < 1.0e-12,
            "linear automation should be partition independent between exact points");
    require(partitioned.advanceTo(96000) && !partitioned.clipActive(0),
            "clip should deactivate on its exclusive end frame");

    talas::runtime::PreparedScorePlaybackCursor direct(prepared.schedule);
    require(direct.relocate(30000) && direct.clipActive(0)
                && direct.eventCursor()
                    == prepared.schedule->firstEventAtOrAfter(30001),
            "direct relocation should reconstruct the same immutable state");
}

void testReplaceOverlapPublishesExactReactivationBoundaries()
{
    auto document = fixture();
    document.timeline.score.tracks.front().overlapMode =
        talas::project::ScoreOverlapMode::Replace;
    document.timeline.score.clips.push_back({
        "clipB", "voice", "voiceDefinition", ticks(960), ticks(960),
        ticks(0), ticks(0), ticks(0), 1});
    const auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 7, 11, 48000, 6));
    require(prepared.ok(), prepared.error);
    talas::runtime::PreparedScorePlaybackCursor cursor(prepared.schedule);
    require(cursor.relocate(20000) && cursor.clipActive(0)
                && !cursor.clipActive(1),
            "earlier Replace clip should be active before the later clip");
    require(cursor.advanceTo(24000) && !cursor.clipActive(0)
                && cursor.clipActive(1),
            "later document-order clip should replace at its exact start");
    require(cursor.advanceTo(72000) && cursor.clipActive(0)
                && !cursor.clipActive(1),
            "underlying clip should reactivate at the replacement end");
    require(cursor.advanceTo(96000) && !cursor.clipActive(0),
            "underlying clip should retain its original exclusive end");
}

void testPreparationRejectsStaleOrUnboundAuthority()
{
    auto invalidBinding = binding();
    invalidBinding.stageGeneration = 0;
    require(!talas::runtime::prepareProjectScore(fixture(), invalidBinding).ok(),
            "zero stage authority should reject before schedule publication");
    auto document = fixture();
    document.timeline.score.clips.front().soundDefinitionId = "missing";
    require(!talas::runtime::prepareProjectScore(document, binding()).ok(),
            "unknown Sound identity should reject without a partial schedule");
}

void testPreparedScheduleIsRetainedByOfflineAndRealtimeEngines()
{
    auto document = fixture();
    const auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 7, 11, 48000, 6));
    require(prepared.ok(), prepared.error);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 64;
    settings.preparedScore = prepared.schedule;
    const auto compiled = talas::runtime::compileProject(
        document, registry(), std::move(settings));
    auto compileDetail = std::string{
        "graph carrying an immutable Score should compile"};
    for (const auto& diagnostic : compiled.diagnostics)
        compileDetail += " | " + diagnostic.message;
    require(compiled.ok(), compileDetail);
    require(compiled.graph->renderSettings().preparedScore == prepared.schedule,
            "offline graph should retain the exact immutable Score object");
    talas::runtime::RealtimeAudioEngine engine(*compiled.graph);
    require(engine.ready(), engine.error());
    require(engine.preparedScoreSchedule() == prepared.schedule,
            "realtime engine should retain the same immutable Score at construction");

    const auto requireStaleScheduleRejected = [&prepared](
        talas::project::ProjectDocument staleDocument,
        std::string_view message) {
        talas::runtime::RenderSettings staleSettings;
        staleSettings.sampleRate = 48000.0;
        staleSettings.maxBlockFrames = 64;
        staleSettings.preparedScore = prepared.schedule;
        require(!talas::runtime::compileProject(
                     staleDocument, registry(), std::move(staleSettings)).ok(),
                message);
    };

    auto staleRouting = document;
    staleRouting.timeline.score.clips.front().fadeIn.tick += 1;
    requireStaleScheduleRejected(
        std::move(staleRouting),
        "clip timing changes must reject a stale prepared Score generation");

    auto staleAutomation = document;
    staleAutomation.timeline.score.automation.front().points.front().value = 0.25;
    requireStaleScheduleRejected(
        std::move(staleAutomation),
        "automation changes must reject a stale prepared Score generation");

    auto staleMarker = document;
    staleMarker.timeline.score.markers.front().title = "Changed marker";
    requireStaleScheduleRejected(
        std::move(staleMarker),
        "marker changes must reject a stale prepared Score generation");

    auto staleDefinition = document;
    staleDefinition.soundDefinitions.front().graph.nodes.front()
        .parameters.front().expression = "0.25";
    requireStaleScheduleRejected(
        std::move(staleDefinition),
        "Sound graph changes must reject a stale prepared Score generation");
}

void testBridgePublishesScoreFailureAtomicallyWithItsGraphStage()
{
    auto document = fixture();
    document.timeline.score.automation.clear();
    const auto nodeRegistry = registry();
    talas::runtime::ProjectRuntimeBridge bridge(document, nodeRegistry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 64;
    const auto firstStage = bridge.compileAndStage(settings);
    auto stageDetail = firstStage.message;
    for (const auto& diagnostic : firstStage.diagnostics)
        stageDetail += " | " + diagnostic.message;
    require(firstStage.ok(),
            "Score-bearing project should stage through the bridge: "
                + stageDetail);
    const auto first = bridge.latestPreparedScoreSchedule();
    require(first != nullptr && first->binding().stageGeneration == 1,
            "accepted graph should publish its exact first Score generation");

    bridge.setPlaying(true);
    float output[16]{};
    float* channels[]{output};
    require(bridge.render(channels, 1, 16),
            "audio boundary should adopt a graph retaining the Score schedule");

    auto replacement = document;
    replacement.timeline.musicalTime.tempo[1].microsecondsPerQuarter = 750000;
    require(bridge.compileAndStageDocument(replacement, settings).ok(),
            "tempo replacement should stage a new immutable Score");
    const auto second = bridge.latestPreparedScoreSchedule();
    require(second != nullptr && second != first
                && second->binding().stageGeneration == 2
                && second->binding().tempoGeneration
                    != first->binding().tempoGeneration,
            "tempo replacement should publish a distinct exact generation binding");

    auto invalid = replacement;
    invalid.timeline.score.clips.front().soundDefinitionId = "missing";
    require(!bridge.compileAndStageDocument(invalid, settings).ok(),
            "invalid replacement should reject before graph publication");
    require(bridge.latestPreparedScoreSchedule() == second,
            "failed Score preparation should retain the prior usable schedule");

    bridge.clearAfterAudioStopped();
    require(
        bridge.activeStageId() == 0
            && bridge.latestPreparedScoreSchedule() == nullptr,
        "device stop should revoke the graph and its stale prepared Score publication together");
}

void testTrackAndMasterAutomationReachExactLiveClipSource()
{
    auto document = fixture();
    document.timeline.score.automation.push_back({
        "submixGain",
        talas::project::ScoreAutomationScope::Track,
        "music",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        {{ticks(1920), 0.75, talas::core::AutomationInterpolation::Step}},
    });
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 64;
    const auto compiled = talas::runtime::compileProject(
        document, registry(), std::move(settings));
    auto detail = std::string{
        "routed Score automation should compile against exact Sound scopes"};
    for (const auto& diagnostic : compiled.diagnostics)
        detail += " | " + diagnostic.message;
    require(compiled.ok(), detail);

    const auto& renderedSettings = compiled.graph->renderSettings();
    require(renderedSettings.preparedScoreClipAudio.empty(),
            "clip, routed-submix, and master automation must reach live source programs without incidental PCM");
    require(std::ranges::none_of(
                renderedSettings.eventValueChanges,
                [](const auto& change) {
                    return change.name == "Gain";
                }),
            "source-target automation must not be applied a second time in parent routing");
    // The Timeline's parameter controllers are
    // internal, not MIDI continuous controllers: its automation
    // sends no MIDI.
    const auto played = talas::runtime::renderOffline(*compiled.graph, 4096);
    require(played.ok() && played.midiEvents.empty() && played.globalControllerEvents.empty(),
        "the Timeline's controllers should stay internal, sending no MIDI");
}

talas::project::ProjectDocument renderFixture();

void testGraphProjectionFailureLeavesRenderSettingsUntouched()
{
    auto document = renderFixture();
    document.timeline.score.tracks[0].channelCount = 3;
    document.timeline.score.tracks[1].channelCount = 3;
    const auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 3, 5, 48000, 7));
    require(prepared.ok(), prepared.error);

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 64;
    settings.eventValues.push_back(
        {"Existing", 0.25, talas::expr::ValueDomain::Scalar});
    settings.preparedScore = prepared.schedule;
    const auto compiled = talas::runtime::compileProject(
        document, registry(), settings);
    require(!compiled.ok(),
            "unsupported three-channel Score output should reject late in graph projection");
    require(settings.eventValues.size() == 1
                && settings.eventValues.front().name == "Existing"
                && settings.eventValueChanges.empty()
                && settings.preparedScore == prepared.schedule,
            "failed Score graph projection must leave prior render settings untouched");
}

void testLiveClipDoesNotCollideWithExistingEventAuthority()
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    settings.eventValues.push_back(
        {"ScoreClipGate0", 0.5, talas::expr::ValueDomain::Scalar});
    const auto compiled = talas::runtime::compileProject(
        renderFixture(), registry(), std::move(settings));
    auto detail = std::string{
        "generated Score gates must not collide with valid caller Event Values"};
    for (const auto& diagnostic : compiled.diagnostics)
        detail += " | " + diagnostic.message;
    require(compiled.ok(), detail);
    const auto& projected = compiled.graph->renderSettings();
    require(std::ranges::any_of(
                projected.eventValues,
                [](const auto& binding) {
                    return binding.scopeId == talas::runtime::globalRuntimeScope
                        && binding.name == "ScoreClipGate0"
                        && binding.value == 0.5;
                })
                && std::ranges::none_of(
                    projected.eventValues,
                    [](const auto& binding) {
                        return binding.name == "ScoreClipGate0_1";
                    })
                && projected.preparedScoreClipAudio.empty(),
            "live Score projection must retain caller authority without authoring a redundant gate or PCM copy");
}

talas::project::ProjectDocument renderFixture()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    talas::project::ProjectNode oscillator;
    oscillator.instanceId = "osc";
    oscillator.typeId = "talas.oscillator";
    talas::project::ProjectParameterExpression frequency;
    frequency.parameterId = "frequency";
    frequency.expression = "1000 hz";
    oscillator.parameters.push_back(std::move(frequency));
    talas::project::ProjectParameterExpression amplitude;
    amplitude.parameterId = "amplitude";
    amplitude.expression = "1.0";
    oscillator.parameters.push_back(std::move(amplitude));
    voice.graph.nodes.push_back(std::move(oscillator));
    document.soundDefinitions.push_back(std::move(voice));
    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::Seconds, "master", 1,
         talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds, {}, 1,
         talas::project::ScoreOverlapMode::Layer},
    };
    document.timeline.score.clips = {
        {"clip", "track", "voice", seconds(2.0 / 48000.0),
         seconds(4.0 / 48000.0), seconds(0.0),
         seconds(1.0 / 48000.0), seconds(1.0 / 48000.0), 1},
    };
    return document;
}

// A track's Angle and Radius place its
// clips around the listener: Angle 0 hard left, 0.5 ahead, 1 hard right,
// 1.5 behind (folded to the front on two channels); Radius 0 at the
// listener, beyond 1 quieter. Offline and in realtime alike.
void testTrackAngleAndRadiusPanClips()
{
    using Channels = std::array<std::array<double, 8>, 2>;
    const auto render = [](std::optional<double> angle, std::optional<double> radius) {
        auto document = renderFixture();
        document.timeline.score.tracks[0].channelCount = 2;
        document.timeline.score.tracks[1].channelCount = 2;
        const auto lane = [&](std::string id, std::string name, double value) {
            document.timeline.score.automation.push_back({std::move(id), talas::project::ScoreAutomationScope::Track,
                "track", std::move(name), talas::expr::ValueDomain::Scalar,
                {{seconds(0.0), value, talas::core::AutomationInterpolation::Step}}});
        };
        if (angle)
            lane("angle", "Angle", *angle);
        if (radius)
            lane("radius", "Radius", *radius);
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 48000.0;
        settings.maxBlockFrames = 8;
        const auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
        auto detail = std::string{"a panned stereo Score should compile"};
        for (const auto& diagnostic : compiled.diagnostics)
            detail += " | " + diagnostic.message;
        require(compiled.ok(), detail);
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
        require(offline.ok() && offline.channels.size() == 2, "a panned stereo Score should render offline");
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), session.error());
        Channels live{};
        double* outputs[] = {live[0].data(), live[1].data()};
        require(session.render({}, outputs, 2, 8), "a panned stereo Score should render in realtime");
        Channels rendered{};
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            for (std::size_t frame = 0; frame < 8; ++frame)
            {
                rendered[channel][frame] = offline.channels[channel][frame];
                require(std::abs(live[channel][frame] - rendered[channel][frame]) < 1.0e-9,
                    "a track's position should pan alike offline and in realtime");
            }
        }
        return rendered;
    };
    const auto scaled = [](const Channels& actual, const Channels& base, double left, double right) {
        for (std::size_t frame = 0; frame < 8; ++frame)
        {
            if (std::abs(actual[0][frame] - left * base[0][frame]) > 1.0e-9
                || std::abs(actual[1][frame] - right * base[1][frame]) > 1.0e-9)
                return false;
        }
        return true;
    };
    const auto base = render(std::nullopt, std::nullopt);
    require(std::ranges::any_of(base[0], [](double value) { return std::abs(value) > 1.0e-4; }) && base[0] == base[1],
        "an unpanned clip should sound alike on both channels");
    require(scaled(render(0.0, std::nullopt), base, 1.0, 0.0), "Angle 0 should be hard left");
    require(scaled(render(1.0, std::nullopt), base, 0.0, 1.0), "Angle 1 should be hard right");
    require(scaled(render(0.75, std::nullopt), base, 0.5, 1.0), "Angle 0.75 should lean right");
    require(scaled(render(1.5, std::nullopt), base, 1.0, 1.0) && scaled(render(2.0, std::nullopt), base, 1.0, 0.0),
        "behind folds onto the front, and Angle wraps every 2");
    require(scaled(render(0.0, 0.0), base, 1.0, 1.0), "Radius 0 should be at the listener, both channels alike");
    require(scaled(render(std::nullopt, 2.0), base, 0.5, 0.5), "Radius 2 should be twice as far, half as loud");
}

// A panned Timeline remixes itself to the
// speakers of the Speaker Placement, a Sound going between the two either
// side of its angle and spreading to all as its Radius shrinks, the
// subwoofer apart; on Stereo as on two channels without a placement.
void testSpeakerPlacementSpreadsTheTimelineOverItsSpeakers()
{
    const auto preset = [](std::string_view name) {
        const auto& presets = talas::project::speakerPlacementPresets();
        const auto found = std::ranges::find(presets, name, &talas::project::SpeakerPlacementPreset::name);
        require(found != presets.end(), "the placement presets should include " + std::string{name});
        return found->speakers;
    };
    const auto stereo = preset("Stereo");
    for (const auto angle : {0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.5})
    {
        for (const auto radius : {0.0, 0.5, 1.0, 1.5, -0.5})
        {
            const auto gains = talas::runtime::scoreSpeakerGains(angle, radius, stereo);
            const auto pan = talas::runtime::scoreTrackPanFor(angle, radius);
            require(std::abs(gains[0] - pan.left) < 1.0e-12 && std::abs(gains[1] - pan.right) < 1.0e-12,
                "Stereo should pan as two channels do");
        }
    }
    const auto quad = preset("Quad");
    const auto gains = [&](double angle, double radius, const std::vector<talas::project::SpeakerPosition>& speakers) {
        const auto all = talas::runtime::scoreSpeakerGains(angle, radius, speakers);
        return std::vector<double>(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(speakers.size()));
    };
    require(gains(0.25, 1.0, quad) == std::vector<double>{1.0, 0.0, 0.0, 0.0}
            && gains(0.5, 1.0, quad) == std::vector<double>{1.0, 1.0, 0.0, 0.0}
            && gains(1.5, 1.0, quad) == std::vector<double>{0.0, 0.0, 1.0, 1.0}
            && gains(1.0, 1.0, quad) == std::vector<double>{0.0, 1.0, 0.0, 1.0},
        "on Quad a Sound should go between the speakers either side of it, behind as in front");
    require(gains(0.25, -1.0, quad) == std::vector<double>{0.0, 0.0, 1.0, 0.0},
        "a negative Radius should be as far behind");
    require(gains(0.5, 0.0, preset("Quad + Ctr + Sub")) == std::vector<double>{1.0, 1.0, 1.0, 1.0, 1.0, 0.0}
            && gains(0.5, 2.0, preset("Quad + Ctr + Sub"))[4] == 0.5,
        "at the listener a Sound should come from every speaker but the subwoofer; twice as far, half as loud");

    // A Timeline of four outputs, panned to 45 degrees, plays from the front
    // left speaker of Quad alone, offline and in realtime.
    const auto render = [&](std::optional<double> angle) {
        auto document = renderFixture();
        document.timeline.score.tracks[0].channelCount = 4;
        document.timeline.score.tracks[1].channelCount = 4;
        if (angle)
            document.timeline.score.automation.push_back({"angle", talas::project::ScoreAutomationScope::Track, "track", "Angle",
                talas::expr::ValueDomain::Scalar, {{seconds(0.0), *angle, talas::core::AutomationInterpolation::Step}}});
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 48000.0;
        settings.maxBlockFrames = 8;
        settings.speakerPlacement = quad;
        const auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
        auto detail = std::string{"a Score of four outputs should compile"};
        for (const auto& diagnostic : compiled.diagnostics)
            detail += " | " + diagnostic.message;
        require(compiled.ok(), detail);
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
        require(offline.ok() && offline.channels.size() == 4, "a Score of four outputs should render four channels offline");
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), session.error());
        std::array<std::array<double, 8>, 4> live{};
        double* outputs[] = {live[0].data(), live[1].data(), live[2].data(), live[3].data()};
        require(session.render({}, outputs, 4, 8), "a Score of four outputs should render in realtime");
        for (std::size_t channel = 0; channel < 4; ++channel)
        {
            for (std::size_t frame = 0; frame < 8; ++frame)
                require(std::abs(live[channel][frame] - offline.channels[channel][frame]) < 1.0e-9,
                    "the placed speakers should pan alike offline and in realtime");
        }
        return offline.channels;
    };
    const auto base = render(std::nullopt);
    const auto front = render(0.25);
    require(std::ranges::any_of(base[0], [](double value) { return std::abs(value) > 1.0e-4; }) && front[0] == base[0]
            && std::ranges::all_of(front[1], [](double value) { return value == 0.0; })
            && std::ranges::all_of(front[2], [](double value) { return value == 0.0; })
            && std::ranges::all_of(front[3], [](double value) { return value == 0.0; }),
        "on Quad, Angle 0.25 should come from the front left speaker alone");
}

// A track hard-wired to output channels
// plays there, its pan controls set aside; alike offline and in realtime.
void testTracksHardWiredToOutputs()
{
    const auto render = [](std::vector<std::uint32_t> wired, bool angled) {
        auto document = renderFixture();
        document.timeline.score.tracks[0].channelCount = 2;
        document.timeline.score.tracks[1].channelCount = 2;
        document.timeline.score.tracks[0].outputChannels = std::move(wired);
        if (angled)
            document.timeline.score.automation.push_back({"angle", talas::project::ScoreAutomationScope::Track, "track", "Angle",
                talas::expr::ValueDomain::Scalar, {{seconds(0.0), 0.0, talas::core::AutomationInterpolation::Step}}});
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 48000.0;
        settings.maxBlockFrames = 8;
        const auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
        require(compiled.ok(), "a wired Score should compile");
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
        require(offline.ok() && offline.channels.size() == 2, "a wired Score should render offline");
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), session.error());
        std::array<std::array<double, 8>, 2> live{};
        double* outputs[] = {live[0].data(), live[1].data()};
        require(session.render({}, outputs, 2, 8), "a wired Score should render in realtime");
        for (std::size_t channel = 0; channel < 2; ++channel)
            for (std::size_t frame = 0; frame < 8; ++frame)
                require(std::abs(live[channel][frame] - offline.channels[channel][frame]) < 1.0e-9,
                    "a wired track plays alike offline and in realtime");
        return offline.channels;
    };
    const auto base = render({}, false);
    const auto right = render({2}, true);
    bool heard = false;
    for (std::size_t frame = 0; frame < 8; ++frame)
    {
        require(right[0][frame] == 0.0 && std::abs(right[1][frame] - (base[0][frame] + base[1][frame])) < 1.0e-9,
            "the track output goes to the fixed hardware output channel, its pan control set aside");
        heard = heard || right[1][frame] != 0.0;
    }
    require(heard, "the wired channel carries the track");
    const auto both = render({1, 2}, true);
    for (std::size_t frame = 0; frame < 8; ++frame)
        require(both[0][frame] == base[0][frame] && both[1][frame] == base[1][frame],
            "wired to both channels, each of the track's channels goes to its own");
}

// A lane plays its function through its
// transformations; a slaved lane's transform its own copy of the master's
// function, the master playing it untransformed.
void testTransformedLanesPlayTheirTransformedFunction()
{
    auto document = renderFixture();
    talas::project::ScoreAutomationLane master;
    master.laneId = "reverbMaster";
    master.scope = talas::project::ScoreAutomationScope::Master;
    master.ownerId = "master";
    master.eventName = "ReverbMaster";
    master.points = {{seconds(0.0), 0.25, talas::core::AutomationInterpolation::Step}};
    auto slaved = master;
    slaved.laneId = "decay";
    slaved.scope = talas::project::ScoreAutomationScope::Track;
    slaved.ownerId = "track";
    slaved.eventName = "Decay";
    slaved.masterLaneId = "reverbMaster";
    slaved.transforms = {{talas::project::ScoreAutomationTransformKind::Invert, 0.5, 0.0, 1}};
    auto own = master;
    own.laneId = "rate";
    own.scope = talas::project::ScoreAutomationScope::Track;
    own.ownerId = "track";
    own.eventName = "Rate";
    own.transforms = {{talas::project::ScoreAutomationTransformKind::Scale, 4.0, 0.0, 1}};
    document.timeline.score.automation = {master, slaved, own};
    const auto prepared = talas::runtime::prepareProjectScore(
        document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);
    const auto valueOf = [&](std::string_view name) {
        const auto lanes = prepared.schedule->automationLanes();
        const auto lane = std::ranges::find(lanes, name, &talas::runtime::PreparedScoreAutomationLane::eventName);
        require(lane != lanes.end(), "the lane is prepared");
        return talas::runtime::scoreLaneValueAt(*prepared.schedule, *lane, 0).value_or(-1.0);
    };
    require(valueOf("Rate") == 1.0, "a lane plays its function through its transformation");
    require(valueOf("Decay") == 0.75, "the transformations affect the slaved function only");
    require(valueOf("ReverbMaster") == 0.25, "the original master function is untransformed");
}

// A Sound reading !BPM is slaved to the
// Timeline's BPM, following its tempo changes, so tempo-dependent Sounds
// share a common tempo; a Sound that does not read it is untouched.
void testSoundsFollowTheTimelinesBpm()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition clicks;
    clicks.definitionId = "clicks";
    clicks.displayName = "Clicks";
    talas::project::ProjectParameterExpression value;
    value.parameterId = "value";
    value.expression = "!BPM / 1000";
    clicks.graph.nodes = {{"level", "talas.valToSound", {value}}};
    clicks.outputs = {{"audioOut", talas::core::PortType::Audio, "level", "audioOut"}};
    document.soundDefinitions = {clicks};
    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds, "master", 1,
            talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::Seconds, {}, 1,
            talas::project::ScoreOverlapMode::Layer}};
    document.timeline.score.clips = {{"clip", "track", "clicks", seconds(0.0), seconds(2.0), seconds(0.0), seconds(0.0),
        seconds(0.0), 1}};
    // 60 BPM, then 120 from the second beat, a second in.
    document.timeline.musicalTime.tempo = {{0, 1000000}, {960, 500000}};
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 64;
    const auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
    require(compiled.ok(), "a Timeline with a BPM-reading Sound compiles"
        + (compiled.diagnostics.empty() ? std::string{} : ": " + compiled.diagnostics.front().message));
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 2000);
    require(rendered.ok() && rendered.mono.size() == 2000, "render the BPM-reading Sound");
    require(std::abs(rendered.mono[500] - 0.06) < 1e-9, "the Sound's BPM is the Timeline's, 60");
    require(std::abs(rendered.mono[1500] - 0.12) < 1e-9, "and follows its tempo change to 120");
}

// A Sound's local BPM may be slaved to the master
// BPM through the transformation buttons, as a function of it (half here),
// or unslaved, with its own tempo; two points make an accelerando.
void testALocalBpmFollowsOrLeavesTheMaster()
{
    const auto played = [](std::vector<talas::project::ScoreAutomationLane> lanes, std::string label) {
        talas::project::ProjectDocument document;
        talas::project::ProjectSoundDefinition clicks;
        clicks.definitionId = "clicks";
        clicks.displayName = "Clicks";
        talas::project::ProjectParameterExpression value;
        value.parameterId = "value";
        value.expression = "!BPM / 1000";
        clicks.graph.nodes = {{"level", "talas.valToSound", {value}}};
        clicks.outputs = {{"audioOut", talas::core::PortType::Audio, "level", "audioOut"}};
        // The Sound's BPM, a control of the Timeline.
        talas::project::ProjectSoundExposedParameter bpm;
        bpm.parameterId = "bpm";
        bpm.displayName = "BPM";
        bpm.defaultValue = {"bpm", "120"};
        bpm.bindings.push_back({talas::project::ProjectSoundParameterTarget::EventValue, {}, {}, "BPM",
            talas::expr::ValueDomain::Scalar});
        clicks.exposedParameters.push_back(std::move(bpm));
        document.soundDefinitions = {clicks};
        document.timeline.score.tracks = {
            {"track", "Track", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds, "master", 1,
                talas::project::ScoreOverlapMode::Layer},
            {"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::Seconds, {}, 1,
                talas::project::ScoreOverlapMode::Layer}};
        document.timeline.score.clips = {{"clip", "track", "clicks", seconds(0.0), seconds(1.0), seconds(0.0),
            seconds(0.0), seconds(0.0), 1}};
        document.timeline.score.automation = std::move(lanes);
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 1000.0;
        settings.maxBlockFrames = 64;
        const auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
        require(compiled.ok(), label + ": a Timeline with a local BPM compiles"
            + (compiled.diagnostics.empty() ? std::string{} : ": " + compiled.diagnostics.front().message));
        const auto rendered = talas::runtime::renderOffline(*compiled.graph, 1000);
        require(rendered.ok() && rendered.mono.size() == 1000, "render the BPM-reading Sound");
        return rendered.mono[500] * 1000.0;
    };
    const auto step = talas::core::AutomationInterpolation::Step;
    const talas::project::ScoreAutomationLane master{"bpmMaster", talas::project::ScoreAutomationScope::Master, "master", "BPM",
        talas::expr::ValueDomain::Scalar, {{seconds(0.0), 120.0, step}}};
    auto local = talas::project::ScoreAutomationLane{"bpm", talas::project::ScoreAutomationScope::Clip, "clip", "BPM",
        talas::expr::ValueDomain::Scalar, {{seconds(0.0), 90.0, step}}};
    const auto alone = played({local}, "alone");
    require(std::abs(alone - 90.0) < 1e-6, "a clip's own BPM lane sets it: " + std::to_string(alone));
    require(std::abs(played({master}, "master") - 120.0) < 1e-6, "the Sound follows the master BPM");
    auto slaved = local;
    slaved.points.clear();
    slaved.masterLaneId = "bpmMaster";
    slaved.transforms = {{talas::project::ScoreAutomationTransformKind::Scale, 0.5, 0.0, 1}};
    const auto half = played({master, slaved}, "slaved");
    require(std::abs(half - 60.0) < 1e-6,
        "transformed, the local BPM should be a function of the master, half of it: " + std::to_string(half));
    const auto own = played({master, local}, "own");
    require(std::abs(own - 90.0) < 1e-6, "unslaved, the local BPM should keep its own tempo: " + std::to_string(own));
    // An accelerando is a point at the original tempo where
    // the change begins and one at the new tempo where it ends.
    auto accelerando = local;
    accelerando.points = {{seconds(0.0), 60.0, talas::core::AutomationInterpolation::Linear},
        {seconds(1.0), 120.0, talas::core::AutomationInterpolation::Linear}};
    const auto between = played({accelerando}, "accelerando");
    require(std::abs(between - 90.0) < 0.5,
        "halfway between its two points the tempo should be halfway, 90: " + std::to_string(between));
}

// A disk player's Rate slaved to AmpMaster, a
// master control, plays at the master's value as the Timeline plays; a
// Sound beside it with no control of that name is untouched by it.
void testADiskPlayersRateFollowsItsMasterControl()
{
    talas::project::ProjectDocument document;
    // A disk player whose Rate is a control.
    talas::project::ProjectSoundDefinition disk;
    disk.definitionId = "disk";
    disk.displayName = "Disk";
    talas::project::ProjectNode player{"player", "talas.diskPlayer", {}};
    talas::project::ProjectParameterExpression file;
    file.parameterId = "fileName";
    file.resource = "Samples/Ramp";
    player.parameters = {file, {"duration", "2 s"}, {"rateScale", "!Rate"}};
    disk.graph.nodes = {player};
    disk.outputs = {{"audioOut", talas::core::PortType::Audio, "player", "audioOut"}};
    talas::project::ProjectSoundExposedParameter rateControl;
    rateControl.parameterId = "rate";
    rateControl.displayName = "Rate";
    rateControl.defaultValue = {"rate", "1"};
    rateControl.bindings.push_back({talas::project::ProjectSoundParameterTarget::EventValue, {}, {}, "Rate",
        talas::expr::ValueDomain::Scalar});
    disk.exposedParameters.push_back(std::move(rateControl));
    // A Sound with no controls.
    talas::project::ProjectSoundDefinition silence;
    silence.definitionId = "silence";
    silence.displayName = "Silence";
    silence.graph.nodes = {{"level", "talas.valToSound", {{"value", "0"}}}};
    silence.outputs = {{"audioOut", talas::core::PortType::Audio, "level", "audioOut"}};
    document.soundDefinitions = {disk, silence};
    document.timeline.score.tracks = {
        {"player", "Player", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds, "master", 1,
            talas::project::ScoreOverlapMode::Layer},
        {"other", "Other", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds, "master", 1,
            talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::Seconds, {}, 1,
            talas::project::ScoreOverlapMode::Layer}};
    document.timeline.score.clips = {
        {"diskClip", "player", "disk", seconds(0.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1},
        {"otherClip", "other", "silence", seconds(0.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1}};
    // AmpMaster: a half for a second, then all the way up.
    const auto step = talas::core::AutomationInterpolation::Step;
    const talas::project::ScoreAutomationLane master{"ampMaster", talas::project::ScoreAutomationScope::Master, "master",
        "AmpMaster", talas::expr::ValueDomain::Scalar, {{seconds(0.0), 0.5, step}, {seconds(1.0), 1.0, step}}};
    auto rate = talas::project::ScoreAutomationLane{"rate", talas::project::ScoreAutomationScope::Clip, "diskClip", "Rate",
        talas::expr::ValueDomain::Scalar, {}};
    rate.masterLaneId = "ampMaster";
    document.timeline.score.automation = {master, rate};
    // The disk file: a ramp, its slope the rate it is played at.
    std::vector<double> ramp(4000);
    for (std::size_t index = 0; index < ramp.size(); ++index)
        ramp[index] = static_cast<double>(index) / 4000.0;
    const auto path = std::filesystem::temp_directory_path()
        / ("talas-score-master-rate-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + ".wav");
    std::string writeError;
    const std::vector<std::vector<double>> channels{ramp};
    require(talas::audio::writePcm16Wav(path, 1000.0, channels, writeError), writeError);
    const auto hash = talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "the ramp WAV should hash");
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Samples/Ramp";
    snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot.generation = 1;
    snapshot.contentHash = hash.contentHash;
    snapshot.resolvedPath = path;
    snapshot.channelCount = 1;
    snapshot.sampleRate = 1000.0;
    snapshot.frameCount = ramp.size();
    snapshot.encoding = "pcm16-wav";
    snapshot.projectAssetPath = "Samples/Ramp.wav";
    snapshot.provenance = "repository-original";
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 64;
    settings.resourceSnapshots = {std::make_shared<const talas::core::ResourceSnapshot>(std::move(snapshot))};
    const auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
    require(compiled.ok(), "a master control should leave a Sound without that control untouched"
        + (compiled.diagnostics.empty() ? std::string{} : ": " + compiled.diagnostics.front().message));
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 2000);
    require(rendered.ok() && rendered.mono.size() == 2000, "render the Timeline"
        + (rendered.diagnostics.empty() ? std::string{} : ": " + rendered.diagnostics.front().message));
    const auto slope = [&](std::size_t from) {
        return (rendered.mono[from + 100] - rendered.mono[from]) / 100.0 * 4000.0;
    };
    std::error_code removed;
    std::filesystem::remove(path, removed);
    // The 16-bit file rounds the ramp by up to half a step of 2^-15.
    require(std::abs(slope(300) - 0.5) < 0.01 && std::abs(slope(1300) - 1.0) < 0.01,
        "the disk player's Rate should follow AmpMaster: " + std::to_string(slope(300)) + ", "
            + std::to_string(slope(1300)));
}

// A MIDIOutputController in a Timeline sends its Amp
// out as MIDI controller 7; promoted to the master control AmpMaster, with
// a disk player's Rate slaved to it, the rate change in the disk player is
// mirrored by controller 7.
void testATimelinesMidiOutputMirrorsADiskPlayersRate()
{
    const auto control = [](std::string id, std::string name, std::string value) {
        talas::project::ProjectSoundExposedParameter exposed;
        exposed.parameterId = std::move(id);
        exposed.displayName = name;
        exposed.defaultValue = {exposed.parameterId, std::move(value)};
        exposed.bindings.push_back({talas::project::ProjectSoundParameterTarget::EventValue, {}, {}, std::move(name),
            talas::expr::ValueDomain::Scalar});
        return exposed;
    };
    talas::project::ProjectDocument document;
    // The MIDIOutputController as the prototype comes, its Amp a control.
    talas::project::ProjectSoundDefinition out;
    out.definitionId = "midiOut";
    out.displayName = "MIDIOutputController";
    out.graph.nodes = {{"out", "talas.midiOutputController", {}}};
    out.outputs = {{"audioOut", talas::core::PortType::Audio, "out", "audioOut"}};
    out.exposedParameters.push_back(control("amp", "Amp", "0"));
    // A Sound reading the disk player's Rate.
    talas::project::ProjectSoundDefinition rate;
    rate.definitionId = "rate";
    rate.displayName = "Rate";
    rate.graph.nodes = {{"level", "talas.valToSound", {{"value", "!Rate"}}}};
    rate.outputs = {{"audioOut", talas::core::PortType::Audio, "level", "audioOut"}};
    rate.exposedParameters.push_back(control("rate", "Rate", "1"));
    document.soundDefinitions = {out, rate};
    document.timeline.score.tracks = {
        {"midi", "MIDI", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds, "master", 1,
            talas::project::ScoreOverlapMode::Layer},
        {"player", "Player", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds, "master", 1,
            talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::Seconds, {}, 1,
            talas::project::ScoreOverlapMode::Layer}};
    document.timeline.score.clips = {
        {"outClip", "midi", "midiOut", seconds(0.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1},
        {"rateClip", "player", "rate", seconds(0.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1}};
    const auto step = talas::core::AutomationInterpolation::Step;
    const talas::project::ScoreAutomationLane master{"ampMaster", talas::project::ScoreAutomationScope::Master, "master",
        "AmpMaster", talas::expr::ValueDomain::Scalar, {{seconds(0.0), 0.5, step}, {seconds(1.0), 1.0, step}}};
    auto amp = talas::project::ScoreAutomationLane{"amp", talas::project::ScoreAutomationScope::Clip, "outClip", "Amp",
        talas::expr::ValueDomain::Scalar, {}};
    amp.masterLaneId = "ampMaster";
    auto slaved = talas::project::ScoreAutomationLane{"rate", talas::project::ScoreAutomationScope::Clip, "rateClip",
        "Rate", talas::expr::ValueDomain::Scalar, {}};
    slaved.masterLaneId = "ampMaster";
    document.timeline.score.automation = {master, amp, slaved};
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 64;
    const auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
    require(compiled.ok(), "a Timeline with a MIDIOutputController should compile"
        + (compiled.diagnostics.empty() ? std::string{} : ": " + compiled.diagnostics.front().message));
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 2000);
    require(rendered.ok() && rendered.mono.size() == 2000, "render the Timeline"
        + (rendered.diagnostics.empty() ? std::string{} : ": " + rendered.diagnostics.front().message));
    const auto controllerAt = [&](std::size_t frame) {
        std::optional<int> value;
        for (const auto& event : rendered.midiEvents)
        {
            if (event.frame <= frame && event.bytes.size() == 3 && event.bytes[0] == 0xb0 && event.bytes[1] == 7)
                value = event.bytes[2];
        }
        return value;
    };
    std::string sent;
    for (const auto& event : rendered.midiEvents)
        sent += std::to_string(event.frame) + ":" + std::to_string(event.bytes.size() > 2 ? event.bytes[2] : -1) + " ";
    require(controllerAt(300) == std::optional<int>{64} && controllerAt(1300) == std::optional<int>{127},
        "the Timeline should send Amp out as MIDI controller 7: " + sent);
    // Each change once: the clip's controller keeps what it sent from one
    // callback to the next.
    require(rendered.midiEvents.size() == 2, "controller 7 should be sent once a change: " + sent);
    require(std::abs(rendered.mono[300] - 0.5) < 1e-9 && std::abs(rendered.mono[1300] - 1.0) < 1e-9,
        "the Rate slaved to AmpMaster should change with controller 7");
}

// A stereo Sound dragged into a Timeline keeps its
// stereo, its left and right apart on the track's two channels.
void testAStereoSoundStaysStereoInATrack()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition pair;
    pair.definitionId = "pair";
    pair.displayName = "Pair";
    talas::project::ProjectParameterExpression left;
    left.parameterId = "value";
    left.expression = "0.25";
    talas::project::ProjectParameterExpression right;
    right.parameterId = "value";
    right.expression = "0.75";
    pair.graph.nodes = {{"l", "talas.valToSound", {left}}, {"r", "talas.valToSound", {right}}};
    pair.outputs = {{"leftOut", talas::core::PortType::Audio, "l", "audioOut"},
        {"rightOut", talas::core::PortType::Audio, "r", "audioOut"}};
    document.soundDefinitions = {pair};
    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds, "master", 2,
            talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::Seconds, {}, 2,
            talas::project::ScoreOverlapMode::Layer}};
    document.timeline.score.clips = {{"clip", "track", "pair", seconds(0.0), seconds(1.0), seconds(0.0), seconds(0.0),
        seconds(0.0), 1}};
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    settings.maxBlockFrames = 64;
    const auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
    require(compiled.ok(), "a stereo Sound in a Timeline compiles"
        + (compiled.diagnostics.empty() ? std::string{} : ": " + compiled.diagnostics.front().message));
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 1000);
    require(rendered.ok() && rendered.channels.size() == 2, "the Timeline plays on two channels");
    const auto l = rendered.channels[0][500];
    const auto r = rendered.channels[1][500];
    require(std::abs(l - 0.25) < 1e-6 && std::abs(r - 0.75) < 1e-6,
        "the stereo Sound's left and right should stay apart: " + std::to_string(l) + " "
            + std::to_string(r));
}

// The Panning status shows the apparent source
// of a track's sound around the listener, moving with its Angle and Radius.
void testTheTrackPanningStatus()
{
    const auto at = [](std::vector<talas::project::ScoreAutomationLane> lanes, std::uint64_t frame) {
        auto document = renderFixture();
        document.timeline.score.automation = std::move(lanes);
        const auto prepared = talas::runtime::prepareProjectScore(
            document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, 48000, 1));
        require(prepared.ok(), prepared.error);
        return talas::runtime::scoreTrackSourcePosition(*prepared.schedule, 0, frame);
    };
    const auto lane = [](std::string name, std::vector<talas::project::ScoreAutomationPoint> points) {
        return talas::project::ScoreAutomationLane{name, talas::project::ScoreAutomationScope::Track, "track", name,
            talas::expr::ValueDomain::Scalar, std::move(points)};
    };
    const auto step = [](double time, double value) {
        return talas::project::ScoreAutomationPoint{seconds(time), value, talas::core::AutomationInterpolation::Step};
    };
    const auto near = [](talas::runtime::ScoreTrackSourcePosition position, double x, double y) {
        return std::abs(position.x - x) < 1e-9 && std::abs(position.y - y) < 1e-9;
    };
    require(near(at({}, 0), 0.0, 1.0), "an unpanned track sounds from straight ahead");
    require(near(at({lane("Angle", {step(0.0, 0.0)})}, 0), -1.0, 0.0), "Angle 0 is hard left");
    require(near(at({lane("Angle", {step(0.0, 1.0)})}, 0), 1.0, 0.0), "Angle 1 is hard right");
    require(near(at({lane("Angle", {step(0.0, 1.5)})}, 0), 0.0, -1.0), "Angle 1.5 is behind");
    require(near(at({lane("Radius", {step(0.0, 0.0)})}, 0), 0.0, 0.0), "Radius 0 is at the listener's head");
    require(near(at({lane("Radius", {step(0.0, -1.0)})}, 0), 0.0, -1.0), "Radius -1 is on the circle behind");
    const auto pan = lane("Angle", {{seconds(0.0), 0.0, talas::core::AutomationInterpolation::Linear},
        {seconds(1.0), 2.0, talas::core::AutomationInterpolation::Linear}});
    require(near(at({pan}, 12000), 0.0, 1.0) && near(at({pan}, 36000), 0.0, -1.0),
        "the source moves around the room as the Timeline plays, a full clockwise pan");
}

// A Sound's FadeInOut control is its
// amplitude envelope, times its track's Level, offline and in realtime
// alike; its Sound reading !FadeInOut is not given it.
void testASoundsFadeInOutIsItsAmplitude()
{
    const auto render = [](const talas::project::ProjectDocument& document) {
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 48000.0;
        settings.maxBlockFrames = 8;
        const auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
        require(compiled.ok(), "a faded Score should compile");
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
        require(offline.ok(), "a faded Score should render offline");
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), session.error());
        std::array<double, 8> live{};
        double* outputs[] = {live.data()};
        require(session.render({}, outputs, 1, 8), "a faded Score should render in realtime");
        for (std::size_t frame = 0; frame < 8; ++frame)
            require(std::abs(live[frame] - offline.mono[frame]) < 1.0e-9, "a faded Sound plays alike offline and in realtime");
        return offline.mono;
    };
    const auto lane = [](std::string id, talas::project::ScoreAutomationScope scope, std::string owner, std::string name, double value) {
        return talas::project::ScoreAutomationLane{std::move(id), scope, std::move(owner), std::move(name),
            talas::expr::ValueDomain::Scalar, {{seconds(0.0), value, talas::core::AutomationInterpolation::Step}}};
    };
    const auto base = render(renderFixture());
    auto faded = renderFixture();
    faded.timeline.score.automation.push_back(lane("fade", talas::project::ScoreAutomationScope::Clip, "clip", "FadeInOut", 0.5));
    const auto half = render(faded);
    faded.timeline.score.automation.push_back(lane("level", talas::project::ScoreAutomationScope::Track, "track", "Level", 0.5));
    const auto quarter = render(faded);
    bool heard = false;
    for (std::size_t frame = 0; frame < 8; ++frame)
    {
        require(std::abs(half[frame] - 0.5 * base[frame]) < 1.0e-9,
            "a Sound's FadeInOut at 0.5 should play it at half its amplitude");
        require(std::abs(quarter[frame] - 0.25 * base[frame]) < 1.0e-9,
            "a Sound's amplitude is its FadeInOut envelope times its track's level");
        heard = heard || base[frame] != 0.0;
    }
    require(heard, "the fixture is heard");
}

// A track's Level is the fader its Sounds pass
// through, their fades times the Level of their track and of each submix
// their output goes through, offline and in realtime alike; a cache stays
// before it, and a Sound reading !Level is not given it.
void testTrackLevelsAreFaders()
{
    const auto render = [](const talas::project::ProjectDocument& document) {
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 48000.0;
        settings.maxBlockFrames = 8;
        const auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
        require(compiled.ok(), "a levelled Score should compile");
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
        require(offline.ok(), "a levelled Score should render offline");
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), session.error());
        std::array<double, 8> live{};
        double* outputs[] = {live.data()};
        require(session.render({}, outputs, 1, 8), "a levelled Score should render in realtime");
        for (std::size_t frame = 0; frame < 8; ++frame)
            require(std::abs(live[frame] - offline.mono[frame]) < 1.0e-9, "a levelled track plays alike offline and in realtime");
        return offline.mono;
    };
    const auto level = [](std::string owner, double value) {
        return talas::project::ScoreAutomationLane{"level_" + owner, talas::project::ScoreAutomationScope::Track, owner, "Level",
            talas::expr::ValueDomain::Scalar, {{seconds(0.0), value, talas::core::AutomationInterpolation::Step}}};
    };
    const auto base = render(renderFixture());
    auto halved = renderFixture();
    halved.timeline.score.automation.push_back(level("track", 0.5));
    const auto half = render(halved);
    auto routed = renderFixture();
    routed.timeline.score.tracks[0].outputTrackId = "sub";
    routed.timeline.score.tracks.insert(routed.timeline.score.tracks.begin() + 1,
        {"sub", "Sub", talas::project::ScoreTrackKind::Submix, talas::project::ScoreTimeDomain::Seconds, "master", 1,
            talas::project::ScoreOverlapMode::Layer});
    routed.timeline.score.automation = {level("track", 0.5), level("sub", 0.5)};
    const auto quarter = render(routed);
    bool heard = false;
    for (std::size_t frame = 0; frame < 8; ++frame)
    {
        require(std::abs(half[frame] - 0.5 * base[frame]) < 1.0e-9,
            "a Sound's amplitude is its FadeInOut envelope times its track's level");
        require(std::abs(quarter[frame] - 0.25 * base[frame]) < 1.0e-9,
            "the Level of the submix its output goes through applies too");
        heard = heard || base[frame] != 0.0;
    }
    require(heard, "the fixture is heard");

    talas::runtime::RenderSettings scored;
    scored.sampleRate = 48000.0;
    scored.maxBlockFrames = 8;
    scored.preparedScore = talas::runtime::prepareProjectScore(
        halved, talas::runtime::preparedScoreBindingForDocument(halved, 1, 1, 48000, 1)).schedule;
    const auto heardClip = talas::runtime::materializeProjectScoreClips(halved, registry(), scored);
    talas::runtime::ScoreClipMaterializationOptions cache;
    cache.preFader = true;
    const auto cached = talas::runtime::materializeProjectScoreClips(halved, registry(), scored, cache);
    require(heardClip.ok() && cached.ok() && heardClip.clips.size() == 1 && cached.clips.size() == 1,
        "materialize the levelled clip: " + heardClip.error + cached.error);
    for (std::size_t frame = 1; frame < 3; ++frame)
        require(std::abs((*heardClip.clips[0]->channels[0])[frame] - 0.5 * (*cached.clips[0]->channels[0])[frame]) < 1.0e-6
                && (*cached.clips[0]->channels[0])[frame] != 0.0,
            "the cache is recorded before the track's level, and played at it");
    require(heardClip.clips[0]->sourceFingerprint != cached.clips[0]->sourceFingerprint, "the levelled clip is its own audio");

    auto reading = renderFixture();
    reading.soundDefinitions[0].graph.nodes[0].parameters[1].expression = "!Level";
    reading.timeline.score.automation.push_back(level("track", 0.5));
    talas::runtime::RenderSettings readingSettings;
    readingSettings.sampleRate = 48000.0;
    readingSettings.maxBlockFrames = 8;
    readingSettings.eventValues = {{"Level", 1.0, talas::expr::ValueDomain::Scalar}};
    const auto compiled = talas::runtime::compileProject(reading, registry(), std::move(readingSettings));
    require(compiled.ok(), "a Sound reading !Level compiles");
    const auto read = talas::runtime::renderOffline(*compiled.graph, 8);
    for (std::size_t frame = 0; frame < 8; ++frame)
        require(read.ok() && std::abs(read.mono[frame] - 0.5 * base[frame]) < 1.0e-9,
            "a track's Level is its fader, not the Sound's !Level");
}

// Caches are recorded pre-fader: what happens
// after the Sound (its fades, its start, panning, sends, which track it is
// on) leaves a cache valid; its duration, a track control reaching it at a
// new start, or its track's input do not.
void testCachesAreRecordedPreFader()
{
    const auto fixture = renderFixture();
    std::string error;
    const auto print = [&](const talas::project::ProjectDocument& document) {
        const auto fingerprint = talas::runtime::scoreClipSourceFingerprint(document, registry(), "clip", 48000, error);
        require(!fingerprint.empty(), "the clip should fingerprint: " + error);
        return fingerprint;
    };
    const auto base = print(fixture);
    auto faded = fixture;
    faded.timeline.score.clips.front().fadeIn = seconds(2.0 / 48000.0);
    require(print(faded) == base, "a new FadeInOut does not invalidate the cache, recorded before it");
    auto moved = fixture;
    moved.timeline.score.clips.front().start = seconds(10.0 / 48000.0);
    require(print(moved) == base, "changing the start time of the Sound leaves the cache valid");
    auto panned = fixture;
    panned.timeline.score.automation.push_back({"angle", talas::project::ScoreAutomationScope::Track, "track", "Angle",
        talas::expr::ValueDomain::Scalar, {{seconds(0.0), 0.25, talas::core::AutomationInterpolation::Step}}});
    require(print(panned) == base, "the cache comes before the track's panning");
    auto otherTrack = fixture;
    otherTrack.timeline.score.tracks.insert(otherTrack.timeline.score.tracks.begin(), otherTrack.timeline.score.tracks[0]);
    otherTrack.timeline.score.tracks.front().trackId = "other";
    otherTrack.timeline.score.clips.front().trackId = "other";
    require(print(otherTrack) == base, "changing the track does not affect the Sound before its track");
    auto sent = fixture;
    sent.timeline.score.tracks.insert(sent.timeline.score.tracks.end() - 1,
        {"vox", "Vox", talas::project::ScoreTrackKind::Submix, talas::project::ScoreTimeDomain::Seconds, "master", 1,
            talas::project::ScoreOverlapMode::Layer});
    auto sentAndKept = sent;
    const auto submixed = print(sent);
    sentAndKept.timeline.score.tracks.front().sendTrackIds = {"vox"};
    require(print(sentAndKept) == submixed, "sending the track to a new submix does not invalidate the cache");
    auto longer = fixture;
    longer.timeline.score.clips.front().duration = seconds(6.0 / 48000.0);
    require(print(longer) != base, "changing the duration of the Sound invalidates the cache");
    auto withInput = sent;
    withInput.timeline.score.tracks.front().inputTrackId = "vox";
    require(print(withInput) != submixed, "changing the input to the track invalidates the cache");
    auto controlled = fixture;
    controlled.timeline.score.automation.push_back({"level", talas::project::ScoreAutomationScope::Track, "track", "Gain",
        talas::expr::ValueDomain::Scalar, {{seconds(0.0), 0.5, talas::core::AutomationInterpolation::Step},
                                           {seconds(4.0 / 48000.0), 1.0, talas::core::AutomationInterpolation::Linear}}});
    auto controlledLater = controlled;
    controlledLater.timeline.score.clips.front().start = seconds(1.0 / 48000.0);
    require(print(controlledLater) != print(controlled),
        "with a parameter on a track control, changing the start invalidates the cache");

    // The cache itself holds the Sound before its fades.
    const auto prepared = talas::runtime::prepareProjectScore(
        fixture, talas::runtime::preparedScoreBindingForDocument(fixture, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    settings.preparedScore = prepared.schedule;
    talas::runtime::ScoreClipMaterializationOptions options;
    options.forceLiveSource = true;
    const auto heard = talas::runtime::materializeProjectScoreClip(fixture, registry(), settings, "clip", options);
    options.preFader = true;
    const auto cached = talas::runtime::materializeProjectScoreClip(fixture, registry(), settings, "clip", options);
    require(heard.ok() && cached.ok(), "the clip should materialize both ways");
    const auto& faded0 = *heard.clips.front()->channels.front();
    const auto& raw = *cached.clips.front()->channels.front();
    require(faded0.front() == 0.0 && raw.back() != 0.0 && faded0.back() == 0.0,
        "the cache contains the audio output of the Sound before it gets modified by FadeInOut");
}

// A Sound's Panner and Distance move it from its
// track's position, Panner added to the track's Angle and Distance to its
// Radius; alike offline and in realtime.
void testSoundPannerAndDistanceDeviateFromTheTrack()
{
    using Channels = std::array<std::array<double, 8>, 2>;
    struct Position
    {
        std::optional<double> angle;
        std::optional<double> panner;
        std::optional<double> distance;
    };
    const auto render = [](Position position) {
        auto document = renderFixture();
        document.timeline.score.tracks[0].channelCount = 2;
        document.timeline.score.tracks[1].channelCount = 2;
        const auto lane = [&](std::string id, talas::project::ScoreAutomationScope scope, std::string owner, std::string name,
                              double value) {
            document.timeline.score.automation.push_back({std::move(id), scope, std::move(owner), std::move(name),
                talas::expr::ValueDomain::Scalar, {{seconds(0.0), value, talas::core::AutomationInterpolation::Step}}});
        };
        if (position.angle)
            lane("angle", talas::project::ScoreAutomationScope::Track, "track", "Angle", *position.angle);
        if (position.panner)
            lane("panner", talas::project::ScoreAutomationScope::Clip, "clip", "Panner", *position.panner);
        if (position.distance)
            lane("distance", talas::project::ScoreAutomationScope::Clip, "clip", "Distance", *position.distance);
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 48000.0;
        settings.maxBlockFrames = 8;
        const auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
        require(compiled.ok(), "a Sound with a position should compile");
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
        require(offline.ok() && offline.channels.size() == 2, "a Sound with a position should render offline");
        talas::runtime::RealtimeRenderSession session(*compiled.graph);
        require(session.ready(), session.error());
        Channels live{};
        double* outputs[] = {live[0].data(), live[1].data()};
        require(session.render({}, outputs, 2, 8), "a Sound with a position should render in realtime");
        Channels rendered{};
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            for (std::size_t frame = 0; frame < 8; ++frame)
            {
                rendered[channel][frame] = offline.channels[channel][frame];
                require(std::abs(live[channel][frame] - rendered[channel][frame]) < 1.0e-9,
                    "a Sound's position should pan alike offline and in realtime");
            }
        }
        return rendered;
    };
    const auto scaled = [](const Channels& actual, const Channels& base, double left, double right) {
        for (std::size_t frame = 0; frame < 8; ++frame)
        {
            if (std::abs(actual[0][frame] - left * base[0][frame]) > 1.0e-9
                || std::abs(actual[1][frame] - right * base[1][frame]) > 1.0e-9)
                return false;
        }
        return true;
    };
    const auto base = render({});
    require(scaled(render({0.5, -0.5, std::nullopt}), base, 1.0, 0.0),
        "a Sound's angle is its Panner added to its track's Angle");
    require(scaled(render({std::nullopt, 0.5, std::nullopt}), base, 0.0, 1.0),
        "the track is the carrier and the Sound's Panner its deviation");
    require(scaled(render({std::nullopt, std::nullopt, 1.0}), base, 0.5, 0.5),
        "a Sound's distance is its Distance added to its track's Radius");
    require(scaled(render({std::nullopt, 0.0, 0.0}), base, 1.0, 1.0),
        "Panner and Distance 0 leave the Sound at its track's position");
}

// A track's Angle is a position on a circle
// counted modulo 2, so 2 and 4 are 0 again and -0.5 the same place as 1.5,
// behind; drawing the Angle from 0 up to 2 pans clockwise all the way
// round, faster the steeper the line. Radius 1 is on the circle, less is
// nearer the listener (0 at the nose, from every side alike), more is
// farther and quieter, up to 2; the Radius starts at 1.
void testTrackPositionWrapsAroundTheCircle()
{
    const auto same = [](talas::runtime::ScoreTrackPan left, talas::runtime::ScoreTrackPan right) {
        return std::abs(left.left - right.left) < 1.0e-12 && std::abs(left.right - right.right) < 1.0e-12;
    };
    using talas::runtime::scoreTrackPanFor;
    require(same(scoreTrackPanFor(2.0, 1.0), scoreTrackPanFor(0.0, 1.0)) && same(scoreTrackPanFor(4.0, 1.0), scoreTrackPanFor(0.0, 1.0))
            && same(scoreTrackPanFor(2.75, 1.0), scoreTrackPanFor(0.75, 1.0)),
        "an Angle should come full circle every 2");
    require(same(scoreTrackPanFor(-0.5, 1.0), scoreTrackPanFor(1.5, 1.0)) && same(scoreTrackPanFor(-0.25, 1.0), scoreTrackPanFor(1.75, 1.0)),
        "an angle less than 0 should be the same place as 2 more, behind");
    const auto inside = scoreTrackPanFor(0.0, 0.5);
    const auto onCircle = scoreTrackPanFor(0.0, 1.0);
    const auto atNose = scoreTrackPanFor(0.0, 0.0);
    require(onCircle.left == 1.0 && onCircle.right == 0.0 && inside.left == 1.0 && inside.right == 0.5
            && atNose.left == 1.0 && atNose.right == 1.0,
        "a Radius under 1 should draw the Sound in from the circle toward the listener, 0 from both sides alike");
    require(same(scoreTrackPanFor(0.5, 3.0), scoreTrackPanFor(0.5, 2.0)) && scoreTrackPanFor(0.5, 2.0).left == 0.5,
        "a Radius over 1 should be quieter, no farther than 2");

    // An Angle drawn from 0 to 2 in one second, and another in half.
    const auto lanePosition = [](double length) {
        auto document = renderFixture();
        document.timeline.score.automation.push_back({"angle", talas::project::ScoreAutomationScope::Track, "track", "Angle",
            talas::expr::ValueDomain::Scalar,
            {{seconds(0.0), 0.0, talas::core::AutomationInterpolation::Linear}, {seconds(length), 2.0, talas::core::AutomationInterpolation::Linear}}});
        const auto prepared = talas::runtime::prepareProjectScore(
            document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, 48000, 1));
        require(prepared.ok(), "the panned Score should prepare: " + prepared.error);
        const auto lane = std::ranges::find(prepared.schedule->automationLanes(), std::string_view{"Angle"},
            &talas::runtime::PreparedScoreAutomationLane::eventName);
        require(lane != prepared.schedule->automationLanes().end(), "the Angle lane should be prepared");
        return [schedule = prepared.schedule, lane = *lane](std::uint64_t frame) {
            return talas::runtime::scoreTrackPan(*schedule, 0, frame).value();
        };
    };
    const auto slow = lanePosition(1.0);
    const auto fast = lanePosition(0.5);
    require(same(slow(0), scoreTrackPanFor(0.0, 1.0)) && same(slow(12000), scoreTrackPanFor(0.5, 1.0))
            && same(slow(24000), scoreTrackPanFor(1.0, 1.0)) && same(slow(36000), scoreTrackPanFor(1.5, 1.0))
            && same(slow(48000), scoreTrackPanFor(0.0, 1.0)),
        "an Angle drawn from 0 to 2 should pan from the left past the front, the right and behind, back to the left");
    require(same(fast(12000), scoreTrackPanFor(1.0, 1.0)), "a steeper Angle line should pan faster");
}

// A Sound's own parameter controls go
// with it when it moves in time or to another track, while a track's
// controls stay where they were drawn, affecting whichever Sounds lie in
// the track; a control has no effect outside its Sound.
void testClipControlsMoveWithTheirSoundAndTrackControlsStay()
{
    const auto prepared = [](double start, std::string track) {
        auto document = renderFixture();
        document.timeline.score.tracks.insert(document.timeline.score.tracks.begin() + 1,
            {"other", "Other", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds, "master", 1,
                talas::project::ScoreOverlapMode::Layer});
        document.timeline.score.clips.front().start = seconds(start);
        document.timeline.score.clips.front().trackId = std::move(track);
        document.timeline.score.clips.front().duration = seconds(1.0);
        document.timeline.score.automation = {
            {"own", talas::project::ScoreAutomationScope::Clip, "clip", "Gain", talas::expr::ValueDomain::Scalar,
                {{seconds(0.25), 0.5, talas::core::AutomationInterpolation::Step}}},
            {"lane", talas::project::ScoreAutomationScope::Track, "track", "Gain", talas::expr::ValueDomain::Scalar,
                {{seconds(1.5), 0.75, talas::core::AutomationInterpolation::Step}}}};
        const auto score = talas::runtime::prepareProjectScore(
            document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, 48000, 1));
        require(score.ok(), "the Score should prepare: " + score.error);
        std::array<std::uint64_t, 2> frames{};
        for (const auto& lane : score.schedule->automationLanes())
        {
            const auto first = score.schedule->automationPoints()[lane.firstPoint].frame;
            frames[lane.scope == talas::project::ScoreAutomationScope::Clip ? 0 : 1] = first;
        }
        return frames;
    };
    const auto before = prepared(1.0, "track");
    const auto after = prepared(2.0, "other");
    require(before[0] == 60000 && after[0] == 108000,
        "a Sound's own control should move with it in time and to another track");
    require(before[1] == 72000 && after[1] == 72000, "a track's control should stay where it was drawn");
}

// A Live or Record-enabled lane leaves its
// parameter to its fader or controller, driving nothing until it is
// Automated again.
void testOnlyAutomatedLanesDriveTheirParameters()
{
    const auto pointsFor = [](talas::project::ScoreAutomationState state) {
        auto document = renderFixture();
        document.timeline.score.automation = {
            {"lane", talas::project::ScoreAutomationScope::Track, "track", "Gain", talas::expr::ValueDomain::Scalar,
                {{seconds(0.5), 0.75, talas::core::AutomationInterpolation::Step}}}};
        document.timeline.score.automation.front().state = state;
        const auto score = talas::runtime::prepareProjectScore(
            document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, 48000, 1));
        require(score.ok(), "the Score should prepare: " + score.error);
        require(score.schedule->automationLanes().size() == 1, "the lane should still be prepared");
        return score.schedule->automationLanes().front().pointCount;
    };
    require(pointsFor(talas::project::ScoreAutomationState::Automated) == 1, "an Automated lane drives its parameter");
    require(pointsFor(talas::project::ScoreAutomationState::Live) == 0, "a Live lane leaves it to the fader");
    require(pointsFor(talas::project::ScoreAutomationState::Record) == 0, "a Record-enabled lane leaves it to the controller");
}

// A slaved control plays its master's function,
// in its master's state, at the master's times in the Timeline.
void testSlavedLanesFollowTheirMaster()
{
    auto document = renderFixture();
    document.timeline.score.clips.front().start = seconds(1.0);
    document.timeline.score.clips.front().duration = seconds(4.0);
    const auto linear = talas::core::AutomationInterpolation::Linear;
    talas::project::ScoreAutomationLane master{"reverbMaster", talas::project::ScoreAutomationScope::Master, "master", "ReverbMaster",
        talas::expr::ValueDomain::Scalar, {{seconds(0.0), 0.0, linear}, {seconds(2.0), 1.0, linear}, {seconds(4.0), 0.0, linear}}};
    talas::project::ScoreAutomationLane amp{"amp", talas::project::ScoreAutomationScope::Clip, "clip", "ReverbAmp",
        talas::expr::ValueDomain::Scalar, {{seconds(0.0), 0.3, talas::core::AutomationInterpolation::Step}}};
    amp.masterLaneId = "reverbMaster";
    document.timeline.score.automation = {master, amp};
    const auto ids = std::vector<std::string_view>{"voice"};
    require(!talas::project::projectScoreValidationError(document.timeline.score, ids).has_value(),
        "a Sound's control may be slaved to a master control");
    const auto prepare = [&] {
        const auto score = talas::runtime::prepareProjectScore(
            document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, 48000, 1));
        require(score.ok(), "the Score should prepare: " + score.error);
        return score;
    };
    auto score = prepare();
    const auto& slaved = score.schedule->automationLanes()[1];
    const auto points = score.schedule->automationPoints().subspan(slaved.firstPoint, slaved.pointCount);
    require(slaved.eventName == "ReverbAmp" && points.size() == 3 && points[1].frame == 96000 && points[1].value == 1.0,
        "one control exactly parallels the changes in the other");
    document.timeline.score.automation.front().state = talas::project::ScoreAutomationState::Live;
    score = prepare();
    require(score.schedule->automationLanes()[1].pointCount == 0,
        "slaved controllers are in the same state as the master's control");
    auto chained = document;
    chained.timeline.score.automation.push_back({"decay", talas::project::ScoreAutomationScope::Clip, "clip", "Decay",
        talas::expr::ValueDomain::Scalar, {}});
    chained.timeline.score.automation.back().masterLaneId = "amp";
    require(talas::project::projectScoreValidationError(chained.timeline.score, ids).has_value(),
        "a control cannot be slaved to one that is slaved itself");
}

// The transport jumps from the cursor to the marker
// before or after it, twice back two markers, and to the important time
// before or after it, the start or end of a Sound's time bar.
void testTransportJumpsToMarkersAndImportantTimes()
{
    auto document = renderFixture();
    document.timeline.score.clips.front().start = seconds(1.0);
    document.timeline.score.clips.front().duration = seconds(2.0);
    document.timeline.score.markers = {{"a", "A", seconds(0.5), std::nullopt}, {"b", "B", seconds(1.5), std::nullopt},
        {"c", "C", seconds(2.5), std::nullopt}};
    const auto prepared = talas::runtime::prepareProjectScore(
        document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, 48000, 1));
    require(prepared.ok(), "the Score should prepare: " + prepared.error);
    const auto& schedule = *prepared.schedule;
    const std::uint64_t cursor = 96000;
    require(talas::runtime::previousScoreMarkerFrame(schedule, cursor) == 72000
            && talas::runtime::nextScoreMarkerFrame(schedule, cursor) == 120000,
        "the jumps should reach the markers either side of the cursor");
    require(talas::runtime::previousScoreMarkerFrame(schedule, *talas::runtime::previousScoreMarkerFrame(schedule, cursor)) == 24000,
        "two jumps back should go back two markers");
    require(!talas::runtime::nextScoreMarkerFrame(schedule, 120000).has_value(), "there should be no marker after the last");
    require(talas::runtime::previousScoreImportantFrame(schedule, cursor) == 48000
            && talas::runtime::nextScoreImportantFrame(schedule, cursor) == 144000,
        "the important times should be the time bar's start and end");
}

void testScoreClipFingerprintReusesRegistryOnBoundedWorkerStack()
{
    const auto document = renderFixture();
    const auto nodeRegistry = registry();
    std::string fingerprint;
    std::string error;
    std::thread worker([&] {
        fingerprint = talas::runtime::scoreClipSourceFingerprint(
            document, nodeRegistry, "clip", 48000, error);
    });
    worker.join();
    require(
        !fingerprint.empty(),
        error.empty()
            ? "Score clip fingerprint should reuse registry authority on a bounded worker stack"
            : error);
}

void testScoreSoundFixtureProducesAudioWithoutProjection()
{
    auto document = renderFixture();
    document.timeline.score = {};
    document.graph = {};
    talas::project::ProjectNode instance;
    instance.instanceId = "voice";
    instance.soundDefinitionId = "voice";
    document.graph.nodes.push_back(std::move(instance));
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}, {}});
    document.graph.connections.push_back(
        {"voice", "audioOut", "out", "audioIn"});
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    const auto compiled = talas::runtime::compileProject(
        document, registry(), std::move(settings));
    require(compiled.ok(),
            "Score Sound fixture should compile without graph projection");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 8);
    require(rendered.ok()
                && std::ranges::any_of(rendered.mono, [](double value) {
                    return std::abs(value) > 1.0e-4;
                }),
            "Score Sound fixture should be audible without graph projection");
}

void testSixChannelScoreMasterUsesDirectTerminal()
{
    auto document = renderFixture();
    document.timeline.score.tracks[0].channelCount = 6;
    document.timeline.score.tracks[1].channelCount = 6;
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    const auto compiled = talas::runtime::compileProject(
        document, registry(), std::move(settings));
    require(compiled.ok(),
        "six-channel Score Master should compile through Output 6");
    require(compiled.graph->plan().outputBufferIndices.size() == 6,
        "six-channel Score Master should publish six exact output lanes");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 8);
    require(rendered.ok() && rendered.channels.size() == 6,
        "six-channel Score Master should render six channels");
    for (std::size_t channel = 1; channel < 6; ++channel)
        require(rendered.channels[channel] == rendered.channels[0],
            "mono Score source should be duplicated into all six routed lanes");
}

void testScoreGateTimelineWorksOnTheProjectedGraphShape()
{
    auto document = renderFixture();
    document.timeline.score = {};
    document.graph = {};
    talas::project::ProjectNode instance;
    instance.instanceId = "scoreClip_0";
    instance.soundDefinitionId = "voice";
    talas::project::ProjectNode gate;
    gate.instanceId = "scoreGate_0_0";
    gate.typeId = "talas.vca";
    talas::project::ProjectNode gateControl;
    gateControl.instanceId = "scoreGateControl_0_0";
    gateControl.typeId = "talas.constant";
    gateControl.parameters.push_back(
        {"value", "!ScoreClipGate0"});
    document.graph.nodes.push_back(std::move(instance));
    document.graph.nodes.push_back(std::move(gateControl));
    document.graph.nodes.push_back(std::move(gate));
    document.graph.nodes.push_back({"scoreOutput", "talas.audioOutput", {}, {}});
    document.graph.connections = {
        {"scoreClip_0", "audioOut", "scoreGate_0_0", "audioIn"},
        {"scoreGateControl_0_0", "value", "scoreGate_0_0", "gain"},
        {"scoreGate_0_0", "audioOut", "scoreOutput", "audioIn"},
    };
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    settings.eventValues.push_back(
        {"ScoreClipGate0", 0.0, talas::expr::ValueDomain::Scalar});
    settings.eventValueChanges = {
        {2, "ScoreClipGate0", 0.0, talas::expr::ValueDomain::Scalar,
         talas::core::AutomationInterpolation::Linear},
        {3, "ScoreClipGate0", 1.0, talas::expr::ValueDomain::Scalar},
        {5, "ScoreClipGate0", 1.0, talas::expr::ValueDomain::Scalar,
         talas::core::AutomationInterpolation::Linear},
        {6, "ScoreClipGate0", 0.0, talas::expr::ValueDomain::Scalar},
    };
    const auto compiled = talas::runtime::compileProject(
        document, registry(), std::move(settings));
    require(compiled.ok(), "projected Score graph shape should compile");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 8);
    require(rendered.ok()
                && std::ranges::any_of(rendered.mono, [](double value) {
                    return std::abs(value) > 1.0e-4;
                }),
            "projected Score graph shape should apply its gate Timeline");
}

std::array<float, 8> renderScore(std::span<const std::size_t> partitions)
{
    const auto nodeRegistry = registry();
    talas::runtime::ProjectRuntimeBridge bridge(renderFixture(), nodeRegistry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    const auto staged = bridge.compileAndStage(settings);
    require(staged.ok(), staged.message);
    bridge.setPlaying(true);
    std::array<float, 8> output{};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        float* channel = output.data() + offset;
        require(bridge.render(&channel, 1, frames),
                "Score graph should render every callback partition");
        offset += frames;
    }
    require(offset == output.size(),
            "Score partition fixture should cover the exact render range");
    return output;
}

std::array<float, 8> renderScoreOffline()
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    const auto compiled = talas::runtime::compileProject(
        renderFixture(), registry(), std::move(settings));
    auto compileDetail = std::string{
        "offline Score document should compile through the shared projection"};
    for (const auto& diagnostic : compiled.diagnostics)
        compileDetail += " | " + diagnostic.message;
    require(compiled.ok(), compileDetail);
    require(compiled.graph->renderSettings().preparedScore != nullptr,
            "offline compilation should retain its immutable Score schedule");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 8);
    require(rendered.ok() && rendered.mono.size() == 8,
            "offline Score graph should render its exact frame range");
    std::array<float, 8> output{};
    std::ranges::transform(
        rendered.mono, output.begin(), [](double value) {
            return static_cast<float>(value);
        });
    return output;
}

void testScoreGraphRendersExactClipFadesAcrossCallbackPartitions()
{
    constexpr std::array<std::size_t, 1> whole{8};
    constexpr std::array<std::size_t, 4> split{2, 1, 3, 2};
    const auto direct = renderScore(whole);
    const auto partitioned = renderScore(split);
    const auto offline = renderScoreOffline();
    for (std::size_t frame = 0; frame < direct.size(); ++frame)
    {
        require(std::abs(direct[frame] - partitioned[frame]) < 1.0e-6F,
                "Score render must be independent of callback partitioning");
        require(std::abs(direct[frame] - offline[frame]) < 1.0e-6F,
                "offline and realtime Score projections must remain sample exact");
    }
    require(std::abs(direct[0]) < 1.0e-6F
                && std::abs(direct[1]) < 1.0e-6F
                && std::abs(direct[2]) < 1.0e-6F,
            "Score clip should remain silent through its exact fade-in start");
    require(std::abs(direct[3]) > 1.0e-4F
                || std::abs(direct[4]) > 1.0e-4F
                || std::abs(direct[5]) > 1.0e-4F,
            "Score clip should render its isolated Sound inside the active range: "
                + std::to_string(direct[0]) + ","
                + std::to_string(direct[1]) + ","
                + std::to_string(direct[2]) + ","
                + std::to_string(direct[3]) + ","
                + std::to_string(direct[4]) + ","
                + std::to_string(direct[5]) + ","
                + std::to_string(direct[6]) + ","
                + std::to_string(direct[7]));
    require(std::abs(direct[6]) < 1.0e-6F
                && std::abs(direct[7]) < 1.0e-6F,
            "Score clip should be silent from its exclusive end frame");
}

void testScoreGraphRelocatesAndLoopsAtExactCallbackBoundaries()
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    auto compiled = talas::runtime::compileProject(
        renderFixture(), registry(), settings);
    require(compiled.ok(), "Score relocation fixture should compile");
    talas::runtime::RealtimeAudioEngine active(*compiled.graph);
    require(active.ready(), active.error());
    // Live DSP history is prepared on control; the audio-boundary setter only
    // installs the requested position. Keep the original numerical requirement.
    std::uint64_t preparationBudget = 1000000;
    std::string preparationError;
    auto initialPoint = active.prepareTransportRelocation(3, preparationBudget, preparationError);
    require(initialPoint != nullptr, preparationError);
    require(active.installTransportRelocation(*initialPoint), "install exact initial Score source history");
    active.setPlaying(true);
    active.setTransportFrameBeforeFirstRender(3);
    std::array<float, 3> relocated{};
    float* relocatedChannel = relocated.data();
    require(active.render(&relocatedChannel, 1, relocated.size())
                && (std::abs(relocated[1]) > 1.0e-4F
                    || std::abs(relocated[2]) > 1.0e-4F),
            "relocation inside a Score clip should reconstruct its active gate");

    talas::runtime::RealtimeAudioEngine ended(*compiled.graph);
    require(ended.ready(), ended.error());
    ended.setPlaying(true);
    ended.setTransportFrameBeforeFirstRender(6);
    std::array<float, 2> afterEnd{};
    float* afterEndChannel = afterEnd.data();
    require(ended.render(&afterEndChannel, 1, afterEnd.size())
                && std::ranges::all_of(afterEnd, [](float value) {
                    return std::abs(value) < 1.0e-6F;
                }),
            "relocation to the exclusive clip end should remain silent");

    settings.transportLoop = talas::runtime::TransportLoopRegion{2, 6};
    auto loopedGraph = talas::runtime::compileProject(
        renderFixture(), registry(), std::move(settings));
    require(loopedGraph.ok(), "Score loop fixture should compile");
    talas::runtime::RealtimeAudioEngine looped(*loopedGraph.graph);
    require(looped.ready(), looped.error());
    looped.setPlaying(true);
    std::array<float, 8> loopedOutput{};
    float* loopedChannel = loopedOutput.data();
    require(looped.render(&loopedChannel, 1, loopedOutput.size()),
            "Score loop fixture should render");
    require(std::abs(loopedOutput[4]) > 1.0e-4F
                && std::abs(loopedOutput[7]) > 1.0e-4F,
            "the prepared clip gate should reopen on the exact loop iteration");
}

void testLiveClipRejectsCallerPcmAndIsExactOfflineRealtimeAndOnSeek()
{
    auto document = renderFixture();
    const auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 3, 5, 48000, 7));
    require(prepared.ok(), prepared.error);
    auto audio = std::make_shared<talas::runtime::PreparedScoreClipAudio>();
    audio->binding = prepared.schedule->binding();
    audio->clipId = "clip";
    audio->sourceFingerprint = std::string(64, 'a');
    audio->sampleRate = 48000;
    audio->startFrame = 2;
    audio->frameCount = 4;
    audio->channels.push_back(
        std::make_shared<const std::vector<double>>(
            std::vector<double>{0.125, 0.25, 0.5, 0.75}));
    require(audio->valid(), "prepared clip-audio fixture should be valid");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    settings.preparedScore = prepared.schedule;
    settings.preparedScoreClipAudio.push_back(audio);
    auto compiled = talas::runtime::compileProject(
        document, registry(), std::move(settings));
    auto detail = std::string{"prepared clip audio should compile"};
    for (const auto& diagnostic : compiled.diagnostics)
        detail += " | " + diagnostic.message;
    require(compiled.ok(), detail);

    const auto& authoritativeAudio =
        compiled.graph->renderSettings().preparedScoreClipAudio;
    require(authoritativeAudio.empty(),
            "public compile must discard caller-supplied PCM and retain only compiler-owned live sources");
    talas::runtime::PreparedRuntimeGraph forged{compiled.graph->plan(), compiled.graph->renderSettings()};
    talas::runtime::RealtimeRenderSession forgedSession(forged);
    require(!forgedSession.ready(), "copying the public plan/settings cannot forge live source authority");

    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    require(offline.ok() && offline.mono.size() == 8,
            "prepared clip audio should render offline");
    std::array<double, 8> expected{};
    // Independent source oracle: clip starts at frame 2, lasts four frames,
    // and applies one-frame fades at both ends.
    expected[3] = std::sin(2 * std::acos(-1.0) * 1000 / 48000);
    expected[4] = std::sin(2 * std::acos(-1.0) * 1000 * 2 / 48000);
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
        require(std::abs(offline.mono[frame] - expected[frame]) < 1.0e-12,
                "offline prepared clip audio should follow exact project frames");

    talas::runtime::RealtimeAudioEngine realtime(*compiled.graph);
    require(realtime.ready(), realtime.error());
    realtime.setPlaying(true);
    std::array<float, 8> realtimeOutput{};
    float* first = realtimeOutput.data();
    require(realtime.render(&first, 1, 3),
            "prepared clip audio should render the first partition");
    float* second = realtimeOutput.data() + 3;
    require(realtime.render(&second, 1, 5),
            "prepared clip audio should render the second partition");
    for (std::size_t frame = 0; frame < expected.size(); ++frame)
        require(std::abs(realtimeOutput[frame] - expected[frame]) < 1.0e-6,
                "realtime prepared clip audio should match offline across partitions");

    talas::runtime::RealtimeAudioEngine relocated(*compiled.graph);
    require(relocated.ready(), relocated.error());
    std::uint64_t preparationBudget = 1000000;
    std::string preparationError;
    auto seekPoint = relocated.prepareTransportRelocation(4, preparationBudget, preparationError);
    require(seekPoint != nullptr, preparationError);
    require(relocated.installTransportRelocation(*seekPoint), "install exact live seek source history");
    relocated.setPlaying(true);
    relocated.setTransportFrameBeforeFirstRender(4);
    std::array<float, 2> seekOutput{};
    float* seek = seekOutput.data();
    require(relocated.render(&seek, 1, seekOutput.size())
                && std::abs(seekOutput[0]
                            - static_cast<float>(expected[4])) < 1.0e-6F
                && std::abs(seekOutput[1]
                            - static_cast<float>(expected[5])) < 1.0e-6F,
            "prepared clip audio should seek by exact project frame without stream state");
}

void testPublicCompilePreparesAllLiveSourcesAndDiscardsCallerPcm()
{
    auto document = renderFixture();
    auto second = document.timeline.score.clips.front();
    second.clipId = "clip2";
    second.start = seconds(8.0 / 48000.0);
    document.timeline.score.clips.push_back(std::move(second));
    const auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 9, 10, 48000, 11));
    require(prepared.ok(), prepared.error);

    auto partial = std::make_shared<talas::runtime::PreparedScoreClipAudio>();
    partial->binding = prepared.schedule->binding();
    partial->clipId = "clip";
    partial->sourceFingerprint = std::string(64, 'a');
    partial->sampleRate = 48000;
    partial->startFrame = 2;
    partial->frameCount = 4;
    partial->channels.push_back(
        std::make_shared<const std::vector<double>>(
            std::vector<double>(4, 0.25)));
    require(partial->valid(), "partial caller table fixture should be structurally valid");

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    settings.preparedScore = prepared.schedule;
    settings.preparedScoreClipAudio.push_back(partial);
    const auto compiled = talas::runtime::compileProject(
        document, registry(), std::move(settings));
    require(compiled.ok(),
            "public compile should replace a partial caller table with exact materialization");
    const auto& rebuilt = compiled.graph->renderSettings()
        .preparedScoreClipAudio;
    require(rebuilt.empty(), "partial public PCM must not replace any live clip source");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 16);
    require(rendered.ok(), "both compiler-owned live sources should render offline");
    for (std::size_t frame = 0; frame < 16; ++frame)
    {
        double expected = 0;
        if (frame == 3 || frame == 9) expected = std::sin(2 * std::acos(-1.0) * 1000 / 48000);
        if (frame == 4 || frame == 10) expected = std::sin(2 * std::acos(-1.0) * 1000 * 2 / 48000);
        require(std::abs(rendered.mono[frame] - expected) < 1e-12,
            "every prepared source keeps its exact independent clip phase and project interval");
    }

}

talas::project::ProjectDocument nonPeriodicMaterializationFixture(
    double offsetFrames,
    double durationFrames,
    std::uint32_t repeats)
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "noiseSource";
    definition.displayName = "Noise source";
    definition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "noise", "audioOut"});
    talas::project::ProjectNode noise;
    noise.instanceId = "noise";
    noise.typeId = "talas.noise";
    definition.graph.nodes.push_back(std::move(noise));
    document.soundDefinitions.push_back(std::move(definition));
    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::Seconds, "master", 1,
         talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds, {}, 1,
         talas::project::ScoreOverlapMode::Layer},
    };
    document.timeline.score.clips = {
        {"clip", "track", "noiseSource", seconds(0.0),
         seconds(durationFrames / 48000.0),
         seconds(offsetFrames / 48000.0),
         seconds(0.0), seconds(0.0), repeats},
    };
    return document;
}

talas::runtime::PreparedScoreClipAudioPtr materializeNoiseFixture(
    const talas::project::ProjectDocument& document)
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);
    settings.preparedScore = prepared.schedule;
    auto materialized = talas::runtime::materializeProjectScoreClips(
        document, registry(), settings);
    require(materialized.ok() && materialized.clips.size() == 1,
            materialized.error);
    return materialized.clips.front();
}

void testLiveMaterializationAppliesContentOffsetAndRestartsRepeats()
{
    const auto baseline = materializeNoiseFixture(
        nonPeriodicMaterializationFixture(0.0, 5.0, 1));
    const auto repeated = materializeNoiseFixture(
        nonPeriodicMaterializationFixture(2.0, 3.0, 2));
    require(baseline->valid() && repeated->valid()
                && baseline->frameCount == 5
                && repeated->frameCount == 6,
            "non-periodic materialization should retain exact frame counts");
    const auto& baselineSamples = *baseline->channels.front();
    const auto& repeatedSamples = *repeated->channels.front();
    require(std::abs(repeatedSamples[0] - baselineSamples[2]) < 1.0e-12
                && std::abs(repeatedSamples[1] - baselineSamples[3]) < 1.0e-12
                && std::abs(repeatedSamples[2] - baselineSamples[4]) < 1.0e-12,
            "content offset should advance the isolated source before capture");
    require(std::equal(
                repeatedSamples.begin(), repeatedSamples.begin() + 3,
                repeatedSamples.begin() + 3,
                [](double left, double right) {
                    return std::abs(left - right) < 1.0e-12;
                }),
            "every repeat should restart the non-periodic source from the same offset state");
}

void testLiveMaterializationRetainsLegacyTimelineEventValueAuthority()
{
    auto document = renderFixture();
    document.timeline.score.clips.front().start = seconds(2.0 / 48000.0);
    document.timeline.score.clips.front().duration = seconds(4.0 / 48000.0);
    document.soundDefinitions.front().graph.nodes.front()
        .parameters.back().expression = "!Gain";
    auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);

    talas::runtime::RenderSettings baselineSettings;
    baselineSettings.sampleRate = 48000.0;
    baselineSettings.maxBlockFrames = 8;
    baselineSettings.preparedScore = prepared.schedule;
    baselineSettings.eventValues.push_back(
        {"Gain", 1.0, talas::expr::ValueDomain::Scalar});
    const auto baseline = talas::runtime::materializeProjectScoreClips(
        document, registry(), baselineSettings);
    require(baseline.ok() && baseline.clips.size() == 1,
            baseline.error);

    auto changedSettings = baselineSettings;
    changedSettings.eventValues.front().value = 0.0;
    changedSettings.eventValueChanges = {
        {0, "Gain", 0.0, talas::expr::ValueDomain::Scalar,
         talas::core::AutomationInterpolation::Linear},
        {4, "Gain", 1.0, talas::expr::ValueDomain::Scalar},
    };
    const auto changed = talas::runtime::materializeProjectScoreClips(
        document, registry(), changedSettings);
    require(changed.ok() && changed.clips.size() == 1,
            changed.error);
    const auto& unity = *baseline.clips.front()->channels.front();
    const auto& ramped = *changed.clips.front()->channels.front();
    require(unity.size() == 4 && ramped.size() == 4,
            "legacy Timeline materialization fixture should retain exact clip frames");
    require(std::abs(ramped[0] - unity[0] * 0.5) < 1.0e-12
                && std::abs(ramped[1] - unity[1] * 0.75) < 1.0e-12
                && std::abs(ramped[2] - unity[2]) < 1.0e-12,
            "legacy Timeline Event Values should be sampled at clip start and advance in absolute project time");
}

void testMaterializationSettingsIgnoreProjectTransportLoop()
{
    auto document = nonPeriodicMaterializationFixture(2.0, 3.0, 2);
    document.timeline.loop = {true, 1.0 / 48000.0, 3.0 / 48000.0};
    const auto prepared = talas::runtime::prepareScoreClipMaterializationSettings(
        document,
        registry(),
        std::nullopt,
        "clip",
        48000.0,
        8);
    require(prepared.ok(), prepared.error);
    require(!prepared.settings->transportLoop.has_value(),
            "private Score clip source rendering must not inherit the project transport loop");

    const auto materialized = talas::runtime::materializeProjectScoreClip(
        document,
        registry(),
        *prepared.settings,
        "clip");
    require(materialized.ok() && materialized.clips.size() == 1
                && materialized.clips.front()->frameCount == 6,
            materialized.error);
}

void testLiveMaterializationRejectsUnboundedSourcePreroll()
{
    const auto document = nonPeriodicMaterializationFixture(100.0, 1.0, 1);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    const auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);
    settings.preparedScore = prepared.schedule;
    talas::runtime::ScoreClipMaterializationOptions options;
    options.maximumAggregateRenderSamples = 50;
    const auto rejected = talas::runtime::materializeProjectScoreClips(
        document, registry(), settings, options);
    require(!rejected.ok() && rejected.clips.empty()
                && rejected.error.find("source-render budget")
                    != std::string::npos,
            "large content offsets should fail before unbounded source preroll");
}

void testLiveMaterializationCancellationIsCheckedBetweenRenderBlocks()
{
    const auto document = nonPeriodicMaterializationFixture(100.0, 4.0, 1);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    const auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);
    settings.preparedScore = prepared.schedule;

    std::size_t cancellationChecks = 0;
    talas::runtime::ScoreClipMaterializationOptions options;
    options.shouldCancel = [&] {
        return ++cancellationChecks >= 6;
    };
    const auto cancelled = talas::runtime::materializeProjectScoreClips(
        document, registry(), settings, std::move(options));
    require(!cancelled.ok() && cancelled.cancelled
                && cancelled.clips.empty()
                && cancellationChecks >= 6
                && cancellationChecks < 12,
            "long source preroll must observe cancellation between bounded offline render blocks");
}

void testLiveMaterializationUsesExactConnectedSoundInstance()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition upstream;
    upstream.definitionId = "upstreamGenerator";
    upstream.displayName = "Upstream generator";
    upstream.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    talas::project::ProjectNode oscillator;
    oscillator.instanceId = "osc";
    oscillator.typeId = "talas.oscillator";
    oscillator.parameters = {
        {"frequency", "440 hz"},
        {"amplitude", "1.0"},
    };
    upstream.graph.nodes.push_back(std::move(oscillator));
    document.soundDefinitions.push_back(std::move(upstream));

    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "connectedVoice";
    definition.displayName = "Connected voice";
    definition.inputs.push_back({
        "sourceIn", talas::core::PortType::Audio, "gain", "audioIn"});
    definition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "gain", "audioOut"});
    definition.graph.nodes.push_back({"gain", "talas.vca", {}});
    document.soundDefinitions.push_back(std::move(definition));

    talas::project::ProjectNode upstreamInstance;
    upstreamInstance.instanceId = "upstreamSource";
    upstreamInstance.soundDefinitionId = "upstreamGenerator";
    document.graph.nodes.push_back(std::move(upstreamInstance));
    talas::project::ProjectNode instance;
    instance.instanceId = "connectedInstance";
    instance.soundDefinitionId = "connectedVoice";
    document.graph.nodes.push_back(std::move(instance));
    document.graph.connections.push_back({
        "upstreamSource", "audioOut", "connectedInstance", "sourceIn"});
    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::Seconds, "master", 1,
         talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds, {}, 1,
         talas::project::ScoreOverlapMode::Layer},
    };
    document.timeline.score.clips = {
        {"clip", "track", "connectedVoice", seconds(0.0),
         seconds(4.0 / 48000.0), seconds(0.0), seconds(0.0),
         seconds(0.0), 1},
    };
    document.timeline.score.clips.front().sourceInstanceId =
        "connectedInstance";

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);
    settings.preparedScore = prepared.schedule;
    const auto materialized = talas::runtime::materializeProjectScoreClips(
        document, registry(), settings);
    require(
        materialized.ok() && materialized.clips.size() == 1
            && materialized.clips.front()->frameCount == 4
            && std::ranges::any_of(
                *materialized.clips.front()->channels.front(),
                [](double sample) { return std::abs(sample) > 1.0e-12; }),
        materialized.error.empty()
            ? "connected Sound instance should materialize its exact upstream graph"
            : materialized.error);

    std::string fingerprintError;
    const auto upstreamFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            document, registry(), "clip", 48000, fingerprintError);
    require(!upstreamFingerprint.empty(), fingerprintError);

    auto withLiveSet = document;
    talas::project::ProjectSnapshotBank liveBank;
    liveBank.bankId = "liveBank";
    liveBank.displayName = "Live Bank";
    liveBank.snapshots.push_back({"snapshot", "Snapshot", {}});
    withLiveSet.snapshotLibrary.banks.push_back(std::move(liveBank));
    talas::project::ProjectLiveSet liveSet;
    liveSet.liveSetId = "performance";
    liveSet.displayName = "Performance";
    liveSet.cells.push_back({
        "snapshot",
        "Snapshot",
        0,
        0,
        {talas::project::ProjectLiveSetQuantizationKind::Immediate, 0},
        talas::project::ProjectLiveSetSnapshotTarget{"liveBank", "snapshot"},
        {},
    });
    liveSet.cells.push_back({
        "sound",
        "Sound",
        0,
        1,
        {talas::project::ProjectLiveSetQuantizationKind::Immediate, 0},
        talas::project::ProjectLiveSetSoundTarget{
            "connectedVoice", std::string{"connectedInstance"}},
        {},
    });
    withLiveSet.liveSetLibrary.liveSets.push_back(std::move(liveSet));
    require(
        talas::project::validateProjectLiveSetLibrary(withLiveSet).ok(),
        "fingerprint fixture should begin with valid unrelated Live Set authority");
    const auto liveSetFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            withLiveSet, registry(), "clip", 48000, fingerprintError);
    require(
        !liveSetFingerprint.empty()
            && liveSetFingerprint == upstreamFingerprint,
        fingerprintError.empty()
            ? "unrelated Live Set state must not affect the exact Score clip audio fingerprint"
            : fingerprintError);
    talas::runtime::RenderSettings liveSetSettings;
    liveSetSettings.sampleRate = 48000.0;
    liveSetSettings.maxBlockFrames = 8;
    const auto compiledWithLiveSet = talas::runtime::compileProject(
        withLiveSet, registry(), std::move(liveSetSettings));
    require(
        compiledWithLiveSet.ok(),
        compiledWithLiveSet.diagnostics.empty()
            ? "Score graph projection should not reinterpret separately prepared Live Set targets"
            : compiledWithLiveSet.diagnostics.front().message);

    auto changedUpstream = document;
    changedUpstream.soundDefinitions.front().graph.nodes.front()
        .parameters.front().expression = "880 hz";
    const auto changedUpstreamFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            changedUpstream,
            registry(),
            "clip",
            48000,
            fingerprintError);
    require(!changedUpstreamFingerprint.empty()
                && changedUpstreamFingerprint != upstreamFingerprint,
            "an upstream reusable-Sound dependency change must invalidate the clip fingerprint");

    auto assetBackedUpstream = document;
    auto& upstreamNode =
        assetBackedUpstream.soundDefinitions.front().graph.nodes.front();
    upstreamNode.typeId = "talas.diskPlayer";
    upstreamNode.parameters.clear();
    talas::project::ProjectSoundExposedParameter exposedSource;
    exposedSource.parameterId = "sourceFile";
    exposedSource.displayName = "Source file";
    exposedSource.kind = talas::core::ParameterKind::Resource;
    exposedSource.defaultValue.parameterId = "sourceFile";
    exposedSource.defaultValue.resource = "upstreamAudio";
    exposedSource.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "osc",
        "fileName",
        {},
        talas::expr::ValueDomain::Scalar,
    });
    assetBackedUpstream.soundDefinitions.front().exposedParameters.push_back(
        std::move(exposedSource));
    talas::project::ProjectAsset upstreamAsset;
    upstreamAsset.id = "upstreamAudio";
    upstreamAsset.kind = talas::project::ProjectAssetKind::Audio;
    upstreamAsset.path = "upstream.wav";
    upstreamAsset.contentHash = std::string(64, 'b');
    upstreamAsset.channelCount = 1;
    upstreamAsset.sampleRate = 48000.0;
    upstreamAsset.frameCount = 4;
    upstreamAsset.encoding = talas::project::pcm16WavAssetEncoding;
    upstreamAsset.generation = 1;
    assetBackedUpstream.assets.push_back(std::move(upstreamAsset));
    assetBackedUpstream.timeline.score.clips.front().renderArtifact =
        talas::project::ScoreClipRenderArtifact{
            talas::project::ScoreClipRenderMode::Cached,
            std::string(64, 'a'),
            48000,
            4,
            {"upstreamAudio"},
        };
    const auto assetFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            assetBackedUpstream,
            registry(),
            "clip",
            48000,
            fingerprintError);
    require(!assetFingerprint.empty(), fingerprintError);
    assetBackedUpstream.assets.front().contentHash = std::string(64, 'c');
    ++assetBackedUpstream.assets.front().generation;
    const auto changedAssetFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            assetBackedUpstream,
            registry(),
            "clip",
            48000,
            fingerprintError);
    require(!changedAssetFingerprint.empty()
                && changedAssetFingerprint != assetFingerprint,
            "an exposed-default asset used by an upstream reusable Sound must invalidate the clip fingerprint even when an artifact reuses its ID");

    auto overriddenAssetBackedUpstream = assetBackedUpstream;
    talas::project::ProjectParameterExpression sourceOverride;
    sourceOverride.parameterId = "sourceFile";
    sourceOverride.resource = "overrideAudio";
    overriddenAssetBackedUpstream.graph.nodes.front()
        .parameters.push_back(std::move(sourceOverride));
    auto overrideAsset = overriddenAssetBackedUpstream.assets.front();
    overrideAsset.id = "overrideAudio";
    overrideAsset.contentHash = std::string(64, 'f');
    overriddenAssetBackedUpstream.assets.push_back(
        std::move(overrideAsset));
    const auto overrideFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            overriddenAssetBackedUpstream,
            registry(),
            "clip",
            48000,
            fingerprintError);
    require(!overrideFingerprint.empty(), fingerprintError);
    auto changedUnusedDefault = overriddenAssetBackedUpstream;
    changedUnusedDefault.assets.front().contentHash = std::string(64, '1');
    ++changedUnusedDefault.assets.front().generation;
    const auto unchangedOverrideFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            changedUnusedDefault,
            registry(),
            "clip",
            48000,
            fingerprintError);
    require(unchangedOverrideFingerprint == overrideFingerprint,
            "an exact Resource override must remove its unused exposed default asset from fingerprint authority");
    overriddenAssetBackedUpstream.assets.back().contentHash =
        std::string(64, '2');
    ++overriddenAssetBackedUpstream.assets.back().generation;
    const auto changedOverrideFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            overriddenAssetBackedUpstream,
            registry(),
            "clip",
            48000,
            fingerprintError);
    require(!changedOverrideFingerprint.empty()
                && changedOverrideFingerprint != overrideFingerprint,
            "an exact Resource override asset must remain fingerprint authority");

    auto sampleMapBackedUpstream = document;
    auto& mapNode =
        sampleMapBackedUpstream.soundDefinitions.front().graph.nodes.front();
    mapNode.typeId = "talas.sampleMapPlayer";
    mapNode.parameters.clear();
    talas::project::ProjectSoundExposedParameter exposedMap;
    exposedMap.parameterId = "sourceMap";
    exposedMap.displayName = "Source map";
    exposedMap.kind = talas::core::ParameterKind::Text;
    exposedMap.defaultValue.parameterId = "sourceMap";
    exposedMap.defaultValue.text = "upstreamMap";
    exposedMap.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "osc",
        "sampleMapId",
        {},
        talas::expr::ValueDomain::Scalar,
    });
    sampleMapBackedUpstream.soundDefinitions.front()
        .exposedParameters.push_back(std::move(exposedMap));
    sampleMapBackedUpstream.sampleMapLibrary.maps.push_back({
        "upstreamMap",
        "Upstream map",
        {{"region", "upstreamSample"}},
    });
    auto mapAsset = assetBackedUpstream.assets.front();
    mapAsset.id = "upstreamSample";
    mapAsset.contentHash = std::string(64, 'd');
    sampleMapBackedUpstream.assets.push_back(std::move(mapAsset));
    const auto sampleMapFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            sampleMapBackedUpstream,
            registry(),
            "clip",
            48000,
            fingerprintError);
    require(!sampleMapFingerprint.empty(), fingerprintError);
    sampleMapBackedUpstream.assets.front().contentHash = std::string(64, 'e');
    ++sampleMapBackedUpstream.assets.front().generation;
    const auto changedSampleMapFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            sampleMapBackedUpstream,
            registry(),
            "clip",
            48000,
            fingerprintError);
    require(!changedSampleMapFingerprint.empty()
                && changedSampleMapFingerprint != sampleMapFingerprint,
            "a Text-bound exposed sample map asset must invalidate the clip fingerprint");

    auto overriddenMapBackedUpstream = sampleMapBackedUpstream;
    overriddenMapBackedUpstream.sampleMapLibrary.maps.push_back({
        "overrideMap",
        "Override map",
        {{"overrideRegion", "overrideSample"}},
    });
    auto overrideMapAsset = overriddenMapBackedUpstream.assets.front();
    overrideMapAsset.id = "overrideSample";
    overrideMapAsset.contentHash = std::string(64, '3');
    overriddenMapBackedUpstream.assets.push_back(
        std::move(overrideMapAsset));
    talas::project::ProjectParameterExpression mapOverride;
    mapOverride.parameterId = "sourceMap";
    mapOverride.text = "overrideMap";
    overriddenMapBackedUpstream.graph.nodes.front()
        .parameters.push_back(std::move(mapOverride));
    const auto mapOverrideFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            overriddenMapBackedUpstream,
            registry(),
            "clip",
            48000,
            fingerprintError);
    require(!mapOverrideFingerprint.empty(), fingerprintError);
    auto changedUnusedMap = overriddenMapBackedUpstream;
    changedUnusedMap.assets.front().contentHash = std::string(64, '4');
    ++changedUnusedMap.assets.front().generation;
    const auto unchangedMapOverrideFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            changedUnusedMap,
            registry(),
            "clip",
            48000,
            fingerprintError);
    require(unchangedMapOverrideFingerprint == mapOverrideFingerprint,
            "an exact Text-bound sample-map override must remove its unused default map asset from fingerprint authority");
    overriddenMapBackedUpstream.assets.back().contentHash =
        std::string(64, '5');
    ++overriddenMapBackedUpstream.assets.back().generation;
    const auto changedMapOverrideFingerprint =
        talas::runtime::scoreClipSourceFingerprint(
            overriddenMapBackedUpstream,
            registry(),
            "clip",
            48000,
            fingerprintError);
    require(!changedMapOverrideFingerprint.empty()
                && changedMapOverrideFingerprint
                    != mapOverrideFingerprint,
            "an exact Text-bound sample-map override asset must remain fingerprint authority");

    document.timeline.score.clips.front().sourceInstanceId.reset();
    prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);
    settings.preparedScore = prepared.schedule;
    const auto unbound = talas::runtime::materializeProjectScoreClips(
        document, registry(), settings);
    require(
        !unbound.ok()
            && unbound.error.find("requires an exact root-graph source instance")
                != std::string::npos,
        "definition-only clips must fail closed when the Sound has external boundary inputs");
}

void testSoundVariableCandidateAssetsLoadBeforeClipCompilation()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition candidate;
    candidate.definitionId = "sampleCandidate";
    candidate.displayName = "Sample candidate";
    candidate.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "player", "audioOut"});
    talas::project::ProjectNode player;
    player.instanceId = "player";
    player.typeId = "talas.diskPlayer";
    talas::project::ProjectParameterExpression file;
    file.parameterId = "fileName";
    file.resource = "candidateAudio";
    player.parameters.push_back(std::move(file));
    candidate.graph.nodes.push_back(std::move(player));
    document.soundDefinitions.push_back(std::move(candidate));

    talas::project::ProjectSoundDefinition wrapper;
    wrapper.definitionId = "variableWrapper";
    wrapper.displayName = "Variable wrapper";
    wrapper.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "selector", "audioOut"});
    talas::project::ProjectNode selector;
    selector.instanceId = "selector";
    selector.typeId = "talas.variable";
    talas::project::ProjectParameterExpression variable;
    variable.parameterId = "variableId";
    variable.text = "selected";
    selector.parameters.push_back(std::move(variable));
    wrapper.graph.nodes.push_back(std::move(selector));
    document.soundDefinitions.push_back(std::move(wrapper));

    talas::project::ProjectSoundVariableDeclaration declaration;
    declaration.variableId = "selected";
    declaration.displayName = "Selected";
    declaration.owner.kind =
        talas::project::ProjectVariableOwnerKind::SoundDefinition;
    declaration.owner.definitionId = "variableWrapper";
    declaration.candidateDefinitionIds = {"sampleCandidate"};
    document.soundVariableLibrary.variables.push_back(
        std::move(declaration));
    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::Seconds, "master", 1,
         talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds, {}, 1,
         talas::project::ScoreOverlapMode::Layer},
    };
    document.timeline.score.clips = {
        {"clip", "track", "variableWrapper", seconds(0.0),
         seconds(4.0 / 48000.0), seconds(0.0), seconds(0.0),
         seconds(0.0), 1},
    };

    const auto path = std::filesystem::temp_directory_path()
        / ("talas-score-variable-candidate-"
           + std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count())
           + ".wav");
    std::string writeError;
    const std::vector<std::vector<double>> candidateSamples{
        {0.25, -0.5, 0.75, -1.0}};
    require(talas::audio::writePcm16Wav(
                path,
                48000.0,
                candidateSamples,
                writeError),
            writeError);
    const auto hash = talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "Sound-variable candidate WAV should hash");
    talas::project::ProjectAsset asset;
    asset.id = "candidateAudio";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = path.string();
    asset.contentHash = hash.contentHash;
    asset.channelCount = 1;
    asset.sampleRate = 48000.0;
    asset.frameCount = 4;
    asset.encoding = talas::project::pcm16WavAssetEncoding;
    asset.generation = 1;
    document.assets.push_back(std::move(asset));

    const auto settings =
        talas::runtime::prepareScoreClipMaterializationSettings(
            document,
            registry(),
            std::nullopt,
            "clip",
            48000.0,
            8);
    require(settings.ok()
                && settings.settings->resourceSnapshots.size() == 1
                && settings.settings->resourceSnapshots.front()->id
                    == "candidateAudio",
            settings.error.empty()
                ? "Sound-variable candidate asset must load before clip compilation"
                : settings.error);
    const auto materialized = talas::runtime::materializeProjectScoreClip(
        document,
        registry(),
        *settings.settings,
        "clip");
    require(materialized.ok() && materialized.clips.size() == 1
                && std::ranges::any_of(
                    *materialized.clips.front()->channels.front(),
                    [](double sample) {
                        return std::abs(sample) > 1.0e-12;
                    }),
            materialized.error.empty()
                ? "Sound-variable candidate asset should render through clip materialization"
                : materialized.error);
    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);
}

void testCachedAndFrozenArtifactsUseExactByteAuthority()
{
    auto document = renderFixture();
    auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);
    std::string fingerprintError;
    const auto fingerprint = talas::runtime::scoreClipSourceFingerprint(
        document, registry(), "clip", 48000, fingerprintError);
    require(!fingerprint.empty(), fingerprintError);

    const auto path = std::filesystem::temp_directory_path()
        / ("talas-score-artifact-"
           + std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count())
           + ".wav");
    const std::vector<std::vector<double>> source{{0.125, -0.25, 0.5, -0.75}};
    std::string writeError;
    require(talas::audio::writePcm16Wav(
                path, 48000.0, source, writeError),
            writeError);
    const auto hash = talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "cached artifact WAV should hash");

    talas::project::ProjectAsset asset;
    asset.id = "score-cache-channel";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = path.string();
    asset.contentHash = hash.contentHash;
    asset.channelCount = 1;
    asset.sampleRate = 48000.0;
    asset.frameCount = 4;
    asset.encoding = "pcm16-wav";
    asset.generation = 1;
    asset.provenance = "score-cache-test";
    document.assets.push_back(asset);
    document.timeline.score.clips.front().renderArtifact =
        talas::project::ScoreClipRenderArtifact{
            talas::project::ScoreClipRenderMode::Cached,
            fingerprint,
            48000,
            4,
            {asset.id},
        };
    prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = asset.id;
    snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot.generation = asset.generation;
    snapshot.contentHash = asset.contentHash;
    snapshot.resolvedPath = path;
    snapshot.channelCount = 1;
    snapshot.sampleRate = 48000.0;
    snapshot.frameCount = 4;
    snapshot.encoding = "pcm16-wav";
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    settings.preparedScore = prepared.schedule;
    settings.resourceSnapshots.push_back(
        std::make_shared<const talas::core::ResourceSnapshot>(snapshot));
    auto cached = talas::runtime::materializeProjectScoreClips(
        document, registry(), settings);
    require(cached.ok() && cached.clips.size() == 1
                && cached.clips.front()->sourceFingerprint == fingerprint,
            cached.error);

    auto changed = document;
    changed.metadata.title = "Presentation-only change";
    auto changedPrepared = talas::runtime::prepareProjectScore(
        changed,
        talas::runtime::preparedScoreBindingForDocument(
            changed, 2, 2, 48000, 2));
    require(changedPrepared.ok(), changedPrepared.error);
    settings.preparedScore = changedPrepared.schedule;
    auto stillCached = talas::runtime::materializeProjectScoreClips(
        changed, registry(), settings);
    require(stillCached.ok()
                && stillCached.clips.front()->sourceFingerprint == fingerprint,
            "presentation-only changes must not invalidate exact clip dependencies");

    changed.soundDefinitions.front().graph.nodes.front()
        .parameters.front().expression = "2000 hz";
    changedPrepared = talas::runtime::prepareProjectScore(
        changed,
        talas::runtime::preparedScoreBindingForDocument(
            changed, 3, 3, 48000, 3));
    require(changedPrepared.ok(), changedPrepared.error);
    settings.preparedScore = changedPrepared.schedule;
    auto staleCache = talas::runtime::materializeProjectScoreClips(
        changed, registry(), settings);
    require(staleCache.ok()
                && staleCache.clips.front()->sourceFingerprint != fingerprint,
            "a cached artifact must not be reused after dependency mismatch");

    changed.timeline.score.clips.front().renderArtifact->mode =
        talas::project::ScoreClipRenderMode::Frozen;
    changedPrepared = talas::runtime::prepareProjectScore(
        changed,
        talas::runtime::preparedScoreBindingForDocument(
            changed, 4, 4, 48000, 4));
    require(changedPrepared.ok(), changedPrepared.error);
    settings.preparedScore = changedPrepared.schedule;
    auto frozen = talas::runtime::materializeProjectScoreClips(
        changed, registry(), settings);
    require(frozen.ok()
                && frozen.clips.front()->sourceFingerprint == fingerprint,
            "a frozen artifact should retain verified bytes after source changes");

    changed.soundDefinitions.front().outputs.clear();
    changedPrepared = talas::runtime::prepareProjectScore(
        changed,
        talas::runtime::preparedScoreBindingForDocument(
            changed, 5, 5, 48000, 5));
    require(changedPrepared.ok(), changedPrepared.error);
    settings.preparedScore = changedPrepared.schedule;
    frozen = talas::runtime::materializeProjectScoreClips(
        changed, registry(), settings);
    require(frozen.ok()
                && frozen.clips.front()->sourceFingerprint == fingerprint,
            "a frozen artifact should remain authoritative when its live source can no longer be materialized");
    std::error_code cleanupError;
    std::filesystem::remove(path, cleanupError);
}

void testOrdinaryDiskPlayerRemainsLiveInParentScore()
{
    auto document = renderFixture();
    auto& node = document.soundDefinitions[0].graph.nodes[0];
    node.typeId = "talas.diskPlayer"; node.parameters.clear();
    talas::project::ProjectParameterExpression resource; resource.parameterId = "fileName"; resource.resource = "source";
    node.parameters.push_back(std::move(resource));
    const auto path = std::filesystem::temp_directory_path() / ("talas-live-score-source-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
    std::string error;
    const std::vector<std::vector<double>> samples{{0.25, 0.25, 0.25, 0.25}};
    require(talas::audio::writePcm16Wav(path, 48000, samples, error), error);
    auto hash = talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "hash ordinary source WAV");
    talas::project::ProjectAsset asset; asset.id = "source"; asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = path.string(); asset.contentHash = hash.contentHash; asset.channelCount = 1;
    asset.sampleRate = 48000; asset.frameCount = 4; asset.encoding = talas::project::pcm16WavAssetEncoding; asset.generation = 1;
    document.assets.push_back(std::move(asset));
    auto configured = talas::runtime::prepareScoreClipMaterializationSettings(document, registry(), std::nullopt, "clip", 48000, 8);
    require(configured.ok(), configured.error);
    // Control: the retained source was playable before parent live integration.
    const auto materialized = talas::runtime::materializeProjectScoreClip(document, registry(), *configured.settings, "clip");
    require(materialized.ok(), materialized.error);
    auto compiled = talas::runtime::compileProject(document, registry(), *configured.settings);
    require(compiled.ok(), "compile ordinary file-backed parent Score");
    require(compiled.graph->renderSettings().preparedScoreClipAudio.empty(), "ordinary source must remain live rather than acquire implicit PCM");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), session.error());
    std::array<double, 8> output{}; double* outputs[] = {output.data()};
    require(session.render({}, outputs, 1, output.size()), "render ordinary streamed parent source");
    require(materialized.clips.size() == 1 && materialized.clips[0]->channels.size() == 1,
        "independent materializer supplies the pre-integration PCM oracle");
    const auto& oracle = *materialized.clips[0];
    const auto offline = talas::runtime::renderOffline(*compiled.graph, output.size());
    require(offline.ok(), "ordinary stream source renders through the parent offline transaction");
    for (std::size_t f = 0; f < output.size(); ++f)
    {
        const auto expected = f >= oracle.startFrame && f - oracle.startFrame < oracle.frameCount
            ? (*oracle.channels[0])[f - oracle.startFrame] : 0.0;
        require(output[f] == expected && offline.mono[f] == expected,
            "ordinary stream preserves decoded PCM and clip fades in realtime and offline");
    }

    // Real parent oracle beyond both prewarm and its prepared first-callback
    // window. The old explicit materializer remains an independent comparator.
    std::vector<std::vector<double>> longSamples{std::vector<double>(32768)};
    for (std::size_t index = 0; index < longSamples[0].size(); ++index)
        longSamples[0][index] = std::sin(double(index) * 0.017) * 0.75;
    require(talas::audio::writePcm16Wav(path, 48000, longSamples, error), error);
    hash = talas::project::calculateProjectAssetSha256(path); require(hash.ok(), "hash long source WAV");
    document.assets.back().contentHash = hash.contentHash;
    document.assets.back().frameCount = longSamples[0].size();
    node.parameters.push_back({"rateScale", "2"});
    document.timeline.score.clips[0].duration = seconds(8192.0 / 48000.0);
    auto longSettings = talas::runtime::prepareScoreClipMaterializationSettings(document, registry(), std::nullopt, "clip", 48000, 2048);
    require(longSettings.ok(), longSettings.error);
    const auto longOracle = talas::runtime::materializeProjectScoreClip(document, registry(), *longSettings.settings, "clip");
    require(longOracle.ok() && longOracle.clips.size() == 1, "materialize long independent source oracle");
    auto longCompiled = talas::runtime::compileProject(document, registry(), *longSettings.settings);
    require(longCompiled.ok() && longCompiled.graph->renderSettings().preparedScoreClipAudio.empty(), "long source retains live DSP authority");
    const auto longOffline = talas::runtime::renderOffline(*longCompiled.graph, 8194);
    require(longOffline.ok(), "long parent stream offline render succeeds");
    for (std::size_t index = 0; index < 8192; ++index)
        require(longOffline.mono[index + 2] == (*longOracle.clips[0]->channels[0])[index],
            "long parent offline stream must equal the old materializer beyond the prepared window");
}

void testFingerprintIncludesPersistedEventValueAuthority()
{
    auto document = renderFixture();
    document.soundDefinitions.front().graph.nodes.front()
        .parameters.back().expression = "!Gain";
    document.virtualControls.push_back({
        "Gain",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.25,
        0.0,
        1.0,
    });
    std::string error;
    const auto first = talas::runtime::scoreClipSourceFingerprint(
        document, registry(), "clip", 48000, error);
    require(!first.empty(), error);
    document.timeline.score.tracks.front().muted = true;
    document.timeline.score.tracks.front().solo = true;
    document.timeline.score.tracks.front().displayName = "Presentation only";
    document.soundDefinitions.front().displayName = "Renamed presentation";
    document.virtualControls.push_back({
        "Unused",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Toggle,
        1.0,
        0.0,
        1.0,
    });
    document.variableLibrary.variables.push_back({
        "unusedVariable",
        "Unused variable",
        talas::expr::ValueDomain::Scalar,
        0.25,
        {},
        true,
    });
    document.expressionDefinitions.definitions.push_back({
        "unusedFunction",
        {},
        talas::expr::ValueDomain::Scalar,
        "1.0",
    });
    document.soundVariableLibrary.variables.push_back({
        "unusedSound",
        "Unused Sound",
        {},
        talas::project::ProjectSoundVariableKind::Sound,
        {"voice"},
        0,
        true,
    });
    talas::project::ProjectSoundDefinition unusedDefinition;
    unusedDefinition.definitionId = "unusedDefinition";
    unusedDefinition.displayName = "Unused definition";
    unusedDefinition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "unusedOsc",
        "audioOut",
    });
    talas::project::ProjectNode unusedOscillator;
    unusedOscillator.instanceId = "unusedOsc";
    unusedOscillator.typeId = "talas.oscillator";
    unusedOscillator.parameters = {
        {"frequency", "123 hz"},
        {"amplitude", "!Unused"},
    };
    unusedDefinition.graph.nodes.push_back(std::move(unusedOscillator));
    document.soundDefinitions.push_back(std::move(unusedDefinition));
    talas::project::ProjectAsset unrelated;
    unrelated.id = "unrelated-asset";
    unrelated.kind = talas::project::ProjectAssetKind::Binary;
    unrelated.path = "unrelated.bin";
    unrelated.generation = 9;
    document.assets.push_back(std::move(unrelated));
    const auto presentationOnly = talas::runtime::scoreClipSourceFingerprint(
        document, registry(), "clip", 48000, error);
    require(presentationOnly == first,
            "mute, solo, labels, unused controls/variables/definitions, and unreachable assets must not invalidate materialized clip bytes");
    document.virtualControls.front().value = 0.75;
    const auto second = talas::runtime::scoreClipSourceFingerprint(
        document, registry(), "clip", 48000, error);
    require(!second.empty() && second != first,
            "persisted Event Value changes consumed by the source must invalidate a cached clip fingerprint");
}

void testScoreClipArtifactDirectoryPublicationIsAtomicAndNoReplace()
{
    auto document = renderFixture();
    // The 16-bit caches of the Audio menu.
    document.timeline.cacheWordSize = 16;
    document.timeline.score.tracks[0].channelCount = 2;
    document.timeline.score.tracks[1].channelCount = 2;
    const auto prepared = talas::runtime::prepareProjectScore(
        document,
        talas::runtime::preparedScoreBindingForDocument(
            document, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    settings.preparedScore = prepared.schedule;

    const auto root = std::filesystem::temp_directory_path()
        / ("talas-score-publication-"
           + std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count()));
    const auto destination = root / "artifact";
    talas::runtime::ScoreClipArtifactPublicationOptions options;
    options.clipId = "clip";
    options.mode = talas::project::ScoreClipRenderMode::Frozen;
    options.destinationDirectory = destination;
    options.storedPathPrefix = "Assets/ScoreClips/artifact";
    options.assetIdPrefix = "score-clip-artifact";
    options.admitPublication = [] { return true; };
    const auto published = talas::runtime::publishScoreClipArtifact(
        document, registry(), settings, options);
    require(published.ok() && published.assets.size() == 2
                && published.artifact.channelAssetIds.size() == 2
                && published.artifact.mode
                    == talas::project::ScoreClipRenderMode::Frozen
                && std::filesystem::is_regular_file(
                    destination / "channel-1.wav")
                && std::filesystem::is_regular_file(
                    destination / "channel-2.wav"),
            published.error);
    require(std::ranges::all_of(
                published.assets,
                [](const auto& asset) {
                    return asset.kind == talas::project::ProjectAssetKind::Audio
                        && asset.channelCount == 1
                        && asset.sampleRate == 48000.0
                        && asset.frameCount == 4
                        && asset.encoding == talas::project::pcm16WavAssetEncoding
                        && talas::project::isCanonicalProjectAssetContentHash(
                            asset.contentHash);
                }),
            "every adopted channel should retain exact verified PCM16 authority");

    auto refreshDocument = document;
    refreshDocument.timeline.score.clips.front().renderArtifact =
        published.artifact;
    refreshDocument.assets = published.assets;
    auto refreshOptions = options;
    refreshOptions.destinationDirectory = root / "refreshed";
    refreshOptions.storedPathPrefix = "Assets/ScoreClips/refreshed";
    refreshOptions.assetIdPrefix = "score-clip-refreshed";
    const auto refreshed = talas::runtime::publishScoreClipArtifact(
        refreshDocument, registry(), settings, std::move(refreshOptions));
    require(refreshed.ok()
                && refreshed.artifact.sourceFingerprint
                    == published.artifact.sourceFingerprint
                && std::filesystem::is_regular_file(
                    root / "refreshed" / "channel-2.wav"),
            refreshed.error.empty()
                ? "refresh must render the live source without requiring the prior artifact snapshot"
                : refreshed.error);

    const auto collision = talas::runtime::publishScoreClipArtifact(
        document, registry(), settings, options);
    require(!collision.ok()
                && collision.error.find("already exists") != std::string::npos
                && std::filesystem::is_regular_file(
                    destination / "channel-1.wav")
                && std::filesystem::is_regular_file(
                    destination / "channel-2.wav"),
            "no-replace publication should preserve the complete prior directory");

    auto invalidatedOptions = options;
    invalidatedOptions.destinationDirectory = root / "invalidated";
    invalidatedOptions.storedPathPrefix = "Assets/ScoreClips/invalidated";
    invalidatedOptions.assetIdPrefix = "score-clip-invalidated";
    invalidatedOptions.admitPublication = [] { return false; };
    const auto invalidated = talas::runtime::publishScoreClipArtifact(
        document, registry(), settings, std::move(invalidatedOptions));
    require(!invalidated.ok() && invalidated.invalidated
                && invalidated.assets.empty()
                && !std::filesystem::exists(root / "invalidated"),
            "authority invalidation should publish no channel directory or manifests");

    auto cancelledOptions = options;
    cancelledOptions.destinationDirectory = root / "cancelled";
    cancelledOptions.storedPathPrefix = "Assets/ScoreClips/cancelled";
    cancelledOptions.assetIdPrefix = "score-clip-cancelled";
    cancelledOptions.shouldCancel = [] { return true; };
    const auto cancelled = talas::runtime::publishScoreClipArtifact(
        document, registry(), settings, std::move(cancelledOptions));
    require(!cancelled.ok() && cancelled.cancelled
                && !std::filesystem::exists(root / "cancelled"),
            "pre-render cancellation should leave no artifact directory");

    for (const auto channelCount : {1U, 4U, 8U})
    {
        auto multichannelDocument = renderFixture();
        multichannelDocument.timeline.score.tracks[0].channelCount =
            channelCount;
        multichannelDocument.timeline.score.tracks[1].channelCount =
            channelCount;
        const auto multichannelPrepared = talas::runtime::prepareProjectScore(
            multichannelDocument,
            talas::runtime::preparedScoreBindingForDocument(
                multichannelDocument, 1, 1, 48000, 1));
        require(multichannelPrepared.ok(), multichannelPrepared.error);
        auto multichannelSettings = settings;
        multichannelSettings.preparedScore = multichannelPrepared.schedule;
        auto multichannelOptions = options;
        multichannelOptions.destinationDirectory =
            root / ("artifact-" + std::to_string(channelCount));
        multichannelOptions.storedPathPrefix =
            "Assets/ScoreClips/artifact-" + std::to_string(channelCount);
        multichannelOptions.assetIdPrefix =
            "score-clip-" + std::to_string(channelCount);
        const auto multichannel = talas::runtime::publishScoreClipArtifact(
            multichannelDocument,
            registry(),
            multichannelSettings,
            std::move(multichannelOptions));
        require(
            multichannel.ok()
                && multichannel.assets.size() == channelCount
                && multichannel.artifact.channelAssetIds.size()
                    == channelCount
                && std::filesystem::is_regular_file(
                    multichannel.publishedDirectory
                        / ("channel-" + std::to_string(channelCount)
                           + ".wav")),
            multichannel.error.empty()
                ? "mono/four/eight-channel publication must adopt every exact channel"
                : multichannel.error);
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}

// By default a Timeline caches a Sound as mono
// 24-bit recordings at the current sample rate, each channel its own WAV;
// the Audio menu may set 16 bits.
void testATimelinesCachesAreMonoTwentyFourBitRecordings()
{
    auto document = renderFixture();
    require(document.timeline.cacheWordSize == 24, "a Timeline should cache in 24 bits by default");
    const auto prepared = talas::runtime::prepareProjectScore(
        document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, 48000, 1));
    require(prepared.ok(), prepared.error);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 8;
    settings.preparedScore = prepared.schedule;
    const auto root = std::filesystem::temp_directory_path()
        / ("talas-score-cache-format-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto publish = [&](const talas::project::ProjectDocument& source, const std::string& name) {
        talas::runtime::ScoreClipArtifactPublicationOptions options;
        options.clipId = "clip";
        options.mode = talas::project::ScoreClipRenderMode::Cached;
        options.destinationDirectory = root / name;
        options.storedPathPrefix = "Assets/ScoreClips/" + name;
        options.assetIdPrefix = "score-clip-" + name;
        options.admitPublication = [] { return true; };
        return talas::runtime::publishScoreClipArtifact(source, registry(), settings, options);
    };
    const auto bitsOf = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::array<unsigned char, 36> header{};
        file.read(reinterpret_cast<char*>(header.data()), header.size());
        return file ? header[34] | (header[35] << 8) : 0;
    };
    const auto published = publish(document, "cache");
    require(published.ok() && published.assets.size() == 1, published.error);
    const auto& asset = published.assets.front();
    const auto file = root / "cache" / "channel-1.wav";
    require(asset.channelCount == 1 && asset.sampleRate == 48000.0
            && asset.encoding == talas::project::sampleFileAssetEncoding && bitsOf(file) == 24,
        "the cache should be a mono 24-bit WAV at the current sample rate");

    // The Timeline plays the cache in place of its Sound.
    auto cachedDocument = document;
    cachedDocument.assets.push_back(asset);
    cachedDocument.timeline.score.clips.front().renderArtifact = published.artifact;
    const auto cachedPrepared = talas::runtime::prepareProjectScore(
        cachedDocument, talas::runtime::preparedScoreBindingForDocument(cachedDocument, 2, 2, 48000, 2));
    require(cachedPrepared.ok(), cachedPrepared.error);
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = asset.id;
    snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot.generation = asset.generation;
    snapshot.contentHash = asset.contentHash;
    snapshot.resolvedPath = file;
    snapshot.channelCount = 1;
    snapshot.sampleRate = 48000.0;
    snapshot.frameCount = asset.frameCount;
    snapshot.encoding = asset.encoding;
    auto cachedSettings = settings;
    cachedSettings.preparedScore = cachedPrepared.schedule;
    cachedSettings.resourceSnapshots.push_back(std::make_shared<const talas::core::ResourceSnapshot>(snapshot));
    const auto cached = talas::runtime::materializeProjectScoreClip(cachedDocument, registry(), cachedSettings, "clip");
    talas::runtime::ScoreClipMaterializationOptions live;
    live.forceLiveSource = true;
    const auto heard = talas::runtime::materializeProjectScoreClip(document, registry(), settings, "clip", live);
    require(cached.ok() && heard.ok() && cached.clips.size() == 1 && heard.clips.size() == 1,
        cached.error.empty() ? heard.error : cached.error);
    const auto& cachedSamples = *cached.clips.front()->channels.front();
    const auto& heardSamples = *heard.clips.front()->channels.front();
    double largest = 0.0;
    double error = 0.0;
    for (std::size_t frame = 0; frame < heardSamples.size() && frame < cachedSamples.size(); ++frame)
    {
        largest = std::max(largest, std::abs(heardSamples[frame]));
        error = std::max(error, std::abs(heardSamples[frame] - cachedSamples[frame]));
    }
    require(cachedSamples.size() == heardSamples.size() && largest > 0.0 && error < 1.0 / 4'000'000.0,
        "the 24-bit cache should play the Sound to 24 bits");

    // The Audio menu's 16 bits.
    auto sixteen = document;
    sixteen.timeline.cacheWordSize = 16;
    const auto shorter = publish(sixteen, "cache16");
    require(shorter.ok() && shorter.assets.size() == 1
            && shorter.assets.front().encoding == talas::project::pcm16WavAssetEncoding
            && bitsOf(root / "cache16" / "channel-1.wav") == 16,
        "a Timeline set to 16 bits should cache 16-bit WAVs");
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
}
}

talas::project::ProjectDocument sharedMemoryWriterScoreFixture(bool sharing)
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition writerSound;
    writerSound.definitionId = "writerSound";
    writerSound.displayName = "Writer";
    writerSound.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "writer", "audioOut"});
    talas::project::ProjectNode source;
    source.instanceId = "source";
    source.typeId = "talas.sample";
    source.parameters.push_back({"frequency", "4 c"});
    talas::project::ProjectParameterExpression pulse;
    pulse.parameterId = "sample";
    pulse.resource = "Source/Pulse";
    source.parameters.push_back(std::move(pulse));
    writerSound.graph.nodes.push_back(std::move(source));
    talas::project::ProjectNode writer;
    writer.instanceId = "writer";
    writer.typeId = "talas.memoryWriter";
    writer.parameters.push_back({"captureDuration", "1 s"});
    talas::project::ProjectParameterExpression recordingName;
    recordingName.parameterId = "recordingName";
    recordingName.resource = "Memory/Capture";
    writer.parameters.push_back(std::move(recordingName));
    talas::project::ProjectParameterExpression silent;
    silent.parameterId = "silent";
    silent.boolean = true;
    writer.parameters.push_back(std::move(silent));
    writerSound.graph.nodes.push_back(std::move(writer));
    writerSound.graph.connections.push_back({
        "source", "audioOut", "writer", "audioIn"});
    document.soundDefinitions.push_back(std::move(writerSound));

    talas::project::ProjectSoundDefinition readerSound;
    readerSound.definitionId = "readerSound";
    readerSound.displayName = "Reader";
    readerSound.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "reader", "audioOut"});
    talas::project::ProjectNode reader;
    reader.instanceId = "reader";
    reader.typeId = "talas.sample";
    reader.parameters.push_back({"frequency", "4 c"});
    talas::project::ProjectParameterExpression sampleName;
    sampleName.parameterId = "sample";
    sampleName.resource = "Memory/Capture";
    reader.parameters.push_back(std::move(sampleName));
    talas::project::ProjectParameterExpression fromWriter;
    fromWriter.parameterId = "fromMemoryWriter";
    fromWriter.boolean = true;
    reader.parameters.push_back(std::move(fromWriter));
    readerSound.graph.nodes.push_back(std::move(reader));
    document.soundDefinitions.push_back(std::move(readerSound));

    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::Seconds, "master", 1,
         talas::project::ScoreOverlapMode::Reject},
        {"master", "Master", talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds, {}, 1,
         talas::project::ScoreOverlapMode::Layer},
    };
    document.timeline.score.tracks.front().sharingMemoryWriters = sharing;
    // Reverse document order to verify that materialization follows Timeline time.
    document.timeline.score.clips = {
        {"readerClip", "track", "readerSound", seconds(1),
         seconds(1), seconds(0), seconds(0), seconds(0), 1},
        {"writerClip", "track", "writerSound", seconds(0),
         seconds(1), seconds(0), seconds(0), seconds(0), 1},
    };
    return document;
}

// A sample whose loop is set, lengthened in the
// Timeline past its own duration, loops to fill out the additional time.
void testALoopedSampleFillsALengthenedClip()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition looped;
    looped.definitionId = "looped";
    looped.displayName = "Looped";
    looped.outputs.push_back({"audioOut", talas::core::PortType::Audio, "sample", "audioOut"});
    talas::project::ProjectNode sample;
    sample.instanceId = "sample";
    sample.typeId = "talas.sample";
    sample.parameters.push_back({"frequency", "4 c"});
    talas::project::ProjectParameterExpression file;
    file.parameterId = "sample";
    file.resource = "Source/Pulse";
    sample.parameters.push_back(std::move(file));
    talas::project::ProjectParameterExpression setLoop;
    setLoop.parameterId = "setLoop";
    setLoop.boolean = true;
    sample.parameters.push_back(std::move(setLoop));
    looped.graph.nodes.push_back(std::move(sample));
    document.soundDefinitions.push_back(std::move(looped));
    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::Seconds, "master", 1,
         talas::project::ScoreOverlapMode::Reject},
        {"master", "Master", talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds, {}, 1,
         talas::project::ScoreOverlapMode::Layer},
    };
    // The sample lasts 1 s; its clip is lengthened to 3 s.
    document.timeline.score.clips = {
        {"loopClip", "track", "looped", seconds(0), seconds(3), seconds(0), seconds(0), seconds(0), 1}};
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 4;
    settings.maxBlockFrames = 8;
    settings.tableResources = {{"Source/Pulse", {1, 2, 3, 4}}};
    const auto prepared = talas::runtime::prepareProjectScore(
        document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, 4, 1));
    require(prepared.ok(), prepared.error);
    settings.preparedScore = prepared.schedule;
    const auto materialized = talas::runtime::materializeProjectScoreClips(document, registry(), settings);
    require(materialized.ok() && materialized.clips.size() == 1,
        materialized.error.empty() ? "the lengthened clip should materialize" : materialized.error);
    const auto& audio = *materialized.clips.front()->channels.front();
    std::string played;
    for (const auto value : audio)
        played += std::to_string(value) + " ";
    require(audio.size() == 12, "the clip should last its 3 s: " + played);
    for (std::size_t frame = 0; frame < audio.size(); ++frame)
    {
        require(std::abs(audio[frame] - static_cast<double>(frame % 4 + 1)) < 1e-9,
            "the looped sample should fill out the lengthened clip: " + played);
    }
}

void testLaterScoreSoundReadsOnlySharedMemoryWriterRecording()
{
    const auto makeSettings = [](const talas::project::ProjectDocument& document) {
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 4;
        settings.maxBlockFrames = 8;
        settings.tableResources = {
            {"Source/Pulse", {1, 2, 3, 4}},
            {"Memory/Capture", {99, 99, 99, 99}},
        };
        const auto prepared = talas::runtime::prepareProjectScore(
            document,
            talas::runtime::preparedScoreBindingForDocument(
                document, 1, 1, 4, 1));
        require(prepared.ok(), prepared.error);
        settings.preparedScore = prepared.schedule;
        return settings;
    };
    const auto shared = sharedMemoryWriterScoreFixture(true);
    const auto settings = makeSettings(shared);
    const auto materialized = talas::runtime::materializeProjectScoreClips(
        shared, registry(), settings);
    require(materialized.ok() && materialized.clips.size() == 2,
        materialized.error.empty() ? "shared Score clip count" : materialized.error);
    require(materialized.clips[0]->clipId == "readerClip"
            && materialized.clips[1]->clipId == "writerClip",
        "Score materialization must retain document result order");
    const auto& readAudio = *materialized.clips[0]->channels.front();
    const auto& writerAudio = *materialized.clips[1]->channels.front();
    require(readAudio.size() == 4 && writerAudio.size() == 4,
        "later Score reader must retain its exact four-frame clip");
    for (std::size_t frame = 0; frame < 4; ++frame)
    {
        require(std::abs(readAudio[frame] - static_cast<double>(frame + 1))
                    < 1e-12,
            "later Score Sound must play the earlier named recording");
        require(std::abs(writerAudio[frame]) < 1e-12,
            "silent MemoryWriter should not emit monitor audio");
    }
    const auto requested = talas::runtime::materializeProjectScoreClip(
        shared, registry(), settings, "readerClip");
    require(requested.ok() && requested.clips.size() == 1
            && requested.clips.front()->clipId == "readerClip"
            && *requested.clips.front()->channels.front() == readAudio,
        "single-clip materialization must first process its earlier writer");

    const auto compiled = talas::runtime::compileProject(
        shared, registry(), settings);
    require(compiled.ok(),
        compiled.diagnostics.empty()
            ? "public Score graph should compile with shared memory"
            : compiled.diagnostics.front().message);
    require(compiled.graph->renderSettings().preparedScoreClipAudio.size() == 2,
        "sharing track must publish bounded stage-owned clip audio");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 8);
    std::string renderDetail = "public Score graph should render both clip intervals";
    renderDetail += " (mono=" + std::to_string(rendered.mono.size()) + ")";
    for (const auto& diagnostic : rendered.diagnostics)
        renderDetail += " | " + diagnostic.message;
    require(rendered.ok() && rendered.mono.size() == 8,
        renderDetail);
    for (std::size_t frame = 0; frame < 8; ++frame)
    {
        const auto expected = frame < 4
            ? 0.0 : static_cast<double>(frame - 3);
        require(std::abs(rendered.mono[frame] - expected) < 1e-12,
            "public Timeline must play the later Sound's shared recording");
    }
    talas::runtime::RealtimeRenderSession live{*compiled.graph};
    require(live.ready(), live.error());
    std::array<double, 8> liveAudio{};
    double* liveOutputs[] = {liveAudio.data()};
    require(live.render({}, liveOutputs, 1, liveAudio.size()),
        "prepared realtime Timeline must play shared clip audio");
    for (std::size_t frame = 0; frame < liveAudio.size(); ++frame)
        require(std::abs(liveAudio[frame] - rendered.mono[frame]) < 1e-12,
            "prepared realtime and offline shared clip playback must agree");

    auto mixed = sharedMemoryWriterScoreFixture(true);
    talas::project::ProjectSoundDefinition ordinarySound;
    ordinarySound.definitionId = "ordinarySound";
    ordinarySound.displayName = "Ordinary";
    ordinarySound.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    talas::project::ProjectNode oscillator;
    oscillator.instanceId = "osc";
    oscillator.typeId = "talas.oscillator";
    oscillator.parameters = {
        {"frequency", "1 hz"}, {"amplitude", "1.0"}};
    ordinarySound.graph.nodes.push_back(std::move(oscillator));
    mixed.soundDefinitions.push_back(std::move(ordinarySound));
    mixed.timeline.score.clips.push_back({
        "ordinaryClip", "track", "ordinarySound", seconds(2),
        seconds(1), seconds(0), seconds(0), seconds(0), 1});
    const auto mixedCompiled = talas::runtime::compileProject(
        mixed, registry(), makeSettings(mixed));
    require(mixedCompiled.ok(),
        mixedCompiled.diagnostics.empty()
            ? "sharing track with an ordinary live Sound should compile"
            : mixedCompiled.diagnostics.front().message);
    require(mixedCompiled.graph->renderSettings().preparedScoreClipAudio.size() == 2,
        "ordinary Sound on sharing track must remain a live clip source");
    const auto mixedRendered = talas::runtime::renderOffline(*mixedCompiled.graph, 12);
    require(mixedRendered.ok() && mixedRendered.mono.size() == 12,
        "sharing track must render an ordinary live Sound after the reader");
    require(std::abs(mixedRendered.mono[9]) > 0.5,
        "ordinary live Sound must remain audible after shared recording clips");

    auto incomplete = sharedMemoryWriterScoreFixture(true);
    auto& captureDuration = incomplete.soundDefinitions.front()
        .graph.nodes[1].parameters.front();
    require(captureDuration.parameterId == "captureDuration",
        "incomplete writer fixture must retain the capture duration parameter");
    captureDuration.expression = "2 s";
    const auto incompleteResult = talas::runtime::materializeProjectScoreClips(
        incomplete, registry(), makeSettings(incomplete));
    require(!incompleteResult.ok() && incompleteResult.clips.empty(),
        "unfinished MemoryWriter capture must not publish zero-filled future frames");

    const auto privateDocument = sharedMemoryWriterScoreFixture(false);
    const auto privateResult = talas::runtime::materializeProjectScoreClips(
        privateDocument, registry(), makeSettings(privateDocument));
    require(!privateResult.ok() && privateResult.clips.empty(),
        "different Sounds on the same private track must not share named memory");

    auto crossTrack = sharedMemoryWriterScoreFixture(true);
    crossTrack.timeline.score.tracks.insert(
        crossTrack.timeline.score.tracks.begin() + 1,
        {"other", "Other", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::Seconds, "master", 1,
         talas::project::ScoreOverlapMode::Reject});
    crossTrack.timeline.score.tracks[1].sharingMemoryWriters = true;
    crossTrack.timeline.score.clips.front().trackId = "other";
    const auto crossTrackResult = talas::runtime::materializeProjectScoreClips(
        crossTrack, registry(), makeSettings(crossTrack));
    require(!crossTrackResult.ok() && crossTrackResult.clips.empty(),
        "named recording must not leak between sharing-enabled tracks");
}

void testLaterScoreSampleCloudReadsSharedMemoryWriterRecording()
{
    auto document = sharedMemoryWriterScoreFixture(true);
    auto& reader = document.soundDefinitions[1].graph.nodes.front();
    reader.typeId = "talas.sampleCloud";
    talas::project::ProjectParameterExpression sampleName;
    sampleName.parameterId = "sample";
    sampleName.resource = "Memory/Capture";
    talas::project::ProjectParameterExpression envelopeName;
    envelopeName.parameterId = "grainEnv";
    envelopeName.resource = "Envelopes/Flat";
    talas::project::ProjectParameterExpression fromWriter;
    fromWriter.parameterId = "fromMemoryWriter";
    fromWriter.boolean = true;
    reader.parameters = {
        std::move(sampleName), std::move(envelopeName),
        {"maxGrains", "1"}, {"amplitude", "1"}, {"density", "1"},
        {"grainDur", "1 s"}, {"grainDurJitter", "0"},
        {"pan", "0.5"}, {"panJitter", "0"}, {"seed", "0"},
        std::move(fromWriter), {"timeIndex", "-1"},
        {"timeIndexJitter", "0"},
    };
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 4;
    settings.maxBlockFrames = 8;
    settings.tableResources = {
        {"Source/Pulse", {1, 2, 3, 4}},
        {"Memory/Capture", {99, 99, 99, 99}},
        {"Envelopes/Flat", {1}},
    };
    const auto prepared = talas::runtime::prepareProjectScore(
        document, talas::runtime::preparedScoreBindingForDocument(
            document, 1, 1, 4, 1));
    require(prepared.ok(), prepared.error);
    settings.preparedScore = prepared.schedule;
    const auto materialized = talas::runtime::materializeProjectScoreClips(
        document, registry(), settings);
    require(materialized.ok() && materialized.clips.size() == 2,
        materialized.error.empty()
            ? "SampleCloud sharing materialization failed"
            : materialized.error);
    const auto& samples = *materialized.clips.front()->channels.front();
    require(materialized.clips.front()->clipId == "readerClip"
            && samples.size() == 4,
        "later SampleCloud must retain the named recording's exact clip");
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
        require(std::abs(samples[frame] - static_cast<double>(frame + 1)) < 1e-12,
            "SampleCloud must read MemoryWriter samples ahead of an immutable same-name decoy");

    const auto compiled = talas::runtime::compileProject(
        document, registry(), settings);
    require(compiled.ok(), compiled.diagnostics.empty()
        ? "public SampleCloud sharing graph should compile"
        : compiled.diagnostics.front().message);
    require(compiled.graph->renderSettings().preparedScoreClipAudio.size() == 2,
        "public Score must prepare both sharing MemoryWriter and SampleCloud clips");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    require(offline.ok() && offline.mono.size() == 8,
        "public Score must play the later SampleCloud clip");
    for (std::size_t frame = 0; frame < offline.mono.size(); ++frame)
    {
        const auto expected = frame < 4 ? 0.0
            : static_cast<double>(frame - 3);
        require(std::abs(offline.mono[frame] - expected) < 1e-12,
            "public Score SampleCloud must play only the earlier recording");
    }
    talas::runtime::RealtimeRenderSession realtime{*compiled.graph};
    require(realtime.ready(), realtime.error());
    std::array<double, 8> output{};
    double* channels[] = {output.data()};
    require(realtime.render({}, channels, 1, output.size()),
        "prepared realtime Score must play the later SampleCloud clip");
    for (std::size_t frame = 0; frame < output.size(); ++frame)
        require(std::abs(output[frame] - offline.mono[frame]) < 1e-12,
            "prepared realtime and offline SampleCloud playback must agree");
}

// Besides feeding both into a Mixer, a reader and
// its MemoryWriter can be scheduled at the same time by putting them in
// separate tracks of a Timeline, with one start time and duration. That
// takes the Timeline's sharingMemoryWritersAcrossTracks (format 156): the
// clips then render together, the writer first in each block, and the
// reader plays what the writer records as it records it, as the Mixer
// does. Off, as in older projects, the reader on its own track finds no
// recording.
void testAReaderAndItsWriterOnSeparateTracksPlayTogether()
{
    // An octave up the reader runs ahead of the writer: its second frame
    // reads where nothing has been recorded yet.
    const auto timeline = [](bool acrossTracks, std::string_view readerPitch) {
        auto document = sharedMemoryWriterScoreFixture(false);
        document.soundDefinitions[1].graph.nodes.front().parameters.front().expression = std::string{readerPitch};
        document.timeline.score.tracks.insert(document.timeline.score.tracks.begin() + 1,
            {"readerTrack", "Reader", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds,
                "master", 1, talas::project::ScoreOverlapMode::Reject});
        document.timeline.score.clips = {
            {"readerClip", "readerTrack", "readerSound", seconds(0), seconds(1), seconds(0), seconds(0), seconds(0), 1},
            {"writerClip", "track", "writerSound", seconds(0), seconds(1), seconds(0), seconds(0), seconds(0), 1},
        };
        document.timeline.sharingMemoryWritersAcrossTracks = acrossTracks;
        return document;
    };
    const auto settingsFor = [](const talas::project::ProjectDocument& document) {
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 4;
        settings.maxBlockFrames = 2;
        settings.tableResources = {{"Source/Pulse", {1, 2, 3, 4}}, {"Memory/Capture", {99, 99, 99, 99}}};
        const auto prepared = talas::runtime::prepareProjectScore(
            document, talas::runtime::preparedScoreBindingForDocument(document, 1, 1, 4, 1));
        require(prepared.ok(), prepared.error);
        settings.preparedScore = prepared.schedule;
        return settings;
    };
    // The same writer and reader fed into a Mixer.
    const auto mixed = [&](std::string_view readerPitch) {
        auto document = sharedMemoryWriterScoreFixture(false);
        auto graph = document.soundDefinitions[0].graph;
        auto reader = document.soundDefinitions[1].graph.nodes.front();
        reader.parameters.front().expression = std::string{readerPitch};
        graph.nodes.push_back(std::move(reader));
        talas::project::ProjectNode mixer;
        mixer.instanceId = "mixer";
        mixer.typeId = "talas.mixer";
        graph.nodes.push_back(std::move(mixer));
        talas::project::ProjectNode output;
        output.instanceId = "out";
        output.typeId = "talas.audioOutput";
        graph.nodes.push_back(std::move(output));
        graph.connections.push_back({"writer", "audioOut", "mixer", "in1"});
        graph.connections.push_back({"reader", "audioOut", "mixer", "in2"});
        graph.connections.push_back({"mixer", "audioOut", "out", "audioIn"});
        talas::project::ProjectDocument plain;
        plain.graph = std::move(graph);
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 4;
        settings.maxBlockFrames = 2;
        settings.tableResources = {{"Source/Pulse", {1, 2, 3, 4}}, {"Memory/Capture", {99, 99, 99, 99}}};
        const auto compiled = talas::runtime::compileProject(plain, registry(), settings);
        require(compiled.ok(), compiled.diagnostics.empty() ? "the Mixer should compile"
                                                            : compiled.diagnostics.front().message);
        const auto rendered = talas::runtime::renderOffline(*compiled.graph, 4);
        require(rendered.ok() && rendered.mono.size() == 4, "the Mixer should render");
        return rendered.mono;
    };

    for (const auto pitch : {std::string_view{"4 c"}, std::string_view{"5 c"}})
    {
        const auto document = timeline(true, pitch);
        const auto settings = settingsFor(document);
        const auto materialized = talas::runtime::materializeProjectScoreClips(document, registry(), settings);
        require(materialized.ok() && materialized.clips.size() == 2,
            materialized.error.empty() ? "both clips should materialize" : materialized.error);
        require(materialized.clips[0]->clipId == "readerClip", "results keep the document's order");
        const auto& reader = *materialized.clips[0]->channels.front();
        const auto reference = mixed(pitch);
        std::string played;
        for (const auto value : reader)
            played += std::to_string(value) + " ";
        for (std::size_t frame = 0; frame < 4; ++frame)
            require(std::abs(reader[frame] - reference[frame]) < 1e-12,
                "the reader on its own track should play as it does in a Mixer with the writer: "
                    + played);
        if (pitch == "4 c")
        {
            for (std::size_t frame = 0; frame < 4; ++frame)
                require(std::abs(reader[frame] - static_cast<double>(frame + 1)) < 1e-12,
                    "the reader should play the recording as the writer makes it: " + played);
        }
        else
        {
            require(std::abs(reader[0] - 1.0) < 1e-12 && std::abs(reader[1]) < 1e-12,
                "at the same time, a reader ahead of the writer finds nothing recorded yet: "
                    + played);
        }

        // The Timeline as it plays, offline and in realtime.
        const auto compiled = talas::runtime::compileProject(document, registry(), settings);
        require(compiled.ok(), compiled.diagnostics.empty() ? "the Timeline should compile"
                                                            : compiled.diagnostics.front().message);
        const auto rendered = talas::runtime::renderOffline(*compiled.graph, 4);
        require(rendered.ok() && rendered.mono.size() == 4, "the Timeline should render");
        for (std::size_t frame = 0; frame < 4; ++frame)
            require(std::abs(rendered.mono[frame] - reference[frame]) < 1e-12,
                "the Timeline should sound as the Mixer does");
        talas::runtime::RealtimeRenderSession live{*compiled.graph};
        require(live.ready(), live.error());
        std::array<double, 4> liveAudio{};
        // Blocks of the two frames the settings allow.
        for (std::size_t offset = 0; offset < liveAudio.size(); offset += 2)
        {
            double* liveOutputs[] = {liveAudio.data() + offset};
            require(live.render({}, liveOutputs, 1, 2), "the Timeline should play in realtime");
        }
        for (std::size_t frame = 0; frame < liveAudio.size(); ++frame)
            require(std::abs(liveAudio[frame] - rendered.mono[frame]) < 1e-12,
                "the Timeline should play in realtime as offline");
    }

    // A clip after them on either track reads the finished recording.
    auto later = timeline(true, "4 c");
    later.timeline.score.clips.push_back(
        {"laterClip", "readerTrack", "readerSound", seconds(1), seconds(1), seconds(0), seconds(0), seconds(0), 1});
    const auto laterResult = talas::runtime::materializeProjectScoreClip(later, registry(), settingsFor(later), "laterClip");
    require(laterResult.ok() && laterResult.clips.size() == 1
            && *laterResult.clips.front()->channels.front() == std::vector<double>({1, 2, 3, 4}),
        laterResult.error.empty() ? "a later clip should read the finished recording" : laterResult.error);

    const auto apart = timeline(false, "4 c");
    const auto apartResult = talas::runtime::materializeProjectScoreClips(apart, registry(), settingsFor(apart));
    require(!apartResult.ok() && apartResult.clips.empty(),
        "without the setting, as before format 156, the reader on another track finds no recording");
}

int main()
{
    bool failed = false;
    for (const auto& [name, test] : std::array{
        std::pair{"testPreparationResolvesExactFramesAndBindings", &testPreparationResolvesExactFramesAndBindings},
        std::pair{"testScheduleOrderingAndSeekArePartitionIndependent", &testScheduleOrderingAndSeekArePartitionIndependent},
        std::pair{"testReplaceOverlapPublishesExactReactivationBoundaries", &testReplaceOverlapPublishesExactReactivationBoundaries},
        std::pair{"testPreparationRejectsStaleOrUnboundAuthority", &testPreparationRejectsStaleOrUnboundAuthority},
        std::pair{"testPreparedScheduleIsRetainedByOfflineAndRealtimeEngines", &testPreparedScheduleIsRetainedByOfflineAndRealtimeEngines},
        std::pair{"testBridgePublishesScoreFailureAtomicallyWithItsGraphStage", &testBridgePublishesScoreFailureAtomicallyWithItsGraphStage},
        std::pair{"testTrackAndMasterAutomationReachExactLiveClipSource", &testTrackAndMasterAutomationReachExactLiveClipSource},
        std::pair{"testGraphProjectionFailureLeavesRenderSettingsUntouched", &testGraphProjectionFailureLeavesRenderSettingsUntouched},
        std::pair{"testLiveClipDoesNotCollideWithExistingEventAuthority", &testLiveClipDoesNotCollideWithExistingEventAuthority},
        std::pair{"testScoreSoundFixtureProducesAudioWithoutProjection", &testScoreSoundFixtureProducesAudioWithoutProjection},
        std::pair{"testSixChannelScoreMasterUsesDirectTerminal", &testSixChannelScoreMasterUsesDirectTerminal},
        std::pair{"testScoreGateTimelineWorksOnTheProjectedGraphShape", &testScoreGateTimelineWorksOnTheProjectedGraphShape},
        std::pair{"testScoreGraphRendersExactClipFadesAcrossCallbackPartitions", &testScoreGraphRendersExactClipFadesAcrossCallbackPartitions},
        std::pair{"testScoreGraphRelocatesAndLoopsAtExactCallbackBoundaries", &testScoreGraphRelocatesAndLoopsAtExactCallbackBoundaries},
        std::pair{"testLiveClipRejectsCallerPcmAndIsExactOfflineRealtimeAndOnSeek", &testLiveClipRejectsCallerPcmAndIsExactOfflineRealtimeAndOnSeek},
        std::pair{"testPublicCompilePreparesAllLiveSourcesAndDiscardsCallerPcm", &testPublicCompilePreparesAllLiveSourcesAndDiscardsCallerPcm},
        std::pair{"testLiveMaterializationAppliesContentOffsetAndRestartsRepeats", &testLiveMaterializationAppliesContentOffsetAndRestartsRepeats},
        std::pair{"testLaterScoreSoundReadsOnlySharedMemoryWriterRecording", &testLaterScoreSoundReadsOnlySharedMemoryWriterRecording},
        std::pair{"testALoopedSampleFillsALengthenedClip", &testALoopedSampleFillsALengthenedClip},
        std::pair{"testLaterScoreSampleCloudReadsSharedMemoryWriterRecording", &testLaterScoreSampleCloudReadsSharedMemoryWriterRecording},
        std::pair{"testAReaderAndItsWriterOnSeparateTracksPlayTogether", &testAReaderAndItsWriterOnSeparateTracksPlayTogether},
        std::pair{"testLiveMaterializationRetainsLegacyTimelineEventValueAuthority", &testLiveMaterializationRetainsLegacyTimelineEventValueAuthority},
        std::pair{"testMaterializationSettingsIgnoreProjectTransportLoop", &testMaterializationSettingsIgnoreProjectTransportLoop},
        std::pair{"testLiveMaterializationRejectsUnboundedSourcePreroll", &testLiveMaterializationRejectsUnboundedSourcePreroll},
        std::pair{"testLiveMaterializationCancellationIsCheckedBetweenRenderBlocks", &testLiveMaterializationCancellationIsCheckedBetweenRenderBlocks},
        std::pair{"testLiveMaterializationUsesExactConnectedSoundInstance", &testLiveMaterializationUsesExactConnectedSoundInstance},
        std::pair{"testSoundVariableCandidateAssetsLoadBeforeClipCompilation", &testSoundVariableCandidateAssetsLoadBeforeClipCompilation},
        std::pair{"testScoreClipFingerprintReusesRegistryOnBoundedWorkerStack", &testScoreClipFingerprintReusesRegistryOnBoundedWorkerStack},
        std::pair{"testTrackAngleAndRadiusPanClips", &testTrackAngleAndRadiusPanClips},
        std::pair{"testTrackPositionWrapsAroundTheCircle", &testTrackPositionWrapsAroundTheCircle},
        std::pair{"testClipControlsMoveWithTheirSoundAndTrackControlsStay", &testClipControlsMoveWithTheirSoundAndTrackControlsStay},
        std::pair{"testOnlyAutomatedLanesDriveTheirParameters", &testOnlyAutomatedLanesDriveTheirParameters},
        std::pair{"testSlavedLanesFollowTheirMaster", &testSlavedLanesFollowTheirMaster},
        std::pair{"testSoundPannerAndDistanceDeviateFromTheTrack", &testSoundPannerAndDistanceDeviateFromTheTrack},
        std::pair{"testCachesAreRecordedPreFader", &testCachesAreRecordedPreFader},
        std::pair{"testTracksHardWiredToOutputs", &testTracksHardWiredToOutputs},
        std::pair{"testSpeakerPlacementSpreadsTheTimelineOverItsSpeakers", &testSpeakerPlacementSpreadsTheTimelineOverItsSpeakers},
        std::pair{"testTrackLevelsAreFaders", &testTrackLevelsAreFaders},
        std::pair{"testASoundsFadeInOutIsItsAmplitude", &testASoundsFadeInOutIsItsAmplitude},
        std::pair{"testTheTrackPanningStatus", &testTheTrackPanningStatus},
        std::pair{"testSoundsFollowTheTimelinesBpm", &testSoundsFollowTheTimelinesBpm},
        std::pair{"testAStereoSoundStaysStereoInATrack", &testAStereoSoundStaysStereoInATrack},
        std::pair{"testALocalBpmFollowsOrLeavesTheMaster", &testALocalBpmFollowsOrLeavesTheMaster},
        std::pair{"testADiskPlayersRateFollowsItsMasterControl", &testADiskPlayersRateFollowsItsMasterControl},
        std::pair{"testATimelinesMidiOutputMirrorsADiskPlayersRate", &testATimelinesMidiOutputMirrorsADiskPlayersRate},
        std::pair{"testTransformedLanesPlayTheirTransformedFunction", &testTransformedLanesPlayTheirTransformedFunction},
        std::pair{"testTransportJumpsToMarkersAndImportantTimes", &testTransportJumpsToMarkersAndImportantTimes},
        std::pair{"testCachedAndFrozenArtifactsUseExactByteAuthority", &testCachedAndFrozenArtifactsUseExactByteAuthority},
        std::pair{"testOrdinaryDiskPlayerRemainsLiveInParentScore", &testOrdinaryDiskPlayerRemainsLiveInParentScore},
        std::pair{"testFingerprintIncludesPersistedEventValueAuthority", &testFingerprintIncludesPersistedEventValueAuthority},
        std::pair{"testScoreClipArtifactDirectoryPublicationIsAtomicAndNoReplace", &testScoreClipArtifactDirectoryPublicationIsAtomicAndNoReplace},
        std::pair{"testATimelinesCachesAreMonoTwentyFourBitRecordings", &testATimelinesCachesAreMonoTwentyFourBitRecordings}})
    {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { failed = true; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
