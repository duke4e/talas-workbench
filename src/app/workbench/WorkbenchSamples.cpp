#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
namespace
{
[[nodiscard]] const talas::project::ProjectSampleMap* activeSampleMap(
    const talas::project::ProjectDocument& document,
    const talas::project::WorkspaceSamplesSessionState& session)
{
    return session.activeMapId.has_value()
        ? talas::project::findProjectSampleMap(
              document,
              *session.activeMapId)
        : nullptr;
}

[[nodiscard]] const talas::project::ProjectSampleRegion* primarySampleRegion(
    const talas::project::ProjectSampleMap* map,
    const talas::project::WorkspaceSamplesSessionState& session)
{
    if (map == nullptr || session.selectedRegionIds.empty())
        return nullptr;
    return talas::project::findProjectSampleRegion(
        *map,
        session.selectedRegionIds.front());
}

[[nodiscard]] const talas::project::ProjectAsset* sampleAsset(
    const talas::project::ProjectDocument& document,
    const talas::project::ProjectSampleRegion* region)
{
    if (region == nullptr)
        return nullptr;
    const auto found = std::ranges::find(
        document.assets,
        region->assetId,
        &talas::project::ProjectAsset::id);
    return found == document.assets.end() ? nullptr : &*found;
}

[[nodiscard]] std::string nextSampleMapId(
    const talas::project::ProjectDocument& document)
{
    for (std::size_t ordinal = 1;
         ordinal <= talas::project::maximumProjectSampleMaps;
         ++ordinal)
    {
        auto candidate = "sample_map_" + std::to_string(ordinal);
        if (talas::project::findProjectSampleMap(document, candidate) == nullptr)
            return candidate;
    }
    return {};
}

[[nodiscard]] talas::project::ProjectParameterExpression textParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

enum class CleanupArtifactPathState
{
    Valid,
    Missing,
    Invalid,
};

[[nodiscard]] CleanupArtifactPathState cleanupArtifactPathState(
    const std::filesystem::path& path,
    bool staging)
{
    const auto assetsDirectory = path.parent_path();
    const auto packageDirectory = assetsDirectory.parent_path();
    if (path.empty() || !path.is_absolute()
        || path.lexically_normal() != path
        || assetsDirectory.filename()
            != talas::project::projectPackageAssetsDirectoryName)
    {
        return CleanupArtifactPathState::Invalid;
    }
    if (staging
        && path.filename().string().find(".tmp-")
            == std::string::npos)
    {
        return CleanupArtifactPathState::Invalid;
    }

    std::error_code targetError;
    const auto targetStatus =
        std::filesystem::symlink_status(path, targetError);
    if (targetError == std::errc::no_such_file_or_directory
        || (!targetError
            && targetStatus.type()
                == std::filesystem::file_type::not_found))
    {
        return CleanupArtifactPathState::Missing;
    }
    if (targetError || std::filesystem::is_symlink(targetStatus)
        || (staging
                ? !std::filesystem::is_directory(targetStatus)
                : !std::filesystem::is_regular_file(targetStatus)))
    {
        return CleanupArtifactPathState::Invalid;
    }

    auto ancestor = packageDirectory.root_path();
    for (const auto& component : packageDirectory.relative_path())
    {
        ancestor /= component;
        std::error_code ancestorError;
        const auto ancestorStatus =
            std::filesystem::symlink_status(
                ancestor,
                ancestorError);
        if (ancestorError
            || std::filesystem::is_symlink(ancestorStatus))
        {
            return CleanupArtifactPathState::Invalid;
        }
    }

    const auto resolved = talas::project::resolveProjectPackagePath(
        packageDirectory);
    if (!resolved.ok()
        || !resolved.packageDirectory.has_value())
    {
        return CleanupArtifactPathState::Invalid;
    }
    std::error_code canonicalError;
    const auto canonicalPackage = std::filesystem::canonical(
        packageDirectory,
        canonicalError);
    if (canonicalError)
        return CleanupArtifactPathState::Invalid;
    const auto canonicalResolved = std::filesystem::canonical(
        *resolved.packageDirectory,
        canonicalError);
    if (canonicalError || canonicalResolved != canonicalPackage)
        return CleanupArtifactPathState::Invalid;
    const auto canonicalAssets = std::filesystem::canonical(
        assetsDirectory,
        canonicalError);
    if (canonicalError
        || canonicalAssets
            != canonicalPackage
                / talas::project::projectPackageAssetsDirectoryName)
    {
        return CleanupArtifactPathState::Invalid;
    }
    return CleanupArtifactPathState::Valid;
}

[[nodiscard]] std::string discardOwnedCleanupFile(
    const std::filesystem::path& path,
    std::string_view expectedContentHash,
    std::string_view expectedPackageProjectHash)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
        return {};
    if (error)
        return "Unable to inspect retained cleanup file: " + error.message();
    if (status.type() == std::filesystem::file_type::not_found)
        return {};
    if (std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status))
    {
        return "Retained cleanup file is no longer a regular owned file";
    }
    const auto currentHash =
        talas::project::calculateProjectAssetSha256(path);
    if (!currentHash.ok())
        return "Unable to fingerprint retained cleanup file";
    if (currentHash.contentHash != expectedContentHash)
        return {};

    const auto packageDirectory = path.parent_path().parent_path();
    const auto projectPath = packageDirectory
        / talas::project::projectPackageDocumentFileName;
    const auto currentProjectHash =
        talas::project::calculateProjectAssetSha256(projectPath);
    if (!currentProjectHash.ok())
        return "Unable to fingerprint retained cleanup package project";
    if (currentProjectHash.contentHash
        != expectedPackageProjectHash)
    {
        return {};
    }
    const auto loaded = talas::project::loadProjectFile(projectPath);
    if (!loaded.ok())
        return "Unable to parse retained cleanup package project";
    const auto relativePath = path.lexically_relative(packageDirectory);
    if (std::ranges::any_of(
            loaded.document->assets,
            [&relativePath](const auto& asset) {
                return talas::project::pathFromUtf8(asset.path)
                           .lexically_normal()
                    == relativePath;
            }))
    {
        return {};
    }
    if (!std::filesystem::remove(path, error) || error)
    {
        return "Unable to remove retained cleanup file"
            + (error ? ": " + error.message() : std::string{});
    }
    return {};
}
}

void WorkbenchComponent::refreshSampleMapWorkspace()
{
    auto& session = workspaceSession_.samples;
    const auto& document = projectBridge_.editor().document();
    talas::project::reconcileWorkspaceSessionState(workspaceSession_, document);
    constexpr talas::app::SampleMapCanvas projectionCanvas{1000.0, 400.0};
    const auto viewport = talas::app::sampleMapViewportFromSession(session);
    auto projection = talas::app::projectSampleMapWorkbench(
        document,
        session,
        viewport,
        projectionCanvas);

    std::string status;
    if (sampleImportPresentation_ == SampleMapImportPresentation::Running)
        status = "Inspecting and preparing import candidate";
    else if (sampleImportPresentation_ == SampleMapImportPresentation::Preview)
        status = sampleImportStatus_.toStdString();
    else if (sampleImportPresentation_ == SampleMapImportPresentation::Cancelled)
        status = "Import cancelled; project was not changed";
    else if (sampleImportPresentation_ == SampleMapImportPresentation::Failed)
        status = sampleImportStatus_.toStdString();
    else if (projection.maps.empty())
        status = "No sample maps in this project";
    else if (projection.regions.empty())
        status = "Active map is empty";
    else
        status = std::to_string(projection.regions.size())
            + (projection.regions.size() == 1 ? " region" : " regions")
            + " · project format 13";

    sampleMapPanel_->setState({
        std::move(projection),
        viewport,
        projectionCanvas,
        selectedSampleMidi_,
        sampleImportPresentation_,
        sampleImportProgress_.load(std::memory_order_relaxed),
        std::move(status),
        sampleImportEmbed_,
        sampleImportPreviewProjection_.regions,
        sampleImportLowNote_,
        sampleImportAnchorRootNote_,
        sampleImportHighNote_,
        sampleImportPresentation_ == SampleMapImportPresentation::Preview
            && sampleImportPreviewProjection_.ok(),
    });

    const auto* map = activeSampleMap(document, session);
    const auto* primary = primarySampleRegion(map, session);
    const auto* asset = sampleAsset(document, primary);
    auto assetPresentation = SampleAssetPresentation::Ready;
    std::string assetStatus;
    auto assetActionRunning = false;
    auto canShowAsset = false;
    auto canEmbedAsset = false;
    auto canReferenceAsset = false;
    if (asset != nullptr)
    {
        const auto resolvedPath = talas::project::resolveProjectAssetPath(
            *asset,
            projectPath());
        std::error_code assetStatusError;
        canShowAsset = !resolvedPath.empty()
            && std::filesystem::is_regular_file(
                resolvedPath,
                assetStatusError)
            && !assetStatusError;
        const auto package = projectPath().has_value()
            ? std::optional<talas::project::ProjectPackagePathResult>{
                  talas::project::resolveProjectPackagePath(*projectPath())}
            : std::nullopt;
        canEmbedAsset = canShowAsset
            && asset->disposition
                == talas::project::ProjectAssetDisposition::Referenced
            && package.has_value()
            && package->ok()
            && package->packageDirectory.has_value();
        canReferenceAsset = asset->disposition
            == talas::project::ProjectAssetDisposition::Embedded;
        assetActionRunning = sampleAssetDispositionAssetId_ == asset->id
            && !sampleAssetDispositionWorkerFinished_.load(
                std::memory_order_acquire);
        const auto watched = std::ranges::find(
            assetWatchBaseline_.entries,
            asset->id,
            &talas::audio::ProjectAssetWatchEntry::assetId);
        if (watched != assetWatchBaseline_.entries.end()
            && watched->state != talas::audio::ProjectAssetWatchFileState::RegularFile)
        {
            assetPresentation = SampleAssetPresentation::Missing;
            assetStatus = watched->error.empty()
                ? "Source is missing or inaccessible"
                : watched->error;
        }
        if (pendingAssetReplacementId_ == asset->id)
        {
            assetPresentation = SampleAssetPresentation::Changed;
            assetStatus = "Source bytes changed; Reload requires explicit acceptance";
        }
        else if (!sampleWaveformError_.isEmpty()
                 && sampleWaveformAssetId_ == asset->id)
        {
            assetPresentation = SampleAssetPresentation::Missing;
            assetStatus = sampleWaveformError_.toStdString();
        }
        if (sampleAssetDispositionAssetId_ == asset->id
            && sampleAssetDispositionStatus_.isNotEmpty())
        {
            if (!assetStatus.empty())
                assetStatus += " · ";
            assetStatus += sampleAssetDispositionStatus_.toStdString();
        }
    }
    sampleRegionInspector_->setState({
        session.selectedRegionIds.size(),
        primary != nullptr
            ? std::optional<talas::project::ProjectSampleRegion>{*primary}
            : std::nullopt,
        asset != nullptr
            ? std::optional<talas::project::ProjectAsset>{*asset}
            : std::nullopt,
        assetPresentation,
        std::move(assetStatus),
        assetActionRunning,
        canShowAsset,
        canEmbedAsset,
        canReferenceAsset,
    });

    requestSelectedSampleWaveform();
    auto waveformViewport = selectedWaveformOverview_ != nullptr
        ? talas::app::waveformViewportFromSession(
              session,
              selectedWaveformOverview_->frameCount)
        : talas::app::WaveformViewport{};
    waveformPanel_->setState({
        selectedWaveformOverview_,
        primary != nullptr
            ? std::optional<talas::project::ProjectSampleRegion>{*primary}
            : std::nullopt,
        waveformViewport,
        sampleWaveformCursorFrame_,
        !sampleWaveformWorkerFinished_.load(std::memory_order_acquire),
        sampleWaveformError_.toStdString(),
        sampleMapAuditionPresentation_,
    });
    commandManager_.commandStatusChanged();
}

void WorkbenchComponent::selectSampleMap(std::string mapId)
{
    if (workspaceSession_.samples.activeMapId == mapId)
        return;
    if (sampleMapAuditionActive_
        || sampleMapAuditionPresentation_
            == SampleMapAuditionPresentation::Preparing)
    {
        stopSampleMapAudition();
    }
    workspaceSession_.samples.activeMapId = std::move(mapId);
    workspaceSession_.samples.selectedRegionIds.clear();
    selectedSampleMidi_.reset();
    sampleWaveformCursorFrame_ = 0;
    noteWorkspaceSessionChanged();
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::createSampleMap()
{
    const auto& document = projectBridge_.editor().document();
    const auto mapId = nextSampleMapId(document);
    if (mapId.empty())
    {
        setStatus("Sample-map limit reached");
        return;
    }
    auto displayName = "Sample Map "
        + std::to_string(document.sampleMapLibrary.maps.size() + 1);
    const auto result = projectBridge_.editor().addSampleMap({
        mapId,
        std::move(displayName),
        {},
    });
    if (!result.ok())
    {
        setStatus("New sample map failed: " + juceString(result.message));
        return;
    }
    workspaceSession_.samples.activeMapId = mapId;
    workspaceSession_.samples.selectedRegionIds.clear();
    documentChanged();
    setStatus("Created a portable sample map");
}

void WorkbenchComponent::renameSampleMap(std::string displayName)
{
    const auto& active = workspaceSession_.samples.activeMapId;
    if (!active.has_value())
        return;
    const auto result = projectBridge_.editor().renameSampleMap(
        *active,
        std::move(displayName));
    if (!result.ok())
    {
        setStatus("Rename failed: " + juceString(result.message));
        return;
    }
    documentChanged();
    setStatus("Sample map renamed");
}

void WorkbenchComponent::removeSampleMap(std::string mapId)
{
    const auto result = projectBridge_.editor().removeSampleMap(mapId);
    if (!result.ok())
    {
        setStatus("Remove failed: " + juceString(result.message));
        return;
    }
    workspaceSession_.samples.activeMapId.reset();
    workspaceSession_.samples.selectedRegionIds.clear();
    documentChanged();
    setStatus("Removed sample map; project assets were retained");
}

void WorkbenchComponent::selectSampleRegion(
    std::string regionId,
    talas::app::SampleMapSelectionMode mode)
{
    workspaceSession_.samples.selectedRegionIds =
        talas::app::updateSampleMapSelection(
            workspaceSession_.samples.selectedRegionIds,
            std::move(regionId),
            mode);
    noteWorkspaceSessionChanged();
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::selectSampleRegionsByMidi(
    std::uint8_t note,
    std::uint8_t velocity)
{
    selectedSampleMidi_ = talas::app::SampleMapMidiPoint{note, velocity};
    const auto* map = activeSampleMap(
        projectBridge_.editor().document(),
        workspaceSession_.samples);
    workspaceSession_.samples.selectedRegionIds = map != nullptr
        ? talas::app::sampleMapRegionIdsAtMidi(*map, note, velocity)
        : std::vector<std::string>{};
    noteWorkspaceSessionChanged();
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::handleSampleMapMidiNoteOn(
    int channel,
    std::uint8_t note,
    std::uint8_t velocity,
    double arrivalTimeSeconds)
{
    selectSampleRegionsByMidi(note, velocity);
    if (workspaceSession_.samples.selectedRegionIds.empty())
        return;
    if (sampleMapAuditionPresentation_
        == SampleMapAuditionPresentation::Stopping)
    {
        setStatus("Wait for the current project graph to finish restoring");
        return;
    }
    SampleMapAuditionVoice voice{
        std::clamp(channel, 1, 16),
        note,
        velocity,
        arrivalTimeSeconds,
        true,
        true,
    };
    if (sampleMapAuditionActive_)
    {
        if (!projectBridge_.enqueueMidiNoteOn(
                voice.channel,
                voice.note,
                voice.velocity,
                voice.arrivalTimeSeconds))
        {
            setStatus("Sample-map MIDI note-on could not enter the realtime boundary");
            return;
        }
        sampleMapAuditionActiveVoice_ = voice;
        setStatus("Auditioning isolated sample map from MIDI");
        refreshSampleMapWorkspace();
        return;
    }
    if (sampleMapAuditionPresentation_
        == SampleMapAuditionPresentation::Preparing)
    {
        ++sampleMapAuditionGeneration_;
        stopSampleMapAuditionWorker();
        sampleMapAuditionPendingVoice_.reset();
        sampleMapAuditionPresentation_ = SampleMapAuditionPresentation::Idle;
    }
    beginSampleMapAudition(voice);
}

void WorkbenchComponent::handleSampleMapMidiNoteOff(
    int channel,
    std::uint8_t note,
    double arrivalTimeSeconds)
{
    if (sampleMapAuditionPresentation_
            == SampleMapAuditionPresentation::Preparing
        && sampleMapAuditionPendingVoice_.has_value()
        && sampleMapAuditionPendingVoice_->physicalKey
        && sampleMapAuditionPendingVoice_->channel == channel
        && sampleMapAuditionPendingVoice_->note == note)
    {
        stopSampleMapAudition();
        return;
    }
    if (!sampleMapAuditionActive_
        || !sampleMapAuditionActiveVoice_.has_value()
        || sampleMapAuditionActiveVoice_->channel != channel
        || sampleMapAuditionActiveVoice_->note != note
        || !sampleMapAuditionActiveVoice_->keyDown)
    {
        return;
    }
    if (projectBridge_.enqueueMidiNoteOff(
            channel,
            note,
            arrivalTimeSeconds))
    {
        sampleMapAuditionActiveVoice_->keyDown = false;
        setStatus("Released isolated sample-map audition note");
        refreshSampleMapWorkspace();
    }
}

void WorkbenchComponent::moveSelectedSampleRegions(
    int noteDelta,
    int velocityDelta)
{
    const auto* map = activeSampleMap(
        projectBridge_.editor().document(),
        workspaceSession_.samples);
    if (map == nullptr || workspaceSession_.samples.selectedRegionIds.empty())
        return;
    const auto moved = talas::app::moveSampleMapRegions(
        map->regions,
        workspaceSession_.samples.selectedRegionIds,
        noteDelta,
        velocityDelta);
    if (moved.appliedNoteDelta == 0
        && moved.appliedVelocityDelta == 0)
    {
        setStatus("Selection is already at the MIDI boundary");
        return;
    }
    const auto result = projectBridge_.editor().updateSampleRegions(
        map->mapId,
        moved.regions);
    if (!result.ok())
    {
        setStatus("Region move failed: " + juceString(result.message));
        return;
    }
    documentChanged();
    setStatus(
        "Moved selection by note "
        + juce::String(moved.appliedNoteDelta)
        + ", velocity "
        + juce::String(moved.appliedVelocityDelta));
}

void WorkbenchComponent::resizePrimarySampleRegion(
    talas::app::SampleMapResizeEdge edge,
    int delta)
{
    const auto* map = activeSampleMap(
        projectBridge_.editor().document(),
        workspaceSession_.samples);
    const auto* region = primarySampleRegion(
        map,
        workspaceSession_.samples);
    if (map == nullptr || region == nullptr)
        return;
    auto resized = talas::app::resizeSampleMapRegion(*region, edge, delta);
    const auto result = projectBridge_.editor().updateSampleRegion(
        map->mapId,
        region->regionId,
        std::move(resized));
    if (!result.ok())
    {
        setStatus("Region resize failed: " + juceString(result.message));
        return;
    }
    documentChanged();
    setStatus("Resized selected region by one exact MIDI boundary");
}

void WorkbenchComponent::applySampleRegion(
    talas::project::ProjectSampleRegion region)
{
    const auto* map = activeSampleMap(
        projectBridge_.editor().document(),
        workspaceSession_.samples);
    if (map == nullptr)
        return;
    const auto regionId = region.regionId;
    const auto result = projectBridge_.editor().updateSampleRegion(
        map->mapId,
        regionId,
        std::move(region));
    if (!result.ok())
    {
        setStatus("Region update failed: " + juceString(result.message));
        return;
    }
    documentChanged();
    setStatus("Applied exact region and loop values");
}

void WorkbenchComponent::removeSelectedSampleRegions()
{
    const auto* map = activeSampleMap(
        projectBridge_.editor().document(),
        workspaceSession_.samples);
    if (map == nullptr || workspaceSession_.samples.selectedRegionIds.empty())
        return;
    const auto result = projectBridge_.editor().removeSampleRegions(
        map->mapId,
        workspaceSession_.samples.selectedRegionIds);
    if (!result.ok())
    {
        setStatus("Region removal failed: " + juceString(result.message));
        return;
    }
    workspaceSession_.samples.selectedRegionIds.clear();
    documentChanged();
    setStatus("Removed regions; project assets were retained");
}

void WorkbenchComponent::showSampleAsset(std::string assetId)
{
    jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto& document = projectBridge_.editor().document();
    const auto asset = std::ranges::find(
        document.assets,
        assetId,
        &talas::project::ProjectAsset::id);
    if (asset == document.assets.end())
    {
        setStatus("Show failed: selected asset no longer exists");
        return;
    }
    const auto path = talas::project::resolveProjectAssetPath(
        *asset,
        projectPath());
    std::error_code statusError;
    if (path.empty()
        || !std::filesystem::is_regular_file(path, statusError)
        || statusError)
    {
        setStatus("Show failed: selected asset path is unavailable");
        return;
    }
    juce::File{jucePathString(path)}.revealToUser();
    setStatus("Revealed selected sample asset");
}

void WorkbenchComponent::embedSampleAsset(std::string assetId)
{
    startSampleAssetDisposition(
        std::move(assetId),
        talas::project::ProjectAssetDisposition::Embedded);
}

void WorkbenchComponent::chooseReferencedSampleAsset(std::string assetId)
{
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }
    const auto& document = projectBridge_.editor().document();
    const auto asset = std::ranges::find(
        document.assets,
        assetId,
        &talas::project::ProjectAsset::id);
    if (asset == document.assets.end())
    {
        setStatus("Reference failed: selected asset no longer exists");
        return;
    }
    const auto resolved = talas::project::resolveProjectAssetPath(
        *asset,
        projectPath());
    auto initialLocation = juce::File::getSpecialLocation(
        juce::File::userDocumentsDirectory);
    if (projectPath().has_value())
    {
        const auto package = talas::project::resolveProjectPackagePath(
            *projectPath());
        if (package.ok() && package.packageDirectory.has_value())
        {
            initialLocation = juce::File{jucePathString(
                package.packageDirectory->parent_path())};
        }
        else if (!resolved.empty())
        {
            initialLocation = juce::File{jucePathString(resolved)};
        }
    }
    fileChooser_ = std::make_unique<juce::FileChooser>(
        "Choose Exact Durable PCM16 WAV Reference",
        initialLocation,
        "*.wav");
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
            if (!selected.existsAsFile())
                return;
            safeThis->startSampleAssetDisposition(
                std::move(assetId),
                talas::project::ProjectAssetDisposition::Referenced,
                filesystemPath(selected.getFullPathName()));
        });
}

void WorkbenchComponent::startSampleAssetDisposition(
    std::string assetId,
    talas::project::ProjectAssetDisposition disposition,
    std::optional<std::filesystem::path> selectedPath)
{
    const auto document = projectBridge_.editor().document();
    const auto asset = std::ranges::find(
        document.assets,
        assetId,
        &talas::project::ProjectAsset::id);
    if (asset == document.assets.end())
    {
        setStatus("Asset disposition failed: selected asset no longer exists");
        return;
    }
    stopSampleAssetDispositionWorker();
    const auto expectedAsset = *asset;
    const auto generation = ++sampleAssetDispositionGeneration_;
    sampleAssetDispositionAssetId_ = assetId;
    sampleAssetDispositionStatus_ = disposition
            == talas::project::ProjectAssetDisposition::Embedded
        ? "Embedding verified bytes..."
        : "Verifying durable reference...";
    sampleAssetDispositionStopRequested_.store(
        false,
        std::memory_order_release);
    sampleAssetDispositionWorkerFinished_.store(
        false,
        std::memory_order_release);

    talas::audio::ProjectAssetDispositionOptions options;
    options.assetId = std::move(assetId);
    options.targetDisposition = disposition;
    options.selectedPath = std::move(selectedPath);
    options.projectFilePath = projectPath();
    options.shouldCancel = [this, generation] {
        return sampleAssetDispositionStopRequested_.load(
                   std::memory_order_acquire)
            || generation != sampleAssetDispositionGeneration_.load(
                std::memory_order_acquire);
    };
    sampleAssetDispositionThread_ = std::thread(
        [this,
         generation,
         document,
         expectedAsset,
         options = std::move(options)]() mutable {
            talas::audio::ProjectAssetDispositionResult result;
            try
            {
                result = talas::audio::prepareProjectAssetDisposition(
                    document,
                    options);
            }
            catch (const std::bad_alloc&)
            {
                result.error = talas::audio::ProjectAssetDispositionError{
                    talas::audio::ProjectAssetDispositionErrorCode::InspectionFailed,
                    "Asset disposition preparation ran out of memory",
                    {},
                };
            }
            catch (const std::exception& error)
            {
                result.error = talas::audio::ProjectAssetDispositionError{
                    talas::audio::ProjectAssetDispositionErrorCode::InspectionFailed,
                    "Asset disposition preparation failed: "
                        + std::string{error.what()},
                    {},
                };
            }
            catch (...)
            {
                result.error = talas::audio::ProjectAssetDispositionError{
                    talas::audio::ProjectAssetDispositionErrorCode::InspectionFailed,
                    "Asset disposition preparation failed unexpectedly",
                    {},
                };
            }
            {
                std::lock_guard lock(sampleAssetDispositionResultMutex_);
                sampleAssetDispositionPublication_ =
                    SampleAssetDispositionPublication{
                        generation,
                        expectedAsset,
                        std::move(result),
                    };
            }
            sampleAssetDispositionWorkerFinished_.store(
                true,
                std::memory_order_release);
        });
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::finishSampleAssetDispositionIfReady()
{
    if (!sampleAssetDispositionWorkerFinished_.load(
            std::memory_order_acquire))
    {
        return;
    }
    if (sampleAssetDispositionThread_.joinable())
        sampleAssetDispositionThread_.join();
    std::optional<SampleAssetDispositionPublication> publication;
    {
        std::lock_guard lock(sampleAssetDispositionResultMutex_);
        publication = std::move(sampleAssetDispositionPublication_);
        sampleAssetDispositionPublication_.reset();
    }
    if (!publication.has_value())
        return;
    auto& result = publication->result;
    if (publication->generation
        != sampleAssetDispositionGeneration_.load(
            std::memory_order_acquire))
    {
        (void)discardOrRetainSampleAssetDispositionCleanup(
            result.candidate);
        return;
    }
    if (!result.ok())
    {
        const auto cleanup =
            discardOrRetainSampleAssetDispositionCleanup(
                result.candidate);
        sampleAssetDispositionStatus_ = result.cancelled()
            ? "Asset disposition change cancelled"
            : "Asset disposition failed: "
                + juceString(result.error->message);
        if (!cleanup.empty())
            sampleAssetDispositionStatus_ += juceString(" · cleanup: ")
                + juceString(cleanup);
        refreshSampleMapWorkspace();
        return;
    }
    const auto edited =
        projectBridge_.editor().acceptProjectAssetDisposition(
            result.candidate.asset,
            publication->expectedAsset);
    if (!edited.ok())
    {
        const auto cleanup =
            discardOrRetainSampleAssetDispositionCleanup(
                result.candidate);
        sampleAssetDispositionStatus_ = "Asset disposition was not published: "
            + juceString(edited.message);
        if (!cleanup.empty())
            sampleAssetDispositionStatus_ += juceString(" · cleanup: ")
                + juceString(cleanup);
        refreshSampleMapWorkspace();
        return;
    }
    sampleAssetDispositionStatus_ =
        result.candidate.asset.disposition
                == talas::project::ProjectAssetDisposition::Embedded
            ? "Embedded verified bytes in the project package"
            : "Using the selected durable external reference";
    documentChanged();
    setStatus(
        sampleAssetDispositionStatus_ + juceString(" · one undoable edit"));
}

void WorkbenchComponent::stopSampleAssetDispositionWorker()
{
    ++sampleAssetDispositionGeneration_;
    sampleAssetDispositionStopRequested_.store(
        true,
        std::memory_order_release);
    if (sampleAssetDispositionThread_.joinable())
        sampleAssetDispositionThread_.join();
    sampleAssetDispositionWorkerFinished_.store(
        true,
        std::memory_order_release);
    std::lock_guard lock(sampleAssetDispositionResultMutex_);
    if (sampleAssetDispositionPublication_.has_value())
    {
        (void)discardOrRetainSampleAssetDispositionCleanup(
            sampleAssetDispositionPublication_->result.candidate);
        sampleAssetDispositionPublication_.reset();
    }
}

std::string WorkbenchComponent::
discardOrRetainSampleAssetDispositionCleanup(
    talas::audio::PreparedProjectAssetDisposition& candidate)
{
    auto cleanup =
        talas::audio::discardPreparedProjectAssetDispositionFiles(candidate);
    auto unresolvedFiles = std::move(candidate.createdFiles);
    candidate.createdFiles.clear();
    for (const auto& path : unresolvedFiles)
    {
        const auto fingerprint = retainOwnedCleanupFile(
            path,
            candidate.asset.contentHash);
        if (!fingerprint.empty())
        {
            candidate.createdFiles.push_back(path);
            cleanup += (cleanup.empty() ? std::string{} : "; ")
                + fingerprint;
        }
    }
    if (!candidate.createdFiles.empty()
        || !candidate.cleanupImports.empty())
    {
        pendingSampleAssetDispositionCleanup_.push_back(
            std::move(candidate));
    }
    const auto ledger = persistPendingCleanupLedger();
    return ledger.empty()
        ? cleanup
        : cleanup + (cleanup.empty() ? std::string{} : "; ")
            + "cleanup ledger: " + ledger;
}

void WorkbenchComponent::chooseSampleMapImportFiles()
{
    chooseSampleMapFiles(false);
}

void WorkbenchComponent::chooseSampleMapReplaceFiles()
{
    chooseSampleMapFiles(true);
}

void WorkbenchComponent::chooseSampleMapFiles(bool replace)
{
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }
    if (!workspaceSession_.samples.activeMapId.has_value())
    {
        setStatus("Create or select a sample map before importing");
        return;
    }
    if (sampleImportEmbed_ && !projectPath().has_value())
    {
        setStatus("Save the project before importing embedded samples");
        return;
    }
    const auto initialLocation = projectPath().has_value()
        ? juce::File(jucePathString(projectPath()->parent_path()))
        : juce::File::getSpecialLocation(
              juce::File::userDocumentsDirectory);
    fileChooser_ = std::make_unique<juce::FileChooser>(
        replace
            ? "Replace Sample Map with PCM16 WAV Samples"
            : "Import PCM16 WAV Samples",
        initialLocation,
        "*.wav");
    fileChooserActive_ = true;
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::canSelectMultipleItems,
        [safeThis, replace](const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            std::vector<std::filesystem::path> paths;
            for (const auto& file : chooser.getResults())
                paths.push_back(filesystemPath(file.getFullPathName()));
            if (!paths.empty())
                safeThis->startSampleMapImport(
                    std::move(paths),
                    replace);
        });
}

void WorkbenchComponent::startSampleMapImport(
    std::vector<std::filesystem::path> sourcePaths,
    bool replace)
{
    if (!workspaceSession_.samples.activeMapId.has_value()
        || sourcePaths.empty())
    {
        return;
    }
    stopSampleMapImportWorker();
    sampleImportStopRequested_.store(false, std::memory_order_release);
    sampleImportWorkerFinished_.store(false, std::memory_order_release);
    sampleImportProgress_.store(0.0, std::memory_order_relaxed);
    sampleImportPresentation_ = SampleMapImportPresentation::Running;
    sampleImportStatus_.clear();
    sampleImportPreviewProjection_ = {};
    sampleImportLowNote_ = 0;
    sampleImportAnchorRootNote_ = 60;
    sampleImportHighNote_ = 127;
    const auto generation = ++sampleImportGeneration_;
    const auto mapId = *workspaceSession_.samples.activeMapId;
    auto replaceConfirmation = replace
        ? projectBridge_.editor().sampleMapReplaceConfirmation(mapId)
        : std::optional<talas::project::ProjectSampleMapReplaceConfirmation>{};
    if (replace && !replaceConfirmation.has_value())
    {
        sampleImportWorkerFinished_.store(true, std::memory_order_release);
        sampleImportPresentation_ = SampleMapImportPresentation::Failed;
        sampleImportStatus_ = "Replace confirmation could not bind to the active map revision";
        refreshSampleMapWorkspace();
        return;
    }
    const auto document = projectBridge_.editor().document();
    talas::audio::SampleMapImportOptions options;
    options.sourcePaths = std::move(sourcePaths);
    options.projectFilePath = projectPath();
    options.disposition = sampleImportEmbed_
        ? talas::audio::SampleMapImportDisposition::Embed
        : talas::audio::SampleMapImportDisposition::Reference;
    options.deferAutoMapRangeValidation = true;
    options.shouldCancel = [this, generation] {
        return sampleImportStopRequested_.load(std::memory_order_acquire)
            || generation != sampleImportGeneration_.load(
                std::memory_order_acquire);
    };
    options.reportProgress = [this, generation](double progress) {
        if (generation == sampleImportGeneration_.load(
                              std::memory_order_acquire))
        {
            sampleImportProgress_.store(
                std::clamp(progress, 0.0, 1.0),
                std::memory_order_relaxed);
        }
    };
    sampleImportThread_ = std::thread(
        [this,
         generation,
         mapId,
         replace,
         replaceConfirmation = std::move(replaceConfirmation),
         document,
         options = std::move(options)]() mutable {
            auto result = talas::audio::prepareSampleMapImport(
                document,
                options);
            {
                std::lock_guard lock(sampleImportResultMutex_);
                sampleImportPublication_ = SampleImportPublication{
                    generation,
                    mapId,
                    replace,
                    std::move(replaceConfirmation),
                    std::move(result),
                };
            }
            sampleImportWorkerFinished_.store(
                true,
                std::memory_order_release);
        });
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::cancelSampleMapImport()
{
    if (sampleImportPresentation_ == SampleMapImportPresentation::Preview
        && sampleImportPreview_.has_value())
    {
        auto publication = std::move(*sampleImportPreview_);
        sampleImportPreview_.reset();
        const auto cleanup = discardOrRetainSampleMapImportCleanup(
            publication.result.candidate);
        sampleImportPreviewProjection_ = {};
        sampleImportPresentation_ = cleanup.empty()
            ? SampleMapImportPresentation::Cancelled
            : SampleMapImportPresentation::Failed;
        sampleImportStatus_ = cleanup.empty()
            ? juce::String{"Import preview cancelled; project was not changed"}
            : "Import preview cleanup failed: " + juceString(cleanup);
        refreshSampleMapWorkspace();
        return;
    }
    if (sampleImportWorkerFinished_.load(std::memory_order_acquire))
        return;
    sampleImportStopRequested_.store(true, std::memory_order_release);
    sampleImportStatus_ = "Cancelling import...";
    refreshSampleMapWorkspace();
}

bool WorkbenchComponent::sampleMapImportOwnsProjectBoundary()
{
    if (!sampleImportWorkerFinished_.load(
            std::memory_order_acquire)
        || sampleImportPresentation_
            == SampleMapImportPresentation::Running
        || sampleImportPreview_.has_value())
    {
        return true;
    }
    std::lock_guard lock(sampleImportResultMutex_);
    return sampleImportPublication_.has_value();
}

void WorkbenchComponent::finishSampleMapImportIfReady()
{
    if (!sampleImportWorkerFinished_.load(std::memory_order_acquire))
        return;
    if (sampleImportThread_.joinable())
        sampleImportThread_.join();
    std::optional<SampleImportPublication> publication;
    {
        std::lock_guard lock(sampleImportResultMutex_);
        publication = std::move(sampleImportPublication_);
        sampleImportPublication_.reset();
    }
    if (!publication.has_value())
        return;
    if (publication->generation
        != sampleImportGeneration_.load(std::memory_order_acquire))
    {
        (void)discardOrRetainSampleMapImportCleanup(
            publication->result.candidate);
        return;
    }
    auto& result = publication->result;
    if (result.cancelled())
    {
        const auto cleanup = discardOrRetainSampleMapImportCleanup(
            result.candidate);
        sampleImportPresentation_ = SampleMapImportPresentation::Cancelled;
        sampleImportStatus_ = "Import cancelled; project was not changed";
        if (!cleanup.empty())
        {
            sampleImportPresentation_ = SampleMapImportPresentation::Failed;
            sampleImportStatus_ += juceString(" · cleanup: ")
                + juceString(cleanup);
        }
        refreshSampleMapWorkspace();
        return;
    }
    if (!result.ok())
    {
        const auto cleanup =
            discardOrRetainSampleMapImportCleanup(result.candidate);
        sampleImportPresentation_ = SampleMapImportPresentation::Failed;
        sampleImportStatus_ = "Import failed: "
            + juceString(result.error->message);
        if (!cleanup.empty())
            sampleImportStatus_ += juceString(" · cleanup: ")
                + juceString(cleanup);
        refreshSampleMapWorkspace();
        return;
    }
    sampleImportPreview_ = std::move(publication);
    sampleImportProgress_.store(1.0, std::memory_order_relaxed);
    updateSampleMapImportPreview(
        sampleImportLowNote_,
        sampleImportAnchorRootNote_,
        sampleImportHighNote_);
}

void WorkbenchComponent::updateSampleMapImportPreview(
    std::uint8_t lowNote,
    std::uint8_t anchorRootNote,
    std::uint8_t highNote)
{
    if (!sampleImportPreview_.has_value())
        return;
    sampleImportLowNote_ = lowNote;
    sampleImportAnchorRootNote_ = anchorRootNote;
    sampleImportHighNote_ = highNote;
    talas::project::ProjectSampleMapAutoMapRequest request;
    request.lowNote = lowNote;
    request.highNote = highNote;
    request.anchorRootNote = anchorRootNote;
    request.candidates =
        sampleImportPreview_->result.candidate.autoMapCandidates;
    constexpr talas::app::SampleMapCanvas projectionCanvas{1000.0, 400.0};
    const auto viewport = talas::app::sampleMapViewportFromSession(
        workspaceSession_.samples);
    sampleImportPreviewProjection_ = talas::app::makeSampleMapAutoMapPreview(
        request,
        viewport,
        projectionCanvas);
    sampleImportPresentation_ = SampleMapImportPresentation::Preview;
    if (sampleImportPreviewProjection_.ok())
    {
        auto& candidate = sampleImportPreview_->result.candidate;
        candidate.regions.clear();
        candidate.regions.reserve(sampleImportPreviewProjection_.regions.size());
        for (const auto& preview : sampleImportPreviewProjection_.regions)
            candidate.regions.push_back(preview.region);
        sampleImportStatus_ = "Previewing "
            + juce::String(static_cast<int>(candidate.regions.size()))
            + (candidate.regions.size() == 1
                   ? juceString(" region · Apply publishes one edit")
                   : juceString(" regions · Apply publishes one edit"));
    }
    else
    {
        const auto& error = *sampleImportPreviewProjection_.error;
        sampleImportStatus_ = "Preview unavailable: "
            + juceString(
                error.path.empty()
                    ? error.message
                    : error.path + ": " + error.message);
    }
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::applySampleMapImportPreview()
{
    if (!sampleImportPreview_.has_value()
        || sampleImportPresentation_ != SampleMapImportPresentation::Preview
        || !sampleImportPreviewProjection_.ok())
    {
        return;
    }
    auto publication = std::move(*sampleImportPreview_);
    sampleImportPreview_.reset();
    auto& result = publication.result;
    talas::project::ProjectSampleMapImportBatch batch{
        result.candidate.assets,
        result.candidate.expectedAssets,
        result.candidate.regions,
    };
    const auto edited = publication.replace
        && publication.replaceConfirmation.has_value()
        ? projectBridge_.editor().replaceSampleMapImport(
              publication.mapId,
              std::move(batch),
              *publication.replaceConfirmation)
        : projectBridge_.editor().mergeSampleMapImport(
              publication.mapId,
              std::move(batch));
    if (!edited.ok())
    {
        const auto cleanup =
            discardOrRetainSampleMapImportCleanup(result.candidate);
        sampleImportPresentation_ = SampleMapImportPresentation::Failed;
        sampleImportStatus_ = "Import was not published: "
            + juceString(edited.message);
        if (!cleanup.empty())
            sampleImportStatus_ += juceString(" · cleanup: ")
                + juceString(cleanup);
        refreshSampleMapWorkspace();
        return;
    }
    sampleImportPreviewProjection_ = {};
    sampleImportPresentation_ = SampleMapImportPresentation::Idle;
    sampleImportProgress_.store(1.0, std::memory_order_relaxed);
    workspaceSession_.samples.selectedRegionIds.clear();
    for (const auto& region : result.candidate.regions)
        workspaceSession_.samples.selectedRegionIds.push_back(region.regionId);
    documentChanged();
    setStatus(
        juce::String(publication.replace ? "Replaced with " : "Imported ")
        + juce::String(static_cast<int>(result.candidate.regions.size()))
        + " verified sample regions as one undoable edit");
}

void WorkbenchComponent::stopSampleMapImportWorker()
{
    ++sampleImportGeneration_;
    sampleImportStopRequested_.store(true, std::memory_order_release);
    if (sampleImportThread_.joinable())
        sampleImportThread_.join();
    sampleImportWorkerFinished_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(sampleImportResultMutex_);
        if (sampleImportPublication_.has_value())
        {
            (void)discardOrRetainSampleMapImportCleanup(
                sampleImportPublication_->result.candidate);
            sampleImportPublication_.reset();
        }
    }
    if (sampleImportPreview_.has_value())
    {
        (void)discardOrRetainSampleMapImportCleanup(
            sampleImportPreview_->result.candidate);
        sampleImportPreview_.reset();
    }
    sampleImportPreviewProjection_ = {};
}

std::optional<std::string>
WorkbenchComponent::runSampleMapImportPreviewSmoke(
    const std::filesystem::path& sampleOne,
    const std::filesystem::path& sampleTwo,
    const std::function<std::optional<std::string>()>& waitForImport,
    const std::function<std::optional<std::string>(std::string_view)>& snapshot)
{
    const auto require = [](bool condition, std::string message)
        -> std::optional<std::string> {
        return condition ? std::optional<std::string>{}
                         : std::optional<std::string>{std::move(message)};
    };
    const auto activeMap = [this]()
        -> const talas::project::ProjectSampleMap* {
        return workspaceSession_.samples.activeMapId.has_value()
            ? talas::project::findProjectSampleMap(
                  projectBridge_.editor().document(),
                  *workspaceSession_.samples.activeMapId)
            : nullptr;
    };

    startSampleMapImport({sampleOne, sampleTwo});
    if (auto error = waitForImport())
        return error;
    const auto* importedMap = activeMap();
    if (auto error = require(
            sampleImportPresentation_
                    == SampleMapImportPresentation::Preview
                && importedMap != nullptr
                && importedMap->regions.empty()
                && sampleImportPreviewProjection_.regions.size() == 2
                && sampleMapPanel_->hasImportPreviewControlsForSmoke()
                && sampleMapPanel_->hasAccessibleImportPreviewForSmoke(),
            "Successful inspection did not stop at an accessible zero-mutation preview"))
    {
        return error;
    }
    juce::ApplicationCommandInfo saveAsInfo(commandSaveProjectAs);
    getCommandInfo(commandSaveProjectAs, saveAsInfo);
    const auto projectPathBeforeBlockedSaveAs = projectPath();
    const auto documentBeforeBlockedSaveAs =
        talas::project::serializeProjectDocument(
            projectBridge_.editor().document());
    const auto previewAssetsBeforeBlockedSaveAs =
        sampleImportPreview_->result.candidate.assets;
    const auto previewRegionsBeforeBlockedSaveAs =
        sampleImportPreview_->result.candidate.regions;
    const auto previewFilesBeforeBlockedSaveAs =
        sampleImportPreview_->result.candidate.createdFiles;
    auto blockedSaveAsCompleted = false;
    auto blockedSaveAsResult = true;
    saveProjectAs([&](bool saved) {
        blockedSaveAsCompleted = true;
        blockedSaveAsResult = saved;
    });
    if (auto error = require(
            (saveAsInfo.flags
             & juce::ApplicationCommandInfo::isDisabled)
                    != 0
                && blockedSaveAsCompleted
                && !blockedSaveAsResult
                && !fileChooserActive_
                && projectPath() == projectPathBeforeBlockedSaveAs
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == documentBeforeBlockedSaveAs
                && sampleImportPreview_.has_value()
                && sampleImportPreview_->result.candidate.assets
                    == previewAssetsBeforeBlockedSaveAs
                && sampleImportPreview_->result.candidate.regions
                    == previewRegionsBeforeBlockedSaveAs
                && sampleImportPreview_->result.candidate.createdFiles
                    == previewFilesBeforeBlockedSaveAs
                && statusLabel_.getText().contains("Apply or Cancel"),
            "Save As did not fail closed while preserving the active import preview"))
    {
        return error;
    }
    if (auto error = require(
            sampleMapPanel_->setImportPreviewRangeForSmoke(48, 60, 72)
                && sampleImportPreviewProjection_.ok()
                && sampleImportPreviewProjection_.regions.size() == 2
                && sampleImportPreviewProjection_.regions[0].region.lowNote
                    == 48
                && sampleImportPreviewProjection_.regions[0].region.rootNote
                    == 60
                && sampleImportPreviewProjection_.regions[0].region.highNote
                    == 60
                && sampleImportPreviewProjection_.regions[1].region.lowNote
                    == 61
                && sampleImportPreviewProjection_.regions[1].region.highNote
                    == 72,
            "User-visible range and anchor controls did not recompute the exact deterministic preview"))
    {
        return error;
    }
    if (auto error = snapshot("workbench-samples-import-preview.png"))
        return error;
    setSize(960, 640);
    compactDock_ = CompactDock::RightWorkspace;
    resized();
    if (auto error = require(
            sampleMapPanel_->hasImportPreviewControlsForSmoke(),
            "Compact Samples layout lost the import preview controls"))
    {
        return error;
    }
    if (auto error = snapshot(
            "workbench-samples-import-preview-compact.png"))
    {
        return error;
    }
    setSize(1180, 700);
    compactDock_ = CompactDock::None;
    resized();
    if (auto error = require(
            sampleMapPanel_->setImportPreviewRangeForSmoke(72, 60, 48)
                && !sampleImportPreviewProjection_.ok()
                && !sampleMapPanel_->applyImportPreviewForSmoke()
                && importedMap->regions.empty(),
            "Invalid preview range did not reject Apply without project mutation"))
    {
        return error;
    }
    if (auto error = require(
            sampleMapPanel_->setImportPreviewRangeForSmoke(48, 60, 72)
                && sampleMapPanel_->applyImportPreviewForSmoke(),
            "Valid sample import preview did not expose Apply"))
    {
        return error;
    }
    importedMap = activeMap();
    if (auto error = require(
            sampleImportPresentation_ == SampleMapImportPresentation::Idle
                && importedMap != nullptr
                && importedMap->regions.size() == 2
                && workspaceSession_.samples.selectedRegionIds.size() == 2,
            "Apply did not publish the exact two-region preview as one edit"))
    {
        return error;
    }

    const auto beforePreviewCancel = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    startSampleMapImport({sampleOne});
    if (auto error = waitForImport())
        return error;
    if (auto error = require(
            sampleMapPanel_->cancelImportPreviewForSmoke()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == beforePreviewCancel,
            "Cancelling an inspected preview changed the project"))
    {
        return error;
    }
    juce::ApplicationCommandInfo reenabledSaveAsInfo(
        commandSaveProjectAs);
    getCommandInfo(
        commandSaveProjectAs,
        reenabledSaveAsInfo);
    if (auto error = require(
            (reenabledSaveAsInfo.flags
             & juce::ApplicationCommandInfo::isDisabled)
                == 0,
            "Save As did not reactivate after import preview cancellation"))
    {
        return error;
    }

    startSampleMapImport({sampleOne}, true);
    if (auto error = waitForImport())
        return error;
    importedMap = activeMap();
    if (auto error = require(
            sampleImportPresentation_
                    == SampleMapImportPresentation::Preview
                && importedMap != nullptr
                && importedMap->regions.size() == 2
                && sampleMapPanel_->applyImportPreviewForSmoke()
                && activeMap() != nullptr
                && activeMap()->regions.size() == 1,
            "Confirmed Samples replace did not preview before one complete replacement"))
    {
        return error;
    }
    undoProject();
    importedMap = activeMap();
    return require(
        importedMap != nullptr && importedMap->regions.size() == 2,
        "One Undo did not restore the complete pre-replace sample map");
}

std::optional<std::string>
WorkbenchComponent::runSampleMapAuditionLifecycleSmoke(
    std::string_view firstRegionId,
    const std::filesystem::path& outputTakePath)
{
    const auto require = [](bool condition, std::string message)
        -> std::optional<std::string> {
        return condition ? std::optional<std::string>{}
                         : std::optional<std::string>{std::move(message)};
    };
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    const auto activeMap = [this]() {
        return activeSampleMap(
            projectBridge_.editor().document(),
            workspaceSession_.samples);
    };
    const auto settleRestore = [&]() -> std::optional<std::string> {
        left.fill(0.0F);
        right.fill(0.0F);
        (void)projectBridge_.render(
            channels.data(),
            channels.size(),
            left.size());
        finishSampleMapAuditionRestoreIfReady();
        return require(
            currentDocumentStaged_
                && !sampleMapAuditionActive_
                && sampleMapAuditionPresentation_
                    == SampleMapAuditionPresentation::Idle,
            "Samples audition boundary did not restore the current project graph · staged "
                + std::to_string(currentDocumentStaged_)
                + " · state "
                + std::to_string(static_cast<int>(
                    sampleMapAuditionPresentation_))
                + " · status " + statusLabel_.getText().toStdString());
    };
    const auto startAudition = [&](std::string_view regionId,
                                   std::uint8_t note)
        -> std::optional<std::string> {
        selectSampleRegion(
            std::string(regionId),
            talas::app::SampleMapSelectionMode::Replace);
        const auto before = talas::project::serializeProjectDocument(
            projectBridge_.editor().document());
        handleSampleMapMidiNoteOn(1, note, 96, 0.0);
        if (auto error = require(
                sampleMapAuditionPresentation_
                    == SampleMapAuditionPresentation::Preparing,
                "Idle Samples MIDI note-on did not start isolated audition preparation"))
        {
            return error;
        }
        const auto deadline =
            juce::Time::getMillisecondCounterHiRes() + 10000.0;
        while (!sampleMapAuditionWorkerFinished_.load(
                   std::memory_order_acquire)
               && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            std::this_thread::yield();
        }
        if (auto error = require(
                sampleMapAuditionWorkerFinished_.load(
                    std::memory_order_acquire),
                "Samples audition preparation exceeded its bounded smoke deadline"))
        {
            return error;
        }
        finishSampleMapAuditionIfReady();
        left.fill(0.0F);
        right.fill(0.0F);
        const auto rendered = projectBridge_.render(
            channels.data(),
            channels.size(),
            left.size());
        const auto peak = std::ranges::max(
            left,
            {},
            [](float sample) { return std::abs(sample); });
        const auto documentUnchanged =
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document())
            == before;
        const auto clock = projectBridge_.midiClockTelemetry();
        const auto stream = projectBridge_.streamHealth();
        return require(
            sampleMapAuditionActive_
                && sampleMapAuditionPresentation_
                    == SampleMapAuditionPresentation::Active
                && rendered
                && std::abs(peak) > 1.0e-4F
                && documentUnchanged,
            "MIDI note-on did not activate an audible isolated immutable graph · active "
                + std::to_string(sampleMapAuditionActive_)
                + " · state "
                + std::to_string(static_cast<int>(
                    sampleMapAuditionPresentation_))
                + " · rendered " + std::to_string(rendered)
                + " · peak " + std::to_string(std::abs(peak))
                + " · immutable " + std::to_string(documentUnchanged)
                + " · clock authority "
                + std::to_string(clock.authorityGeneration)
                + " · clock external "
                + std::to_string(clock.externalAuthority)
                + " · clock running "
                + std::to_string(clock.running)
                + " · transport frame "
                + std::to_string(projectBridge_.transportFramePosition())
                + " · stream stage "
                + std::to_string(stream.stageId)
                + " · stream consumers "
                + std::to_string(stream.consumerCount)
                + " · stream missing "
                + std::to_string(stream.totals.missingFrames)
                + " · status " + statusLabel_.getText().toStdString());
    };

    const auto* map = activeMap();
    const auto* region = map != nullptr
        ? talas::project::findProjectSampleRegion(*map, firstRegionId)
        : nullptr;
    if (auto error = require(
            map != nullptr && region != nullptr,
            "Samples audition smoke lost its selected source region"))
    {
        return error;
    }
    const auto originalMapId = map->mapId;
    const auto auditionNote = region->rootNote;

    selectSampleRegion(
        std::string(firstRegionId),
        talas::app::SampleMapSelectionMode::Replace);
    auditionActiveSampleMap();
    if (auto error = require(
            sampleMapAuditionPresentation_
                == SampleMapAuditionPresentation::Preparing,
            "Samples audition did not publish its generation-tagged preparing state"))
    {
        return error;
    }
    stopSampleMapAudition();
    if (auto error = require(
            !sampleMapAuditionActive_
                && sampleMapAuditionPresentation_
                    == SampleMapAuditionPresentation::Idle,
            "Samples audition stop did not cancel its pending generation cleanly"))
    {
        return error;
    }

    const auto preparedBeforeClockRace =
        projectBridge_.preparedGraphStatus({}).prepared;
    const auto preparedStageBeforeClockRace =
        preparedBeforeClockRace != nullptr
        ? preparedBeforeClockRace->stageId
        : 0;
    const auto originalDeviceSettings =
        projectBridge_.editor().document().deviceSettings;
    if (auto error = require(
            midiInputSources_.empty()
                && !projectBridge_.midiClockTelemetry()
                        .externalAuthority,
            "Clock/audition smoke fixture requires an Internal transport with no live MIDI source"))
    {
        return error;
    }
    sampleMapAuditionSmokeHoldNextWorker_ = true;
    beginSampleMapAudition(
        {1, auditionNote, 96, 0.0, false, true});
    const auto pausedDeadline =
        juce::Time::getMillisecondCounterHiRes() + 10000.0;
    while (!sampleMapAuditionSmokeWorkerPaused_.load(
               std::memory_order_acquire)
           && juce::Time::getMillisecondCounterHiRes()
               < pausedDeadline)
    {
        std::this_thread::yield();
    }
    if (!sampleMapAuditionSmokeWorkerPaused_.load(
            std::memory_order_acquire))
    {
        stopSampleMapAudition();
        return "Clock/audition smoke worker did not reach its bounded publication barrier";
    }

    MidiInputSourceRecord clockSource;
    clockSource.identifier = "smoke-audition-clock";
    clockSource.displayName = "Smoke Audition Clock";
    clockSource.slot = 1;
    clockSource.generation = midiInputSourceGeneration_.load(
        std::memory_order_acquire);
    midiInputSources_.push_back(std::move(clockSource));
    auto externalClockSettings = originalDeviceSettings;
    externalClockSettings.configured = true;
    externalClockSettings.sampleRate = 48'000.0;
    externalClockSettings.bufferSize = 256;
    externalClockSettings.outputChannels = {1, 2};
    externalClockSettings.midiInputDeviceIdentifiers.push_back(
        "smoke-audition-clock");
    std::ranges::sort(
        externalClockSettings.midiInputDeviceIdentifiers);
    const auto uniqueMidiInputs = std::ranges::unique(
        externalClockSettings.midiInputDeviceIdentifiers).begin();
    externalClockSettings.midiInputDeviceIdentifiers.erase(
        uniqueMidiInputs,
        externalClockSettings.midiInputDeviceIdentifiers.end());
    externalClockSettings.transportSync.sourceMode =
        talas::project::TransportSyncSourceMode::MidiClockExactInput;
    externalClockSettings.transportSync.sourceIdentifier =
        "smoke-audition-clock";
    const auto externalClockConfigured = projectBridge_.editor()
        .setDeviceSettings(externalClockSettings).ok();
    if (externalClockConfigured)
        rebuildTransportSyncAuthority();
    sampleMapAuditionSmokeHoldWorker_.store(
        false,
        std::memory_order_release);
    sampleMapAuditionSmokeHoldWorker_.notify_all();

    const auto finishedDeadline =
        juce::Time::getMillisecondCounterHiRes() + 10000.0;
    while (!sampleMapAuditionWorkerFinished_.load(
               std::memory_order_acquire)
           && juce::Time::getMillisecondCounterHiRes()
               < finishedDeadline)
    {
        std::this_thread::yield();
    }
    if (sampleMapAuditionWorkerFinished_.load(
            std::memory_order_acquire))
    {
        finishSampleMapAuditionIfReady();
    }
    const auto preparedAfterClockRace =
        projectBridge_.preparedGraphStatus({}).prepared;
    const auto preparedStageAfterClockRace =
        preparedAfterClockRace != nullptr
        ? preparedAfterClockRace->stageId
        : 0;
    const auto clockRaceRejected = externalClockConfigured
        && sampleMapAuditionWorkerFinished_.load(
            std::memory_order_acquire)
        && projectBridge_.midiClockTelemetry().externalAuthority
        && preparedStageAfterClockRace
            == preparedStageBeforeClockRace
        && !sampleMapAuditionActive_
        && sampleMapAuditionPresentation_
            == SampleMapAuditionPresentation::Failed
        && !sampleMapAuditionPendingVoice_.has_value()
        && !sampleMapAuditionRestoreSettings_.has_value()
        && sampleMapAuditionRestoreDeviceName_.isEmpty();
    const auto clockRestored = projectBridge_.editor()
        .setDeviceSettings(originalDeviceSettings).ok();
    midiInputSources_.clear();
    rebuildTransportSyncAuthority();
    if (auto error = require(
            clockRaceRejected && clockRestored,
            "A sample audition begun under Internal transport was published after exact Clock authority replaced it"))
    {
        return error;
    }

    std::string outputTakeError;
    const auto outputTakeStage = projectBridge_.activeStageId();
    auto outputTake = talas::audio::RealtimeOutputTake::start(
        {outputTakePath,
         talas::audio::ExistingAudioFilePolicy::Replace,
         8000.0,
         2,
         left.size(),
         4,
         outputTakeStage,
         analysisOutputTakeAuthorityGeneration_.load(
             std::memory_order_acquire),
         projectBridge_.editor().documentAuthorityGeneration()},
        outputTakeError);
    if (outputTake == nullptr)
    {
        return "Sample-map audition output-take fixture could not start: "
            + outputTakeError;
    }
    analysisOutputTakeLastSnapshot_.reset();
    analysisOutputTake_ = std::move(outputTake);
    analysisOutputTakeCallback_.store(
        analysisOutputTake_.get(), std::memory_order_release);
    analysisOutputTakeLifecycle_.store(1, std::memory_order_seq_cst);
    if (!analysisOutputTake_->publishFromAudio(
            channels.data(),
            channels.size(),
            left.size(),
            outputTakeStage,
            analysisOutputTakeAuthorityGeneration_.load(
                std::memory_order_acquire),
            projectBridge_.editor().documentAuthorityGeneration()))
    {
        return "Sample-map audition output-take fixture rejected its source stage";
    }
    if (auto error = startAudition(firstRegionId, auditionNote))
        return error;
    if (auto error = require(
            analysisOutputTake_ == nullptr
                && analysisOutputTakeLastSnapshot_.has_value()
                && analysisOutputTakeLastSnapshot_->state
                    == talas::audio::RealtimeOutputTakeState::Completed
                && std::filesystem::exists(outputTakePath),
            "Sample-map audition staging did not finalize the exact prior-stage output take"))
    {
        return error;
    }
    std::error_code outputTakeCleanupError;
    std::filesystem::remove(outputTakePath, outputTakeCleanupError);
    handleSampleMapMidiNoteOff(1, auditionNote, 0.0);
    left.fill(0.0F);
    right.fill(0.0F);
    const auto renderedRelease = projectBridge_.render(
        channels.data(),
        channels.size(),
        left.size());
    const auto releasePeak = std::ranges::max(
        left,
        {},
        [](float sample) { return std::abs(sample); });
    if (auto error = require(
            sampleMapAuditionActiveVoice_.has_value()
                && !sampleMapAuditionActiveVoice_->keyDown
                && renderedRelease
                && std::abs(releasePeak) < 1.0e-5F,
            "Matching Samples MIDI note-off did not release the audition gate"))
    {
        return error;
    }
    (void)perform(juce::ApplicationCommandTarget::InvocationInfo{commandStop});
    if (auto error = settleRestore())
        return "Global Stop: " + *error;

    if (auto error = startAudition(firstRegionId, auditionNote))
        return error;
    (void)perform(
        juce::ApplicationCommandTarget::InvocationInfo{commandToggleLibrary});
    if (auto error = settleRestore())
        return "Show Library: " + *error;
    if (auto error = require(
            workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch
                && !sampleMidiRoutingEnabled_.load(std::memory_order_acquire)
                && libraryVisible_,
            "Show Library did not leave Samples through the single workspace activation boundary"))
    {
        return error;
    }
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Samples);
    if (auto error = require(
            workspaceSession_.samples.activeMapId == originalMapId
                && sampleMapPanel_->hasReadySurfaceForSmoke(),
            "Workspace switching did not restore the local Samples selection"))
    {
        return error;
    }

    if (auto error = startAudition(firstRegionId, auditionNote))
        return error;
    const auto renameResult = projectBridge_.editor().renameSampleMap(
        originalMapId,
        "Audition Restore Smoke Map");
    if (auto error = require(
            renameResult.ok(),
            "Samples audition edit fixture failed: " + renameResult.message))
    {
        return error;
    }
    documentChanged();
    if (auto error = settleRestore())
        return "Document edit: " + *error;

    map = activeMap();
    region = map != nullptr
        ? talas::project::findProjectSampleRegion(*map, firstRegionId)
        : nullptr;
    if (auto error = require(
            map != nullptr && region != nullptr,
            "Samples audition map-switch fixture lost its source region"))
    {
        return error;
    }
    auto alternateRegion = *region;
    alternateRegion.regionId = "audition_boundary_region";
    auto alternateMapId = nextSampleMapId(projectBridge_.editor().document());
    auto alternateMap = talas::project::ProjectSampleMap{
        alternateMapId,
        "Audition Boundary Map",
        {alternateRegion},
    };
    const auto addResult = projectBridge_.editor().addSampleMap(
        std::move(alternateMap));
    if (auto error = require(
            addResult.ok(),
            "Samples audition map-switch fixture failed: " + addResult.message))
    {
        return error;
    }
    documentChanged();
    if (auto error = startAudition(firstRegionId, auditionNote))
        return error;
    selectSampleMap(alternateMapId);
    if (auto error = settleRestore())
        return "Sample-map switch: " + *error;
    if (auto error = require(
            workspaceSession_.samples.activeMapId == alternateMapId,
            "Sample-map switch did not publish the new active map after restoring audio"))
    {
        return error;
    }
    if (auto error = startAudition(
            alternateRegion.regionId,
            alternateRegion.rootNote))
    {
        return "Sample-map switch MIDI: " + *error;
    }
    if (auto error = require(
            workspaceSession_.samples.selectedRegionIds
                == std::vector<std::string>{alternateRegion.regionId},
            "MIDI after a map switch did not target the new map's region"))
    {
        return error;
    }
    stopSampleMapAudition();
    if (auto error = settleRestore())
        return error;
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    return {};
}

std::string WorkbenchComponent::discardOrRetainSampleMapImportCleanup(
    talas::audio::PreparedSampleMapImport& candidate)
{
    auto cleanup =
        talas::audio::discardPreparedSampleMapImportFiles(candidate);
    auto unresolvedFiles = std::move(candidate.createdFiles);
    candidate.createdFiles.clear();
    for (const auto& path : unresolvedFiles)
    {
        const auto packageDirectory = path.parent_path().parent_path();
        const auto relativePath = path.lexically_relative(packageDirectory);
        const auto asset = std::ranges::find_if(
            candidate.assets,
            [&relativePath](const auto& candidateAsset) {
                return talas::project::pathFromUtf8(candidateAsset.path)
                           .lexically_normal()
                    == relativePath;
            });
        const auto fingerprint = retainOwnedCleanupFile(
            path,
            asset == candidate.assets.end()
                ? std::string_view{}
                : std::string_view{asset->contentHash});
        if (!fingerprint.empty())
        {
            candidate.createdFiles.push_back(path);
            cleanup += (cleanup.empty() ? std::string{} : "; ")
                + fingerprint;
        }
    }
    if (!candidate.createdFiles.empty()
        || !candidate.cleanupImports.empty())
    {
        pendingSampleMapImportCleanup_.push_back(std::move(candidate));
    }
    const auto ledger = persistPendingCleanupLedger();
    return ledger.empty()
        ? cleanup
        : cleanup + (cleanup.empty() ? std::string{} : "; ")
            + "cleanup ledger: " + ledger;
}

std::string WorkbenchComponent::retainOwnedCleanupFile(
    const std::filesystem::path& path,
    std::string_view expectedContentHash)
{
    if (cleanupArtifactPathState(path, false)
        != CleanupArtifactPathState::Valid)
        return "refusing to fingerprint cleanup file outside a project package";
    const auto fileHash = talas::project::calculateProjectAssetSha256(path);
    if (!fileHash.ok())
        return "unable to fingerprint retained cleanup file";
    if (!expectedContentHash.empty()
        && fileHash.contentHash != expectedContentHash)
    {
        return "retained cleanup file no longer matches candidate content";
    }
    const auto projectHash = talas::project::calculateProjectAssetSha256(
        path.parent_path().parent_path()
            / talas::project::projectPackageDocumentFileName);
    if (!projectHash.ok())
        return "unable to fingerprint retained cleanup package project";
    pendingOwnedCleanupFiles_.push_back({
        path,
        fileHash.contentHash,
        projectHash.contentHash,
    });
    return {};
}

std::size_t WorkbenchComponent::pendingCleanupArtifactCount() const
{
    auto count = pendingProjectPackageImportCleanup_.size()
        + pendingOwnedCleanupFiles_.size();
    for (const auto& candidate : pendingSampleAssetDispositionCleanup_)
    {
        count += candidate.createdFiles.size()
            + candidate.cleanupImports.size();
    }
    for (const auto& candidate : pendingSampleMapImportCleanup_)
    {
        count += candidate.createdFiles.size()
            + candidate.cleanupImports.size();
    }
    return count;
}

std::string WorkbenchComponent::persistPendingCleanupLedger()
{
    struct Artifact
    {
        std::string kind;
        std::filesystem::path path;
        std::string contentHash;
        std::string packageProjectHash;
    };
    std::vector<Artifact> artifacts;
    std::unordered_set<std::string> identities;
    std::string validationError;
    const auto append = [&artifacts, &identities, &validationError](
                            std::string kind,
                            const std::filesystem::path& path,
                            std::string contentHash = {},
                            std::string packageProjectHash = {}) {
        const auto staging = kind == "import-staging";
        const auto pathState = cleanupArtifactPathState(path, staging);
        if (pathState == CleanupArtifactPathState::Missing)
            return;
        if (pathState == CleanupArtifactPathState::Invalid)
        {
            if (validationError.empty())
            {
                validationError =
                    "refusing invalid retained cleanup path: "
                    + talas::project::pathToUtf8(path);
            }
            return;
        }
        if (!staging
            && (!talas::project::isCanonicalProjectAssetContentHash(contentHash)
                || !talas::project::isCanonicalProjectAssetContentHash(
                    packageProjectHash)))
        {
            if (validationError.empty())
            {
                validationError =
                    "refusing cleanup file without immutable ownership fingerprints";
            }
            return;
        }
        const auto identity = kind + "\n"
            + talas::project::pathToUtf8(path);
        if (identities.insert(identity).second)
        {
            artifacts.push_back({
                std::move(kind),
                path,
                std::move(contentHash),
                std::move(packageProjectHash),
            });
        }
    };
    const auto appendImport = [&append](
                                  const talas::audio::ProjectPackageImportResult& imported) {
        if (!imported.retainedStagingPath.empty())
        {
            append(
                "import-staging",
                imported.retainedStagingPath,
                {},
                {});
        }
    };
    for (const auto& imported : pendingProjectPackageImportCleanup_)
        appendImport(imported);
    for (const auto& file : pendingOwnedCleanupFiles_)
    {
        append(
            "candidate-file",
            file.path,
            file.contentHash,
            file.packageProjectHash);
    }
    for (const auto& candidate : pendingSampleAssetDispositionCleanup_)
    {
        if (!candidate.createdFiles.empty() && validationError.empty())
            validationError = "cleanup file fingerprint is still pending";
        for (const auto& imported : candidate.cleanupImports)
            appendImport(imported);
    }
    for (const auto& candidate : pendingSampleMapImportCleanup_)
    {
        if (!candidate.createdFiles.empty() && validationError.empty())
            validationError = "cleanup file fingerprint is still pending";
        for (const auto& imported : candidate.cleanupImports)
            appendImport(imported);
    }
    if (!validationError.empty())
        return validationError;

    std::error_code error;
    const auto targetStatus =
        std::filesystem::symlink_status(cleanupLedgerPath_, error);
    if (error == std::errc::no_such_file_or_directory)
        error.clear();
    if (error)
    {
        return "unable to inspect cleanup ledger: " + error.message();
    }
    const auto targetExists = targetStatus.type()
        != std::filesystem::file_type::not_found;
    if (targetExists
        && (std::filesystem::is_symlink(targetStatus)
            || !std::filesystem::is_regular_file(targetStatus)))
    {
        return "cleanup ledger destination is not a regular owner file";
    }
    if (artifacts.empty())
    {
        if (targetExists
            && (!std::filesystem::remove(cleanupLedgerPath_, error)
                || error))
        {
            return "unable to remove completed cleanup ledger"
                + (error ? ": " + error.message() : std::string{});
        }
        return {};
    }

    const auto parent = cleanupLedgerPath_.parent_path();
    std::filesystem::create_directories(parent, error);
    if (error)
        return "unable to create cleanup ledger directory: " + error.message();
    const auto parentStatus =
        std::filesystem::symlink_status(parent, error);
    if (error || std::filesystem::is_symlink(parentStatus)
        || !std::filesystem::is_directory(parentStatus))
    {
        return "cleanup ledger parent is not a private owner directory";
    }
#if !defined(_WIN32)
    std::filesystem::permissions(
        parent,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        error);
    if (error)
        return "unable to secure cleanup ledger directory: " + error.message();
#endif

    juce::Array<juce::var> serializedArtifacts;
    for (const auto& artifact : artifacts)
    {
        auto* serialized = new juce::DynamicObject();
        serialized->setProperty("kind", juceString(artifact.kind));
        serialized->setProperty(
            "path",
            juceString(talas::project::pathToUtf8(artifact.path)));
        if (artifact.kind == "candidate-file")
        {
            serialized->setProperty(
                "contentHash",
                juceString(artifact.contentHash));
            serialized->setProperty(
                "packageProjectHash",
                juceString(artifact.packageProjectHash));
        }
        serializedArtifacts.add(juce::var(serialized));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("format", "TalasCleanupLedger");
    root->setProperty("version", 1);
    root->setProperty("artifacts", juce::var(serializedArtifacts));
    const auto serialized = juce::JSON::toString(
        juce::var(root),
        true).toStdString();

    auto temporary = cleanupLedgerPath_;
    temporary += ".tmp";
    const auto temporaryStatus =
        std::filesystem::symlink_status(temporary, error);
    if (error == std::errc::no_such_file_or_directory)
        error.clear();
    if (error)
        return "unable to inspect temporary cleanup ledger: " + error.message();
    if (temporaryStatus.type()
        != std::filesystem::file_type::not_found)
    {
        if (std::filesystem::is_symlink(temporaryStatus)
            || !std::filesystem::is_regular_file(temporaryStatus)
            || !std::filesystem::remove(temporary, error)
            || error)
        {
            return "unable to replace temporary cleanup ledger";
        }
    }
    {
        std::ofstream output(
            temporary,
            std::ios::binary | std::ios::trunc);
        if (!output.is_open())
            return "unable to create temporary cleanup ledger";
        output.write(
            serialized.data(),
            static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output.good())
            return "unable to write temporary cleanup ledger";
    }
#if !defined(_WIN32)
    std::filesystem::permissions(
        temporary,
        std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        error);
    if (error)
        return "unable to secure temporary cleanup ledger: " + error.message();
#endif

#if defined(_WIN32)
    auto backup = cleanupLedgerPath_;
    backup += ".bak";
    if (targetExists)
    {
        const auto backupStatus =
            std::filesystem::symlink_status(backup, error);
        if (error == std::errc::no_such_file_or_directory)
            error.clear();
        if (error
            || backupStatus.type()
                != std::filesystem::file_type::not_found)
        {
            return "cleanup ledger backup is unavailable";
        }
        std::filesystem::rename(cleanupLedgerPath_, backup, error);
        if (error)
            return "unable to preserve prior cleanup ledger: " + error.message();
    }
#endif
    std::filesystem::rename(temporary, cleanupLedgerPath_, error);
    if (error)
    {
#if defined(_WIN32)
        if (targetExists)
        {
            std::error_code restoreError;
            std::filesystem::rename(
                backup,
                cleanupLedgerPath_,
                restoreError);
        }
#endif
        return "unable to publish cleanup ledger: " + error.message();
    }
#if defined(_WIN32)
    if (targetExists)
    {
        std::error_code removeError;
        std::filesystem::remove(backup, removeError);
    }
#endif
    return {};
}

std::string WorkbenchComponent::loadPendingCleanupLedger()
{
    struct LoadedArtifact
    {
        bool staging = false;
        std::filesystem::path path;
        std::string contentHash;
        std::string packageProjectHash;
    };
    auto temporary = cleanupLedgerPath_;
    temporary += ".tmp";
    auto backup = cleanupLedgerPath_;
    backup += ".bak";

    std::error_code error;
    const auto pathExists = [&error](const std::filesystem::path& path) {
        error.clear();
        const auto status = std::filesystem::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory)
        {
            error.clear();
            return false;
        }
        return !error
            && status.type() != std::filesystem::file_type::not_found;
    };
    const auto primaryExists = pathExists(cleanupLedgerPath_);
    if (error)
        return "unable to inspect cleanup ledger: " + error.message();
    const auto backupExists = pathExists(backup);
    if (error)
        return "unable to inspect cleanup ledger backup: " + error.message();
    const auto temporaryExists = pathExists(temporary);
    if (error)
        return "unable to inspect temporary cleanup ledger: " + error.message();
    if (!primaryExists && !backupExists && !temporaryExists)
        return {};

    const auto parentStatus = std::filesystem::symlink_status(
        cleanupLedgerPath_.parent_path(),
        error);
    if (error == std::errc::no_such_file_or_directory)
        return {};
    if (error || std::filesystem::is_symlink(parentStatus)
        || !std::filesystem::is_directory(parentStatus))
    {
        return "cleanup ledger parent is not a private owner directory";
    }
#if !defined(_WIN32)
    if ((parentStatus.permissions()
                & (std::filesystem::perms::group_all
                   | std::filesystem::perms::others_all))
            != std::filesystem::perms::none)
    {
        return "cleanup ledger parent permissions are not owner-only";
    }
#endif

    const auto readLedger = [](const std::filesystem::path& source,
                               std::vector<LoadedArtifact>& loaded,
                               bool& consumedMissing)
        -> std::string {
        std::error_code readError;
        const auto status =
            std::filesystem::symlink_status(source, readError);
        if (readError || std::filesystem::is_symlink(status)
            || !std::filesystem::is_regular_file(status))
        {
            return "cleanup ledger recovery source is not a regular owner file";
        }
#if !defined(_WIN32)
        if ((status.permissions()
                & (std::filesystem::perms::group_all
                   | std::filesystem::perms::others_all))
            != std::filesystem::perms::none)
        {
            return "cleanup ledger recovery source is not owner-only";
        }
#endif
        constexpr std::uintmax_t maximumLedgerBytes = 1024 * 1024;
        const auto size = std::filesystem::file_size(source, readError);
        if (readError || size > maximumLedgerBytes)
            return "cleanup ledger is unreadable or exceeds its size limit";
        std::ifstream input(source, std::ios::binary);
        if (!input.is_open())
            return "unable to open cleanup ledger";
        const std::string serialized{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
        const auto parsed = juce::JSON::parse(juceString(serialized));
        const auto* root = parsed.getDynamicObject();
        if (root == nullptr
            || root->getProperty("format").toString()
                != "TalasCleanupLedger"
            || static_cast<int>(root->getProperty("version")) != 1)
        {
            return "cleanup ledger schema is invalid";
        }
        const auto* artifacts = root->getProperty("artifacts").getArray();
        if (artifacts == nullptr || artifacts->size() > 1024)
            return "cleanup ledger artifact list is invalid";
        std::unordered_set<std::string> identities;
        for (const auto& artifactValue : *artifacts)
        {
            const auto* artifact = artifactValue.getDynamicObject();
            if (artifact == nullptr)
                return "cleanup ledger artifact is invalid";
            const auto kind =
                artifact->getProperty("kind").toString().toStdString();
            if (kind != "import-staging" && kind != "candidate-file")
                return "cleanup ledger artifact kind is invalid";
            const auto path = talas::project::pathFromUtf8(
                artifact->getProperty("path").toString().toStdString());
            const auto staging = kind == "import-staging";
            const auto contentHash = staging
                ? std::string{}
                : artifact->getProperty("contentHash")
                      .toString().toStdString();
            const auto packageProjectHash = staging
                ? std::string{}
                : artifact->getProperty("packageProjectHash")
                      .toString().toStdString();
            const auto pathState = cleanupArtifactPathState(path, staging);
            if (pathState == CleanupArtifactPathState::Missing)
            {
                consumedMissing = true;
                continue;
            }
            if (pathState == CleanupArtifactPathState::Invalid
                || (!staging
                    && (!talas::project::isCanonicalProjectAssetContentHash(
                            contentHash)
                        || !talas::project::isCanonicalProjectAssetContentHash(
                            packageProjectHash))))
            {
                return "cleanup ledger contains an invalid artifact boundary";
            }
            const auto identity = kind + "\n"
                + talas::project::pathToUtf8(path);
            if (identities.insert(identity).second)
            {
                loaded.push_back({
                    staging,
                    path,
                    contentHash,
                    packageProjectHash,
                });
            }
        }
        return {};
    };

    const auto source = primaryExists
        ? cleanupLedgerPath_
        : backupExists ? backup : temporary;
    std::vector<LoadedArtifact> loaded;
    auto consumedMissing = false;
    if (const auto readError = readLedger(
            source,
            loaded,
            consumedMissing);
        !readError.empty())
    {
        return readError;
    }
    if (source != cleanupLedgerPath_)
    {
        std::filesystem::rename(source, cleanupLedgerPath_, error);
        if (error)
            return "unable to recover cleanup ledger: " + error.message();
    }
    const auto removeSidecar = [](const std::filesystem::path& path)
        -> std::string {
        std::error_code removeError;
        const auto status =
            std::filesystem::symlink_status(path, removeError);
        if (removeError == std::errc::no_such_file_or_directory
            || (!removeError
                && status.type()
                    == std::filesystem::file_type::not_found))
        {
            return {};
        }
        if (removeError || std::filesystem::is_symlink(status)
            || !std::filesystem::is_regular_file(status))
        {
            return "cleanup ledger sidecar is unsafe";
        }
        if (!std::filesystem::remove(path, removeError) || removeError)
            return "unable to remove recovered cleanup ledger sidecar";
        return {};
    };
    if (source != backup)
    {
        if (const auto sidecarError = removeSidecar(backup);
            !sidecarError.empty())
        {
            return sidecarError;
        }
    }
    if (source != temporary)
    {
        if (const auto sidecarError = removeSidecar(temporary);
            !sidecarError.empty())
        {
            return sidecarError;
        }
    }
    for (const auto& artifact : loaded)
    {
        if (artifact.staging)
        {
            pendingProjectPackageImportCleanup_.push_back({
                {},
                "Recovered retained package-import staging",
                false,
                talas::audio::ProjectPackageImportErrorCode::CleanupFailed,
                artifact.path,
            });
        }
        else
        {
            pendingOwnedCleanupFiles_.push_back({
                artifact.path,
                artifact.contentHash,
                artifact.packageProjectHash,
            });
        }
    }
    if (consumedMissing)
    {
        if (const auto rewriteError = persistPendingCleanupLedger();
            !rewriteError.empty())
        {
            return "unable to consume missing cleanup artifacts: "
                + rewriteError;
        }
    }
    return {};
}

void WorkbenchComponent::retryPendingSampleCleanup()
{
    const auto before = pendingCleanupArtifactCount();
    std::vector<talas::audio::ProjectPackageImportResult>
        remainingPackageImports;
    remainingPackageImports.reserve(
        pendingProjectPackageImportCleanup_.size());
    for (auto& imported : pendingProjectPackageImportCleanup_)
    {
        (void)talas::audio::discardProjectPackageImportArtifacts(imported);
        if (!imported.retainedStagingPath.empty())
            remainingPackageImports.push_back(std::move(imported));
    }
    pendingProjectPackageImportCleanup_ =
        std::move(remainingPackageImports);

    std::vector<PendingOwnedCleanupFile> remainingOwnedFiles;
    remainingOwnedFiles.reserve(pendingOwnedCleanupFiles_.size());
    for (const auto& file : pendingOwnedCleanupFiles_)
    {
        if (!discardOwnedCleanupFile(
                 file.path,
                 file.contentHash,
                 file.packageProjectHash).empty())
        {
            remainingOwnedFiles.push_back(file);
        }
    }
    pendingOwnedCleanupFiles_ = std::move(remainingOwnedFiles);

    std::vector<talas::audio::PreparedProjectAssetDisposition>
        remainingDispositions;
    remainingDispositions.reserve(
        pendingSampleAssetDispositionCleanup_.size());
    for (auto& candidate : pendingSampleAssetDispositionCleanup_)
    {
        (void)talas::audio::discardPreparedProjectAssetDispositionFiles(
            candidate);
        if (!candidate.createdFiles.empty()
            || !candidate.cleanupImports.empty())
        {
            remainingDispositions.push_back(std::move(candidate));
        }
    }
    pendingSampleAssetDispositionCleanup_ =
        std::move(remainingDispositions);

    std::vector<talas::audio::PreparedSampleMapImport> remainingImports;
    remainingImports.reserve(pendingSampleMapImportCleanup_.size());
    for (auto& candidate : pendingSampleMapImportCleanup_)
    {
        (void)talas::audio::discardPreparedSampleMapImportFiles(candidate);
        if (!candidate.createdFiles.empty()
            || !candidate.cleanupImports.empty())
        {
            remainingImports.push_back(std::move(candidate));
        }
    }
    pendingSampleMapImportCleanup_ = std::move(remainingImports);
    if (pendingCleanupArtifactCount() != before)
    {
        if (const auto ledger = persistPendingCleanupLedger();
            !ledger.empty())
        {
            pendingProjectFileStatus_ =
                "Pending cleanup ledger failed: " + juceString(ledger);
        }
    }
}

void WorkbenchComponent::requestSelectedSampleWaveform()
{
    if (workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples)
        return;
    const auto& document = projectBridge_.editor().document();
    const auto* map = activeSampleMap(document, workspaceSession_.samples);
    const auto* region = primarySampleRegion(map, workspaceSession_.samples);
    const auto* asset = sampleAsset(document, region);
    if (asset == nullptr)
    {
        if (!sampleWaveformAssetId_.empty())
            stopSampleWaveformWorker();
        sampleWaveformAssetId_.clear();
        sampleWaveformAssetGeneration_ = 0;
        selectedWaveformOverview_.reset();
        sampleWaveformError_.clear();
        sampleWaveformCursorFrame_ = 0;
        return;
    }
    if (sampleWaveformAssetId_ == asset->id
        && sampleWaveformAssetGeneration_ == asset->generation)
    {
        return;
    }
    stopSampleWaveformWorker();
    sampleWaveformAssetId_ = asset->id;
    sampleWaveformAssetGeneration_ = asset->generation;
    selectedWaveformOverview_.reset();
    sampleWaveformError_.clear();
    sampleWaveformCursorFrame_ = 0;
    const auto path = talas::project::resolveProjectAssetPath(
        *asset,
        projectPath());
    if (path.empty())
    {
        sampleWaveformError_ = "Selected asset path cannot be resolved";
        return;
    }
    const auto generation = ++sampleWaveformGeneration_;
    const talas::audio::WaveformOverviewRequest request{
        asset->id,
        asset->generation,
        2048,
        asset->contentHash,
    };
    sampleWaveformStopRequested_.store(false, std::memory_order_release);
    sampleWaveformWorkerFinished_.store(false, std::memory_order_release);
    sampleWaveformThread_ = std::thread(
        [this, generation, path, request] {
            talas::audio::WaveformOverviewResult result;
            try
            {
                result = talas::audio::loadPcm16WaveformOverview(
                    path,
                    request,
                    [this, generation] {
                        return sampleWaveformStopRequested_.load(
                                   std::memory_order_acquire)
                            || generation != sampleWaveformGeneration_.load(
                                std::memory_order_acquire);
                    });
            }
            catch (const std::bad_alloc&)
            {
                result.error =
                    "Waveform overview ran out of memory";
            }
            catch (const std::exception& error)
            {
                result.error = "Waveform overview failed: "
                    + std::string{error.what()};
            }
            catch (...)
            {
                result.error =
                    "Waveform overview failed unexpectedly";
            }
            {
                std::lock_guard lock(sampleWaveformResultMutex_);
                sampleWaveformPublication_ = SampleWaveformPublication{
                    generation,
                    std::move(result),
                };
            }
            sampleWaveformWorkerFinished_.store(
                true,
                std::memory_order_release);
        });
}

void WorkbenchComponent::finishSampleWaveformIfReady()
{
    if (!sampleWaveformWorkerFinished_.load(std::memory_order_acquire))
        return;
    if (sampleWaveformThread_.joinable())
        sampleWaveformThread_.join();
    std::optional<SampleWaveformPublication> publication;
    {
        std::lock_guard lock(sampleWaveformResultMutex_);
        publication = std::move(sampleWaveformPublication_);
        sampleWaveformPublication_.reset();
    }
    if (!publication.has_value()
        || publication->generation
            != sampleWaveformGeneration_.load(std::memory_order_acquire))
    {
        return;
    }
    if (publication->result.ok())
    {
        selectedWaveformOverview_ =
            std::move(publication->result.overview);
        sampleWaveformError_.clear();
    }
    else if (!publication->result.cancelled)
    {
        selectedWaveformOverview_.reset();
        sampleWaveformError_ = juceString(publication->result.error);
    }
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::stopSampleWaveformWorker()
{
    ++sampleWaveformGeneration_;
    sampleWaveformStopRequested_.store(true, std::memory_order_release);
    if (sampleWaveformThread_.joinable())
        sampleWaveformThread_.join();
    sampleWaveformWorkerFinished_.store(true, std::memory_order_release);
    std::lock_guard lock(sampleWaveformResultMutex_);
    sampleWaveformPublication_.reset();
}

void WorkbenchComponent::zoomSampleWaveform(double zoomFactor)
{
    if (selectedWaveformOverview_ == nullptr)
        return;
    const auto viewport = talas::app::waveformViewportFromSession(
        workspaceSession_.samples,
        selectedWaveformOverview_->frameCount);
    const auto cursorPixel = talas::app::waveformFrameToPixel(
        static_cast<double>(sampleWaveformCursorFrame_),
        viewport,
        std::max(1, waveformPanel_->getWidth()));
    const auto zoomed = talas::app::zoomWaveformViewport(
        viewport,
        cursorPixel,
        zoomFactor,
        std::max(1, waveformPanel_->getWidth()),
        selectedWaveformOverview_->frameCount);
    const auto frameCount = static_cast<double>(
        std::max<std::uint64_t>(1, selectedWaveformOverview_->frameCount));
    workspaceSession_.samples.waveformViewport.normalizedStart =
        zoomed.startFrame / frameCount;
    workspaceSession_.samples.waveformViewport.normalizedSpan =
        (zoomed.endFrameExclusive - zoomed.startFrame) / frameCount;
    noteWorkspaceSessionChanged();
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::setSampleWaveformCursor(std::uint64_t frame)
{
    if (selectedWaveformOverview_ == nullptr)
        return;
    sampleWaveformCursorFrame_ = std::min(
        frame,
        selectedWaveformOverview_->frameCount);
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::moveSampleLoopMarker(
    bool releaseLoop,
    talas::app::WaveformLoopMarker marker,
    std::int64_t delta)
{
    const auto& document = projectBridge_.editor().document();
    const auto* map = activeSampleMap(document, workspaceSession_.samples);
    const auto* current = primarySampleRegion(map, workspaceSession_.samples);
    const auto* asset = sampleAsset(document, current);
    if (map == nullptr || current == nullptr || asset == nullptr)
        return;
    auto region = *current;
    auto& loop = releaseLoop ? region.releaseLoop : region.sustainLoop;
    if (!loop.has_value())
    {
        setStatus("Enable the loop in Region Inspector before moving markers");
        return;
    }
    loop = talas::app::moveWaveformLoopMarker(
        *loop,
        marker,
        delta,
        asset->frameCount);
    applySampleRegion(std::move(region));
}

void WorkbenchComponent::auditionActiveSampleMap()
{
    if (sampleMapAuditionActive_
        || sampleMapAuditionPresentation_
            == SampleMapAuditionPresentation::Preparing)
    {
        stopSampleMapAudition();
        return;
    }
    const auto& source = projectBridge_.editor().document();
    const auto* map = activeSampleMap(source, workspaceSession_.samples);
    const auto* region = primarySampleRegion(map, workspaceSession_.samples);
    if (map == nullptr || region == nullptr)
    {
        setStatus("Select a sample region before auditioning");
        return;
    }
    const auto note = selectedSampleMidi_.has_value()
        ? selectedSampleMidi_->note
        : region->rootNote;
    const auto velocity = selectedSampleMidi_.has_value()
        ? selectedSampleMidi_->velocity
        : static_cast<std::uint8_t>(127);
    beginSampleMapAudition({1, note, velocity, 0.0, false, true});
}

void WorkbenchComponent::beginSampleMapAudition(
    SampleMapAuditionVoice voice)
{
    if (usesExternalMidiClock())
    {
        setStatus("Sample-map audition unavailable while external MIDI Clock controls transport");
        return;
    }
    const auto& source = projectBridge_.editor().document();
    const auto sourceAvailability =
        talas::runtime::projectComponentExecutionDiagnostics(
            source,
            registry_);
    if (!sourceAvailability.empty())
    {
        sampleMapAuditionPresentation_ =
            SampleMapAuditionPresentation::Failed;
        publishCompileDiagnostics(sourceAvailability);
        setStatus(
            "Samples audition unavailable: "
            + juceString(sourceAvailability.front().message));
        refreshPersistentStatus();
        commandManager_.commandStatusChanged();
        refreshSampleMapWorkspace();
        return;
    }
    const auto* map = activeSampleMap(source, workspaceSession_.samples);
    const auto* region = primarySampleRegion(map, workspaceSession_.samples);
    if (map == nullptr || region == nullptr)
    {
        setStatus("Select a sample region before auditioning");
        return;
    }
    auto audition = source;
    audition.graph = {};
    audition.virtualControls.clear();
    audition.graph.nodes.push_back({
        "sampleMapAuditionOutput",
        "talas.audioOutput",
        {},
    });
    audition.graph.nodes.push_back({
        "sampleMapAuditionPlayer",
        "talas.sampleMapPlayer",
        {
            textParameter("sampleMapId", map->mapId),
            expressionParameter("frequency", "!Pitch"),
            expressionParameter("gate", "!KeyDown"),
            expressionParameter("velocity", "!KeyVelocity"),
            expressionParameter("scale", "1.0"),
            expressionParameter("attackTime", "0 s"),
            expressionParameter("releaseTime", "0 s"),
        },
    });
    audition.graph.connections.push_back({
        "sampleMapAuditionPlayer",
        "audioOut",
        "sampleMapAuditionOutput",
        "audioIn",
    });

    sampleMapAuditionPresentation_ =
        SampleMapAuditionPresentation::Preparing;
    stopSampleMapAuditionWorker();
    if (sampleMapAuditionSmokeHoldNextWorker_)
    {
        sampleMapAuditionSmokeHoldNextWorker_ = false;
        sampleMapAuditionSmokeHoldWorker_.store(
            true,
            std::memory_order_release);
    }
    const auto generation = ++sampleMapAuditionGeneration_;
    sampleMapAuditionPendingVoice_ = voice;
    auto document = std::make_shared<const talas::project::ProjectDocument>(
        std::move(audition));
    talas::runtime::RenderSettings settings;
    if (auto* device = deviceManager_.getCurrentAudioDevice();
        device != nullptr
        && device->getCurrentSampleRate() > 0.0
        && device->getCurrentBufferSizeSamples() > 0)
    {
        settings.sampleRate = device->getCurrentSampleRate();
        settings.maxBlockFrames = static_cast<std::size_t>(
            device->getCurrentBufferSizeSamples());
    }
    else
    {
        settings.sampleRate = 48000.0;
        settings.maxBlockFrames = 256;
    }
    sampleMapAuditionPreviousDocumentStaged_ = currentDocumentStaged_;
    sampleMapAuditionRestoreSettings_ = settings;
    sampleMapAuditionRestoreDeviceName_ =
        deviceManager_.getCurrentAudioDevice() != nullptr
        ? deviceManager_.getCurrentAudioDevice()->getName()
        : juce::String{"offline render"};
    const auto sourceProjectPath = projectPath();
    sampleMapAuditionStopRequested_.store(
        false,
        std::memory_order_release);
    sampleMapAuditionWorkerFinished_.store(
        false,
        std::memory_order_release);
    sampleMapAuditionThread_ = std::thread(
        [this,
         generation,
         document,
         voice,
         settings = std::move(settings),
         sourceProjectPath]() mutable {
            if (sampleMapAuditionSmokeHoldWorker_.load(
                    std::memory_order_acquire))
            {
                sampleMapAuditionSmokeWorkerPaused_.store(
                    true,
                    std::memory_order_release);
                sampleMapAuditionSmokeHoldWorker_.wait(
                    true,
                    std::memory_order_acquire);
                sampleMapAuditionSmokeWorkerPaused_.store(
                    false,
                    std::memory_order_release);
            }
            auto loaded =
                talas::audio::loadReferencedProjectResourceSnapshots(
                    *document,
                    registry_,
                    sourceProjectPath,
                    std::span<const std::string>{},
                    [this, generation] {
                        return sampleMapAuditionStopRequested_.load(
                                   std::memory_order_acquire)
                            || generation
                                != sampleMapAuditionGeneration_.load(
                                    std::memory_order_acquire);
                    });
            {
                std::lock_guard lock(sampleMapAuditionResultMutex_);
                sampleMapAuditionPublication_ =
                    SampleMapAuditionPublication{
                        generation,
                        document,
                        std::move(settings),
                        voice,
                        std::move(loaded),
                    };
            }
            sampleMapAuditionWorkerFinished_.store(
                true,
                std::memory_order_release);
        });
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::finishSampleMapAuditionIfReady()
{
    if (!sampleMapAuditionWorkerFinished_.load(
            std::memory_order_acquire))
    {
        return;
    }
    if (sampleMapAuditionThread_.joinable())
        sampleMapAuditionThread_.join();
    std::optional<SampleMapAuditionPublication> publication;
    {
        std::lock_guard lock(sampleMapAuditionResultMutex_);
        publication = std::move(sampleMapAuditionPublication_);
        sampleMapAuditionPublication_.reset();
    }
    if (!publication.has_value()
        || publication->generation
            != sampleMapAuditionGeneration_.load(
                std::memory_order_acquire))
    {
        return;
    }
    if (!publication->loaded.ok())
    {
        sampleMapAuditionPendingVoice_.reset();
        currentDocumentStaged_ =
            sampleMapAuditionPreviousDocumentStaged_;
        sampleMapAuditionRestoreSettings_.reset();
        sampleMapAuditionRestoreDeviceName_.clear();
        sampleMapAuditionPresentation_ =
            SampleMapAuditionPresentation::Failed;
        if (publication->loaded.failure.has_value())
        {
            const auto& failure = *publication->loaded.failure;
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
                publication->document.get());
            if (failure.assetId.empty()
                || !openAssetRecovery(failure))
            {
                setStatus(juceString(message));
            }
        }
        else
        {
            setStatus(
                publication->loaded.wasCancelled
                    ? juce::String{"Sample-map audition cancelled"}
                    : juce::String{"Sample-map audition asset preparation failed"});
        }
        refreshSampleMapWorkspace();
        return;
    }
    if (usesExternalMidiClock())
    {
        sampleMapAuditionPendingVoice_.reset();
        currentDocumentStaged_ =
            sampleMapAuditionPreviousDocumentStaged_;
        sampleMapAuditionRestoreSettings_.reset();
        sampleMapAuditionRestoreDeviceName_.clear();
        sampleMapAuditionPresentation_ =
            SampleMapAuditionPresentation::Failed;
        setStatus("Sample-map audition cancelled because external MIDI Clock now controls transport");
        refreshSampleMapWorkspace();
        return;
    }
    publication->settings.resourceSnapshots.insert(
        publication->settings.resourceSnapshots.end(),
        std::make_move_iterator(
            publication->loaded.snapshots.begin()),
        std::make_move_iterator(
            publication->loaded.snapshots.end()));
    stopAnalysisOutputTakeForLifecycle();
    const auto staged = projectBridge_.compileAndStageDocument(
        *publication->document,
        std::move(publication->settings));
    if (!staged.ok())
    {
        sampleMapAuditionPendingVoice_.reset();
        currentDocumentStaged_ =
            sampleMapAuditionPreviousDocumentStaged_;
        sampleMapAuditionRestoreSettings_.reset();
        sampleMapAuditionRestoreDeviceName_.clear();
        sampleMapAuditionPresentation_ =
            SampleMapAuditionPresentation::Failed;
        if (!staged.diagnostics.empty())
        {
            publishCompileDiagnostics(
                staged.diagnostics,
                publication->document.get());
        }
        setStatus(
            "Sample-map audition failed: "
            + juceString(
                staged.message.empty()
                    ? std::string{"unknown staging error"}
                    : staged.message));
        refreshSampleMapWorkspace();
        return;
    }
    if (!projectBridge_.enqueueMidiNoteOn(
            publication->voice.channel,
            publication->voice.note,
            publication->voice.velocity,
            publication->voice.arrivalTimeSeconds))
    {
        sampleMapAuditionPendingVoice_.reset();
        sampleMapAuditionActiveVoice_.reset();
        sampleMapAuditionActive_ = true;
        sampleMapAuditionPresentation_ =
            SampleMapAuditionPresentation::Active;
        currentDocumentStaged_ = false;
        stopSampleMapAudition();
        if (sampleMapAuditionPresentation_
            == SampleMapAuditionPresentation::Stopping)
        {
            setStatus("Sample-map audition note-on was rejected; restoring the current project graph...");
        }
        return;
    }
    setAuditionPresentation(std::nullopt);
    sampleMapAuditionActive_ = true;
    sampleMapAuditionActiveVoice_ = publication->voice;
    sampleMapAuditionPendingVoice_.reset();
    sampleMapAuditionPresentation_ =
        SampleMapAuditionPresentation::Active;
    currentDocumentStaged_ = false;
    projectBridge_.setPlaying(true);
    transportPlaying_ = true;
    analysisPanel_->setPlaying(true);
    setStatus(
        "Auditioning isolated sample map"
        + outputMappingStatus(staged.outputChannelCount));
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::stopSampleMapAuditionWorker()
{
    sampleMapAuditionStopRequested_.store(
        true,
        std::memory_order_release);
    sampleMapAuditionSmokeHoldWorker_.store(
        false,
        std::memory_order_release);
    sampleMapAuditionSmokeHoldWorker_.notify_all();
    if (sampleMapAuditionThread_.joinable())
        sampleMapAuditionThread_.join();
    sampleMapAuditionSmokeWorkerPaused_.store(
        false,
        std::memory_order_release);
    sampleMapAuditionWorkerFinished_.store(
        true,
        std::memory_order_release);
    std::lock_guard lock(sampleMapAuditionResultMutex_);
    sampleMapAuditionPublication_.reset();
}

void WorkbenchComponent::stopSampleMapAudition()
{
    const auto wasActive = sampleMapAuditionActive_;
    const auto previousDocumentStaged =
        sampleMapAuditionPreviousDocumentStaged_;
    ++sampleMapAuditionGeneration_;
    sampleMapAuditionPresentation_ =
        SampleMapAuditionPresentation::Stopping;
    refreshSampleMapWorkspace();
    stopSampleMapAuditionWorker();
    sampleMapAuditionActive_ = false;
    projectBridge_.stopTransport();
    transportPlaying_ = false;
    analysisPanel_->setPlaying(false);
    sampleMapAuditionPendingVoice_.reset();
    sampleMapAuditionActiveVoice_.reset();
    if (!wasActive)
    {
        currentDocumentStaged_ = previousDocumentStaged;
        sampleMapAuditionRestoreSettings_.reset();
        sampleMapAuditionRestoreDeviceName_.clear();
        sampleMapAuditionPresentation_ = SampleMapAuditionPresentation::Idle;
        setStatus("Cancelled isolated sample-map audition before graph publication");
    }
    else
    {
        setStatus("Stopped isolated sample-map audition; restoring current project graph...");
        finishSampleMapAuditionRestoreIfReady();
    }
    refreshPersistentStatus();
    commandManager_.commandStatusChanged();
    refreshSampleMapWorkspace();
}

void WorkbenchComponent::finishSampleMapAuditionRestoreIfReady()
{
    if (sampleMapAuditionPresentation_
            != SampleMapAuditionPresentation::Stopping
        || !sampleMapAuditionRestoreSettings_.has_value())
    {
        return;
    }
    if (projectBridge_.preparedGraphStatus().awaitingActivation)
        return;
    const auto restored = stageProject(
        *sampleMapAuditionRestoreSettings_,
        sampleMapAuditionRestoreDeviceName_);
    sampleMapAuditionRestoreSettings_.reset();
    sampleMapAuditionRestoreDeviceName_.clear();
    sampleMapAuditionPresentation_ = restored
        ? SampleMapAuditionPresentation::Idle
        : SampleMapAuditionPresentation::Failed;
    setStatus(
        restored
            ? juce::String{"Stopped isolated sample-map audition; current project graph restored"}
            : juce::String{"Stopped isolated sample-map audition; current project graph could not be restored"});
    refreshPersistentStatus();
    commandManager_.commandStatusChanged();
    refreshSampleMapWorkspace();
}
}
