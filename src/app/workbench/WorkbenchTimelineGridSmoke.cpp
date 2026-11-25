#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/TimelineGrid.h"

namespace talas::app::workbench
{
// A Sound moved down a track over an occupied
// one, a Sound dropped on a Magnet grid sticking to the one before it, one
// dropped on a Quantized grid landing on the quantum, the arrow nudging by
// it, the zoom buttons, and Enter beat or time opening a Sound's Start.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runTimelineGridSmoke()
{
#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        (void) timelinePanel_->setGridForSmoke(talas::app::TimelineGridMode::Free, {});
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "Timeline grid smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    const auto seconds = [](double value) {
        return talas::project::ScoreTime{talas::project::ScoreTimeDomain::Seconds, value, 0};
    };
    talas::project::ProjectDocument fixture;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.outputs.push_back({"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    definition.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    fixture.soundDefinitions.push_back(std::move(definition));
    const auto track = [](std::string id) {
        talas::project::ScoreTrack result;
        result.trackId = id;
        result.displayName = id;
        result.outputTrackId = "master";
        return result;
    };
    talas::project::ScoreTrack master;
    master.trackId = "master";
    master.displayName = "Master";
    master.kind = talas::project::ScoreTrackKind::Master;
    fixture.timeline.score.tracks = {track("trk1"), track("trk2"), track("trk3"), master};
    fixture.timeline.score.clips = {
        {"a", "trk1", "voice", seconds(0.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1},
        {"b", "trk2", "voice", seconds(0.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1},
    };
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Timeline grid fixture was rejected: " + replaced.message);
    documentChanged();
    timelinePanel_->refresh();
    const auto& score = projectBridge_.editor().document().timeline.score;
    const auto clip = [&score](std::string_view id) {
        return *std::ranges::find(score.clips, id, &talas::project::ScoreClip::clipId);
    };
    const auto rate = static_cast<double>(timelinePanel_->sampleRateForSmoke());
    const auto frameAt = [rate](double time) { return static_cast<std::uint64_t>(std::llround(time * rate)); };

    timelinePanel_->selectForSmoke({talas::app::TimelineScoreSelectionKind::Clip, "a", 0});
    if (!timelinePanel_->keyForSmoke(juce::KeyPress{juce::KeyPress::downKey, juce::ModifierKeys::altModifier, 0})
        || clip("a").trackId != "trk3")
        return finish("the down arrow did not move the Sound over the occupied track below it");

    if (!timelinePanel_->setGridForSmoke(talas::app::TimelineGridMode::Magnet, {1, 16})
        || !timelinePanel_->dropSoundOnTrackForSmoke("voice", "trk2", frameAt(5.0)) || score.clips.size() != 3
        || score.clips.back().start != seconds(2.0))
        return finish("a Sound dropped on a Magnet grid did not stick to the Sound before it");
    if (!timelinePanel_->setGridForSmoke(talas::app::TimelineGridMode::Quantized, {1, 4})
        || !timelinePanel_->dropSoundOnTrackForSmoke("voice", "trk1", frameAt(1.03)) || score.clips.size() != 4
        || std::abs(score.clips.back().start.seconds - 1.0) > 1e-9)
        return finish("a Sound dropped on a Quantized grid did not land on the quantization");
    const auto dropped = score.clips.back().clipId;
    timelinePanel_->selectForSmoke({talas::app::TimelineScoreSelectionKind::Clip, dropped, 0});
    if (!timelinePanel_->keyForSmoke(juce::KeyPress{juce::KeyPress::rightKey, juce::ModifierKeys::altModifier, 0})
        || std::abs(clip(dropped).start.seconds - 1.125) > 1e-9)
        return finish("the arrow did not nudge the Sound by one quantum, a quarter of a beat");
    // A Sound dropped below the last track makes a
    // new track for it.
    const auto tracksBefore = score.tracks.size();
    const auto clipsBefore = score.clips.size();
    if (!timelinePanel_->dropSoundBelowTracksForSmoke("voice", frameAt(2.0)) || score.tracks.size() != tracksBefore + 1
        || score.clips.size() != clipsBefore + 1 || score.clips.back().trackId != score.tracks.back().trackId)
        return finish("a Sound dropped below the tracks did not make a new track for it");

    // Zoom out, zoom in and zoom to fit.
    auto* const scoreView = timelinePanel_->findChildWithID("timeline.score");
    auto* const zoom = scoreView != nullptr ? dynamic_cast<juce::Slider*>(scoreView->findChildWithID("timeline.score.zoom")) : nullptr;
    const auto press = [scoreView](const char* id) {
        auto* const button = scoreView != nullptr ? dynamic_cast<juce::Button*>(scoreView->findChildWithID(id)) : nullptr;
        if (button == nullptr || !button->onClick)
            return false;
        button->onClick();
        return true;
    };
    if (zoom == nullptr || !press("timeline.score.zoom-in") || !(zoom->getValue() > 1.0) || !press("timeline.score.zoom-fit")
        || zoom->getValue() != 1.0 || !press("timeline.score.zoom-out") || !(zoom->getValue() < 1.0))
        return finish("the zoom buttons did not zoom in, fit the Timeline and zoom out");
    (void) press("timeline.score.zoom-fit");

    // A group of Sounds, Shift+clicked together and
    // moved by typing the group's start.
    timelinePanel_->selectForSmoke({talas::app::TimelineScoreSelectionKind::Clip, "b", 0});
    if (!timelinePanel_->shiftClickClipForSmoke("a") || timelinePanel_->selectedClipsForSmoke().size() != 2)
        return finish("Shift+click did not add a Sound to the selected group");
    const talas::app::TimelineScoreSelection groupSelection{talas::app::TimelineScoreSelectionKind::Clip, "b", 0};
    if (!timelinePanel_->enterInfoFieldForSmoke(groupSelection, TimelineInfoField::SoundStart, "3")
        || clip("a").start != seconds(3.0) || clip("b").start != seconds(3.0))
        return finish("typing the group's start did not move the group together");

    // Enter beat or time starts a Sound put into a
    // track at the time cursor, its Start field open for its time to be
    // typed by hand.
    const auto cursor = timelinePanel_->infoFieldTextForSmoke(groupSelection, TimelineInfoField::Cursor);
    if (!timelinePanel_->setGridForSmoke(talas::app::TimelineGridMode::EnterTime, {1, 4})
        || !timelinePanel_->dropSoundOnTrackForSmoke("voice", "trk3", frameAt(9.0))
        || timelinePanel_->typingInfoFieldForSmoke() != TimelineInfoField::SoundStart
        || timelinePanel_->infoFieldTextForSmoke(
               {talas::app::TimelineScoreSelectionKind::Clip, score.clips.back().clipId, 0}, TimelineInfoField::SoundStart)
            != cursor)
        return finish("a Sound put in with Enter beat or time did not start at the cursor with its Start field open");
    return finish();
#else
    return std::nullopt;
#endif
}
}
