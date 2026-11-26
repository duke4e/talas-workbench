#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/TimelineGrid.h"

namespace talas::app::workbench
{
void WorkbenchComponent::addTimelineScoreGainFixture(
    talas::project::ProjectSoundDefinition& definition)
{
    talas::project::ProjectSoundExposedParameter gain;
    gain.parameterId = "scoreGain";
    gain.displayName = "Score Gain";
    gain.kind = talas::core::ParameterKind::Expression;
    gain.domain = talas::expr::ValueDomain::Scalar;
    gain.defaultValue = expressionParameter("scoreGain", "1.0");
    gain.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::EventValue,
        {},
        {},
        "Gain",
        talas::expr::ValueDomain::Scalar,
    });
    definition.exposedParameters.push_back(std::move(gain));
}

[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runTimelineScoreShellSmoke(
    const std::filesystem::path& smokeDirectory,
    std::string_view editableDefinitionId)
{
    const auto require = [](bool condition, std::string message)
        -> std::optional<std::string> {
        if (!condition)
            return message;
        return {};
    };
    const auto snapshot = [&](std::string_view fileName)
        -> std::optional<std::string> {
        const auto image = createComponentSnapshot(
            getLocalBounds(),
            true,
            1.0F);
        if (!image.isValid())
            return "Workbench Timeline snapshot did not produce a valid image";
        const auto path = (smokeDirectory / fileName).lexically_normal();
        auto stream = juce::File{juceString(path.string())}.createOutputStream();
        if (stream == nullptr || !stream->openedOk())
            return "Workbench Timeline snapshot output could not be opened";
        if (!stream->setPosition(0) || stream->truncate().failed())
            return "Workbench Timeline snapshot output could not be replaced";
        if (!juce::PNGImageFormat{}.writeImageToStream(image, *stream))
            return "Workbench Timeline snapshot PNG encoding failed";
        stream->flush();
        return {};
    };

    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Automation);
    timelinePanel_->refresh();
    auto* timelineScore = timelinePanel_->findChildWithID(
        "timeline.score");
    auto* const timelineRuler = timelineScore != nullptr
        ? timelineScore->findChildWithID("timeline.score.ruler")
        : nullptr;
    auto* const timelineZoom = timelineScore != nullptr
        ? timelineScore->findChildWithID("timeline.score.zoom")
        : nullptr;
    if (auto error = require(
            !timelinePanel_->getBounds().isEmpty()
                && timelineScore != nullptr
                && timelineRuler != nullptr
                && timelineZoom != nullptr
                && timelineScore->isVisible()
                && !timelineScore->getBounds().isEmpty()
                && timelineScore->getTitle().containsIgnoreCase(
                    "Score is empty")
                && timelineRuler->isVisible()
                && timelineZoom->isVisible(),
            "Automation workspace did not expose the virtualized Score shell and its explicit empty state: panel="
                + timelinePanel_->getBounds().toString().toStdString()
                + ", score="
                + (timelineScore != nullptr
                       ? timelineScore->getBounds().toString().toStdString()
                       : std::string{"missing"})
                + ", showing="
                + (timelineScore != nullptr && timelineScore->isShowing()
                       ? "true" : "false")
                + ", title="
                + (timelineScore != nullptr
                       ? timelineScore->getTitle().toStdString()
                       : std::string{"missing"})))
    {
        return error;
    }
    timelinePanel_->focusTimeline();
    if (auto error = require(
            timelineScore->getWantsKeyboardFocus(),
            "Timeline Score surface did not retain a keyboard-focus target"))
    {
        return error;
    }
    if (auto error = snapshot("workbench-timeline-score-empty.png"))
        return error;

    if (auto error = require(
            timelineScore->keyPressed(juce::KeyPress{'n'}),
            "keyboard Score action did not expose empty-state track bootstrap"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().document().timeline.score.tracks.size() == 2,
            "empty-state Add Track did not author one routed track and one master"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && projectBridge_.editor().document().timeline.score
                    == talas::project::ProjectScore{},
            "empty-state Add Track did not remain one isolated Undo"))
    {
        return error;
    }
    timelinePanel_->refresh();

    auto scoreSourceFixtureBeforeDocument =
        projectBridge_.editor().document();
    auto scoreSourceFixtureBefore =
        talas::project::serializeProjectDocument(
            scoreSourceFixtureBeforeDocument);
    auto scoreSourceFixtureAdded = false;
    auto scoreSourceInstanceId = std::string{};
    const auto existingScoreSource = std::ranges::find_if(
        projectBridge_.editor().document().graph.nodes,
        [&](const auto& candidate) {
            return candidate.soundDefinitionId
                == std::optional<std::string>{
                    std::string{editableDefinitionId}};
        });
    if (existingScoreSource
        != projectBridge_.editor().document().graph.nodes.end())
    {
        scoreSourceInstanceId = existingScoreSource->instanceId;
    }
    else
    {
        auto sourceDocument = projectBridge_.editor().document();
        const auto definition = std::ranges::find(
            sourceDocument.soundDefinitions,
            editableDefinitionId,
            &talas::project::ProjectSoundDefinition::definitionId);
        if (auto error = require(
                definition != sourceDocument.soundDefinitions.end(),
                "Score shell fixture lost its extracted Sound definition"))
        {
            return error;
        }
        scoreSourceInstanceId = "timelineScoreSourceInstance";
        talas::project::ProjectNode instance;
        instance.instanceId = scoreSourceInstanceId;
        instance.soundDefinitionId = std::string{editableDefinitionId};
        sourceDocument.graph.nodes.push_back(std::move(instance));
        for (std::size_t index = 0;
             index < definition->inputs.size();
             ++index)
        {
            const auto& input = definition->inputs[index];
            talas::project::ProjectNode source;
            source.instanceId = "timelineScoreInput"
                + std::to_string(index);
            std::string sourcePort;
            if (input.type == talas::core::PortType::Audio)
            {
                source.typeId = "talas.noise";
                sourcePort = "audioOut";
            }
            else if (input.type == talas::core::PortType::Control)
            {
                source.typeId = "talas.constant";
                sourcePort = "value";
            }
            else
            {
                return "Score shell connected-source fixture supports only the extracted Audio/Control boundary";
            }
            sourceDocument.graph.nodes.push_back(source);
            sourceDocument.graph.connections.push_back({
                source.instanceId,
                std::move(sourcePort),
                scoreSourceInstanceId,
                input.portId,
            });
        }
        const auto sourceEdit = projectBridge_.editor().applyDocumentEdit(
            std::move(sourceDocument));
        if (auto error = require(
                sourceEdit.ok(),
                "Score shell could not author its connected source fixture: "
                    + sourceEdit.message))
        {
            return error;
        }
        scoreSourceFixtureAdded = true;
        documentChanged();
        timelinePanel_->refresh();
    }

    talas::project::ProjectScoreEditBatch scoreSmokeEdit;
    auto scoreSmokeBeforeDocument = projectBridge_.editor().document();
    auto scoreSmokeBefore = talas::project::serializeProjectDocument(
        scoreSmokeBeforeDocument);
    scoreSmokeEdit.tracks = {
        {"scoreVoice", "Signal Voice", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::Seconds, "scoreMaster", 2,
         talas::project::ScoreOverlapMode::Layer},
        {"scoreMaster", "Master", talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds, {}, 2,
         talas::project::ScoreOverlapMode::Layer},
    };
    scoreSmokeEdit.clips = {
        {"scoreClip", "scoreVoice", std::string{editableDefinitionId},
         {talas::project::ScoreTimeDomain::Seconds, 0.5, 0},
         {talas::project::ScoreTimeDomain::Seconds, 2.0, 0},
         {talas::project::ScoreTimeDomain::Seconds, 0.0, 0},
         {talas::project::ScoreTimeDomain::Seconds, 0.1, 0},
         {talas::project::ScoreTimeDomain::Seconds, 0.2, 0},
         2},
    };
    scoreSmokeEdit.clips.front().sourceInstanceId =
        scoreSourceInstanceId;
    scoreSmokeEdit.automation = {
        {"scoreGain", talas::project::ScoreAutomationScope::Track, "scoreVoice",
         "Gain", talas::expr::ValueDomain::Scalar,
         {{{talas::project::ScoreTimeDomain::Seconds, 0.25, 0}, 0.25,
           talas::core::AutomationInterpolation::Step},
          {{talas::project::ScoreTimeDomain::Seconds, 0.75, 0}, 0.75,
           talas::core::AutomationInterpolation::Step}}},
    };
    scoreSmokeEdit.markers = {
        {"scoreMarker", "Entry",
         {talas::project::ScoreTimeDomain::Seconds, 1.0, 0},
         std::nullopt},
    };
    const auto scoreSmokeResult = projectBridge_.editor().applyScoreEdits(
        std::move(scoreSmokeEdit));
    if (auto error = require(
            scoreSmokeResult.ok(),
            "Score shell fixture was rejected: " + scoreSmokeResult.message))
    {
        return error;
    }
    timelinePanel_->refresh();
#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    auto unavailableSourceClip = projectBridge_.editor().document()
                                     .timeline.score.clips.front();
    unavailableSourceClip.clipId = "unboundScoreClip";
    unavailableSourceClip.start.seconds = 5.0;
    unavailableSourceClip.sourceInstanceId.reset();
    if (auto error = require(
            projectBridge_.editor().addScoreClip(
                std::move(unavailableSourceClip)).ok(),
            "Score shell could not prepare its unavailable-source action fixture"))
    {
        return error;
    }
    documentChanged();
    timelinePanel_->refresh();
    const auto unavailableSourceState =
        timelinePanel_->clipArtifactStateForSmoke("unboundScoreClip");
    const auto unavailableActionDispatched =
        timelinePanel_->invokeClipArtifactActionForSmoke(
            "unboundScoreClip",
            TimelineScoreClipArtifactAction::Freeze);
    if (auto error = require(
            unavailableSourceState.kind
                    == TimelineScoreClipArtifactStateKind::Invalid
                && !unavailableSourceState.sourceAvailable
                && unavailableActionDispatched
                && !scoreClipArtifactRunning_
                && statusLabel_.getText().containsIgnoreCase("unavailable"),
            "Timeline did not disable and fail closed an unbound external-input clip artifact action"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok(),
            "Unavailable-source action fixture did not remain one isolated Undo"))
    {
        return error;
    }
    documentChanged();
    timelinePanel_->refresh();
#endif
    auto* scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    auto* const scoreTrack = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.track:scoreVoice")
        : nullptr;
    auto* scoreClip = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.clip:scoreClip")
        : nullptr;
    auto* const scoreMarker = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.marker:scoreMarker")
        : nullptr;
    auto scoreTraverser = timelineScore->createFocusTraverser();
    const auto scoreFocusOrder = scoreTraverser != nullptr
        ? scoreTraverser->getAllComponents(timelineScore)
        : std::vector<juce::Component*>{};
    const auto clipFocus = std::ranges::find(
        scoreFocusOrder,
        scoreClip);
    const auto markerFocus = std::ranges::find(
        scoreFocusOrder,
        scoreMarker);
    if (auto error = require(
            timelineScore->getTitle().contains("3 rows")
                && timelineScore->getTitle().contains("5 visible items")
                && timelineScore->getComponentID() == "timeline.score"
                && scoreAccessibility != nullptr
                && scoreTrack != nullptr
                && scoreClip != nullptr
                && scoreMarker != nullptr
                && scoreTrack->isAccessible()
                && scoreClip->isAccessible()
                && scoreMarker->isAccessible()
                && clipFocus != scoreFocusOrder.end()
                && markerFocus != scoreFocusOrder.end()
                && clipFocus < markerFocus,
            "virtualized Score shell did not publish exact visible track, clip, and marker content: "
                + timelineScore->getTitle().toStdString()
                + ", rootId=" + timelineScore->getComponentID().toStdString()
                + ", container=" + (scoreAccessibility != nullptr ? "yes" : "no")
                + ", track=" + (scoreTrack != nullptr ? "yes" : "no")
                + ", clip=" + (scoreClip != nullptr ? "yes" : "no")
                + ", marker=" + (scoreMarker != nullptr ? "yes" : "no")
                + ", trackAT=" + (scoreTrack != nullptr
                        && scoreTrack->isAccessible() ? "yes" : "no")
                + ", clipAT=" + (scoreClip != nullptr
                        && scoreClip->isAccessible() ? "yes" : "no")
                + ", markerAT=" + (scoreMarker != nullptr
                        && scoreMarker->isAccessible() ? "yes" : "no")
                + ", focusCount=" + std::to_string(scoreFocusOrder.size())))
    {
        return error;
    }
    if (auto error = snapshot("workbench-timeline-score-content.png"))
        return error;

#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    const auto liveArtifactState =
        timelinePanel_->clipArtifactStateForSmoke("scoreClip");
    auto unsavedCacheTruth = true;
    if (!projectPath().has_value())
    {
        unsavedCacheTruth =
            timelinePanel_->invokeClipArtifactActionForSmoke(
                "scoreClip", TimelineScoreClipArtifactAction::Cache)
            && !scoreClipArtifactRunning_
            && statusLabel_.getText().containsIgnoreCase(
                "save the project");
    }
    if (auto error = require(
            liveArtifactState.kind
                    == TimelineScoreClipArtifactStateKind::Live
                && !liveArtifactState.detail.empty()
                && unsavedCacheTruth,
            "native Timeline did not project the Live artifact authority or truthfully reject an unsaved cache request"))
    {
        return error;
    }

    const auto artifactProjectPath = (
        smokeDirectory / "timeline-score-artifact.talas.json")
        .lexically_normal();
    const auto artifactPreviousProjectPath = projectPath();
    const auto artifactBeforeSave = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    if (!saveProjectToPath(artifactProjectPath))
    {
        return "Score clip artifact smoke could not establish a saved project authority: "
            + statusLabel_.getText().toStdString();
    }
    const auto artifactBaseline = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    if (artifactBaseline != artifactBeforeSave)
    {
        static_cast<void>(talas::project::rebaseProjectAssetPaths(
            scoreSmokeBeforeDocument,
            artifactPreviousProjectPath,
            artifactProjectPath));
        scoreSmokeBefore = talas::project::serializeProjectDocument(
            scoreSmokeBeforeDocument);
        static_cast<void>(talas::project::rebaseProjectAssetPaths(
            scoreSourceFixtureBeforeDocument,
            artifactPreviousProjectPath,
            artifactProjectPath));
        scoreSourceFixtureBefore = talas::project::serializeProjectDocument(
            scoreSourceFixtureBeforeDocument);
    }
    if (auto error = require(
            timelinePanel_->invokeClipArtifactActionForSmoke(
                "scoreClip", TimelineScoreClipArtifactAction::Freeze),
            "native Timeline did not dispatch Freeze through the shared clip-artifact callback"))
    {
        return error;
    }
    const auto startingArtifactState =
        timelinePanel_->clipArtifactStateForSmoke("scoreClip");
    if (auto error = require(
            scoreClipArtifactRunning_
                && startingArtifactState.kind
                    == TimelineScoreClipArtifactStateKind::Freezing
                && timelinePanel_->cacheLookForSmoke("scoreClip") == talas::app::TimelineCacheLook::Caching,
            "Freeze did not publish an immediate bounded in-progress Timeline state"))
    {
        return error;
    }
    const auto artifactDeadline = juce::Time::getMillisecondCounterHiRes()
        + 10'000.0;
    while (scoreClipArtifactRunning_
           && juce::Time::getMillisecondCounterHiRes() < artifactDeadline)
    {
        juce::Thread::sleep(1);
        finishScoreClipArtifactPublicationIfReady();
    }
    if (auto error = require(
            !scoreClipArtifactRunning_,
            "Score clip Freeze worker exceeded its bounded smoke deadline"))
    {
        return error;
    }
    const auto frozenArtifactState =
        timelinePanel_->clipArtifactStateForSmoke("scoreClip");
    const auto frozenClip = std::ranges::find(
        projectBridge_.editor().document().timeline.score.clips,
        std::string_view{"scoreClip"},
        &talas::project::ScoreClip::clipId);
    auto frozenFilesAvailable = frozenClip
        != projectBridge_.editor().document().timeline.score.clips.end()
        && frozenClip->renderArtifact.has_value()
        && frozenClip->renderArtifact->mode
            == talas::project::ScoreClipRenderMode::Frozen
        && !frozenClip->renderArtifact->channelAssetIds.empty();
    if (frozenFilesAvailable)
    {
        for (const auto& assetId :
             frozenClip->renderArtifact->channelAssetIds)
        {
            const auto asset = std::ranges::find(
                projectBridge_.editor().document().assets,
                assetId,
                &talas::project::ProjectAsset::id);
            std::error_code statusError;
            frozenFilesAvailable = frozenFilesAvailable
                && asset != projectBridge_.editor().document().assets.end()
                && std::filesystem::is_regular_file(
                    talas::project::resolveProjectAssetPath(
                        *asset, projectPath()),
                    statusError)
                && !statusError;
        }
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreClip = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.clip:scoreClip")
        : nullptr;
    if (auto error = require(
            frozenArtifactState.kind
                    == TimelineScoreClipArtifactStateKind::Frozen
                && frozenFilesAvailable
                && scoreClip != nullptr
                && scoreClip->getTitle().containsIgnoreCase("frozen"),
            "verified frozen bytes were not projected through Timeline text/accessibility authority: status="
                + statusLabel_.getText().toStdString()))
    {
        return error;
    }
    // The Cache menu shows a cached Sound blue, and
    // its Live plays the Sound live again, one Undo.
    if (auto error = require(
            timelinePanel_->cacheLookForSmoke("scoreClip") == talas::app::TimelineCacheLook::Cached
                && timelinePanel_->invokeClipArtifactActionForSmoke("scoreClip", TimelineScoreClipArtifactAction::Live)
                && !projectBridge_.editor().document().timeline.score.clips.front().renderArtifact.has_value()
                && timelinePanel_->cacheLookForSmoke("scoreClip") == talas::app::TimelineCacheLook::Live
                && projectBridge_.editor().undo().ok(),
            "the Cache menu did not show the frozen Sound as cached, or its Live did not play it live"))
    {
        return error;
    }
    documentChanged();
    if (auto error = require(
            timelinePanel_->invokeClipArtifactActionForSmoke(
                "scoreClip", TimelineScoreClipArtifactAction::Unfreeze)
                && !projectBridge_.editor().document().timeline.score
                        .clips.front().renderArtifact.has_value(),
            "native Timeline Unfreeze did not clear only the persisted artifact authority"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok(),
            "Unfreeze did not remain one isolated Undo"))
    {
        return error;
    }
    documentChanged();
    if (auto error = require(
            timelinePanel_->clipArtifactStateForSmoke("scoreClip").kind
                    == TimelineScoreClipArtifactStateKind::Frozen
                && projectBridge_.editor().undo().ok(),
            "one Undo did not restore the exact Frozen state before removing the publication"))
    {
        return error;
    }
    documentChanged();
    const auto artifactUndoDocument = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    const auto artifactUndoState =
        timelinePanel_->clipArtifactStateForSmoke("scoreClip");
    if (auto error = require(
            artifactUndoDocument == artifactBaseline
                && artifactUndoState.kind
                    == TimelineScoreClipArtifactStateKind::Live,
            "Freeze and Unfreeze did not each remain one coherent document transaction: bytes="
                + std::string{
                    artifactUndoDocument == artifactBaseline
                        ? "restored"
                        : "changed"}
                + "; state=" + artifactUndoState.label
                + "; assets="
                + std::to_string(
                    projectBridge_.editor().document().assets.size())))
    {
        return error;
    }
    timelinePanel_->refresh();
#endif

    const auto scoreInteractionBaseline = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    const auto altRight = juce::KeyPress(
        juce::KeyPress::rightKey,
        juce::ModifierKeys::altModifier,
        0);
    // The fine step is one quantum, 1/16 of a beat
    // as the Timeline starts.
    const auto fineStep = talas::app::timelineQuantum({}, talas::app::TimelineRulerMode::BarsBeatsTicks,
        talas::project::ScoreTimeDomain::Seconds, projectBridge_.editor().document().timeline.musicalTime, 30.0).seconds;
    const auto sendScorePointer = [timelineScore](
                                      juce::Point<float> pointer,
                                      int clickCount) {
        const auto eventTime = juce::Time::getCurrentTime();
        const juce::MouseEvent mouseDown{
            juce::Desktop::getInstance().getMainMouseSource(),
            pointer,
            {},
            1.0F,
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            timelineScore,
            timelineScore,
            eventTime,
            pointer,
            eventTime,
            clickCount,
            false,
        };
        timelineScore->mouseDown(mouseDown);
    };
    const auto selectScoreTarget = [timelineScore, &sendScorePointer](
                                       juce::Component& target) {
        sendScorePointer(
            timelineScore->getLocalPoint(
                &target,
                target.getLocalBounds().getCentre().toFloat()),
            1);
    };
    const auto dragScorePointer = [timelineScore](
                                      juce::Point<float> pointer,
                                      juce::Point<float> syntheticDown) {
        const auto eventTime = juce::Time::getCurrentTime();
        const juce::MouseEvent drag{
            juce::Desktop::getInstance().getMainMouseSource(),
            pointer,
            {},
            1.0F,
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            timelineScore,
            timelineScore,
            eventTime,
            syntheticDown,
            eventTime,
            1,
            true,
        };
        timelineScore->mouseDrag(drag);
        timelineScore->mouseUp(drag);
    };

    auto markerPointer = timelineScore->getLocalPoint(
        scoreTrack,
        juce::Point<float>{static_cast<float>(scoreTrack->getWidth()),
                           static_cast<float>(scoreTrack->getHeight()) * 0.5F});
    markerPointer.x += 20.0F;
    // The time ruler lies under the toolbar and the information strip.
    const auto* const infoStrip = timelineScore->findChildWithID("timeline.score.info");
    markerPointer.y = static_cast<float>(
        (infoStrip != nullptr ? infoStrip->getBottom() : timelineRuler->getBottom()) + 12);
    sendScorePointer(markerPointer, 2);
    if (auto error = require(
            projectBridge_.editor().document().timeline.score.markers.size() == 2,
            "native double-click did not create one explicit Score marker"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == scoreInteractionBaseline,
            "pointer marker creation did not remain one isolated Undo"))
    {
        return error;
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreClip = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.clip:scoreClip")
        : nullptr;
    if (auto error = require(
            scoreClip != nullptr,
            "Score clip focus identity did not restore after pointer marker Undo"))
    {
        return error;
    }
    selectScoreTarget(*scoreClip);
    const auto repeatedClipBeforeResize =
        talas::project::serializeProjectDocument(projectBridge_.editor().document());
    const auto repeatedRightEdge = timelineScore->getLocalPoint(
        scoreClip,
        juce::Point<float>{
            static_cast<float>(scoreClip->getWidth() - 1),
            static_cast<float>(scoreClip->getHeight()) * 0.5F});
    sendScorePointer(repeatedRightEdge, 1);
    dragScorePointer(
        repeatedRightEdge,
        repeatedRightEdge.translated(-12.0F, 0.0F));
    if (auto error = require(
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document()) == repeatedClipBeforeResize,
            "zero-motion repeated-clip edge resize rewrote one-cycle duration or fades"))
    {
        return error;
    }
    sendScorePointer(repeatedRightEdge, 1);
    dragScorePointer(
        repeatedRightEdge.translated(24.0F, 0.0F),
        repeatedRightEdge);
    if (auto error = require(
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document()) != repeatedClipBeforeResize
                && projectBridge_.editor().document().timeline.score
                       .clips.front().duration.seconds > 2.0,
            "native non-zero repeated-clip edge drag did not resize the clip"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == repeatedClipBeforeResize,
            "native pointer clip resize did not remain one isolated Undo"))
    {
        return error;
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreClip = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.clip:scoreClip")
        : nullptr;
    if (auto error = require(scoreClip != nullptr,
            "Score clip focus identity did not restore after pointer resize Undo"))
    {
        return error;
    }
    selectScoreTarget(*scoreClip);
    if (auto error = require(
            scoreClip->keyPressed(juce::KeyPress{']'})
                && std::abs(projectBridge_.editor().document().timeline.score
                        .clips.front().duration.seconds - (2.0 + fineStep / 2.0)) < 1.0e-12,
            "keyboard-accessible repeated-clip resize did not adjust total extent exactly"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok(),
            "keyboard clip resize did not remain one isolated Undo"))
    {
        return error;
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreClip = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.clip:scoreClip")
        : nullptr;
    if (auto error = require(scoreClip != nullptr,
            "Score clip focus did not restore after keyboard resize Undo"))
    {
        return error;
    }
    auto boundaryFadeClip = projectBridge_.editor().document()
                                .timeline.score.clips.front();
    boundaryFadeClip.fadeIn = {
        talas::project::ScoreTimeDomain::Seconds, 2.0, 0};
    boundaryFadeClip.fadeOut = {
        talas::project::ScoreTimeDomain::Seconds, 2.0, 0};
    if (auto error = require(
            projectBridge_.editor().updateScoreClip(boundaryFadeClip).ok(),
            "Score smoke could not prepare the valid full-duration fade boundary"))
    {
        return error;
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreClip = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.clip:scoreClip")
        : nullptr;
    if (auto error = require(scoreClip != nullptr,
            "Score clip focus did not survive the fade-boundary refresh"))
    {
        return error;
    }
    selectScoreTarget(*scoreClip);
    if (auto error = require(
            scoreClip->keyPressed(juce::KeyPress{'['})
                && std::abs(projectBridge_.editor().document().timeline.score
                        .clips.front().duration.seconds - (2.0 - fineStep / 2.0)) < 1.0e-12
                && projectBridge_.editor().document().timeline.score
                           .clips.front().fadeIn.seconds
                    + projectBridge_.editor().document().timeline.score
                          .clips.front().fadeOut.seconds
                    <= 2.0 * (2.0 - fineStep / 2.0) + 1.0e-12,
            "shortening a valid full-duration fade clip did not preserve the combined fade invariant"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == repeatedClipBeforeResize,
            "fade-boundary resize and fixture setup did not remain isolated Undo steps"))
    {
        return error;
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreClip = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.clip:scoreClip")
        : nullptr;
    if (auto error = require(scoreClip != nullptr,
            "Score clip focus did not restore after fade-boundary Undo"))
    {
        return error;
    }
    selectScoreTarget(*scoreClip);
    if (auto error = require(
            scoreClip->keyPressed(juce::KeyPress{'.'})
                && std::abs(projectBridge_.editor().document().timeline.score
                        .clips.front().start.seconds - (0.5 + fineStep)) < 1.0e-12
                && std::abs(projectBridge_.editor().document().timeline.score
                        .clips.front().duration.seconds - (2.0 - fineStep / 2.0)) < 1.0e-12,
            "keyboard-accessible clip trim did not preserve the repeated exclusive end"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok(),
            "keyboard clip trim did not remain one isolated Undo"))
    {
        return error;
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreClip = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.clip:scoreClip")
        : nullptr;
    if (auto error = require(
            scoreClip != nullptr && scoreClip->keyPressed(juce::KeyPress{'g'}),
            "keyboard-accessible Score seek command did not consume the exact selected position"))
    {
        return error;
    }
    selectScoreTarget(*scoreClip);
    if (auto error = require(
            timelineScore->keyPressed(altRight),
            "native pointer-selected Score clip did not admit one owned keyboard nudge"))
    {
        return error;
    }
    if (auto error = require(
            timelineScore->keyStateChanged(false),
            "native Score clip focus did not close its owned keyboard nudge"))
    {
        return error;
    }
    const auto nudgedClip = std::ranges::find(
        projectBridge_.editor().document().timeline.score.clips,
        std::string_view{"scoreClip"},
        &talas::project::ScoreClip::clipId);
    if (auto error = require(
            nudgedClip
                    != projectBridge_.editor().document().timeline.score.clips.end()
                && std::abs(nudgedClip->start.seconds - 0.5 - fineStep) < 1.0e-12,
            "native owned nudge did not move the Score clip by one quantum"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == scoreInteractionBaseline,
            "one Undo did not restore the exact pre-nudge Score bytes"))
    {
        return error;
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    auto* scorePoint = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID(
            "timeline.automation-point:scoreGain:seconds:0.25")
        : nullptr;
    if (auto error = require(
            scorePoint != nullptr,
            "native automation point did not retain its exact focus identity"))
    {
        return error;
    }
    if (auto error = require(
            scorePoint->keyPressed(juce::KeyPress{'l'}),
            "native automation point focus did not admit its curve command"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().document().timeline.score
                    .automation.front().points.front().interpolation
                == talas::core::AutomationInterpolation::Linear,
            "native automation curve command did not update the exact point authority"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == scoreInteractionBaseline,
            "automation curve change did not remain one isolated Undo"))
    {
        return error;
    }
    // Shift held as a point's drag starts keeps it to the
    // direction first taken, here upward: its value alone changes.
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scorePoint = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID(
            "timeline.automation-point:scoreGain:seconds:0.25")
        : nullptr;
    if (auto error = require(scorePoint != nullptr,
            "automation point focus did not restore after the curve Undo"))
    {
        return error;
    }
    {
        const auto pointCentre = timelineScore->getLocalPoint(
            scorePoint, scorePoint->getLocalBounds().getCentre().toFloat());
        sendScorePointer(pointCentre, 1);
        const auto eventTime = juce::Time::getCurrentTime();
        const juce::MouseEvent shiftDrag{
            juce::Desktop::getInstance().getMainMouseSource(),
            pointCentre.translated(2.0F, -16.0F),
            juce::ModifierKeys{juce::ModifierKeys::shiftModifier | juce::ModifierKeys::leftButtonModifier},
            1.0F, 0.0F, 0.0F, 0.0F, 0.0F,
            timelineScore, timelineScore, eventTime, pointCentre, eventTime, 1, true,
        };
        timelineScore->mouseDrag(shiftDrag);
        timelineScore->mouseUp(shiftDrag);
        const auto& dragged = projectBridge_.editor().document().timeline.score.automation.front().points.front();
        if (auto error = require(dragged.value > 0.25 && dragged.position.seconds == 0.25,
                "a Shift-drag upward did not change the point's value alone"))
        {
            return error;
        }
        if (auto error = require(projectBridge_.editor().undo().ok()
                    && talas::project::serializeProjectDocument(projectBridge_.editor().document())
                        == scoreInteractionBaseline,
                "a point's drag did not remain one isolated Undo"))
        {
            return error;
        }
#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
        // The lane zooms out and back in.
        if (auto error = require(timelinePanel_->zoomLaneValuesForSmoke("scoreGain", 1)
                    && timelinePanel_->zoomLaneValuesForSmoke("scoreGain", -1),
                "the lane's vertical zoom did not zoom out and back"))
        {
            return error;
        }
#endif
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    auto* scoreLane = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.automation:scoreGain")
        : nullptr;
    if (auto error = require(
            scoreLane != nullptr
                && scoreLane->keyPressed(juce::KeyPress{'p'})
                && projectBridge_.editor().document().timeline.score
                       .automation.front().points.size() == 3,
            "native automation lane command did not create an explicit playhead point"))
    {
        return error;
    }

#if defined(TALAS_WORKBENCH_TIMELINE_TEST_SUPPORT)
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreLane = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.automation:scoreGain")
        : nullptr;
    if (auto error = require(
            scoreLane != nullptr,
            "controller recording smoke could not restore the exact automation lane"))
    {
        return error;
    }
    selectScoreTarget(*scoreLane);
    const auto recordingAuthorityBaseline = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    currentDocumentStaged_ = false;
    const auto staleArmConsumed = scoreLane->keyPressed(juce::KeyPress{'e'});
    if (auto error = require(
            staleArmConsumed
                && !projectBridge_.controllerRecordingActive()
                && statusLabel_.getText().containsIgnoreCase(
                    "stage the current project"),
            "native Timeline allowed controller recording against a stale accepted stage"))
    {
        return error;
    }
    auto recordingDocument = projectBridge_.editor().document();
    const auto recordingOscillator = std::ranges::find(
        recordingDocument.graph.nodes,
        std::string_view{"talas.oscillator"},
        &talas::project::ProjectNode::typeId);
    const auto recordingOutput = std::ranges::find(
        recordingDocument.graph.nodes,
        std::string_view{"talas.audioOutput"},
        &talas::project::ProjectNode::typeId);
    auto recordingLane = std::ranges::find(
        recordingDocument.timeline.score.automation,
        std::string_view{"scoreGain"},
        &talas::project::ScoreAutomationLane::laneId);
    if (auto error = require(
            recordingOscillator != recordingDocument.graph.nodes.end()
                && recordingOutput != recordingDocument.graph.nodes.end()
                && recordingLane
                    != recordingDocument.timeline.score.automation.end(),
            "controller recording smoke could not prepare a valid exact stage"))
    {
        return error;
    }
    const auto oscillatorNode = *recordingOscillator;
    const auto outputNode = *recordingOutput;
    recordingDocument.graph.nodes = {oscillatorNode, outputNode};
    recordingDocument.graph.connections = {{
        oscillatorNode.instanceId,
        "audioOut",
        outputNode.instanceId,
        "audioIn",
    }};
    recordingDocument.soundDefinitions.clear();
    recordingDocument.timeline.score.clips.clear();
    recordingLane = std::ranges::find(
        recordingDocument.timeline.score.automation,
        std::string_view{"scoreGain"},
        &talas::project::ScoreAutomationLane::laneId);
    recordingLane->eventName = "SmokePitch";
    recordingLane->domain = talas::expr::ValueDomain::Hertz;
    const auto recordingAuthorityEdit =
        projectBridge_.editor().applyDocumentEdit(
            std::move(recordingDocument));
    if (auto error = require(
            recordingAuthorityEdit.ok(),
            "controller recording smoke exact-stage edit failed: "
                + recordingAuthorityEdit.message))
    {
        return error;
    }
    documentChanged();
    talas::runtime::RenderSettings recordingSettings;
    recordingSettings.sampleRate = 8000.0;
    recordingSettings.maxBlockFrames = 128;
    if (!stageProject(
            std::move(recordingSettings),
            "controller recording smoke"))
    {
        return "controller recording smoke could not stage the current Score: "
            + statusLabel_.getText().toStdString();
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreLane = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.automation:scoreGain")
        : nullptr;
    if (auto error = require(
            scoreLane != nullptr,
            "controller recording smoke lost the exact lane after staging"))
    {
        return error;
    }
    selectScoreTarget(*scoreLane);
    const auto recordingBaseline = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    auto recordingEventId =
        projectBridge_.realtimeEventValueIdForLatestStage(
            "SmokePitch", talas::expr::ValueDomain::Hertz);
    auto recordingStage = projectBridge_.latestStageGeneration();
    setPlaying(true);
    std::array<float, 8> recordingLeft{};
    std::array<float, 8> recordingRight{};
    std::array<float*, 2> recordingChannels{
        recordingLeft.data(), recordingRight.data()};
    const auto discardArmConsumed = recordingEventId.has_value()
        && recordingStage != 0
        && scoreLane->keyPressed(juce::KeyPress{'e'});
    const auto discardEventRecorded = discardArmConsumed
        && projectBridge_.controllerRecordingActive()
        && projectBridge_.enqueueControllerRecordingObservationAtTimeForStage(
            *recordingEventId,
            0.2,
            talas::expr::ValueDomain::Hertz,
            500'000'000,
            recordingStage,
            {
                talas::project::ControllerRecordingOrigin::Ui,
                500'000'000,
                1,
                0,
            })
        && projectBridge_.render(
            recordingChannels.data(),
            recordingChannels.size(),
            recordingLeft.size());
    const auto discardStopConsumed = discardEventRecorded
        && scoreLane->keyPressed(juce::KeyPress{'e'});
    if (discardStopConsumed)
    {
        static_cast<void>(projectBridge_.render(
            recordingChannels.data(),
            recordingChannels.size(),
            recordingLeft.size()));
    }
    const auto discardMenuBefore =
        timelinePanel_->controllerRecordingMenuStateForSmoke();
    talas::runtime::RenderSettings replacementRecordingSettings;
    replacementRecordingSettings.sampleRate = 8000.0;
    replacementRecordingSettings.maxBlockFrames = 128;
    const auto replacementStageAccepted = discardStopConsumed
        && stageProject(
            std::move(replacementRecordingSettings),
            "controller recording stale-preview smoke");
    const auto stalePreviewMenu =
        timelinePanel_->controllerRecordingMenuStateForSmoke();
    const auto stalePreviewRejected = replacementStageAccepted
        && !stalePreviewMenu.previewEnabled
        && !stalePreviewMenu.approveEnabled
        && stalePreviewMenu.discardEnabled
        && !timelinePanel_->invokeControllerRecordingMenuActionForSmoke(41);
    const auto discardMenuDispatched = stalePreviewRejected
        && discardMenuBefore.previewEnabled
        && !discardMenuBefore.approveEnabled
        && discardMenuBefore.discardEnabled
        && timelinePanel_->invokeControllerRecordingMenuActionForSmoke(43);
    const auto discardMenuAfter =
        timelinePanel_->controllerRecordingMenuStateForSmoke();
    if (auto error = require(
            discardMenuDispatched
                && !discardMenuAfter.previewEnabled
                && !discardMenuAfter.approveEnabled
                && !discardMenuAfter.discardEnabled
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == recordingBaseline,
            "native Timeline popup authority did not expose and dispatch exact raw-take discard without editing the Score"))
    {
        return error;
    }
    recordingEventId = projectBridge_.realtimeEventValueIdForLatestStage(
        "SmokePitch", talas::expr::ValueDomain::Hertz);
    recordingStage = projectBridge_.latestStageGeneration();
    const auto armConsumed = recordingEventId.has_value()
        && recordingStage != 0
        && scoreLane->keyPressed(juce::KeyPress{'e'});
    const auto firstRecorded = armConsumed
        && projectBridge_.controllerRecordingActive()
        && projectBridge_.enqueueControllerRecordingObservationAtTimeForStage(
            *recordingEventId,
            0.1,
            talas::expr::ValueDomain::Hertz,
            1'000'000'000,
            recordingStage,
            {
                talas::project::ControllerRecordingOrigin::Ui,
                1'000'000'000,
                1,
                0,
            })
        && projectBridge_.render(
            recordingChannels.data(),
            recordingChannels.size(),
            recordingLeft.size());
    const auto secondRecorded = firstRecorded
        && projectBridge_.enqueueControllerRecordingObservationAtTimeForStage(
            *recordingEventId,
            0.9,
            talas::expr::ValueDomain::Hertz,
            2'000'000'000,
            recordingStage,
            {
                talas::project::ControllerRecordingOrigin::Ui,
                2'000'000'000,
                2,
                0,
            })
        && projectBridge_.render(
            recordingChannels.data(),
            recordingChannels.size(),
            recordingLeft.size());
    const auto stopConsumed = secondRecorded
        && scoreLane->keyPressed(juce::KeyPress{'e'});
    if (stopConsumed)
    {
        static_cast<void>(projectBridge_.render(
            recordingChannels.data(),
            recordingChannels.size(),
            recordingLeft.size()));
    }
    const auto previewConsumed = stopConsumed
        && scoreLane->keyPressed(juce::KeyPress{'v'});
    const auto approvalConsumed = previewConsumed
        && scoreLane->keyPressed(
            juce::KeyPress{juce::KeyPress::returnKey});
    if (auto error = require(
            armConsumed && firstRecorded && secondRecorded
                && stopConsumed && previewConsumed && approvalConsumed,
            "native Timeline did not execute E, exact callback capture, E stop, Preview, and Approve: event="
                + std::string{recordingEventId.has_value() ? "yes" : "no"}
                + ", stage=" + std::to_string(recordingStage)
                + ", arm=" + (armConsumed ? "yes" : "no")
                + ", first=" + (firstRecorded ? "yes" : "no")
                + ", second=" + (secondRecorded ? "yes" : "no")
                + ", stop=" + (stopConsumed ? "yes" : "no")
                + ", preview=" + (previewConsumed ? "yes" : "no")
                + ", approve=" + (approvalConsumed ? "yes" : "no")
                + ", status=" + statusLabel_.getText().toStdString()))
    {
        return error;
    }
    const auto recordedLane = std::ranges::find(
        projectBridge_.editor().document().timeline.score.automation,
        std::string_view{"scoreGain"},
        &talas::project::ScoreAutomationLane::laneId);
    if (auto error = require(
            recordedLane
                    != projectBridge_.editor().document().timeline.score
                        .automation.end()
                && recordedLane->points.size() == 2
                && recordedLane->points.front().value == 0.1
                && recordedLane->points.back().value == 0.9,
            "approved controller recording preview did not replace only the exact lane with simplified points: status="
                + statusLabel_.getText().toStdString()
                + ", points="
                + (recordedLane
                           != projectBridge_.editor().document().timeline.score
                               .automation.end()
                       ? std::to_string(recordedLane->points.size())
                       : std::string{"missing"})))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == recordingBaseline,
            "approved controller recording did not remain one isolated Undo"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == recordingAuthorityBaseline,
            "controller recording smoke stage authority did not remain one isolated setup edit"))
    {
        return error;
    }
#endif
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == scoreInteractionBaseline,
            "automation point creation did not remain one isolated Undo"))
    {
        return error;
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreClip = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.clip:scoreClip")
        : nullptr;
    if (auto error = require(scoreClip != nullptr,
            "Score clip focus identity did not restore after Undo"))
    {
        return error;
    }

    (void) scoreClip->keyPressed(altRight);
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreClip = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.clip:scoreClip")
        : nullptr;
    if (scoreClip != nullptr)
        (void) scoreClip->keyPressed(altRight);
    const auto cancelled = timelineScore->keyPressed(
        juce::KeyPress{juce::KeyPress::escapeKey});
    if (auto error = require(
            cancelled,
            "Escape did not consume the active owned held-nudge transaction"))
    {
        return error;
    }
    if (auto error = require(
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document())
                == scoreInteractionBaseline,
            "Escape did not restore the exact pre-nudge Score bytes"))
    {
        return error;
    }

    const auto commandD = juce::KeyPress(
        'd', juce::ModifierKeys::commandModifier, 'd');
    if (auto error = require(
            timelineScore->keyPressed(commandD)
                && projectBridge_.editor().document().timeline.score.clips.size() == 2
                && std::abs(projectBridge_.editor().document().timeline.score
                        .clips.back().start.seconds - 4.5) < 1.0e-12,
            "native Score create command did not add one explicit clip duplicate"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == scoreInteractionBaseline,
            "clip creation did not remain one isolated Undo"))
    {
        return error;
    }
    timelinePanel_->refresh();
    timelineScore = timelinePanel_->findChildWithID("timeline.score");
    if (auto error = require(
            timelineScore != nullptr
                && timelineScore->keyPressed(juce::KeyPress{'k'})
                && projectBridge_.editor().document().timeline.score.markers.size() == 2,
            "native Score marker command did not create at the coherent playhead"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == scoreInteractionBaseline,
            "marker creation did not remain one isolated Undo"))
    {
        return error;
    }
    timelinePanel_->refresh();
    scoreAccessibility = timelineScore->findChildWithID(
        "timeline.score.accessibility");
    scoreClip = scoreAccessibility != nullptr
        ? scoreAccessibility->findChildWithID("timeline.clip:scoreClip")
        : nullptr;
    if (auto error = require(
            scoreClip != nullptr
                && scoreClip->keyPressed(
                    juce::KeyPress{juce::KeyPress::deleteKey})
                && projectBridge_.editor().document().timeline.score.clips.empty(),
            "native Score delete command did not consume the exact selected clip identity"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == scoreInteractionBaseline,
            "clip deletion did not remain one isolated Undo"))
    {
        return error;
    }
    timelinePanel_->refresh();
    const auto scoreSmokeUndo = projectBridge_.editor().undo();
    const auto scoreCleanupDocument = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    const auto scoreCleanupCommonLength = std::min(
        scoreCleanupDocument.size(), scoreSmokeBefore.size());
    const auto scoreCleanupMismatch = std::mismatch(
        scoreCleanupDocument.begin(),
        scoreCleanupDocument.begin()
            + static_cast<std::ptrdiff_t>(scoreCleanupCommonLength),
        scoreSmokeBefore.begin(),
        scoreSmokeBefore.begin()
            + static_cast<std::ptrdiff_t>(scoreCleanupCommonLength));
    const auto scoreCleanupOffset = static_cast<std::size_t>(
        std::distance(scoreCleanupDocument.begin(), scoreCleanupMismatch.first));
    if (auto error = require(
            scoreSmokeUndo.ok()
                && scoreCleanupDocument == scoreSmokeBefore,
            std::string{"Score shell fixture did not restore exact rebased project bytes in one isolated Undo step"}
                + "; tracks="
                + std::to_string(projectBridge_.editor().document()
                                     .timeline.score.tracks.size())
                + "; clips="
                + std::to_string(projectBridge_.editor().document()
                                     .timeline.score.clips.size())
                + "; equalsPreSource="
                + std::string{
                    scoreCleanupDocument == scoreSourceFixtureBefore
                        ? "true"
                        : "false"}
                + "; offset=" + std::to_string(scoreCleanupOffset)
                + "; actual="
                + scoreCleanupDocument.substr(scoreCleanupOffset, 120)
                + "; expected="
                + scoreSmokeBefore.substr(scoreCleanupOffset, 120)))
    {
        return error;
    }
    timelinePanel_->refresh();
    if (scoreSourceFixtureAdded)
    {
        if (auto error = require(
                projectBridge_.editor().undo().ok()
                    && talas::project::serializeProjectDocument(
                           projectBridge_.editor().document())
                        == scoreSourceFixtureBefore,
                "connected Score source fixture did not remain one isolated Undo"))
        {
            return error;
        }
        documentChanged();
        timelinePanel_->refresh();
    }
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    return {};
}
}
