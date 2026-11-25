#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/SoundLibraryAuditionModel.h"
#include "talas/project/ProjectAssetPaths.h"

#include <ranges>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
void WorkbenchComponent::openSoundLibraryHelp(std::string stableId)
{
    const auto* entry = soundLibraryEntry(stableId);
    if (entry == nullptr
        || soundDefinitionsPanel_->selectedEntryId() != stableId
        || entry->sourceKind
            != talas::app::SoundLibrarySourceKind::OriginalExample
        || !entry->helpAnchor.has_value()
        || !talas::app::soundLibraryHasAction(
            entry->actions, talas::app::SoundLibraryAction::OpenHelp))
    {
        setStatus("Sound Library help selection is stale or unavailable");
        return;
    }
    const auto compatibility = [value = entry->compatibility] {
        switch (value)
        {
        case talas::app::SoundLibraryCompatibility::Ready:
            return std::string{"Renderable original example"};
        case talas::app::SoundLibraryCompatibility::ValidationOnly:
            return std::string{"Validation-only original example"};
        case talas::app::SoundLibraryCompatibility::PreservedUnavailable:
            return std::string{"Preserved and unavailable original example"};
        case talas::app::SoundLibraryCompatibility::MissingDependency:
            return std::string{"Missing dependency"};
        case talas::app::SoundLibraryCompatibility::MissingAsset:
            return std::string{"Missing asset"};
        case talas::app::SoundLibraryCompatibility::UnsupportedRuntime:
            return std::string{"Unsupported runtime"};
        case talas::app::SoundLibraryCompatibility::Malformed:
            return std::string{"Malformed entry"};
        }
        return std::string{"Unavailable"};
    }();
    const auto state = talas::app::workbenchSurfaceState({
        talas::app::WorkbenchSurfaceStateKind::Content,
        entry->displayName,
        entry->summary
            + " Local repository-original evidence only; tutorial completeness and "
              "hosted help are not claimed.",
        std::nullopt,
        true,
    });
    if (!state.has_value())
    {
        setStatus("Sound Library help evidence exceeded its closed bounds");
        return;
    }
    // A demo's help names its table in the demos' own README.
    const auto demo = entry->demoKind != talas::app::SoundLibraryDemoKind::None;
    const auto demoKind = juceString(
        talas::app::soundLibraryDemoKindLabel(entry->demoKind));
    WorkbenchContextHelpContent content{
        "sound-library.help:" + juceString(entry->stableId),
        juceString(entry->displayName),
        demo ? "Demo " + demoKind.toLowerCase() + " help"
             : juce::String{"Original example help"},
        juceString(compatibility) + juceString(" · ")
            + (demo ? juce::String{"examples/demos/README.md"}
                    : juce::String{})
            + juceString(*entry->helpAnchor),
        "Use Tab to inspect actions; press Return on Open Example",
        juceString(entry->summary)
            + (entry->demoKind == talas::app::SoundLibraryDemoKind::Effect
                    ? " " + juceString(entry->actionDiagnostic) + "."
                    : entry->demoKind
                            == talas::app::SoundLibraryDemoKind::Instrument
                        ? juce::String{" Audition plays its demo phrase."}
                        : juce::String{})
            + " Local repository-original evidence; no hosted or copied reference material.",
        "Use Open Example for the exact checked project bytes",
        state,
    };
    workspaceSession_.accessibility.contextHelpVisible = true;
    applyAccessibilityPreferences();
    resized();
    contextHelpPanel_->setContent(std::move(content));
    noteWorkspaceSessionChanged();
    commandManager_.commandStatusChanged();
    setStatus("Opened local Sound Library help for "
        + juceString(entry->displayName));
}

void WorkbenchComponent::refreshSoundLibraryProjection()
{
    soundLibraryEntries_ = talas::app::buildSoundLibraryEntries(
        projectBridge_.editor().document(),
        registry_,
        soundLibraryManifest_);
    std::vector<std::string> validIds;
    validIds.reserve(soundLibraryEntries_.size());
    for (const auto& entry : soundLibraryEntries_)
        validIds.push_back(entry.stableId);
    const auto retainedState = workspaceSession_.soundLibrary;
    talas::project::reconcileWorkspaceSoundLibraryState(
        workspaceSession_.soundLibrary,
        validIds);
    (void)talas::app::reconcileSoundLibraryAuditionSubstitutions(
        workspaceSession_.soundLibrary,
        soundLibraryEntries_);
    if (workspaceSession_.soundLibrary != retainedState)
        noteWorkspaceSessionChanged();
    soundDefinitionsPanel_->setLibraryState(workspaceSession_.soundLibrary);
    soundDefinitionsPanel_->setLibraryEntries(
        soundLibraryEntries_, soundLibraryDiagnostic_);
}

const talas::app::SoundLibraryEntry* WorkbenchComponent::soundLibraryEntry(
    std::string_view stableId) const
{
    const auto found = std::ranges::find(
        soundLibraryEntries_, stableId, &talas::app::SoundLibraryEntry::stableId);
    return found == soundLibraryEntries_.end() ? nullptr : &*found;
}

void WorkbenchComponent::useSoundLibraryEntry(std::string stableId)
{
    const auto* entry = soundLibraryEntry(stableId);
    if (entry == nullptr
        || !talas::app::soundLibraryHasAction(
            entry->actions, talas::app::SoundLibraryAction::UseInProject))
    {
        setStatus("The selected Sound Library entry is stale or not usable");
        return;
    }
    switch (entry->sourceKind)
    {
    case talas::app::SoundLibrarySourceKind::ProjectDefinition:
        addSoundInstance(entry->definitionId.value_or(""));
        return;
    case talas::app::SoundLibrarySourceKind::Component:
    {
        const auto* descriptor = entry->descriptorTypeId.has_value()
            ? registry_.findNodeType(*entry->descriptorTypeId)
            : nullptr;
        if (descriptor == nullptr
            || descriptor->provenance.origin == talas::core::NodeOrigin::BuiltIn)
        {
            setStatus("The selected component is no longer available");
            return;
        }
        addPrototype(*descriptor);
        return;
    }
    case talas::app::SoundLibrarySourceKind::OriginalExample:
        importSoundLibraryExample(std::move(stableId));
        return;
    }
}

void WorkbenchComponent::requestOpenSoundLibraryExample(std::string stableId)
{
    const auto* entry = soundLibraryEntry(stableId);
    if (entry == nullptr
        || entry->sourceKind
            != talas::app::SoundLibrarySourceKind::OriginalExample
        || !talas::app::soundLibraryHasAction(
            entry->actions, talas::app::SoundLibraryAction::OpenExample))
    {
        setStatus("The selected original example is stale or unavailable");
        return;
    }
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    requestAfterDiscardConfirmation(
        [safeThis, stableId = std::move(stableId)] {
            if (safeThis != nullptr)
                safeThis->openSoundLibraryExampleNow(stableId);
        });
}

void WorkbenchComponent::openSoundLibraryExampleNow(std::string stableId)
{
    const auto* entry = soundLibraryEntry(stableId);
    if (entry == nullptr || !entry->projectPath.has_value()
        || soundLibraryRoot_.empty()
        || soundDefinitionsPanel_->selectedEntryId() != stableId)
    {
        setStatus("The selected original example is no longer available");
        return;
    }
    const auto refreshed = talas::app::loadSoundLibraryManifest(
        soundLibraryRoot_, registry_);
    const auto manifestId = stableId.starts_with("example:")
        ? stableId.substr(8)
        : std::string{};
    const talas::app::SoundLibraryManifestEntry* manifestEntry = nullptr;
    if (refreshed.ok())
    {
        const auto found = std::ranges::find(
            refreshed.manifest->entries,
            manifestId,
            &talas::app::SoundLibraryManifestEntry::id);
        if (found != refreshed.manifest->entries.end())
            manifestEntry = &*found;
    }
    if (manifestEntry == nullptr
        || manifestEntry->projectPath != *entry->projectPath
        || !entry->verifiedProjectContentHash.has_value()
        || manifestEntry->verifiedProjectContentHash
            != *entry->verifiedProjectContentHash)
    {
        setStatus("Open Example failed: original manifest or project changed");
        return;
    }
    const auto path = soundLibraryRoot_ / manifestEntry->projectPath;
    auto snapshot = talas::app::loadSoundLibraryExampleSnapshot(
        soundLibraryRoot_, *manifestEntry, registry_);
    if (!snapshot.ok())
    {
        setStatus("Open Example failed: " + juceString(snapshot.diagnostic));
        return;
    }
    (void)talas::project::makeProjectAssetPathsAbsolute(
        snapshot.snapshot->document, path);

    stopRecoveryWriter();
    prepareForDocumentReplacement();
    const auto replaced = projectBridge_.editor().replaceDocument(
        std::move(snapshot.snapshot->document));
    if (!replaced.ok())
    {
        rebuildOscEgress();
        updateRecoveryAfterDocumentChange();
        setStatus("Open Example failed: " + juceString(replaced.message));
        return;
    }
    resetForDocumentReplacement();
    editBoundary_.markNew(projectBridge_.editor().document());
    const auto deviceWarning = applyProjectDeviceSettings();
    documentChanged();
    applyWorkspaceSessionProjection(false);
    const auto demoNote = [kind = manifestEntry->demoKind] {
        switch (kind)
        {
        case talas::app::SoundLibraryDemoKind::Effect:
            return juceString(" · it plays live audio input 1");
        case talas::app::SoundLibraryDemoKind::Instrument:
            return juceString(" · Play runs its demo phrase");
        case talas::app::SoundLibraryDemoKind::None:
            break;
        }
        return juce::String{};
    }();
    setStatus(juce::String{"Opened an untitled copy of "}
        + (manifestEntry->demoKind == talas::app::SoundLibraryDemoKind::None
                ? "original example "
                : "demo ")
        + juceString(manifestEntry->title) + demoNote
        + (deviceWarning.isEmpty()
               ? juce::String{}
               : juceString(" · ") + deviceWarning));
}

void WorkbenchComponent::importSoundLibraryExample(std::string stableId)
{
    const auto* entry = soundLibraryEntry(stableId);
    if (entry == nullptr || !entry->projectPath.has_value()
        || !entry->definitionId.has_value() || soundLibraryRoot_.empty())
    {
        setStatus("The selected example has no importable reusable Sound");
        return;
    }
    const auto refreshed = talas::app::loadSoundLibraryManifest(
        soundLibraryRoot_, registry_);
    const auto manifestId = stableId.starts_with("example:")
        ? stableId.substr(8)
        : std::string{};
    const talas::app::SoundLibraryManifestEntry* manifestEntry = nullptr;
    if (refreshed.ok())
    {
        const auto found = std::ranges::find(
            refreshed.manifest->entries,
            manifestId,
            &talas::app::SoundLibraryManifestEntry::id);
        if (found != refreshed.manifest->entries.end())
            manifestEntry = &*found;
    }
    if (manifestEntry == nullptr
        || manifestEntry->projectPath != *entry->projectPath
        || manifestEntry->definitionId != entry->definitionId
        || !talas::app::soundLibraryHasAction(
            manifestEntry->actions,
            talas::app::SoundLibraryAction::UseInProject)
        || !entry->verifiedProjectContentHash.has_value()
        || manifestEntry->verifiedProjectContentHash
            != *entry->verifiedProjectContentHash)
    {
        setStatus("Import failed: original manifest or project changed");
        return;
    }
    const auto sourceProjectPath =
        soundLibraryRoot_ / manifestEntry->projectPath;
    auto snapshot = talas::app::loadSoundLibraryExampleSnapshot(
        soundLibraryRoot_, *manifestEntry, registry_);
    if (!snapshot.ok())
    {
        setStatus("Import failed: " + juceString(snapshot.diagnostic));
        return;
    }
    talas::project::SoundDefinitionImportRequest request;
    request.rootDefinitionId = *entry->definitionId;
    request.sourceProjectFilePath = sourceProjectPath;
    request.destinationProjectFilePath = projectPath();
    const auto direct = talas::project::planSoundDefinitionImport(
        projectBridge_.editor().document(),
        snapshot.snapshot->document,
        request,
        registry_);
    if (direct.ok())
    {
        if (direct.plan->noOp)
        {
            setStatus("The exact reusable Sound is already present");
            return;
        }
        commitSoundLibraryImport(
            std::move(stableId),
            std::move(request),
            currentDocumentSnapshot());
        return;
    }

    const auto collision =
        talas::project::suggestSoundDefinitionImportCollisionPlan(
            projectBridge_.editor().document(),
            snapshot.snapshot->document,
            *entry->definitionId,
            registry_,
            sourceProjectPath,
            projectPath());
    if (!collision.ok())
    {
        setStatus("Import failed: " + juceString(collision.diagnostic));
        return;
    }
    juce::String preview{"The following stable identities will be remapped:\n"};
    for (const auto& remap : collision.request->definitionRemaps)
        preview += "\nSound " + juceString(remap.sourceId)
            + juceString(" → ")
            + juceString(remap.destinationId);
    for (const auto& remap : collision.request->assetRemaps)
        preview += "\nAsset " + juceString(remap.sourceId)
            + juceString(" → ")
            + juceString(remap.destinationId);
    soundDialog_ = std::make_unique<juce::AlertWindow>(
        "Import Reusable Sound",
        preview,
        juce::MessageBoxIconType::WarningIcon,
        this);
    soundDialog_->addButton(
        "Import", 1, juce::KeyPress(juce::KeyPress::returnKey));
    soundDialog_->addButton(
        "Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    const auto expected = currentDocumentSnapshot();
    auto acceptedRequest = *collision.request;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    soundDialog_->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safeThis,
             stableId = std::move(stableId),
             request = std::move(acceptedRequest),
             expected](int result) mutable {
                if (safeThis == nullptr)
                    return;
                safeThis->soundDialog_.reset();
                safeThis->scheduleWorkbenchFocusRestoration();
                if (result == 1)
                {
                    safeThis->commitSoundLibraryImport(
                        std::move(stableId),
                        std::move(request),
                        expected);
                }
            }),
        false);
}

void WorkbenchComponent::commitSoundLibraryImport(
    std::string stableId,
    talas::project::SoundDefinitionImportRequest request,
    std::string expectedDestinationBytes)
{
    if (currentDocumentSnapshot() != expectedDestinationBytes)
    {
        setStatus("Import cancelled because the destination project changed");
        return;
    }
    if (soundDefinitionsPanel_->selectedEntryId() != stableId)
    {
        setStatus("Import cancelled because the selected library entry changed");
        return;
    }
    const auto* entry = soundLibraryEntry(stableId);
    if (entry == nullptr || !entry->projectPath.has_value()
        || entry->definitionId
            != std::optional<std::string>{request.rootDefinitionId})
    {
        setStatus("Import cancelled because the library entry changed");
        return;
    }
    const auto refreshed = talas::app::loadSoundLibraryManifest(
        soundLibraryRoot_, registry_);
    const auto manifestId = stableId.starts_with("example:")
        ? stableId.substr(8)
        : std::string{};
    const talas::app::SoundLibraryManifestEntry* manifestEntry = nullptr;
    if (refreshed.ok())
    {
        const auto found = std::ranges::find(
            refreshed.manifest->entries,
            manifestId,
            &talas::app::SoundLibraryManifestEntry::id);
        if (found != refreshed.manifest->entries.end())
            manifestEntry = &*found;
    }
    if (manifestEntry == nullptr
        || manifestEntry->projectPath != *entry->projectPath
        || manifestEntry->definitionId
            != std::optional<std::string>{request.rootDefinitionId}
        || !talas::app::soundLibraryHasAction(
            manifestEntry->actions,
            talas::app::SoundLibraryAction::UseInProject)
        || !entry->verifiedProjectContentHash.has_value()
        || manifestEntry->verifiedProjectContentHash
            != *entry->verifiedProjectContentHash)
    {
        setStatus("Import cancelled because the original example changed");
        return;
    }
    const auto sourceProjectPath =
        soundLibraryRoot_ / manifestEntry->projectPath;
    if (request.sourceProjectFilePath
            != std::optional<std::filesystem::path>{sourceProjectPath}
        || request.destinationProjectFilePath != projectPath())
    {
        setStatus("Import cancelled because the project path authority changed");
        return;
    }
    auto snapshot = talas::app::loadSoundLibraryExampleSnapshot(
        soundLibraryRoot_, *manifestEntry, registry_);
    if (!snapshot.ok())
    {
        setStatus("Import failed: " + juceString(snapshot.diagnostic));
        return;
    }
    auto planned = talas::project::planSoundDefinitionImport(
        projectBridge_.editor().document(),
        snapshot.snapshot->document,
        request,
        registry_);
    if (!planned.ok())
    {
        setStatus("Import failed: " + juceString(planned.diagnostic));
        return;
    }
    const auto importedRoot = planned.plan->importedRootDefinitionId;
    virtualControlSurface_->finishMidiTransactionNow();
    projectBridge_.editor().endHistoryTransaction();
    const auto applied = projectBridge_.editor().applyDocumentEdit(
        std::move(planned.plan->document));
    if (!applied.ok())
    {
        setStatus("Import failed: " + juceString(applied.message));
        return;
    }
    documentChanged();
    workspaceSession_.soundLibrary.selectedEntryId =
        "project:" + importedRoot;
    std::erase(
        workspaceSession_.soundLibrary.recentEntryIds,
        *workspaceSession_.soundLibrary.selectedEntryId);
    workspaceSession_.soundLibrary.recentEntryIds.insert(
        workspaceSession_.soundLibrary.recentEntryIds.begin(),
        *workspaceSession_.soundLibrary.selectedEntryId);
    if (workspaceSession_.soundLibrary.recentEntryIds.size()
        > talas::project::maximumWorkspaceSoundLibraryRecents)
    {
        workspaceSession_.soundLibrary.recentEntryIds.pop_back();
    }
    soundDefinitionsPanel_->setLibraryState(workspaceSession_.soundLibrary);
    noteWorkspaceSessionChanged();
    setStatus("Imported reusable Sound " + juceString(importedRoot)
        + " as one undoable project edit");
}

std::optional<std::string>
WorkbenchComponent::runE8SoundLibraryWorkflowSmoke(
    std::string_view editableDefinitionId)
{
    const auto require = [](bool condition, std::string message)
        -> std::optional<std::string> {
        return condition
            ? std::optional<std::string>{}
            : std::optional<std::string>{std::move(message)};
    };
    if (auto error = require(
            soundDefinitionsPanel_->hasAccessibleWorkflowControls(),
            "Sound Library workflow controls were not fully accessible"))
        return error;
    if (auto error = require(
            soundDefinitionsPanel_->libraryProjectionIsTruthfulForSmoke(),
            "Sound Library product action projection was not truthful"))
        return error;
    if (!soundLibraryRoot_.empty())
    {
        if (auto error =
                soundDefinitionsPanel_->libraryCollectionsSmokeError())
            return error;
    }
    if (auto error = require(
            soundDefinitionsPanel_
                ->auditionInputSubstitutionsWorkForSmoke(),
            "Sound Library audition input substitutions were not exact, bounded, and local-only"))
    {
        return error;
    }
    if (auto error = require(
            soundDefinitionsPanel_->libraryLayoutModesWorkForSmoke(),
            "Sound Library compact and expanded layouts were incomplete"))
        return error;

    showRootGraph();
    const auto* activeBeforeUse = graphForContext(activeGraphContext_);
    const auto activeNodeCountBeforeUse = activeBeforeUse == nullptr
        ? std::size_t{}
        : activeBeforeUse->nodes.size();
    handleSoundAction(
        SoundDefinitionsPanel::Action::UseInProject,
        "project:" + std::string{editableDefinitionId});
    const auto* activeAfterUse = graphForContext(activeGraphContext_);
    if (auto error = require(
            activeAfterUse != nullptr
                && activeAfterUse->nodes.size() == activeNodeCountBeforeUse + 1,
            "project Sound Library Use did not execute the exact reusable-instance controller path"))
        return error;
    const auto undoneUse = projectBridge_.editor().undo();
    if (!undoneUse.ok())
        return "project Sound Library Use did not remain one undoable edit: "
            + undoneUse.message;
    documentChanged();

    if (soundLibraryRoot_.empty())
    {
        const auto hasUnexpectedExample = std::ranges::any_of(
            soundLibraryEntries_, [](const auto& entry) {
                return entry.sourceKind
                    == talas::app::SoundLibrarySourceKind::OriginalExample;
            });
        return require(
            !hasUnexpectedExample && !soundLibraryDiagnostic_.empty(),
            "tests-off Sound Library did not fail closed without a bundled resource root");
    }

    const auto importableExample = std::ranges::find_if(
        soundLibraryEntries_, [](const auto& entry) {
            return entry.sourceKind
                    == talas::app::SoundLibrarySourceKind::OriginalExample
                && entry.definitionId.has_value()
                && talas::app::soundLibraryHasAction(
                    entry.actions,
                    talas::app::SoundLibraryAction::UseInProject);
        });
    if (importableExample == soundLibraryEntries_.end())
        return "Sound Library smoke found no exact importable original example";
    auto importLibraryState = soundDefinitionsPanel_->libraryState();
    importLibraryState.selectedEntryId = importableExample->stableId;
    soundDefinitionsPanel_->setLibraryState(importLibraryState);
    if (auto error = require(
            soundDefinitionsPanel_->selectedEntryId()
                == importableExample->stableId,
            "Sound Library smoke could not select the exact import authority"))
        return error;
    const auto contextHelpWasVisible =
        workspaceSession_.accessibility.contextHelpVisible;
    handleSoundAction(
        SoundDefinitionsPanel::Action::OpenHelp,
        importableExample->stableId);
    const auto localHelpExact =
            workspaceSession_.accessibility.contextHelpVisible
                && contextHelpPanel_->contentContainsForSmoke(
                    "sound-library.help:" + juceString(importableExample->stableId),
                    juceString(*importableExample->helpAnchor))
                && contextHelpPanel_->contentContainsForSmoke(
                    "sound-library.help:" + juceString(importableExample->stableId),
                    "no hosted or copied reference material");
    workspaceSession_.accessibility.contextHelpVisible =
        contextHelpWasVisible;
    applyAccessibilityPreferences();
    resized();
    if (auto error = require(
            localHelpExact,
            "Open Help did not project the exact checked local example authority"))
    {
        return error;
    }

    const auto beforeStaleImport = currentDocumentSnapshot();
    auto staleSelection = importLibraryState;
    staleSelection.selectedEntryId =
        "project:" + std::string{editableDefinitionId};
    soundDefinitionsPanel_->setLibraryState(staleSelection);
    talas::project::SoundDefinitionImportRequest staleRequest;
    staleRequest.rootDefinitionId = *importableExample->definitionId;
    commitSoundLibraryImport(
        importableExample->stableId,
        std::move(staleRequest),
        beforeStaleImport);
    if (auto error = require(
            currentDocumentSnapshot() == beforeStaleImport
                && statusLabel_.getText().contains(
                    "selected library entry changed"),
            "terminal import publication accepted a stale Sound Library selection"))
        return error;

    soundDefinitionsPanel_->setLibraryState(importLibraryState);
    talas::project::SoundDefinitionImportRequest stalePathRequest;
    stalePathRequest.rootDefinitionId = *importableExample->definitionId;
    stalePathRequest.sourceProjectFilePath =
        soundLibraryRoot_ / *importableExample->projectPath;
    stalePathRequest.destinationProjectFilePath =
        std::filesystem::path{"/stale/sound-library-destination.talas.json"};
    commitSoundLibraryImport(
        importableExample->stableId,
        std::move(stalePathRequest),
        beforeStaleImport);
    if (auto error = require(
            currentDocumentSnapshot() == beforeStaleImport
                && statusLabel_.getText().contains(
                    "project path authority changed"),
            "terminal import publication accepted a stale destination path"))
        return error;

    const auto titleBeforeImportTransaction =
        projectBridge_.editor().document().metadata.title;
    projectBridge_.editor().beginHistoryTransaction();
    auto transactionDocument = projectBridge_.editor().document();
    transactionDocument.metadata.title += " · before Sound Library import";
    const auto transactionEdit = projectBridge_.editor().applyDocumentEdit(
        std::move(transactionDocument));
    if (!transactionEdit.ok())
        return "Could not prepare the isolated import Undo regression: "
            + transactionEdit.message;
    const auto definitionsBeforeImport =
        projectBridge_.editor().document().soundDefinitions.size();
    handleSoundAction(
        SoundDefinitionsPanel::Action::UseInProject,
        importableExample->stableId);
    if (auto error = require(
            projectBridge_.editor().document().soundDefinitions.size()
                > definitionsBeforeImport,
            "original-example Use did not import its exact reusable-definition closure"))
        return error;
    const auto undoneImport = projectBridge_.editor().undo();
    if (!undoneImport.ok()
        || projectBridge_.editor().document().soundDefinitions.size()
            != definitionsBeforeImport
        || projectBridge_.editor().document().metadata.title
            == titleBeforeImportTransaction)
    {
        return "original-example import did not remain one undoable edit: "
            + undoneImport.message;
    }
    const auto undonePriorTransaction = projectBridge_.editor().undo();
    if (!undonePriorTransaction.ok()
        || projectBridge_.editor().document().metadata.title
            != titleBeforeImportTransaction)
    {
        return "Sound Library import did not isolate the preceding editor transaction: "
            + undonePriorTransaction.message;
    }
    documentChanged();
    return {};
}
}
