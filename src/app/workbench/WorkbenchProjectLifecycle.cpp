#include "app/workbench/WorkbenchComponentInternal.h"
#include "app/workbench/WorkbenchAccessibilityHost.h"

#include "talas/app/DiskButton.h"
#include "talas/project/SosSpectrumFile.h"

namespace talas::app::workbench
{
void WorkbenchComponent::resetForDocumentReplacement()
{
    stopSoundLibraryAudition();
    stopAnalysisAudition();
    stopPartialTrackPreview();
    const auto clockWasPublished =
        midiClockAuthorityGeneration_.exchange(
            0, std::memory_order_acq_rel);
    if (clockWasPublished != 0
        || projectBridge_.midiClockTelemetry().externalAuthority)
    {
        projectBridge_.configureMidiClockAuthority(0);
    }
    const auto mtcWasPublished = mtcAuthorityGeneration_.exchange(
        0, std::memory_order_acq_rel);
    if (mtcWasPublished != 0
        || projectBridge_.mtcTelemetry().externalAuthority)
    {
        projectBridge_.configureMtcAuthority(0);
    }
    latestMidiClockTelemetry_ = projectBridge_.midiClockTelemetry();
    latestMtcTelemetry_ = projectBridge_.mtcTelemetry();
    midiClockDiagnostic_.clear();
    const auto previouslySuppressed =
        suppressWorkspaceSessionPersistence_;
    suppressWorkspaceSessionPersistence_ = true;
    stopSampleMapImportWorker();
    stopSampleAssetDispositionWorker();
    retryPendingSampleCleanup();
    stopSampleWaveformWorker();
    ++sampleMapAuditionGeneration_;
    stopSampleMapAuditionWorker();
    sampleMidiSelectionGeneration_.fetch_add(
        1,
        std::memory_order_acq_rel);
    sampleMidiRoutingEnabled_.store(false, std::memory_order_release);
    sampleImportPresentation_ = SampleMapImportPresentation::Idle;
    sampleImportStatus_.clear();
    sampleImportProgress_.store(0.0, std::memory_order_relaxed);
    sampleAssetDispositionAssetId_.clear();
    sampleAssetDispositionStatus_.clear();
    selectedWaveformOverview_.reset();
    sampleWaveformError_.clear();
    sampleWaveformAssetId_.clear();
    sampleWaveformAssetGeneration_ = 0;
    sampleWaveformCursorFrame_ = 0;
    selectedSampleMidi_.reset();
    sampleMapAuditionActive_ = false;
    sampleMapAuditionPendingVoice_.reset();
    sampleMapAuditionActiveVoice_.reset();
    sampleMapAuditionRestoreSettings_.reset();
    sampleMapAuditionRestoreDeviceName_.clear();
    sampleMapAuditionPresentation_ =
        SampleMapAuditionPresentation::Idle;
    setAuditionPresentation(std::nullopt);
    projectBridge_.setPlaying(false);
    transportPlaying_ = false;
    projectBridge_.stopTransport();
    analysisPanel_->setPlaying(false);
    analysisPanel_->clearCapture();
    projectBridge_.resetRealtimeMetrics();
    requestDeviceMetricsReset();
    latestRealtimeMetrics_ = {};
    projectBridge_.resetResourceIdentityHistory();
    currentDocumentStaged_ = false;
    assetWatchBaseline_ = {};
    pendingAssetWatchStatus_.clear();
    pendingAssetReplacementId_.reset();
    projectFileRevision_.reset();
    projectChangedOnDisk_ = false;
    pendingProjectFileStatus_.clear();
    workspaceSession_ = {};
    workspaceVariableRestoreStageGeneration_ = 0;
    workspaceVariableRestorePending_ = false;
    workspaceSoundVariableRestoreStageGeneration_ = 0;
    workspaceSoundVariableRestorePending_ = false;
    primaryGraphContext_.reset();
    secondaryGraphContext_.reset();
    activeGraphContext_.reset();
    hierarchyPath_.clear();
    graphCanvas_->setSoundDefinitionContext(std::nullopt);
    secondaryGraphCanvas_->setSoundDefinitionContext(std::nullopt);
    soundDefinitionsPanel_->setNavigationPath({});
    selectNode({});
    suppressWorkspaceSessionPersistence_ =
        previouslySuppressed;
}

void WorkbenchComponent::prepareForDocumentReplacement()
{
    for (const auto source : {
             talas::runtime::RealtimeLiveSetSourceKind::Workbench,
             talas::runtime::RealtimeLiveSetSourceKind::MidiProgram,
             talas::runtime::RealtimeLiveSetSourceKind::Osc,
             talas::runtime::RealtimeLiveSetSourceKind::Tool})
    {
        projectBridge_.revokeLiveSetSourceAuthority(source);
    }
    for (const auto source : {
             talas::runtime::RealtimeLiveToolSourceKind::Workbench,
             talas::runtime::RealtimeLiveToolSourceKind::MidiProgram,
             talas::runtime::RealtimeLiveToolSourceKind::Osc})
    {
        projectBridge_.revokeLiveToolSourceAuthority(source);
    }
    if (scoreClipArtifactToken_.has_value())
    {
        (void) fileOperationCoordinator_.requestCancel(
            *scoreClipArtifactToken_);
    }
    if (diskCacheCaptureToken_.has_value())
    {
        (void) fileOperationCoordinator_.requestCancel(
            *diskCacheCaptureToken_);
    }
    stopSoundLibraryAudition();
    stopAnalysisOutputTakeForLifecycle();
    stopAnalysisAudition();
    stopPartialTrackPreview();
    suspendOscLoopbackProfile();
    stopComponentReplacement();
    (void) persistCurrentWorkspaceSession();
    resetAssetRecovery();
    partialTrackEditorPanel_->setLoading();
    virtualControlSurface_->finishMidiTransactionNow();
    talas::runtime::RealtimeControlCommand discarded;
    const auto drainBudget = midiUiIngress_.capacity();
    for (std::size_t drained = 0;
         drained < drainBudget && midiUiIngress_.tryPop(discarded);
         ++drained)
    {
    }
    uiDocumentGeneration_.fetch_add(1, std::memory_order_acq_rel);
    workspaceSession_.performance.activeLiveSetId.reset();
    workspaceSession_.performance.activeLiveSetCellId.reset();
    workspaceSession_.performance.queuedLiveSetId.reset();
    workspaceSession_.performance.queuedLiveSetCellId.reset();
    workspaceSession_.performance.queuedLiveSetRequestGeneration = 0;
}

void WorkbenchComponent::scheduleStartupRecoveryCheck()
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis] {
        if (safeThis != nullptr)
            safeThis->offerStartupRecovery();
    });
}

void WorkbenchComponent::offerStartupRecovery()
{
    auto loaded = talas::project::loadProjectRecoveryFile(recoveryPath_);
    if (loaded.missing())
    {
        enableRecoveryMonitoring();
        return;
    }
    if (!loaded.ok())
    {
        offerInvalidRecoveryChoice(
            loaded.error.has_value()
                ? loaded.error->message
                : "Unknown recovery read failure");
        return;
    }

    auto record = std::move(*loaded.record);
    if (record.versions.empty())
    {
        offerInvalidRecoveryChoice(
            "Recovery history contains no versions");
        return;
    }
    const auto cleanSnapshot =
        talas::project::serializeProjectDocument(record.cleanDocument);
    const auto recoveredSnapshot =
        talas::project::serializeProjectDocument(
            record.versions.back().recoveredDocument);
    if (cleanSnapshot == recoveredSnapshot)
    {
        if (clearProjectRecovery())
            enableRecoveryMonitoring();
        return;
    }

    std::optional<std::filesystem::path> safeProjectPath;
    std::optional<
        talas::project::ProjectFileRevision>
        safeProjectRevision;
    bool sourceAlreadyContainsRecovery = false;
    if (record.projectPath.has_value() && record.projectPath->is_absolute())
    {
        const auto resolvedSource =
            talas::project::resolveProjectPackagePath(
                *record.projectPath);
        const auto source = resolvedSource.ok()
            ? talas::project::loadProjectFile(
                resolvedSource.projectFilePath)
            : talas::project::ProjectFileLoadResult{};
        const auto packageValidation =
            source.ok()
            ? talas::project::validateProjectPackageAssets(
                *source.document,
                resolvedSource)
            : talas::project::
                ProjectPackageValidationResult{};
        if (source.ok()
            && packageValidation.ok())
        {
            const auto sourceSnapshot =
                talas::project::serializeProjectDocument(*source.document);
            sourceAlreadyContainsRecovery =
                sourceSnapshot == recoveredSnapshot;
            if (source.revision.has_value()
                && talas::project::
                    projectRecoverySourceMatchesBaseline(
                        record,
                        *source.document,
                        *source.revision))
            {
                safeProjectPath =
                    resolvedSource.projectFilePath;
                safeProjectRevision =
                    source.revision;
            }
        }
    }
    if (sourceAlreadyContainsRecovery)
    {
        if (clearProjectRecovery())
        {
            enableRecoveryMonitoring();
            setStatus("The last saved project is already up to date");
        }
        return;
    }
    if (record.projectPath.has_value() && !safeProjectPath.has_value())
    {
        static_cast<void>(
            talas::project::
                makeProjectRecoveryAssetPathsAbsolute(
                    record,
                    *record.projectPath));
    }

    destructivePromptActive_ = true;
    auto message =
        juce::String("Talas found unsaved project changes from an "
                     "interrupted session.");
    if (record.projectPath.has_value() && !safeProjectPath.has_value())
    {
        message +=
            "\n\nThe original project is missing or changed. Recovery "
            "will open as Untitled to avoid overwriting newer work.";
    }
    juce::StringArray versionLabels;
    for (auto iterator = record.versions.rbegin();
         iterator != record.versions.rend();
         ++iterator)
    {
        versionLabels.add(
            recoveryVersionLabel(
                *iterator,
                iterator == record.versions.rbegin()));
    }
    recoveryDialog_ =
        std::make_unique<juce::AlertWindow>(
            "Recover Unsaved Project",
            message,
            juce::MessageBoxIconType::WarningIcon,
            this);
    recoveryDialog_->addComboBox(
        "recoveryVersion",
        versionLabels,
        "Recovery version");
    if (auto* selector =
            recoveryDialog_->getComboBoxComponent(
                "recoveryVersion"))
    {
        selector->setSelectedId(
            1,
            juce::dontSendNotification);
    }
    recoveryDialog_->addButton(
        "Recover",
        1,
        juce::KeyPress(juce::KeyPress::returnKey));
    recoveryDialog_->addButton("Discard All", 2);
    recoveryDialog_->addButton(
        "Keep for Later",
        0,
        juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    recoveryDialog_->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safeThis,
             record = std::move(record),
             safeProjectPath = std::move(safeProjectPath),
             safeProjectRevision =
                 std::move(safeProjectRevision)](
                int result) mutable {
            if (safeThis == nullptr
                || safeThis->recoveryDialog_ == nullptr)
                return;
            auto newestFirstPosition = std::size_t{0};
            if (const auto* selector =
                    safeThis->recoveryDialog_
                        ->getComboBoxComponent(
                            "recoveryVersion");
                selector != nullptr
                && selector->getSelectedId() > 0)
            {
                newestFirstPosition =
                    static_cast<std::size_t>(
                        selector->getSelectedId() - 1);
            }
            safeThis->recoveryDialog_.reset();
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            if (result == 1)
            {
                const auto versionIndex =
                    talas::project::
                        projectRecoveryVersionIndexNewestFirst(
                            record,
                            newestFirstPosition);
                if (!versionIndex.has_value())
                {
                    safeThis->setStatus(
                        "Recovery failed: invalid version selection");
                    return;
                }
                safeThis->restoreRecovery(
                    std::move(record),
                    *versionIndex,
                    std::move(safeProjectPath),
                    std::move(
                        safeProjectRevision));
                return;
            }
            if (result == 2)
            {
                if (safeThis->clearProjectRecovery())
                {
                    safeThis->enableRecoveryMonitoring();
                    safeThis->setStatus(
                        "Discarded all recovered project versions");
                }
                return;
            }
            safeThis->preserveRecoveryFile_ = true;
            safeThis->setStatus(
                "Recovery history preserved; autosave is disabled");
        }),
        false);
}

juce::String WorkbenchComponent::recoveryVersionLabel(
    const talas::project::ProjectRecoveryVersion& version,
    bool newest)
{
    auto label = newest
        ? juceString("Newest · ")
        : juce::String{};
    if (version.createdAtUnixMilliseconds == 0)
    {
        label += "Legacy recovery";
    }
    else if (
        talas::project::
            projectRecoveryTimestampIsCalendarDisplaySafe(
                version.createdAtUnixMilliseconds))
    {
        const juce::Time timestamp{
            static_cast<juce::int64>(
                version.createdAtUnixMilliseconds)};
        label += timestamp.toString(
            true,
            true,
            true,
            true);
    }
    else
    {
        label +=
            juceString(std::to_string(
                version.createdAtUnixMilliseconds))
            + " ms since Unix epoch";
    }
    auto title = juceString(
        version.recoveredDocument.metadata.title);
    if (title.trim().isEmpty())
        title = "Untitled";
    return label + juceString(" · ") + title;
}

void WorkbenchComponent::offerInvalidRecoveryChoice(const std::string& error)
{
    destructivePromptActive_ = true;
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("Recovery File Could Not Be Read")
        .withMessage(
            "Talas did not apply the recovery file.\n\n"
            + juceString(error))
        .withButton("Keep File")
        .withButton("Discard File")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(
        options,
        [safeThis](int result) {
            if (safeThis == nullptr)
                return;
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            if (result == 1)
            {
                if (safeThis->clearProjectRecovery())
                {
                    safeThis->enableRecoveryMonitoring();
                    safeThis->setStatus("Discarded unreadable recovery file");
                }
                return;
            }
            safeThis->preserveRecoveryFile_ = true;
            safeThis->setStatus(
                "Unreadable recovery file preserved; autosave is disabled");
        });
}

void WorkbenchComponent::restoreRecovery(
    talas::project::ProjectRecoveryRecord record,
    std::size_t versionIndex,
    std::optional<std::filesystem::path> safeProjectPath,
    std::optional<
        talas::project::ProjectFileRevision>
        safeProjectRevision)
{
    if (versionIndex >= record.versions.size())
    {
        setStatus("Recovery failed: version is unavailable");
        return;
    }
    if (const auto check = projectBridge_.editor().checkReplacementDocument(
            record.versions[versionIndex].recoveredDocument);
        !check.ok())
    {
        // The unrecovered changes stay in the preserved recovery file, so
        // autosave must not overwrite them.
        preserveRecoveryFile_ = true;
        setStatus("Recovery failed: " + juceString(check.message)
            + " · recovery file preserved; autosave is disabled");
        return;
    }
    stopRecoveryWriter();
    prepareForDocumentReplacement();
    const auto hadSafeProjectPath =
        safeProjectPath.has_value();
    auto recoveredDocument =
        record.versions[versionIndex].recoveredDocument;
    if (safeProjectPath.has_value())
    {
        const auto resolvedSource =
            talas::project::resolveProjectPackagePath(
                *safeProjectPath);
        const auto packageValidation =
            resolvedSource.ok()
            ? talas::project::
                validateProjectPackageAssets(
                    recoveredDocument,
                    resolvedSource)
            : talas::project::
                ProjectPackageValidationResult{
                    {},
                    resolvedSource.error};
        if (!packageValidation.ok())
        {
            safeProjectPath.reset();
            safeProjectRevision.reset();
        }
    }
    if (safeProjectPath.has_value())
    {
        const auto currentRevision =
            talas::project::
                captureProjectFileRevision(
                    *safeProjectPath);
        if (!safeProjectRevision.has_value()
            || !currentRevision.ok()
            || currentRevision.revision
                != safeProjectRevision)
        {
            safeProjectPath.reset();
            safeProjectRevision.reset();
        }
    }
    if (hadSafeProjectPath
        && !safeProjectPath.has_value()
        && record.projectPath.has_value())
    {
        static_cast<void>(
            talas::project::
                makeProjectRecoveryAssetPathsAbsolute(
                    record,
                    *record.projectPath));
        recoveredDocument =
            record.versions[versionIndex]
                .recoveredDocument;
    }
    const auto replaced = projectBridge_.editor().replaceDocument(
        std::move(recoveredDocument));
    if (!replaced.ok())
    {
        rebuildOscEgress();
        setStatus("Recovery failed: " + juceString(replaced.message));
        return;
    }

    resetForDocumentReplacement();
    recoveryVersions_ = record.versions;
    editBoundary_.markRecovered(
        record.cleanDocument,
        safeProjectPath);
    projectFileRevision_ =
        std::move(safeProjectRevision);
    projectChangedOnDisk_ = false;
    nextProjectFilePollMilliseconds_ =
        juce::Time::
            getMillisecondCounterHiRes()
        + 1000.0;
    const auto sessionRestore = projectPath().has_value()
        ? restoreWorkspaceSessionForPath(*projectPath())
        : WorkspaceSessionRestore{};
    const auto deviceWarning = applyProjectDeviceSettings();
    documentChanged();
    applyWorkspaceSessionProjection(
        sessionRestore.restored);
    enableRecoveryMonitoring();
    juce::String status = projectPath().has_value()
        ? "Recovered unsaved project changes"
        : "Recovered unsaved project changes as Untitled";
    status += juceString(" · version ")
        + juce::String(
            static_cast<int>(versionIndex + 1))
        + "/"
        + juce::String(
            static_cast<int>(record.versions.size()));
    if (deviceWarning.isNotEmpty())
        status += juceString(" · ") + deviceWarning;
    if (sessionRestore.warning.isNotEmpty())
        status += juceString(" · ") + sessionRestore.warning;
    setStatus(status);
}

void WorkbenchComponent::enableRecoveryMonitoring()
{
    preserveRecoveryFile_ = false;
    recoveryMonitoringEnabled_ = true;
    recoveryAutosaveState_.documentChanged(
        editBoundary_.isDirty(projectBridge_.editor().document()),
        juce::Time::getMillisecondCounterHiRes());
}

void WorkbenchComponent::updateRecoveryAfterDocumentChange()
{
    if (!persistentStateEnabled_ || !recoveryMonitoringEnabled_)
        return;
    const auto dirty =
        editBoundary_.isDirty(projectBridge_.editor().document());
    recoveryAutosaveState_.documentChanged(
        dirty,
        juce::Time::getMillisecondCounterHiRes());
    if (!dirty)
        clearProjectRecovery();
}

void WorkbenchComponent::startRecoveryWriteIfDue()
{
    if (!recoveryMonitoringEnabled_)
    {
        return;
    }
    const auto dirty =
        editBoundary_.isDirty(projectBridge_.editor().document());
    if (!recoveryAutosaveState_.shouldStartWrite(
            dirty,
            recoveryThread_.joinable(),
            juce::Time::getMillisecondCounterHiRes()))
        return;

    talas::project::ProjectRecoveryRecord record;
    record.projectPath = projectPath();
    record.projectRevision =
        projectFileRevision_;
    record.cleanDocument =
        editBoundary_.cleanDocument();
    record.versions = recoveryVersions_;
    const auto wallClockMilliseconds =
        std::max<juce::int64>(
            0,
            juce::Time::currentTimeMillis());
    if (!talas::project::appendProjectRecoveryVersion(
            record,
            projectBridge_.editor().document(),
            static_cast<std::uint64_t>(
                wallClockMilliseconds)))
    {
        recoveryAutosaveState_.writeStarted();
        recoveryAutosaveState_.writeFinished(
            true,
            dirty,
            juce::Time::getMillisecondCounterHiRes());
        return;
    }
    recoveryAutosaveState_.writeStarted();
    recoveryWriteFinished_.store(false, std::memory_order_release);
    recoveryStopRequested_.store(
        false,
        std::memory_order_release);
    try
    {
        recoveryThread_ = std::thread(
            [this,
             path = recoveryPath_,
             record = std::move(record)]() mutable {
                auto result = talas::project::saveProjectRecoveryFile(
                    path,
                    record,
                    [this] {
                        return recoveryStopRequested_.load(
                            std::memory_order_acquire);
                    });
                {
                    const std::lock_guard lock(recoveryResultMutex_);
                    recoveryWriteResult_ = std::move(result);
                    if (recoveryWriteResult_.ok())
                    {
                        recoveryWriteVersions_ =
                            std::move(record.versions);
                    }
                }
                recoveryWriteFinished_.store(
                    true,
                    std::memory_order_release);
            });
    }
    catch (const std::system_error& error)
    {
        recoveryWriteFinished_.store(true, std::memory_order_release);
        recoveryAutosaveState_.writeFinished(
            false,
            true,
            juce::Time::getMillisecondCounterHiRes());
        refreshWindowTitle();
        setStatus(
            "Autosave failed to start: "
            + juceString(error.what()));
    }
}

void WorkbenchComponent::finishRecoveryWriteIfReady()
{
    if (!recoveryThread_.joinable()
        || !recoveryWriteFinished_.load(std::memory_order_acquire))
    {
        return;
    }
    recoveryThread_.join();
    consumeRecoveryWriteResult();
}

void WorkbenchComponent::consumeRecoveryWriteResult()
{
    talas::project::ProjectRecoverySaveResult result;
    std::vector<talas::project::ProjectRecoveryVersion>
        publishedVersions;
    {
        const std::lock_guard lock(recoveryResultMutex_);
        result = recoveryWriteResult_;
        publishedVersions =
            std::move(recoveryWriteVersions_);
    }
    const auto hadWriteFailure =
        recoveryAutosaveState_.hasWriteFailure();
    recoveryAutosaveState_.writeFinished(
        result.ok(),
        editBoundary_.isDirty(projectBridge_.editor().document()),
        juce::Time::getMillisecondCounterHiRes());
    const auto publicationConsumed =
        talas::project::consumeSuccessfulProjectRecoveryWrite(
            result,
            publishedVersions,
            recoveryVersions_);
    juce::ignoreUnused(publicationConsumed);
    refreshWindowTitle();
    if (!result.ok() && !result.cancelled())
    {
        setStatus(
            "Autosave failed: "
            + juceString(result.error->message));
    }
    else if (result.ok() && hadWriteFailure)
    {
        setStatus("Autosave recovered after a temporary failure");
    }
}

void WorkbenchComponent::stopRecoveryWriter()
{
    if (!recoveryThread_.joinable())
        return;
    recoveryStopRequested_.store(
        true,
        std::memory_order_release);
    recoveryThread_.join();
    recoveryWriteFinished_.store(true, std::memory_order_release);
    consumeRecoveryWriteResult();
}

bool WorkbenchComponent::clearProjectRecovery()
{
    stopRecoveryWriter();
    recoveryAutosaveState_.clear();
    if (!persistentStateEnabled_)
    {
        recoveryVersions_.clear();
        recoveryWriteVersions_.clear();
        return true;
    }
    if (preserveRecoveryFile_)
        return true;
    const auto discarded =
        talas::project::discardProjectRecoveryFile(recoveryPath_);
    if (!discarded.ok())
    {
        setStatus(
            "Recovery cleanup failed: "
            + juceString(discarded.error->message));
        return false;
    }
    recoveryVersions_.clear();
    recoveryWriteVersions_.clear();
    return true;
}

void WorkbenchComponent::requestNewProject()
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    requestAfterDiscardConfirmation([safeThis] {
        if (safeThis != nullptr)
            safeThis->newProjectNow();
    });
}

void WorkbenchComponent::requestOpenProject()
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    requestAfterDiscardConfirmation([safeThis] {
        if (safeThis != nullptr)
            safeThis->openProject();
    });
}

void WorkbenchComponent::requestOpenRecentProject(std::filesystem::path path)
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    requestAfterDiscardConfirmation(
        [safeThis, path = std::move(path)] {
            if (safeThis != nullptr)
                safeThis->loadProjectFromPath(path);
        });
}

void WorkbenchComponent::requestAfterDiscardConfirmation(std::function<void()> action)
{
    if (destructivePromptActive_ || fileChooserActive_)
    {
        setStatus("Finish the active dialog before changing projects");
        return;
    }
    stopComponentReplacement();
    const auto decisionSnapshot = currentDocumentSnapshot();
    if (!editBoundary_.isDirty(projectBridge_.editor().document()))
    {
        scheduleDestructiveContinuation(
            std::move(action),
            decisionSnapshot);
        return;
    }

    destructivePromptActive_ = true;
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("Unsaved Changes")
        .withMessage("Save changes before replacing the current project?")
        .withButton("Save")
        .withButton("Discard")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(
        options,
        [safeThis, action = std::move(action), decisionSnapshot](int result) mutable {
            if (safeThis == nullptr)
                return;
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            switch (talas::project::unsavedChangesChoiceForButtonIndex(result))
            {
            case talas::project::UnsavedChangesChoice::Save:
                safeThis->saveProject(
                    [safeThis, action = std::move(action)](bool saved) mutable {
                        if (saved && safeThis != nullptr)
                        {
                            safeThis->scheduleDestructiveContinuation(
                                std::move(action),
                                safeThis->currentDocumentSnapshot());
                        }
                    });
                break;
            case talas::project::UnsavedChangesChoice::Discard:
                safeThis->scheduleDestructiveContinuation(
                    std::move(action),
                    decisionSnapshot);
                break;
            case talas::project::UnsavedChangesChoice::Cancel:
                break;
            }
        });
}

std::string WorkbenchComponent::currentDocumentSnapshot() const
{
    return talas::project::serializeProjectDocument(projectBridge_.editor().document());
}

void WorkbenchComponent::scheduleDestructiveContinuation(
    std::function<void()> action,
    std::string expectedSnapshot)
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::MessageManager::callAsync(
        [safeThis,
         action = std::move(action),
         expectedSnapshot = std::move(expectedSnapshot)]() mutable {
            if (safeThis == nullptr)
                return;
            if (safeThis->currentDocumentSnapshot() != expectedSnapshot)
            {
                safeThis->requestAfterDiscardConfirmation(std::move(action));
                return;
            }
            action();
        });
}

void WorkbenchComponent::scheduleContinuation(std::function<void()> action)
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::MessageManager::callAsync(
        [safeThis, action = std::move(action)]() mutable {
            if (safeThis != nullptr)
                action();
        });
}

void WorkbenchComponent::newProjectNow()
{
    auto untitled = makeUntitledDocument();
    if (const auto check =
            projectBridge_.editor().checkReplacementDocument(untitled);
        !check.ok())
    {
        setStatus("New project failed: " + juceString(check.message));
        return;
    }
    stopRecoveryWriter();
    prepareForDocumentReplacement();
    const auto replaced = projectBridge_.editor().replaceDocument(std::move(untitled));
    if (!replaced.ok())
    {
        rebuildOscEgress();
        updateRecoveryAfterDocumentChange();
        setStatus("New project failed: " + juceString(replaced.message));
        return;
    }

    resetForDocumentReplacement();
    editBoundary_.markNew(projectBridge_.editor().document());
    const auto deviceWarning = applyProjectDeviceSettings();
    documentChanged();
    applyWorkspaceSessionProjection(false);
    setStatus(
        deviceWarning.isEmpty()
            ? juce::String{"Created new project"}
            : juceString("Created new project · ") + deviceWarning);
}

void WorkbenchComponent::openProject()
{
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }
    const auto& currentPath = projectPath();
    const auto initialLocation = currentPath.has_value()
        ? juce::File(jucePathString(*currentPath))
        : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Open Talas Project or Package",
        initialLocation,
        "*.talas.json;*.talastimeline;*.talaspkg");
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [safeThis](const juce::FileChooser& chooser) {
        if (safeThis == nullptr)
            return;
        safeThis->fileChooserActive_ = false;
        safeThis->scheduleWorkbenchFocusRestoration();
        const auto selected = chooser.getResult();
        if (selected == juce::File{})
            return;
        const auto path = filesystemPath(selected.getFullPathName());
        safeThis->scheduleContinuation([safeThis, path] {
            if (safeThis != nullptr)
                safeThis->loadProjectFromPath(path);
        });
    });
}

void WorkbenchComponent::browseResource(
    std::optional<std::string> definitionId,
    std::string nodeId,
    std::string parameterId)
{
    // Command or Control opens an editor on the file, Shift a window of
    // information on it.
    const auto modifiers = juce::ModifierKeys::getCurrentModifiers();
    switch (talas::app::diskButtonAction(modifiers.isCommandDown() || modifiers.isCtrlDown(), modifiers.isShiftDown()))
    {
    case talas::app::DiskButtonAction::OpenEditor:
        (void)openFieldFileEditor(definitionId, nodeId, parameterId);
        return;
    case talas::app::DiskButtonAction::ShowInformation:
        showFieldFileInformation(definitionId, nodeId, parameterId);
        return;
    case talas::app::DiskButtonAction::ChooseFile:
        break;
    }
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }
    const auto& currentPath = projectPath();
    juce::File initialLocation = currentPath.has_value()
        ? juce::File(jucePathString(currentPath->parent_path()))
        : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    const auto* node = nodeForContext(definitionId, nodeId);
    if (node != nullptr)
    {
        const auto* parameter = findParameter(*node, parameterId);
        if (parameter != nullptr && parameter->resource.has_value())
        {
            const auto asset = std::ranges::find_if(
                projectBridge_.editor().document().assets,
                [&](const talas::project::ProjectAsset& candidate) {
                    return candidate.id == *parameter->resource;
                });
            if (asset != projectBridge_.editor().document().assets.end())
                initialLocation = juce::File(
                    jucePathString(
                        talas::audio::resolveProjectAssetPath(
                            *asset,
                            currentPath)));
        }
    }
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Choose Resource File",
        initialLocation,
        "*");
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safeThis,
         definitionId = std::move(definitionId),
         nodeId = std::move(nodeId),
         parameterId = std::move(parameterId)](
            const juce::FileChooser& chooser) mutable {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            const auto selected = chooser.getResult();
            if (selected == juce::File{})
                return;
            if (safeThis->bindSpectrumFileAsTable(definitionId, nodeId, parameterId, filesystemPath(selected.getFullPathName())))
                return;
            safeThis->bindResourceFile(
                std::move(definitionId),
                std::move(nodeId),
                std::move(parameterId),
                filesystemPath(selected.getFullPathName()));
        });
}

void WorkbenchComponent::bindResourceFile(
                      std::optional<std::string> definitionId,
                      std::string nodeId,
                      std::string parameterId,
                      const std::filesystem::path& selectedPath)
{
    auto asset = makeAssetReference(selectedPath);
    std::optional<std::filesystem::path>
        importedPath;
    if (!importAssetIntoPackage(asset, selectedPath, importedPath))
        return;
    // Its folder is searched first the next time a name is typed in.
    talas::app::rememberLocatedFile(selectedPath, fileLocatorMemory_);
    // A spectrum file needs its media metadata read from it to be one the
    // project takes.
    if (asset.kind == talas::project::ProjectAssetKind::PartialSpectrum && asset.contentHash.empty())
    {
        const auto inspected = talas::project::inspectProjectAsset(
            asset, talas::audio::resolveProjectAssetPath(asset, projectPath()));
        if (inspected.ok())
            asset = inspected.asset;
    }
    const auto result = definitionId.has_value()
        ? projectBridge_.editor().setResourceParameterAssetInSoundDefinition(
              *definitionId,
              nodeId,
              parameterId,
              asset)
        : projectBridge_.editor().setResourceParameterAsset(
              nodeId,
              parameterId,
              asset);
    if (!result.ok())
    {
        if (importedPath.has_value())
        {
            std::error_code error;
            std::filesystem::remove(
                *importedPath,
                error);
        }
        setStatus("Resource binding failed: " + juceString(result.message));
        return;
    }

    documentChanged();
    setStatus(
        "Bound " + juceString(asset.id) + " to "
        + juceString(nodeId) + "."
        + juceString(parameterId)
        + (importedPath.has_value()
               ? juceString(" · imported into package")
               : juce::String{}));
}

// A file chosen for the project, copied into its package when the project
// is one; false, with the status said, when the copy fails.
bool WorkbenchComponent::importAssetIntoPackage(talas::project::ProjectAsset& asset,
    const std::filesystem::path& selectedPath, std::optional<std::filesystem::path>& importedPath)
{
    if (projectPath().has_value())
    {
        const auto resolved =
            talas::project::resolveProjectPackagePath(
                *projectPath());
        if (resolved.ok()
            && resolved.packageDirectory.has_value())
        {
            auto imported =
                talas::audio::importProjectPackageAsset(
                    *resolved.packageDirectory,
                    selectedPath,
                    asset.id);
            if (!imported.ok())
            {
                const auto importError = imported.error;
                if (!imported.retainedStagingPath.empty())
                {
                    pendingProjectPackageImportCleanup_.push_back(
                        std::move(imported));
                }
                const auto ledgerError = persistPendingCleanupLedger();
                setStatus(
                    "Resource import failed: "
                    + juceString(importError)
                    + (ledgerError.empty()
                           ? juce::String{}
                           : juceString(" · cleanup ledger: ")
                               + juceString(ledgerError)));
                return false;
            }
            asset.path = imported.relativePath;
            if (imported.created)
            {
                importedPath =
                    *resolved.packageDirectory
                    / talas::project::pathFromUtf8(
                        imported.relativePath);
            }
        }
    }
    return true;
}

talas::project::ProjectAsset WorkbenchComponent::makeAssetReference(const std::filesystem::path& selectedPath) const
{
    const auto selectedNormalized = normalizedAbsolutePath(selectedPath);
    const auto& assets = projectBridge_.editor().document().assets;
    const auto existing = std::ranges::find_if(assets, [&](const talas::project::ProjectAsset& asset) {
        return normalizedAbsolutePath(
            talas::audio::resolveProjectAssetPath(asset, projectPath())) == selectedNormalized;
    });
    if (existing != assets.end())
        return *existing;

    auto baseId = talas::project::pathToUtf8(selectedPath.filename());
    if (baseId.empty())
        baseId = "Asset";
    auto id = baseId;
    for (std::size_t ordinal = 2;
         std::ranges::any_of(assets, [&](const talas::project::ProjectAsset& asset) {
             return asset.id == id;
         });
         ++ordinal)
    {
        id = baseId + "#" + std::to_string(ordinal);
    }

    auto storedPath = selectedNormalized;
    if (projectPath().has_value())
    {
        const auto relative = selectedNormalized.lexically_relative(
            normalizedAbsolutePath(projectPath()->parent_path()));
        if (!relative.empty())
            storedPath = relative;
    }
    return {
        std::move(id),
        assetKindForPath(selectedPath),
        talas::project::pathToUtf8(storedPath),
    };
}

talas::project::ProjectAssetKind WorkbenchComponent::assetKindForPath(const std::filesystem::path& path)
{
    auto extension = talas::project::pathToUtf8(path.extension());
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (extension == ".mid" || extension == ".midi")
        return talas::project::ProjectAssetKind::Binary;
    // A spectrum file the Spectral Analysis Tool or the partial-track
    // editor wrote, or a .spc file.
    if (talas::project::isSpectrumFilePath(path))
        return talas::project::ProjectAssetKind::PartialSpectrum;
    return talas::project::ProjectAssetKind::Table;
}

std::filesystem::path WorkbenchComponent::normalizedAbsolutePath(const std::filesystem::path& path)
{
    std::error_code error;
    const auto absolute = path.is_absolute() ? path : std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

const std::optional<std::filesystem::path>& WorkbenchComponent::projectPath() const noexcept
{
    return editBoundary_.projectPath();
}

juce::String WorkbenchComponent::persistWorkspaceSessionForPath(
    const std::filesystem::path& path)
{
    if (!persistentStateEnabled_ || path.empty())
        return {};
    if (performancePanel_ != nullptr)
        performancePanel_->synchronizeLiveToolSessionState();
    talas::project::reconcileWorkspaceSessionState(
        workspaceSession_,
        projectBridge_.editor().document());
    const auto sessionPath =
        talas::project::workspaceSessionFilePathForProject(
            workspaceSessionsPath_,
            path);
    if (!sessionPath.has_value())
        return "Workspace session was not saved: invalid project identity";
    const auto saved = talas::project::saveWorkspaceSessionFile(
        *sessionPath,
        workspaceSession_);
    if (saved.ok()
        && projectPath().has_value()
        && normalizedAbsolutePath(path)
            == normalizedAbsolutePath(*projectPath()))
    {
        workspaceSessionWriteCoalescer_.clear();
    }
    return saved.ok()
        ? juce::String{}
        : "Workspace session was not saved: "
            + juceString(saved.error->message);
}

juce::String WorkbenchComponent::persistCurrentWorkspaceSession()
{
    return projectPath().has_value()
        ? persistWorkspaceSessionForPath(*projectPath())
        : juce::String{};
}

void WorkbenchComponent::noteWorkspaceSessionChanged()
{
    if (suppressWorkspaceSessionPersistence_
        || !persistentStateEnabled_
        || !projectPath().has_value())
        return;
    workspaceSessionWriteCoalescer_.noteChange(
        juce::Time::getMillisecondCounter());
    if (!isTimerRunning())
        startTimerHz(30);
}

bool WorkbenchComponent::applyWorkspaceSessionVariableValuesToCurrentStage()
{
    talas::project::reconcileWorkspaceSessionState(
        workspaceSession_,
        projectBridge_.editor().document());
    const auto stageGeneration = projectBridge_.latestStageGeneration();
    workspaceVariableRestoreStageGeneration_ = stageGeneration;
    if (stageGeneration == 0 || workspaceSession_.variableValues.empty())
    {
        workspaceVariableRestorePending_ = stageGeneration == 0
            && !workspaceSession_.variableValues.empty();
        return workspaceSession_.variableValues.empty();
    }

    std::vector<talas::runtime::ProjectVariableAssignment> assignments;
    try
    {
        assignments.reserve(workspaceSession_.variableValues.size());
        for (const auto& value : workspaceSession_.variableValues)
        {
            assignments.push_back({
                {
                    talas::project::ProjectVariableOwnerKind::WorkspaceSession,
                    value.variableId,
                    {},
                    {},
                },
                {value.value, value.domain},
            });
        }
    }
    catch (const std::exception&)
    {
        return false;
    }
    const auto accepted = projectBridge_.scheduleVariableAssignments(
               assignments,
               stageGeneration)
        == talas::runtime::SnapshotDirectTransactionResult::Accepted;
    workspaceVariableRestorePending_ = !accepted;
    return accepted;
}

void WorkbenchComponent::synchronizeWorkspaceSessionVariableValues()
{
    if (!currentDocumentStaged_)
        return;
    const auto publication = projectBridge_.preparedVariableValues();
    if (!publication.publicationValid || publication.stageId == 0
        || publication.stageId != projectBridge_.activeStageId()
        || publication.stageId != projectBridge_.latestPreparedStageId())
    {
        return;
    }

    if (workspaceVariableRestoreStageGeneration_
        == projectBridge_.latestStageGeneration())
    {
        const auto restored = std::ranges::all_of(
            workspaceSession_.variableValues,
            [&](const auto& expected) {
                const talas::project::PreparedProjectVariableIdentity identity{
                    talas::project::ProjectVariableOwnerKind::WorkspaceSession,
                    expected.variableId,
                    {},
                    {},
                };
                const auto found = std::ranges::find(
                    publication.values,
                    identity,
                    &talas::runtime::PreparedVariableValue::identity);
                return found != publication.values.end()
                    && found->value.domain == expected.domain
                    && found->value.value == expected.value;
            });
        if (!restored)
            return;
        workspaceVariableRestoreStageGeneration_ = 0;
    }

    std::vector<talas::project::WorkspaceVariableValue> nextValues;
    try
    {
        nextValues.reserve(talas::project::maximumWorkspaceVariableValues);
        for (const auto& declaration :
             projectBridge_.editor().document().variableLibrary.variables)
        {
            if (declaration.owner.kind
                    != talas::project::ProjectVariableOwnerKind::WorkspaceSession
                || nextValues.size()
                    >= talas::project::maximumWorkspaceVariableValues)
            {
                continue;
            }
            const talas::project::PreparedProjectVariableIdentity identity{
                talas::project::ProjectVariableOwnerKind::WorkspaceSession,
                declaration.variableId,
                {},
                {},
            };
            const auto found = std::ranges::find(
                publication.values,
                identity,
                &talas::runtime::PreparedVariableValue::identity);
            if (found == publication.values.end()
                || found->value.domain != declaration.domain
                || !std::isfinite(found->value.value))
            {
                continue;
            }
            const auto fingerprint =
                talas::project::workspaceSessionVariableDeclarationFingerprint(
                    declaration);
            if (fingerprint.empty())
                continue;
            nextValues.push_back({
                declaration.variableId,
                declaration.domain,
                found->value.value,
                fingerprint,
            });
        }
        std::ranges::sort(
            nextValues,
            {},
            &talas::project::WorkspaceVariableValue::variableId);
    }
    catch (const std::exception&)
    {
        return;
    }
    if (workspaceSession_.variableValues == nextValues)
        return;
    workspaceSession_.variableValues = std::move(nextValues);
    noteWorkspaceSessionChanged();
}

bool WorkbenchComponent::
applyWorkspaceSessionSoundVariableValuesToCurrentStage()
{
    talas::project::reconcileWorkspaceSessionState(
        workspaceSession_,
        projectBridge_.editor().document());
    const auto stageGeneration = projectBridge_.latestStageGeneration();
    workspaceSoundVariableRestoreStageGeneration_ = stageGeneration;
    if (stageGeneration == 0
        || workspaceSession_.soundVariableValues.empty())
    {
        workspaceSoundVariableRestorePending_ = stageGeneration == 0
            && !workspaceSession_.soundVariableValues.empty();
        return workspaceSession_.soundVariableValues.empty();
    }

    std::vector<talas::runtime::ProjectSoundVariableAssignment> assignments;
    try
    {
        assignments.reserve(workspaceSession_.soundVariableValues.size());
        for (const auto& value : workspaceSession_.soundVariableValues)
        {
            assignments.push_back({
                {
                    talas::project::ProjectVariableOwnerKind::WorkspaceSession,
                    value.variableId,
                    {},
                    {},
                },
                value.selectedDefinitionId,
            });
        }
    }
    catch (const std::exception&)
    {
        return false;
    }
    const auto accepted = projectBridge_.scheduleSoundVariableAssignments(
               assignments,
               stageGeneration)
        == talas::runtime::SnapshotDirectTransactionResult::Accepted;
    workspaceSoundVariableRestorePending_ = !accepted;
    return accepted;
}

void WorkbenchComponent::synchronizeWorkspaceSessionSoundVariableValues()
{
    if (!currentDocumentStaged_)
        return;
    const auto publication = projectBridge_.preparedSoundVariableValues();
    if (!publication.publicationValid || publication.stageId == 0
        || publication.stageId != projectBridge_.activeStageId()
        || publication.stageId != projectBridge_.latestPreparedStageId())
    {
        return;
    }

    if (workspaceSoundVariableRestoreStageGeneration_
        == projectBridge_.latestStageGeneration())
    {
        const auto restored = std::ranges::all_of(
            workspaceSession_.soundVariableValues,
            [&](const auto& expected) {
                const talas::project::PreparedProjectSoundVariableIdentity
                    identity{
                        talas::project::ProjectVariableOwnerKind::WorkspaceSession,
                        expected.variableId,
                        {},
                        {},
                    };
                const auto found = std::ranges::find(
                    publication.values,
                    identity,
                    &talas::runtime::PreparedSoundVariableValue::identity);
                return found != publication.values.end()
                    && found->selectedDefinitionId
                        == expected.selectedDefinitionId;
            });
        if (!restored)
            return;
        workspaceSoundVariableRestoreStageGeneration_ = 0;
    }

    std::vector<talas::project::WorkspaceSoundVariableValue> nextValues;
    try
    {
        nextValues.reserve(
            talas::project::maximumWorkspaceSoundVariableValues);
        for (const auto& declaration :
             projectBridge_.editor().document()
                 .soundVariableLibrary.variables)
        {
            if (declaration.owner.kind
                    != talas::project::ProjectVariableOwnerKind::WorkspaceSession
                || nextValues.size()
                    >= talas::project::maximumWorkspaceSoundVariableValues)
            {
                continue;
            }
            const talas::project::PreparedProjectSoundVariableIdentity identity{
                talas::project::ProjectVariableOwnerKind::WorkspaceSession,
                declaration.variableId,
                {},
                {},
            };
            const auto found = std::ranges::find(
                publication.values,
                identity,
                &talas::runtime::PreparedSoundVariableValue::identity);
            if (found == publication.values.end()
                || std::ranges::find(
                       declaration.candidateDefinitionIds,
                       found->selectedDefinitionId)
                    == declaration.candidateDefinitionIds.end())
            {
                continue;
            }
            const auto fingerprint = talas::project::
                workspaceSessionSoundVariableDeclarationFingerprint(
                    declaration);
            if (fingerprint.empty())
                continue;
            nextValues.push_back({
                declaration.variableId,
                found->selectedDefinitionId,
                fingerprint,
            });
        }
        std::ranges::sort(
            nextValues,
            {},
            &talas::project::WorkspaceSoundVariableValue::variableId);
    }
    catch (const std::exception&)
    {
        return;
    }
    if (workspaceSession_.soundVariableValues == nextValues)
        return;
    workspaceSession_.soundVariableValues = std::move(nextValues);
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::updateWorkspaceGraphView(
    std::optional<std::string> definitionId,
    double panX,
    double panY,
    double zoom)
{
    auto& context =
        talas::project::ensureWorkspaceGraphSessionState(
            workspaceSession_,
            std::move(definitionId));
    context.panX = panX;
    context.panY = panY;
    context.zoom = zoom;
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::updateWorkspacePaneSplit(juce::Point<int> position)
{
    if (graphSplitArea_.isEmpty())
        return;
    const auto split = graphSplitUsesVerticalDivider_
        ? talas::app::workbenchPaneSplitRatio(
              position.x,
              graphSplitArea_.getX(),
              graphSplitArea_.getWidth())
        : talas::app::workbenchPaneSplitRatio(
              position.y,
              graphSplitArea_.getY(),
              graphSplitArea_.getHeight());
    const auto primarySplit = workspaceSession_.primaryPaneFirst
        ? split
        : 1.0 - split;
    if (std::abs(workspaceSession_.paneSplit - primarySplit) < 0.0001)
        return;
    workspaceSession_.paneSplit = primarySplit;
    resized();
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::setWorkspaceStructureSplitOrientation(
    talas::project::WorkspaceStructureSplitOrientation orientation)
{
    if (workspaceSession_.structureSplitOrientation == orientation)
        return;
    workspaceSession_.structureSplitOrientation = orientation;
    resized();
    noteWorkspaceSessionChanged();
    commandManager_.commandStatusChanged();
}

void WorkbenchComponent::swapWorkspaceStructurePanes()
{
    if (!secondaryGraphContext_.has_value()
        || !workspaceSession_.secondaryPaneVisible)
    {
        return;
    }
    workspaceSession_.primaryPaneFirst =
        !workspaceSession_.primaryPaneFirst;
    resized();
    noteWorkspaceSessionChanged();
    commandManager_.commandStatusChanged();
    setStatus(
        workspaceSession_.primaryPaneFirst
            ? "Primary Structure pane moved first"
            : "Secondary Structure pane moved first");
}

void WorkbenchComponent::toggleSecondaryWorkspacePane()
{
    if (!secondaryGraphContext_.has_value())
        return;

    workspaceSession_.secondaryPaneVisible =
        !workspaceSession_.secondaryPaneVisible;
    if (!workspaceSession_.secondaryPaneVisible
        && activeGraphContext_ == secondaryGraphContext_)
    {
        activeGraphContext_ = primaryGraphContext_;
        workspaceSession_.activeSoundDefinitionId =
            activeGraphContext_;
        const auto selection = talas::app::soundWorkbenchSessionSelection(
            workspaceSession_,
            optionalStringView(activeGraphContext_));
        selectNode(activeGraphContext_, selection);
        graphCanvas_->focusGraph();
    }
    resized();
    refreshGraphContextPresentation();
    noteWorkspaceSessionChanged();
    commandManager_.commandStatusChanged();
    setStatus(
        workspaceSession_.secondaryPaneVisible
            ? "Secondary Structure pane restored"
            : "Secondary Structure pane closed; its context remains cached");
}

void WorkbenchComponent::setWorkspaceGraphViewMode(
    talas::project::WorkspaceGraphViewMode mode)
{
    if (workspaceSession_.graphViewMode == mode)
        return;
    workspaceSession_.graphViewMode = mode;
    applyWorkspaceGraphViewMode();
    noteWorkspaceSessionChanged();
    commandManager_.commandStatusChanged();
}

void WorkbenchComponent::applyWorkspaceGraphViewMode()
{
    graphCanvas_->setGraphViewMode(workspaceSession_.graphViewMode);
    secondaryGraphCanvas_->setGraphViewMode(
        workspaceSession_.graphViewMode);
}

void WorkbenchComponent::applyWorkspaceGraphView(
    GraphCanvas& canvas,
    const std::optional<std::string>& definitionId)
{
    const auto* context =
        talas::project::findWorkspaceGraphSessionState(
            workspaceSession_,
            optionalStringView(definitionId));
    if (context == nullptr)
        return;
    canvas.setSessionViewState(
        context->panX,
        context->panY,
        context->zoom);
}

WorkbenchComponent::WorkspaceSessionRestore WorkbenchComponent::restoreWorkspaceSessionForPath(
    const std::filesystem::path& path)
{
    workspaceSession_ = {};
    if (!persistentStateEnabled_ || path.empty())
        return {};
    const auto sessionPath =
        talas::project::workspaceSessionFilePathForProject(
            workspaceSessionsPath_,
            path);
    if (!sessionPath.has_value())
    {
        return {
            false,
            "Workspace session ignored: invalid project identity",
        };
    }
    auto loaded = talas::project::loadWorkspaceSessionFile(
        *sessionPath);
    if (loaded.missing())
        return {};
    if (!loaded.ok())
    {
        return {
            false,
            "Workspace session ignored: "
                + juceString(loaded.error->message),
        };
    }
    workspaceSession_ = std::move(loaded.state);
    talas::project::reconcileWorkspaceSessionState(
        workspaceSession_,
        projectBridge_.editor().document());
    // Live activation is session-only but never auto-resumes after reopening.
    workspaceSession_.performance.activeLiveSetId.reset();
    workspaceSession_.performance.activeLiveSetCellId.reset();
    workspaceSession_.performance.queuedLiveSetId.reset();
    workspaceSession_.performance.queuedLiveSetCellId.reset();
    workspaceSession_.performance.queuedLiveSetRequestGeneration = 0;
    workspaceVariableRestorePending_ =
        !workspaceSession_.variableValues.empty();
    workspaceSoundVariableRestorePending_ =
        !workspaceSession_.soundVariableValues.empty();
    return {true, {}};
}

void WorkbenchComponent::applyWorkspaceSessionProjection(bool useStoredTransforms)
{
    const auto previouslySuppressed =
        suppressWorkspaceSessionPersistence_;
    suppressWorkspaceSessionPersistence_ = true;
    talas::project::reconcileWorkspaceSessionState(
        workspaceSession_,
        projectBridge_.editor().document());
    applyAccessibilityPreferences();
    refreshAnalysisWorkspace();
    workspaceMode_ = talas::app::workbenchWorkspaceMode(
        workspaceSession_.viewMode);
    sampleMidiRoutingEnabled_.store(
        workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Samples,
        std::memory_order_release);

    hierarchyPath_ =
        workspaceSession_.structureNavigationPath;
    primaryGraphContext_ = hierarchyPath_.size() > 1
        ? std::optional<std::string>{
              hierarchyPath_[hierarchyPath_.size() - 2]}
        : std::optional<std::string>{};
    secondaryGraphContext_ = hierarchyPath_.empty()
        ? std::optional<std::string>{}
        : std::optional<std::string>{hierarchyPath_.back()};
    activeGraphContext_ =
        workspaceSession_.activeSoundDefinitionId;

    soundDefinitionsPanel_->setBookmarks(
        talas::project::workspaceSessionBookmarkedDefinitionIds(
            workspaceSession_));
    refreshSoundLibraryProjection();
    applyWorkspaceGraphViewMode();
    refreshWorkspaceButtons();
    applyHierarchyContexts();
    resized();

    if (useStoredTransforms)
        applyWorkspaceGraphView(*graphCanvas_, primaryGraphContext_);
    else
        graphCanvas_->zoomToFit(false);
    if (secondaryGraphContext_.has_value())
    {
        if (useStoredTransforms)
        {
            applyWorkspaceGraphView(
                *secondaryGraphCanvas_,
                secondaryGraphContext_);
        }
        else
        {
            secondaryGraphCanvas_->zoomToFit(false);
        }
    }
    if (workspaceSession_
            .focusedPerformanceEventName
            .has_value()
        && talas::app::workbenchWorkspaceVisibility(
               workspaceMode_)
               .virtualControls)
    {
        static_cast<void>(
            virtualControlSurface_->focusEventValue(
                *workspaceSession_
                     .focusedPerformanceEventName));
    }
    suppressWorkspaceSessionPersistence_ =
        previouslySuppressed;
}

void WorkbenchComponent::applyAccessibilityPreferences()
{
    auto hostScale = static_cast<double>(getDesktopScaleFactor());
    if (const auto* display = juce::Desktop::getInstance()
                                  .getDisplays()
                                  .getDisplayForRect(getScreenBounds()))
    {
        hostScale = display->scale;
    }
    const auto projection = talas::app::workbenchAccessibilityProjection(
        workspaceSession_.accessibility,
        hostScale,
        workbenchHostReduceMotionPreference());
    if (!projection.has_value())
        return;
    if (*projection == accessibilityProjection_
        && WorkbenchType::interfaceScale()
            == projection->interfaceScale())
        return;

    const auto previousScale = std::max(1.0F, appliedInterfaceScale_);
    const auto nextScale = projection->interfaceScale();
    const auto ratio = nextScale / previousScale;
    accessibilityProjection_ = *projection;
    appliedInterfaceScale_ = nextScale;
    WorkbenchType::setInterfaceScale(nextScale);
    lookAndFeel_.setAccessibilityPresentation(
        projection->enhancedContrast(),
        static_cast<float>(projection->focusOutlinePixels()));

    const auto updateAuthoredFonts = [&](auto&& self,
                                         juce::Component& component) -> void {
        if (auto* label = dynamic_cast<juce::Label*>(&component))
        {
            auto font = label->getFont();
            font.setHeight(std::max(
                static_cast<float>(projection->minimumAuthoredTextPixels()),
                font.getHeight() * ratio));
            label->setFont(font);
        }
        else if (auto* editor = dynamic_cast<juce::TextEditor*>(&component))
        {
            auto font = editor->getFont();
            font.setHeight(std::max(
                static_cast<float>(projection->minimumAuthoredTextPixels()),
                font.getHeight() * ratio));
            editor->setFont(font);
        }
        for (auto index = 0; index < component.getNumChildComponents(); ++index)
        {
            if (auto* child = component.getChildComponent(index))
                self(self, *child);
        }
    };
    updateAuthoredFonts(updateAuthoredFonts, *this);
    sendLookAndFeelChange();
    resized();
    repaint();
}

void WorkbenchComponent::saveProject(SaveCompletion completion)
{
    if (projectPath().has_value())
    {
        if (!projectFileRevision_.has_value())
        {
            setStatus(
                juceString(
                    "Save blocked: the project file baseline is unavailable · use "
                    "Save As"));
            if (completion)
                completion(false);
            return;
        }
        const auto saved = saveProjectToPath(
            *projectPath(),
            projectFileRevision_);
        if (completion)
            completion(saved);
        return;
    }
    saveProjectAs(std::move(completion));
}

void WorkbenchComponent::saveProjectAs(SaveCompletion completion)
{
    if (sampleMapImportOwnsProjectBoundary())
    {
        setStatus(
            juceString(
                "Save As blocked while sample import owns the current project "
                "path · Apply or Cancel the import first"));
        if (completion)
            completion(false);
        return;
    }
    stopComponentReplacement();
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        if (completion)
            completion(false);
        return;
    }
    const auto initialLocation = projectPath().has_value()
        ? juce::File(jucePathString(*projectPath()))
        : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
              .getChildFile("Untitled.talas.json");
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Save Talas Project As",
        initialLocation,
        "*.talas.json;*.talastimeline");
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                              [safeThis, completion = std::move(completion)](
                                  const juce::FileChooser& chooser) mutable {
        if (safeThis == nullptr)
            return;
        safeThis->fileChooserActive_ = false;
        safeThis->scheduleWorkbenchFocusRestoration();
        auto selected = chooser.getResult();
        if (selected == juce::File{})
        {
            if (completion)
                completion(false);
            return;
        }
        if (selected.getFileExtension().isEmpty())
            selected = selected.withFileExtension(".talas.json");
        const auto saved = safeThis->saveProjectToPath(
            filesystemPath(selected.getFullPathName()));
        if (completion)
            completion(saved);
    });
}

void WorkbenchComponent::loadProjectFromPath(const std::filesystem::path& path)
{
    const auto resolved =
        talas::project::resolveProjectPackagePath(path);
    if (!resolved.ok())
    {
        auto status =
            "Open failed: " + juceString(resolved.error);
        std::error_code error;
        if (!std::filesystem::exists(path, error)
            && !error
            && recentProjects_.remove(path))
        {
            const auto persistenceWarning =
                persistRecentProjects();
            status += persistenceWarning.isEmpty()
                ? juceString(" · Removed missing recent project")
                : juceString(" · ") + persistenceWarning;
        }
        setStatus(status);
        return;
    }

    auto loaded = talas::project::loadProjectFile(
        resolved.projectFilePath);
    if (!loaded.ok())
    {
        const auto message = loaded.errors.empty() ? "Unable to load project" : loaded.errors.front().message;
        auto status = "Open failed: " + juceString(message);
        std::error_code error;
        if (!std::filesystem::exists(path, error) && !error
            && recentProjects_.remove(path))
        {
            const auto persistenceWarning = persistRecentProjects();
            status += persistenceWarning.isEmpty()
                ? juceString(" · Removed missing recent project")
                : juceString(" · ") + persistenceWarning;
        }
        setStatus(status);
        return;
    }
    const auto packageValidation =
        talas::project::validateProjectPackageAssets(
            *loaded.document,
            resolved);
    if (!packageValidation.ok())
    {
        juce::String status{
            "Open failed: package asset"};
        if (!packageValidation.assetId.empty())
        {
            status += " '";
            status += juceString(
                packageValidation.assetId);
            status += "'";
        }
        status += " ";
        status += juceString(
            packageValidation.error);
        setStatus(status);
        return;
    }

    if (const auto check =
            projectBridge_.editor().checkReplacementDocument(*loaded.document);
        !check.ok())
    {
        setStatus("Open failed: " + juceString(check.message));
        return;
    }
    stopRecoveryWriter();
    prepareForDocumentReplacement();
    const auto replaced = projectBridge_.editor().replaceDocument(std::move(*loaded.document));
    if (!replaced.ok())
    {
        rebuildOscEgress();
        updateRecoveryAfterDocumentChange();
        setStatus("Open failed: " + juceString(replaced.message));
        return;
    }

    resetForDocumentReplacement();
    editBoundary_.markLoaded(
        projectBridge_.editor().document(),
        resolved.projectFilePath);
    projectFileRevision_ =
        loaded.revision;
    projectChangedOnDisk_ = false;
    nextProjectFilePollMilliseconds_ =
        juce::Time::
            getMillisecondCounterHiRes()
        + 1000.0;
    const auto sessionRestore =
        restoreWorkspaceSessionForPath(
            resolved.projectFilePath);
    const auto deviceWarning = applyProjectDeviceSettings();
    documentChanged();
    applyWorkspaceSessionProjection(
        sessionRestore.restored);
    auto status =
        "Opened "
        + jucePathString(
            resolved.displayPath.filename());
    if (deviceWarning.isNotEmpty())
        status += juceString(" · ") + deviceWarning;
    const auto recentWarning =
        rememberRecentProject(
            resolved.displayPath);
    if (recentWarning.isNotEmpty())
        status += juceString(" · ") + recentWarning;
    if (sessionRestore.warning.isNotEmpty())
        status += juceString(" · ") + sessionRestore.warning;
    setStatus(status);
}

bool WorkbenchComponent::saveProjectToPath(
    const std::filesystem::path& path,
    const std::optional<
        talas::project::ProjectFileRevision>&
        expectedRevision)
{
    const auto previousProjectPath = projectPath();
    const auto resolved =
        talas::project::resolveProjectPackagePath(path);
    if (!resolved.ok())
    {
        setStatus(
            "Save failed: "
            + juceString(resolved.error));
        return false;
    }
    const auto& destinationPath =
        resolved.projectFilePath;
    auto documentToSave =
        projectBridge_.editor().document();
    static_cast<void>(
        talas::project::rebaseProjectAssetPaths(
            documentToSave,
            projectPath(),
            destinationPath));
    const auto packageValidation =
        talas::project::validateProjectPackageAssets(
            documentToSave,
            resolved);
    if (!packageValidation.ok())
    {
        setStatus(
            "Save failed: "
            + juceString(
                packageValidation.error));
        return false;
    }
    const auto saved =
        talas::project::saveProjectFile(
            destinationPath,
            documentToSave,
            expectedRevision);
    if (!saved.ok())
    {
        if (saved.error->code
            == talas::project::
                ProjectFileErrorCode::
                    ExternalConflict)
        {
            projectChangedOnDisk_ = true;
            refreshWindowTitle();
            setStatus(
                juceString(
                    "Save blocked: project changed on disk; external file preserved · use Save As"));
        }
        else
        {
            setStatus(
                "Save failed: "
                + juceString(
                    saved.error->message));
        }
        return false;
    }
    const auto rebased = projectBridge_.editor().rebaseAssetPathsForProjectMove(
        projectPath(),
        destinationPath);
    if (!rebased.ok())
    {
        setStatus("Save completed but asset rebasing failed: " + juceString(rebased.message));
        return false;
    }
    projectFileRevision_ =
        saved.revision;
    projectChangedOnDisk_ = false;
    pendingProjectFileStatus_.clear();
    nextProjectFilePollMilliseconds_ =
        juce::Time::
            getMillisecondCounterHiRes()
        + 1000.0;
    editBoundary_.markSaved(
        projectBridge_.editor().document(),
        destinationPath);
    if (previousProjectPath
        != std::optional<std::filesystem::path>{destinationPath})
    {
        resetAssetRecovery();
    }
    const auto sessionWarning =
        persistWorkspaceSessionForPath(
            destinationPath);
    const auto recoveryCleared = clearProjectRecovery();
    const auto recentWarning =
        rememberRecentProject(
            resolved.displayPath);
    refreshWindowTitle();
    auto status = recoveryCleared
        ? "Saved "
            + jucePathString(
                resolved.displayPath.filename())
        : juce::String{"Saved project, but recovery cleanup failed"};
    if (recentWarning.isNotEmpty())
        status += juceString(" · ") + recentWarning;
    if (sessionWarning.isNotEmpty())
        status += juceString(" · ") + sessionWarning;
    setStatus(status);
    return true;
}

void WorkbenchComponent::loadRecentProjectsState()
{
    auto loaded = talas::project::loadRecentProjectList(
        recentProjectsPath_);
    if (loaded.ok())
    {
        recentProjects_ = std::move(*loaded.list);
        if (loaded.usedBackup)
        {
            const auto repaired = talas::project::saveRecentProjectList(
                recentProjectsPath_,
                recentProjects_);
            if (!repaired.ok())
            {
                setStatus(
                    "Recent projects recovered, but state repair failed");
            }
            else
            {
                setStatus("Recovered recent projects from backup");
            }
        }
        return;
    }
    if (!loaded.missing())
    {
        setStatus(
            "Recent projects unavailable: "
            + juceString(
                loaded.error.has_value()
                    ? loaded.error->message
                    : "unknown state error"));
    }
}

juce::String WorkbenchComponent::persistRecentProjects()
{
    if (!persistentStateEnabled_)
        return {};
    const auto saved = talas::project::saveRecentProjectList(
        recentProjectsPath_,
        recentProjects_);
    if (saved.ok())
        return {};
    return "Recent-project state could not be saved";
}

juce::String WorkbenchComponent::rememberRecentProject(
    const std::filesystem::path& path)
{
    if (!persistentStateEnabled_)
        return {};
    static_cast<void>(recentProjects_.note(path));
    return persistRecentProjects();
}

void WorkbenchComponent::choosePackageDestination()
{
    if (packageRunning_)
    {
        setStatus("A project package is already running");
        return;
    }
    if (exportRunning_)
    {
        setStatus("Finish the audio export before packaging");
        return;
    }
    if (fileChooserActive_ || exportDialog_ != nullptr)
    {
        setStatus("Finish the active dialog before packaging");
        return;
    }

    // The File Archivist's Sound file, else the project open.
    const auto source = archivedProject_.has_value()
        ? std::optional<std::filesystem::path>{archivedProject_->second}
        : projectPath();
    juce::File initialFile;
    if (source.has_value())
    {
        const auto resolved =
            talas::project::resolveProjectPackagePath(
                *source);
        const auto displayPath = resolved.ok()
            ? resolved.displayPath
            : *source;
        auto displayFile =
            juce::File(jucePathString(displayPath));
        if (resolved.packageDirectory.has_value())
        {
            initialFile =
                displayFile.getSiblingFile(
                    displayFile.getFileNameWithoutExtension()
                    + " Copy.talaspkg");
        }
        else
        {
            initialFile =
                displayFile.withFileExtension(
                    ".talaspkg");
        }
    }
    else
    {
        initialFile =
            juce::File::getSpecialLocation(
                juce::File::userDocumentsDirectory)
                .getChildFile("Untitled.talaspkg");
    }

    fileChooser_ =
        std::make_unique<juce::FileChooser>(
            archivedProject_.has_value()
                ? "Save the Archive"
                : "Export Portable Project Package",
            initialFile,
            "*.talaspkg");
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent>
        safeThis(this);
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::
                canSelectFiles,
        [safeThis](
            const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            auto selected = chooser.getResult();
            if (selected == juce::File{})
            {
                safeThis->archivedProject_.reset();
                return;
            }
            if (selected.getFileExtension()
                    .isEmpty())
            {
                selected =
                    selected.withFileExtension(
                        ".talaspkg");
            }
            const auto outputPath =
                filesystemPath(
                    selected.getFullPathName());
            safeThis->scheduleContinuation(
                [safeThis, outputPath] {
                    if (safeThis != nullptr)
                    {
                        safeThis->
                            startProjectPackage(
                                outputPath);
                    }
                });
        });
}

// The Sound file a package holds: the one the File Archivist read, once,
// else the project open.
WorkbenchComponent::PackageSource WorkbenchComponent::takePackageSource()
{
    if (auto archived = std::exchange(archivedProject_, std::nullopt))
        return {std::move(archived->first), std::move(archived->second), true};
    return {projectBridge_.editor().document(), projectPath(), false};
}

void WorkbenchComponent::startProjectPackage(
    std::filesystem::path outputPath)
{
    // A Sound file the File Archivist read is packaged in place of the
    // project, and the project's edits do not cancel it.
    auto source = takePackageSource();
    if (packageRunning_)
    {
        setStatus("A project package is already running");
        return;
    }
    if (exportRunning_)
    {
        setStatus("Finish the audio export before packaging");
        return;
    }
    if (packageThread_.joinable())
        packageThread_.join();

    const auto operation = fileOperationCoordinator_.begin(
        talas::app::WorkbenchOperationKind::ProjectPackage);
    if (!operation.has_value())
    {
        setStatus("Unable to begin project packaging operation");
        return;
    }

    packageRunning_ = true;
    fileOperationToken_ = *operation;
    commandManager_.commandStatusChanged();
    setStatus("Packaging project... 0%");

    auto* const editor = &projectBridge_.editor();
    const auto documentAuthorityGeneration =
        editor->documentAuthorityGeneration();
    const auto archiving = source.archived;
    auto document = std::move(source.document);
    auto sourceProjectPath = std::move(source.path);
    const auto outputName =
        jucePathString(outputPath.filename());
    juce::Component::SafePointer<WorkbenchComponent>
        safeThis(this);
    try
    {
        packageThread_ = std::thread(
            [safeThis,
             document = std::move(document),
             sourceProjectPath =
                 std::move(sourceProjectPath),
             outputPath = std::move(outputPath),
             outputName,
             operation = *operation,
             operationOwner = this,
             archiving,
             documentAuthorityGeneration,
             coordinator =
                 &fileOperationCoordinator_]() mutable {
            talas::audio::ProjectPackageOptions
                options;
            options.outputPackagePath =
                std::move(outputPath);
            options.sourceProjectFilePath =
                std::move(sourceProjectPath);
            options.shouldCancel = [operationOwner,
                                    coordinator,
                                    operation,
                                    archiving,
                                    documentAuthorityGeneration] {
                return archiving
                    ? coordinator->shouldCancel(operation)
                    : operationOwner->fileOperationShouldCancel(
                        operation, documentAuthorityGeneration);
            };
            options.reportProgress =
                [coordinator, operation](double value) {
                    (void) coordinator->publishProgress(
                        operation,
                        value);
                };
            options.admitPublication =
                [operationOwner,
                 coordinator,
                 operation,
                 archiving,
                 documentAuthorityGeneration] {
                    return archiving
                        ? coordinator->admitPublication(operation)
                        : operationOwner->admitFileOperationPublication(
                            operation, documentAuthorityGeneration);
                };
            auto result =
                talas::audio::saveProjectPackage(
                    document,
                    options);

            juce::MessageManager::callAsync(
                [safeThis,
                 outputName,
                 operation,
                 result =
                     std::move(result)]() mutable {
                    if (safeThis == nullptr)
                        return;
                    const auto outcome = result.ok()
                        ? talas::app::WorkbenchOperationOutcome::Succeeded
                        : result.cancelled()
                            ? talas::app::WorkbenchOperationOutcome::Cancelled
                            : talas::app::WorkbenchOperationOutcome::Failed;
                    (void) safeThis->fileOperationCoordinator_.finish(
                        operation,
                        outcome);
                    const auto operationResult =
                        safeThis->fileOperationCoordinator_.snapshot();
                    safeThis->packageRunning_ =
                        false;
                    safeThis->fileOperationToken_.reset();
                    safeThis->commandManager_
                        .commandStatusChanged();
                    if (operationResult.phase
                        == talas::app::WorkbenchOperationPhase::Invalidated)
                    {
                        safeThis->setStatus(
                            "Project packaging cancelled because the project changed; existing packages were preserved");
                    }
                    else if (result.ok()
                             && operationResult.phase
                                 == talas::app::WorkbenchOperationPhase::Succeeded)
                    {
                        auto status =
                            "Packaged "
                            + outputName + " ("
                            + juce::String(
                                static_cast<
                                    juce::int64>(
                                    result
                                        .assetCount))
                            + " assets, "
                            + juce::String(
                                static_cast<
                                    juce::int64>(
                                    result
                                        .copiedByteCount))
                            + " bytes)";
                        if (!result.warning.empty())
                        {
                            status +=
                                juceString(" · ")
                                + juceString(
                                    result.warning);
                        }
                        safeThis->setStatus(
                            status);
                    }
                    else if (result.cancelled())
                    {
                        safeThis->setStatus(
                            "Project packaging cancelled");
                    }
                    else
                    {
                        safeThis->setStatus(
                            "Packaging failed: "
                            + juceString(
                                result.message));
                    }
                    });
            });
    }
    catch (const std::system_error& error)
    {
        (void) fileOperationCoordinator_.finish(
            *operation,
            talas::app::WorkbenchOperationOutcome::Failed);
        packageRunning_ = false;
        fileOperationToken_.reset();
        commandManager_.commandStatusChanged();
        setStatus(
            "Packaging failed to start: "
            + juceString(error.what()));
    }
}

void WorkbenchComponent::cancelProjectPackage()
{
    if (!packageRunning_
        || !packageThread_.joinable())
    {
        return;
    }
    if (fileOperationToken_.has_value()
        && fileOperationCoordinator_.requestCancel(
            *fileOperationToken_))
    {
        commandManager_.commandStatusChanged();
        setStatus("Cancelling project package...");
        return;
    }
    commandManager_.commandStatusChanged();
    setStatus(
        "Project package publication has started and can no longer be cancelled");
}

bool WorkbenchComponent::fileOperationShouldCancel(
    talas::app::WorkbenchOperationToken token,
    std::uint64_t documentAuthorityGeneration) const noexcept
{
    return projectBridge_.editor().documentAuthorityGeneration()
            != documentAuthorityGeneration
        || fileOperationCoordinator_.shouldCancel(token);
}

bool WorkbenchComponent::admitFileOperationPublication(
    talas::app::WorkbenchOperationToken token,
    std::uint64_t documentAuthorityGeneration) noexcept
{
    if (projectBridge_.editor().documentAuthorityGeneration()
        != documentAuthorityGeneration)
    {
        return false;
    }
    if (!fileOperationCoordinator_.admitPublication(token))
        return false;
    if (projectBridge_.editor().documentAuthorityGeneration()
        == documentAuthorityGeneration)
    {
        return true;
    }
    (void) fileOperationCoordinator_.invalidatePublication(token);
    return false;
}

double WorkbenchComponent::defaultExportDurationSeconds() const
{
    double duration = 10.0;
    for (const auto& event : projectBridge_.editor().document().timeline.eventValues)
    {
        if (std::isfinite(event.timeSeconds))
            duration = std::max(duration, event.timeSeconds + 1.0);
    }
    return duration;
}

double WorkbenchComponent::defaultExportSampleRate()
{
    if (auto* device = deviceManager_.getCurrentAudioDevice();
        device != nullptr && std::isfinite(device->getCurrentSampleRate())
        && device->getCurrentSampleRate() > 0.0)
    {
        return device->getCurrentSampleRate();
    }
    return 48000.0;
}

std::optional<double> WorkbenchComponent::parsePositiveFiniteDouble(const juce::String& text)
{
    const auto input = text.trim().toStdString();
    if (input.empty())
        return std::nullopt;
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtod(input.c_str(), &end);
    if (errno == ERANGE || end == input.c_str() || *end != '\0'
        || !std::isfinite(value) || value <= 0.0)
    {
        return std::nullopt;
    }
    return value;
}

void WorkbenchComponent::showExportAudioDialog()
{
    if (exportRunning_)
    {
        setStatus("An audio export is already running");
        return;
    }
    if (packageRunning_)
    {
        setStatus("Finish project packaging before exporting audio");
        return;
    }
    if (fileChooserActive_ || exportDialog_ != nullptr)
    {
        setStatus("Finish the active dialog before exporting");
        return;
    }

    exportDialog_ = std::make_unique<juce::AlertWindow>(
        "Export Audio",
        "Render the current project snapshot to a PCM16 WAV file.",
        juce::MessageBoxIconType::NoIcon,
        this);
    exportDialog_->addTextEditor(
        "duration",
        juce::String(defaultExportDurationSeconds(), 3),
        "Duration (seconds)");
    exportDialog_->addTextEditor(
        "sampleRate",
        juce::String(defaultExportSampleRate(), 0),
        "Sample rate (Hz)");
    exportDialog_->addButton("Export...", 1, juce::KeyPress(juce::KeyPress::returnKey));
    exportDialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    exportDialog_->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis](int result) {
            if (safeThis == nullptr || safeThis->exportDialog_ == nullptr)
                return;
            const auto durationText =
                safeThis->exportDialog_->getTextEditorContents("duration");
            const auto sampleRateText =
                safeThis->exportDialog_->getTextEditorContents("sampleRate");
            safeThis->exportDialog_.reset();
            safeThis->scheduleWorkbenchFocusRestoration();
            if (result != 1)
                return;

            const auto duration = parsePositiveFiniteDouble(durationText);
            const auto sampleRate = parsePositiveFiniteDouble(sampleRateText);
            if (!duration.has_value() || !sampleRate.has_value())
            {
                safeThis->setStatus(
                    "Export requires a positive finite duration and sample rate");
                return;
            }
            safeThis->chooseExportDestination(*duration, *sampleRate);
        }),
        false);
}

void WorkbenchComponent::chooseExportDestination(double durationSeconds, double sampleRate)
{
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }

    juce::File initialFile;
    if (projectPath().has_value())
    {
        initialFile = juce::File(jucePathString(*projectPath()))
                          .withFileExtension(".wav");
    }
    else
    {
        initialFile = juce::File::getSpecialLocation(
                          juce::File::userDocumentsDirectory)
                          .getChildFile("Untitled.wav");
    }
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Export Project Audio",
        initialFile,
        "*.wav");
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, durationSeconds, sampleRate](
            const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            auto selected = chooser.getResult();
            if (selected == juce::File{})
                return;
            if (selected.getFileExtension().isEmpty())
                selected = selected.withFileExtension(".wav");
            const auto outputPath =
                filesystemPath(selected.getFullPathName());
            safeThis->scheduleContinuation(
                [safeThis, outputPath, durationSeconds, sampleRate] {
                    if (safeThis != nullptr)
                    {
                        safeThis->startAudioExport(
                            outputPath,
                            durationSeconds,
                            sampleRate);
                    }
                });
        });
}

void WorkbenchComponent::startAudioExport(
    std::filesystem::path outputPath,
    double durationSeconds,
    double sampleRate)
{
    if (exportRunning_)
    {
        setStatus("An audio export is already running");
        return;
    }
    if (packageRunning_)
    {
        setStatus("Finish project packaging before exporting audio");
        return;
    }
    if (exportThread_.joinable())
        exportThread_.join();

    const auto operation = fileOperationCoordinator_.begin(
        talas::app::WorkbenchOperationKind::AudioExport);
    if (!operation.has_value())
    {
        setStatus("Unable to begin audio export operation");
        return;
    }

    exportRunning_ = true;
    fileOperationToken_ = *operation;
    commandManager_.commandStatusChanged();
    setStatus("Exporting audio... 0%");

    auto* const editor = &projectBridge_.editor();
    const auto documentAuthorityGeneration =
        editor->documentAuthorityGeneration();
    auto document = editor->document();
    auto sourceProjectPath = projectPath();
    const auto outputName = jucePathString(outputPath.filename());
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    try
    {
        exportThread_ = std::thread(
            [safeThis,
             document = std::move(document),
             sourceProjectPath = std::move(sourceProjectPath),
             outputPath = std::move(outputPath),
             outputName,
             durationSeconds,
             sampleRate,
             operation = *operation,
             operationOwner = this,
             documentAuthorityGeneration,
             coordinator = &fileOperationCoordinator_]() mutable {
            talas::audio::ProjectWavExportOptions options;
            options.outputPath = std::move(outputPath);
            options.projectFilePath = std::move(sourceProjectPath);
            options.durationSeconds = durationSeconds;
            options.sampleRate = sampleRate;
            options.maxBlockFrames = 4096;
            options.shouldCancel = [operationOwner,
                                    operation,
                                    documentAuthorityGeneration] {
                return operationOwner->fileOperationShouldCancel(
                    operation, documentAuthorityGeneration);
            };
            options.reportProgress = [coordinator, operation](double value) {
                (void) coordinator->publishProgress(operation, value);
            };
            options.admitPublication = [operationOwner,
                                        operation,
                                        documentAuthorityGeneration] {
                return operationOwner->admitFileOperationPublication(
                    operation, documentAuthorityGeneration);
            };
            auto result = talas::audio::exportProjectToPcm16Wav(
                document,
                talas::core::makeBuiltinNodeRegistry(),
                std::move(options));

            juce::MessageManager::callAsync(
                [safeThis,
                 outputName,
                 operation,
                 result = std::move(result)]() mutable {
                    if (safeThis == nullptr)
                        return;
                    const auto outcome = result.ok()
                        ? talas::app::WorkbenchOperationOutcome::Succeeded
                        : result.cancelled()
                            ? talas::app::WorkbenchOperationOutcome::Cancelled
                            : talas::app::WorkbenchOperationOutcome::Failed;
                    (void) safeThis->fileOperationCoordinator_.finish(
                        operation,
                        outcome);
                    const auto operationResult =
                        safeThis->fileOperationCoordinator_.snapshot();
                    safeThis->exportRunning_ = false;
                    safeThis->fileOperationToken_.reset();
                    safeThis->commandManager_.commandStatusChanged();
                    if (result.ok() || !result.diagnostics.empty())
                    {
                        safeThis->publishCompileDiagnostics(
                            result.diagnostics);
                    }
                    if (operationResult.phase
                        == talas::app::WorkbenchOperationPhase::Invalidated)
                    {
                        safeThis->setStatus(
                            "Audio export cancelled because the project changed; existing WAV files were preserved");
                    }
                    else if (result.ok()
                             && operationResult.phase
                                 == talas::app::WorkbenchOperationPhase::Succeeded)
                    {
                        safeThis->setStatus(
                            "Exported " + outputName + " ("
                            + juce::String(
                                static_cast<juce::int64>(result.frameCount))
                            + " frames, "
                            + juce::String(
                                static_cast<int>(result.channelCount))
                            + " ch)");
                    }
                    else if (result.cancelled())
                    {
                        safeThis->setStatus("Audio export cancelled");
                    }
                    else
                    {
                        safeThis->setStatus(
                            "Export failed: " + juceString(result.error));
                    }
                });
            });
    }
    catch (const std::system_error& error)
    {
        (void) fileOperationCoordinator_.finish(
            *operation,
            talas::app::WorkbenchOperationOutcome::Failed);
        exportRunning_ = false;
        fileOperationToken_.reset();
        commandManager_.commandStatusChanged();
        setStatus(
            "Audio export failed to start: "
            + juceString(error.what()));
    }
}

void WorkbenchComponent::cancelAudioExport()
{
    if (!exportRunning_ || !exportThread_.joinable())
        return;
    if (fileOperationToken_.has_value()
        && fileOperationCoordinator_.requestCancel(
            *fileOperationToken_))
    {
        commandManager_.commandStatusChanged();
        setStatus("Cancelling audio export...");
        return;
    }
    commandManager_.commandStatusChanged();
    setStatus(
        "Audio export publication has started and can no longer be cancelled");
}

void WorkbenchComponent::pollProjectFileChanges()
{
    if (!projectPath().has_value()
        || !projectFileRevision_.has_value())
    {
        return;
    }
    const auto now =
        juce::Time::getMillisecondCounterHiRes();
    if (now
        < nextProjectFilePollMilliseconds_)
    {
        return;
    }
    nextProjectFilePollMilliseconds_ =
        now + 1000.0;
    const auto current =
        talas::project::
            captureProjectFileRevision(
                *projectPath());
    const auto changed =
        !current.ok()
        || current.revision
            != projectFileRevision_;
    if (!changed)
    {
        if (projectChangedOnDisk_)
        {
            projectChangedOnDisk_ = false;
            pendingProjectFileStatus_.clear();
            refreshWindowTitle();
        }
        return;
    }
    if (projectChangedOnDisk_)
        return;

    projectChangedOnDisk_ = true;
    refreshWindowTitle();
    pendingProjectFileStatus_ =
        current.ok()
        ? juceString(
            "Project changed on disk · Save is blocked; use Save As")
        : "Project file cannot be verified: "
            + juceString(
                current.error.has_value()
                    ? current.error->message
                    : "unknown read error")
            + juceString(" · Save is blocked; use Save As");
}

[[nodiscard]] std::vector<talas::app::AssetRecoveryWorkbenchItem>
WorkbenchComponent::assetRecoveryItemsForFailure(
    const talas::audio::ProjectAssetLoadFailure& failure) const
{
    const auto& document = projectBridge_.editor().document();
    const auto resolvedReferences = talas::audio::reachableProjectAssetReferences(
        document, registry_);
    if (!resolvedReferences.ok())
        return {};
    const auto& references = resolvedReferences.references;
    std::vector<talas::app::AssetRecoveryWorkbenchItem> items;
    for (const auto& asset : document.assets)
    {
        if (asset.disposition
            != talas::project::ProjectAssetDisposition::Referenced)
        {
            continue;
        }
        const auto reference = std::ranges::find(
            references,
            asset.id,
            &talas::audio::ProjectAssetReference::assetId);
        if (reference == references.end())
            continue;

        const auto resolvedPath =
            talas::project::resolveProjectAssetPath(
                asset,
                projectPath());
        std::error_code statusError;
        const auto status = resolvedPath.empty()
            ? std::filesystem::file_status{
                  std::filesystem::file_type::none}
            : std::filesystem::status(
                  resolvedPath,
                  statusError);
        const auto missing = statusError
                == std::errc::no_such_file_or_directory
            || (!statusError
                && status.type()
                    == std::filesystem::file_type::not_found);
        const auto regular = !statusError
            && std::filesystem::is_regular_file(status);
        const auto isFailure = asset.id == failure.assetId;
        if (regular && !isFailure)
            continue;

        auto state = talas::app::AssetRecoveryItemState::Inaccessible;
        auto message = std::string{};
        if (asset.contentHash.empty())
        {
            state = talas::app::AssetRecoveryItemState::Unverified;
            message = isFailure
                ? failure.message
                    + "; choose a file for first inspection"
                : "Choose a file for first inspection";
        }
        else if (isFailure && failure.replacementMayBeAccepted)
        {
            state = talas::app::AssetRecoveryItemState::Changed;
            message = failure.message;
        }
        else if (missing)
        {
            state = talas::app::AssetRecoveryItemState::Missing;
            message = isFailure
                ? failure.message
                : "Referenced asset is missing";
        }
        else
        {
            state = talas::app::AssetRecoveryItemState::Inaccessible;
            if (isFailure)
                message = failure.message;
            else if (statusError)
                message = "Asset cannot be inspected: "
                    + statusError.message();
            else if (!regular)
                message = "Asset path is not a regular file";
            else
                message = "Asset cannot be inspected";
        }
        items.push_back({
            asset,
            state,
            reference->structuralSourcePath,
            std::move(message),
        });
    }
    return items;
}

[[nodiscard]] std::vector<talas::app::AssetRecoveryInventoryEntry>
WorkbenchComponent::assetRecoveryInventoryEntries() const
{
    const auto& document = projectBridge_.editor().document();
    const auto resolvedReferences = talas::audio::reachableProjectAssetReferences(
        document, registry_);
    if (!resolvedReferences.ok())
        return {};
    const auto& references = resolvedReferences.references;
    std::vector<talas::app::AssetRecoveryInventoryEntry> entries;
    entries.reserve(references.size());
    for (const auto& asset : document.assets)
    {
        if (asset.disposition
            != talas::project::ProjectAssetDisposition::Referenced)
        {
            continue;
        }
        const auto reference = std::ranges::find(
            references,
            asset.id,
            &talas::audio::ProjectAssetReference::assetId);
        if (reference == references.end())
            continue;
        entries.push_back({
            asset,
            reference->structuralSourcePath,
        });
    }
    return entries;
}

void WorkbenchComponent::refreshAssetRecoveryPanel()
{
    assetRecoveryPanel_->setState(
        assetRecoveryCoordinator_.state());
    if (!assetRecoveryCoordinator_.state().items.empty())
    {
        rightWorkspaceVisible_ = true;
        compactDock_ = CompactDock::RightWorkspace;
    }
    resized();
}

void WorkbenchComponent::stopAssetRecoveryWorker()
{
    assetRecoveryStopRequested_.store(
        true,
        std::memory_order_release);
    if (assetRecoveryThread_.joinable())
        assetRecoveryThread_.join();
    assetRecoveryWorkerFinished_.store(
        true,
        std::memory_order_release);
    const std::lock_guard lock(assetRecoveryProgressMutex_);
    assetRecoveryPendingProgress_.reset();
}

void WorkbenchComponent::finishAssetRecoveryWorkerIfReady()
{
    if (assetRecoveryThread_.joinable()
        && assetRecoveryWorkerFinished_.load(
            std::memory_order_acquire))
    {
        assetRecoveryThread_.join();
    }
}

void WorkbenchComponent::resetAssetRecovery()
{
    stopAssetRecoveryWorker();
    assetRecoveryCoordinator_.replaceItems({});
    refreshAssetRecoveryPanel();
}

void WorkbenchComponent::startAssetRecoveryInventoryInspection(
    std::vector<talas::app::AssetRecoveryInventoryEntry> entries)
{
    const auto generation =
        assetRecoveryCoordinator_.beginInventoryInspection();
    refreshAssetRecoveryPanel();
    setStatus("Inspecting referenced project assets...");
    assetRecoveryStopRequested_.store(
        false,
        std::memory_order_release);
    assetRecoveryWorkerFinished_.store(
        false,
        std::memory_order_release);
    const auto projectFilePath = projectPath();
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    assetRecoveryThread_ = std::thread(
        [this,
         safeThis,
         generation,
         entries = std::move(entries),
         projectFilePath]() mutable {
            auto result = talas::app::inspectAssetRecoveryInventory(
                entries,
                projectFilePath,
                [this] {
                    return assetRecoveryStopRequested_.load(
                        std::memory_order_acquire);
                });
            assetRecoveryWorkerFinished_.store(
                true,
                std::memory_order_release);
            juce::MessageManager::callAsync(
                [safeThis,
                 generation,
                 result = std::move(result)]() mutable {
                    if (safeThis == nullptr)
                        return;
                    const auto status = result.status;
                    if (!safeThis->assetRecoveryCoordinator_
                             .finishInventoryInspection(
                                 generation,
                                 std::move(result)))
                    {
                        return;
                    }
                    safeThis->refreshAssetRecoveryPanel();
                    if (status
                        == talas::app::AssetRecoveryInventoryStatus::Cancelled)
                    {
                        safeThis->setStatus(
                            "Project asset inspection cancelled; project unchanged");
                        return;
                    }
                    const auto count =
                        safeThis->assetRecoveryCoordinator_
                            .state().items.size();
                    safeThis->setStatus(
                        count == 0
                            ? juceString("Ready · all referenced project "
                                         "assets "
                                         "are available")
                            : juceString("Ready · ") +
                                  juce::String(static_cast<int>(count)) +
                                  (count == 1 ? " asset needs recovery"
                                              : " assets need recovery"));
                });
        });
}

[[nodiscard]] bool WorkbenchComponent::openAssetRecovery(
    const talas::audio::ProjectAssetLoadFailure& failure)
{
    auto items = assetRecoveryItemsForFailure(failure);
    auto inventoryEntries = assetRecoveryInventoryEntries();
    if (items.empty() || inventoryEntries.empty())
        return false;

    stopAssetRecoveryWorker();
    for (const auto& previous :
         assetRecoveryCoordinator_.state().items)
    {
        const auto found = std::ranges::find(
            items,
            previous.manifest.id,
            [](const auto& item) -> const std::string& {
                return item.manifest.id;
            });
        if (found != items.end()
            && previous.manifest == found->manifest
            && previous.ignored)
        {
            found->ignored = true;
            found->state =
                talas::app::AssetRecoveryItemState::Ignored;
            found->message = "Ignored for this session";
            found->candidates.clear();
            found->selectedCandidateIndex.reset();
        }
    }
    assetRecoveryCoordinator_.replaceItems(std::move(items));
    pendingAssetReplacementId_.reset();
    refreshAssetRecoveryPanel();
    setStatus(
        "Asset recovery opened: "
        + juceString(failure.assetId)
        + juceString(" · the active prepared graph remains unchanged"));
    juce::AccessibilityHandler::postAnnouncement(
        "Asset Recovery opened for "
            + juceString(failure.assetId)
            + ". The active prepared graph remains unchanged.",
        juce::AccessibilityHandler::AnnouncementPriority::high);
    startAssetRecoveryInventoryInspection(
        std::move(inventoryEntries));
    return true;
}

void WorkbenchComponent::openAssetRecoveryForAsset(
    std::string assetId,
    std::string message,
    bool replacementMayBeAccepted)
{
    const auto& document = projectBridge_.editor().document();
    const auto asset = std::ranges::find(
        document.assets,
        assetId,
        &talas::project::ProjectAsset::id);
    if (asset == document.assets.end())
        return;
    const auto resolvedReferences = talas::audio::reachableProjectAssetReferences(
        document, registry_);
    if (!resolvedReferences.ok())
    {
        setStatus(
            "Asset recovery is unavailable: "
            + juceString(resolvedReferences.failure->message));
        return;
    }
    const auto& references = resolvedReferences.references;
    const auto reference = std::ranges::find(
        references,
        assetId,
        &talas::audio::ProjectAssetReference::assetId);
    talas::audio::ProjectAssetLoadFailure failure;
    failure.assetId = std::move(assetId);
    failure.resolvedPath =
        talas::project::resolveProjectAssetPath(
            *asset,
            projectPath());
    failure.message = std::move(message);
    failure.replacementMayBeAccepted =
        replacementMayBeAccepted;
    if (reference != references.end())
    {
        failure.nodeId = reference->nodeId;
        failure.parameterId = reference->parameterId;
        failure.structuralSourcePath =
            reference->structuralSourcePath;
    }
    (void) openAssetRecovery(failure);
}

void WorkbenchComponent::locateAssetRecoveryFile(std::string assetId)
{
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }
    const auto item = std::ranges::find(
        assetRecoveryCoordinator_.state().items,
        assetId,
        [](const auto& candidate) -> const std::string& {
            return candidate.manifest.id;
        });
    if (item == assetRecoveryCoordinator_.state().items.end()
        || item->ignored)
    {
        setStatus("Asset recovery row is no longer available");
        return;
    }
    auto initial = projectPath().has_value()
        ? juce::File(jucePathString(projectPath()->parent_path()))
        : juce::File::getSpecialLocation(
              juce::File::userDocumentsDirectory);
    const auto stored =
        talas::project::resolveProjectAssetPath(
            item->manifest,
            projectPath());
    if (!stored.empty())
    {
        std::error_code ignored;
        const auto parent = stored.parent_path();
        if (std::filesystem::is_directory(parent, ignored)
            && !ignored)
        {
            initial = juce::File(jucePathString(parent));
        }
    }
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Locate " + juceString(assetId),
        initial,
        "*");
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, assetId = std::move(assetId)](
            const juce::FileChooser& chooser) mutable {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            const auto selected = chooser.getResult();
            if (selected == juce::File{})
                return;
            safeThis->startAssetRecoveryCandidateInspection(
                std::move(assetId),
                filesystemPath(selected.getFullPathName()));
        });
}

void WorkbenchComponent::startAssetRecoveryCandidateInspection(
    std::string assetId,
    std::filesystem::path candidatePath)
{
    stopAssetRecoveryWorker();
    const auto item = std::ranges::find(
        assetRecoveryCoordinator_.state().items,
        assetId,
        [](const auto& candidate) -> const std::string& {
            return candidate.manifest.id;
        });
    if (item == assetRecoveryCoordinator_.state().items.end())
        return;
    const auto manifest = item->manifest;
    const auto generation =
        assetRecoveryCoordinator_.beginCandidateInspection(
            assetId);
    if (!generation.has_value())
    {
        setStatus("Asset inspection could not start");
        return;
    }
    refreshAssetRecoveryPanel();
    setStatus("Inspecting selected asset file...");
    assetRecoveryStopRequested_.store(
        false,
        std::memory_order_release);
    assetRecoveryWorkerFinished_.store(
        false,
        std::memory_order_release);
    const auto projectFilePath = projectPath();
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    assetRecoveryThread_ = std::thread(
        [this,
         safeThis,
         generation = *generation,
         assetId = std::move(assetId),
         manifest,
         candidatePath = std::move(candidatePath),
         projectFilePath]() mutable {
            auto result =
                talas::project::classifyProjectAssetCandidate(
                    manifest,
                    candidatePath,
                    projectFilePath,
                    [this] {
                        return assetRecoveryStopRequested_.load(
                            std::memory_order_acquire);
                    });
            assetRecoveryWorkerFinished_.store(
                true,
                std::memory_order_release);
            juce::MessageManager::callAsync(
                [safeThis,
                 generation,
                 assetId = std::move(assetId),
                 result = std::move(result)]() mutable {
                    if (safeThis == nullptr)
                        return;
                    const auto wasOk = result.ok();
                    const auto wasCancelled = result.error.has_value()
                        && result.error->code
                            == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
                    const auto mode = wasOk
                        ? std::optional{result.resolution->mode}
                        : std::nullopt;
                    const auto message = result.error.has_value()
                        ? result.error->message
                        : std::string{};
                    if (!safeThis->assetRecoveryCoordinator_
                             .finishCandidateInspection(
                                 generation,
                                 assetId,
                                 std::move(result)))
                    {
                        return;
                    }
                    safeThis->refreshAssetRecoveryPanel();
                    if (!wasOk)
                    {
                        safeThis->setStatus(
                            wasCancelled
                                ? juce::String{"Asset inspection cancelled; project unchanged"}
                                : "Asset inspection failed: "
                                    + juceString(message));
                    }
                    else if (*mode
                             == talas::project::ProjectAssetResolutionMode::Replacement)
                    {
                        safeThis->setStatus(
                            juceString(
                                "Asset replacement blocked · different content found; review Replace before accepting it"));
                    }
                    else
                    {
                        safeThis->setStatus(
                            juceString(
                                "Ready · asset identity verified and available to apply"));
                    }
                });
        });
}

void WorkbenchComponent::searchAssetRecoveryFolder()
{
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }
    // The frequently used folders are searched first, one after another,
    // before a folder is asked for.
    if (!preferences_.frequentlyUsedFolders.empty() && !frequentFolderSearchStarted_)
    {
        frequentFolderSearchStarted_ = true;
        pendingFrequentFolderSearches_.clear();
        for (const auto& folder : preferences_.frequentlyUsedFolders)
            pendingFrequentFolderSearches_.push_back(talas::project::pathFromUtf8(folder));
        auto first = pendingFrequentFolderSearches_.front();
        pendingFrequentFolderSearches_.erase(pendingFrequentFolderSearches_.begin());
        startAssetRecoveryDirectorySearch(std::move(first));
        return;
    }
    frequentFolderSearchStarted_ = false;
    auto initial = projectPath().has_value()
        ? juce::File(jucePathString(projectPath()->parent_path()))
        : juce::File::getSpecialLocation(
              juce::File::userDocumentsDirectory);
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Search Folder for Exact Asset Matches",
        initial,
        "*");
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectDirectories,
        [safeThis](const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            const auto selected = chooser.getResult();
            if (selected == juce::File{})
                return;
            safeThis->startAssetRecoveryDirectorySearch(
                filesystemPath(selected.getFullPathName()));
        });
}

void WorkbenchComponent::startAssetRecoveryDirectorySearch(
    std::filesystem::path root)
{
    stopAssetRecoveryWorker();
    const auto projection =
        talas::app::assetRecoveryWorkbenchProjection(
            assetRecoveryCoordinator_.state());
    std::vector<talas::project::ProjectAsset> manifests;
    for (std::size_t index = 0;
         index < projection.rows.size()
             && index
                 < assetRecoveryCoordinator_.state().items.size();
         ++index)
    {
        if (projection.rows[index].searchEligible)
        {
            manifests.push_back(
                assetRecoveryCoordinator_.state()
                    .items[index].manifest);
        }
    }
    if (manifests.empty())
    {
        setStatus("No verified unresolved assets are eligible for folder search");
        return;
    }

    const auto generation = assetRecoveryCoordinator_.beginSearch();
    refreshAssetRecoveryPanel();
    setStatus("Searching selected folder for exact asset identities...");
    assetRecoveryStopRequested_.store(
        false,
        std::memory_order_release);
    assetRecoveryWorkerFinished_.store(
        false,
        std::memory_order_release);
    const auto projectFilePath = projectPath();
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    assetRecoveryThread_ = std::thread(
        [this,
         safeThis,
         generation,
         manifests = std::move(manifests),
         root = std::move(root),
         projectFilePath]() mutable {
            auto result =
                talas::project::searchProjectAssetDirectory(
                    manifests,
                    root,
                    projectFilePath,
                    {},
                    [this] {
                        return assetRecoveryStopRequested_.load(
                            std::memory_order_acquire);
                    },
                    [this, generation](
                        const talas::project::ProjectAssetSearchProgress&
                            progress) {
                        const std::lock_guard lock(
                            assetRecoveryProgressMutex_);
                        assetRecoveryPendingProgress_ =
                            std::pair{generation, progress};
                    });
            assetRecoveryWorkerFinished_.store(
                true,
                std::memory_order_release);
            juce::MessageManager::callAsync(
                [safeThis,
                 generation,
                 result = std::move(result)]() mutable {
                    if (safeThis == nullptr)
                        return;
                    const auto status = result.status;
                    if (!safeThis->assetRecoveryCoordinator_
                             .finishSearch(
                                 generation,
                                 std::move(result)))
                    {
                        return;
                    }
                    safeThis->refreshAssetRecoveryPanel();
                    const auto projection =
                        talas::app::assetRecoveryWorkbenchProjection(
                            safeThis->assetRecoveryCoordinator_.state());
                    switch (status)
                    {
                    case talas::project::ProjectAssetSearchStatus::Completed:
                        if (projection.applyCount == 0 && !safeThis->pendingFrequentFolderSearches_.empty())
                        {
                            auto next = safeThis->pendingFrequentFolderSearches_.front();
                            safeThis->pendingFrequentFolderSearches_.erase(
                                safeThis->pendingFrequentFolderSearches_.begin());
                            safeThis->startAssetRecoveryDirectorySearch(std::move(next));
                            return;
                        }
                        if (projection.applyCount == 0 && safeThis->frequentFolderSearchStarted_)
                        {
                            safeThis->setStatus("Not found in the frequently used folders · Search Folder... to choose another");
                            break;
                        }
                        safeThis->setStatus(
                            projection.applyCount == 0
                                ? juceString("Ready · asset search "
                                             "completed; no exact "
                                             "matches were found")
                                : juceString("Ready · asset search completed · ") +
                                      juce::String(static_cast<int>(
                                          projection.applyCount)) +
                                      " exact matches ready to apply");
                        break;
                    case talas::project::ProjectAssetSearchStatus::Cancelled:
                        safeThis->setStatus("Asset search cancelled; project unchanged");
                        break;
                    case talas::project::ProjectAssetSearchStatus::CapacityExceeded:
                        safeThis->setStatus("Asset search limit reached; select a narrower folder");
                        break;
                    case talas::project::ProjectAssetSearchStatus::InvalidRoot:
                        safeThis->setStatus("Asset search failed: selected folder is inaccessible");
                        break;
                    }
                });
        });
}

void WorkbenchComponent::pollAssetRecoveryProgress()
{
    std::optional<std::pair<
        std::uint64_t,
        talas::project::ProjectAssetSearchProgress>> progress;
    {
        const std::lock_guard lock(assetRecoveryProgressMutex_);
        progress = std::move(assetRecoveryPendingProgress_);
        assetRecoveryPendingProgress_.reset();
    }
    if (!progress.has_value())
        return;
    if (assetRecoveryCoordinator_.publishSearchProgress(
            progress->first,
            std::move(progress->second)))
    {
        assetRecoveryPanel_->setState(
            assetRecoveryCoordinator_.state());
    }
}

void WorkbenchComponent::cancelAssetRecoveryOperation()
{
    if (!assetRecoveryCoordinator_.cancelActiveOperation())
        return;
    assetRecoveryStopRequested_.store(
        true,
        std::memory_order_release);
    {
        const std::lock_guard lock(assetRecoveryProgressMutex_);
        assetRecoveryPendingProgress_.reset();
    }
    refreshAssetRecoveryPanel();
    setStatus("Asset recovery cancelled; project unchanged");
}

void WorkbenchComponent::applyAssetRecoveryRelocations()
{
    const auto batch = talas::app::assetRecoveryApplyBatch(
        assetRecoveryCoordinator_.state());
    if (batch.empty())
    {
        setStatus("No exact asset relocations are ready to apply");
        return;
    }
    const auto before = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    const auto applied = projectBridge_.editor()
                             .applyProjectAssetResolutions(batch);
    if (!applied.ok())
    {
        setStatus(
            "Asset relocation failed; project unchanged: "
            + juceString(applied.message));
        return;
    }

    std::unordered_set<std::string> appliedIds;
    for (const auto& resolution : batch)
        appliedIds.insert(resolution.asset.id);
    std::vector<talas::app::AssetRecoveryWorkbenchItem> remaining;
    for (const auto& item : assetRecoveryCoordinator_.state().items)
    {
        if (!appliedIds.contains(item.manifest.id))
            remaining.push_back(item);
    }
    assetRecoveryCoordinator_.replaceItems(std::move(remaining));
    const auto changed = before
        != talas::project::serializeProjectDocument(
            projectBridge_.editor().document());
    if (changed)
        documentChanged();
    else
        publishCompileDiagnostics({});
    refreshAssetRecoveryPanel();
    setStatus(
        changed
            ? juceString("Ready · applied ")
                + juce::String(static_cast<int>(batch.size()))
                + " asset recoveries as one Undo step"
            : juceString(
                "Ready · verified asset identity; no project edit was required"));
}

void WorkbenchComponent::replaceAssetRecoveryCandidate(std::string assetId)
{
    const auto confirmed =
        assetRecoveryCoordinator_
            .confirmedReplacementCandidate();
    if (!confirmed.has_value()
        || confirmed->asset.id != assetId)
    {
        if (!assetRecoveryCoordinator_
                 .requestReplacementConfirmation(assetId))
        {
            setStatus("No different-content candidate is ready for replacement");
            return;
        }
        refreshAssetRecoveryPanel();
        setStatus(
            juceString(
                "Ready for confirmation · review the old and new hashes, then "
                "choose Confirm Replace"));
        return;
    }

    const auto applied = projectBridge_.editor()
                             .applyProjectAssetResolutions(
                                 {*confirmed});
    if (!applied.ok())
    {
        setStatus(
            "Asset replacement failed; project unchanged: "
            + juceString(applied.message));
        return;
    }
    std::vector<talas::app::AssetRecoveryWorkbenchItem> remaining;
    for (const auto& item : assetRecoveryCoordinator_.state().items)
    {
        if (item.manifest.id != assetId)
            remaining.push_back(item);
    }
    assetRecoveryCoordinator_.replaceItems(std::move(remaining));
    documentChanged();
    refreshAssetRecoveryPanel();
    setStatus(
        juceString("Ready · accepted replacement for ") + juceString(assetId)
        + " as the next asset generation");
}

void WorkbenchComponent::ignoreAssetRecoveryItem(std::string assetId)
{
    if (!assetRecoveryCoordinator_.ignore(assetId))
    {
        setStatus("Asset recovery row could not be ignored");
        return;
    }
    refreshAssetRecoveryPanel();
    setStatus(
        juceString("Ready · ignored ") + juceString(assetId)
        + " for this session; project unchanged");
}

void WorkbenchComponent::selectAssetRecoveryCandidate(
    std::string assetId,
    std::size_t candidateIndex)
{
    if (!assetRecoveryCoordinator_.selectCandidate(
            assetId,
            candidateIndex))
    {
        setStatus("Exact asset candidate is no longer available");
        return;
    }
    refreshAssetRecoveryPanel();
    setStatus("Selected exact matching asset path");
}

void WorkbenchComponent::pollProjectAssetChanges()
{
    if (!currentDocumentStaged_
        || assetWatchBaseline_.entries.empty())
    {
        return;
    }
    const auto now =
        juce::Time::getMillisecondCounterHiRes();
    if (now
        < nextAssetWatchPollMilliseconds_)
    {
        return;
    }
    nextAssetWatchPollMilliseconds_ =
        now + 1000.0;
    const auto current =
        talas::audio::
            captureProjectAssetWatchSnapshot(
                projectBridge_.editor()
                    .document(),
                registry_,
                projectPath());
    const auto change =
        talas::audio::
            findProjectAssetWatchChange(
                assetWatchBaseline_,
                current);
    if (!change.has_value())
        return;

    fileOperationCoordinator_.advanceAuthority();
    scoreClipArtifactStateCache_.clear();
    invalidateAnalysisOutputTakeForProjectAuthorityChange();
    stopSoundLibraryAudition();
    stopAnalysisAudition();
    currentDocumentStaged_ = false;
    assetWatchBaseline_ = {};
    const auto currentEntry = std::ranges::find(
        current.entries,
        change->assetId,
        &talas::audio::ProjectAssetWatchEntry::assetId);
    const auto replacementMayBeAccepted =
        currentEntry != current.entries.end()
        && currentEntry->state
            == talas::audio::ProjectAssetWatchFileState::RegularFile;
    openAssetRecoveryForAsset(
        change->assetId,
        change->message,
        replacementMayBeAccepted);
}

bool WorkbenchComponent::acceptPendingProjectAssetReplacement()
{
    if (!pendingAssetReplacementId_.has_value())
        return true;

    const auto assetId = *pendingAssetReplacementId_;
    const auto& document =
        projectBridge_.editor().document();
    const auto found = std::ranges::find(
        document.assets,
        assetId,
        &talas::project::ProjectAsset::id);
    if (found == document.assets.end())
    {
        pendingAssetReplacementId_.reset();
        setStatus(
            "Asset replacement is no longer referenced: "
            + juceString(assetId));
        return false;
    }
    const auto resolvedPath =
        talas::audio::resolveProjectAssetPath(
            *found,
            projectPath());
    std::string contentHash;
    std::string loadError;
    auto replacement = *found;
    if (found->kind
        == talas::project::ProjectAssetKind::Table)
    {
        auto loaded = talas::audio::loadTableResource(
            resolvedPath);
        if (loaded.ok())
            contentHash = std::move(loaded.contentHash);
        else
            loadError = std::move(loaded.error);
    }
    else if (found->kind
             == talas::project::ProjectAssetKind::Binary)
    {
        auto loaded = talas::audio::loadBinaryResource(
            resolvedPath);
        if (loaded.ok())
            contentHash = std::move(loaded.contentHash);
        else
            loadError = std::move(loaded.error);
    }
    else
    {
        auto candidate = *found;
        candidate.contentHash.clear();
        candidate.channelCount = 0;
        candidate.sampleRate = 0.0;
        candidate.frameCount = 0;
        candidate.encoding.clear();
        auto inspected =
            talas::project::inspectProjectAsset(
                candidate,
                resolvedPath);
        if (inspected.ok())
        {
            replacement = std::move(inspected.asset);
            contentHash = replacement.contentHash;
        }
        else
        {
            loadError = inspected.error.has_value()
                ? std::move(inspected.error->message)
                : "unknown Audio inspection failure";
        }
    }
    if (!loadError.empty())
    {
        setStatus(
            "Asset replacement inspection failed: "
            + juceString(assetId)
            + juceString(" · ")
            + juceString(loadError));
        return false;
    }
    const auto acceptedIdentity =
        projectBridge_.acceptedResourceIdentity(
            assetId);
    const auto snapshotKind = [&] {
        switch (found->kind)
        {
        case talas::project::ProjectAssetKind::Table:
            return talas::core::ResourceSnapshotKind::Table;
        case talas::project::ProjectAssetKind::Binary:
            return talas::core::ResourceSnapshotKind::Binary;
        case talas::project::ProjectAssetKind::Audio:
            return talas::core::ResourceSnapshotKind::Audio;
        case talas::project::ProjectAssetKind::PartialSpectrum:
            return talas::core::ResourceSnapshotKind::PartialSpectrum;
        }
        return talas::core::ResourceSnapshotKind::Binary;
    }();
    const auto manifestMatchesHistory =
        !acceptedIdentity.has_value()
        || (acceptedIdentity->kind == snapshotKind
            && acceptedIdentity->generation
                == found->generation
            && acceptedIdentity->contentHash
                == found->contentHash);
    if (acceptedIdentity.has_value()
        && acceptedIdentity->kind
            != snapshotKind)
    {
        setStatus(
            "Asset kind cannot be reconciled with staged history: "
            + juceString(assetId));
        return false;
    }
    if (contentHash == found->contentHash
        && manifestMatchesHistory)
    {
        pendingAssetReplacementId_.reset();
        return true;
    }

    replacement.contentHash =
        std::move(contentHash);
    const auto accepted = acceptedIdentity.has_value()
        ? projectBridge_.editor()
              .reconcileProjectAssetRuntimeIdentity(
                  std::move(replacement),
                  acceptedIdentity->generation,
                  acceptedIdentity->contentHash)
        : projectBridge_.editor()
              .acceptProjectAssetReplacement(
                  std::move(replacement));
    if (!accepted.ok())
    {
        setStatus(
            "Asset replacement was not accepted: "
            + juceString(assetId)
            + juceString(" · ")
            + juceString(accepted.message));
        return false;
    }

    pendingAssetReplacementId_.reset();
    documentChanged();
    return true;
}

[[nodiscard]] std::optional<std::string>
WorkbenchComponent::firstProjectAssetIdentityConflict() const
{
    const auto& document =
        projectBridge_.editor().document();
    const auto resolvedReferences = talas::audio::reachableProjectAssetReferences(
        document, registry_);
    if (!resolvedReferences.ok())
    {
        return "Asset reference resolution failed: "
            + resolvedReferences.failure->message;
    }
    const auto& references = resolvedReferences.references;
    for (const auto& asset : document.assets)
    {
        if (std::ranges::find(
                   references,
                   asset.id,
                   &talas::audio::ProjectAssetReference::assetId)
                == references.end())
        {
            continue;
        }
        const auto accepted =
            projectBridge_.acceptedResourceIdentity(
                asset.id);
        if (!accepted.has_value())
            continue;
        const auto kind = [&] {
            switch (asset.kind)
            {
            case talas::project::ProjectAssetKind::Table:
                return talas::core::ResourceSnapshotKind::Table;
            case talas::project::ProjectAssetKind::Binary:
                return talas::core::ResourceSnapshotKind::Binary;
            case talas::project::ProjectAssetKind::Audio:
                return talas::core::ResourceSnapshotKind::Audio;
            case talas::project::ProjectAssetKind::PartialSpectrum:
                return talas::core::ResourceSnapshotKind::PartialSpectrum;
            }
            return talas::core::ResourceSnapshotKind::Binary;
        }();
        if (kind != accepted->kind
            || asset.generation
                < accepted->generation
            || (asset.generation
                    == accepted->generation
                && asset.contentHash
                    != accepted->contentHash)
            || (asset.generation
                    > accepted->generation
                && asset.contentHash
                    == accepted->contentHash))
        {
            return asset.id;
        }
    }
    return {};
}

[[nodiscard]] std::optional<std::string>
WorkbenchComponent::firstChangedLoadedResourceSnapshot(
    std::span<
        const talas::core::ResourceSnapshotPtr>
        snapshots) const
{
    for (const auto& snapshot : snapshots)
    {
        if (snapshot == nullptr
            || snapshot->generation == 0
            || snapshot->resolvedPath.empty())
        {
            continue;
        }
        const auto current =
            talas::project::calculateProjectAssetSha256(
                snapshot->resolvedPath);
        if (!current.ok()
            || current.contentHash
                != snapshot->contentHash)
        {
            return snapshot->id;
        }
    }
    return {};
}

}
