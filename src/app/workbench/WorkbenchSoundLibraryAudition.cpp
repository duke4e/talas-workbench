#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace talas::app::workbench
{
namespace
{
[[nodiscard]] bool sameAuditionEntry(
    const talas::app::SoundLibraryEntry& left,
    const talas::app::SoundLibraryEntry& right)
{
    return left.stableId == right.stableId
        && left.sourceKind == right.sourceKind
        && left.compatibility == right.compatibility
        && left.actions == right.actions
        && left.projectPath == right.projectPath
        && left.definitionId == right.definitionId
        && left.descriptorTypeId == right.descriptorTypeId
        && left.auditionInputs == right.auditionInputs
        && left.auditionAudioAssetIds == right.auditionAudioAssetIds
        && left.verifiedProjectContentHash
            == right.verifiedProjectContentHash;
}

[[nodiscard]] const talas::app::SoundLibraryManifestEntry* manifestEntryFor(
    const talas::app::SoundLibraryManifest& manifest,
    std::string_view stableId)
{
    if (!stableId.starts_with("example:"))
        return nullptr;
    const auto id = stableId.substr(std::string_view{"example:"}.size());
    const auto found = std::ranges::find(
        manifest.entries,
        id,
        &talas::app::SoundLibraryManifestEntry::id);
    return found == manifest.entries.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<std::string> controllerTerminalName(
    const talas::project::ProjectDocument& document)
{
    const auto terminal = std::ranges::find_if(
        document.graph.nodes,
        [](const auto& node) {
            return node.typeId == "talas.soundToGlobalController"
                || node.typeId == "talas.triggeredSoundToGlobalController";
        });
    if (terminal == document.graph.nodes.end()
        || std::ranges::count_if(
               document.graph.nodes,
               [](const auto& node) {
                   return talas::project::isTerminalNodeType(node.typeId);
               }) != 1)
        return std::nullopt;
    const auto field = std::ranges::find(
        terminal->parameters, std::string_view{"generatedEvent"},
        &talas::project::ProjectParameterExpression::parameterId);
    if (field == terminal->parameters.end()
        || !field->text.has_value()
        || field->text->size() < 2
        || field->text->front() != '!')
        return std::nullopt;
    return *field->text;
}
}

void WorkbenchComponent::startSoundLibraryAudition(
    std::string stableEntryId,
    bool routeControllerToProject)
{
    const auto samePendingRequest = soundLibraryAuditionPending_.has_value()
        && soundLibraryAuditionPending_->stableEntryId == stableEntryId
        && soundLibraryAuditionPending_->routeControllerToProject
            == routeControllerToProject;
    const auto samePresentedRequest = auditionDefinitionId_
            == std::optional<std::string>{stableEntryId}
        && auditionControlsCurrentProject_ == routeControllerToProject;
    if (samePendingRequest
        || (samePresentedRequest
            && (!soundLibraryAuditionWorkerFinished_.load(
                    std::memory_order_acquire)
                || soundLibraryAuditionActive_ != nullptr
                || soundLibraryAuditionRetiring_ != nullptr)))
    {
        stopSoundLibraryAudition();
        return;
    }
    if (soundLibraryAuditionSmokeUsePreparedProfile_)
    {
        startSoundLibraryAuditionWithProfile(
            std::move(stableEntryId),
            8000.0,
            64,
            deviceGeneration_.load(std::memory_order_acquire),
            routeControllerToProject);
        return;
    }
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr
        || device->getCurrentSampleRate() <= 0.0
        || device->getCurrentBufferSizeSamples() <= 0)
    {
        setStatus("Sound Library audition requires an active audio device");
        return;
    }
    startSoundLibraryAuditionWithProfile(
        std::move(stableEntryId),
        device->getCurrentSampleRate(),
        static_cast<std::size_t>(
            device->getCurrentBufferSizeSamples()),
        deviceGeneration_.load(std::memory_order_acquire),
        routeControllerToProject);
}

void WorkbenchComponent::startSoundLibraryAuditionWithProfile(
    std::string stableEntryId,
    double sampleRate,
    std::size_t maxBlockFrames,
    std::uint64_t sourceDeviceGeneration,
    bool routeControllerToProject)
{
    stopPartialTrackPreview();
    if (soundLibraryAuditionWorkerFinished_.load(std::memory_order_acquire)
        && soundLibraryAuditionThread_.joinable())
    {
        finishSoundLibraryAuditionIfReady();
    }
    retireSoundLibraryAuditionIfQuiescent();
    if (!soundLibraryAuditionWorkerFinished_.load(std::memory_order_acquire)
        || soundLibraryAuditionActive_ != nullptr
        || soundLibraryAuditionRetiring_ != nullptr
        || soundLibraryAuditionPending_.has_value())
    {
        const auto sameRequest = (auditionDefinitionId_
                    == std::optional<std::string>{stableEntryId}
                && auditionControlsCurrentProject_
                    == routeControllerToProject)
            || (soundLibraryAuditionPending_.has_value()
                && soundLibraryAuditionPending_->stableEntryId
                    == stableEntryId
                && soundLibraryAuditionPending_->routeControllerToProject
                    == routeControllerToProject);
        stopSoundLibraryAudition();
        if (!sameRequest)
        {
            soundLibraryAuditionPending_ = PendingSoundLibraryAudition{
                std::move(stableEntryId),
                sampleRate,
                maxBlockFrames,
                sourceDeviceGeneration,
                routeControllerToProject,
            };
            soundDefinitionsPanel_->setAuditionState(
                soundLibraryAuditionPending_->stableEntryId,
                routeControllerToProject);
            soundLibraryAuditionStatus_ =
                "Switching to the selected Sound Library audition...";
            setStatus(soundLibraryAuditionStatus_);
        }
        return;
    }
    if (usesExternalMidiClock() || usesExternalMtc())
    {
        setStatus(
            "Sound Library audition unavailable while external transport controls playback");
        return;
    }
    if (!std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || maxBlockFrames == 0)
    {
        setStatus("Sound Library audition requires a valid prepared device profile");
        return;
    }
    const auto* currentEntry = soundLibraryEntry(stableEntryId);
    if (currentEntry == nullptr
        || soundDefinitionsPanel_->selectedEntryId() != stableEntryId
        || currentEntry->compatibility
            != talas::app::SoundLibraryCompatibility::Ready
        || !talas::app::soundLibraryHasAction(
            currentEntry->actions,
            talas::app::SoundLibraryAction::Audition))
    {
        setStatus(
            currentEntry != nullptr
                    && !currentEntry->actionDiagnostic.empty()
                ? "Sound Library audition unavailable: "
                    + juceString(currentEntry->actionDiagnostic)
                : juce::String{
                    "The selected Sound Library entry is stale or not auditionable"});
        return;
    }
    if (currentEntry->sourceKind
        == talas::app::SoundLibrarySourceKind::Component)
    {
        setStatus(
            "Component audition requires an authorized private component host");
        return;
    }
    if (routeControllerToProject
        && (currentEntry->sourceKind
                != talas::app::SoundLibrarySourceKind::OriginalExample
            || std::ranges::find(currentEntry->tags, "controller-route")
                == currentEntry->tags.end()
            || !currentDocumentStaged_
            || projectBridge_.activeOutputChannelCount() == 0))
    {
        setStatus("Controlling the current project requires a staged Audio Sound and an original controller example");
        return;
    }

    const auto capturedEntry = *currentEntry;
    const auto editableDocument = projectBridge_.editor().document();
    const auto editableDocumentBytes =
        talas::project::serializeProjectDocument(editableDocument);
    const auto editableDocumentGeneration =
        uiDocumentGeneration_.load(std::memory_order_acquire);
    const auto requestGeneration =
        soundLibraryAuditionRequestGeneration_.fetch_add(
            1, std::memory_order_acq_rel)
        + 1;
    const auto substitutions = workspaceSession_.soundLibrary.inputSubstitutions;
    const auto sourceProjectPath = projectPath();
    const auto manifest = soundLibraryManifest_;
    const auto libraryRoot = soundLibraryRoot_;
    const auto smokeHoldWorker =
        soundLibraryAuditionSmokeHoldNextWorker_;
    soundLibraryAuditionSmokeHoldNextWorker_ = false;
    if (smokeHoldWorker)
    {
        soundLibraryAuditionSmokeWorkerPaused_.store(
            false, std::memory_order_release);
        soundLibraryAuditionSmokeHoldWorker_.store(
            true, std::memory_order_release);
    }

    stopAnalysisAudition();
    stopAnalysisOutputTakeForLifecycle();
    soundLibraryAuditionWorkerFinished_.store(false, std::memory_order_release);
    setAuditionPresentation(
        capturedEntry.stableId, routeControllerToProject);
    soundLibraryAuditionStatus_ = routeControllerToProject
        ? "Preparing controller route to current project..."
        : "Preparing exact Sound Library audition...";
    setStatus(soundLibraryAuditionStatus_);
    try
    {
        soundLibraryAuditionThread_ = std::jthread(
            [this,
             capturedEntry,
             editableDocument,
             editableDocumentBytes,
             editableDocumentGeneration,
             sourceDeviceGeneration,
             sampleRate,
             maxBlockFrames,
             requestGeneration,
             substitutions,
             sourceProjectPath,
             manifest,
             libraryRoot,
             smokeHoldWorker,
             routeControllerToProject](std::stop_token stopToken) mutable {
                SoundLibraryAuditionPublication publication;
                try
                {
                    talas::project::ProjectDocument source = editableDocument;
                    std::optional<std::filesystem::path> exactSourcePath =
                        sourceProjectPath;
                    if (capturedEntry.sourceKind
                        == talas::app::SoundLibrarySourceKind::OriginalExample)
                    {
                        const auto* manifestEntry = manifestEntryFor(
                            manifest, capturedEntry.stableId);
                        if (manifestEntry == nullptr
                            || capturedEntry.projectPath
                                != std::optional<std::string>{
                                    manifestEntry->projectPath}
                            || capturedEntry.verifiedProjectContentHash
                                != std::optional<std::string>{
                                    manifestEntry->verifiedProjectContentHash})
                        {
                            publication.diagnostic =
                                "Original example audition authority is stale";
                        }
                        else
                        {
                            auto snapshot =
                                talas::app::loadSoundLibraryExampleSnapshot(
                                    libraryRoot, *manifestEntry, registry_);
                            if (!snapshot.ok())
                            {
                                publication.diagnostic = snapshot.diagnostic;
                            }
                            else
                            {
                                source = std::move(snapshot.snapshot->document);
                                exactSourcePath =
                                    libraryRoot / manifestEntry->projectPath;
                            }
                        }
                    }
                    if (stopToken.stop_requested())
                        publication.cancelled = true;

                    talas::audio::ProjectAssetWatchSnapshot beforeAssets;
                    if (publication.diagnostic.empty()
                        && !publication.cancelled)
                    {
                        auto prepared = talas::app::prepareSoundLibraryAudition(
                            source,
                            capturedEntry,
                            registry_,
                            substitutions);
                        if (!prepared.ok())
                        {
                            publication.diagnostic = prepared.diagnostic;
                        }
                        else if (stopToken.stop_requested())
                        {
                            publication.cancelled = true;
                        }
                        else
                        {
                            beforeAssets =
                                talas::audio::captureProjectAssetWatchSnapshot(
                                    *prepared.document,
                                    registry_,
                                    exactSourcePath);
                            talas::runtime::RenderSettings settings;
                            settings.sampleRate = sampleRate;
                            settings.maxBlockFrames = maxBlockFrames;
                            auto loaded =
                                talas::audio::loadReferencedProjectResourceSnapshots(
                                    *prepared.document,
                                    registry_,
                                    exactSourcePath,
                                    {},
                                    [&stopToken] {
                                        return stopToken.stop_requested();
                                    });
                            if (!loaded.ok())
                            {
                                publication.cancelled = loaded.wasCancelled;
                                if (!loaded.wasCancelled)
                                {
                                    publication.assetFailure = loaded.failure;
                                    publication.diagnosticDocument =
                                        *prepared.document;
                                    publication.diagnostic =
                                        loaded.failure.has_value()
                                        ? "Sound audition asset load failed: "
                                            + loaded.failure->message
                                        : "Sound audition asset load failed";
                                }
                            }
                            else
                            {
                                settings.resourceSnapshots =
                                    std::move(loaded.snapshots);
                                auto runtime = std::make_unique<
                                    SoundLibraryAuditionRuntime>();
                                runtime->entry = capturedEntry;
                                runtime->inputSubstitutions = substitutions;
                                runtime->editableDocumentBytes =
                                    editableDocumentBytes;
                                runtime->editableDocumentGeneration =
                                    editableDocumentGeneration;
                                runtime->sourceDeviceGeneration =
                                    sourceDeviceGeneration;
                                runtime->requestGeneration = requestGeneration;
                                runtime->routeControllerToProject =
                                    routeControllerToProject;
                                runtime->sourceProjectPath = exactSourcePath;
                                runtime->verifiedAssetWatch =
                                    talas::audio::captureProjectAssetWatchSnapshot(
                                        *prepared.document,
                                        registry_,
                                        exactSourcePath);
                                if (talas::audio::findProjectAssetWatchChange(
                                        beforeAssets,
                                        runtime->verifiedAssetWatch)
                                        .has_value())
                                {
                                    publication.diagnostic =
                                        "Sound audition asset changed during preparation";
                                }
                                else
                                {
                                    runtime->preparation = std::move(prepared);
                                    runtime->bridge = std::make_unique<
                                        talas::runtime::ProjectRuntimeBridge>(
                                            *runtime->preparation.document,
                                            registry_);
                                    const auto staged =
                                        runtime->bridge->compileAndStageDocument(
                                            *runtime->preparation.document,
                                            std::move(settings));
                                    if (!staged.ok())
                                    {
                                        publication.diagnostic =
                                            staged.message.empty()
                                            ? "Private Sound audition graph did not stage"
                                            : staged.message;
                                    }
                                    else
                                    {
                                        runtime->bridge->setPlaying(true);
                                        publication.runtime = std::move(runtime);
                                    }
                                }
                            }
                        }
                    }
                }
                catch (const std::bad_alloc&)
                {
                    publication.runtime.reset();
                    publication.diagnostic =
                        "Sound Library audition allocation failed";
                }
                catch (const std::length_error&)
                {
                    publication.runtime.reset();
                    publication.diagnostic =
                        "Sound Library audition exceeded a bounded collection";
                }
                catch (const std::exception& error)
                {
                    publication.runtime.reset();
                    publication.diagnostic =
                        "Sound Library audition preparation failed: "
                        + std::string{error.what()};
                }
                if (smokeHoldWorker)
                {
                    soundLibraryAuditionSmokeWorkerPaused_.store(
                        true, std::memory_order_release);
                    while (soundLibraryAuditionSmokeHoldWorker_.load(
                               std::memory_order_acquire)
                           && !stopToken.stop_requested())
                    {
                        std::this_thread::yield();
                    }
                    soundLibraryAuditionSmokeWorkerPaused_.store(
                        false, std::memory_order_release);
                }
                if (stopToken.stop_requested())
                {
                    publication.runtime.reset();
                    publication.cancelled = true;
                }
                {
                    const std::scoped_lock lock{
                        soundLibraryAuditionResultMutex_};
                    soundLibraryAuditionPublication_ = std::move(publication);
                }
                soundLibraryAuditionWorkerFinished_.store(
                    true, std::memory_order_release);
            });
    }
    catch (const std::exception& error)
    {
        soundLibraryAuditionWorkerFinished_.store(
            true, std::memory_order_release);
        setAuditionPresentation(std::nullopt);
        soundLibraryAuditionStatus_ =
            "Sound Library audition worker failed to start: "
            + juceString(error.what());
        setStatus(soundLibraryAuditionStatus_);
    }
}

void WorkbenchComponent::stopSoundLibraryAudition()
{
    const auto hadPending = soundLibraryAuditionPending_.has_value();
    soundLibraryAuditionPending_.reset();
    soundLibraryAuditionSmokeHoldWorker_.store(
        false, std::memory_order_release);
    const auto hadAudition = soundLibraryAuditionThread_.joinable()
        || soundLibraryAuditionActive_ != nullptr
        || soundLibraryAuditionRetiring_ != nullptr
        || auditionActive_
        || hadPending;
    if (!hadAudition)
        return;
    soundLibraryAuditionRequestGeneration_.fetch_add(
        1, std::memory_order_acq_rel);
    if (soundLibraryAuditionThread_.joinable()
        && !soundLibraryAuditionWorkerFinished_.load(std::memory_order_acquire))
    {
        soundLibraryAuditionThread_.request_stop();
        soundLibraryAuditionStatus_ = "Cancelling Sound Library audition...";
        setStatus(soundLibraryAuditionStatus_);
    }
    soundLibraryAuditionLifecycle_.store(0, std::memory_order_seq_cst);
    soundLibraryAuditionCallback_.store(nullptr, std::memory_order_release);
    soundLibraryAuditionOutputChannels_.store(0, std::memory_order_release);
    std::optional<bool> restorePlaying;
    if (soundLibraryAuditionActive_ != nullptr)
    {
        restorePlaying =
            soundLibraryAuditionActive_->previousTransportPlaying;
        soundLibraryAuditionRetiring_ =
            std::move(soundLibraryAuditionActive_);
    }
    setAuditionPresentation(std::nullopt);
    if (restorePlaying.has_value())
    {
        transportPlaying_ = *restorePlaying;
        if (currentDocumentStaged_)
            projectBridge_.setPlaying(*restorePlaying);
    }
    retireSoundLibraryAuditionIfQuiescent();
    refreshPersistentStatus();
    commandManager_.commandStatusChanged();
}

void WorkbenchComponent::finishSoundLibraryAuditionIfReady()
{
    retireSoundLibraryAuditionIfQuiescent();
    if (soundLibraryAuditionActive_ != nullptr
        && soundLibraryAuditionActive_->renderFailed.load(
            std::memory_order_acquire))
    {
        stopSoundLibraryAudition();
        setStatus("Sound Library audition stopped after a private render failure");
    }
    processSoundLibraryControllerRoute();
    if (!soundLibraryAuditionWorkerFinished_.load(std::memory_order_acquire))
        return;
    if (soundLibraryAuditionThread_.joinable())
        soundLibraryAuditionThread_.join();
    std::optional<SoundLibraryAuditionPublication> publication;
    {
        const std::scoped_lock lock{soundLibraryAuditionResultMutex_};
        publication = std::move(soundLibraryAuditionPublication_);
        soundLibraryAuditionPublication_.reset();
    }
    if (!publication.has_value())
    {
        startPendingSoundLibraryAuditionIfReady();
        return;
    }
    if (publication->cancelled
        || publication->runtime == nullptr
        || publication->runtime->requestGeneration
            != soundLibraryAuditionRequestGeneration_.load(
                std::memory_order_acquire))
    {
        if (publication->assetFailure.has_value()
            && publication->diagnosticDocument.has_value())
        {
            const auto& failure = *publication->assetFailure;
            const auto message =
                talas::audio::describeProjectAssetLoadFailure(failure);
            const std::array diagnostics = {
                talas::runtime::Diagnostic{
                    talas::runtime::DiagnosticCode::InvalidRenderSettings,
                    failure.nodeId,
                    {},
                    failure.parameterId,
                    message,
                    failure.structuralSourcePath,
                },
            };
            publishCompileDiagnostics(
                diagnostics,
                &*publication->diagnosticDocument);
        }
        if (!publication->cancelled
            && !publication->diagnostic.empty())
        {
            setStatus("Sound Library audition unavailable: "
                + juceString(publication->diagnostic));
        }
        else
        {
            setStatus("Sound Library audition cancelled");
        }
        setAuditionPresentation(std::nullopt);
        startPendingSoundLibraryAuditionIfReady();
        return;
    }

    auto& runtime = *publication->runtime;
    const auto* currentEntry = soundLibraryEntry(runtime.entry.stableId);
    bool sourceStillExact = currentEntry != nullptr
        && soundDefinitionsPanel_->selectedEntryId()
            == runtime.entry.stableId
        && sameAuditionEntry(*currentEntry, runtime.entry)
        && runtime.editableDocumentBytes
            == talas::project::serializeProjectDocument(
                projectBridge_.editor().document())
        && runtime.editableDocumentGeneration
            == uiDocumentGeneration_.load(std::memory_order_acquire)
        && runtime.sourceDeviceGeneration
            == deviceGeneration_.load(std::memory_order_acquire)
        && workspaceSession_.soundLibrary.inputSubstitutions
            == runtime.inputSubstitutions;
    if (sourceStillExact
        && currentEntry->sourceKind
            == talas::app::SoundLibrarySourceKind::OriginalExample)
    {
        const auto refreshed = talas::app::loadSoundLibraryManifest(
            soundLibraryRoot_, registry_);
        const auto* refreshedEntry = refreshed.ok()
            ? manifestEntryFor(*refreshed.manifest, runtime.entry.stableId)
            : nullptr;
        sourceStillExact = refreshedEntry != nullptr
            && runtime.entry.projectPath
                == std::optional<std::string>{refreshedEntry->projectPath}
            && runtime.entry.definitionId == refreshedEntry->definitionId
            && runtime.entry.verifiedProjectContentHash
                == std::optional<std::string>{
                    refreshedEntry->verifiedProjectContentHash}
            && refreshedEntry->expectedOutcome
                == talas::app::SoundLibraryExpectedOutcome::Renderable
            && talas::app::soundLibraryHasAction(
                refreshedEntry->actions,
                talas::app::SoundLibraryAction::Audition);
    }
    if (sourceStillExact)
    {
        sourceStillExact = !talas::audio::findProjectAssetWatchChange(
            runtime.verifiedAssetWatch,
            talas::audio::captureProjectAssetWatchSnapshot(
                *runtime.preparation.document,
                registry_,
                runtime.sourceProjectPath))
            .has_value();
    }
    if (!sourceStillExact)
    {
        setAuditionPresentation(std::nullopt);
        setStatus(
            "Sound Library audition cancelled because entry, document, input, asset, or device authority changed");
        startPendingSoundLibraryAuditionIfReady();
        return;
    }

    if (runtime.routeControllerToProject)
    {
        const auto name = controllerTerminalName(
            *runtime.preparation.document);
        if (runtime.preparation.outputChannelCount != 0
            || !name.has_value()
            || !currentDocumentStaged_
            || projectBridge_.activeOutputChannelCount() == 0)
        {
            setAuditionPresentation(std::nullopt);
            setStatus("Controller route requires a terminal controller example and a prepared Audio project");
            startPendingSoundLibraryAuditionIfReady();
            return;
        }
        const auto eventName = std::string_view{*name}.substr(1);
        const auto& controls =
            projectBridge_.editor().document().virtualControls;
        const auto control = std::ranges::find(
            controls, eventName,
            &talas::project::VirtualControl::eventName);
        if (control == controls.end())
        {
            setAuditionPresentation(std::nullopt);
            setStatus("Controller route requires a matching VCS control in the current project");
            startPendingSoundLibraryAuditionIfReady();
            return;
        }
        talas::project::SnapshotTargetIdentity target;
        target.kind = talas::project::SnapshotTargetKind::EventValue;
        target.eventName = std::string{eventName};
        target.domain = control->domain;
        if (!projectBridge_.snapshotDirectTargetPrepared(target))
        {
            setAuditionPresentation(std::nullopt);
            setStatus("Controller route target is not direct in the prepared project");
            startPendingSoundLibraryAuditionIfReady();
            return;
        }
        runtime.routedControllerName = *name;
    }

    runtime.previousTransportPlaying =
        currentDocumentStaged_ && transportPlaying_;
    if (currentDocumentStaged_)
        projectBridge_.setPlaying(runtime.routeControllerToProject);
    soundLibraryAuditionActive_ = std::move(publication->runtime);
    soundLibraryAuditionOutputChannels_.store(
        soundLibraryAuditionActive_->preparation.outputChannelCount,
        std::memory_order_release);
    soundLibraryAuditionCallback_.store(
        soundLibraryAuditionActive_.get(), std::memory_order_release);
    soundLibraryAuditionLifecycle_.store(1, std::memory_order_seq_cst);
    setAuditionPresentation(
        soundLibraryAuditionActive_->entry.stableId,
        soundLibraryAuditionActive_->routeControllerToProject);
    transportPlaying_ = true;
    soundLibraryAuditionStatus_ =
        (soundLibraryAuditionActive_->routeControllerToProject
            ? juceString("Controller route to current project active · ")
            : juceString("Private audition active · "))
        + juceString(soundLibraryAuditionActive_->preparation.sourceLabel);
    setStatus(soundLibraryAuditionStatus_);
    refreshPersistentStatus();
    commandManager_.commandStatusChanged();
}

void WorkbenchComponent::processSoundLibraryControllerRoute()
{
    auto* runtime = soundLibraryAuditionActive_.get();
    if (runtime == nullptr || !runtime->routeControllerToProject)
        return;
    if (runtime->routedEventsOverflowed.load(std::memory_order_acquire))
    {
        stopSoundLibraryAudition();
        setStatus("Controller route stopped: generated events exceeded its bounded queue");
        return;
    }
    bool applied = false;
    for (std::size_t consumed = 0;
         consumed < SoundLibraryAuditionRuntime::routedEventCapacity;
         ++consumed)
    {
        const auto read = runtime->routedReadIndex.load(
            std::memory_order_relaxed);
        const auto write = runtime->routedWriteIndex.load(
            std::memory_order_acquire);
        if (read == write)
            break;
        const auto event = runtime->routedEvents[
            read % SoundLibraryAuditionRuntime::routedEventCapacity];
        runtime->routedReadIndex.store(
            read + 1, std::memory_order_release);
        if (event.targetStageGeneration
            != projectBridge_.latestStageGeneration())
            continue;
        const auto routed = projectBridge_.applyGlobalControllerEvent(
            runtime->routedControllerName,
            event.value,
            event.allowLiveOverride,
            event.targetStageGeneration);
        if (!routed.ok())
        {
            stopSoundLibraryAudition();
            setStatus("Controller route stopped: "
                + juceString(routed.message));
            return;
        }
        applied = true;
    }
    if (applied)
        virtualControlSurface_->refresh();
}

void WorkbenchComponent::startPendingSoundLibraryAuditionIfReady()
{
    retireSoundLibraryAuditionIfQuiescent();
    if (!soundLibraryAuditionPending_.has_value()
        || !soundLibraryAuditionWorkerFinished_.load(
            std::memory_order_acquire)
        || soundLibraryAuditionThread_.joinable()
        || soundLibraryAuditionActive_ != nullptr
        || soundLibraryAuditionRetiring_ != nullptr)
    {
        return;
    }
    auto pending = std::move(*soundLibraryAuditionPending_);
    soundLibraryAuditionPending_.reset();
    setAuditionPresentation(std::nullopt);
    startSoundLibraryAuditionWithProfile(
        std::move(pending.stableEntryId),
        pending.sampleRate,
        pending.maxBlockFrames,
        pending.sourceDeviceGeneration,
        pending.routeControllerToProject);
}

void WorkbenchComponent::retireSoundLibraryAuditionIfQuiescent()
{
    if (soundLibraryAuditionRetiring_ != nullptr
        && soundLibraryAuditionAudioOperations_.load(
               std::memory_order_seq_cst)
            == 0)
    {
        soundLibraryAuditionRetiring_.reset();
    }
}

std::optional<bool> WorkbenchComponent::renderSoundLibraryAudition(
    double callbackTimeSeconds,
    float* const* outputChannels,
    std::size_t outputChannelCount,
    std::size_t frameCount,
    talas::runtime::RealtimeAudioInputBlock projectInputs) noexcept
{
    const auto renderSilence = [&] {
        for (std::size_t channel = 0;
             channel < outputChannelCount;
             ++channel)
        {
            if (outputChannels[channel] != nullptr)
            {
                std::fill_n(
                    outputChannels[channel], frameCount, 0.0F);
            }
        }
        return std::optional<bool>{true};
    };
    const auto lifecycle = soundLibraryAuditionLifecycle_.load(
        std::memory_order_seq_cst);
    if (lifecycle == 2)
        return renderSilence();
    if (lifecycle != 1)
    {
        return std::nullopt;
    }
    soundLibraryAuditionAudioOperations_.fetch_add(
        1, std::memory_order_seq_cst);
    const auto admittedLifecycle = soundLibraryAuditionLifecycle_.load(
        std::memory_order_seq_cst);
    if (admittedLifecycle != 1)
    {
        soundLibraryAuditionAudioOperations_.fetch_sub(
            1, std::memory_order_seq_cst);
        return admittedLifecycle == 2
            ? renderSilence()
            : std::optional<bool>{};
    }
    auto* runtime = soundLibraryAuditionCallback_.load(
        std::memory_order_acquire);
    bool rendered = false;
    if (runtime != nullptr && runtime->bridge != nullptr)
    {
        if (runtime->preparation.outputChannelCount == 0)
        {
            talas::runtime::RealtimeTerminalOutputBlock terminalOutput{
                runtime->terminalMidiEvents};
            terminalOutput.globalControllerEvents =
                runtime->terminalGlobalControllerEvents;
            terminalOutput.globalControllerName =
                runtime->terminalGlobalControllerName;
            rendered = runtime->bridge->renderTerminalEvents(
                           callbackTimeSeconds,
                           frameCount,
                           terminalOutput)
                == talas::runtime::RealtimeTerminalRenderResult::Rendered;
            runtime->terminalMidiEventCount.store(
                rendered ? terminalOutput.midiEventCount : 0,
                std::memory_order_release);
            runtime->terminalGlobalControllerEventCount.store(
                rendered ? terminalOutput.globalControllerEventCount : 0,
                std::memory_order_release);
            runtime->terminalGlobalControllerNameLength.store(
                rendered ? terminalOutput.globalControllerNameLength : 0,
                std::memory_order_release);
            runtime->terminalStageId.store(
                rendered ? terminalOutput.stageId : 0,
                std::memory_order_release);
            if (rendered && runtime->routeControllerToProject
                && terminalOutput.globalControllerEventCount != 0)
            {
                if (terminalOutput.globalControllerNameLength
                        > terminalOutput.globalControllerName.size()
                    || terminalOutput.globalControllerEventCount
                        > terminalOutput.globalControllerEvents.size())
                    rendered = false;
            }
            if (rendered && runtime->routeControllerToProject
                && terminalOutput.globalControllerEventCount != 0)
            {
                const auto name = std::string_view{
                    terminalOutput.globalControllerName.data(),
                    terminalOutput.globalControllerNameLength};
                if (name != runtime->routedControllerName
                    || terminalOutput.stageId == 0
                    || (runtime->routedSourceStageId != 0
                        && runtime->routedSourceStageId
                            != terminalOutput.stageId))
                    rendered = false;
                else
                {
                    runtime->routedSourceStageId = terminalOutput.stageId;
                    for (std::size_t index = 0;
                         index < terminalOutput.globalControllerEventCount;
                         ++index)
                    {
                        const auto write = runtime->routedWriteIndex.load(
                            std::memory_order_relaxed);
                        const auto read = runtime->routedReadIndex.load(
                            std::memory_order_acquire);
                        if (write - read
                            >= SoundLibraryAuditionRuntime::routedEventCapacity)
                        {
                            runtime->routedEventsOverflowed.store(
                                true, std::memory_order_release);
                            rendered = false;
                            break;
                        }
                        const auto& event =
                            terminalOutput.globalControllerEvents[index];
                        runtime->routedEvents[
                            write
                                % SoundLibraryAuditionRuntime::routedEventCapacity]
                            = {event.value, event.allowLiveOverride,
                                projectBridge_.latestStageGeneration()};
                        runtime->routedWriteIndex.store(
                            write + 1, std::memory_order_release);
                    }
                }
            }
            if (rendered && runtime->routeControllerToProject)
                rendered = projectBridge_.renderOrdered(
                    projectInputs,
                    callbackTimeSeconds,
                    outputChannels,
                    outputChannelCount,
                    frameCount);
            else
            {
                for (std::size_t channel = 0;
                     channel < outputChannelCount;
                     ++channel)
                {
                    if (outputChannels[channel] != nullptr)
                        std::fill_n(
                            outputChannels[channel], frameCount, 0.0F);
                }
            }
        }
        else
        {
            std::array<const float*, 8> silentInputs{};
            rendered = runtime->bridge->renderOrdered(
                {silentInputs.data(), silentInputs.size()},
                callbackTimeSeconds,
                outputChannels,
                outputChannelCount,
                frameCount);
        }
        if (!rendered)
            runtime->renderFailed.store(true, std::memory_order_release);
    }
    soundLibraryAuditionAudioOperations_.fetch_sub(
        1, std::memory_order_seq_cst);
    return rendered;
}
}
