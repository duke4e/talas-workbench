#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectDiskCache.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSoundExpansion.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <ranges>

namespace talas::app::workbench
{
namespace
{
struct ResolvedDiskCacheSelection
{
    bool visible = false;
    std::optional<talas::project::ProjectNode> effectiveNode;
    std::string sourcePath;
    std::string error;
};

[[nodiscard]] ResolvedDiskCacheSelection resolveDiskCacheSelection(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::string>& definitionId,
    std::string_view nodeId)
{
    ResolvedDiskCacheSelection result;
    if (nodeId.empty()) return result;
    if (definitionId.has_value())
    {
        const auto definition = std::ranges::find(
            document.soundDefinitions, *definitionId,
            &talas::project::ProjectSoundDefinition::definitionId);
        if (definition == document.soundDefinitions.end()) return result;
        const auto local = std::ranges::find(
            definition->graph.nodes, nodeId,
            &talas::project::ProjectNode::instanceId);
        result.visible = local != definition->graph.nodes.end()
            && local->typeId == "talas.diskCache";
    }
    else
    {
        const auto local = std::ranges::find(
            document.graph.nodes, nodeId,
            &talas::project::ProjectNode::instanceId);
        result.visible = local != document.graph.nodes.end()
            && local->typeId == "talas.diskCache";
    }
    if (!result.visible) return result;

    const auto expanded = talas::project::expandProjectSounds(
        document, registry);
    if (!expanded.ok())
    {
        result.error = "capture target could not expand reusable Sounds";
        return result;
    }
    const talas::project::ExpandedSoundNodeIdentity* identity = nullptr;
    for (const auto& candidate : expanded.manifest.nodes)
    {
        const auto matches = definitionId.has_value()
            ? candidate.definitionId == *definitionId
                && candidate.localNodeId == nodeId
            : candidate.instancePath.empty()
                && candidate.localNodeId == nodeId;
        if (!matches) continue;
        if (identity != nullptr)
        {
            result.error = "select one exact instantiated Sound target before capture";
            return result;
        }
        identity = &candidate;
    }
    if (identity == nullptr)
    {
        result.error = "capture target has no exact instantiated source path";
        return result;
    }
    const auto effective = std::ranges::find(
        expanded.document.graph.nodes, identity->flatNodeId,
        &talas::project::ProjectNode::instanceId);
    if (effective == expanded.document.graph.nodes.end()
        || effective->typeId != "talas.diskCache")
    {
        result.error = "capture target no longer resolves to DiskCache";
        return result;
    }
    result.effectiveNode = *effective;
    result.sourcePath = identity->sourcePath;
    return result;
}

[[nodiscard]] std::optional<talas::project::ProjectNode>
effectiveDiskCacheNodeForSourcePath(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view sourcePath)
{
    const auto expanded = talas::project::expandProjectSounds(
        document, registry);
    if (!expanded.ok()) return std::nullopt;
    const auto identity = std::ranges::find(
        expanded.manifest.nodes, sourcePath,
        &talas::project::ExpandedSoundNodeIdentity::sourcePath);
    if (identity == expanded.manifest.nodes.end()) return std::nullopt;
    const auto effective = std::ranges::find(
        expanded.document.graph.nodes, identity->flatNodeId,
        &talas::project::ProjectNode::instanceId);
    if (effective == expanded.document.graph.nodes.end()
        || effective->typeId != "talas.diskCache")
    {
        return std::nullopt;
    }
    return *effective;
}

[[nodiscard]] bool exactBoolean(
    const talas::project::ProjectNode& node,
    std::string_view parameterId,
    bool value) noexcept
{
    const auto parameter = std::ranges::find(
        node.parameters, parameterId,
        &talas::project::ProjectParameterExpression::parameterId);
    return parameter != node.parameters.end()
        && parameter->boolean == std::optional<bool>{value};
}

[[nodiscard]] std::optional<std::string> resourceParameter(
    const talas::project::ProjectNode& node,
    std::string_view parameterId)
{
    const auto parameter = std::ranges::find(
        node.parameters, parameterId,
        &talas::project::ProjectParameterExpression::parameterId);
    if (parameter == node.parameters.end()
        || !parameter->resource.has_value()
        || parameter->resource->empty())
    {
        return std::nullopt;
    }
    return parameter->resource;
}

[[nodiscard]] std::string boundedCacheToken(std::string_view value)
{
    std::string result;
    result.reserve(std::min<std::size_t>(value.size(), 80));
    for (const auto character : value)
    {
        if (result.size() == 80)
            break;
        const auto accepted = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '_';
        result.push_back(accepted ? character : '_');
    }
    return result.empty() ? std::string{"cache"} : result;
}
}

DiskCacheCapturePresentation WorkbenchComponent::diskCacheCapturePresentation(
    const std::optional<std::string>& definitionId,
    std::string_view nodeId)
{
    DiskCacheCapturePresentation result;
    const auto& document = projectBridge_.editor().document();
    ResolvedDiskCacheSelection resolved;
    try
    {
        resolved = resolveDiskCacheSelection(
            document, registry_, definitionId, nodeId);
    }
    catch (...)
    {
        result.visible = true;
        result.label = "Capture Disk Cache Loop";
        result.detail = "Unavailable: capture target resolution failed within bounded resources.";
        return result;
    }
    result.visible = resolved.visible;
    result.sourcePath = resolved.sourcePath;
    if (!resolved.effectiveNode.has_value())
    {
        if (result.visible)
        {
            result.label = "Capture Disk Cache Loop";
            result.detail = "Unavailable: " + juceString(resolved.error);
        }
        return result;
    }
    const auto* node = &*resolved.effectiveNode;

    if (diskCacheCaptureRunning_
        && diskCacheCaptureSourcePath_ == result.sourcePath)
    {
        const auto operation = fileOperationCoordinator_.snapshot();
        result.cancelling =
            operation.phase == talas::app::WorkbenchOperationPhase::Cancelling
            || operation.phase == talas::app::WorkbenchOperationPhase::Invalidated;
        result.enabled = operation.phase
            == talas::app::WorkbenchOperationPhase::Running;
        result.label = result.cancelling
            ? "Cancelling Capture"
            : "Cancel Capture";
        result.detail = result.cancelling
            ? "Disk Cache capture cancellation is pending."
            : "Cancel the active off-audio Disk Cache loop capture.";
        return result;
    }

    result.label = "Capture Disk Cache Loop";
    if (!exactBoolean(*node, "record", true))
    {
        const auto assetId = resourceParameter(*node, "fileName");
        const auto asset = assetId.has_value()
            ? std::ranges::find(
                  document.assets, *assetId,
                  &talas::project::ProjectAsset::id)
            : document.assets.end();
        if (asset != document.assets.end())
        {
            const auto authority =
                talas::project::diskCacheAssetAuthority(*asset);
            if (authority
                == talas::project::DiskCacheAssetAuthority::CapturedPlayback)
            {
                const auto resolvedAsset =
                    talas::project::resolveProjectAssetPath(
                        *asset, projectPath());
                talas::project::ProjectAssetInspectionResult inspected;
                try
                {
                    inspected = resolvedAsset.empty()
                        ? talas::project::ProjectAssetInspectionResult{
                              *asset,
                              talas::project::ProjectAssetInspectionError{
                                  talas::project::ProjectAssetInspectionErrorCode::Unavailable,
                                  "Captured bytes are unavailable",
                                  {}}}
                        : talas::project::inspectProjectAsset(
                              *asset, resolvedAsset);
                }
                catch (...)
                {
                    result.label = "Invalid Disk Cache Capture";
                    result.detail =
                        "Unavailable: captured generation inspection could not allocate its bounded byte snapshot.";
                    return result;
                }
                if (!inspected.ok()
                    && inspected.error->code
                        == talas::project::ProjectAssetInspectionErrorCode::Unavailable)
                {
                    result.label = "Missing Disk Cache";
                    result.detail =
                        "Unavailable: captured generation "
                        + juce::String(static_cast<juce::int64>(
                            asset->generation))
                        + " is missing at "
                        + juceString(asset->path) + ".";
                    return result;
                }
                if (!inspected.ok() || inspected.asset != *asset)
                {
                    result.label = "Invalid Disk Cache Capture";
                    result.detail =
                        "Unavailable: captured generation bytes, SHA-256, or PCM16 media authority is invalid.";
                    return result;
                }
                const auto fingerprint =
                    talas::project::diskCacheCaptureFingerprint(
                        asset->provenance);
                std::string currentError;
                const auto current = projectPath().has_value()
                    ? talas::project::projectDiskCacheSourceFingerprint(
                          document, registry_, result.sourcePath,
                          static_cast<std::uint32_t>(asset->sampleRate),
                          currentError)
                    : std::string{};
                const auto seconds = asset->sampleRate > 0.0
                    ? static_cast<double>(asset->frameCount)
                        / asset->sampleRate
                    : 0.0;
                result.label = "Captured Disk Cache";
                result.detail =
                    (fingerprint.has_value() && current == *fingerprint
                         ? "Current captured generation "
                         : "Stale captured generation ")
                    + juce::String(
                        static_cast<juce::int64>(asset->generation))
                    + juceString(" · mono PCM16 · ")
                    + juce::String(seconds, 3) + juceString(" s · ")
                    + juceString(asset->path);
                return result;
            }
            if (authority
                == talas::project::DiskCacheAssetAuthority::InvalidCapturedAsset)
            {
                result.label = "Invalid Disk Cache Capture";
                result.detail = "Unavailable: captured asset provenance or media authority is invalid.";
                return result;
            }
            result.label = "Legacy Disk Cache";
            result.detail = "Legacy passthrough/prewarm asset; set Record to On to create exact captured playback.";
            return result;
        }
        result.detail = "Unavailable: set Record to On for this DiskCache.";
        return result;
    }
    if (!projectPath().has_value() || projectPath()->empty())
    {
        result.detail = "Unavailable: save the project before capture.";
        return result;
    }
    if (projectBridge_.latestStageGeneration() == 0)
    {
        result.detail = "Unavailable: establish a prepared device profile before capture.";
        return result;
    }
    if (fileOperationCoordinator_.snapshot().active())
    {
        result.detail = "Unavailable: finish the active file operation first.";
        return result;
    }
    const auto sampleRate = defaultExportSampleRate();
    const auto range = talas::project::diskCacheCaptureRange(
        document.timeline.loop, sampleRate);
    if (!range.ok())
    {
        result.detail = "Unavailable: enable one valid bounded Timeline loop.";
        return result;
    }
    std::string fingerprintError;
    if (talas::project::projectDiskCacheSourceFingerprint(
            document, registry_, result.sourcePath,
            static_cast<std::uint32_t>(sampleRate),
            fingerprintError).empty())
    {
        result.detail = "Unavailable: "
            + juceString(fingerprintError.empty()
                ? std::string_view{"DiskCache source is not authorable."}
                : std::string_view{fingerprintError});
        return result;
    }
    const auto assetId = resourceParameter(*node, "fileName")
        .value_or("disk-cache-" + boundedCacheToken(result.sourcePath));
    const auto generation =
        talas::project::nextDiskCacheCaptureAssetGeneration(
            document, assetId);
    if (!generation.ok())
    {
        result.detail = generation.error
                == talas::project::DiskCacheCaptureGenerationError::Exhausted
            ? "Unavailable: the DiskCache asset generation is exhausted."
            : "Unavailable: the destination asset identity belongs to unrelated authority.";
        return result;
    }
    result.enabled = true;
    result.detail = "Capture the enabled Timeline loop off the audio callback as immutable mono PCM16, then publish one exact Undo step.";
    return result;
}

void WorkbenchComponent::performDiskCacheCaptureAction(
    std::optional<std::string> definitionId,
    std::string nodeId)
{
    const auto presentation = diskCacheCapturePresentation(
        definitionId, nodeId);
    if (diskCacheCaptureRunning_
        && diskCacheCaptureSourcePath_ == presentation.sourcePath)
    {
        cancelDiskCacheCapture();
        return;
    }
    if (!presentation.visible || !presentation.enabled)
    {
        setStatus(presentation.detail.isNotEmpty()
            ? presentation.detail
            : juce::String{"Disk Cache capture is unavailable"});
        return;
    }
    startDiskCacheCapture(presentation.sourcePath);
}

void WorkbenchComponent::startDiskCacheCapture(std::string sourcePath)
{
    if (diskCacheCaptureThread_.joinable())
        diskCacheCaptureThread_.join();
    const auto operation = fileOperationCoordinator_.begin(
        talas::app::WorkbenchOperationKind::DiskCacheCapture);
    if (!operation.has_value())
    {
        setStatus("Finish the active file operation before capturing Disk Cache audio");
        return;
    }

    const auto& current = projectBridge_.editor().document();
    const auto node = effectiveDiskCacheNodeForSourcePath(
        current, registry_, sourcePath);
    if (!node.has_value() || !projectPath().has_value())
    {
        (void) fileOperationCoordinator_.finish(
            *operation, talas::app::WorkbenchOperationOutcome::Failed);
        setStatus("Disk Cache capture target changed before start");
        return;
    }

    try
    {
        auto document = current;
        const auto expectedDocument = currentDocumentSnapshot();
        const auto documentGeneration =
            projectBridge_.editor().documentAuthorityGeneration();
        const auto deviceGeneration = deviceGeneration_.load(
            std::memory_order_acquire);
        const auto sampleRate = defaultExportSampleRate();
        std::size_t maximumBlockFrames = 256;
        if (auto* device = deviceManager_.getCurrentAudioDevice();
            device != nullptr && device->getCurrentBufferSizeSamples() > 0)
        {
            maximumBlockFrames = static_cast<std::size_t>(
                device->getCurrentBufferSizeSamples());
        }

        auto assetId = resourceParameter(*node, "fileName")
            .value_or("disk-cache-" + boundedCacheToken(sourcePath));
        const auto generation =
            talas::project::nextDiskCacheCaptureAssetGeneration(
                document, assetId);
        if (!generation.ok())
        {
            throw std::runtime_error(
                "Disk Cache destination asset authority changed before start");
        }
        const auto assetGeneration = *generation.generation;
        const auto leaf = boundedCacheToken(sourcePath)
            + "-g" + std::to_string(assetGeneration)
            + "-op" + std::to_string(operation->generation)
            + "-" + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch().count())
            + ".wav";

        talas::audio::DiskCacheCaptureOptions options;
        options.projectFilePath = *projectPath();
        options.sourcePath = sourcePath;
        options.assetId = std::move(assetId);
        options.relativeAssetPath = "DiskCache/" + leaf;
        options.assetGeneration = assetGeneration;
        options.expectedDocumentAuthorityGeneration = documentGeneration;
        options.expectedCanonicalDocumentBytes = expectedDocument;
        options.expectedDeviceGeneration = deviceGeneration;
        options.sampleRate = sampleRate;
        options.maximumBlockFrames = maximumBlockFrames;

        diskCacheCaptureRunning_ = true;
        diskCacheCaptureToken_ = *operation;
        diskCacheCaptureSourcePath_ = sourcePath;
        inspectorPanel_->refresh();
        commandManager_.commandStatusChanged();
        setStatus("Capturing Disk Cache loop... 0%");

        diskCacheCaptureThread_ = std::thread(
            [this,
             operation = *operation,
             documentGeneration,
             deviceGeneration,
             document = std::move(document),
             options = std::move(options)]() mutable {
                options.shouldCancel = [this, operation,
                                        documentGeneration,
                                        deviceGeneration] {
                    return fileOperationShouldCancel(
                               operation, documentGeneration)
                        || deviceGeneration_.load(
                               std::memory_order_acquire)
                            != deviceGeneration;
                };
                options.reportProgress = [this, operation](double progress) {
                    (void) fileOperationCoordinator_.publishProgress(
                        operation, progress);
                };
                auto result = talas::audio::prepareProjectDiskCacheCapture(
                    document, registry_, std::move(options));
                std::lock_guard lock{diskCacheCaptureCompletionMutex_};
                diskCacheCaptureCompletion_.emplace(
                    DiskCacheCaptureCompletion{
                        operation,
                        documentGeneration,
                        deviceGeneration,
                        std::move(result),
                    });
            });
    }
    catch (const std::exception& exception)
    {
        (void) fileOperationCoordinator_.finish(
            *operation, talas::app::WorkbenchOperationOutcome::Failed);
        diskCacheCaptureRunning_ = false;
        diskCacheCaptureToken_.reset();
        diskCacheCaptureSourcePath_.clear();
        inspectorPanel_->refresh();
        commandManager_.commandStatusChanged();
        setStatus("Disk Cache capture failed to start: "
            + juceString(exception.what()));
    }
}

void WorkbenchComponent::finishDiskCacheCaptureIfReady()
{
    std::optional<DiskCacheCaptureCompletion> completion;
    {
        std::lock_guard lock{diskCacheCaptureCompletionMutex_};
        if (!diskCacheCaptureCompletion_.has_value())
            return;
        completion = std::move(diskCacheCaptureCompletion_);
        diskCacheCaptureCompletion_.reset();
    }
    if (diskCacheCaptureThread_.joinable())
        diskCacheCaptureThread_.join();

    auto outcome = talas::app::WorkbenchOperationOutcome::Failed;
    juce::String status;
    auto& result = completion->result;
    if (result.ok())
    {
        talas::project::PreparedDiskCacheCaptureEdit edit;
        const auto prepare = projectBridge_.editor().prepareDiskCacheCapture(
            result.prepared->takeEditorPublication(), edit);
        const auto deviceCurrent = deviceGeneration_.load(
            std::memory_order_acquire) == completion->deviceGeneration;
        if (prepare.ok() && deviceCurrent
            && admitFileOperationPublication(
                completion->operation,
                completion->documentAuthorityGeneration)
            && projectBridge_.editor().diskCacheCaptureStillCurrent(edit)
            && deviceGeneration_.load(std::memory_order_acquire)
                == completion->deviceGeneration)
        {
            std::string publishError;
            const auto published =
                result.prepared->publishFile(publishError);
            if (published
                && projectBridge_.editor().commitPreparedDiskCacheCapture(
                    std::move(edit)))
            {
                outcome = talas::app::WorkbenchOperationOutcome::Succeeded;
                status = juceString(
                    "Captured and published exact Disk Cache loop · ")
                    + jucePathString(result.prepared->destination())
                    + juceString(" · ") + juce::String(
                        static_cast<juce::int64>(result.renderedFrames))
                    + " frames";
                (void) fileOperationCoordinator_.finish(
                    completion->operation, outcome);
                documentChanged();
            }
            else
            {
                if (published)
                {
                    std::string rollbackError;
                    if (!result.prepared->retractPublishedFile(
                            rollbackError))
                    {
                        if (!publishError.empty())
                            publishError += "; ";
                        publishError += "WAV rollback failed: "
                            + rollbackError;
                    }
                }
                status = "Disk Cache capture publication failed: "
                    + juceString(publishError.empty()
                        ? std::string_view{"prepared document authority changed"}
                        : std::string_view{publishError});
            }
        }
        else
        {
            (void) fileOperationCoordinator_.invalidatePublication(
                completion->operation);
            status = prepare.ok()
                ? "Disk Cache capture cancelled because project or device authority changed"
                : "Disk Cache capture publication rejected: "
                    + juceString(prepare.message);
        }
    }
    else if (result.cancelled())
    {
        outcome = talas::app::WorkbenchOperationOutcome::Cancelled;
        const auto operation = fileOperationCoordinator_.snapshot();
        status = operation.phase
                == talas::app::WorkbenchOperationPhase::Invalidated
            ? "Disk Cache capture cancelled because project authority changed"
            : "Disk Cache capture cancelled";
    }
    else
    {
        status = "Disk Cache capture failed: " + juceString(result.error);
    }
    if (outcome != talas::app::WorkbenchOperationOutcome::Succeeded)
    {
        (void) fileOperationCoordinator_.finish(
            completion->operation, outcome);
    }
    diskCacheCaptureRunning_ = false;
    diskCacheCaptureToken_.reset();
    diskCacheCaptureSourcePath_.clear();
    inspectorPanel_->refresh();
    commandManager_.commandStatusChanged();
    setStatus(status);
}

void WorkbenchComponent::cancelDiskCacheCapture()
{
    if (!diskCacheCaptureToken_.has_value())
    {
        setStatus("No Disk Cache capture is active");
        return;
    }
    if (fileOperationCoordinator_.requestCancel(*diskCacheCaptureToken_))
    {
        setStatus("Cancelling Disk Cache capture...");
        inspectorPanel_->refresh();
    }
    else
    {
        setStatus("Disk Cache capture publication has started and can no longer be cancelled");
    }
}

void WorkbenchComponent::refreshDiskCacheCaptureStatus()
{
    const auto operation = fileOperationCoordinator_.snapshot();
    if (operation.phase
        == talas::app::WorkbenchOperationPhase::Invalidated)
    {
        setStatus("Cancelling Disk Cache capture: project authority changed");
    }
    else if (operation.phase
             == talas::app::WorkbenchOperationPhase::Cancelling)
    {
        setStatus("Cancelling Disk Cache capture...");
    }
    else
    {
        setStatus(
            "Capturing Disk Cache loop... "
            + juce::String(
                static_cast<int>(operation.progressPermille / 10U))
            + "%");
    }
    inspectorPanel_->refresh();
}

void WorkbenchComponent::stopDiskCacheCaptureForLifecycle()
{
    if (diskCacheCaptureToken_.has_value())
        (void) fileOperationCoordinator_.requestCancel(
            *diskCacheCaptureToken_);
    if (diskCacheCaptureThread_.joinable())
        diskCacheCaptureThread_.join();
    {
        std::lock_guard lock{diskCacheCaptureCompletionMutex_};
        diskCacheCaptureCompletion_.reset();
    }
    diskCacheCaptureRunning_ = false;
    diskCacheCaptureToken_.reset();
    diskCacheCaptureSourcePath_.clear();
}

std::optional<std::string> WorkbenchComponent::runDiskCacheCaptureSmoke(
    const std::filesystem::path& smokeDirectory)
{
    const auto fail = [](bool condition, std::string message)
        -> std::optional<std::string> {
        return condition ? std::optional<std::string>{}
                         : std::optional<std::string>{std::move(message)};
    };
    const auto completionReady = [this] {
        std::lock_guard lock{diskCacheCaptureCompletionMutex_};
        return diskCacheCaptureCompletion_.has_value();
    };
    talas::project::ProjectDocument profileFixture;
    profileFixture.graph.nodes = {
        {"profileOsc", "talas.oscillator",
         {{"frequency", std::string{"1000 hz"}},
          {"amplitude", std::string{"1"}}}, {}},
        {"profileOut", "talas.audioOutput", {}, {}},
    };
    profileFixture.graph.connections = {
        {"profileOsc", "audioOut", "profileOut", "audioIn"},
    };
    const auto profileReplaced = projectBridge_.editor().replaceDocument(
        profileFixture);
    if (!profileReplaced.ok())
        return "Disk Cache smoke profile fixture was rejected";
    documentChanged();
    talas::runtime::RenderSettings profile;
    profile.sampleRate = 8000.0;
    profile.maxBlockFrames = 64;
    if (!stageProject(std::move(profile), "Disk Cache smoke profile"))
        return "Disk Cache smoke could not establish a device profile";
    setPlaying(true);
    std::array<float, 1> profileSample{};
    std::array<float*, 1> profileChannels{profileSample.data()};
    if (!projectBridge_.render(
            profileChannels.data(), profileChannels.size(), 1))
    {
        return "Disk Cache smoke could not adopt its device profile";
    }

    const auto nestedFixture = [](bool multipleInstances) {
        talas::project::ProjectDocument document;
        document.timeline.loop = {true, 0.002, 0.006};
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId = "cachedVoice";
        definition.displayName = "Cached Voice";
        definition.graph.nodes = {
            {"osc", "talas.oscillator",
             {{"frequency", std::string{"1000 hz"}},
              {"amplitude", std::string{"1"}}}, {}},
            {"cache", "talas.diskCache",
             {{"fileName", {}, {}, {}, std::string{"Smoke/Nested"}},
              {"record", {}, {}, true}}, {}},
        };
        definition.graph.connections = {
            {"osc", "audioOut", "cache", "audioIn"},
        };
        definition.outputs = {
            {"audioOut", talas::core::PortType::Audio, "cache", "audioOut"},
        };
        document.soundDefinitions.push_back(std::move(definition));
        document.graph.nodes = {
            {"voiceA", {}, {}, std::string{"cachedVoice"}},
            {"outA", "talas.audioOutput", {}, {}},
        };
        document.graph.connections = {
            {"voiceA", "audioOut", "outA", "audioIn"},
        };
        if (multipleInstances)
        {
            document.graph.nodes.push_back(
                {"voiceB", {}, {}, std::string{"cachedVoice"}});
            document.graph.nodes.push_back(
                {"outB", "talas.audioOutput", {}, {}});
            document.graph.connections.push_back(
                {"voiceB", "audioOut", "outB", "audioIn"});
        }
        return document;
    };
    const auto nestedProjectFile =
        (smokeDirectory / "disk-cache-nested-smoke.talas.json")
            .lexically_normal();
    auto nested = nestedFixture(false);
    auto nestedReplaced = projectBridge_.editor().replaceDocument(nested);
    if (!nestedReplaced.ok())
        return "nested Disk Cache smoke fixture was rejected: "
            + nestedReplaced.message;
    documentChanged();
    if (!saveProjectToPath(nestedProjectFile))
        return "nested Disk Cache smoke fixture could not be saved";
    selectNode(
        std::optional<std::string>{"cachedVoice"}, "cache");
    inspectorPanel_->refresh();
    const auto uniqueNested = diskCacheCapturePresentation(
        std::optional<std::string>{"cachedVoice"}, "cache");
    if (auto error = fail(
            uniqueNested.visible
                && uniqueNested.enabled
                && uniqueNested.sourcePath
                    == "voiceA/cachedVoice::cache"
                && inspectorPanel_->diskCacheCaptureReadyForSmoke(
                    "Capture Disk Cache Loop", true,
                    "immutable mono PCM16"),
            "Inspector did not resolve one definition-owned Disk Cache to its exact instantiated source path"))
    {
        return error;
    }

    nested = nestedFixture(true);
    nestedReplaced = projectBridge_.editor().replaceDocument(nested);
    if (!nestedReplaced.ok())
        return "ambiguous nested Disk Cache smoke fixture was rejected: "
            + nestedReplaced.message;
    documentChanged();
    if (!saveProjectToPath(nestedProjectFile))
        return "ambiguous nested Disk Cache smoke fixture could not be saved";
    selectNode(
        std::optional<std::string>{"cachedVoice"}, "cache");
    inspectorPanel_->refresh();
    const auto ambiguousNested = diskCacheCapturePresentation(
        std::optional<std::string>{"cachedVoice"}, "cache");
    if (auto error = fail(
            ambiguousNested.visible
                && !ambiguousNested.enabled
                && ambiguousNested.sourcePath.empty()
                && inspectorPanel_->diskCacheCaptureReadyForSmoke(
                    "Capture Disk Cache Loop", false,
                    "one exact instantiated Sound target"),
            "Inspector did not fail closed for an ambiguous definition-owned Disk Cache"))
    {
        return error;
    }

    talas::project::ProjectDocument fixture;
    fixture.timeline.loop = {true, 0.002, 0.006};
    fixture.graph.nodes = {
        {"osc", "talas.oscillator",
         {{"frequency", std::string{"1000 hz"}},
          {"amplitude", std::string{"1"}}}, {}},
        {"cache", "talas.diskCache",
         {{"fileName", {}, {}, {}, std::string{"Smoke/Cache"}},
          {"record", {}, {}, true}}, {}},
        {"out", "talas.audioOutput", {}, {}},
    };
    fixture.graph.connections = {
        {"osc", "audioOut", "cache", "audioIn"},
        {"cache", "audioOut", "out", "audioIn"},
    };
    const auto projectFile =
        (smokeDirectory / "disk-cache-capture-smoke.talas.json")
            .lexically_normal();

    auto unavailableFixture = fixture;
    talas::project::ProjectAsset unrelatedAsset;
    unrelatedAsset.id = "Smoke/Cache";
    unrelatedAsset.kind = talas::project::ProjectAssetKind::Audio;
    unrelatedAsset.path = "Audio/unrelated.wav";
    unrelatedAsset.disposition =
        talas::project::ProjectAssetDisposition::Referenced;
    unrelatedAsset.contentHash.assign(64, 'a');
    unrelatedAsset.channelCount = 1;
    unrelatedAsset.sampleRate = 8000.0;
    unrelatedAsset.frameCount = 32;
    unrelatedAsset.encoding = "pcm16-wav";
    unrelatedAsset.generation = 1;
    unrelatedAsset.provenance = "repository-original";
    unavailableFixture.assets.push_back(unrelatedAsset);
    auto unavailableReplaced = projectBridge_.editor().replaceDocument(
        unavailableFixture);
    if (!unavailableReplaced.ok())
        return "unrelated Disk Cache authority smoke fixture was rejected";
    documentChanged();
    selectNode("cache");
    inspectorPanel_->refresh();
    if (auto error = fail(
            inspectorPanel_->diskCacheCaptureReadyForSmoke(
                "Capture Disk Cache Loop", false,
                "unrelated authority"),
            "Inspector enabled capture over an unrelated existing asset identity"))
    {
        return error;
    }

    auto exhaustedFixture = fixture;
    auto exhaustedAsset = unrelatedAsset;
    exhaustedAsset.path = "DiskCache/cache-gmax.wav";
    exhaustedAsset.generation =
        talas::project::maximumProjectAssetJsonInteger;
    exhaustedAsset.provenance =
        talas::project::makeDiskCacheCaptureProvenance(
            std::string(
                talas::project::diskCacheCaptureFingerprintBytes,
                'b'));
    exhaustedFixture.assets.push_back(std::move(exhaustedAsset));
    unavailableReplaced = projectBridge_.editor().replaceDocument(
        exhaustedFixture);
    if (!unavailableReplaced.ok())
        return "exhausted Disk Cache authority smoke fixture was rejected";
    documentChanged();
    selectNode("cache");
    inspectorPanel_->refresh();
    if (auto error = fail(
            inspectorPanel_->diskCacheCaptureReadyForSmoke(
                "Capture Disk Cache Loop", false,
                "generation is exhausted"),
            "Inspector enabled capture beyond exact persisted generation authority"))
    {
        return error;
    }

    const auto replaced = projectBridge_.editor().replaceDocument(fixture);
    if (!replaced.ok())
        return "Disk Cache smoke fixture was rejected: " + replaced.message;
    documentChanged();
    if (!saveProjectToPath(projectFile))
        return "Disk Cache smoke fixture could not be saved";
    selectNode("cache");
    inspectorPanel_->refresh();
    if (auto error = fail(
            inspectorPanel_->diskCacheCaptureReadyForSmoke(
                "Capture Disk Cache Loop", true,
                "immutable mono PCM16"),
            "Inspector did not expose one truthful enabled Disk Cache capture action"))
    {
        return error;
    }
    inspectorPanel_->activateDiskCacheCaptureForSmoke();
    for (std::size_t attempt = 0;
         attempt < 1'000'000 && !completionReady();
         ++attempt)
    {
        std::this_thread::yield();
    }
    finishDiskCacheCaptureIfReady();
    const auto& captured = projectBridge_.editor().document();
    const auto asset = std::ranges::find(
        captured.assets, std::string_view{"Smoke/Cache"},
        &talas::project::ProjectAsset::id);
    if (auto error = fail(
            !diskCacheCaptureRunning_
                && asset != captured.assets.end()
                && asset->generation == 1
                && talas::project::diskCacheAssetAuthority(*asset)
                    == talas::project::DiskCacheAssetAuthority::CapturedPlayback,
            "real Inspector capture did not publish exact captured asset authority: "
                + statusLabel_.getText().toStdString()))
    {
        return error;
    }
    const auto capturedPath = talas::project::resolveProjectAssetPath(
        *asset, projectPath());
    const auto capturedSampleRate = asset->sampleRate;
    const auto wave = talas::audio::readPcm16Wav(capturedPath);
    if (auto error = fail(
            wave.ok()
                && wave.sampleRate
                    == static_cast<std::uint32_t>(capturedSampleRate)
                && wave.channels.size() == 1
                && wave.channels.front().size() == asset->frameCount,
            "real Inspector capture did not publish the exact loop WAV"))
    {
        return error;
    }
    inspectorPanel_->refresh();
    if (auto error = fail(
            inspectorPanel_->diskCacheCaptureReadyForSmoke(
                "Captured Disk Cache", false,
                "Current captured generation 1"),
            "Inspector did not expose current captured generation truth"))
    {
        return error;
    }

    auto amplitude = talas::project::ProjectParameterExpression{};
    amplitude.parameterId = "amplitude";
    amplitude.expression = "0";
    if (!projectBridge_.editor().setParameter("osc", amplitude).ok())
        return "Disk Cache smoke could not change upstream input";
    documentChanged();
    inspectorPanel_->refresh();
    if (auto error = fail(
            inspectorPanel_->diskCacheCaptureReadyForSmoke(
                "Captured Disk Cache", false,
                "Stale captured generation 1"),
            "Inspector did not expose stale captured generation truth"))
    {
        return error;
    }
    const auto& playbackDocument = projectBridge_.editor().document();
    const auto playbackCache = std::ranges::find(
        playbackDocument.graph.nodes, std::string_view{"cache"},
        &talas::project::ProjectNode::instanceId);
    if (playbackCache == playbackDocument.graph.nodes.end()
        || !exactBoolean(*playbackCache, "record", false))
    {
        return "captured Disk Cache project did not leave capture mode";
    }
    talas::runtime::RenderSettings playbackProfile;
    playbackProfile.sampleRate = capturedSampleRate;
    playbackProfile.maxBlockFrames = 64;
    if (!stageProject(std::move(playbackProfile), "Disk Cache playback smoke"))
        return "captured Disk Cache playback did not stage: "
            + statusLabel_.getText().toStdString();
    auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        playbackDocument, registry_, projectPath());
    if (!loaded.ok())
        return "captured Disk Cache smoke could not retain immutable resource bytes";
    talas::runtime::RenderSettings isolatedSettings;
    isolatedSettings.sampleRate = capturedSampleRate;
    isolatedSettings.maxBlockFrames = 64;
    isolatedSettings.resourceSnapshots = std::move(loaded.snapshots);
    talas::runtime::ProjectRuntimeBridge isolated{
        playbackDocument, registry_};
    if (!isolated.compileAndStage(std::move(isolatedSettings)).ok())
        return "captured Disk Cache isolated prepared stage failed";
    isolated.setPlaying(true);
    std::array<float, 32> output{};
    std::array<float*, 1> channels{output.data()};
    if (!isolated.render(
            channels.data(), channels.size(), output.size()))
    {
        return "captured Disk Cache playback did not render";
    }
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        if (std::abs(output[frame] - wave.channels.front()[frame])
            > 1.0 / 32767.0 + 1.0e-6)
        {
            return "captured playback rebound to changed upstream input at frame "
                + std::to_string(frame) + ": got "
                + std::to_string(output[frame]) + " expected "
                + std::to_string(wave.channels.front()[frame]);
        }
    }

    talas::project::ProjectParameterExpression armed;
    armed.parameterId = "record";
    armed.boolean = true;
    if (!projectBridge_.editor().setParameter("cache", armed).ok())
        return "Disk Cache cancellation fixture could not re-arm capture";
    auto longLoop = projectBridge_.editor().document();
    longLoop.timeline.loop = {true, 0.0, 10.0};
    if (!projectBridge_.editor().applyDocumentEdit(std::move(longLoop)).ok())
        return "Disk Cache cancellation fixture could not extend the loop";
    documentChanged();
    selectNode("cache");
    inspectorPanel_->refresh();
    inspectorPanel_->activateDiskCacheCaptureForSmoke();
    for (std::size_t attempt = 0;
         attempt < 1'000'000
             && fileOperationCoordinator_.snapshot().phase
                 != talas::app::WorkbenchOperationPhase::Running
             && !completionReady();
         ++attempt)
    {
        std::this_thread::yield();
    }
    inspectorPanel_->refresh();
    inspectorPanel_->activateDiskCacheCaptureForSmoke();
    for (std::size_t attempt = 0;
         attempt < 1'000'000 && !completionReady();
         ++attempt)
    {
        std::this_thread::yield();
    }
    finishDiskCacheCaptureIfReady();
    if (auto error = fail(
            !diskCacheCaptureRunning_
                && projectBridge_.editor().document().assets.front().generation
                    == 1,
            "Inspector Cancel Capture did not retain the prior generation"))
    {
        return error;
    }

    inspectorPanel_->refresh();
    inspectorPanel_->activateDiskCacheCaptureForSmoke();
    for (std::size_t attempt = 0;
         attempt < 1'000'000
             && fileOperationCoordinator_.snapshot().phase
                 != talas::app::WorkbenchOperationPhase::Running
             && !completionReady();
         ++attempt)
    {
        std::this_thread::yield();
    }
    talas::project::ProjectParameterExpression changedFrequency;
    changedFrequency.parameterId = "frequency";
    changedFrequency.expression = "500 hz";
    if (!projectBridge_.editor().setParameter(
            "osc", std::move(changedFrequency)).ok())
    {
        return "Disk Cache stale-authority fixture could not edit its source";
    }
    documentChanged();
    for (std::size_t attempt = 0;
         attempt < 1'000'000 && !completionReady();
         ++attempt)
    {
        std::this_thread::yield();
    }
    finishDiskCacheCaptureIfReady();
    if (auto error = fail(
            !diskCacheCaptureRunning_
                && projectBridge_.editor().document().assets.front().generation
                    == 1
                && statusLabel_.getText().containsIgnoreCase("authority"),
            "stale Disk Cache capture did not reject publication with exact authority status"))
    {
        return error;
    }

    talas::project::ProjectParameterExpression playback;
    playback.parameterId = "record";
    playback.boolean = false;
    if (!projectBridge_.editor().setParameter("cache", playback).ok())
        return "Disk Cache missing-state fixture could not return to playback mode";
    documentChanged();
    std::error_code corruptPermissionsError;
    std::filesystem::permissions(
        capturedPath,
        std::filesystem::perms::owner_read
            | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        corruptPermissionsError);
    if (corruptPermissionsError)
    {
        return "Disk Cache invalid-state fixture could not make captured WAV owner-writable: "
            + corruptPermissionsError.message();
    }
    {
        std::ofstream corrupt(
            capturedPath,
            std::ios::binary | std::ios::trunc);
        corrupt << "corrupt";
        if (!corrupt.good())
            return "Disk Cache invalid-state fixture could not corrupt captured WAV";
    }
    selectNode("cache");
    inspectorPanel_->refresh();
    if (auto error = fail(
            inspectorPanel_->diskCacheCaptureReadyForSmoke(
                "Invalid Disk Cache Capture", false,
                "SHA-256, or PCM16"),
            "Inspector reported corrupted captured bytes as Current"))
    {
        return error;
    }
    std::error_code removeError;
    if (!std::filesystem::remove(capturedPath, removeError)
        || removeError)
    {
        return "Disk Cache missing-state fixture could not remove captured WAV";
    }
    selectNode("cache");
    inspectorPanel_->refresh();
    return fail(
        inspectorPanel_->diskCacheCaptureReadyForSmoke(
            "Missing Disk Cache", false,
            "is missing at"),
        "Inspector did not expose missing captured-byte authority");
}
}
