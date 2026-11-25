#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
// A Timeline lane for any of a track's
// controls, curves between its points, and the controller transformations.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runTimelineAutomationSmoke()
{
#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
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
            error = "Timeline automation smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectDocument fixture;
    fixture.virtualControls.push_back({"Rate", talas::expr::ValueDomain::Scalar});
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
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture);
        !replaced.ok())
    {
        return finish("Timeline automation fixture was rejected: " + replaced.message);
    }
    documentChanged();
    const talas::app::TimelineScoreSelection trackSelection{
        talas::app::TimelineScoreSelectionKind::Track, "track1", 0};
    if (timelinePanel_->automateMenuForSmoke(trackSelection)
        != std::vector<juce::String>{"Gain", "Level", "Angle", "Radius", "Rate"})
        return finish("the Automate menu did not offer the track's Gain, Level, Angle, Radius and Rate");
    if (!timelinePanel_->automateForSmoke(trackSelection, "Rate"))
        return finish("Automate > Rate did not add a lane");
    const auto& automation = projectBridge_.editor().document().timeline.score.automation;
    if (automation.size() != 1 || automation.front().eventName != "Rate"
        || automation.front().ownerId != "track1")
        return finish("the Rate lane did not belong to the track");
    if (timelinePanel_->automateMenuForSmoke(trackSelection) != std::vector<juce::String>{"Gain", "Level", "Angle", "Radius"})
        return finish("the Automate menu still offered the automated Rate");
    auto lane = automation.front();
    talas::project::ScoreTime start;
    talas::project::ScoreTime end;
    end.seconds = 2.0;
    lane.points = {
        {start, 0.0, talas::core::AutomationInterpolation::Step},
        {end, 1.0, talas::core::AutomationInterpolation::Step},
    };
    if (!projectBridge_.editor().updateScoreAutomationLane(lane).ok())
        return finish("the Rate lane's points were refused");
    documentChanged();
    const talas::app::TimelineScoreSelection pointSelection{
        talas::app::TimelineScoreSelectionKind::AutomationPoint, lane.laneId, 0};
    if (!timelinePanel_->shapeAutomationForSmoke(
            pointSelection, talas::app::AutomationCurveShape::SCurve)
        || automation.front().points.size() != talas::app::automationCurveSteps + 1)
        return finish("Curve to Next Point did not draw an S-curve");
    const auto curved = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    const talas::app::TimelineScoreSelection laneSelection{
        talas::app::TimelineScoreSelectionKind::AutomationLane, lane.laneId, 0};
    // The transformations pass the function
    // through them without changing its points, combine, and pressed
    // again are taken away.
    const auto shown = [&] {
        return talas::app::automationShownPoints(projectBridge_.editor().document().timeline.score, automation.front());
    };
    const auto points = automation.front().points;
    if (!timelinePanel_->transformAutomationForSmoke(
            laneSelection, talas::app::AutomationTransform::Loop, {6.0})
        || shown().back().position.seconds != 6.0 || automation.front().points != points)
        return finish("Loop did not repeat the curve to 6 s without changing its points");
    if (!timelinePanel_->transformAutomationForSmoke(
            laneSelection, talas::app::AutomationTransform::Invert, {0.5})
        || shown().front().value != 1.0 || automation.front().transforms.size() != 2)
        return finish("Invert did not mirror the looped lane about 0.5");
    if (!timelinePanel_->transformAutomationForSmoke(laneSelection, talas::app::AutomationTransform::Invert, {})
        || shown().front().value != 0.0 || automation.front().transforms.size() != 1)
        return finish("pressing Invert again did not take it away");
    undoProject();
    undoProject();
    undoProject();
    if (talas::project::serializeProjectDocument(projectBridge_.editor().document()) != curved)
        return finish("three Undos did not take back Invert, Invert and Loop");

    // An Automated lane's control leaves
    // the virtual control surface and the Lane State menu's Live brings it
    // back; the Automation menu record-enables every track's level and
    // angle when no Sound is selected.
    documentChanged();
    if (virtualControlSurface_->controlPresentationForSmoke("Rate").has_value())
        return finish("the automated Rate still showed in the virtual control surface");
    if (!timelinePanel_->invokeLaneStateMenuActionForSmoke(laneSelection, timelineLaneStateMenuBase + 1)
        || automation.front().state != talas::project::ScoreAutomationState::Live)
        return finish("Lane State > Live did not set the Rate lane live");
    documentChanged();
    if (!virtualControlSurface_->controlPresentationForSmoke("Rate").has_value())
        return finish("the live Rate did not come back to the virtual control surface");
    if (!timelinePanel_->automateForSmoke(trackSelection, "Gain") || automation.size() != 2)
        return finish("Automate > Gain did not add a lane");
    const talas::app::TimelineScoreSelection nothing{};
    if (!timelinePanel_->invokeLaneStateMenuActionForSmoke(
            nothing, timelineAutomationMenuBase + 2 * static_cast<int>(talas::project::ScoreAutomationState::Record) + 1)
        || automation.back().eventName != "Gain" || automation.back().state != talas::project::ScoreAutomationState::Record
        || automation.front().state != talas::project::ScoreAutomationState::Live)
        return finish("Automation > Record Enable > Track Level & Angle Controls did not record-enable the track's Gain alone");
    if (!timelinePanel_->invokeLaneStateMenuActionForSmoke(
            nothing, timelineAutomationMenuBase + 2 * static_cast<int>(talas::project::ScoreAutomationState::Automated) + 1)
        || automation.back().state != talas::project::ScoreAutomationState::Automated)
        return finish("Automation > Automated > Track Level & Angle Controls did not automate the Gain again");
    return finish();
#else
    return std::nullopt;
#endif
}
}
