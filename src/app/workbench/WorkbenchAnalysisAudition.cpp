#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/audio/WavReader.h"
#include "talas/project/ProjectSoundExpansion.h"

#include <limits>
#include <ranges>
#include <string>

namespace talas::app::workbench
{
void WorkbenchComponent::pinSelectedAnalysisTarget()
{
    const auto* selected = nodeForContext(
        activeGraphContext_,
        selectedNodeId_);
    if (selected == nullptr)
    {
        setStatus("Select a graph node to pin in Analysis");
        return;
    }
    const auto* descriptor = registry_.findNodeType(selected->typeId);
    if (descriptor == nullptr)
    {
        setStatus("Selected node has no available analysis contract");
        return;
    }
    talas::project::WorkspaceAnalysisTraceState trace;
    for (std::size_t ordinal = 1;; ++ordinal)
    {
        const auto candidate = "analysis-"
            + std::to_string(ordinal);
        const auto used = std::ranges::any_of(
            workspaceSession_.analysis.traces,
            [&](const auto& existing) {
                return existing.traceId == candidate;
            });
        if (!used)
        {
            trace.traceId = candidate;
            break;
        }
    }
    trace.nodeId = selectedNodeId_;
    trace.colourSlot = static_cast<std::uint8_t>(
        workspaceSession_.analysis.traces.size()
        % (static_cast<std::size_t>(
               talas::project::maximumWorkspaceAnalysisColourSlot)
            + 1));

    if (activeGraphContext_.has_value())
    {
        const auto expanded = talas::project::expandProjectSounds(
            projectBridge_.editor().document(),
            registry_);
        if (!expanded.ok())
        {
            setStatus("Selected Sound context cannot be resolved for Analysis");
            return;
        }
        std::vector<const talas::project::ExpandedSoundNodeIdentity*>
            matches;
        for (const auto& node : expanded.manifest.nodes)
        {
            if (node.definitionId == *activeGraphContext_
                && node.localNodeId == selectedNodeId_)
            {
                matches.push_back(&node);
            }
        }
        if (matches.size() != 1)
        {
            setStatus(
                matches.empty()
                    ? "Selected Sound node is not reachable in the prepared graph"
                    : "Selected Sound node has several runtime instances; select one exact instance before pinning");
            return;
        }
        trace.instancePath = matches.front()->instancePath;
        trace.containingDefinitionId = *activeGraphContext_;
    }

    if (!talas::app::selectNextAnalysisTargetForNode(
            *descriptor,
            workspaceSession_.analysis,
            trace))
    {
        setStatus(
            "Selected node has no prepared Audio, Control, Event, PartialSpectrumV1, or ComplexSpectrumV1 target to pin");
        return;
    }

    if (!talas::app::addAnalysisTrace(
            projectBridge_.editor().document(),
            registry_,
            workspaceSession_.analysis,
            std::move(trace)))
    {
        setStatus("Analysis target is already pinned or the 16-trace limit was reached");
        return;
    }
    refreshAnalysisWorkspace();
    noteWorkspaceSessionChanged();
    if (currentDocumentStaged_)
        restageAnalysisCaptureIfActive();
    else
        setStatus(juceString(
            "Analysis target pinned · press Play to prepare capture"));
}

void WorkbenchComponent::removeAnalysisTrace(std::string traceId)
{
    if (!talas::app::removeAnalysisTrace(
            workspaceSession_.analysis,
            traceId))
    {
        return;
    }
    refreshAnalysisWorkspace();
    noteWorkspaceSessionChanged();
    if (currentDocumentStaged_)
        restageAnalysisCaptureIfActive();
    else
        setStatus("Analysis trace unpinned");
}

void WorkbenchComponent::selectAnalysisTrace(std::string traceId)
{
    if (!talas::app::selectAnalysisTrace(
            workspaceSession_.analysis,
            traceId))
    {
        return;
    }
    refreshAnalysisWorkspace();
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::moveAnalysisTrace(
    std::string traceId,
    int direction)
{
    const auto found = std::ranges::find(
        workspaceSession_.analysis.traces,
        traceId,
        &talas::project::WorkspaceAnalysisTraceState::traceId);
    if (found == workspaceSession_.analysis.traces.end())
        return;
    const auto sourceIndex = static_cast<std::size_t>(
        std::distance(
            workspaceSession_.analysis.traces.begin(), found));
    if ((direction < 0 && sourceIndex == 0)
        || (direction > 0
            && sourceIndex + 1
                >= workspaceSession_.analysis.traces.size())
        || direction == 0)
    {
        return;
    }
    const auto destinationIndex = direction < 0
        ? sourceIndex - 1
        : sourceIndex + 1;
    if (!talas::app::moveAnalysisTrace(
            workspaceSession_.analysis,
            traceId,
            destinationIndex))
    {
        return;
    }
    refreshAnalysisWorkspace();
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::setAnalysisTraceVisible(
    std::string traceId,
    bool visible)
{
    if (!talas::app::setAnalysisTraceVisible(
            workspaceSession_.analysis,
            traceId,
            visible))
    {
        return;
    }
    refreshAnalysisWorkspace();
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::setAnalysisTraceScale(
    std::string traceId,
    double scale)
{
    if (!talas::app::setAnalysisTraceVerticalScale(
            workspaceSession_.analysis,
            traceId,
            scale))
    {
        return;
    }
    refreshAnalysisWorkspace();
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::setAnalysisRefreshRateHz(
    std::uint32_t refreshRateHz)
{
    if (!talas::app::setAnalysisRefreshRateHz(
            workspaceSession_.analysis,
            refreshRateHz))
    {
        return;
    }
    refreshAnalysisWorkspace();
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::setAnalysisHistoryFrames(
    std::uint32_t historyFrames)
{
    if (!talas::app::setAnalysisHistoryFrames(
            workspaceSession_.analysis,
            historyFrames))
    {
        return;
    }
    refreshAnalysisWorkspace();
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::setAnalysisFrozen(bool frozen)
{
    if (workspaceSession_.analysis.frozen == frozen)
        return;
    workspaceSession_.analysis.frozen = frozen;
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::startAnalysisAudition(
    talas::project::WorkspaceAnalysisAuditionState request)
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr
        || device->getCurrentSampleRate() <= 0.0
        || device->getCurrentBufferSizeSamples() <= 0)
    {
        setStatus("Analysis audition requires an active audio device");
        return;
    }
    startAnalysisAuditionWithProfile(
        std::move(request),
        device->getCurrentSampleRate(),
        static_cast<std::size_t>(
            device->getCurrentBufferSizeSamples()),
        deviceGeneration_.load(std::memory_order_acquire));
}

void WorkbenchComponent::startAnalysisAuditionWithProfile(
    talas::project::WorkspaceAnalysisAuditionState request,
    double sampleRate,
    std::size_t maxBlockFrames,
    std::uint64_t sourceDeviceGeneration)
{
    stopPartialTrackPreview();
    if (analysisAuditionWorkerFinished_.load(std::memory_order_acquire)
        && analysisAuditionThread_.joinable())
    {
        finishAnalysisAuditionIfReady();
    }
    retireAnalysisAuditionIfQuiescent();
    if (!analysisAuditionWorkerFinished_.load(std::memory_order_acquire)
        || analysisAuditionActive_ != nullptr
        || analysisAuditionRetiring_ != nullptr)
    {
        setStatus("Stop the current Analysis audition before starting another");
        return;
    }
    if (!currentDocumentStaged_)
    {
        setStatus("Analysis audition requires the current exact prepared graph");
        return;
    }
    if (usesExternalMidiClock() || usesExternalMtc())
    {
        setStatus(
            "Analysis audition unavailable while external transport controls playback");
        return;
    }
    const auto trace = std::ranges::find(
        workspaceSession_.analysis.traces,
        request.traceId,
        &talas::project::WorkspaceAnalysisTraceState::traceId);
    if (trace == workspaceSession_.analysis.traces.end())
    {
        setStatus("Select one retained Analysis trace before auditioning");
        return;
    }
    if (!std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || maxBlockFrames == 0)
    {
        setStatus("Analysis audition requires a valid prepared device profile");
        return;
    }

    const auto sourceDocument = projectBridge_.editor().document();
    const auto sourceDocumentBytes =
        talas::project::serializeProjectDocument(sourceDocument);
    const auto sourceProjectPath = projectPath();
    const auto sourceStageGeneration =
        projectBridge_.latestStageGeneration();
    const auto sourceDocumentGeneration =
        uiDocumentGeneration_.load(std::memory_order_acquire);
    const auto requestGeneration =
        analysisAuditionRequestGeneration_.fetch_add(
            1, std::memory_order_acq_rel)
        + 1;
    workspaceSession_.analysis.audition = request;
    noteWorkspaceSessionChanged();
    analysisAuditionWorkerFinished_.store(false, std::memory_order_release);
    analysisAuditionStatus_ = "Preparing exact Analysis audition...";
    setStatus(analysisAuditionStatus_);
    refreshAnalysisWorkspace();

    try
    {
        analysisAuditionThread_ = std::jthread(
            [this,
             sourceDocument,
             sourceDocumentBytes,
             sourceProjectPath,
             trace = *trace,
             request = std::move(request),
             sampleRate,
             maxBlockFrames,
             sourceDocumentGeneration,
             sourceStageGeneration,
             sourceDeviceGeneration,
             requestGeneration](std::stop_token stopToken) mutable {
            AnalysisAuditionPublication publication;
            publication.requestGeneration = requestGeneration;
            publication.sourceDocumentGeneration = sourceDocumentGeneration;
            publication.sourceDocumentBytes = sourceDocumentBytes;
            try
            {
                const auto beforeAssets =
                    talas::audio::captureProjectAssetWatchSnapshot(
                        sourceDocument, registry_, sourceProjectPath);
                auto prepared = talas::app::prepareAnalysisAudition(
                    sourceDocument, registry_, trace, request);
                if (!prepared.ok())
                {
                    publication.diagnostic = prepared.diagnostic.empty()
                        ? "Analysis audition overlay preparation failed"
                        : prepared.diagnostic;
                }
                else if (stopToken.stop_requested())
                {
                    publication.cancelled = true;
                }
                else
                {
                    talas::runtime::AnalysisTestInputPreparation inputPreparation;
                    inputPreparation.kind = request.source;
                    inputPreparation.sampleRate = sampleRate;
                    inputPreparation.sineFrequencyHz = request.sineFrequencyHz;
                    if (request.source
                            == talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset
                        && request.frozenAssetId.has_value())
                    {
                        const auto asset = std::ranges::find(
                            sourceDocument.assets,
                            *request.frozenAssetId,
                            &talas::project::ProjectAsset::id);
                        if (asset == sourceDocument.assets.end()
                            || asset->kind != talas::project::ProjectAssetKind::Audio)
                        {
                            publication.diagnostic =
                                "Selected frozen Analysis asset is unavailable";
                        }
                        else
                        {
                            const auto assetPath =
                                talas::audio::resolveProjectAssetPath(
                                    *asset, sourceProjectPath);
                            auto wav = talas::audio::readPcm16Wav(
                                assetPath,
                                [&stopToken] {
                                    return stopToken.stop_requested();
                                });
                            if (!wav.ok())
                            {
                                publication.cancelled = wav.cancelled;
                                if (!wav.cancelled)
                                {
                                    publication.diagnostic =
                                        "Frozen Analysis asset load failed: "
                                        + wav.error;
                                }
                            }
                            else if (!asset->contentHash.empty()
                                     && asset->contentHash != wav.contentHash)
                            {
                                publication.diagnostic =
                                    "Frozen Analysis asset digest changed before preparation";
                            }
                            else if (wav.channels.empty()
                                     || wav.channels.size() > 8
                                     || wav.channels.front().empty()
                                     || wav.sampleRate == 0)
                            {
                                publication.diagnostic =
                                    "Frozen Analysis asset has no supported audio channels";
                            }
                            else
                            {
                                const auto inputFrames =
                                    wav.channels.front().size();
                                const auto outputFramesDouble =
                                    static_cast<double>(inputFrames)
                                    * sampleRate
                                    / static_cast<double>(wav.sampleRate);
                                const auto outputFrames =
                                    outputFramesDouble
                                            > static_cast<double>(
                                                talas::runtime::maximumAnalysisFrozenAssetFrames)
                                        ? talas::runtime::maximumAnalysisFrozenAssetFrames + 1
                                        : static_cast<std::size_t>(
                                            std::ceil(outputFramesDouble));
                                if (outputFrames == 0
                                    || outputFrames
                                        > talas::runtime::maximumAnalysisFrozenAssetFrames
                                    || std::ranges::any_of(
                                        wav.channels,
                                        [&](const auto& channel) {
                                            return channel.size() != inputFrames;
                                        }))
                                {
                                    publication.diagnostic =
                                        "Frozen Analysis asset exceeds the bounded decoded profile";
                                }
                                else
                                {
                                    inputPreparation.frozenMonoSamples.resize(
                                        outputFrames);
                                    for (std::size_t frame = 0;
                                         frame < outputFrames;
                                         ++frame)
                                    {
                                        if (stopToken.stop_requested())
                                        {
                                            publication.cancelled = true;
                                            break;
                                        }
                                        const auto sourcePosition =
                                            std::min(
                                                static_cast<double>(inputFrames - 1),
                                                static_cast<double>(frame)
                                                    * static_cast<double>(wav.sampleRate)
                                                    / sampleRate);
                                        const auto left = std::min(
                                            static_cast<std::size_t>(sourcePosition),
                                            inputFrames - 1);
                                        const auto right = std::min(
                                            left + 1, inputFrames - 1);
                                        const auto fraction =
                                            sourcePosition
                                            - static_cast<double>(left);
                                        double mono = 0.0;
                                        for (const auto& channel : wav.channels)
                                        {
                                            mono += std::lerp(
                                                channel[left],
                                                channel[right],
                                                fraction);
                                        }
                                        inputPreparation.frozenMonoSamples[frame] =
                                            static_cast<float>(
                                                mono
                                                / static_cast<double>(
                                                    wav.channels.size()));
                                    }
                                }
                            }
                        }
                    }

                    if (publication.diagnostic.empty()
                        && !publication.cancelled)
                    {
                        auto runtime =
                            std::make_unique<AnalysisAuditionRuntime>();
                        std::string sourceDiagnostic;
                        if (!talas::runtime::AnalysisTestInputSource::prepare(
                                std::move(inputPreparation),
                                runtime->testSource,
                                sourceDiagnostic))
                        {
                            publication.diagnostic =
                                std::move(sourceDiagnostic);
                        }
                        else
                        {
                            runtime->testInputScratch.resize(maxBlockFrames);
                            runtime->preparation = std::move(prepared);
                            runtime->sourceDocumentBytes = sourceDocumentBytes;
                            runtime->requestGeneration = requestGeneration;
                            runtime->sourceDocumentGeneration =
                                sourceDocumentGeneration;
                            runtime->sourceStageGeneration = sourceStageGeneration;
                            runtime->sourceDeviceGeneration = sourceDeviceGeneration;
                            runtime->usesTemporarySource = request.source
                                != talas::project::WorkspaceAnalysisTestSourceKind::None;
                            talas::runtime::RenderSettings settings;
                            settings.sampleRate = sampleRate;
                            settings.maxBlockFrames = maxBlockFrames;
                            auto loaded =
                                talas::audio::loadReferencedProjectResourceSnapshots(
                                    *runtime->preparation.overlayDocument,
                                    registry_,
                                    sourceProjectPath,
                                    {},
                                    [&stopToken] {
                                        return stopToken.stop_requested();
                                    });
                            if (!loaded.ok())
                            {
                                publication.cancelled = loaded.wasCancelled;
                                if (!loaded.wasCancelled)
                                {
                                    publication.diagnostic =
                                        loaded.failure.has_value()
                                        ? "Audition asset load failed: "
                                            + loaded.failure->message
                                        : "Audition asset load failed";
                                }
                            }
                            else
                            {
                                settings.resourceSnapshots =
                                    std::move(loaded.snapshots);
                                runtime->bridge = std::make_unique<
                                    talas::runtime::ProjectRuntimeBridge>(
                                        *runtime->preparation.overlayDocument,
                                        registry_);
                                const auto staged =
                                    runtime->bridge->compileAndStageDocument(
                                        *runtime->preparation.overlayDocument,
                                        std::move(settings));
                                if (!staged.ok())
                                {
                                    publication.diagnostic =
                                        staged.message.empty()
                                        ? "Audition graph did not stage"
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
                publication.verifiedAssetWatch =
                    talas::audio::captureProjectAssetWatchSnapshot(
                        sourceDocument, registry_, sourceProjectPath);
                if (publication.diagnostic.empty()
                    && !publication.cancelled
                    && talas::audio::findProjectAssetWatchChange(
                           beforeAssets,
                           publication.verifiedAssetWatch)
                        .has_value())
                {
                    publication.runtime.reset();
                    publication.diagnostic =
                        "Project asset changed while preparing Analysis audition";
                }
            }
            catch (const std::bad_alloc&)
            {
                publication.runtime.reset();
                publication.diagnostic =
                    "Analysis audition allocation failed";
            }
            catch (const std::length_error&)
            {
                publication.runtime.reset();
                publication.diagnostic =
                    "Analysis audition exceeded a bounded collection limit";
            }
            catch (const std::exception& error)
            {
                publication.runtime.reset();
                publication.diagnostic =
                    "Analysis audition preparation failed: "
                    + std::string{error.what()};
            }
            {
                const std::scoped_lock lock{analysisAuditionResultMutex_};
                analysisAuditionPublication_ = std::move(publication);
            }
            analysisAuditionWorkerFinished_.store(
                true, std::memory_order_release);
            });
    }
    catch (const std::exception& error)
    {
        analysisAuditionWorkerFinished_.store(true, std::memory_order_release);
        workspaceSession_.analysis.audition.reset();
        noteWorkspaceSessionChanged();
        analysisAuditionStatus_ =
            "Analysis audition worker failed to start: "
            + juceString(error.what());
        setStatus(analysisAuditionStatus_);
        refreshAnalysisWorkspace();
    }
}

void WorkbenchComponent::stopAnalysisAudition()
{
    const auto hadWorkspaceRequest =
        workspaceSession_.analysis.audition.has_value();
    const auto hadAudition = hadWorkspaceRequest
        || analysisAuditionThread_.joinable()
        || analysisAuditionActive_ != nullptr
        || analysisAuditionRetiring_ != nullptr;
    if (!hadAudition)
        return;
    analysisAuditionRequestGeneration_.fetch_add(
        1, std::memory_order_acq_rel);
    if (analysisAuditionThread_.joinable()
        && !analysisAuditionWorkerFinished_.load(std::memory_order_acquire))
    {
        analysisAuditionThread_.request_stop();
        analysisAuditionStatus_ = "Cancelling Analysis audition...";
        setStatus(analysisAuditionStatus_);
    }
    analysisAuditionLifecycle_.store(0, std::memory_order_seq_cst);
    analysisAuditionCallback_.store(nullptr, std::memory_order_release);
    if (analysisAuditionActive_ != nullptr)
    {
        analysisAuditionRetiring_ = std::move(analysisAuditionActive_);
        analysisAuditionStatus_ = "Analysis audition stopped";
        setStatus(analysisAuditionStatus_);
    }
    if (hadWorkspaceRequest)
    {
        workspaceSession_.analysis.audition.reset();
        noteWorkspaceSessionChanged();
    }
    retireAnalysisAuditionIfQuiescent();
    refreshAnalysisWorkspace();
}

void WorkbenchComponent::commitAnalysisAudition()
{
    if (analysisAuditionActive_ == nullptr
        || !analysisAuditionActive_->preparation
                .portableCommitDocument.has_value())
    {
        setStatus("Current Analysis source is ephemeral and cannot be committed");
        return;
    }
    const auto sourceStillExact =
        analysisAuditionActive_->sourceDocumentBytes
            == talas::project::serializeProjectDocument(
                projectBridge_.editor().document())
        && analysisAuditionActive_->sourceDocumentGeneration
            == uiDocumentGeneration_.load(std::memory_order_acquire)
        && analysisAuditionActive_->sourceStageGeneration
            == projectBridge_.latestStageGeneration()
        && analysisAuditionActive_->sourceDeviceGeneration
            == deviceGeneration_.load(std::memory_order_acquire);
    if (!sourceStillExact)
    {
        stopAnalysisAudition();
        setStatus(
            "Analysis Commit cancelled because its document, stage, or device authority changed");
        return;
    }
    auto commitDocument =
        *analysisAuditionActive_->preparation.portableCommitDocument;
    stopAnalysisAudition();
    const auto committed = projectBridge_.editor().applyDocumentEdit(
        std::move(commitDocument));
    if (!committed.ok())
    {
        setStatus(
            "Analysis Commit failed: "
            + juceString(committed.message));
        return;
    }
    documentChanged();
    setStatus("Analysis test source committed as one project edit");
}

void WorkbenchComponent::finishAnalysisAuditionIfReady()
{
    retireAnalysisAuditionIfQuiescent();
    if (!analysisAuditionWorkerFinished_.load(std::memory_order_acquire))
        return;
    if (analysisAuditionThread_.joinable())
        analysisAuditionThread_.join();
    std::optional<AnalysisAuditionPublication> publication;
    {
        const std::scoped_lock lock{analysisAuditionResultMutex_};
        publication = std::move(analysisAuditionPublication_);
        analysisAuditionPublication_.reset();
    }
    if (!publication.has_value())
        return;
    const auto currentGeneration =
        analysisAuditionRequestGeneration_.load(std::memory_order_acquire);
    if (publication->cancelled
        || publication->requestGeneration != currentGeneration)
    {
        analysisAuditionStatus_ = "Analysis audition cancelled";
        setStatus(analysisAuditionStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    if (publication->runtime == nullptr)
    {
        analysisAuditionStatus_ =
            juceString(publication->diagnostic.empty()
                    ? "Analysis audition preparation failed"
                    : publication->diagnostic);
        workspaceSession_.analysis.audition.reset();
        noteWorkspaceSessionChanged();
        setStatus(analysisAuditionStatus_);
        refreshAnalysisWorkspace();
        return;
    }
    const auto sourceStillExact =
        publication->sourceDocumentBytes
            == talas::project::serializeProjectDocument(
                projectBridge_.editor().document())
        && publication->sourceDocumentGeneration
            == uiDocumentGeneration_.load(std::memory_order_acquire)
        && publication->runtime->sourceStageGeneration
            == projectBridge_.latestStageGeneration()
        && publication->runtime->sourceDeviceGeneration
            == deviceGeneration_.load(std::memory_order_acquire)
        && !talas::audio::findProjectAssetWatchChange(
                publication->verifiedAssetWatch,
                talas::audio::captureProjectAssetWatchSnapshot(
                    projectBridge_.editor().document(),
                    registry_,
                    projectPath()))
                .has_value();
    if (!sourceStillExact)
    {
        analysisAuditionStatus_ =
            "Analysis audition cancelled because project, stage, asset, or device authority changed";
        workspaceSession_.analysis.audition.reset();
        noteWorkspaceSessionChanged();
        setStatus(analysisAuditionStatus_);
        refreshAnalysisWorkspace();
        return;
    }

    analysisAuditionActive_ = std::move(publication->runtime);
    analysisAuditionCallback_.store(
        analysisAuditionActive_.get(), std::memory_order_release);
    analysisAuditionLifecycle_.store(1, std::memory_order_seq_cst);
    analysisAuditionStatus_ =
        juceString("Audition active · ")
        + juceString(analysisAuditionActive_->preparation.sourceLabel)
        + " → "
        + juceString(analysisAuditionActive_->preparation.destinationLabel);
    setStatus(analysisAuditionStatus_);
    refreshAnalysisWorkspace();
}

void WorkbenchComponent::retireAnalysisAuditionIfQuiescent()
{
    if (analysisAuditionRetiring_ != nullptr
        && analysisAuditionAudioOperations_.load(
               std::memory_order_seq_cst)
            == 0)
    {
        analysisAuditionRetiring_.reset();
    }
}

std::optional<bool> WorkbenchComponent::renderAnalysisAudition(
    std::array<const float*, 8> physicalInputChannels,
    double callbackTimeSeconds,
    float* const* outputChannels,
    std::size_t outputChannelCount,
    std::size_t frameCount) noexcept
{
    if (analysisAuditionLifecycle_.load(std::memory_order_seq_cst) != 1)
        return std::nullopt;
    analysisAuditionAudioOperations_.fetch_add(
        1, std::memory_order_seq_cst);
    if (analysisAuditionLifecycle_.load(std::memory_order_seq_cst) != 1)
    {
        analysisAuditionAudioOperations_.fetch_sub(
            1, std::memory_order_seq_cst);
        return std::nullopt;
    }
    auto* runtime = analysisAuditionCallback_.load(
        std::memory_order_acquire);
    bool rendered = false;
    if (runtime != nullptr && runtime->bridge != nullptr)
    {
        if (runtime->usesTemporarySource)
        {
            if (frameCount <= runtime->testInputScratch.size())
            {
                runtime->testSource.render(
                    std::span<float>{
                        runtime->testInputScratch.data(), frameCount});
                physicalInputChannels.fill(nullptr);
                physicalInputChannels.front() =
                    runtime->testInputScratch.data();
                rendered = runtime->bridge->renderOrdered(
                    {physicalInputChannels.data(),
                     physicalInputChannels.size()},
                    callbackTimeSeconds,
                    outputChannels,
                    outputChannelCount,
                    frameCount);
            }
        }
        else
        {
            rendered = runtime->bridge->renderOrdered(
                {physicalInputChannels.data(),
                 physicalInputChannels.size()},
                callbackTimeSeconds,
                outputChannels,
                outputChannelCount,
                frameCount);
        }
    }
    analysisAuditionAudioOperations_.fetch_sub(
        1, std::memory_order_seq_cst);
    return rendered;
}

void WorkbenchComponent::restageAnalysisCaptureIfActive()
{
    if (!currentDocumentStaged_)
        return;
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr
        || device->getCurrentSampleRate() <= 0.0
        || device->getCurrentBufferSizeSamples() <= 0)
    {
        setStatus("Analysis layout changed; capture will update on the next Play");
        currentDocumentStaged_ = false;
        return;
    }
    talas::runtime::RenderSettings settings;
    settings.sampleRate = device->getCurrentSampleRate();
    settings.maxBlockFrames = static_cast<std::size_t>(
        device->getCurrentBufferSizeSamples());
    const auto wasPlaying = transportPlaying_;
    if (stageProject(std::move(settings), device->getName())
        && wasPlaying)
    {
        setPlaying(true);
    }
}

// MIDI pitch bend scrubs the spectrum shown: the Synchronizing Spectra
// tool's active view while its window is open, else the spectrum editor
// while it shows; the Sound playing still takes the bend. Called on the
// MIDI thread, it acts on the message thread.
void WorkbenchComponent::scrubSpectraForPitchBend(int bend)
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis, bend] {
        if (safeThis != nullptr)
            (void) safeThis->scrubShownSpectrum(bend);
    });
}

bool WorkbenchComponent::scrubShownSpectrum(int bend)
{
    if (synchronizingSpectraPanel_ != nullptr && synchronizingSpectraWindow_ != nullptr
        && synchronizingSpectraWindow_->isVisible() && synchronizingSpectraPanel_->scrubForPitchBend(bend))
        return true;
    return partialTrackEditorPanel_ != nullptr && partialTrackEditorPanel_->isShowing()
        && partialTrackEditorPanel_->scrubForPitchBend(bend).has_value();
}
}
