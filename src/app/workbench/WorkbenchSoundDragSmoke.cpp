#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/SoundBrowserModel.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectJson.h"

#include <fstream>

namespace talas::app::workbench
{
// A Sound dragged from the Sound
// browser goes into a Sound editor (onto a line, onto a Sound to replace
// it, or into empty space) and into the Timeline's tracks;
// Set replaceable input, and a Sound with one dropped on a line takes the
// line there.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runSoundDragSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousGraphViewMode = workspaceSession_.graphViewMode;
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        setWorkspaceGraphViewMode(previousGraphViewMode);
        if (!restored.ok() && error.empty())
            error = "Sound drag smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto document = [this]() -> const talas::project::ProjectDocument& {
        return projectBridge_.editor().document();
    };

    talas::project::ProjectDocument fixture;
    talas::project::ProjectSoundDefinition pad;
    pad.definitionId = "pad";
    pad.displayName = "Pad";
    pad.graph.nodes.push_back({"tone", "talas.oscillator", {}});
    pad.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
    fixture.soundDefinitions.push_back(std::move(pad));
    talas::project::ProjectSoundDefinition doppler;
    doppler.definitionId = "doppler";
    doppler.displayName = "Effects-Doppler shift";
    doppler.graph.nodes = {{"Mixer206", "talas.oscillator", {}}, {"shifter", "talas.vca", {}}};
    doppler.graph.connections = {{"Mixer206", "audioOut", "shifter", "audioIn"}};
    doppler.outputs.push_back({"audioOut", talas::core::PortType::Audio, "shifter", "audioOut"});
    fixture.soundDefinitions.push_back(std::move(doppler));
    fixture.graph.nodes = {
        {"noise", "talas.noise", {}},
        {"out", "talas.audioOutput", {}},
    };
    fixture.graph.connections = {{"noise", "audioOut", "out", "audioIn"}};
    talas::project::ScoreTrack master;
    master.trackId = "master";
    master.displayName = "Master";
    master.kind = talas::project::ScoreTrackKind::Master;
    talas::project::ScoreTrack track;
    track.trackId = "track1";
    track.displayName = "Track 1";
    track.outputTrackId = "master";
    fixture.timeline.score.tracks = {master, track};
    talas::project::reconcileProjectPresentation(fixture);
    talas::project::findProjectNodePresentation(fixture, std::nullopt, "noise")->position = {40, 80};
    talas::project::findProjectNodePresentation(fixture, std::nullopt, "out")->position = {640, 80};
    talas::project::findProjectNodePresentation(fixture, std::string_view{"doppler"}, "Mixer206")->position = {40, 80};
    talas::project::findProjectNodePresentation(fixture, std::string_view{"doppler"}, "shifter")->position = {400, 80};
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture);
        !replaced.ok())
    {
        return finish("Sound drag fixture was rejected: " + replaced.message);
    }
    documentChanged();
    showRootGraph();
    setWorkspaceGraphViewMode(talas::project::WorkspaceGraphViewMode::PortsAndWires);
    graphCanvas_->refreshModel();
    const auto description = soundDefinitionsPanel_->dragDescriptionForSmoke("project:pad");
    if (description != juceString(soundDragPrefix) + "pad")
        return finish("the Sound browser did not drag the project's Pad");
    const auto baseline = talas::project::serializeProjectDocument(document());
    const auto instanceOfPad = [&]() -> std::string {
        const auto found = std::ranges::find_if(document().graph.nodes, [](const auto& node) {
            return node.soundDefinitionId == std::optional<std::string>{"pad"};
        });
        return found == document().graph.nodes.end() ? std::string{} : found->instanceId;
    };

    auto* canvas = graphCanvas_.get();
    const auto empty = juce::Point<int>{
        canvas->nodeCentreForSmoke("noise")->x,
        canvas->nodeCentreForSmoke("noise")->y + 260};
    if (!canvas->dropDescriptionForSmoke(description, empty) || instanceOfPad().empty())
        return finish("a Sound dropped on empty canvas did not add an instance");
    undoProject();
    graphCanvas_->refreshModel();
    const auto noise = canvas->nodeCentreForSmoke("noise");
    smokeInputChoice_ = 0; // Replace, asked as one source takes another's place.
    const auto droppedOn = noise.has_value() && canvas->dropDescriptionForSmoke(description, *noise);
    smokeInputChoice_.reset();
    if (!droppedOn)
        return finish("a Sound dropped on another was refused");
    const auto padId = instanceOfPad();
    if (padId.empty()
        || std::ranges::find(document().graph.connections,
               talas::core::Connection{padId, "audioOut", "out", "audioIn"})
            == document().graph.connections.end())
        return finish("a Sound dropped on another did not replace it");
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("one Undo did not take back a dropped Sound");
    // With Control held a Sound of the signal flow
    // dropped on another takes its place itself, not a copy.
    if (!canvas->dropDescriptionForSmoke(juceString(prototypeDragPrefix) + "talas.oscillator", empty))
        return finish("an Oscillator dropped on empty canvas was refused");
    const auto tone = std::ranges::find(document().graph.nodes, std::string{"talas.oscillator"}, &talas::project::ProjectNode::typeId);
    if (tone == document().graph.nodes.end())
        return finish("the Oscillator dropped on empty canvas did not join the signal flow");
    const auto toneId = tone->instanceId;
    graphCanvas_->refreshModel();
    const auto soundCount = document().soundDefinitions.size();
    const auto noiseCentre = canvas->nodeCentreForSmoke("noise");
    if (!noiseCentre.has_value()
        || !canvas->dropDescriptionForSmoke(juce::String{canvas->dragDescriptionForSmoke(toneId)}, *noiseCentre, true)
        || std::ranges::find(document().graph.nodes, std::string{"noise"}, &talas::project::ProjectNode::instanceId)
            != document().graph.nodes.end()
        || document().soundDefinitions.size() != soundCount
        || std::ranges::find(document().graph.connections, talas::core::Connection{toneId, "audioOut", "out", "audioIn"})
            == document().graph.connections.end())
        return finish("the Oscillator dropped with Control held did not take the Noise's place itself: "
            + statusLabel_.getText().toStdString());
    undoProject();
    undoProject();
    if (talas::project::serializeProjectDocument(document()) != baseline)
        return finish("two Undos did not take back the shared Sound and the Oscillator");
    graphCanvas_->refreshModel();

#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    if (!timelinePanel_->dropSoundOnTrackForSmoke("pad", "track1", 0)
        || document().timeline.score.clips.size() != 1
        || document().timeline.score.clips.front().soundDefinitionId != "pad"
        || document().timeline.score.clips.front().trackId != "track1"
        || timelinePanel_->selectedClipsForSmoke()
            != std::vector<std::string>{document().timeline.score.clips.front().clipId})
        return finish("a Sound dropped on a Timeline track did not become a selected clip there");
#endif

    // Set replaceable input on Mixer206 in the Sound's editor gives
    // it the cyan halo.
    showSoundDefinition("doppler");
    auto* editorCanvas = secondaryGraphCanvas_.get();
    editorCanvas->refreshModel();
    const auto mixer = editorCanvas->nodeCentreForSmoke("Mixer206");
    int setReplaceable = 0;
    if (mixer.has_value())
    {
        const auto menu = editorCanvas->contextMenuForSmoke(*mixer);
        for (juce::PopupMenu::MenuItemIterator item{menu}; item.next();)
            if (item.getItem().text == "Set Replaceable Input" && item.getItem().isEnabled)
                setReplaceable = item.getItem().itemID;
    }
    const auto replaceableOf = [&]() -> const talas::project::ProjectSoundPort* {
        const auto found = std::ranges::find(document().soundDefinitions, std::string{"doppler"},
            &talas::project::ProjectSoundDefinition::definitionId);
        return found == document().soundDefinitions.end() ? nullptr : talas::project::replaceableSoundInput(*found);
    };
    if (setReplaceable == 0 || !editorCanvas->chooseContextMenuItemForSmoke(*mixer, setReplaceable)
        || replaceableOf() == nullptr || !replaceableOf()->replacesSound || replaceableOf()->nodeId != "Mixer206"
        || editorCanvas->replaceableInputNodeIdForSmoke() != "Mixer206")
        return finish("Set replaceable input did not make Mixer206 the Sound's replaceable input: "
            + statusLabel_.getText().toStdString());

    // Dropped on a line, the Sound takes the line into its
    // replaceable input.
    showRootGraph();
    graphCanvas_->refreshModel();
    const auto line = canvas->connectionMidpointForSmoke({"noise", "audioOut", "out", "audioIn"});
    const auto dopplerDrag = soundDefinitionsPanel_->dragDescriptionForSmoke("project:doppler");
    if (!line.has_value() || !canvas->dropDescriptionForSmoke(dopplerDrag, *line))
        return finish("the Sound with a replaceable input was not dropped on the line");
    const auto shifted = std::ranges::find_if(document().graph.nodes, [](const auto& node) {
        return node.soundDefinitionId == std::optional<std::string>{"doppler"};
    });
    if (shifted == document().graph.nodes.end()
        || std::ranges::find(document().graph.connections,
               talas::core::Connection{"noise", "audioOut", shifted->instanceId, "replaceableInput"})
            == document().graph.connections.end())
        return finish("the line did not go into the dropped Sound's replaceable input");

    // A sample and a Sound dragged from a Sound Browser
    // join the project's Sounds where they are dropped: the Sound file
    // window, the signal flow and a Timeline track.
    const auto folder = std::filesystem::temp_directory_path() / "talas-browser-drag-smoke";
    std::filesystem::create_directories(folder);
    std::string wavError;
    talas::project::ProjectDocument bells;
    bells.soundDefinitions.push_back(document().soundDefinitions.front());
    bells.soundDefinitions.front().definitionId = "bell";
    bells.soundDefinitions.front().displayName = "Bell";
    talas::project::reconcileProjectPresentation(bells);
    {
        std::ofstream output(folder / "Bells.talas.json", std::ios::binary);
        output << talas::project::serializeProjectDocument(bells);
    }
    const std::vector<std::vector<double>> gearbox(1, std::vector<double>(800, 0.1));
    if (!talas::audio::writePcm16Wav(folder / "gearbox.wav", 8000.0, gearbox, wavError))
        return finish("the browser drag smoke could not write its sample: " + wavError);
    const auto sampleDrag = juce::String{talas::app::soundBrowserDragDescription(
        {folder / "gearbox.wav", {}, "gearbox.wav", 1, talas::app::SoundBrowserKind::Sample})};
    const auto soundDrag = juce::String{talas::app::soundBrowserDragDescription(
        {folder / "Bells.talas.json", "bell", "Bell", 1, talas::app::SoundBrowserKind::Sound})};
    const auto sounds = [&] { return document().soundDefinitions.size(); };
    auto before = sounds();
    showSoundFileWindow();
    if (soundFileForSmoke() == nullptr || !soundFileForSmoke()->dropForSmoke(sampleDrag) || sounds() != before + 1)
        return finish("a sample dragged from a Sound Browser into the Sound file window did not become a Sound");
    // A GenericSource named for the sample file.
    if (document().soundDefinitions.back().displayName != "gearbox"
        || document().soundDefinitions.back().graph.nodes.front().typeId != "talas.genericSource")
        return finish("a sample dragged in did not become a GenericSource named for its file");
    showRootGraph();
    graphCanvas_->refreshModel();
    before = sounds();
    const auto nodes = document().graph.nodes.size();
    if (!canvas->dropDescriptionForSmoke(soundDrag, empty) || sounds() != before + 1 || document().graph.nodes.size() != nodes + 1
        || std::ranges::none_of(document().soundDefinitions, [](const auto& sound) { return sound.displayName == "Bell"; }))
        return finish("a Sound dragged from a Sound Browser into the signal flow did not join it: "
            + statusLabel_.getText().toStdString());
#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    before = sounds();
    const auto clips = document().timeline.score.clips.size();
    if (!timelinePanel_->dropDescriptionOnTrackForSmoke(sampleDrag, "track1", 48000) || sounds() != before + 1
        || document().timeline.score.clips.size() != clips + 1)
        return finish("a sample dragged from a Sound Browser onto a track did not become a clip there");
    // A Sound dragged out of a signal flow is a
    // copy of it with the Sounds feeding it, in the Sound file window or on
    // a track.
    const auto shifterDrag = juce::String{talas::app::signalFlowSoundDragDescription(std::string{"doppler"}, "shifter")};
    if (!soundFileForSmoke()->dropForSmoke(shifterDrag) || sounds() != before + 2
        || document().soundDefinitions.back().graph.nodes.size() != 2
        || document().soundDefinitions.back().outputs.front().nodeId != "shifter")
        return finish("a Sound dragged out of its signal flow did not leave a copy in the Sound file window");
    if (!timelinePanel_->dropDescriptionOnTrackForSmoke(juce::String{canvas->dragDescriptionForSmoke("noise")}, "track1", 144000)
        || sounds() != before + 3 || document().timeline.score.clips.size() != clips + 2)
        return finish("a Sound dragged out of the signal flow onto a track did not become a clip there");
    // A prototype dropped on a track is a new Sound there.
    if (!timelinePanel_->dropDescriptionOnTrackForSmoke(juceString(prototypeDragPrefix) + "talas.oscillator", "track1", 96000)
        || sounds() != before + 4 || document().timeline.score.clips.size() != clips + 3)
        return finish("a prototype dropped on a track did not become a Sound there");
#endif
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    return finish();
}
}
