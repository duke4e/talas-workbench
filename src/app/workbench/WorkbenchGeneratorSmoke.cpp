#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
std::optional<std::string> WorkbenchComponent::runGeneratorWorkflowSmoke(
    const std::filesystem::path& smokeDirectory)
{
    const auto graphBefore = projectBridge_.editor().document().graph;
    const auto makeProgram = [](
        std::string id,
        std::string grammar,
        std::uint64_t seed) {
        talas::project::ProjectGeneratorProgram program;
        program.generatorId = std::move(id);
        program.displayName = "Smoke Generator";
        program.source.terminals = {"note", "rest"};
        program.source.grammar = std::move(grammar);
        program.source.seed = seed;
        program.source.maximumRewritePasses = 16;
        program.source.maximumOutputSymbols = 64;
        return program;
    };

    auto phrase = makeProgram(
        "smokePhrase",
        "start: phrase\n"
        "phrase -> note phrase | rest phrase | note\n",
        0);
    auto literal = makeProgram(
        "smokeLiteral",
        "start: note rest\n",
        0);
    auto soundChain = makeProgram(
        "smokeSoundChain",
        "start: source processor\n",
        0);
    soundChain.displayName = "Smoke Sound Chain";
    soundChain.source.terminals = {"source", "processor"};
    soundChain.source.maximumRewritePasses = 1;
    soundChain.source.maximumOutputSymbols = 2;
    soundChain.output.bindings = {
        {
            "source",
            {talas::project::ProjectGeneratorGraphNodeAction{
                "talas.oscillator"}},
        },
        {
            "processor",
            {talas::project::ProjectGeneratorGraphNodeAction{
                "talas.filter"}},
        },
    };

    if (!projectBridge_.editor()
             .addSoundDefinition({"generatorSmokeVoice", "Generator Smoke Voice"})
             .ok())
    {
        return "Generator smoke Sound target could not be authored";
    }
    talas::project::ProjectScoreEditBatch targetEdits;
    targetEdits.tracks = {
        {
            "generatorSmokeTrack",
            "Generator Smoke Track",
            talas::project::ScoreTrackKind::Track,
            talas::project::ScoreTimeDomain::MusicalTicks,
            "generatorSmokeMaster",
            2,
            talas::project::ScoreOverlapMode::Layer,
        },
        {
            "generatorSmokeMaster",
            "Generator Smoke Master",
            talas::project::ScoreTrackKind::Master,
            talas::project::ScoreTimeDomain::Seconds,
            {},
            2,
            talas::project::ScoreOverlapMode::Layer,
        },
    };
    targetEdits.automation.push_back({
        "generatorSmokeGain",
        talas::project::ScoreAutomationScope::Track,
        "generatorSmokeTrack",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        {},
    });
    if (!projectBridge_.editor().applyScoreEdits(std::move(targetEdits)).ok())
        return "Generator smoke Score targets could not be authored";

    talas::project::ProjectGeneratorTerminalBinding noteOutput;
    noteOutput.terminal = "note";
    noteOutput.actions = {
        talas::project::ProjectGeneratorTimelineEventAction{
            "Pitch",
            talas::expr::ValueDomain::NoteNumber,
            60.0,
            talas::core::AutomationInterpolation::Step,
        },
        talas::project::ProjectGeneratorScoreClipAction{
            "generatorSmokeTrack",
            "generatorSmokeVoice",
            240,
            0,
            0,
            0,
            1,
        },
        talas::project::ProjectGeneratorScoreAutomationAction{
            "generatorSmokeGain",
            talas::expr::ValueDomain::Scalar,
            0.5,
            talas::core::AutomationInterpolation::Linear,
        },
        talas::project::ProjectGeneratorScoreMarkerAction{
            "Generated note",
            std::nullopt,
        },
    };
    phrase.output.timelineStepMicroseconds = 500000;
    phrase.output.scoreStepTicks = 480;
    phrase.output.bindings.push_back(std::move(noteOutput));
    documentChanged();
    inspectorPanel_->showGenerators();
    inspectorPanel_->createGeneratorForSmoke(phrase);
    inspectorPanel_->createGeneratorForSmoke(literal);
    inspectorPanel_->createGeneratorForSmoke(soundChain);
    if (!inspectorPanel_->selectGeneratorForSmoke("smokeSoundChain")
        || !inspectorPanel_->generatorGraphNodePickerReadyForSmoke())
    {
        return "Generator graph picker exposed a non-approved node type or omitted the closed source/processor set";
    }
    if (!inspectorPanel_->selectGeneratorForSmoke("smokePhrase")
        || !inspectorPanel_->generateGeneratorForSmoke()
        || !inspectorPanel_->generatorSurfaceReadyForSmoke(
            "smokePhrase",
            "rest note rest rest rest note note"))
    {
        return "Generator panel did not publish the seed-zero golden preview";
    }
    {
        const auto image = createComponentSnapshot(
            getLocalBounds(), true, 1.0F);
        if (!image.isValid())
            return "Generator snapshot did not produce a valid image";
        const auto path =
            (smokeDirectory / "workbench-generator.png").lexically_normal();
        auto stream = juce::File{juceString(path.string())}
                          .createOutputStream();
        if (stream == nullptr || !stream->openedOk()
            || !stream->setPosition(0)
            || stream->truncate().failed()
            || !juce::PNGImageFormat{}.writeImageToStream(image, *stream))
        {
            return "Generator snapshot output could not be written";
        }
        stream->flush();
    }
    if (!inspectorPanel_->selectGeneratorForSmoke("smokeLiteral")
        || !inspectorPanel_->generateGeneratorForSmoke()
        || !inspectorPanel_->generatorSurfaceReadyForSmoke(
            "smokeLiteral", "note rest"))
    {
        return "independent generator preview aliased shared symbol identities";
    }

    const auto targetAuthorityBytes = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    const auto rejectedTargetDeletion =
        projectBridge_.editor().removeSoundDefinition("generatorSmokeVoice");
    if (rejectedTargetDeletion.ok()
        || talas::project::serializeProjectDocument(
               projectBridge_.editor().document())
            != targetAuthorityBytes)
    {
        return "Generator target deletion did not reject without mutation";
    }

    if (!inspectorPanel_->selectGeneratorForSmoke("smokePhrase")
        || !inspectorPanel_->configureGeneratorProjectionForSmoke(
            250000, 960, "")
        || !inspectorPanel_->generatorProjectionActionsEnabledForSmoke(
            true, false)
        || !inspectorPanel_->configureGeneratorProjectionForSmoke(
            talas::project::maximumProjectGeneratorTimelineMicroseconds + 1U,
            960,
            "generatorSmokeOut")
        || !inspectorPanel_->generatorProjectionActionsEnabledForSmoke(
            false, true))
    {
        return "Generator actions did not share their exact command-specific request predicate";
    }

    const auto timelineBeforeCount =
        projectBridge_.editor().document().timeline.eventValues.size();
    const auto timelineBeforeBytes = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    if (!inspectorPanel_->selectGeneratorForSmoke("smokePhrase")
        || !inspectorPanel_->configureGeneratorProjectionForSmoke(
            250000, 960, "generatorSmokeOut")
        || !inspectorPanel_->generateGeneratorTimelineForSmoke())
    {
        return "Generator Timeline worker did not accept its bounded request";
    }
    auto timelineFinished = false;
    for (int attempt = 0; attempt < 2000 && !timelineFinished; ++attempt)
    {
        timelineFinished =
            inspectorPanel_->generatorProjectionFinishedForSmoke();
        if (!timelineFinished)
            juce::Thread::sleep(1);
    }
    if (!timelineFinished
        || projectBridge_.editor().document().timeline.eventValues.size()
            != timelineBeforeCount + 3)
    {
        return "Generator Timeline worker/mailbox did not publish all exact note terminals";
    }
    const auto& generatedTimeline =
        projectBridge_.editor().document().timeline.eventValues;
    if (generatedTimeline[generatedTimeline.size() - 3].timeSeconds != 0.75
        || generatedTimeline[generatedTimeline.size() - 2].timeSeconds != 2.75
        || generatedTimeline.back().timeSeconds != 3.25)
    {
        return "Generator Timeline base/grid controls did not preserve exact terminal order";
    }
    if (!projectBridge_.editor().undo().ok()
        || talas::project::serializeProjectDocument(
               projectBridge_.editor().document())
            != timelineBeforeBytes)
    {
        return "Generated Timeline batch did not remain one isolated Undo";
    }
    documentChanged();

    const auto scoreBefore = projectBridge_.editor().document().timeline.score;
    if (!inspectorPanel_->selectGeneratorForSmoke("smokePhrase")
        || !inspectorPanel_->configureGeneratorProjectionForSmoke(
            250000, 960, "generatorSmokeOut")
        || !inspectorPanel_->generateGeneratorScoreForSmoke())
    {
        return "Generator Score action did not publish its bounded candidate";
    }
    const auto& generatedScore =
        projectBridge_.editor().document().timeline.score;
    if (generatedScore.clips.size() != 3
        || generatedScore.markers.size() != 3
        || generatedScore.automation.size() != 1
        || generatedScore.automation.front().points.size() != 3
        || generatedScore.clips.front().clipId
            != "generatorSmokeOut_c1_1"
        || generatedScore.clips.front().start.tick != 1440
        || generatedScore.markers.front().markerId
            != "generatorSmokeOut_m1_3")
    {
        return "Generator Score action did not publish clip, automation, and marker output";
    }
    if (!projectBridge_.editor().undo().ok()
        || projectBridge_.editor().document().timeline.score != scoreBefore)
    {
        return "Generated Score batch did not remain one isolated Undo";
    }
    documentChanged();

    const auto stageBeforeSoundAuthoring =
        projectBridge_.latestStageGeneration();
    if (!inspectorPanel_->selectGeneratorForSmoke("smokeSoundChain")
        || !inspectorPanel_->configureGeneratorSoundForSmoke(
            "generatorSmokeGenerated",
            "Generator Smoke Generated",
            "generatorSmoke")
        || !inspectorPanel_->generatorProjectionActionsEnabledForSmoke(
            false,
            false,
            true)
        || !inspectorPanel_->generateGeneratorSoundForSmoke())
    {
        return "Generate Sound action did not accept its exact bounded request";
    }
    if (!projectBridge_.editor().renameSoundDefinition(
            "generatorSmokeVoice",
            "Temporarily renamed voice").ok())
    {
        return "Generate Sound stale-authority fixture edit failed";
    }
    auto staleSoundFinished = false;
    for (int attempt = 0;
         attempt < 2000 && !staleSoundFinished;
         ++attempt)
    {
        staleSoundFinished =
            inspectorPanel_->generatorProjectionFinishedForSmoke();
        if (!staleSoundFinished)
            juce::Thread::sleep(1);
    }
    const auto staleSound = std::ranges::find(
        projectBridge_.editor().document().soundDefinitions,
        "generatorSmokeGenerated",
        &talas::project::ProjectSoundDefinition::definitionId);
    if (!staleSoundFinished
        || staleSound
            != projectBridge_.editor().document().soundDefinitions.end()
        || projectBridge_.latestStageGeneration()
            != stageBeforeSoundAuthoring
        || !projectBridge_.editor().undo().ok())
    {
        return "stale Generate Sound result changed authored or active-stage authority";
    }
    documentChanged();

    if (!inspectorPanel_->selectGeneratorForSmoke("smokeSoundChain")
        || !inspectorPanel_->configureGeneratorSoundForSmoke(
            "generatorSmokeGenerated",
            "Generator Smoke Generated",
            "generatorSmoke")
        || !inspectorPanel_->generateGeneratorSoundForSmoke())
    {
        return "Generate Sound worker did not accept the restored exact authority";
    }
    auto soundFinished = false;
    for (int attempt = 0; attempt < 2000 && !soundFinished; ++attempt)
    {
        soundFinished =
            inspectorPanel_->generatorProjectionFinishedForSmoke();
        if (!soundFinished)
            juce::Thread::sleep(1);
    }
    const auto generatedSound = std::ranges::find(
        projectBridge_.editor().document().soundDefinitions,
        "generatorSmokeGenerated",
        &talas::project::ProjectSoundDefinition::definitionId);
    if (!soundFinished)
        return "Generate Sound worker did not publish terminal completion";
    if (generatedSound
        == projectBridge_.editor().document().soundDefinitions.end())
        return "Generate Sound completion did not author its exact definition ID";
    if (generatedSound->graph.nodes.size() != 2
        || generatedSound->graph.connections.size() != 1
        || generatedSound->graph.nodes[0].typeId != "talas.oscillator"
        || generatedSound->graph.nodes[1].typeId != "talas.filter")
        return "Generate Sound authored the wrong restricted chain";
    if (projectBridge_.editor().document().graph != graphBefore)
        return "Generate Sound changed the root graph without explicit instantiation";
    if (projectBridge_.latestStageGeneration()
        != stageBeforeSoundAuthoring)
        return "Generate Sound changed active stage authority during authoring";
    if (!inspectorPanel_->generatorSoundSurfaceReadyForSmoke(
            "generatorSmokeGenerated",
            "Generator Smoke Generated",
            "Sound candidate applied: 2 nodes / 1 connections"))
        return "Generate Sound UI summary or accessibility identity was not exact";
    const auto generatedSoundBytes = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    if (!projectBridge_.editor().undo().ok()
        || std::ranges::any_of(
            projectBridge_.editor().document().soundDefinitions,
            [](const auto& definition) {
                return definition.definitionId
                    == "generatorSmokeGenerated";
            })
        || !projectBridge_.editor().redo().ok()
        || talas::project::serializeProjectDocument(
               projectBridge_.editor().document())
            != generatedSoundBytes)
    {
        return "Generated Sound did not remain one exact Undo/redo candidate";
    }
    documentChanged();

    phrase.source.seed = 1;
    if (!projectBridge_.editor()
             .replaceProjectGenerator(
                 "smokePhrase",
                 projectBridge_.editor().documentAuthorityGeneration(),
                 phrase)
             .ok())
    {
        return "Generator seed edit failed";
    }
    documentChanged();
    if (!inspectorPanel_->selectGeneratorForSmoke("smokePhrase")
        || !inspectorPanel_->generateGeneratorForSmoke()
        || !inspectorPanel_->generatorSurfaceReadyForSmoke(
            "smokePhrase", "note"))
    {
        return "Generator seed edit did not publish its exact preview";
    }
    if (!projectBridge_.editor().undo().ok())
        return "Generator seed edit Undo failed";
    documentChanged();
    if (!inspectorPanel_->selectGeneratorForSmoke("smokePhrase")
        || !inspectorPanel_->generateGeneratorForSmoke()
        || !inspectorPanel_->generatorSurfaceReadyForSmoke(
            "smokePhrase",
            "rest note rest rest rest note note"))
    {
        return "Generator Undo did not restore exact source and output";
    }
    if (!projectBridge_.editor().redo().ok())
        return "Generator seed edit Redo failed";
    documentChanged();
    if (!inspectorPanel_->selectGeneratorForSmoke("smokePhrase")
        || !inspectorPanel_->generateGeneratorForSmoke()
        || !inspectorPanel_->generatorSurfaceReadyForSmoke(
            "smokePhrase", "note"))
    {
        return "Generator Redo did not restore exact source and output";
    }

    auto exhausted = phrase;
    exhausted.source.maximumRewritePasses = 0;
    if (!projectBridge_.editor()
             .replaceProjectGenerator(
                 "smokePhrase",
                 projectBridge_.editor().documentAuthorityGeneration(),
                 exhausted)
             .ok())
    {
        return "Generator exhaustion fixture edit failed";
    }
    documentChanged();
    if (!inspectorPanel_->selectGeneratorForSmoke("smokePhrase")
        || !inspectorPanel_->generateGeneratorForSmoke()
        || !inspectorPanel_->generatorSurfaceReadyForSmoke(
            "smokePhrase", "note"))
    {
        return "exhausted generator replaced the retained last-good preview";
    }
    if (!projectBridge_.editor().undo().ok())
        return "Generator exhaustion Undo failed";
    documentChanged();

    if (!inspectorPanel_->selectGeneratorForSmoke("smokeLiteral")
        || !inspectorPanel_->requestGeneratorDeleteForSmoke()
        || !inspectorPanel_->acceptGeneratorDeleteForSmoke())
    {
        return "Generator native delete confirmation failed";
    }
    if (!projectBridge_.editor().undo().ok())
        return "Generator delete Undo failed";
    documentChanged();

    const auto expectedPersistedBytes =
        talas::project::serializeProjectDocument(
            projectBridge_.editor().document());
    const auto persistedPath =
        (smokeDirectory / "workbench-generator-format32.talas.json")
            .lexically_normal();
    if (!saveProjectToPath(persistedPath))
        return "Format-32 Workbench save failed";
    newProjectNow();
    loadProjectFromPath(persistedPath);
    const auto& persisted = projectBridge_.editor().document();
    const auto persistedPhrase = std::ranges::find(
        persisted.generatorLibrary.programs,
        "smokePhrase",
        &talas::project::ProjectGeneratorProgram::generatorId);
    const auto persistedSoundChain = std::ranges::find(
        persisted.generatorLibrary.programs,
        "smokeSoundChain",
        &talas::project::ProjectGeneratorProgram::generatorId);
    const auto persistedGeneratedSound = std::ranges::find(
        persisted.soundDefinitions,
        "generatorSmokeGenerated",
        &talas::project::ProjectSoundDefinition::definitionId);
    if (persisted.formatVersion != talas::project::currentProjectFormatVersion
        || talas::project::serializeProjectDocument(persisted)
            != expectedPersistedBytes
        || persistedPhrase
            == persisted.generatorLibrary.programs.end()
        || persistedPhrase->output.bindings.size() != 1
        || persistedPhrase->output.bindings.front().actions.size() != 4
        || persistedSoundChain
            == persisted.generatorLibrary.programs.end()
        || persistedSoundChain->output.bindings.size() != 2
        || persistedGeneratedSound == persisted.soundDefinitions.end()
        || persistedGeneratedSound->graph.nodes.size() != 2
        || persistedGeneratedSound->graph.connections.size() != 1)
    {
        return "Format-32 Workbench save/reload did not preserve generator graph actions and generated Sound";
    }

    auto previewOnlyPhrase = phrase;
    previewOnlyPhrase.output = {};
    if (!projectBridge_.editor()
             .replaceProjectGenerator(
                 "smokePhrase",
                 projectBridge_.editor().documentAuthorityGeneration(),
                 std::move(previewOnlyPhrase))
             .ok())
    {
        return "Generator smoke output cleanup failed";
    }
    talas::project::ProjectScoreEditBatch targetCleanup;
    targetCleanup.removeAutomationLaneIds.push_back("generatorSmokeGain");
    targetCleanup.removeTrackIds = {
        "generatorSmokeTrack",
        "generatorSmokeMaster",
    };
    if (!projectBridge_.editor().applyScoreEdits(std::move(targetCleanup)).ok()
        || !projectBridge_.editor()
                .removeSoundDefinition("generatorSmokeVoice")
                .ok())
    {
        return "Generator smoke target cleanup failed";
    }
    documentChanged();

    if (!projectBridge_.editor().addSoundInstance(
            "generatorSoundInstance",
            "generatorSmokeGenerated",
            {120, 120}).ok()
        || !projectBridge_.editor().addNode(
            {"generatorSoundOutput", "talas.audioOutput", {}, {}, {}},
            {360, 120}).ok()
        || !projectBridge_.editor().connect({
            "generatorSoundInstance",
            "audioOut",
            "generatorSoundOutput",
            "audioIn"}).ok())
    {
        return "Reloaded generated Sound could not be instantiated through the ordinary reusable-Sound graph path";
    }
    documentChanged();
    talas::runtime::RenderSettings generatedSoundSettings;
    generatedSoundSettings.sampleRate = 8000.0;
    generatedSoundSettings.maxBlockFrames = 64;
    auto compiledGeneratedSound = talas::runtime::compileProject(
        projectBridge_.editor().document(),
        registry_,
        generatedSoundSettings);
    if (!compiledGeneratedSound.ok())
        return "Reloaded generated Sound did not compile through the ordinary runtime path";
    const auto offlineGeneratedSound = talas::runtime::renderOffline(
        *compiledGeneratedSound.graph,
        64);
    auto offlineGeneratedPeak = 0.0;
    for (const auto sample : offlineGeneratedSound.mono)
    {
        offlineGeneratedPeak = std::max(
            offlineGeneratedPeak, std::abs(sample));
    }
    if (!offlineGeneratedSound.ok()
        || offlineGeneratedSound.mono.size() != 64
        || offlineGeneratedPeak <= 1.0e-6)
    {
        return "Reloaded generated Sound offline render mismatch: samples="
            + std::to_string(offlineGeneratedSound.mono.size())
            + ", diagnostics="
            + std::to_string(offlineGeneratedSound.diagnostics.size())
            + ", peak=" + std::to_string(offlineGeneratedPeak);
    }
    if (!stageProject(
            generatedSoundSettings,
            "generated Sound workflow smoke"))
    {
        return "Reloaded generated Sound did not stage through the ordinary realtime path";
    }
    std::array<float, 64> realtimeGeneratedSound{};
    std::array<float*, 1> generatedSoundChannels{
        realtimeGeneratedSound.data()};
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            generatedSoundChannels.data(),
            generatedSoundChannels.size(),
            realtimeGeneratedSound.size()))
    {
        return "Reloaded generated Sound realtime render failed";
    }
    for (std::size_t frame = 0; frame < realtimeGeneratedSound.size(); ++frame)
    {
        if (std::abs(
                static_cast<double>(realtimeGeneratedSound[frame])
                - offlineGeneratedSound.mono[frame]) > 1.0e-5)
        {
            return "Generated Sound offline and realtime expansion did not agree";
        }
    }
    projectBridge_.setPlaying(false);
    static_cast<void>(projectBridge_.collectRetired());

    if (!projectBridge_.editor().disconnect(
            "generatorSoundOutput", "audioIn").ok()
        || !projectBridge_.editor().removeNode(
            "generatorSoundInstance").ok()
        || !projectBridge_.editor().removeNode(
            "generatorSoundOutput").ok()
        || !projectBridge_.editor().removeSoundDefinition(
            "generatorSmokeGenerated").ok()
        || !projectBridge_.editor().removeProjectGenerator(
            "smokeSoundChain").ok())
    {
        return "Generated Sound runtime proof did not cleanly restore authored graph authority";
    }
    documentChanged();

    const auto& document = projectBridge_.editor().document();
    if (document.generatorLibrary.programs.size() != 2
        || document.graph != graphBefore
        || document.timeline.score != talas::project::ProjectScore{})
    {
        return "Generator workflow changed graph authority or lost an exact program";
    }
    inspectorPanel_->showNode();
    return {};
}
}
