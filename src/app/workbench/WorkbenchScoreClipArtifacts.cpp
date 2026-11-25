#include "app/workbench/WorkbenchComponentInternal.h"
#include "talas/app/TimelineInfo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <system_error>

namespace talas::app::workbench
{
namespace
{
[[nodiscard]] std::string boundedArtifactToken(std::string_view value)
{
    std::string result;
    result.reserve(std::min<std::size_t>(value.size(), 96));
    for (const auto character : value)
    {
        if (result.size() == 96)
            break;
        const auto accepted = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '_';
        result.push_back(accepted ? character : '_');
    }
    return result.empty() ? std::string{"clip"} : result;
}

[[nodiscard]] bool artifactAssetShapeMatches(
    const talas::project::ProjectAsset& asset,
    const talas::project::ScoreClipRenderArtifact& artifact) noexcept
{
    return asset.kind == talas::project::ProjectAssetKind::Audio
        && asset.channelCount == 1
        && asset.sampleRate == static_cast<double>(artifact.sampleRate)
        && asset.frameCount == artifact.frameCount
        && (asset.encoding == talas::project::pcm16WavAssetEncoding
            || asset.encoding == talas::project::sampleFileAssetEncoding)
        && talas::project::isCanonicalProjectAssetContentHash(asset.contentHash)
        && asset.generation != 0;
}
}

TimelineScoreClipArtifactState WorkbenchComponent::scoreClipArtifactState(
    std::string_view clipId)
{
    if (scoreClipArtifactRunning_
        && scoreClipArtifactActiveClipId_ == clipId
        && scoreClipArtifactToken_.has_value())
    {
        const auto operation = fileOperationCoordinator_.snapshot();
        TimelineScoreClipArtifactState state;
        state.progressPermille = operation.progressPermille;
        if (operation.phase == talas::app::WorkbenchOperationPhase::Cancelling
            || operation.phase == talas::app::WorkbenchOperationPhase::Invalidated)
        {
            state.kind = TimelineScoreClipArtifactStateKind::Cancelling;
            state.label = "Cancelling";
            state.detail = "Cancelling clip artifact operation";
        }
        else if (scoreClipArtifactActiveMode_
                 == talas::project::ScoreClipRenderMode::Frozen)
        {
            state.kind = TimelineScoreClipArtifactStateKind::Freezing;
            state.label = "Freezing "
                + std::to_string(operation.progressPermille / 10U) + "%";
            state.detail = "Freezing exact clip audio · "
                + std::to_string(operation.progressPermille / 10U) + "%";
        }
        else
        {
            state.kind = TimelineScoreClipArtifactStateKind::RenderingCache;
            state.label = "Caching "
                + std::to_string(operation.progressPermille / 10U) + "%";
            state.detail = "Rendering exact clip cache · "
                + std::to_string(operation.progressPermille / 10U) + "%";
        }
        return state;
    }

    if (const auto cached = scoreClipArtifactStateCache_.find(
            std::string{clipId});
        cached != scoreClipArtifactStateCache_.end())
    {
        return cached->second;
    }

    TimelineScoreClipArtifactState state;
    const auto& document = projectBridge_.editor().document();
    const auto clip = std::ranges::find(
        document.timeline.score.clips,
        clipId,
        &talas::project::ScoreClip::clipId);
    if (clip == document.timeline.score.clips.end())
    {
        state.kind = TimelineScoreClipArtifactStateKind::Invalid;
        state.label = "Artifact invalid";
        state.detail = "Score clip identity is no longer available";
        state.sourceAvailable = false;
        return state;
    }
    std::string fingerprintError;
    const auto fingerprint = talas::runtime::scoreClipSourceFingerprint(
        document,
        registry_,
        clip->clipId,
        static_cast<std::uint32_t>(defaultExportSampleRate()),
        fingerprintError);
    state.sourceAvailable = !fingerprint.empty();
    if (!clip->renderArtifact.has_value())
    {
        if (state.sourceAvailable)
        {
            state.kind = TimelineScoreClipArtifactStateKind::Live;
            state.label = "Live";
            state.detail = "Live source · cache or freeze available after saving the project";
        }
        else
        {
            state.kind = TimelineScoreClipArtifactStateKind::Invalid;
            state.label = "Source unavailable";
            state.detail = fingerprintError.empty()
                ? "Clip source is unavailable for cache or freeze"
                : fingerprintError;
        }
        scoreClipArtifactStateCache_.insert_or_assign(
            clip->clipId, state);
        return state;
    }

    const auto& artifact = *clip->renderArtifact;
    const auto currentSampleRate = defaultExportSampleRate();
    if (!std::isfinite(currentSampleRate)
        || currentSampleRate <= 0.0
        || currentSampleRate
            != static_cast<double>(artifact.sampleRate))
    {
        state.kind = TimelineScoreClipArtifactStateKind::Invalid;
        state.label = "Artifact invalid";
        state.detail = "Artifact sample rate does not match the current render authority";
        scoreClipArtifactStateCache_.insert_or_assign(clip->clipId, state);
        return state;
    }
    const auto& recoveryState = assetRecoveryCoordinator_.state();
    for (const auto& assetId : artifact.channelAssetIds)
    {
        const auto asset = std::ranges::find(
            document.assets, assetId, &talas::project::ProjectAsset::id);
        if (asset == document.assets.end())
        {
            state.kind = TimelineScoreClipArtifactStateKind::Missing;
            state.label = "Artifact missing";
            state.detail = "Artifact manifest is missing channel asset " + assetId;
            scoreClipArtifactStateCache_.insert_or_assign(clip->clipId, state);
            return state;
        }
        if (!artifactAssetShapeMatches(*asset, artifact))
        {
            state.kind = TimelineScoreClipArtifactStateKind::Invalid;
            state.label = "Artifact invalid";
            state.detail = "Artifact channel metadata is invalid for " + assetId;
            scoreClipArtifactStateCache_.insert_or_assign(clip->clipId, state);
            return state;
        }
        const auto recovery = std::ranges::find(
            recoveryState.items,
            assetId,
            [](const auto& item) -> const std::string& {
                return item.manifest.id;
            });
        if (recovery != recoveryState.items.end())
        {
            state.kind = TimelineScoreClipArtifactStateKind::Invalid;
            state.label = "Artifact invalid";
            state.detail = recovery->message.empty()
                ? "Artifact channel requires asset recovery for " + assetId
                : recovery->message;
            scoreClipArtifactStateCache_.insert_or_assign(clip->clipId, state);
            return state;
        }
        const auto resolved = talas::project::resolveProjectAssetPath(
            *asset, projectPath());
        std::error_code statusError;
        const auto status = std::filesystem::symlink_status(
            resolved, statusError);
        if (statusError
            || status.type() != std::filesystem::file_type::regular)
        {
            state.kind = TimelineScoreClipArtifactStateKind::Missing;
            state.label = "Artifact missing";
            state.detail = "Artifact channel bytes are unavailable for " + assetId;
            scoreClipArtifactStateCache_.insert_or_assign(clip->clipId, state);
            return state;
        }
    }
    if (artifact.mode == talas::project::ScoreClipRenderMode::Frozen)
    {
        state.kind = TimelineScoreClipArtifactStateKind::Frozen;
        state.label = "Frozen";
        state.detail = "Frozen verified artifact · source changes do not replace these bytes";
        scoreClipArtifactStateCache_.insert_or_assign(clip->clipId, state);
        return state;
    }

    if (fingerprint.empty())
    {
        state.kind = TimelineScoreClipArtifactStateKind::Invalid;
        state.label = "Artifact invalid";
        state.detail = fingerprintError.empty()
            ? "Artifact dependency fingerprint is unavailable"
            : fingerprintError;
    }
    else if (fingerprint == artifact.sourceFingerprint)
    {
        state.kind = TimelineScoreClipArtifactStateKind::Cached;
        state.label = "Cached";
        state.detail = "Cached artifact matches the exact current source dependencies";
    }
    else
    {
        state.kind = TimelineScoreClipArtifactStateKind::CacheStale;
        state.label = "Cache stale";
        state.detail = "Cached artifact is retained but live source audio will be used until refresh";
    }
    scoreClipArtifactStateCache_.insert_or_assign(clip->clipId, state);
    return state;
}

void WorkbenchComponent::performScoreClipArtifactAction(
    std::string clipId,
    TimelineScoreClipArtifactAction action)
{
    const auto& clips = projectBridge_.editor().document().timeline.score.clips;
    const auto clip = std::ranges::find(
        clips, clipId, &talas::project::ScoreClip::clipId);
    if (clip == clips.end())
    {
        setStatus("Clip artifact action cancelled because its target changed");
        return;
    }
    if (action == TimelineScoreClipArtifactAction::Cancel)
    {
        cancelScoreClipArtifactPublication();
        return;
    }
    if (action == TimelineScoreClipArtifactAction::Live)
    {
        if (!clip->renderArtifact.has_value())
        {
            setStatus("The Sound already plays live");
            return;
        }
        if (const auto result = projectBridge_.editor().clearScoreClipRenderArtifact(clipId); !result.ok())
        {
            setStatus("Live failed: " + juceString(result.message));
            return;
        }
        documentChanged();
        setStatus("The Sound plays live again; its cache files stay on disk");
        return;
    }
    if (action == TimelineScoreClipArtifactAction::Unfreeze)
    {
        if (!clip->renderArtifact.has_value()
            || clip->renderArtifact->mode
                != talas::project::ScoreClipRenderMode::Frozen)
        {
            setStatus("Unfreeze is no longer available for this clip");
            return;
        }
        const auto result = projectBridge_.editor()
            .clearScoreClipRenderArtifact(clipId);
        if (!result.ok())
        {
            setStatus("Unfreeze failed: " + juceString(result.message));
            return;
        }
        documentChanged();
        setStatus("Unfroze clip; retained immutable artifact files are recoverable and no longer authoritative");
        return;
    }
    const auto state = scoreClipArtifactState(clipId);
    if (!state.sourceAvailable)
    {
        setStatus("Clip artifact action unavailable: "
            + juceString(state.detail));
        return;
    }
    startScoreClipArtifactPublication(
        std::move(clipId),
        action == TimelineScoreClipArtifactAction::Freeze
            ? talas::project::ScoreClipRenderMode::Frozen
            : talas::project::ScoreClipRenderMode::Cached);
}

void WorkbenchComponent::startScoreClipArtifactPublication(
    std::string clipId,
    talas::project::ScoreClipRenderMode mode)
{
    if (scoreClipArtifactRunning_)
    {
        setStatus("A clip artifact operation is already running");
        return;
    }
    if (!projectPath().has_value() || projectPath()->empty())
    {
        setStatus("Save the project before caching or freezing Score clips");
        return;
    }
    std::string sourceError;
    if (talas::runtime::scoreClipSourceFingerprint(
            projectBridge_.editor().document(),
            registry_,
            clipId,
            static_cast<std::uint32_t>(defaultExportSampleRate()),
            sourceError).empty())
    {
        setStatus("Clip artifact action unavailable: "
            + juceString(sourceError.empty()
                ? std::string{"clip source is unavailable"}
                : sourceError));
        return;
    }
    if (scoreClipArtifactThread_.joinable())
        scoreClipArtifactThread_.join();
    const auto operation = fileOperationCoordinator_.begin(
        talas::app::WorkbenchOperationKind::CacheFreeze);
    if (!operation.has_value())
    {
        setStatus("Finish the active file operation before caching or freezing a clip");
        return;
    }

    const auto documentAuthorityGeneration =
        projectBridge_.editor().documentAuthorityGeneration();
    const auto deviceGeneration = deviceGeneration_.load(
        std::memory_order_acquire);
    std::string expectedDocument;
    talas::project::ProjectDocument document;
    std::filesystem::path sourceProjectPath;
    std::filesystem::path destination;
    std::string storedPrefix;
    std::string assetPrefix;
    double sampleRate = 0.0;
    std::size_t maximumBlockFrames = 256;
    try
    {
        expectedDocument = currentDocumentSnapshot();
        document = projectBridge_.editor().document();
        sourceProjectPath = *projectPath();
        sampleRate = defaultExportSampleRate();
        if (auto* device = deviceManager_.getCurrentAudioDevice();
            device != nullptr && device->getCurrentBufferSizeSamples() > 0)
        {
            maximumBlockFrames = static_cast<std::size_t>(
                device->getCurrentBufferSizeSamples());
        }
        const auto safeClip = boundedArtifactToken(clipId);
        const auto stamp = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto leaf = safeClip + "-"
            + std::to_string(operation->generation) + "-" + stamp;
        // In the Timeline's own Caches folder.
        destination = std::filesystem::absolute(
            talas::app::timelineCachesFolder(sourceProjectPath) / leaf)
            .lexically_normal();
        storedPrefix = talas::project::projectAssetStoredPath(
            destination, sourceProjectPath);
        assetPrefix = "scoreclip-" + safeClip + "-"
            + std::to_string(operation->generation) + "-" + stamp;
    }
    catch (const std::exception& exception)
    {
        (void) fileOperationCoordinator_.finish(
            *operation, talas::app::WorkbenchOperationOutcome::Failed);
        setStatus("Unable to snapshot clip artifact authority: "
            + juceString(exception.what()));
        return;
    }

    scoreClipArtifactRunning_ = true;
    scoreClipArtifactToken_ = *operation;
    scoreClipArtifactActiveClipId_ = clipId;
    scoreClipArtifactActiveMode_ = mode;
    scoreClipArtifactStateCache_.erase(clipId);
    timelinePanel_->refresh();
    commandManager_.commandStatusChanged();
    setStatus(mode == talas::project::ScoreClipRenderMode::Frozen
        ? "Freezing Score clip... 0%"
        : "Rendering Score clip cache... 0%");

    try
    {
        scoreClipArtifactThread_ = std::thread(
            [operation = *operation,
             operationOwner = this,
             documentAuthorityGeneration,
             deviceGeneration,
             expectedDocument = std::move(expectedDocument),
             document = std::move(document),
             sourceProjectPath,
             destination,
             storedPrefix,
             assetPrefix,
             clipId = std::move(clipId),
             mode,
             sampleRate,
             maximumBlockFrames,
             registry = &registry_]() mutable {
                const auto shouldCancel = [operationOwner,
                                           operation,
                                           documentAuthorityGeneration,
                                           deviceGeneration] {
                    return operationOwner->fileOperationShouldCancel(
                               operation, documentAuthorityGeneration)
                        || operationOwner->deviceGeneration_.load(
                               std::memory_order_acquire)
                            != deviceGeneration;
                };
                auto settings = talas::runtime::prepareScoreClipMaterializationSettings(
                    document,
                    *registry,
                    sourceProjectPath,
                    clipId,
                    sampleRate,
                    maximumBlockFrames,
                    shouldCancel);
                talas::runtime::ScoreClipArtifactPublicationResult result;
                if (!settings.ok())
                {
                    result.cancelled = settings.cancelled || shouldCancel();
                    result.error = result.cancelled
                        ? std::string{}
                        : std::move(settings.error);
                }
                else
                {
                    talas::runtime::ScoreClipArtifactPublicationOptions options;
                    options.clipId = clipId;
                    options.mode = mode;
                    options.destinationDirectory = destination;
                    options.storedPathPrefix = storedPrefix;
                    options.assetIdPrefix = assetPrefix;
                    options.shouldCancel = shouldCancel;
                    options.reportProgress = [operationOwner,
                                              operation](double value) {
                        (void) operationOwner->fileOperationCoordinator_
                            .publishProgress(operation, value);
                    };
                    options.admitPublication = [operationOwner,
                                                operation,
                                                documentAuthorityGeneration,
                                                deviceGeneration] {
                        return operationOwner->deviceGeneration_.load(
                                   std::memory_order_acquire)
                                == deviceGeneration
                            && operationOwner->admitFileOperationPublication(
                                operation, documentAuthorityGeneration);
                    };
                    result = talas::runtime::publishScoreClipArtifact(
                        document,
                        *registry,
                        *settings.settings,
                        std::move(options));
                }
                std::lock_guard lock{
                    operationOwner->scoreClipArtifactCompletionMutex_};
                operationOwner->scoreClipArtifactCompletion_.emplace(
                    ScoreClipArtifactCompletion{
                        operation,
                        documentAuthorityGeneration,
                        deviceGeneration,
                        std::move(expectedDocument),
                        std::move(clipId),
                        std::move(result),
                    });
            });
    }
    catch (const std::exception& error)
    {
        (void) fileOperationCoordinator_.finish(
            *operation, talas::app::WorkbenchOperationOutcome::Failed);
        scoreClipArtifactRunning_ = false;
        scoreClipArtifactToken_.reset();
        scoreClipArtifactActiveClipId_.clear();
        scoreClipArtifactActiveMode_.reset();
        scoreClipArtifactStateCache_.clear();
        timelinePanel_->refresh();
        commandManager_.commandStatusChanged();
        setStatus("Clip artifact worker failed to start: "
            + juceString(error.what()));
    }
}

void WorkbenchComponent::finishScoreClipArtifactPublicationIfReady()
{
    std::optional<ScoreClipArtifactCompletion> completion;
    {
        std::lock_guard lock{scoreClipArtifactCompletionMutex_};
        if (!scoreClipArtifactCompletion_.has_value())
            return;
        completion = std::move(scoreClipArtifactCompletion_);
        scoreClipArtifactCompletion_.reset();
    }
    auto outcome = completion->result.cancelled
            || completion->result.invalidated
        ? talas::app::WorkbenchOperationOutcome::Cancelled
        : talas::app::WorkbenchOperationOutcome::Failed;
    juce::String status;
    const auto deviceCurrent = deviceGeneration_.load(
        std::memory_order_acquire) == completion->deviceGeneration;
    std::string dependencyError;
    const auto dependencyFingerprint = completion->result.ok()
        ? talas::runtime::scoreClipSourceFingerprint(
            projectBridge_.editor().document(),
            registry_,
            completion->clipId,
            completion->result.artifact.sampleRate,
            dependencyError)
        : std::string{};
    const auto dependencyCurrent = completion->result.ok()
        && !dependencyFingerprint.empty()
        && dependencyFingerprint
            == completion->result.artifact.sourceFingerprint;
    if (completion->result.ok() && deviceCurrent && dependencyCurrent)
    {
        talas::project::ScoreClipRenderArtifactPublication publication;
        publication.expectedDocumentAuthorityGeneration =
            completion->documentAuthorityGeneration;
        publication.expectedCanonicalDocumentBytes =
            std::move(completion->expectedDocument);
        publication.clipId = completion->clipId;
        publication.artifact = std::move(completion->result.artifact);
        publication.assets = std::move(completion->result.assets);
        const auto edit = projectBridge_.editor()
            .publishScoreClipRenderArtifact(std::move(publication));
        if (edit.ok())
        {
            documentChanged();
            outcome = talas::app::WorkbenchOperationOutcome::Succeeded;
            status = "Published exact Score clip artifact as one Undo step";
        }
        else
        {
            (void) fileOperationCoordinator_.invalidatePublication(
                completion->operation);
            status = "Clip artifact files were retained but document publication was rejected: "
                + juceString(edit.message) + juceString(" · ")
                + jucePathString(completion->result.publishedDirectory);
        }
    }
    else if (completion->result.ok())
    {
        (void) fileOperationCoordinator_.invalidatePublication(
            completion->operation);
        status = dependencyCurrent
            ? juceString(
                "Clip artifact files were retained but sample-rate authority changed before document publication · ")
            : juceString(
                "Clip artifact files were retained but source dependency authority changed before document publication · ");
        if (!dependencyCurrent && !dependencyError.empty())
            status += juceString(dependencyError) + juceString(" · ");
        status += jucePathString(
            completion->result.publishedDirectory);
    }
    else if (completion->result.invalidated)
    {
        status = "Clip artifact operation cancelled because project authority changed";
    }
    else if (completion->result.cancelled)
    {
        status = "Clip artifact operation cancelled";
    }
    else
    {
        status = "Clip artifact operation failed: "
            + juceString(completion->result.error);
    }
    (void) fileOperationCoordinator_.finish(
        completion->operation, outcome);
    scoreClipArtifactRunning_ = false;
    scoreClipArtifactToken_.reset();
    scoreClipArtifactActiveClipId_.clear();
    scoreClipArtifactActiveMode_.reset();
    scoreClipArtifactStateCache_.clear();
    timelinePanel_->refresh();
    commandManager_.commandStatusChanged();
    setStatus(status);
}

void WorkbenchComponent::cancelScoreClipArtifactPublication()
{
    if (!scoreClipArtifactRunning_
        || !scoreClipArtifactToken_.has_value())
    {
        setStatus("No clip artifact operation is active");
        return;
    }
    if (fileOperationCoordinator_.requestCancel(*scoreClipArtifactToken_))
    {
        setStatus("Cancelling clip artifact operation...");
        timelinePanel_->refresh();
    }
    else
    {
        setStatus("Clip artifact publication has started and can no longer be cancelled");
    }
}

void WorkbenchComponent::stopScoreClipArtifactPublicationForLifecycle()
{
    if (scoreClipArtifactToken_.has_value())
    {
        (void) fileOperationCoordinator_.requestCancel(
            *scoreClipArtifactToken_);
    }
    if (scoreClipArtifactThread_.joinable())
        scoreClipArtifactThread_.join();
    {
        std::lock_guard lock{scoreClipArtifactCompletionMutex_};
        scoreClipArtifactCompletion_.reset();
    }
    scoreClipArtifactRunning_ = false;
    scoreClipArtifactToken_.reset();
    scoreClipArtifactActiveClipId_.clear();
    scoreClipArtifactActiveMode_.reset();
}
}
