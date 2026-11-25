#include "app/workbench/WorkbenchComponentInternal.h"

#include <fstream>

namespace talas::app::workbench
{
// A track's Angle and
// Radius, the pencil, an effect dropped on a clip, a clip's input replaced,
// minus removing the effect; and a MIDI file imported into the Timeline.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runTimelineEditingSmoke(const std::filesystem::path& smokeDirectory)
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "Timeline editing smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto document = [this]() -> const talas::project::ProjectDocument& {
        return projectBridge_.editor().document();
    };
#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    talas::project::ProjectDocument fixture;
    for (const auto& [id, name, type] : {std::tuple{"pad", "Pad", "talas.oscillator"}, std::tuple{"bass", "Bass", "talas.noise"}})
    {
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId = id;
        definition.displayName = name;
        definition.graph.nodes.push_back({"tone", type, {}});
        definition.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        fixture.soundDefinitions.push_back(std::move(definition));
    }
    talas::project::ScoreTrack master;
    master.trackId = "master";
    master.displayName = "Master";
    master.kind = talas::project::ScoreTrackKind::Master;
    talas::project::ScoreTrack track;
    track.trackId = "track1";
    track.displayName = "Track 1";
    track.outputTrackId = "master";
    fixture.timeline.score.tracks = {master, track};
    const talas::project::ScoreTime zero{talas::project::ScoreTimeDomain::Seconds, 0.0, 0};
    fixture.timeline.score.clips.push_back({"clip1", "track1", "pad", zero,
        {talas::project::ScoreTimeDomain::Seconds, 1.0, 0}, zero, zero, zero, 1});
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Timeline editing fixture was rejected: " + replaced.message);
    documentChanged();
    const auto clipSound = [&]() -> std::string {
        const auto& clips = document().timeline.score.clips;
        const auto found = std::ranges::find(clips, std::string_view{"clip1"}, &talas::project::ScoreClip::clipId);
        return found == clips.end() ? std::string{} : found->soundDefinitionId;
    };
    const auto definitionOf = [&](const std::string& id) -> const talas::project::ProjectSoundDefinition* {
        const auto& definitions = document().soundDefinitions;
        const auto found = std::ranges::find(definitions, id, &talas::project::ProjectSoundDefinition::definitionId);
        return found == definitions.end() ? nullptr : &*found;
    };

    // Angle and Radius are the track's controls.
    const TimelineScoreSelection trackSelection{TimelineScoreSelectionKind::Track, "track1", 0};
    const auto menu = timelinePanel_->automateMenuForSmoke(trackSelection);
    if (std::ranges::find(menu, juce::String{"Angle"}) == menu.end()
        || std::ranges::find(menu, juce::String{"Radius"}) == menu.end())
        return finish("the track's Automate menu lacks Angle and Radius");
    if (!timelinePanel_->automateForSmoke(trackSelection, "Angle"))
        return finish("Automate > Angle did not add the track's Angle lane");
    const auto& lanes = document().timeline.score.automation;
    const auto angle = std::ranges::find(lanes, std::string_view{"Angle"}, &talas::project::ScoreAutomationLane::eventName);
    if (angle == lanes.end())
        return finish("the Angle lane is missing");
    const auto angleId = angle->laneId;
    // A pencil stroke from hard left to hard right.
    if (!timelinePanel_->drawAutomationStrokeForSmoke(angleId, {{0, 0.0}, {12000, 0.5}, {24000, 1.0}}))
        return finish("the pencil did not draw into the Angle lane");
    const auto drawn = std::ranges::find(document().timeline.score.automation, angleId, &talas::project::ScoreAutomationLane::laneId);
    if (drawn == document().timeline.score.automation.end() || drawn->points.size() < 3
        || drawn->points.back().value != 1.0)
        return finish("the drawn Angle function was not kept");

    // An effect dropped on a clip.
    if (!timelinePanel_->applyEffectToClipForSmoke("clip1", "talas.filter"))
        return finish("the clip refused a dropped Filter");
    const auto* wrapped = definitionOf(clipSound());
    if (wrapped == nullptr || wrapped->displayName.find("(Pad)") == std::string::npos)
        return finish("the clip's Sound was not the Filter on Pad");
    // Option-drop a Sound to replace the input.
    if (!timelinePanel_->replaceClipInputForSmoke("clip1", "bass"))
        return finish("the clip refused a replaced input");
    const auto* replaced = definitionOf(clipSound());
    if (replaced == nullptr || replaced->displayName.find("(Bass)") == std::string::npos)
        return finish("the clip's input was not replaced by Bass");
    // Minus removes the last effect.
    if (!timelinePanel_->removeClipEffectForSmoke("clip1") || clipSound() != "bass")
        return finish("minus did not remove the clip's effect");
    undoProject();
    if (definitionOf(clipSound()) == nullptr || definitionOf(clipSound())->displayName.find("(Bass)") == std::string::npos)
        return finish("one Undo did not bring the effect back");

    // A MIDI file imported into the Timeline.
    const auto midi = smokeDirectory / "timeline-import.mid";
    {
        const std::vector<unsigned char> track{0x00, 0x90, 69, 100, 0x81, 0x70, 0x80, 69, 0, 0x00, 0xFF, 0x2F, 0x00};
        std::vector<unsigned char> file{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k', 0, 0,
            0, static_cast<unsigned char>(track.size())};
        file.insert(file.end(), track.begin(), track.end());
        std::ofstream output(midi, std::ios::binary);
        output.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    const auto tracksBefore = document().timeline.score.tracks.size();
    if (!importMidiFileToTimeline(midi) || document().timeline.score.tracks.size() != tracksBefore + 1)
        return finish("Import MIDI File did not add a track");
    const auto& clips = document().timeline.score.clips;
    const auto imported = std::ranges::find_if(clips, [](const auto& clip) { return clip.clipId.starts_with("midiClip"); });
    const auto* sound = imported == clips.end() ? nullptr : definitionOf(imported->soundDefinitionId);
    if (sound == nullptr || std::ranges::none_of(sound->graph.nodes, [](const auto& node) {
            return node.typeId == "talas.midiVoice";
        })
        || std::abs(imported->duration.seconds - 0.25) > 1e-9)
        return finish("the imported MIDI file's clip is not a MIDIVoice of the file's length");
    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandImportMidiToTimeline;
    if (!listed)
        return finish("the Tools menu lacks Import MIDI File to Timeline");
    return finish();
#else
    (void)smokeDirectory;
    (void)document;
    return finish();
#endif
}
}
