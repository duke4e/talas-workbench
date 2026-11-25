#include "talas/project/ProjectDiskCache.h"

#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectMidiVoiceIslands.h"
#include "talas/project/ProjectScore.h"
#include "talas/project/ProjectSoundExpansion.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace talas::project
{
namespace
{
[[nodiscard]] bool identifierContinue(char value) noexcept
{
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9')
        || value == '_';
}

[[nodiscard]] bool callsDefinition(
    std::string_view source,
    std::string_view name) noexcept
{
    for (auto offset = source.find(name);
         offset != std::string_view::npos;
         offset = source.find(name, offset + 1U))
    {
        if ((offset != 0 && identifierContinue(source[offset - 1U]))
            || (offset + name.size() < source.size()
                && identifierContinue(source[offset + name.size()])))
        {
            continue;
        }
        auto cursor = offset + name.size();
        while (cursor < source.size()
               && (source[cursor] == ' ' || source[cursor] == '\t'
                   || source[cursor] == '\r' || source[cursor] == '\n'))
        {
            ++cursor;
        }
        if (cursor < source.size() && source[cursor] == '(')
            return true;
    }
    return false;
}

[[nodiscard]] bool referencesVariable(
    std::string_view source,
    std::string_view id) noexcept
{
    const auto token = std::string{"@"} + std::string{id};
    for (auto offset = source.find(token);
         offset != std::string_view::npos;
         offset = source.find(token, offset + 1U))
    {
        const auto end = offset + token.size();
        if (end == source.size() || !identifierContinue(source[end]))
            return true;
    }
    return false;
}

void appendParameterSources(
    const ProjectParameterExpression& parameter,
    std::vector<std::string>& sources)
{
    if (parameter.expression.has_value())
        sources.push_back(*parameter.expression);
    if (parameter.text.has_value())
        sources.push_back(*parameter.text);
}

void appendGraphSources(
    const ProjectGraph& graph,
    const talas::core::NodeRegistry& registry,
    std::vector<std::string>& sources)
{
    for (const auto& node : graph.nodes)
    {
        for (const auto& parameter : node.parameters)
            appendParameterSources(parameter, sources);
        const auto* descriptor = registry.findNodeType(node.typeId);
        if (descriptor == nullptr)
            continue;
        for (const auto& parameter : descriptor->parameters)
        {
            if (std::ranges::find(
                    node.parameters,
                    parameter.id,
                    &ProjectParameterExpression::parameterId)
                != node.parameters.end())
            {
                continue;
            }
            if (parameter.kind == talas::core::ParameterKind::Expression
                && !parameter.defaultExpression.empty())
            {
                sources.push_back(parameter.defaultExpression);
            }
            else if (parameter.kind == talas::core::ParameterKind::Text
                     && !parameter.defaultText.empty())
            {
                sources.push_back(parameter.defaultText);
            }
        }
    }
}
}

bool isCanonicalDiskCacheCaptureFingerprint(
    std::string_view fingerprint) noexcept
{
    if (fingerprint.size() != diskCacheCaptureFingerprintBytes)
        return false;
    for (const auto value : fingerprint)
    {
        if (!((value >= '0' && value <= '9')
              || (value >= 'a' && value <= 'f')))
        {
            return false;
        }
    }
    return true;
}

std::string makeDiskCacheCaptureProvenance(
    std::string_view fingerprint)
{
    if (!isCanonicalDiskCacheCaptureFingerprint(fingerprint))
        return {};
    std::string result;
    result.reserve(
        diskCacheCaptureProvenancePrefix.size() + fingerprint.size());
    result.append(diskCacheCaptureProvenancePrefix);
    result.append(fingerprint);
    return result;
}

std::optional<std::string_view> diskCacheCaptureFingerprint(
    std::string_view provenance) noexcept
{
    if (!provenance.starts_with(diskCacheCaptureProvenancePrefix))
        return std::nullopt;
    const auto fingerprint =
        provenance.substr(diskCacheCaptureProvenancePrefix.size());
    if (!isCanonicalDiskCacheCaptureFingerprint(fingerprint))
        return std::nullopt;
    return fingerprint;
}

DiskCacheAssetAuthority diskCacheAssetAuthority(
    const ProjectAsset& asset) noexcept
{
    if (!asset.provenance.starts_with(diskCacheCaptureProvenanceNamespace))
        return DiskCacheAssetAuthority::LegacyPassthrough;
    if (!diskCacheCaptureFingerprint(asset.provenance).has_value()
        || asset.kind != ProjectAssetKind::Audio
        || asset.disposition != ProjectAssetDisposition::Referenced
        || !isValidDiskCacheAssetPath(asset.path)
        || !isCanonicalProjectAssetContentHash(asset.contentHash)
        || asset.channelCount != 1
        || !std::isfinite(asset.sampleRate)
        || asset.sampleRate
            < static_cast<double>(minimumDiskCacheCaptureSampleRate)
        || asset.sampleRate
            > static_cast<double>(maximumDiskCacheCaptureSampleRate)
        || std::floor(asset.sampleRate) != asset.sampleRate
        || asset.frameCount == 0
        || asset.frameCount > maximumDiskCacheCaptureFrames
        || asset.encoding != "pcm16-wav"
        || asset.generation == 0
        || asset.generation > maximumProjectAssetJsonInteger)
    {
        return DiskCacheAssetAuthority::InvalidCapturedAsset;
    }
    return DiskCacheAssetAuthority::CapturedPlayback;
}

DiskCacheCaptureGenerationResult nextDiskCacheCaptureAssetGeneration(
    const ProjectDocument& document,
    std::string_view assetId) noexcept
{
    DiskCacheCaptureGenerationResult result;
    if (assetId.empty() || assetId.size() > 512)
    {
        result.error = DiskCacheCaptureGenerationError::InvalidAssetId;
        return result;
    }
    const auto existing = std::ranges::find(
        document.assets, assetId, &ProjectAsset::id);
    if (existing == document.assets.end())
    {
        result.generation = 1;
        return result;
    }
    if (diskCacheAssetAuthority(*existing)
        != DiskCacheAssetAuthority::CapturedPlayback)
    {
        result.error = DiskCacheCaptureGenerationError::UnrelatedAsset;
        return result;
    }
    if (existing->generation >= maximumProjectAssetJsonInteger)
    {
        result.error = DiskCacheCaptureGenerationError::Exhausted;
        return result;
    }
    result.generation = existing->generation + 1U;
    return result;
}

bool isValidDiskCacheAssetPath(std::string_view path) noexcept
{
    if (!path.starts_with("DiskCache/") || path.size() > 512
        || path.ends_with('/') || path.find('\\') != std::string_view::npos
        || path.find("//") != std::string_view::npos)
    {
        return false;
    }
    std::size_t start = 0;
    while (start < path.size())
    {
        const auto end = path.find('/', start);
        const auto segment = path.substr(
            start,
            end == std::string_view::npos
                ? path.size() - start
                : end - start);
        if (segment.empty() || segment == "." || segment == "..")
            return false;
        if (end == std::string_view::npos)
            break;
        start = end + 1U;
    }
    return true;
}

DiskCacheCaptureRangeResult diskCacheCaptureRange(
    const TimelineLoopRegion& loop,
    double sampleRate) noexcept
{
    DiskCacheCaptureRangeResult result;
    if (!loop.enabled)
    {
        result.error = DiskCacheCaptureRangeError::Disabled;
        return result;
    }
    if (!std::isfinite(sampleRate)
        || sampleRate
            < static_cast<double>(minimumDiskCacheCaptureSampleRate)
        || sampleRate
            > static_cast<double>(maximumDiskCacheCaptureSampleRate)
        || std::floor(sampleRate) != sampleRate
        || sampleRate
            > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
    {
        result.error = DiskCacheCaptureRangeError::InvalidSampleRate;
        return result;
    }
    if (!std::isfinite(loop.startSeconds)
        || !std::isfinite(loop.endSeconds)
        || loop.startSeconds < 0.0
        || loop.endSeconds <= loop.startSeconds)
    {
        result.error = DiskCacheCaptureRangeError::InvalidLoop;
        return result;
    }

    ProjectMusicalTime musicalTime;
    const auto rate = static_cast<std::uint32_t>(sampleRate);
    const auto start = scoreTimeToSampleFrame(
        {ScoreTimeDomain::Seconds, loop.startSeconds, 0},
        musicalTime,
        rate);
    const auto end = scoreTimeToSampleFrame(
        {ScoreTimeDomain::Seconds, loop.endSeconds, 0},
        musicalTime,
        rate);
    if (!start.has_value() || !end.has_value())
    {
        result.error = DiskCacheCaptureRangeError::Overflow;
        return result;
    }
    if (*end <= *start)
    {
        result.error = DiskCacheCaptureRangeError::InvalidLoop;
        return result;
    }
    if (*end > maximumDiskCacheCaptureFrames)
    {
        result.error = DiskCacheCaptureRangeError::ExceedsFrameLimit;
        return result;
    }

    result.range = DiskCacheCaptureRange{*start, *end};
    return result;
}

std::string projectDiskCacheSourceFingerprint(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view sourcePath,
    std::uint32_t sampleRate,
    std::string& error)
{
    error.clear();
    if (sourcePath.empty() || sourcePath.size() > 4096
        || sampleRate < minimumDiskCacheCaptureSampleRate
        || sampleRate > maximumDiskCacheCaptureSampleRate)
    {
        error = "DiskCache source fingerprint input is invalid";
        return {};
    }
    const auto range = diskCacheCaptureRange(
        document.timeline.loop,
        static_cast<double>(sampleRate));
    if (!range.ok())
    {
        error = "DiskCache source fingerprint requires a valid bounded Timeline loop";
        return {};
    }

    try
    {
        auto expanded = expandProjectSounds(document, registry);
        if (!expanded.ok())
        {
            error = "DiskCache source fingerprint could not expand reusable Sounds";
            return {};
        }
        const auto midiVoiceTopology = discoverProjectMidiVoiceIslands(
            expanded.document,
            expanded.manifest,
            {});
        if (!midiVoiceTopology.ok()
            || !materializeProjectSoundVariableBranches(
                    document,
                    expanded,
                    midiVoiceTopology.islands)
                    .ok())
        {
            error = "DiskCache source fingerprint could not materialize Sound-variable branches";
            return {};
        }

        const auto identity = std::ranges::find(
            expanded.manifest.nodes,
            sourcePath,
            &ExpandedSoundNodeIdentity::sourcePath);
        if (identity == expanded.manifest.nodes.end())
        {
            error = "DiskCache source fingerprint lost target '"
                + std::string{sourcePath} + "'";
            return {};
        }
        const auto targetNode = std::ranges::find(
            expanded.document.graph.nodes,
            identity->flatNodeId,
            &ProjectNode::instanceId);
        if (targetNode == expanded.document.graph.nodes.end()
            || targetNode->typeId != "talas.diskCache")
        {
            error = "DiskCache source fingerprint target is not a DiskCache";
            return {};
        }

        const talas::core::Connection* sourceConnection = nullptr;
        for (const auto& connection : expanded.document.graph.connections)
        {
            if (connection.toNode != identity->flatNodeId
                || connection.toPort != "audioIn")
            {
                continue;
            }
            if (sourceConnection != nullptr)
            {
                error = "DiskCache source fingerprint found ambiguous audio input";
                return {};
            }
            sourceConnection = &connection;
        }
        if (sourceConnection == nullptr)
        {
            error = "DiskCache source fingerprint lost its audio input";
            return {};
        }
        const auto sourceNodeId = sourceConnection->fromNode;
        const auto sourcePortId = sourceConnection->fromPort;

        std::unordered_multimap<std::string_view, std::string_view> incoming;
        incoming.reserve(expanded.document.graph.connections.size());
        for (const auto& connection : expanded.document.graph.connections)
            incoming.emplace(connection.toNode, connection.fromNode);
        std::unordered_set<std::string> retained;
        retained.reserve(expanded.document.graph.nodes.size());
        std::vector<std::string> pending{sourceNodeId};
        retained.insert(pending.front());
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor)
        {
            const auto [first, last] = incoming.equal_range(pending[cursor]);
            for (auto connection = first; connection != last; ++connection)
            {
                if (retained.insert(std::string{connection->second}).second)
                    pending.emplace_back(connection->second);
            }
        }

        std::unordered_map<std::string, ProjectVariableOwner>
            soundVariableVoiceOwners;
        for (const auto& island : midiVoiceTopology.islands)
        {
            ProjectVariableOwner owner;
            owner.kind = ProjectVariableOwnerKind::MidiVoice;
            owner.definitionId = island.definitionId;
            owner.nodeId = island.localOwnerNodeId;
            for (const auto& nodeId : island.nodeIds)
                soundVariableVoiceOwners.emplace(nodeId, owner);
        }
        struct RetainedSoundVariableSelectorAuthority
        {
            std::string flatNodeId;
            std::string variableId;
            ProjectSoundVariableKind kind =
                ProjectSoundVariableKind::Sound;
            std::vector<std::string> candidateDefinitionIds;
            std::uint16_t initialSelectedOrdinal = 0;
        };
        std::vector<RetainedSoundVariableSelectorAuthority>
            retainedSoundVariableSelectors;
        for (const auto& selector :
             expanded.manifest.soundVariableSelectors)
        {
            if (!retained.contains(selector.flatNodeId))
                continue;
            std::vector<std::string_view> definitionScope;
            auto currentScope = selector.scopeOrdinal;
            for (std::size_t depth = 0;
                 currentScope != 0
                     && depth <= expanded.manifest.scopes.size();
                 ++depth)
            {
                const auto scope = std::ranges::find(
                    expanded.manifest.scopes,
                    currentScope,
                    &ExpandedSoundScopeIdentity::scopeOrdinal);
                if (scope == expanded.manifest.scopes.end())
                    break;
                definitionScope.push_back(scope->definitionId);
                currentScope = scope->parentScopeOrdinal;
            }
            if (currentScope != 0)
            {
                error = "DiskCache source fingerprint lost exact Sound-variable scope authority";
                return {};
            }
            std::ranges::reverse(definitionScope);
            const auto inheritedVoice = soundVariableVoiceOwners.find(
                selector.flatNodeId);
            const auto* declaration =
                findVisibleProjectSoundVariableDeclaration(
                    document,
                    selector.variableId,
                    definitionScope,
                    inheritedVoice == soundVariableVoiceOwners.end()
                        ? nullptr
                        : &inheritedVoice->second);
            if (declaration == nullptr
                || declaration->kind != selector.kind
                || declaration->candidateDefinitionIds
                    != selector.candidateDefinitionIds)
            {
                error = "DiskCache source fingerprint lost exact Sound-variable selector authority";
                return {};
            }
            retainedSoundVariableSelectors.push_back({
                selector.flatNodeId,
                selector.variableId,
                selector.kind,
                selector.candidateDefinitionIds,
                declaration->initialSelectedOrdinal,
            });
        }
        std::ranges::sort(
            retainedSoundVariableSelectors,
            {},
            &RetainedSoundVariableSelectorAuthority::flatNodeId);

        auto authority = std::move(expanded.document);
        std::erase_if(authority.graph.nodes, [&](const auto& node) {
            return !retained.contains(node.instanceId);
        });
        std::erase_if(authority.graph.connections, [&](const auto& connection) {
            return !retained.contains(connection.fromNode)
                || !retained.contains(connection.toNode);
        });
        std::ranges::sort(
            authority.graph.nodes,
            {},
            &ProjectNode::instanceId);
        std::ranges::sort(
            authority.graph.connections,
            {},
            [](const auto& connection) {
                return std::tie(
                    connection.fromNode,
                    connection.fromPort,
                    connection.toNode,
                    connection.toPort);
            });

        authority.metadata = {};
        authority.presentation = {2, {{std::nullopt, {}}}, {}, {}};
        authority.snapshotLibrary = {};
        authority.liveSetLibrary = {};
        authority.partialTrackEdits.clear();
        authority.deviceSettings = {};
        authority.generatorLibrary = {};
        authority.timeline.loop = {};
        authority.timeline.score = {};
        authority.soundDefinitions.clear();
        authority.soundVariableLibrary = {};

        std::unordered_set<std::string> referencedAssetIds;
        std::unordered_set<std::string> referencedSampleMapIds;
        for (const auto& node : authority.graph.nodes)
        {
            for (const auto& parameter : node.parameters)
            {
                if (parameter.resource.has_value()
                    && !parameter.resource->empty())
                {
                    referencedAssetIds.insert(*parameter.resource);
                }
                if (node.typeId == "talas.sampleMapPlayer"
                    && parameter.parameterId == "sampleMapId"
                    && parameter.text.has_value()
                    && !parameter.text->empty())
                {
                    referencedSampleMapIds.insert(*parameter.text);
                }
            }
        }
        std::erase_if(
            authority.sampleMapLibrary.maps,
            [&](const auto& map) {
                return !referencedSampleMapIds.contains(map.mapId);
            });
        if (authority.sampleMapLibrary.maps.size()
            != referencedSampleMapIds.size())
        {
            error = "DiskCache source fingerprint lost referenced Sample Map authority";
            return {};
        }
        for (auto& map : authority.sampleMapLibrary.maps)
        {
            map.displayName = "sample-map";
            for (const auto& region : map.regions)
                referencedAssetIds.insert(region.assetId);
        }
        std::ranges::sort(
            authority.sampleMapLibrary.maps,
            {},
            &ProjectSampleMap::mapId);
        std::erase_if(authority.assets, [&](const auto& asset) {
            return !referencedAssetIds.contains(asset.id);
        });
        struct RetainedDiskCacheModeAuthority
        {
            std::string nodeId;
            std::string assetId;
            DiskCacheAssetAuthority mode =
                DiskCacheAssetAuthority::LegacyPassthrough;
        };
        std::vector<RetainedDiskCacheModeAuthority>
            retainedDiskCacheModes;
        for (const auto& node : authority.graph.nodes)
        {
            if (node.typeId != "talas.diskCache")
                continue;
            const auto record = std::ranges::find(
                node.parameters,
                std::string_view{"record"},
                &ProjectParameterExpression::parameterId);
            const auto recordEnabled = record != node.parameters.end()
                && record->boolean.value_or(false);
            if (recordEnabled)
            {
                error = "DiskCache source fingerprint cannot capture through another armed DiskCache";
                return {};
            }
            const auto fileName = std::ranges::find(
                node.parameters,
                std::string_view{"fileName"},
                &ProjectParameterExpression::parameterId);
            if (fileName == node.parameters.end()
                || !fileName->resource.has_value()
                || fileName->resource->empty())
            {
                error = "DiskCache source fingerprint lost upstream DiskCache asset authority";
                return {};
            }
            const auto asset = std::ranges::find(
                authority.assets,
                *fileName->resource,
                &ProjectAsset::id);
            if (asset == authority.assets.end())
            {
                error = "DiskCache source fingerprint lost upstream DiskCache asset metadata";
                return {};
            }
            const auto mode = diskCacheAssetAuthority(*asset);
            if (mode == DiskCacheAssetAuthority::InvalidCapturedAsset)
            {
                error = "DiskCache source fingerprint found malformed upstream captured authority";
                return {};
            }
            retainedDiskCacheModes.push_back({
                node.instanceId,
                asset->id,
                mode,
            });
        }
        for (auto& asset : authority.assets)
        {
            asset.path.clear();
            asset.disposition = ProjectAssetDisposition::Referenced;
            asset.provenance.clear();
        }
        std::ranges::sort(authority.assets, {}, &ProjectAsset::id);

        std::erase_if(authority.virtualControls, [&](const auto& control) {
            return !projectReferencesEventValue(
                authority,
                registry,
                control.eventName);
        });
        std::erase_if(authority.timeline.eventValues, [&](const auto& event) {
            return !projectReferencesEventValue(
                authority,
                registry,
                event.eventName);
        });
        for (auto& control : authority.virtualControls)
        {
            control.kind = VirtualControlKind::Fader;
            control.minimum = 0.0;
            control.maximum = 1.0;
            control.midiMapping.reset();
            control.oscMapping.reset();
            control.oscEgressMapping.reset();
        }

        std::vector<std::string> expressionSources;
        appendGraphSources(
            authority.graph,
            registry,
            expressionSources);
        std::unordered_set<std::string> usedDefinitions;
        bool addedDefinition = true;
        while (addedDefinition)
        {
            addedDefinition = false;
            for (const auto& definition :
                 authority.expressionDefinitions.definitions)
            {
                if (usedDefinitions.contains(definition.name))
                    continue;
                const auto referenced = std::ranges::any_of(
                    expressionSources,
                    [&](const auto& source) {
                        return callsDefinition(source, definition.name);
                    }) || std::ranges::any_of(
                    usedDefinitions,
                    [&](const auto& usedName) {
                        const auto used = std::ranges::find(
                            authority.expressionDefinitions.definitions,
                            usedName,
                            &talas::expr::ExpressionDefinition::name);
                        return used
                                != authority.expressionDefinitions.definitions.end()
                            && callsDefinition(
                                used->source,
                                definition.name);
                    });
                if (referenced)
                {
                    usedDefinitions.insert(definition.name);
                    addedDefinition = true;
                }
            }
        }
        std::erase_if(
            authority.expressionDefinitions.definitions,
            [&](const auto& definition) {
                return !usedDefinitions.contains(definition.name);
            });
        for (const auto& definition :
             authority.expressionDefinitions.definitions)
        {
            expressionSources.push_back(definition.source);
        }

        std::unordered_set<std::string> usedVariableIds;
        for (const auto& variable : authority.variableLibrary.variables)
        {
            if (std::ranges::any_of(
                    expressionSources,
                    [&](const auto& source) {
                        return referencesVariable(
                            source,
                            variable.variableId);
                    }))
            {
                usedVariableIds.insert(variable.variableId);
            }
        }
        std::erase_if(
            authority.variableLibrary.variables,
            [&](const auto& variable) {
                return !usedVariableIds.contains(variable.variableId);
            });
        for (auto& variable : authority.variableLibrary.variables)
        {
            variable.displayName = "variable";
            variable.snapshotEligible = false;
        }

        auto bytes = serializeProjectDocument(authority, registry);
        const auto appendSized = [&](std::string_view value) {
            bytes.append(std::to_string(value.size()));
            bytes.push_back(':');
            bytes.append(value);
        };
        for (const auto& selector : retainedSoundVariableSelectors)
        {
            bytes.append("\nsound-variable-selector:");
            appendSized(selector.flatNodeId);
            appendSized(selector.variableId);
            bytes.append(std::to_string(
                static_cast<int>(selector.kind)));
            bytes.push_back(':');
            bytes.append(std::to_string(
                selector.initialSelectedOrdinal));
            bytes.push_back(':');
            bytes.append(std::to_string(
                selector.candidateDefinitionIds.size()));
            bytes.push_back(':');
            for (const auto& candidate :
                 selector.candidateDefinitionIds)
            {
                appendSized(candidate);
            }
        }
        for (const auto& diskCache : retainedDiskCacheModes)
        {
            bytes.append("\nupstream-disk-cache:");
            appendSized(diskCache.nodeId);
            appendSized(diskCache.assetId);
            bytes.append(std::to_string(
                static_cast<int>(diskCache.mode)));
            bytes.push_back(':');
        }
        bytes.append("\ndisk-cache-target:");
        bytes.append(sourcePath);
        bytes.append("\nsource-node:");
        bytes.append(sourceNodeId);
        bytes.push_back('/');
        bytes.append(sourcePortId);
        bytes.append("\ncapture-start:");
        bytes.append(std::to_string(range.range->startFrame));
        bytes.append("\ncapture-end:");
        bytes.append(std::to_string(range.range->endFrame));
        bytes.append("\nsample-rate:");
        bytes.append(std::to_string(sampleRate));
        bytes.append("\ncapture-version:1");
        const auto hashed = calculateProjectAssetSha256(
            std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(bytes.data()),
                bytes.size()});
        if (!isCanonicalDiskCacheCaptureFingerprint(hashed))
        {
            error = "DiskCache source fingerprint hashing failed";
            return {};
        }
        return hashed;
    }
    catch (const std::bad_alloc&)
    {
        error = "DiskCache source fingerprint allocation failed";
    }
    catch (const std::length_error&)
    {
        error = "DiskCache source fingerprint exceeded supported limits";
    }
    catch (const std::exception& exception)
    {
        error = "DiskCache source fingerprint failed: ";
        error += exception.what();
    }
    return {};
}
}
