#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace talas::app::workbench
{
namespace
{
[[nodiscard]] talas::project::MaterializedPartialTrackEditsResult
materializePreviewSource(
    const PartialTrackEditorSourceSnapshot& snapshot)
{
    if (snapshot.edits.has_value())
    {
        return talas::project::materializeProjectPartialTrackEdits(
            *snapshot.source,
            *snapshot.edits);
    }
    talas::project::MaterializedPartialTrackEditsResult result;
    result.materialized.asset = *snapshot.source;
    std::size_t pointCount = 0;
    for (const auto& frame : snapshot.source->frames)
        pointCount += frame.points.size();
    result.materialized.pointInterpolations.assign(
        pointCount,
        talas::project::PartialTrackPointInterpolation::Linear);
    return result;
}

void applyPreviewTrackStates(
    talas::project::MaterializedPartialTrackEdits& materialized)
{
    const auto hasSolo = std::ranges::any_of(
        materialized.trackStates,
        &talas::project::ProjectPartialTrackState::solo);
    std::unordered_set<std::uint64_t> mutedTrackIds;
    std::unordered_set<std::uint64_t> soloTrackIds;
    mutedTrackIds.reserve(materialized.trackStates.size());
    soloTrackIds.reserve(materialized.trackStates.size());
    for (const auto& state : materialized.trackStates)
    {
        if (state.muted)
            mutedTrackIds.insert(state.trackId);
        if (state.solo && !state.muted)
            soloTrackIds.insert(state.trackId);
    }
    if (!hasSolo && mutedTrackIds.empty())
        return;
    std::vector<talas::project::PartialTrackPointInterpolation> retained;
    retained.reserve(materialized.pointInterpolations.size());
    std::size_t interpolationIndex = 0;
    for (auto& frame : materialized.asset.frames)
    {
        auto output = frame.points.begin();
        for (const auto& point : frame.points)
        {
            if (!mutedTrackIds.contains(point.trackId)
                && (!hasSolo || soloTrackIds.contains(point.trackId)))
            {
                retained.push_back(
                    materialized.pointInterpolations[interpolationIndex]);
                *output++ = point;
            }
            ++interpolationIndex;
        }
        frame.points.erase(output, frame.points.end());
    }
    materialized.pointInterpolations = std::move(retained);
}
}

void WorkbenchComponent::refreshPartialTrackOperationState()
{
    PartialTrackEditorOperationState state;
    state.previewPreparing =
        !partialTrackPreviewWorkerFinished_.load(
            std::memory_order_acquire);
    state.previewActive = partialTrackPreviewActive_ != nullptr;
    state.derivativeRunning =
        !partialTrackDerivativeFinished_.load(
            std::memory_order_acquire);
    const auto sourceInfo =
        partialTrackEditorPanel_->operationSourceInfo();
    const auto hasSource = sourceInfo.has_value();
    auto* device = deviceManager_.getCurrentAudioDevice();
    state.canStartPreview = hasSource
        && currentDocumentStaged_
        && device != nullptr
        && device->getCurrentSampleRate() > 0.0
        && device->getCurrentBufferSizeSamples() > 0
        && device->getCurrentSampleRate()
            == sourceInfo->sampleRate
        && sourceInfo->maximumRenderedSegmentTrackCount
            <= talas::audio::maximumRealtimePartialTrackVoices
        && partialTrackPreviewWorkerFinished_.load(
            std::memory_order_acquire)
        && partialTrackPreviewRetiring_ == nullptr
        && partialTrackPreviewRetiringNext_ == nullptr
        && !state.derivativeRunning;
    const auto canStartDerivative = hasSource
        && !state.derivativeRunning
        && !exportRunning_
        && !packageRunning_
        && !fileChooserActive_
        && !fileOperationCoordinator_.snapshot().active();
    state.canStartAudioDerivative = canStartDerivative
        && sourceInfo->maximumRenderedSegmentTrackCount
            <= talas::audio::maximumOfflinePartialTrackVoices;
    state.canStartPartialDerivative = canStartDerivative;
    state.canImportDerivative =
        lastPartialTrackDerivative_.has_value()
        && lastPartialTrackDerivative_->ok();
    if (partialTrackDerivativeToken_.has_value())
    {
        const auto operation = fileOperationCoordinator_.snapshot();
        if (operation.generation
            == partialTrackDerivativeToken_->generation)
        {
            state.progressPermille = operation.progressPermille;
        }
    }
    else if (state.previewPreparing)
    {
        state.progressPermille = partialTrackPreviewProgress_.load(
            std::memory_order_acquire);
    }
    if (state.derivativeRunning
        || partialTrackDerivativeStatus_ != "Render idle")
    {
        state.status = partialTrackDerivativeStatus_.toStdString();
    }
    else
    {
        state.status = partialTrackPreviewStatus_.toStdString();
    }
    partialTrackEditorPanel_->setOperationState(std::move(state));
}

void WorkbenchComponent::startPartialTrackPreview()
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr
        || device->getCurrentSampleRate() <= 0.0
        || device->getCurrentBufferSizeSamples() <= 0)
    {
        setStatus("Partial-track preview requires an active audio device");
        return;
    }
    startPartialTrackPreviewWithProfile(
        device->getCurrentSampleRate(),
        static_cast<std::size_t>(
            device->getCurrentBufferSizeSamples()),
        deviceGeneration_.load(std::memory_order_acquire));
}

void WorkbenchComponent::startPartialTrackPreviewWithProfile(
    double sampleRate,
    std::size_t maxBlockFrames,
    std::uint64_t sourceDeviceGeneration)
{
    stopSoundLibraryAudition();
    stopAnalysisAudition();
    finishPartialTrackPreviewIfReady();
    retirePartialTrackPreviewIfQuiescent();
    if (!partialTrackPreviewWorkerFinished_.load(
            std::memory_order_acquire))
    {
        setStatus("Partial-track preview preparation is already running");
        return;
    }
    if (partialTrackPreviewRetiring_ != nullptr
        || partialTrackPreviewRetiringNext_ != nullptr)
    {
        setStatus("Prior partial-track preview is still retiring");
        return;
    }
    if (!currentDocumentStaged_)
    {
        setStatus("Partial-track preview requires the current exact staged project");
        return;
    }
    auto snapshot = partialTrackEditorPanel_->sourceSnapshot();
    if (!snapshot.has_value()
        || snapshot->source == nullptr
        || !std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || sampleRate != snapshot->source->sampleRate
        || maxBlockFrames == 0
        || maxBlockFrames > (1U << 20U))
    {
        setStatus("Partial-track preview requires an exact-rate immutable editor source");
        return;
    }

    std::string sourceDocumentBytes;
    try
    {
        sourceDocumentBytes = talas::project::serializeProjectDocument(
            projectBridge_.editor().document());
    }
    catch (const std::exception& error)
    {
        setStatus(
            "Partial-track preview authority capture failed: "
            + juceString(error.what()));
        return;
    }
    if (partialTrackPreviewThread_.joinable())
        partialTrackPreviewThread_.join();
    const auto requestGeneration =
        partialTrackPreviewRequestGeneration_.fetch_add(
            1, std::memory_order_acq_rel)
        + 1;
    const auto sourceDocumentGeneration =
        uiDocumentGeneration_.load(std::memory_order_acquire);
    const auto sourceStageGeneration =
        projectBridge_.latestStageGeneration();
    partialTrackPreviewWorkerFinished_.store(
        false, std::memory_order_release);
    partialTrackPreviewProgress_.store(0, std::memory_order_release);
    partialTrackPreviewStatus_ =
        partialTrackPreviewActive_ != nullptr
        ? juceString("Preparing replacement preview · last good remains active")
        : "Preparing immutable partial-track preview";
    setStatus(partialTrackPreviewStatus_);
    refreshPartialTrackOperationState();

    try
    {
        partialTrackPreviewThread_ = std::jthread(
            [this,
             snapshot = std::move(*snapshot),
             sourceDocumentBytes = std::move(sourceDocumentBytes),
             maxBlockFrames,
             sourceDocumentGeneration,
             sourceStageGeneration,
             sourceDeviceGeneration,
             requestGeneration](std::stop_token stopToken) mutable {
                PartialTrackPreviewPublication publication;
                publication.requestGeneration = requestGeneration;
                publication.sourceDocumentGeneration =
                    sourceDocumentGeneration;
                try
                {
                    publication.sourceDocumentBytes = sourceDocumentBytes;
                    if (stopToken.stop_requested())
                    {
                        publication.cancelled = true;
                    }
                    else
                    {
                        const auto canonicalSource =
                            talas::project::encodePartialTrackAsset(
                                *snapshot.source);
                        if (!canonicalSource.ok()
                            || talas::project::calculateProjectAssetSha256(
                                    canonicalSource.bytes)
                                != snapshot.manifest.contentHash)
                        {
                            publication.diagnostic =
                                "Partial-track preview source bytes do not match the exact manifest hash";
                        }
                        else
                        {
                            partialTrackPreviewProgress_.store(
                                250, std::memory_order_release);
                            auto materialized =
                                materializePreviewSource(snapshot);
                            if (!materialized.ok())
                            {
                                publication.diagnostic =
                                    materialized.error.has_value()
                                    ? materialized.error->message
                                    : "Partial-track preview materialization failed";
                            }
                            else
                            {
                                partialTrackPreviewProgress_.store(
                                    600, std::memory_order_release);
                                applyPreviewTrackStates(
                                    materialized.materialized);
                                // The scrub bar or a stroke plays the frames
                                // from there.
                                auto playable = snapshot.scrub.has_value()
                                    ? talas::app::partialTrackScrubbed(materialized.materialized, snapshot.scrub->tracks,
                                          snapshot.selection, snapshot.scrub->frame, snapshot.scrub->frames)
                                    : talas::app::partialTrackPlayable(
                                          materialized.materialized, snapshot.play, snapshot.selection, snapshot.markers);
                                const auto nothingScrubbed = snapshot.scrub.has_value() && !playable.has_value();
                                if (playable.has_value())
                                    materialized.materialized = std::move(*playable);
                                auto runtime = std::make_unique<
                                    PartialTrackPreviewRuntime>();
                                runtime->synthesizer = std::make_unique<
                                    talas::audio::PreparedPartialTrackResynthesizer>(
                                        materialized.materialized.asset,
                                        materialized.materialized
                                            .pointInterpolations,
                                        talas::audio::PartialTrackResynthesisProfile{
                                            talas::audio::maximumRealtimePartialTrackVoices,
                                            1.0,
                                            0});
                                runtime->scratch.resize(maxBlockFrames);
                                runtime->sourceDocumentBytes =
                                    sourceDocumentBytes;
                                runtime->sourceManifest = snapshot.manifest;
                                runtime->requestGeneration =
                                    requestGeneration;
                                runtime->sourceDocumentGeneration =
                                    sourceDocumentGeneration;
                                runtime->sourceStageGeneration =
                                    sourceStageGeneration;
                                runtime->sourceDeviceGeneration =
                                    sourceDeviceGeneration;
                                if (nothingScrubbed)
                                {
                                    // No point under the scrub bar: nothing to hear.
                                    publication.cancelled = true;
                                }
                                else if (!runtime->synthesizer->ready())
                                {
                                    publication.diagnostic =
                                        "Partial-track preview resynthesizer preparation failed";
                                }
                                else if (stopToken.stop_requested())
                                {
                                    publication.cancelled = true;
                                }
                                else
                                {
                                    partialTrackPreviewProgress_.store(
                                        900, std::memory_order_release);
                                    publication.runtime =
                                        std::move(runtime);
                                }
                            }
                        }
                    }
                }
                catch (const std::bad_alloc&)
                {
                    publication.allocationFailed = true;
                }
                catch (...)
                {
                    publication.preparationFailed = true;
                }
                {
                    const std::scoped_lock lock{
                        partialTrackPreviewResultMutex_};
                    partialTrackPreviewPublication_ =
                        std::move(publication);
                }
                partialTrackPreviewWorkerFinished_.store(
                    true, std::memory_order_release);
            });
    }
    catch (const std::exception& error)
    {
        partialTrackPreviewWorkerFinished_.store(
            true, std::memory_order_release);
        partialTrackPreviewStatus_ =
            "Partial-track preview worker failed to start: "
            + juceString(error.what());
        setStatus(partialTrackPreviewStatus_);
        refreshPartialTrackOperationState();
    }
}

void WorkbenchComponent::stopPartialTrackPreview()
{
    const auto hadPreview =
        partialTrackPreviewThread_.joinable()
        || partialTrackPreviewActive_ != nullptr
        || partialTrackPreviewRetiring_ != nullptr;
    partialTrackPreviewRequestGeneration_.fetch_add(
        1, std::memory_order_acq_rel);
    if (partialTrackPreviewThread_.joinable()
        && !partialTrackPreviewWorkerFinished_.load(
            std::memory_order_acquire))
    {
        partialTrackPreviewThread_.request_stop();
        partialTrackPreviewStatus_ =
            "Cancelling partial-track preview preparation";
    }
    partialTrackPreviewLifecycle_.store(
        0, std::memory_order_seq_cst);
    partialTrackPreviewCallback_.store(
        nullptr, std::memory_order_release);
    if (partialTrackPreviewActive_ != nullptr)
    {
        if (partialTrackPreviewRetiring_ == nullptr)
            partialTrackPreviewRetiring_ =
                std::move(partialTrackPreviewActive_);
        else if (partialTrackPreviewRetiringNext_ == nullptr)
            partialTrackPreviewRetiringNext_ =
                std::move(partialTrackPreviewActive_);
        else
            partialTrackPreviewStatus_ =
                "Partial-track preview is waiting for callback quiescence";
    }
    retirePartialTrackPreviewIfQuiescent();
    if (hadPreview
        && partialTrackPreviewWorkerFinished_.load(
            std::memory_order_acquire))
    {
        partialTrackPreviewStatus_ = "Partial-track preview stopped";
    }
    refreshPartialTrackOperationState();
}

void WorkbenchComponent::finishPartialTrackPreviewIfReady()
{
    retirePartialTrackPreviewIfQuiescent();
    if (partialTrackPreviewActive_ != nullptr)
    {
        const auto renderFailed =
            partialTrackPreviewActive_->renderFailed.exchange(
                false, std::memory_order_acq_rel);
        const auto authorityChanged = !currentDocumentStaged_
            || partialTrackPreviewActive_->sourceDocumentGeneration
                != uiDocumentGeneration_.load(std::memory_order_acquire)
            || partialTrackPreviewActive_->sourceStageGeneration
                != projectBridge_.latestStageGeneration()
            || partialTrackPreviewActive_->sourceDeviceGeneration
                != deviceGeneration_.load(std::memory_order_acquire);
        if (renderFailed || authorityChanged)
        {
            stopPartialTrackPreview();
            partialTrackPreviewStatus_ = renderFailed
                ? "Partial-track preview held after a bounded render failure"
                : "Partial-track preview revoked because stage or device authority changed";
            setStatus(partialTrackPreviewStatus_);
            refreshPartialTrackOperationState();
        }
    }
    if (!partialTrackPreviewWorkerFinished_.load(
            std::memory_order_acquire))
    {
        return;
    }
    if (partialTrackPreviewThread_.joinable())
        partialTrackPreviewThread_.join();
    if (partialTrackPreviewRetiring_ != nullptr
        || partialTrackPreviewRetiringNext_ != nullptr)
        return;
    std::optional<PartialTrackPreviewPublication> publication;
    {
        const std::scoped_lock lock{partialTrackPreviewResultMutex_};
        publication = std::move(partialTrackPreviewPublication_);
        partialTrackPreviewPublication_.reset();
    }
    if (!publication.has_value())
        return;
    const auto currentGeneration =
        partialTrackPreviewRequestGeneration_.load(
            std::memory_order_acquire);
    if (publication->cancelled
        || publication->requestGeneration != currentGeneration)
    {
        partialTrackPreviewStatus_ = "Partial-track preview cancelled";
        refreshPartialTrackOperationState();
        return;
    }
    if (publication->runtime == nullptr)
    {
        partialTrackPreviewStatus_ = juceString(
            publication->allocationFailed
                ? "Partial-track preview allocation failed"
                : publication->preparationFailed
                    ? "Partial-track preview preparation failed"
                    : publication->diagnostic.empty()
                ? "Partial-track preview preparation failed"
                : publication->diagnostic);
        setStatus(partialTrackPreviewStatus_);
        refreshPartialTrackOperationState();
        return;
    }
    bool exact = false;
    try
    {
        const auto& document = projectBridge_.editor().document();
        const auto manifest = std::ranges::find(
            document.assets,
            publication->runtime->sourceManifest.id,
            &talas::project::ProjectAsset::id);
        exact = publication->sourceDocumentBytes
                == talas::project::serializeProjectDocument(document)
            && publication->sourceDocumentGeneration
                == uiDocumentGeneration_.load(std::memory_order_acquire)
            && publication->runtime->sourceStageGeneration
                == projectBridge_.latestStageGeneration()
            && publication->runtime->sourceDeviceGeneration
                == deviceGeneration_.load(std::memory_order_acquire)
            && currentDocumentStaged_
            && manifest != document.assets.end()
            && *manifest == publication->runtime->sourceManifest;
    }
    catch (...)
    {
        exact = false;
    }
    if (!exact)
    {
        partialTrackPreviewStatus_ =
            "Partial-track preview cancelled because source, edits, stage, or device authority changed";
        setStatus(partialTrackPreviewStatus_);
        refreshPartialTrackOperationState();
        return;
    }

    partialTrackPreviewLifecycle_.store(
        0, std::memory_order_seq_cst);
    partialTrackPreviewCallback_.store(
        nullptr, std::memory_order_release);
    if (partialTrackPreviewActive_ != nullptr)
    {
        partialTrackPreviewRetiring_ =
            std::move(partialTrackPreviewActive_);
    }
    partialTrackPreviewActive_ = std::move(publication->runtime);
    partialTrackPreviewCallback_.store(
        partialTrackPreviewActive_.get(),
        std::memory_order_release);
    partialTrackPreviewLifecycle_.store(
        1, std::memory_order_seq_cst);
    partialTrackPreviewStatus_ =
        juceString("Preview active · immutable generation ")
        + juce::String(static_cast<juce::int64>(
            partialTrackPreviewActive_->sourceManifest.generation))
        + juceString(" · additive resynthesis with deterministic bandwidth noise");
    partialTrackPreviewProgress_.store(
        1000, std::memory_order_release);
    setStatus(partialTrackPreviewStatus_);
    retirePartialTrackPreviewIfQuiescent();
    refreshPartialTrackOperationState();
}

void WorkbenchComponent::retirePartialTrackPreviewIfQuiescent()
{
    if (partialTrackPreviewAudioOperations_.load(
            std::memory_order_seq_cst)
        == 0)
    {
        partialTrackPreviewRetiring_.reset();
        partialTrackPreviewRetiringNext_.reset();
        if (partialTrackPreviewLifecycle_.load(
                std::memory_order_seq_cst)
            != 1)
        {
            partialTrackPreviewActive_.reset();
        }
    }
}

std::optional<bool> WorkbenchComponent::renderPartialTrackPreview(
    float* const* outputChannels,
    std::size_t outputChannelCount,
    std::size_t frameCount) noexcept
{
    if (partialTrackPreviewLifecycle_.load(
            std::memory_order_seq_cst)
        != 1)
    {
        return std::nullopt;
    }
    partialTrackPreviewAudioOperations_.fetch_add(
        1, std::memory_order_seq_cst);
    if (partialTrackPreviewLifecycle_.load(
            std::memory_order_seq_cst)
        != 1)
    {
        partialTrackPreviewAudioOperations_.fetch_sub(
            1, std::memory_order_seq_cst);
        return std::nullopt;
    }
    auto* runtime = partialTrackPreviewCallback_.load(
        std::memory_order_acquire);
    bool rendered = false;
    if (runtime != nullptr
        && runtime->synthesizer != nullptr
        && frameCount <= runtime->scratch.size()
        && outputChannels != nullptr
        && outputChannelCount != 0)
    {
        rendered = true;
        std::size_t completed = 0;
        while (completed < frameCount)
        {
            if (runtime->sourceFrame
                >= runtime->synthesizer->sourceFrameCount())
            {
                runtime->synthesizer->reset();
                runtime->sourceFrame = 0;
            }
            const auto count = static_cast<std::size_t>(std::min<
                std::uint64_t>(
                    frameCount - completed,
                    runtime->synthesizer->sourceFrameCount()
                        - runtime->sourceFrame));
            if (count == 0
                || runtime->synthesizer->render(
                       runtime->sourceFrame,
                       std::span<double>{runtime->scratch}.first(count))
                    != talas::audio::PartialTrackResynthesisStatus::Ok)
            {
                rendered = false;
                runtime->renderFailed.store(
                    true, std::memory_order_release);
                break;
            }
            for (std::size_t channel = 0;
                 channel < std::min<std::size_t>(2, outputChannelCount);
                 ++channel)
            {
                if (outputChannels[channel] == nullptr)
                    continue;
                for (std::size_t frame = 0; frame < count; ++frame)
                {
                    outputChannels[channel][completed + frame] =
                        static_cast<float>(runtime->scratch[frame]);
                }
            }
            runtime->sourceFrame += count;
            completed += count;
        }
    }
    partialTrackPreviewAudioOperations_.fetch_sub(
        1, std::memory_order_seq_cst);
    return rendered;
}

void WorkbenchComponent::choosePartialTrackDerivativeDestination(
    talas::audio::PartialTrackDerivativeKind kind)
{
    if (fileChooserActive_)
    {
        setStatus("A file dialog is already open");
        return;
    }
    if (!partialTrackDerivativeFinished_.load(
            std::memory_order_acquire))
    {
        setStatus("A partial-track derivative is already rendering");
        return;
    }
    const auto extension =
        kind == talas::audio::PartialTrackDerivativeKind::Audio
        ? ".wav"
        : ".talaspartial";
    juce::File initial = projectPath().has_value()
        ? juce::File(jucePathString(*projectPath()))
              .getSiblingFile("Partial Tracks Edited" + juce::String(extension))
        : juce::File::getSpecialLocation(
              juce::File::userDocumentsDirectory)
              .getChildFile("Partial Tracks Edited" + juce::String(extension));
    fileChooser_ = std::make_unique<juce::FileChooser>(
        kind == talas::audio::PartialTrackDerivativeKind::Audio
            ? "Render Edited Partial Tracks to Audio"
            : "Render Edited Partial Tracks",
        initial,
        kind == talas::audio::PartialTrackDerivativeKind::Audio
            ? "*.wav"
            : "*.talaspartial");
    fileChooserActive_ = true;
    refreshPartialTrackOperationState();
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    fileChooser_->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, kind, extension](const juce::FileChooser& chooser) {
            if (safeThis == nullptr)
                return;
            safeThis->fileChooserActive_ = false;
            safeThis->refreshPartialTrackOperationState();
            safeThis->scheduleWorkbenchFocusRestoration();
            auto selected = chooser.getResult();
            if (selected == juce::File{})
                return;
            if (selected.getFileExtension().isEmpty())
                selected = selected.withFileExtension(extension);
            const auto destination = filesystemPath(
                selected.getFullPathName());
            safeThis->scheduleContinuation(
                [safeThis, kind, destination] {
                    if (safeThis != nullptr)
                    {
                        safeThis->startPartialTrackDerivative(
                            kind, destination);
                    }
                });
        });
}

void WorkbenchComponent::startPartialTrackDerivative(
    talas::audio::PartialTrackDerivativeKind kind,
    std::filesystem::path destination)
{
    finishPartialTrackDerivativeIfReady();
    if (!partialTrackDerivativeFinished_.load(
            std::memory_order_acquire)
        || exportRunning_
        || packageRunning_)
    {
        setStatus("Finish the current file operation before rendering a partial-track derivative");
        return;
    }
    auto snapshot = partialTrackEditorPanel_->sourceSnapshot();
    if (!snapshot.has_value() || snapshot->source == nullptr)
    {
        setStatus("No exact PartialSpectrum source is available to render");
        return;
    }
    if (partialTrackDerivativeThread_.joinable())
        partialTrackDerivativeThread_.join();
    const auto operation = fileOperationCoordinator_.begin(
        talas::app::WorkbenchOperationKind::PartialTrackDerivative);
    if (!operation.has_value())
    {
        setStatus("Unable to begin partial-track derivative operation");
        return;
    }
    const auto documentAuthorityGeneration =
        projectBridge_.editor().documentAuthorityGeneration();
    talas::audio::PartialTrackDerivativeRequest request;
    request.destination = std::move(destination);
    request.resolvedSourcePath = talas::audio::resolveProjectAssetPath(
        snapshot->manifest, projectPath());
    request.sourceManifest = snapshot->manifest;
    request.source = snapshot->source;
    request.edits = snapshot->edits;
    request.kind = kind;
    request.blockFrames = 4096;
    partialTrackDerivativeToken_ = *operation;
    partialTrackDerivativeFinished_.store(
        false, std::memory_order_release);
    partialTrackDerivativeStatus_ =
        kind == talas::audio::PartialTrackDerivativeKind::Audio
        ? "Rendering immutable Audio derivative"
        : "Rendering immutable edited PartialSpectrum derivative";
    setStatus(partialTrackDerivativeStatus_);
    refreshPartialTrackOperationState();

    try
    {
        partialTrackDerivativeThread_ = std::thread(
            [this,
             request = std::move(request),
             operation = *operation,
             documentAuthorityGeneration]() mutable {
                auto result = talas::audio::renderPartialTrackDerivative(
                    request,
                    [this, operation, documentAuthorityGeneration] {
                        return fileOperationShouldCancel(
                            operation, documentAuthorityGeneration);
                    },
                    [this, operation, documentAuthorityGeneration] {
                        return admitFileOperationPublication(
                            operation, documentAuthorityGeneration);
                    },
                    [this, operation](std::uint16_t value) {
                        (void) fileOperationCoordinator_.publishProgress(
                            operation,
                            static_cast<double>(value) / 1000.0);
                    });
                {
                    const std::scoped_lock lock{
                        partialTrackDerivativeResultMutex_};
                    partialTrackDerivativePublication_ =
                        std::move(result);
                }
                partialTrackDerivativeFinished_.store(
                    true, std::memory_order_release);
            });
    }
    catch (const std::exception& error)
    {
        partialTrackDerivativeFinished_.store(
            true, std::memory_order_release);
        (void) fileOperationCoordinator_.finish(
            *operation,
            talas::app::WorkbenchOperationOutcome::Failed);
        partialTrackDerivativeToken_.reset();
        partialTrackDerivativeStatus_ =
            "Partial-track derivative worker failed to start: "
            + juceString(error.what());
        setStatus(partialTrackDerivativeStatus_);
        refreshPartialTrackOperationState();
    }
}

void WorkbenchComponent::cancelPartialTrackDerivative()
{
    if (!partialTrackDerivativeToken_.has_value())
        return;
    if (fileOperationCoordinator_.requestCancel(
            *partialTrackDerivativeToken_))
    {
        partialTrackDerivativeStatus_ =
            "Cancelling partial-track derivative";
        setStatus(partialTrackDerivativeStatus_);
        refreshPartialTrackOperationState();
    }
}

void WorkbenchComponent::finishPartialTrackDerivativeIfReady()
{
    if (!partialTrackDerivativeFinished_.load(
            std::memory_order_acquire))
    {
        refreshPartialTrackOperationState();
        return;
    }
    if (partialTrackDerivativeThread_.joinable())
        partialTrackDerivativeThread_.join();
    std::optional<talas::audio::PartialTrackDerivativeResult> result;
    {
        const std::scoped_lock lock{partialTrackDerivativeResultMutex_};
        result = std::move(partialTrackDerivativePublication_);
        partialTrackDerivativePublication_.reset();
    }
    if (!result.has_value())
        return;
    const auto token = partialTrackDerivativeToken_;
    if (token.has_value())
    {
        const auto outcome = result->ok()
            ? talas::app::WorkbenchOperationOutcome::Succeeded
            : result->status
                    == talas::audio::PartialTrackDerivativeStatus::Cancelled
                || result->status
                    == talas::audio::PartialTrackDerivativeStatus::AuthorityChanged
                ? talas::app::WorkbenchOperationOutcome::Cancelled
                : talas::app::WorkbenchOperationOutcome::Failed;
        (void) fileOperationCoordinator_.finish(*token, outcome);
    }
    const auto operation = fileOperationCoordinator_.snapshot();
    partialTrackDerivativeToken_.reset();
    if (result->ok()
        && operation.phase
            == talas::app::WorkbenchOperationPhase::Succeeded)
    {
        lastPartialTrackDerivative_ = *result;
        partialTrackDerivativeStatus_ =
            juceString("Derivative ready · verified SHA-256 · explicit import available");
    }
    else if (operation.phase
             == talas::app::WorkbenchOperationPhase::Invalidated
             || result->status
                 == talas::audio::PartialTrackDerivativeStatus::AuthorityChanged)
    {
        partialTrackDerivativeStatus_ =
            juceString("Derivative cancelled because source or edit authority changed · no file published");
    }
    else if (result->status
             == talas::audio::PartialTrackDerivativeStatus::Cancelled)
    {
        partialTrackDerivativeStatus_ =
            juceString("Partial-track derivative cancelled · no file published");
    }
    else
    {
        partialTrackDerivativeStatus_ =
            "Partial-track derivative failed: "
            + juceString(result->message);
    }
    setStatus(partialTrackDerivativeStatus_);
    refreshPartialTrackOperationState();
}

void WorkbenchComponent::importLastPartialTrackDerivative()
{
    finishPartialTrackDerivativeIfReady();
    if (!lastPartialTrackDerivative_.has_value()
        || !lastPartialTrackDerivative_->ok())
    {
        setStatus("No complete verified partial-track derivative is available to import");
        return;
    }
    const auto& derivative = *lastPartialTrackDerivative_;
    auto candidate = projectBridge_.editor().document();
    std::string assetId;
    for (std::size_t ordinal = 1; ordinal <= 4096; ++ordinal)
    {
        const auto proposed = "partial-derivative-"
            + std::to_string(ordinal);
        if (std::ranges::none_of(
                candidate.assets,
                [&](const auto& asset) {
                    return asset.id == proposed;
                }))
        {
            assetId = proposed;
            break;
        }
    }
    if (assetId.empty())
    {
        setStatus("No bounded project asset identity remains for the derivative");
        return;
    }
    talas::project::ProjectAsset manifest;
    manifest.id = assetId;
    manifest.kind = derivative.encoding
            == talas::project::pcm16WavAssetEncoding
        ? talas::project::ProjectAssetKind::Audio
        : talas::project::ProjectAssetKind::PartialSpectrum;
    manifest.path = talas::project::pathToUtf8(derivative.destination);
    manifest.disposition =
        talas::project::ProjectAssetDisposition::Referenced;
    manifest.contentHash = derivative.contentHash;
    manifest.channelCount = manifest.kind
            == talas::project::ProjectAssetKind::Audio
        ? 1U
        : 0U;
    manifest.sampleRate = derivative.sampleRate;
    manifest.frameCount = derivative.frameCount;
    manifest.encoding = derivative.encoding;
    manifest.generation = 1;
    manifest.provenance =
        "Explicit Partial Tracks edited derivative import";
    const auto inspected = talas::project::inspectProjectAsset(
        manifest,
        derivative.destination);
    if (!inspected.ok()
        || inspected.asset.contentHash != derivative.contentHash)
    {
        setStatus(
            "Derivative import rejected because published bytes changed: "
            + juceString(inspected.error.has_value()
                ? inspected.error->message
                : std::string{"content hash mismatch"}));
        return;
    }
    candidate.assets.push_back(inspected.asset);
    const auto applied = projectBridge_.editor().applyDocumentEdit(
        std::move(candidate));
    if (!applied.ok())
    {
        setStatus(
            "Derivative import failed: "
            + juceString(applied.message));
        return;
    }
    lastPartialTrackDerivative_.reset();
    documentChanged();
    partialTrackDerivativeStatus_ =
        "Imported verified derivative as new asset "
        + juceString(assetId);
    setStatus(partialTrackDerivativeStatus_);
    refreshPartialTrackOperationState();
}

std::optional<std::string>
WorkbenchComponent::runPartialTrackOperationSmoke(
    const std::filesystem::path& smokeDirectory,
    const talas::project::PartialTrackAsset& source)
{
    currentDocumentStaged_ = true;
    refreshPartialTrackOperationState();
    const auto sourceInfo =
        partialTrackEditorPanel_->operationSourceInfo();
    if (!sourceInfo.has_value())
        return "Partial Tracks operation source authority was unavailable";
    auto* smokeDevice = deviceManager_.getCurrentAudioDevice();
    const auto expectedPreviewEnabled = smokeDevice != nullptr
        && smokeDevice->getCurrentSampleRate() > 0.0
        && smokeDevice->getCurrentBufferSizeSamples() > 0
        && smokeDevice->getCurrentSampleRate() == source.sampleRate
        && sourceInfo->maximumRenderedSegmentTrackCount
            <= talas::audio::maximumRealtimePartialTrackVoices;
    const auto expectedDerivativeEnabled = !exportRunning_
        && !packageRunning_
        && !fileChooserActive_
        && !fileOperationCoordinator_.snapshot().active();
    if (!partialTrackEditorPanel_->operationControlsMatchForSmoke(
            expectedPreviewEnabled,
            expectedDerivativeEnabled
                && sourceInfo->maximumRenderedSegmentTrackCount
                    <= talas::audio::maximumOfflinePartialTrackVoices,
            expectedDerivativeEnabled))
        return "Partial Tracks preview/render actions did not expose truthful ready state";
    startPartialTrackPreviewWithProfile(
        source.sampleRate,
        64,
        deviceGeneration_.load(std::memory_order_acquire));
    for (std::size_t attempt = 0;
         attempt < 5000
         && !partialTrackPreviewWorkerFinished_.load(
             std::memory_order_acquire);
         ++attempt)
    {
        juce::Thread::sleep(1);
    }
    finishPartialTrackPreviewIfReady();
    if (partialTrackPreviewActive_ == nullptr)
        return "Partial Tracks private preview did not adopt its exact prepared generation";
    std::array<float, 64> previewLeft{};
    std::array<float, 64> previewRight{};
    std::array<float*, 2> previewOutputs{
        previewLeft.data(), previewRight.data()};
    const auto previewRendered = renderPartialTrackPreview(
        previewOutputs.data(), previewOutputs.size(), previewLeft.size());
    if (!previewRendered.value_or(false)
        || previewLeft != previewRight)
    {
        return "Partial Tracks preview callback did not render its preallocated mono generation";
    }
    auto* const retainedPreview = partialTrackPreviewActive_.get();
    startPartialTrackPreviewWithProfile(
        source.sampleRate,
        64,
        deviceGeneration_.load(std::memory_order_acquire));
    for (std::size_t attempt = 0;
         attempt < 5000
         && !partialTrackPreviewWorkerFinished_.load(
             std::memory_order_acquire);
         ++attempt)
    {
        juce::Thread::sleep(1);
    }
    partialTrackPreviewRequestGeneration_.fetch_add(
        1, std::memory_order_acq_rel);
    finishPartialTrackPreviewIfReady();
    if (partialTrackPreviewActive_.get() != retainedPreview)
        return "Cancelled replacement altered the last good Partial Tracks preview";
    stopPartialTrackPreview();

    const auto derivativePath = smokeDirectory
        / "e10-edited-partial-derivative.talaspartial";
    std::error_code removeDerivativeError;
    (void) std::filesystem::remove(
        derivativePath, removeDerivativeError);
    startPartialTrackDerivative(
        talas::audio::PartialTrackDerivativeKind::EditedPartial,
        derivativePath);
    for (std::size_t attempt = 0;
         attempt < 5000
         && !partialTrackDerivativeFinished_.load(
             std::memory_order_acquire);
         ++attempt)
    {
        juce::Thread::sleep(1);
    }
    finishPartialTrackDerivativeIfReady();
    if (!lastPartialTrackDerivative_.has_value()
        || !lastPartialTrackDerivative_->ok()
        || !std::filesystem::is_regular_file(derivativePath))
    {
        return "Partial Tracks edited derivative did not publish through the local coordinator";
    }
    const auto retainedDerivative = *lastPartialTrackDerivative_;
    startPartialTrackDerivative(
        talas::audio::PartialTrackDerivativeKind::EditedPartial,
        derivativePath);
    for (std::size_t attempt = 0;
         attempt < 5000
         && !partialTrackDerivativeFinished_.load(
             std::memory_order_acquire);
         ++attempt)
    {
        juce::Thread::sleep(1);
    }
    finishPartialTrackDerivativeIfReady();
    if (!lastPartialTrackDerivative_.has_value()
        || lastPartialTrackDerivative_->destination
            != retainedDerivative.destination
        || lastPartialTrackDerivative_->contentHash
            != retainedDerivative.contentHash)
    {
        return "Failed replacement render altered the last verified Partial Tracks derivative";
    }
    auto& editor = projectBridge_.editor();
    const auto assetCountBeforeImport = editor.document().assets.size();
    importLastPartialTrackDerivative();
    if (editor.document().assets.size()
            != assetCountBeforeImport + 1
        || lastPartialTrackDerivative_.has_value())
    {
        return "Partial Tracks explicit derivative import did not add exactly one inspected asset";
    }
    return {};
}
}
