#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/TimelineInfo.h"

namespace talas::app::workbench
{
// The strip at the top of the Timeline, the
// Sound's times typed into its fields, the anchor and the alignments to
// it, Capture, and the markers' names, times and program changes.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runTimelineInfoSmoke()
{
#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "Timeline information smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    const auto seconds = [](double value) {
        return talas::project::ScoreTime{talas::project::ScoreTimeDomain::Seconds, value, 0};
    };
    talas::project::ProjectDocument fixture;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Vocoder";
    definition.outputs.push_back({"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    definition.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    fixture.soundDefinitions.push_back(std::move(definition));
    talas::project::ScoreTrack master;
    master.trackId = "master";
    master.displayName = "Master";
    master.kind = talas::project::ScoreTrackKind::Master;
    talas::project::ScoreTrack track;
    track.trackId = "track1";
    track.displayName = "Track 1";
    track.outputTrackId = "master";
    track.overlapMode = talas::project::ScoreOverlapMode::Layer;
    fixture.timeline.score.tracks = {master, track};
    fixture.timeline.score.clips = {
        {"first", "track1", "voice", seconds(1.0), seconds(4.0), seconds(0.0), seconds(0.0), seconds(0.0), 1},
        {"second", "track1", "voice", seconds(0.0), seconds(1.0), seconds(0.0), seconds(0.0), seconds(0.0), 1},
    };
    fixture.timeline.score.markers = {{"entry", "Entry", seconds(2.0), std::nullopt}};
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Timeline information fixture was rejected: " + replaced.message);
    documentChanged();
    const auto& score = projectBridge_.editor().document().timeline.score;
    const auto clip = [&score](std::string_view id) {
        return *std::ranges::find(score.clips, id, &talas::project::ScoreClip::clipId);
    };
    const talas::app::TimelineScoreSelection first{talas::app::TimelineScoreSelectionKind::Clip, "first", 0};
    const talas::app::TimelineScoreSelection second{talas::app::TimelineScoreSelectionKind::Clip, "second", 0};
    if (timelinePanel_->infoFieldTextForSmoke(first, TimelineInfoField::SoundStart) != "1"
        || timelinePanel_->infoFieldTextForSmoke(first, TimelineInfoField::SoundAnchor) != "1"
        || timelinePanel_->infoFieldTextForSmoke(first, TimelineInfoField::SoundEnd) != "5")
        return finish("the strip did not show the selected Sound's start, anchor and end");
    if (!timelinePanel_->enterInfoFieldForSmoke(first, TimelineInfoField::SoundStart, "0") || clip("first").start != seconds(0.0))
        return finish("typing 0 in the Start field did not start the Sound at zero");
    if (!timelinePanel_->enterInfoFieldForSmoke(first, TimelineInfoField::SoundEnd, "00:00:60:00")
        || clip("first").duration != seconds(60.0))
        return finish("typing 00:00:60:00 in the End field did not make the Sound sixty seconds long");
    if (!timelinePanel_->enterInfoFieldForSmoke(first, TimelineInfoField::SoundAnchor, "3")
        || talas::app::clipAnchorTime(clip("first")) != seconds(3.0)
        || timelinePanel_->infoFieldTextForSmoke(first, TimelineInfoField::SoundAnchor) != "3")
        return finish("typing 3 in the Anchor field did not anchor the Sound at 3 seconds");
    if (timelinePanel_->enterInfoFieldForSmoke(first, TimelineInfoField::SoundAnchor, "61"))
        return finish("an anchor outside the Sound was taken");
    if (!timelinePanel_->alignClipForSmoke(second, "first", true) || clip("second").start != seconds(2.0))
        return finish("Align End to Anchor did not end the second Sound at the first's anchor");
    if (!timelinePanel_->alignClipForSmoke(second, "first", false) || clip("second").start != seconds(3.0))
        return finish("Align Start to Anchor did not begin the second Sound at the first's anchor");
    if (!timelinePanel_->anchorAtCursorForSmoke(first) || clip("first").anchorOffset.has_value())
        return finish("the Anchor button did not anchor the Sound at the cursor, its start");

    const talas::app::TimelineScoreSelection entry{talas::app::TimelineScoreSelectionKind::Marker, "entry", 0};
    if (timelinePanel_->infoFieldTextForSmoke(entry, TimelineInfoField::MarkerName) != "Entry"
        || timelinePanel_->infoFieldTextForSmoke(entry, TimelineInfoField::MarkerTime) != "2")
        return finish("the strip did not show the selected marker's name and time");
    if (!timelinePanel_->enterInfoFieldForSmoke(entry, TimelineInfoField::MarkerName, "Chorus")
        || !timelinePanel_->enterInfoFieldForSmoke(entry, TimelineInfoField::MarkerTime, "2.5")
        || score.markers.front().title != "Chorus" || score.markers.front().start != seconds(2.5))
        return finish("typing into the marker's name and time fields did not rename and move it");
    // The name field takes no spaces, Space playing and pausing
    // there; the Large window (Ctrl+L) gives a name with spaces.
    auto spaces = 0;
    timelinePanel_->setSpaceKey([&spaces] { ++spaces; });
    const auto typed = timelinePanel_->typeIntoInfoFieldForSmoke(entry, TimelineInfoField::MarkerName, "Verse two");
    timelinePanel_->setSpaceKey([this] { pressTransportKey(talas::app::TalasTransportPress::Space); });
    if (typed != "Versetwo" || spaces != 1)
        return finish("Space typed into the marker's name did not play and pause instead: " + typed);
    if (!timelinePanel_->enterInfoFieldForSmoke(entry, TimelineInfoField::MarkerName,
            talas::app::largeWindowResult({"Chorus", "Verse two"}, true))
        || score.markers.front().title != "Verse two")
        return finish("the Large window's name with spaces did not rename the marker");
    if (!timelinePanel_->setMarkerProgramForSmoke(entry, 9) || score.markers.front().programNumber != std::optional<std::uint8_t>{9})
        return finish("the program change menu did not give the marker program change 9");
    if (!timelinePanel_->captureMarkerForSmoke() || score.markers.size() != 2)
        return finish("Capture did not place a marker at the cursor");
    // The Timeline's equivalent Sound joins the project.
    const auto soundsBefore = projectBridge_.editor().document().soundDefinitions.size();
    if (!timelinePanel_->makeSoundOfTimelineForSmoke()
        || projectBridge_.editor().document().soundDefinitions.size() != soundsBefore + 1
        || projectBridge_.editor().document().soundDefinitions.back().displayName.rfind("Timeline", 0) != 0)
        return finish("Make a Sound of the Timeline did not add the Timeline's Sound: " + statusLabel_.getText().toStdString());
    undoProject();
    // A double-click on a Sound in the Timeline opens its editor.
    const auto edited = std::ranges::find(score.clips, std::string{"second"}, &talas::project::ScoreClip::clipId);
    if (edited == score.clips.end() || !timelinePanel_->editClipSoundForSmoke("second")
        || activeGraphContext_ != std::optional<std::string>{edited->soundDefinitionId})
        return finish("a double-click on a Sound in the Timeline did not open its editor");
    showRootGraph();
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Automation);
    // Copy and Paste put a copy of a Sound on a track.
    const auto clipsBefore = score.clips.size();
    if (!timelinePanel_->copyAndPasteClipsForSmoke({"second"}, "track1", 48000 * 70)
        || score.clips.size() != clipsBefore + 1 || score.clips.back().clipId != "secondCopy"
        || score.clips.back().start.seconds < 69.9)
        return finish("Copy and Paste did not put a copy of the Sound at the time clicked: " + statusLabel_.getText().toStdString());
    undoProject();
    // A Sound copied in a signal flow pastes as a new Sound's clip.
    talas::project::ProjectGraphFragment copiedSound;
    copiedSound.nodes = {{"bell", "talas.oscillator", {}}};
    copiedSound.positions = {{40, 40}};
    setGraphClipboardText(juceString(talas::project::serializeProjectGraphFragment(copiedSound)));
    const auto soundsBeforePaste = projectBridge_.editor().document().soundDefinitions.size();
    if (!timelinePanel_->copyAndPasteClipsForSmoke({}, "track1", 48000 * 80) || score.clips.size() != clipsBefore + 1
        || projectBridge_.editor().document().soundDefinitions.size() != soundsBeforePaste + 1
        || score.clips.back().soundDefinitionId != projectBridge_.editor().document().soundDefinitions.back().definitionId)
        return finish("a Sound copied in the signal flow did not paste as a clip: " + statusLabel_.getText().toStdString());
    undoProject();
    undoProject();
    if (!timelinePanel_->setMarkerProgramChannelForSmoke(16)
        || projectBridge_.editor().document().timeline.markerProgramChannel != 16)
        return finish("the program change channel menu did not take channel 16");
    if (!timelinePanel_->jumpToMarkerForProgramChange(16, 9) || timelinePanel_->jumpToMarkerForProgramChange(15, 9))
        return finish("a program change on the Timeline's channel alone should jump to its marker");
    // Set time code offset, the video's starting time code.
    if (!timelinePanel_->setTimeCodeOffsetForSmoke("01:00:00:00")
        || projectBridge_.editor().document().timeline.timeCodeOffsetSeconds != 3600.0)
        return finish("Set time code offset did not start the Timeline at 01:00:00:00");
    if (timelinePanel_->infoFieldTextForSmoke({}, TimelineInfoField::Cursor).empty())
        return finish("the strip did not show the cursor's time");

    // Option-click marks a Sound, Shift toggles it,
    // and Shift+Space plays the marked Sounds only.
    auto& marks = timelinePanel_->marks();
    if (!timelinePanel_->markClip("second", false) || !marks.marked("second")
        || !timelinePanel_->markClip("second", true) || marks.marked("second"))
        return finish("marking and unmarking a Sound did not take");
    (void) timelinePanel_->markClip("first", false);
    marks.playMarkedOnly();
    const auto played = marks.playedClips(score);
    marks.playAll();
    marks.clear();
    if (played != std::optional<std::vector<std::string>>{std::vector<std::string>{"first"}})
        return finish("playing the marked Sounds did not play the marked Sound alone");
    return finish();
#else
    return std::nullopt;
#endif
}
}
