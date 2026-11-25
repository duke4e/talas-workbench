#include "app/workbench/WorkbenchAnalysisPanel.h"
#include "app/workbench/WorkbenchTheme.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <array>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void testScoreSourceSelection(const juce::File* screenshots)
{
    using namespace talas::app::workbench;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice"; voice.displayName = "Voice";
    voice.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "20 hz"}, {"amplitude", "0.5"}}}};
    voice.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    document.soundDefinitions = {voice};
    document.graph.nodes = {{"voiceInstance", {}, {}, "voice"}, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"voiceInstance", "audioOut", "out", "audioIn"}};
    using Time = talas::project::ScoreTime;
    const auto seconds = [](double value) { return Time{talas::project::ScoreTimeDomain::Seconds, value, 0}; };
    document.timeline.score.tracks = {
        {"track", "Track", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds,
            "master", 1, talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::Seconds,
            {}, 1, talas::project::ScoreOverlapMode::Layer}};
    document.timeline.score.clips = {{"melody", "track", "voice", seconds(0), seconds(0.128),
        seconds(0), seconds(0), seconds(0), 2}};
    document.timeline.score.clips[0].sourceInstanceId = "voiceInstance";
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000; settings.maxBlockFrames = 64; settings.enableRealtimeAnalysisCapture = true;
    settings.realtimeAnalysisTargets.push_back({{"voiceInstance"}, "voice", "osc", "audioOut",
        talas::runtime::RealtimeAnalysisKind::Oscilloscope, talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock,
        1, "voice-trace"});
    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    const auto stage = bridge.compileAndStage(settings);
    if (!stage.ok())
    {
        std::string message = "stage Score analysis UI fixture: " + stage.message;
        for (const auto& diagnostic : stage.diagnostics) message += " | " + diagnostic.message;
        throw std::runtime_error(message);
    }
    bridge.setPlaying(true);
    std::array<float, 64> audio{}; float* outputs[]{audio.data()};
    for (int block = 0; block < 3; ++block) require(bridge.render(outputs, 1, audio.size()), "render Score source probes");

    talas::project::WorkspaceAnalysisSessionState workspace;
    talas::project::WorkspaceAnalysisTraceState trace;
    trace.traceId = "voice-trace"; trace.instancePath = {"voiceInstance"};
    trace.containingDefinitionId = "voice"; trace.nodeId = "osc"; trace.portId = "audioOut";
    workspace.traces = {trace}; workspace.selectedTraceId = trace.traceId;
    WorkbenchLookAndFeel theme;
    auto panel = makeAnalysisPanel();
    panel->setLookAndFeel(&theme);
    panel->setSize(640, 600);
    panel->setWorkspaceProjection(talas::app::buildAnalysisWorkbenchProjection(document, registry, workspace), false, 30, 1024);
    AnalysisAuditionPanelState audition;
    audition.status = "Audition idle";
    panel->setAuditionState(std::move(audition));
    AnalysisOutputTakePanelState outputTake;
    outputTake.status = "No output take";
    panel->setOutputTakeState(std::move(outputTake));
    panel->setPreparedStatus("Score source analysis / 2 repeats");
    panel->setCapture(bridge.latestAnalysisCapture(), bridge.latestAnalysisCaptureSampleRate(), bridge.latestScoreSourceAnalysisCaptures());
    panel->setPlaying(true);
    require(panel->sourceControlsUsableForSmoke(), "source selector is labelled and contained");
    panel->poll();
    require(panel->selectedTraceSequenceForSmoke() == 2, "initial selection shows first repeat evidence");
    require(panel->selectScoreSourceForSmoke("melody", 1), "select exact second repeat");
    require(!panel->selectedTraceSequenceForSmoke(), "switching sources clears the previous repeat's evidence");
    panel->poll();
    require(panel->selectedTraceSequenceForSmoke() == 1, "second repeat shows its own sequence");
    // The normal Workbench refresh must preserve the user's source selection.
    panel->setCapture(bridge.latestAnalysisCapture(), bridge.latestAnalysisCaptureSampleRate(), bridge.latestScoreSourceAnalysisCaptures());
    require(panel->selectedTraceSequenceForSmoke() == 1, "unchanged stage refresh retains the source selection");
    for (const auto width : {640, 320})
    {
        panel->setSize(width, 600);
        if (screenshots)
        {
            auto stream = screenshots->getChildFile("score-analysis-" + juce::String(width) + ".png").createOutputStream();
            require(stream && stream->setPosition(0) && stream->truncate().wasOk()
                && juce::PNGImageFormat{}.writeImageToStream(panel->createComponentSnapshot(panel->getLocalBounds()), *stream),
                "write Score analysis UI evidence");
        }
        require(panel->sourceControlsUsableForSmoke(),
            "source selector remains labelled and contained at both panel widths");
    }
    require(bridge.compileAndStage(settings).ok(), "replace Score analysis UI stage");
    panel->setCapture(bridge.latestAnalysisCapture(), bridge.latestAnalysisCaptureSampleRate(), bridge.latestScoreSourceAnalysisCaptures());
    require(!panel->selectedTraceSequenceForSmoke(), "replacement stage clears stale analysis evidence");
    require(bridge.render(outputs, 1, audio.size()), "adopt replacement Score stage");
    panel->poll();
    require(panel->selectedTraceSequenceForSmoke() == 1, "replacement retains the exact selected repeat");
    panel->clearCapture();
    require(!panel->selectedTraceSequenceForSmoke() && !panel->selectScoreSourceForSmoke("melody", 1),
        "clearing analysis retires Score source selection and evidence");
}
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    try
    {
        const auto screenshots = argc == 2 ? std::optional<juce::File>{juce::String(argv[1])} : std::nullopt;
        testScoreSourceSelection(screenshots ? &*screenshots : nullptr);
        std::cout << "PASS Score analysis source selection, repeat identity, stage replacement and compact layout\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
