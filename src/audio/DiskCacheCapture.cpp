#include "talas/audio/DiskCacheCapture.h"

#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectDiskCache.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectMidiVoiceIslands.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/runtime/ProjectRenderSettings.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace talas::audio
{
namespace
{
inline constexpr std::size_t maximumDiskCacheCaptureWavBytes =
    44U + 2U * static_cast<std::size_t>(
                    talas::project::maximumDiskCacheCaptureFrames);
[[nodiscard]] bool cancelled(const DiskCacheCaptureOptions& options)
{
    return options.shouldCancel && options.shouldCancel();
}

[[nodiscard]] std::optional<talas::project::ProjectDocument>
captureSourceDocument(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view sourcePath,
    std::string_view assetId,
    std::string& error)
{
    auto expanded = talas::project::expandProjectSounds(document, registry);
    if (!expanded.ok())
    {
        error = "DiskCache capture could not expand reusable Sounds";
        return std::nullopt;
    }
    const auto midiVoices = talas::project::discoverProjectMidiVoiceIslands(
        expanded.document,
        expanded.manifest,
        {});
    if (!midiVoices.ok()
        || !talas::project::materializeProjectSoundVariableBranches(
                document,
                expanded,
                midiVoices.islands)
                .ok())
    {
        error = "DiskCache capture could not materialize Sound-variable branches";
        return std::nullopt;
    }

    const auto identity = std::ranges::find(
        expanded.manifest.nodes,
        sourcePath,
        &talas::project::ExpandedSoundNodeIdentity::sourcePath);
    if (identity == expanded.manifest.nodes.end())
    {
        error = "DiskCache capture target no longer exists";
        return std::nullopt;
    }
    const auto target = std::ranges::find(
        expanded.document.graph.nodes,
        identity->flatNodeId,
        &talas::project::ProjectNode::instanceId);
    if (target == expanded.document.graph.nodes.end()
        || target->typeId != "talas.diskCache")
    {
        error = "DiskCache capture target is not an exact DiskCache";
        return std::nullopt;
    }
    const auto record = std::ranges::find(
        target->parameters,
        std::string_view{"record"},
        &talas::project::ProjectParameterExpression::parameterId);
    const auto fileName = std::ranges::find(
        target->parameters,
        std::string_view{"fileName"},
        &talas::project::ProjectParameterExpression::parameterId);
    if (record == target->parameters.end()
        || record->boolean != std::optional<bool>{true}
        || (fileName != target->parameters.end()
            && fileName->resource.has_value()
            && !fileName->resource->empty()
            && *fileName->resource != assetId))
    {
        error = "DiskCache capture target is not armed for the exact destination asset";
        return std::nullopt;
    }

    const talas::core::Connection* input = nullptr;
    for (const auto& connection : expanded.document.graph.connections)
    {
        if (connection.toNode != identity->flatNodeId
            || connection.toPort != "audioIn")
        {
            continue;
        }
        if (input != nullptr)
        {
            error = "DiskCache capture target has ambiguous audio input";
            return std::nullopt;
        }
        input = &connection;
    }
    if (input == nullptr)
    {
        error = "DiskCache capture target has no audio input";
        return std::nullopt;
    }
    const auto sourceNode = input->fromNode;
    const auto sourcePort = input->fromPort;

    std::unordered_multimap<std::string, std::string> incoming;
    incoming.reserve(expanded.document.graph.connections.size());
    for (const auto& connection : expanded.document.graph.connections)
        incoming.emplace(connection.toNode, connection.fromNode);
    std::unordered_set<std::string> retained;
    retained.reserve(expanded.document.graph.nodes.size());
    std::vector<std::string> pending{sourceNode};
    retained.insert(sourceNode);
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor)
    {
        const auto [first, last] = incoming.equal_range(pending[cursor]);
        for (auto current = first; current != last; ++current)
        {
            if (retained.insert(current->second).second)
                pending.push_back(current->second);
        }
    }

    auto capture = std::move(expanded.document);
    std::erase_if(capture.graph.nodes, [&](const auto& node) {
        return !retained.contains(node.instanceId);
    });
    std::erase_if(capture.graph.connections, [&](const auto& connection) {
        return !retained.contains(connection.fromNode)
            || !retained.contains(connection.toNode);
    });
    std::string outputId = "diskCacheCaptureOutput";
    for (std::size_t suffix = 2;
         retained.contains(outputId);
         ++suffix)
    {
        outputId = "diskCacheCaptureOutput-" + std::to_string(suffix);
    }
    capture.graph.nodes.push_back(
        {outputId, "talas.audioOutput", {}, {}});
    capture.graph.connections.push_back(
        {sourceNode, sourcePort, outputId, "audioIn"});
    capture.soundDefinitions.clear();
    capture.soundVariableLibrary = {};
    capture.timeline.loop.enabled = false;
    capture.timeline.score = {};
    return capture;
}

[[nodiscard]] bool directDirectory(
    const std::filesystem::path& path,
    std::string& error)
{
    std::error_code statusError;
    const auto status = std::filesystem::symlink_status(path, statusError);
    if (statusError)
    {
        error = "Unable to inspect DiskCache directory: "
            + statusError.message();
        return false;
    }
    if (std::filesystem::is_symlink(status)
        || !std::filesystem::is_directory(status))
    {
        error = "DiskCache path must be a direct project-local directory";
        return false;
    }
    return true;
}

[[nodiscard]] bool directPathComponents(
    const std::filesystem::path& path,
    std::string& error)
{
    std::error_code absoluteError;
    const auto absolute = std::filesystem::absolute(
        path, absoluteError).lexically_normal();
    if (absoluteError || absolute.empty())
    {
        error = "Unable to resolve direct DiskCache path";
        if (absoluteError)
            error += ": " + absoluteError.message();
        return false;
    }

    const auto relative = absolute.relative_path();
    auto component = relative.begin();
    const auto end = relative.end();
    if (component == end)
    {
        error = "DiskCache path has no direct filesystem components";
        return false;
    }

    // Resolve only the privileged volume/top-level alias (for example macOS
    // /var -> /private/var). Every project-controlled descendant is inspected
    // without following it.
    auto current = absolute.root_path() / *component;
    std::error_code topLevelError;
    current = std::filesystem::weakly_canonical(
        current, topLevelError);
    if (topLevelError)
    {
        error = "Unable to resolve DiskCache volume authority: "
            + topLevelError.message();
        return false;
    }
    ++component;
    for (; component != end; ++component)
    {
        current /= *component;
        std::error_code statusError;
        const auto status = std::filesystem::symlink_status(
            current, statusError);
        if (statusError)
        {
            error = "Unable to inspect DiskCache path component: "
                + statusError.message();
            return false;
        }
        if (std::filesystem::is_symlink(status))
        {
            error = "DiskCache path contains an indirect symlink component";
            return false;
        }
    }
    return true;
}
}

PreparedDiskCacheCapture::PreparedDiskCacheCapture(
    talas::project::DiskCacheCapturePublication editorPublication,
    std::unique_ptr<AudioFilePublication> filePublication,
    std::uint64_t expectedDeviceGeneration)
    : editorPublication_(std::move(editorPublication))
    , publishedAssetAuthority_(editorPublication_.asset)
    , filePublication_(std::move(filePublication))
    , expectedDeviceGeneration_(expectedDeviceGeneration)
{
}

const talas::project::DiskCacheCapturePublication&
PreparedDiskCacheCapture::editorPublication() const noexcept
{
    return editorPublication_;
}

talas::project::DiskCacheCapturePublication
PreparedDiskCacheCapture::takeEditorPublication() noexcept
{
    return std::move(editorPublication_);
}

const std::filesystem::path&
PreparedDiskCacheCapture::destination() const noexcept
{
    return filePublication_->destination();
}

const std::filesystem::path&
PreparedDiskCacheCapture::temporaryPath() const noexcept
{
    return filePublication_->temporaryPath();
}

std::uint64_t PreparedDiskCacheCapture::expectedDeviceGeneration() const noexcept
{
    return expectedDeviceGeneration_;
}

bool PreparedDiskCacheCapture::publishFile(std::string& error) noexcept
{
    try
    {
        if (published_)
        {
            error = "DiskCache capture file was already published";
            return false;
        }
        if (filePublication_ == nullptr)
        {
            error = "DiskCache capture has no prepared file publication";
            return false;
        }
        if (!filePublication_->publish(error))
            return false;
        published_ = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        try
        {
            error = "Unable to publish DiskCache capture file: ";
            error += exception.what();
        }
        catch (...)
        {
        }
        return false;
    }
    catch (...)
    {
        try
        {
            error = "Unable to publish DiskCache capture file";
        }
        catch (...)
        {
        }
        return false;
    }
}

bool PreparedDiskCacheCapture::retractPublishedFile(
    std::string& error) noexcept
{
    try
    {
        error.clear();
        if (!published_ || retracted_ || filePublication_ == nullptr)
        {
            error = "DiskCache capture has no retractable published file";
            return false;
        }
        std::vector<std::uint8_t> bytes;
        if (!filePublication_->readDestinationFile(
                maximumDiskCacheCaptureWavBytes,
                bytes,
                error))
        {
            return false;
        }
        const auto inspected = talas::project::inspectProjectAsset(
            publishedAssetAuthority_,
            bytes,
            filePublication_->destination());
        if (!inspected.ok()
            || inspected.asset != publishedAssetAuthority_)
        {
            error = "Published DiskCache bytes changed before rollback";
            return false;
        }
        if (!filePublication_->retract(error))
        {
            return false;
        }
        retracted_ = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        try
        {
            error = "Unable to retract published DiskCache file: ";
            error += exception.what();
        }
        catch (...)
        {
        }
        return false;
    }
    catch (...)
    {
        try
        {
            error = "Unable to retract published DiskCache file";
        }
        catch (...)
        {
        }
        return false;
    }
}

DiskCacheCaptureResult prepareProjectDiskCacheCapture(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    DiskCacheCaptureOptions options)
{
    DiskCacheCaptureResult result;
    try
    {
        if (options.projectFilePath.empty()
            || options.projectFilePath.filename().empty()
            || options.sourcePath.empty()
            || options.sourcePath.size() > 4096
            || options.assetId.empty() || options.assetId.size() > 512
            || !talas::project::isValidDiskCacheAssetPath(
                options.relativeAssetPath)
            || options.assetGeneration == 0
            || options.assetGeneration
                > talas::project::maximumProjectAssetJsonInteger
            || options.expectedDocumentAuthorityGeneration == 0
            || options.expectedDeviceGeneration == 0
            || !std::isfinite(options.sampleRate)
            || std::floor(options.sampleRate) != options.sampleRate
            || options.sampleRate
                < talas::project::minimumDiskCacheCaptureSampleRate
            || options.sampleRate
                > talas::project::maximumDiskCacheCaptureSampleRate
            || options.maximumBlockFrames == 0
            || options.maximumBlockFrames > 65536)
        {
            result.error = "DiskCache capture request is invalid";
            return result;
        }
        if (options.expectedCanonicalDocumentBytes
            != talas::project::serializeProjectDocument(document, registry))
        {
            result.error = "DiskCache capture canonical project authority is stale";
            return result;
        }
        const auto generation =
            talas::project::nextDiskCacheCaptureAssetGeneration(
                document, options.assetId);
        if (!generation.ok()
            || *generation.generation != options.assetGeneration)
        {
            result.error = "DiskCache destination asset authority is unrelated, exhausted, or stale";
            return result;
        }
        if (cancelled(options))
        {
            result.outcome = DiskCacheCaptureOutcome::Cancelled;
            result.error = "DiskCache capture cancelled";
            return result;
        }

        const auto range = talas::project::diskCacheCaptureRange(
            document.timeline.loop, options.sampleRate);
        if (!range.ok())
        {
            result.error = "DiskCache capture requires one exact bounded Timeline loop";
            return result;
        }
        std::string fingerprintError;
        const auto fingerprint =
            talas::project::projectDiskCacheSourceFingerprint(
                document,
                registry,
                options.sourcePath,
                static_cast<std::uint32_t>(options.sampleRate),
                fingerprintError);
        if (fingerprint.empty())
        {
            result.error = std::move(fingerprintError);
            return result;
        }
        if (cancelled(options))
        {
            result.outcome = DiskCacheCaptureOutcome::Cancelled;
            result.error = "DiskCache capture cancelled";
            return result;
        }

        auto captureDocument = captureSourceDocument(
            document,
            registry,
            options.sourcePath,
            options.assetId,
            result.error);
        if (!captureDocument.has_value())
            return result;

        if (!directPathComponents(options.projectFilePath, result.error))
            return result;
        std::error_code projectStatusError;
        const auto projectStatus = std::filesystem::symlink_status(
            options.projectFilePath,
            projectStatusError);
        if (projectStatusError
            || std::filesystem::is_symlink(projectStatus)
            || !std::filesystem::is_regular_file(projectStatus))
        {
            result.error = "DiskCache capture requires a direct saved project file";
            return result;
        }
        const auto projectDirectory =
            options.projectFilePath.parent_path();
        if (!directDirectory(projectDirectory, result.error))
            return result;
        const auto relativePath = talas::project::pathFromUtf8(
            options.relativeAssetPath);
        if (relativePath.parent_path()
            != std::filesystem::path{"DiskCache"})
        {
            result.error = "DiskCache capture destination must be one direct file under DiskCache";
            return result;
        }
        const auto destination = projectDirectory / relativePath;
        std::vector<std::filesystem::path> protectedFiles{
            options.projectFilePath};
        protectedFiles.reserve(document.assets.size() + 1U);
        for (const auto& asset : document.assets)
        {
            protectedFiles.push_back(
                resolveProjectAssetPath(
                    asset,
                    std::optional<std::filesystem::path>{
                        options.projectFilePath}));
        }
        if (audioFileDestinationConflicts(destination, protectedFiles, {}))
        {
            result.error = "DiskCache destination conflicts with current project authority";
            return result;
        }
        auto filePublication = AudioFilePublication::prepareContained(
            projectDirectory,
            relativePath,
            ExistingAudioFilePolicy::Reject,
            result.error);
        if (!filePublication)
            return result;

        auto loaded = loadReferencedProjectResourceSnapshots(
            *captureDocument,
            registry,
            std::optional<std::filesystem::path>{options.projectFilePath},
            {},
            options.shouldCancel);
        if (!loaded.ok())
        {
            if (loaded.wasCancelled)
            {
                result.outcome = DiskCacheCaptureOutcome::Cancelled;
                result.error = "DiskCache capture cancelled";
            }
            else
            {
                result.error = loaded.failure.has_value()
                    ? describeProjectAssetLoadFailure(*loaded.failure)
                    : "DiskCache capture resource loading failed";
            }
            return result;
        }

        talas::runtime::RenderSettings settings;
        settings.sampleRate = options.sampleRate;
        settings.maxBlockFrames = options.maximumBlockFrames;
        settings.resourceSnapshots = std::move(loaded.snapshots);
        auto preparedSettings = talas::runtime::prepareProjectRenderSettings(
            *captureDocument,
            registry,
            std::move(settings));
        if (!preparedSettings.ok())
        {
            result.error = std::move(preparedSettings.error);
            return result;
        }
        auto compiled = talas::runtime::compileProject(
            *captureDocument,
            registry,
            std::move(*preparedSettings.settings));
        if (!compiled.ok())
        {
            result.error = "DiskCache capture source compilation failed";
            for (const auto& diagnostic : compiled.diagnostics)
                result.error += " | " + diagnostic.message;
            return result;
        }
        if (cancelled(options))
        {
            result.outcome = DiskCacheCaptureOutcome::Cancelled;
            result.error = "DiskCache capture cancelled";
            return result;
        }

        auto* output = filePublication->openTemporaryFile(result.error);
        if (output == nullptr)
            return result;
        Pcm16WavStreamWriter writer(
            output,
            talas::project::pathToUtf8(filePublication->temporaryPath()),
            options.sampleRate,
            1,
            static_cast<std::size_t>(range.range->frameCount()));
        if (!writer.ready())
        {
            result.error = std::string{writer.error()};
            return result;
        }
        talas::runtime::OfflineRenderSession session{*compiled.graph};
        std::uint64_t renderedFrames = 0;
        while (renderedFrames < range.range->endFrame)
        {
            if (cancelled(options))
            {
                writer.abandon();
                result.outcome = DiskCacheCaptureOutcome::Cancelled;
                result.error = "DiskCache capture cancelled";
                return result;
            }
            const auto boundary = renderedFrames < range.range->startFrame
                ? range.range->startFrame
                : range.range->endFrame;
            const auto block = static_cast<std::size_t>(std::min<std::uint64_t>(
                options.maximumBlockFrames,
                boundary - renderedFrames));
            auto rendered = session.render(block);
            if (!rendered.ok() || rendered.channels.size() != 1
                || rendered.channels.front().size() != block)
            {
                writer.abandon();
                result.error = "DiskCache capture source must render exact mono blocks";
                return result;
            }
            if (renderedFrames >= range.range->startFrame
                && !writer.write(rendered.channels))
            {
                result.error = std::string{writer.error()};
                return result;
            }
            renderedFrames += block;
            result.renderedFrames = renderedFrames;
            if (options.reportProgress)
            {
                options.reportProgress(
                    static_cast<double>(renderedFrames)
                    / static_cast<double>(range.range->endFrame));
            }
        }
        if (!writer.finish())
        {
            result.error = std::string{writer.error()};
            return result;
        }
        if (cancelled(options))
        {
            result.outcome = DiskCacheCaptureOutcome::Cancelled;
            result.error = "DiskCache capture cancelled";
            return result;
        }

        talas::project::ProjectAsset manifest;
        manifest.id = options.assetId;
        manifest.kind = talas::project::ProjectAssetKind::Audio;
        manifest.path = options.relativeAssetPath;
        manifest.disposition =
            talas::project::ProjectAssetDisposition::Referenced;
        manifest.channelCount = 1;
        manifest.sampleRate = options.sampleRate;
        manifest.frameCount = range.range->frameCount();
        manifest.encoding = "pcm16-wav";
        manifest.generation = options.assetGeneration;
        manifest.provenance =
            talas::project::makeDiskCacheCaptureProvenance(fingerprint);
        std::vector<std::uint8_t> stagedBytes;
        if (!filePublication->readTemporaryFile(
                maximumDiskCacheCaptureWavBytes,
                stagedBytes,
                result.error))
        {
            return result;
        }
        auto inspected = talas::project::inspectProjectAsset(
            manifest,
            stagedBytes,
            filePublication->temporaryPath(),
            options.shouldCancel);
        if (!inspected.ok())
        {
            if (inspected.error->code
                == talas::project::ProjectAssetInspectionErrorCode::Cancelled)
            {
                result.outcome = DiskCacheCaptureOutcome::Cancelled;
                result.error = "DiskCache capture cancelled";
            }
            else
            {
                result.error = inspected.error->message;
            }
            return result;
        }
        if (talas::project::diskCacheAssetAuthority(inspected.asset)
            != talas::project::DiskCacheAssetAuthority::CapturedPlayback)
        {
            result.error = "DiskCache staged bytes failed captured-asset authority";
            return result;
        }

        talas::project::DiskCacheCapturePublication editorPublication;
        editorPublication.expectedDocumentAuthorityGeneration =
            options.expectedDocumentAuthorityGeneration;
        editorPublication.expectedCanonicalDocumentBytes =
            std::move(options.expectedCanonicalDocumentBytes);
        editorPublication.sourcePath = std::move(options.sourcePath);
        editorPublication.asset = std::move(inspected.asset);
        result.prepared = std::make_unique<PreparedDiskCacheCapture>(
            std::move(editorPublication),
            std::move(filePublication),
            options.expectedDeviceGeneration);
        result.outcome = DiskCacheCaptureOutcome::Prepared;
        result.error.clear();
        return result;
    }
    catch (const std::bad_alloc&)
    {
        result.error = "DiskCache capture allocation failed";
    }
    catch (const std::length_error&)
    {
        result.error = "DiskCache capture exceeded supported capacity";
    }
    catch (const std::exception& exception)
    {
        result.error = "DiskCache capture failed: ";
        result.error += exception.what();
    }
    return result;
}
}
