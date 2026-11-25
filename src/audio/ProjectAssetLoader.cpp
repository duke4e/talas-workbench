#include "talas/audio/ProjectAssetLoader.h"

#include "talas/audio/ResourceLoader.h"
#include "talas/core/MidiInputSource.h"
#include "talas/core/StaticResourceList.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectMidiVoiceIslands.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectSoundExpansion.h"

#include <ranges>
#include <sstream>
#include <utility>

namespace talas::audio
{
namespace
{
// The other sample files in the folder of a KeyMappedMultisample's
// FirstSample, in the order of their names: a
// table each, naming the bank's first sample. Folders inside it and files
// that are no samples are passed over.
[[nodiscard]] std::string loadBankSamples(
    const talas::project::ProjectAsset& asset,
    const std::filesystem::path& firstSample,
    const talas::project::ProjectAssetInspectionCancellationCheck& shouldCancel,
    bool& cancelled,
    std::vector<talas::core::ResourceSnapshot>& bank)
{
    constexpr std::size_t maximumBankSamples = 128;
    std::vector<std::filesystem::path> files;
    std::error_code listError;
    for (std::filesystem::directory_iterator entry{firstSample.parent_path(), listError}, end;
         !listError && entry != end;
         entry.increment(listError))
    {
        std::error_code typeError;
        if (entry->is_regular_file(typeError) && !typeError
            && entry->path().filename() != firstSample.filename()
            && isSampleFilePath(entry->path()))
        {
            files.push_back(entry->path());
        }
    }
    if (listError)
        return "KeyMappedMultisample folder could not be listed: " + listError.message();
    if (files.size() >= maximumBankSamples)
        return "KeyMappedMultisample folder holds more than 128 samples";
    std::ranges::sort(files, {}, [](const std::filesystem::path& path) {
        return talas::project::pathToUtf8(path.filename());
    });
    for (const auto& path : files)
    {
        auto table = loadTableResource(path, shouldCancel);
        if (table.cancelled)
        {
            cancelled = true;
            return table.error.empty() ? std::string{"asset loading cancelled"} : table.error;
        }
        if (!table.ok())
            continue;
        const auto name = talas::project::pathToUtf8(path.filename());
        talas::core::ResourceSnapshot member;
        member.id = asset.id + "#" + name;
        member.kind = talas::core::ResourceSnapshotKind::Table;
        member.generation = asset.generation;
        member.contentHash = std::move(table.contentHash);
        member.resolvedPath = path;
        member.provenance = asset.provenance;
        member.tableSamples = std::move(table.samples);
        member.tableRightSamples = std::move(table.rightSamples);
        member.tableSourceSampleRate = table.sourceSampleRate;
        member.tableForwardLoop = table.forwardLoop;
        member.tableSamplePitch = table.samplePitch;
        member.tableKeyRange = table.keyRange;
        member.tableMarkers = std::move(table.markers);
        member.tableBank = asset.id;
        member.tableBankName = name;
        bank.push_back(std::move(member));
    }
    return {};
}

std::optional<ProjectAssetReference> firstReference(
    std::span<const ProjectAssetReference> references,
    std::string_view assetId)
{
    const auto found = std::ranges::find(
        references,
        assetId,
        &ProjectAssetReference::assetId);
    return found == references.end()
        ? std::optional<ProjectAssetReference>{}
        : std::optional<ProjectAssetReference>{*found};
}

[[nodiscard]] ProjectAssetReferenceResolutionFailure expansionFailure(
    const talas::project::ProjectSoundExpansionResult& expanded)
{
    ProjectAssetReferenceResolutionFailure failure;
    if (expanded.validation.errors.empty())
    {
        failure.message = "Project Sound expansion failed while resolving reachable assets";
        return failure;
    }
    const auto& error = expanded.validation.errors.front();
    failure.nodeId = error.nodeId;
    failure.structuralSourcePath = error.nodeId;
    failure.message = error.message;
    return failure;
}

[[nodiscard]] ProjectAssetReferenceResolutionFailure validationFailure(
    const talas::core::ValidationResult& validation,
    const talas::project::ProjectSoundExpansionManifest& manifest)
{
    ProjectAssetReferenceResolutionFailure failure;
    if (validation.errors.empty())
    {
        failure.message = "Project Sound-variable branch materialization failed while resolving reachable assets";
        return failure;
    }
    const auto& error = validation.errors.front();
    failure.nodeId = error.nodeId;
    const auto identity = std::ranges::find(
        manifest.nodes,
        error.nodeId,
        &talas::project::ExpandedSoundNodeIdentity::flatNodeId);
    failure.structuralSourcePath = identity == manifest.nodes.end()
        ? error.nodeId
        : identity->sourcePath;
    failure.message = error.message;
    return failure;
}
}

ProjectAssetReferencesResult
reachableProjectAssetReferences(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    auto expanded = talas::project::expandProjectSounds(
        document,
        registry);
    if (!expanded.ok())
        return {{}, expansionFailure(expanded)};
    auto midiVoiceTopology =
        talas::project::discoverProjectMidiVoiceIslands(
            expanded.document,
            expanded.manifest,
            {});
    if (!midiVoiceTopology.ok())
        midiVoiceTopology = talas::project::discoverProjectMidiVoiceHierarchyTemplates(
            expanded.document, expanded.manifest);
    if (!midiVoiceTopology.ok())
    {
        return {{}, ProjectAssetReferenceResolutionFailure{
                        midiVoiceTopology.errorNodeId,
                        midiVoiceTopology.errorPortId,
                        midiVoiceTopology.errorNodeId,
                        midiVoiceTopology.error}};
    }
    const auto materialized =
        talas::project::materializeProjectSoundVariableBranches(
            document,
            expanded,
            midiVoiceTopology.islands);
    if (!materialized.ok())
    {
        return {{}, validationFailure(materialized, expanded.manifest)};
    }

    ProjectAssetReferencesResult result;
    auto& references = result.references;
    for (const auto& node : expanded.document.graph.nodes)
    {
        const auto identity = std::ranges::find(
            expanded.manifest.nodes,
            node.instanceId,
            &talas::project::ExpandedSoundNodeIdentity::flatNodeId);
        const auto structuralSourcePath =
            identity == expanded.manifest.nodes.end()
            ? node.instanceId
            : identity->sourcePath;
        const auto* descriptor = registry.findNodeType(node.typeId);
        const bool polyphonicMapper = node.typeId == "talas.midiMapper"
            && std::ranges::find(
                midiVoiceTopology.islands,
                node.instanceId,
                &talas::project::ProjectMidiVoiceIsland::flatOwnerNodeId)
                != midiVoiceTopology.islands.end();
        bool midiFileIsInactive = node.typeId == "talas.midiMapper";
        if (node.typeId == "talas.midiVoice" || polyphonicMapper)
        {
            std::string_view sourceText;
            if (descriptor != nullptr)
            {
                const auto sourceDefault = std::ranges::find(
                    descriptor->parameters, "source", &talas::core::ParameterDescriptor::id);
                if (sourceDefault != descriptor->parameters.end())
                    sourceText = sourceDefault->defaultText;
            }
            std::size_t sourceCount = 0;
            bool sourceHasText = true;
            for (const auto& parameter : node.parameters)
            {
                if (parameter.parameterId != "source")
                    continue;
                ++sourceCount;
                sourceHasText = parameter.text.has_value();
                if (sourceHasText)
                    sourceText = *parameter.text;
            }
            const auto source = talas::core::parseMidiInputSource(sourceText);
            if (sourceCount > 1 || !sourceHasText || !source)
            {
                return {{}, ProjectAssetReferenceResolutionFailure{
                    node.instanceId, "source", structuralSourcePath,
                    "MIDI Input source must be Live MIDI, MIDI File or Script (blank selects Live MIDI)",
                }};
            }
            midiFileIsInactive = *source != talas::core::MidiInputSource::File;
        }
        for (const auto& parameter : node.parameters)
        {
            if (!parameter.resource.has_value()
                || parameter.resource->empty()
                || (midiFileIsInactive && parameter.parameterId == "midiFile"))
            {
                continue;
            }
            auto selectedResource = *parameter.resource;
            if (talas::core::beginsStaticStringResourceSelection(
                    selectedResource))
            {
                const auto selected =
                    talas::core::selectBoundedStaticStringResource(
                        selectedResource);
                if (!selected.has_value())
                {
                    return {{}, ProjectAssetReferenceResolutionFailure{
                        node.instanceId,
                        parameter.parameterId,
                        structuralSourcePath,
                        "Resource collection must be a bounded literal String array with a one-based in-range at: selector",
                    }};
                }
                selectedResource = *selected;
            }
            references.push_back({
                std::move(selectedResource),
                node.instanceId,
                parameter.parameterId,
                structuralSourcePath,
            });
            // Every sample in FirstSample's folder makes a bank;
            // projects before format 82 played FirstSample alone.
            if (node.typeId == "talas.keyMappedMultisample" && parameter.parameterId == "firstSample")
            {
                const auto samples = std::ranges::find(node.parameters, std::string_view{"samples"},
                    &talas::project::ProjectParameterExpression::parameterId);
                references.back().bank = samples == node.parameters.end()
                    || samples->choice != std::optional<std::string>{"legacyFirstSampleOnly"};
            }
        }
        // A DiskSplicer's Segments name a file first on each line.
        if (node.typeId == "talas.diskSplicer")
        {
            for (const auto& parameter : node.parameters)
            {
                if (parameter.parameterId != "segments" || !parameter.text.has_value())
                    continue;
                std::istringstream lines{*parameter.text};
                for (std::string line; std::getline(lines, line);)
                {
                    auto words = talas::core::splitQuotedWords(line);
                    if (words && !words->empty())
                        references.push_back({std::move(words->front()), node.instanceId, "segments", structuralSourcePath});
                }
            }
        }
        if (descriptor != nullptr)
        {
            for (const auto& parameterDescriptor : descriptor->parameters)
            {
                if (parameterDescriptor.resourceAuthority
                    != talas::core::ParameterResourceAuthority::StaticResourceList)
                {
                    continue;
                }
                const auto parameter = std::ranges::find(
                    node.parameters,
                    parameterDescriptor.id,
                    &talas::project::ProjectParameterExpression::parameterId);
                if (parameter == node.parameters.end()
                    || !parameter->text.has_value())
                {
                    continue;
                }
                auto resourceIds =
                    talas::core::parseBoundedStaticResourceList(
                        *parameter->text);
                if (!resourceIds.has_value())
                {
                    return {
                        {},
                        ProjectAssetReferenceResolutionFailure{
                            node.instanceId,
                            parameterDescriptor.id,
                            structuralSourcePath,
                            descriptor->displayName + " "
                                + parameterDescriptor.id
                                + " must contain a bounded static resource list",
                        },
                    };
                }
                for (auto resourceId : *resourceIds)
                {
                    references.push_back({
                        std::move(resourceId),
                        node.instanceId,
                        parameterDescriptor.id,
                        structuralSourcePath,
                    });
                }
            }
        }
        if (node.typeId != "talas.sampleMapPlayer")
            continue;

        const auto mapParameter = std::ranges::find_if(
            node.parameters,
            [](const auto& parameter) {
                return parameter.parameterId == "sampleMapId"
                    && parameter.text.has_value()
                    && !parameter.text->empty();
            });
        if (mapParameter == node.parameters.end())
            continue;
        const auto* map = talas::project::findProjectSampleMap(
            document,
            *mapParameter->text);
        if (map == nullptr)
            continue;
        for (const auto& region : map->regions)
        {
            references.push_back({
                region.assetId,
                node.instanceId,
                "sampleMapId",
                structuralSourcePath
                    + "/sampleMap[" + map->mapId
                    + "]/region[" + region.regionId + "]",
            });
        }
    }
    for (const auto& clip : document.timeline.score.clips)
    {
        if (!clip.renderArtifact.has_value())
            continue;
        for (const auto& assetId :
             clip.renderArtifact->channelAssetIds)
        {
            references.push_back({
                assetId,
                "timeline.score.clip[" + clip.clipId + "]",
                "renderArtifact",
                "timeline/score/clip[" + clip.clipId + "]",
            });
        }
    }
    return result;
}

ProjectAssetReferencesResult
referencedProjectAssetParameters(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    return reachableProjectAssetReferences(
        document,
        registry);
}

std::string describeProjectAssetLoadFailure(
    const ProjectAssetLoadFailure& failure)
{
    if (failure.assetId.empty())
    {
        return "Asset reference resolution failed: "
            + failure.message;
    }
    return "Asset '" + failure.assetId
        + "' failed to load from "
        + talas::project::pathToUtf8(failure.resolvedPath)
        + ": " + failure.message;
}

std::filesystem::path resolveProjectAssetPath(
    const talas::project::ProjectAsset& asset,
    const std::optional<std::filesystem::path>& projectFilePath)
{
    return talas::project::resolveProjectAssetPath(asset, projectFilePath);
}

ProjectResourceSnapshotsLoadResult
loadReferencedProjectResourceSnapshots(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>& projectFilePath,
    std::span<const std::string> excludedAssetIds,
    talas::project::ProjectAssetInspectionCancellationCheck
        shouldCancel)
{
    ProjectResourceSnapshotsLoadResult result;
    auto resolvedReferences = reachableProjectAssetReferences(
        document,
        registry);
    if (!resolvedReferences.ok())
    {
        const auto& failure = *resolvedReferences.failure;
        result.failure = ProjectAssetLoadFailure{
            {},
            {},
            failure.nodeId,
            failure.parameterId,
            failure.structuralSourcePath,
            failure.message,
            false,
        };
        return result;
    }
    const auto& references = resolvedReferences.references;
    for (const auto& asset : document.assets)
    {
        if (std::ranges::find(excludedAssetIds, asset.id) != excludedAssetIds.end())
            continue;
        const auto reference = firstReference(references, asset.id);
        if (!reference.has_value())
            continue;

        const auto resolvedPath =
            talas::audio::resolveProjectAssetPath(
                asset,
                projectFilePath);
        std::string error;
        bool replacementMayBeAccepted = false;

        auto inspected = error.empty()
            ? talas::project::inspectProjectAsset(
                  asset,
                  resolvedPath,
                  shouldCancel)
            : talas::project::ProjectAssetInspectionResult{};
        if (error.empty() && !inspected.ok())
        {
            result.wasCancelled =
                inspected.error->code
                == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
            replacementMayBeAccepted =
                inspected.error->code
                == talas::project::ProjectAssetInspectionErrorCode::ContentMismatch;
            error = inspected.error->message;
        }

        talas::core::ResourceSnapshot snapshot;
        snapshot.id = asset.id;
        snapshot.generation = asset.generation;
        snapshot.resolvedPath = resolvedPath;
        snapshot.projectAssetPath = asset.path;
        snapshot.provenance = asset.provenance;
        if (error.empty())
        {
            snapshot.contentHash = inspected.asset.contentHash;
            snapshot.channelCount = inspected.asset.channelCount;
            snapshot.sampleRate = inspected.asset.sampleRate;
            snapshot.frameCount = inspected.asset.frameCount;
            snapshot.encoding = inspected.asset.encoding;
            if (asset.kind == talas::project::ProjectAssetKind::Audio)
                snapshot.audioBytes = std::move(inspected.inspectedBytes);
        }
        std::string loadedContentHash;

        if (error.empty()
            && asset.kind
                == talas::project::ProjectAssetKind::Table)
        {
            snapshot.kind =
                talas::core::ResourceSnapshotKind::Table;
            auto table = loadTableResource(
                resolvedPath,
                shouldCancel);
            if (table.ok())
            {
                snapshot.tableSamples = std::move(table.samples);
                snapshot.tableRightSamples = std::move(table.rightSamples);
                snapshot.tableSourceSampleRate = table.sourceSampleRate;
                snapshot.tableForwardLoop = table.forwardLoop;
                snapshot.tableSamplePitch = table.samplePitch;
                snapshot.tableKeyRange = table.keyRange;
                snapshot.tableMarkers = std::move(table.markers);
                loadedContentHash = std::move(table.contentHash);
            }
            else
            {
                result.wasCancelled = table.cancelled;
                error = std::move(table.error);
            }
        }
        else if (error.empty()
                 && asset.kind
                     == talas::project::ProjectAssetKind::Binary)
        {
            snapshot.kind =
                talas::core::ResourceSnapshotKind::Binary;
            auto binary = loadBinaryResource(
                resolvedPath,
                shouldCancel);
            if (binary.ok())
            {
                snapshot.binaryBytes = std::move(binary.bytes);
                loadedContentHash = std::move(binary.contentHash);
            }
            else
            {
                result.wasCancelled = binary.cancelled;
                error = std::move(binary.error);
            }
        }
        else if (error.empty()
                 && asset.kind
                     == talas::project::ProjectAssetKind::PartialSpectrum)
        {
            snapshot.kind =
                talas::core::ResourceSnapshotKind::PartialSpectrum;
            auto partial = loadPartialTrackResource(
                resolvedPath,
                shouldCancel);
            if (partial.ok())
            {
                snapshot.partialTrackBytes = std::move(partial.bytes);
                loadedContentHash = std::move(partial.contentHash);
            }
            else
            {
                result.wasCancelled = partial.cancelled;
                error = std::move(partial.error);
            }
        }
        else if (error.empty())
        {
            snapshot.kind =
                talas::core::ResourceSnapshotKind::Audio;
            if (!snapshot.audioBytes.empty())
            {
                loadedContentHash = snapshot.contentHash;
            }
            else
            {
                const auto stableHash =
                    talas::project::calculateProjectAssetSha256(
                        resolvedPath,
                        shouldCancel);
                if (stableHash.ok())
                {
                    loadedContentHash =
                        std::move(stableHash.contentHash);
                }
                else
                {
                    result.wasCancelled =
                        stableHash.error.has_value()
                        && stableHash.error->code
                            == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
                    error = stableHash.error.has_value()
                        ? std::move(stableHash.error->message)
                        : "audio asset hash verification failed";
                }
            }
        }

        if (error.empty()
            && shouldCancel
            && shouldCancel())
        {
            error = "asset loading cancelled";
            result.wasCancelled = true;
        }
        else if (error.empty()
                 && loadedContentHash
                     != snapshot.contentHash)
        {
            error = "asset changed while preparing its immutable resource snapshot";
            replacementMayBeAccepted = true;
        }

        std::vector<talas::core::ResourceSnapshot> bank;
        if (error.empty()
            && asset.kind == talas::project::ProjectAssetKind::Table
            && std::ranges::any_of(references, [&](const ProjectAssetReference& candidate) {
                   return candidate.assetId == asset.id && candidate.bank;
               }))
        {
            snapshot.tableBank = asset.id;
            snapshot.tableBankName = talas::project::pathToUtf8(resolvedPath.filename());
            error = loadBankSamples(asset, resolvedPath, shouldCancel, result.wasCancelled, bank);
        }

        if (!error.empty())
        {
            result.failure = ProjectAssetLoadFailure{
                asset.id,
                resolvedPath,
                reference->nodeId,
                reference->parameterId,
                reference->structuralSourcePath,
                std::move(error),
                replacementMayBeAccepted,
            };
            result.snapshots.clear();
            return result;
        }
        result.snapshots.push_back(
            std::make_shared<
                const talas::core::ResourceSnapshot>(
                std::move(snapshot)));
        for (auto& member : bank)
        {
            result.snapshots.push_back(
                std::make_shared<const talas::core::ResourceSnapshot>(std::move(member)));
        }
    }
    return result;
}

ProjectAssetsLoadResult loadReferencedProjectAssets(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>& projectFilePath,
    std::span<const std::string> excludedAssetIds,
    talas::project::ProjectAssetInspectionCancellationCheck
        shouldCancel)
{
    auto loaded = loadReferencedProjectResourceSnapshots(
        document,
        registry,
        projectFilePath,
        excludedAssetIds,
        std::move(shouldCancel));
    ProjectAssetsLoadResult result;
    result.failure = std::move(loaded.failure);
    result.wasCancelled = loaded.wasCancelled;
    if (!loaded.ok())
        return result;
    result.assets.reserve(loaded.snapshots.size());
    for (const auto& snapshot : loaded.snapshots)
    {
        LoadedProjectAsset asset;
        asset.id = snapshot->id;
        switch (snapshot->kind)
        {
        case talas::core::ResourceSnapshotKind::Table:
            asset.kind = talas::project::ProjectAssetKind::Table;
            break;
        case talas::core::ResourceSnapshotKind::Binary:
            asset.kind = talas::project::ProjectAssetKind::Binary;
            break;
        case talas::core::ResourceSnapshotKind::Audio:
            asset.kind = talas::project::ProjectAssetKind::Audio;
            break;
        case talas::core::ResourceSnapshotKind::PartialSpectrum:
            asset.kind = talas::project::ProjectAssetKind::PartialSpectrum;
            break;
        }
        asset.resolvedPath = snapshot->resolvedPath;
        asset.tableSamples = snapshot->tableSamples;
        asset.tableRightSamples = snapshot->tableRightSamples;
        asset.tableSourceSampleRate = snapshot->tableSourceSampleRate;
        asset.tableForwardLoop = snapshot->tableForwardLoop;
        asset.tableSamplePitch = snapshot->tableSamplePitch;
        asset.binaryBytes = snapshot->binaryBytes;
        asset.partialTrackBytes = snapshot->partialTrackBytes;
        asset.channelCount = snapshot->channelCount;
        asset.sampleRate = snapshot->sampleRate;
        asset.frameCount = snapshot->frameCount;
        asset.encoding = snapshot->encoding;
        result.assets.push_back(std::move(asset));
    }
    return result;
}
}
