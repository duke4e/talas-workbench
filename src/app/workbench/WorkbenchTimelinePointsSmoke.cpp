#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/TimelineGrid.h"

namespace talas::app::workbench
{
// The points of a control boxed, set to a
// typed value, moved by typing the earliest's time, nudged with an arrow,
// deleted together, a point Ctrl+Clicked in at a height, and Ctrl+A.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runTimelinePointsSmoke()
{
#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousMode = workspaceMode_;
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        activateWorkspaceMode(previousMode);
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "Timeline points smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    const auto seconds = [](double value) {
        return talas::project::ScoreTime{talas::project::ScoreTimeDomain::Seconds, value, 0};
    };
    talas::project::ProjectDocument fixture;
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
    talas::project::ScoreAutomationLane rate;
    rate.laneId = "rate";
    rate.ownerId = "track1";
    rate.eventName = "Rate";
    const auto linear = talas::core::AutomationInterpolation::Linear;
    rate.points = {{seconds(0.0), 0.0, linear}, {seconds(1.0), 0.2, linear}, {seconds(2.0), 0.4, linear},
        {seconds(3.0), 0.6, linear}};
    fixture.timeline.score.automation = {rate};
    talas::project::ProjectSoundDefinition honey;
    honey.definitionId = "honey";
    honey.displayName = "Honey";
    honey.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    honey.outputs.push_back({"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    fixture.soundDefinitions.push_back(std::move(honey));
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("Timeline points fixture was rejected: " + replaced.message);
    documentChanged();
    // The box is drawn over the lane as the Timeline shows it.
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Automation);
    timelinePanel_->refresh();
    const auto points = [this] { return projectBridge_.editor().document().timeline.score.automation.front().points; };
    const talas::app::TimelineScoreSelection first{talas::app::TimelineScoreSelectionKind::AutomationPoint, "rate", 1};

    if (!timelinePanel_->boxLaneForSmoke("rate") || timelinePanel_->selectedPointsForSmoke().size() != 4)
        return finish("a box drawn around the points did not select them all");
    if (!timelinePanel_->selectPointsForSmoke("rate", {1, 2}) || !timelinePanel_->setSelectedPointsValueForSmoke("0.9")
        || points()[1].value != 0.9 || points()[2].value != 0.9 || points()[3].value != 0.6)
        return finish("Enter and a value did not set the selected points to it");
    if (timelinePanel_->infoFieldTextForSmoke(first, TimelineInfoField::PointValue) != "0.9"
        || timelinePanel_->infoFieldTextForSmoke(first, TimelineInfoField::PointTime) != "1")
        return finish("the fields did not show the earliest selected point's time and value");
    if (!timelinePanel_->enterInfoFieldForSmoke(first, TimelineInfoField::PointTime, "1.5")
        || points()[1].position != seconds(1.5) || points()[2].position != seconds(2.5)
        || timelinePanel_->selectedPointsForSmoke() != std::vector<std::size_t>{1, 2})
        return finish("typing the earliest point's time did not move the selected points together");
    // One quantum, 1/16 of a beat as the Timeline starts.
    const auto quantum = talas::app::timelineQuantum({}, talas::app::TimelineRulerMode::BarsBeatsTicks,
        talas::project::ScoreTimeDomain::Seconds, projectBridge_.editor().document().timeline.musicalTime, 30.0).seconds;
    if (!timelinePanel_->keyForSmoke(juce::KeyPress{juce::KeyPress::rightKey})
        || std::abs(points()[1].position.seconds - 1.5 - quantum) > 1e-9
        || std::abs(points()[2].position.seconds - 2.5 - quantum) > 1e-9)
        return finish("the right arrow did not nudge the selected points");
    if (!timelinePanel_->keyForSmoke(juce::KeyPress{juce::KeyPress::deleteKey}) || points().size() != 2)
        return finish("Delete did not take the selected points out");
    if (!timelinePanel_->addPointForSmoke("rate", 48000, 0.5) || points().size() != 3
        || std::abs(points()[1].value - 0.3) > 1e-9)
        return finish("Ctrl+Click did not add a point at the value of its height");
    if (!timelinePanel_->keyForSmoke(juce::KeyPress{'a', juce::ModifierKeys::commandModifier, 'a'})
        || timelinePanel_->selectedPointsForSmoke().size() != 3)
        return finish("Ctrl+A did not select all of the lane's points");

    // New Take keeps take1, Rename take, and the
    // Source menu back to take1.
    const auto lane = [this] { return projectBridge_.editor().document().timeline.score.automation.front(); };
    const auto take1 = lane().points;
    timelinePanel_->selectForSmoke({talas::app::TimelineScoreSelectionKind::AutomationLane, "rate", 0});
    if (!timelinePanel_->newTakeForSmoke() || lane().takeName != "take2" || lane().takes.size() != 1)
        return finish("New Take did not make take2 and keep take1");
    if (!timelinePanel_->addPointForSmoke("rate", 24000, 1.0) || lane().points == take1)
        return finish("take2 could not be edited apart from take1");
    timelinePanel_->selectForSmoke({talas::app::TimelineScoreSelectionKind::AutomationLane, "rate", 0});
    if (!timelinePanel_->renameTakeForSmoke("rise") || lane().takeName != "rise")
        return finish("Rename take did not rename the take");
    if (!timelinePanel_->chooseTakeForSmoke("take1") || lane().takeName != "take1" || lane().points != take1)
        return finish("the Source menu did not return to take1");

    // New Master, and Rate slaved to it, then
    // freed.
    timelinePanel_->selectForSmoke({});
    const auto& automation = projectBridge_.editor().document().timeline.score.automation;
    if (!timelinePanel_->newMasterControlForSmoke("ReverbMaster") || automation.size() != 2
        || automation.back().scope != talas::project::ScoreAutomationScope::Master || automation.back().eventName != "ReverbMaster")
        return finish("New Master did not make the master control ReverbMaster");
    const auto masterId = automation.back().laneId;
    timelinePanel_->selectForSmoke({talas::app::TimelineScoreSelectionKind::AutomationLane, "rate", 0});
    if (!timelinePanel_->slaveLaneForSmoke(masterId) || lane().masterLaneId != masterId)
        return finish("Slave to did not slave Rate to ReverbMaster");
    timelinePanel_->selectForSmoke({talas::app::TimelineScoreSelectionKind::AutomationLane, "rate", 0});
    if (!timelinePanel_->slaveLaneForSmoke("") || !lane().masterLaneId.empty())
        return finish("Rate could not be freed from its master");

    // Honey's amplitude envelope extracted into
    // Rate, a new take.
    talas::runtime::EnvelopeExtractionRequest request;
    request.soundDefinitionId = "honey";
    request.durationSeconds = 0.2;
    request.sampleRate = 8000.0;
    const auto takesBefore = lane().takes.size();
    if (!timelinePanel_->extractEnvelopeForSmoke("rate", request) || lane().takes.size() != takesBefore + 1 || lane().points.size() < 2)
        return finish("a Sound dropped into Rate's lane did not give its amplitude envelope as a new take");
    return finish();
#else
    return std::nullopt;
#endif
}
}
