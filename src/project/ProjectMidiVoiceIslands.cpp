#include "talas/project/ProjectMidiVoiceIslands.h"

#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/project/ProjectValidation.h"
#include "talas/expr/Expression.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace talas::project
{
bool hasProjectAnalogSequencerKeySource(const ProjectNode& node) noexcept
{
    if (node.typeId != "talas.analogSequencer")
        return false;
    // A Polyphony above 1 lets one event's release overlap the next's attack
    //, so the sequence plays note events on that
    // many voices of its Input, as with Pitches; a Polyphony read from Event
    // Values or variables keeps the sequence path, which plays one.
    const auto polyphony = std::ranges::find(node.parameters, std::string_view{"polyphony"},
        &ProjectParameterExpression::parameterId);
    if (polyphony != node.parameters.end() && polyphony->expression)
    {
        try
        {
            // A literal reads alike in every dialect; the compiled step
            // decides from its own parse in the project's.
            const auto parsed = talas::expr::parse(*polyphony->expression,
                talas::expr::latestExpressionLanguage);
            if (parsed.ok() && parsed.expression->instructionCount() == 1
                && parsed.expression->eventReferences().empty()
                && parsed.expression->variableReferences().empty()
                && parsed.expression->identifiers().empty()
                && !parsed.expression->usesTime())
            {
                const auto value = talas::expr::evaluate(*parsed.expression, {});
                if (value.ok() && value.value.domain == talas::expr::ValueDomain::Scalar
                    && std::isfinite(value.value.value) && value.value.value > 1.0)
                    return true;
            }
        }
        catch (...)
        {
            return false;
        }
    }
    return std::ranges::any_of(node.parameters, [](const auto& parameter) {
            return (parameter.parameterId == "keyDowns" || parameter.parameterId == "pitches")
                && parameter.text
                && std::ranges::any_of(*parameter.text, [](char value) {
                    return value != ' ' && value != '\t' && value != '\r'
                        && value != '\n' && value != '\f' && value != '\v';
                });
        });
}

namespace
{
[[nodiscard]] bool isPolyphonicInputOwner(const ProjectNode& node) noexcept
{
    if (node.typeId == "talas.midiVoice")
        return true;
    if (node.typeId == "talas.analogSequencer")
        return hasProjectAnalogSequencerKeySource(node);
    if (node.typeId != "talas.midiMapper")
        return false;
    const auto model = std::ranges::find(
        node.parameters,
        std::string_view{"voiceModel"},
        &ProjectParameterExpression::parameterId);
    return model == node.parameters.end()
        || (model->choice.has_value()
            && *model->choice == "polyphonicInput");
}

[[nodiscard]] bool islandLess(
    const ProjectMidiVoiceIsland& left,
    const ProjectMidiVoiceIsland& right) noexcept
{
    if (left.instancePath != right.instancePath)
        return left.instancePath < right.instancePath;
    if (left.definitionId != right.definitionId)
        return left.definitionId < right.definitionId;
    if (left.localOwnerNodeId != right.localOwnerNodeId)
        return left.localOwnerNodeId < right.localOwnerNodeId;
    return left.flatOwnerNodeId < right.flatOwnerNodeId;
}

struct ExpandedVoiceSourceMetadata
{
    std::unordered_map<std::uint32_t, const ExpandedSoundScopeIdentity*> scopes;
    std::unordered_map<std::string, const ExpandedSoundNodeIdentity*> identities;
};

[[nodiscard]] bool indexExpandedVoiceSourceMetadata(
    const ProjectSoundExpansionManifest& expansion,
    ExpandedVoiceSourceMetadata& metadata,
    std::string& error,
    std::string& errorNodeId)
{
    if (expansion.scopes.size() > maximumProjectSoundExpansionWorkItems
        || expansion.nodes.size() > maximumProjectSoundExpansionWorkItems)
    {
        error = "Nested MIDI Voice source metadata exceeds preparation capacity";
        return false;
    }

    std::set<std::vector<std::string>> sourcePaths;
    for (const auto& scope : expansion.scopes)
    {
        if (scope.scopeOrdinal == 0 || scope.parentScopeOrdinal >= scope.scopeOrdinal
            || scope.definitionId.empty() || scope.instancePath.empty()
            || !metadata.scopes.emplace(scope.scopeOrdinal, &scope).second
            || !sourcePaths.insert(scope.instancePath).second)
        {
            error = "Nested MIDI Voice preparation received an invalid Sound scope";
            return false;
        }
    }
    for (const auto& scope : expansion.scopes)
    {
        const auto parent = metadata.scopes.find(scope.parentScopeOrdinal);
        if (scope.parentScopeOrdinal != 0
            && (parent == metadata.scopes.end()
                || parent->second->instancePath.size() >= scope.instancePath.size()
                || !std::equal(parent->second->instancePath.begin(),
                    parent->second->instancePath.end(), scope.instancePath.begin())))
        {
            error = "Nested MIDI Voice preparation received an unknown Sound parent scope";
            return false;
        }
    }

    std::set<std::pair<std::uint32_t, std::string>> localNodeIds;
    for (const auto& identity : expansion.nodes)
    {
        const auto source = metadata.scopes.find(identity.scopeOrdinal);
        const auto validSource = identity.scopeOrdinal == 0
            ? identity.definitionId.empty() && identity.instancePath.empty()
            : source != metadata.scopes.end()
                && source->second->definitionId == identity.definitionId
                && source->second->instancePath == identity.instancePath;
        if (!validSource || identity.localNodeId.empty()
            || !metadata.identities.emplace(identity.flatNodeId, &identity).second
            || !localNodeIds.emplace(identity.scopeOrdinal, identity.localNodeId).second)
        {
            error = "Nested MIDI Voice preparation received an invalid node source scope";
            errorNodeId = identity.flatNodeId;
            return false;
        }
    }
    return true;
}
}

ProjectMidiVoiceLocalIslandResult discoverProjectMidiVoiceInputHierarchy(
    const ProjectGraph& graph)
{
    ProjectMidiVoiceLocalIslandResult result;
    std::unordered_map<std::string, const ProjectNode*> nodes;
    nodes.reserve(graph.nodes.size());
    for (const auto& node : graph.nodes)
    {
        if (!nodes.emplace(node.instanceId, &node).second)
        {
            result.error =
                "MIDI Voice island discovery received duplicate node identities";
            return result;
        }
    }
    std::unordered_map<
        std::string,
        std::vector<const talas::core::Connection*>> incoming;
    incoming.reserve(graph.nodes.size());
    for (const auto& connection : graph.connections)
        incoming[connection.toNode].push_back(&connection);

    std::unordered_map<std::string, std::string> claimedNodes;
    claimedNodes.reserve(graph.nodes.size());
    for (const auto& owner : graph.nodes)
    {
        if (!isPolyphonicInputOwner(owner))
            continue;
        const auto ownerIncoming = incoming.find(owner.instanceId);
        const talas::core::Connection* audioIn = nullptr;
        const talas::core::Connection* rightIn = nullptr;
        if (ownerIncoming != incoming.end())
        {
            for (const auto* connection : ownerIncoming->second)
            {
                if (connection->toPort == "audioIn")
                {
                    if (audioIn != nullptr)
                    {
                        result.error =
                            "MIDI Voice owner has ambiguous audioIn authority";
                        result.errorNodeId = owner.instanceId;
                        return result;
                    }
                    audioIn = connection;
                }
                else if (connection->toPort == "rightIn")
                {
                    if (rightIn != nullptr)
                    {
                        result.error =
                            "MIDI Voice owner has ambiguous rightIn authority";
                        result.errorNodeId = owner.instanceId;
                        return result;
                    }
                    rightIn = connection;
                }
            }
        }
        if (audioIn == nullptr || !nodes.contains(audioIn->fromNode))
        {
            result.error = "MIDI Voice requires an exact audioIn graph";
            result.errorNodeId = owner.instanceId;
            return result;
        }
        if (rightIn != nullptr && !nodes.contains(rightIn->fromNode))
        {
            result.error = "MIDI Voice rightIn references an unknown graph";
            result.errorNodeId = owner.instanceId;
            return result;
        }

        std::unordered_set<std::string> islandNodes;
        islandNodes.reserve(graph.nodes.size());
        std::vector<std::string> ancestors{audioIn->fromNode};
        if (rightIn != nullptr)
            ancestors.push_back(rightIn->fromNode);
        for (std::size_t index = 0; index < ancestors.size(); ++index)
        {
            const auto& nodeId = ancestors[index];
            if (!islandNodes.insert(nodeId).second)
                continue;
            const auto current = nodes.find(nodeId);
            if (current == nodes.end())
            {
                result.error =
                    "MIDI Voice island references an unknown upstream node";
                result.errorNodeId = nodeId;
                return result;
            }
            if (nodeId == owner.instanceId)
            {
                result.error =
                    "MIDI Voice input ownership cannot contain cycles";
                result.errorNodeId = nodeId;
                return result;
            }
            if (isTerminalNodeType(current->second->typeId))
            {
                result.error =
                    "MIDI Voice input graph cannot contain terminal output nodes";
                result.errorNodeId = nodeId;
                return result;
            }
            const auto nodeIncoming = incoming.find(nodeId);
            if (nodeIncoming == incoming.end())
                continue;
            for (const auto* connection : nodeIncoming->second)
            {
                // A nested owner is processed in the parent's voice, but its
                // input is a separate region instantiated for each child voice.
                // Its control producers remain in the parent region.
                if (isPolyphonicInputOwner(*current->second)
                    && (connection->toPort == "audioIn"
                        || connection->toPort == "rightIn"))
                    continue;
                ancestors.push_back(connection->fromNode);
            }
        }
        for (const auto& connection : graph.connections)
        {
            if (!islandNodes.contains(connection.fromNode)
                || islandNodes.contains(connection.toNode))
            {
                continue;
            }
            const auto isBoundary =
                connection.toNode == owner.instanceId
                && ((connection.toPort == "audioIn"
                     && connection.fromNode == audioIn->fromNode
                     && connection.fromPort == audioIn->fromPort)
                    || (rightIn != nullptr
                        && connection.toPort == "rightIn"
                        && connection.fromNode == rightIn->fromNode
                        && connection.fromPort == rightIn->fromPort));
            if (!isBoundary)
            {
                result.error =
                    "MIDI Voice input graph cannot share upstream outputs outside its voice island";
                result.errorNodeId = connection.fromNode;
                result.errorPortId = connection.fromPort;
                return result;
            }
        }
        ProjectMidiVoiceLocalIsland island;
        island.ownerNodeId = owner.instanceId;
        island.rootNodeId = audioIn->fromNode;
        island.rootPortId = audioIn->fromPort;
        if (rightIn != nullptr)
        {
            island.rightRootNodeId = rightIn->fromNode;
            island.rightRootPortId = rightIn->fromPort;
        }
        island.nodeIds.assign(islandNodes.begin(), islandNodes.end());
        std::ranges::sort(island.nodeIds);
        for (const auto& nodeId : island.nodeIds)
        {
            if (!claimedNodes.emplace(nodeId, owner.instanceId).second)
            {
                result.error =
                    "Prepared MIDI Voice regions cannot share upstream voice state";
                result.errorNodeId = nodeId;
                result.islands.clear();
                return result;
            }
        }
        result.islands.push_back(std::move(island));
    }
    for (auto& island : result.islands)
    {
        if (const auto parent = claimedNodes.find(island.ownerNodeId);
            parent != claimedNodes.end())
            island.parentOwnerNodeId = parent->second;
    }
    // Audio boundaries stop each local traversal, so a cycle made entirely of
    // owner boundaries needs a separate parent-chain check.
    for (const auto& island : result.islands)
    {
        auto parent = island.parentOwnerNodeId;
        std::size_t depth = 0;
        while (!parent.empty())
        {
            if (++depth >= result.islands.size() || parent == island.ownerNodeId)
            {
                result.error = "MIDI Voice input ownership cannot contain cycles";
                result.errorNodeId = island.ownerNodeId;
                result.islands.clear();
                return result;
            }
            const auto ancestor = claimedNodes.find(parent);
            parent = ancestor == claimedNodes.end() ? std::string{} : ancestor->second;
        }
    }
    std::ranges::sort(
        result.islands,
        {},
        &ProjectMidiVoiceLocalIsland::ownerNodeId);
    return result;
}

ProjectMidiVoiceLocalIslandResult discoverProjectMidiVoiceLocalIslands(
    const ProjectGraph& graph)
{
    auto result = discoverProjectMidiVoiceInputHierarchy(graph);
    if (!result.ok()) return result;
    for (const auto& island : result.islands)
    {
        // Preserve the currently admitted flat profile, including the legacy
        // MIDI Mapper restriction. Tree discovery alone must not enable a plan
        // that the compiler and realtime scheduler still interpret as peers.
        for (const auto& nodeId : island.nodeIds)
        {
            const auto node = std::ranges::find(graph.nodes, nodeId, &ProjectNode::instanceId);
            if (node != graph.nodes.end()
                && (node->typeId == "talas.midiMapper"
                    || isPolyphonicInputOwner(*node)))
            {
                result.error = "MIDI Voice input graph cannot contain nested MIDI-In nodes";
                result.errorNodeId = nodeId;
                result.islands.clear();
                return result;
            }
        }
    }
    return result;
}

namespace
{
ProjectMidiVoiceIslandResult projectVoiceInstances(
    std::vector<ProjectMidiVoiceIsland> templates,
    const ProjectMidiVoiceLocalIslandResult& local,
    const ProjectSoundExpansionManifest& expansion,
    std::uint32_t nextScope)
{
    ProjectMidiVoiceIslandResult result;
    const auto fail = [&](std::string message, std::string node = {}) {
        result.error = std::move(message);
        result.errorNodeId = std::move(node);
        result.islands.clear();
        return false;
    };
    ExpandedVoiceSourceMetadata sourceMetadata;
    std::string metadataError;
    std::string metadataErrorNode;
    if (!indexExpandedVoiceSourceMetadata(
            expansion, sourceMetadata, metadataError, metadataErrorNode))
    {
        fail(std::move(metadataError), std::move(metadataErrorNode));
        return result;
    }
    const auto& scopes = sourceMetadata.scopes;
    const auto& identities = sourceMetadata.identities;
    std::unordered_map<std::string, std::vector<std::size_t>> children;
    std::vector<std::size_t> roots;
    std::unordered_set<std::string> templateOwners;
    for (const auto& island : templates) templateOwners.insert(island.flatOwnerNodeId);
    for (std::size_t index = 0; index < templates.size(); ++index)
    {
        const auto& island = templates[index];
        const auto source = std::ranges::find(local.islands,
            island.flatOwnerNodeId, &ProjectMidiVoiceLocalIsland::ownerNodeId);
        if (source->parentOwnerNodeId.empty()) roots.push_back(index);
        else if (templateOwners.contains(source->parentOwnerNodeId))
            children[source->parentOwnerNodeId].push_back(index);
        else
        {
            fail("Nested MIDI Voice owner has no reachable parent", island.flatOwnerNodeId);
            return result;
        }
    }
    std::size_t allocatedScopes = 0;
    const auto allocate = [&](std::size_t count) {
        if (count > maximumPreparedMidiVoiceHierarchyScopes - allocatedScopes
            || count > std::numeric_limits<std::uint32_t>::max() - nextScope)
            return false;
        allocatedScopes += count;
        nextScope += static_cast<std::uint32_t>(count);
        return true;
    };
    const auto append = [&](std::size_t templateIndex,
                            const ProjectMidiVoiceIsland* parent,
                            std::size_t parentVoice) {
        auto island = templates[templateIndex];
        if (parent != nullptr)
        {
            island.voiceAncestors = parent->voiceAncestors;
            island.voiceAncestors.push_back({parent->localOwnerNodeId, parent->definitionId,
                parent->instancePath, static_cast<std::uint16_t>(parentVoice)});
            if (island.voiceAncestors.size() >= maximumPreparedMidiVoiceNestingDepth)
                return fail("Nested MIDI Voice hierarchy exceeds the preparation depth limit",
                    island.flatOwnerNodeId);
            island.parentScopeOrdinal = parent->firstVoiceScopeOrdinal
                + static_cast<std::uint32_t>(parentVoice);
            if (island.sourceScopeOrdinal != parent->sourceScopeOrdinal)
            {
                const auto containing = std::ranges::find_if(parent->scopeProjections,
                    [&](const auto& scope) {
                        return scope.sourceScopeOrdinal == island.sourceScopeOrdinal
                            && scope.voiceAncestors == island.voiceAncestors;
                    });
                if (containing == parent->scopeProjections.end())
                    return fail("Nested MIDI Voice owner is missing its containing Sound projection",
                        island.flatOwnerNodeId);
                island.parentScopeOrdinal = containing->scopeOrdinal;
            }
        }
        if (island.voiceCount == 0 || !allocate(island.voiceCount))
            return fail("Nested MIDI Voice scopes exceed the preparation capacity",
                island.flatOwnerNodeId);
        island.firstVoiceScopeOrdinal = nextScope
            - static_cast<std::uint32_t>(island.voiceCount) + 1U;

        // Collect complete source paths once, including intermediate Sounds
        // with declarations but no direct processing node in this input region.
        std::unordered_set<std::uint32_t> requiredScopes;
        for (const auto& nodeId : island.nodeIds)
        {
            const auto identity = identities.find(nodeId);
            if (identity == identities.end())
                return fail("Nested MIDI Voice input node is missing expansion identity", nodeId);
            auto source = identity->second->scopeOrdinal;
            while (source != island.sourceScopeOrdinal)
            {
                if (source == 0)
                    return fail("Nested MIDI Voice input source is outside the owner's lexical subtree",
                        nodeId);
                requiredScopes.insert(source);
                source = scopes.at(source)->parentScopeOrdinal;
            }
        }
        std::vector<std::uint32_t> orderedScopes(requiredScopes.begin(), requiredScopes.end());
        std::ranges::sort(orderedScopes);
        for (std::size_t voice = 0; voice < island.voiceCount; ++voice)
        {
            std::unordered_map<std::uint32_t, std::uint32_t> projected;
            projected.emplace(island.sourceScopeOrdinal,
                island.firstVoiceScopeOrdinal + static_cast<std::uint32_t>(voice));
            auto ancestors = island.voiceAncestors;
            ancestors.push_back({island.localOwnerNodeId, island.definitionId,
                island.instancePath, static_cast<std::uint16_t>(voice)});
            for (const auto sourceOrdinal : orderedScopes)
            {
                if (!allocate(1))
                    return fail("Nested MIDI Voice scopes exceed the preparation capacity",
                        island.flatOwnerNodeId);
                const auto parentOrdinal = projected.at(scopes.at(sourceOrdinal)->parentScopeOrdinal);
                island.scopeProjections.push_back(
                    {sourceOrdinal, nextScope, parentOrdinal, ancestors});
                projected.emplace(sourceOrdinal, nextScope);
            }
        }
        result.islands.push_back(std::move(island));
        return true;
    };
    for (const auto root : roots)
        if (!append(root, nullptr, 0)) return result;
    // Breadth-first allocation is deterministic and makes every parent voice
    // and its intervening Sound scopes available before child materialization.
    for (std::size_t index = 0; index < result.islands.size(); ++index)
    {
        const auto found = children.find(result.islands[index].flatOwnerNodeId);
        if (found == children.end()) continue;
        const auto voices = result.islands[index].voiceCount;
        for (std::size_t voice = 0; voice < voices; ++voice)
            for (const auto child : found->second)
                if (!append(child, &result.islands[index], voice)) return result;
    }
    return result;
}

ProjectMidiVoiceIslandResult discoverProjectMidiVoiceIslandsImpl(
    const ProjectDocument& expandedDocument,
    const ProjectSoundExpansionManifest& expansion,
    const std::vector<ProjectMidiVoiceOwnerConfiguration>& configurations,
    bool hierarchical,
    bool templatesOnly = false)
{
    ProjectMidiVoiceIslandResult result;
    ExpandedVoiceSourceMetadata templateSourceMetadata;
    if (templatesOnly)
    {
        if (expandedDocument.graph.nodes.size() > maximumProjectSoundExpansionWorkItems
            || expandedDocument.graph.connections.size()
                > maximumProjectSoundExpansionWorkItems)
        {
            result.error = "MIDI Voice hierarchy templates exceed preparation capacity";
            return result;
        }
        if (!indexExpandedVoiceSourceMetadata(
                expansion, templateSourceMetadata, result.error, result.errorNodeId))
            return result;
    }
    const auto localIslands =
        hierarchical ? discoverProjectMidiVoiceInputHierarchy(expandedDocument.graph)
                     : discoverProjectMidiVoiceLocalIslands(expandedDocument.graph);
    if (!localIslands.ok())
    {
        result.error = localIslands.error;
        result.errorNodeId = localIslands.errorNodeId;
        result.errorPortId = localIslands.errorPortId;
        return result;
    }
    std::unordered_map<std::string, const ProjectNode*> nodes;
    nodes.reserve(expandedDocument.graph.nodes.size());
    for (const auto& node : expandedDocument.graph.nodes)
    {
        if (!nodes.emplace(node.instanceId, &node).second)
        {
            result.error =
                "MIDI Voice island discovery received duplicate node identities";
            return result;
        }
    }

    std::unordered_map<
        std::string,
        std::vector<const talas::core::Connection*>> incoming;
    incoming.reserve(expandedDocument.graph.nodes.size());
    for (const auto& connection : expandedDocument.graph.connections)
        incoming[connection.toNode].push_back(&connection);

    std::unordered_set<std::string> reachable;
    reachable.reserve(expandedDocument.graph.nodes.size());
    std::vector<std::string> pending;
    pending.reserve(expandedDocument.graph.nodes.size());
    for (const auto& node : expandedDocument.graph.nodes)
    {
        if (isTerminalNodeType(node.typeId)
            && reachable.insert(node.instanceId).second)
        {
            pending.push_back(node.instanceId);
        }
    }
    for (std::size_t index = 0; index < pending.size(); ++index)
    {
        const auto found = incoming.find(pending[index]);
        if (found == incoming.end())
            continue;
        for (const auto* connection : found->second)
        {
            if (nodes.contains(connection->fromNode)
                && reachable.insert(connection->fromNode).second)
            {
                pending.push_back(connection->fromNode);
            }
        }
    }

    std::unordered_map<std::string, std::size_t> voiceCounts;
    voiceCounts.reserve(configurations.size());
    for (const auto& configuration : configurations)
    {
        if (configuration.flatOwnerNodeId.empty()
            || configuration.voiceCount == 0
            || configuration.voiceCount
                > maximumPreparedMidiVoiceSlotsPerOwner
            || !voiceCounts.emplace(
                    configuration.flatOwnerNodeId,
                    configuration.voiceCount)
                    .second)
        {
            result.error =
                "MIDI Voice island configuration is invalid or duplicated";
            return result;
        }
    }
    std::unordered_set<std::string> matchedConfiguredOwners;
    matchedConfiguredOwners.reserve(configurations.size());

    std::uint32_t maximumStructuralScope = 0;
    for (const auto& scope : expansion.scopes)
        maximumStructuralScope = std::max(
            maximumStructuralScope,
            scope.scopeOrdinal);
    for (const auto& identity : expansion.nodes)
        maximumStructuralScope = std::max(
            maximumStructuralScope,
            identity.scopeOrdinal);

    for (const auto& node : expandedDocument.graph.nodes)
    {
        if (!isPolyphonicInputOwner(node)
            || !reachable.contains(node.instanceId))
        {
            continue;
        }
        const auto configured = voiceCounts.find(node.instanceId);
        if (!templatesOnly && (hierarchical || !configurations.empty())
            && configured == voiceCounts.end())
        {
            result.error =
                "Reachable MIDI Voice owner is missing prepared configuration";
            return result;
        }
        if (configured != voiceCounts.end())
            matchedConfiguredOwners.insert(node.instanceId);
        const auto identity = std::ranges::find(
            expansion.nodes,
            node.instanceId,
            &ExpandedSoundNodeIdentity::flatNodeId);
        if (identity == expansion.nodes.end()
            || identity->localNodeId.empty())
        {
            result.error =
                "MIDI Voice owner is missing exact expansion identity";
            return result;
        }
        const auto localIsland = std::ranges::find(
            localIslands.islands,
            node.instanceId,
            &ProjectMidiVoiceLocalIsland::ownerNodeId);
        if (localIsland == localIslands.islands.end())
        {
            result.error =
                "MIDI Voice owner is missing shared structural island authority";
            result.errorNodeId = node.instanceId;
            return result;
        }

        ProjectMidiVoiceIsland island;
        island.flatOwnerNodeId = node.instanceId;
        island.localOwnerNodeId = identity->localNodeId;
        island.definitionId = identity->definitionId;
        island.instancePath = identity->instancePath;
        island.parentScopeOrdinal = identity->scopeOrdinal;
        island.sourceScopeOrdinal = identity->scopeOrdinal;
        island.parentOwnerNodeId = localIsland->parentOwnerNodeId;
        island.voiceCount = templatesOnly || configured == voiceCounts.end()
            ? 0U
            : configured->second;
        island.rootNodeId = localIsland->rootNodeId;
        island.rootPortId = localIsland->rootPortId;
        island.rightRootNodeId = localIsland->rightRootNodeId;
        island.rightRootPortId = localIsland->rightRootPortId;
        island.nodeIds = localIsland->nodeIds;
        result.islands.push_back(std::move(island));
    }

    if (matchedConfiguredOwners.size() != voiceCounts.size())
    {
        result.error =
            "MIDI Voice island configuration references an unreachable or unknown owner";
        result.islands.clear();
        return result;
    }

    std::ranges::sort(result.islands, islandLess);
    if (templatesOnly)
    {
        for (const auto& island : result.islands)
        {
            for (const auto& nodeId : island.nodeIds)
            {
                if (!templateSourceMetadata.identities.contains(nodeId))
                {
                    result.error = "MIDI Voice input node is missing exact expansion identity";
                    result.errorNodeId = nodeId;
                    result.islands.clear();
                    return result;
                }
            }
        }
        return result;
    }
    if (hierarchical)
        return projectVoiceInstances(std::move(result.islands), localIslands, expansion,
            maximumStructuralScope);
    auto nextScope = maximumStructuralScope;
    for (auto& island : result.islands)
    {
        if (island.voiceCount == 0)
            continue;
        if (island.voiceCount
            > std::numeric_limits<std::uint32_t>::max() - nextScope)
        {
            result.error =
                "Prepared MIDI Voice scopes exceed the fixed identity range";
            result.islands.clear();
            return result;
        }
        island.firstVoiceScopeOrdinal = nextScope + 1U;
        nextScope += static_cast<std::uint32_t>(island.voiceCount);
    }
    return result;
}
}

ProjectMidiVoiceIslandResult discoverProjectMidiVoiceIslands(
    const ProjectDocument& expandedDocument,
    const ProjectSoundExpansionManifest& expansion,
    const std::vector<ProjectMidiVoiceOwnerConfiguration>& configurations)
{
    return discoverProjectMidiVoiceIslandsImpl(
        expandedDocument, expansion, configurations, false);
}

ProjectMidiVoiceIslandResult discoverProjectMidiVoiceHierarchyTemplates(
    const ProjectDocument& expandedDocument,
    const ProjectSoundExpansionManifest& expansion)
{
    return discoverProjectMidiVoiceIslandsImpl(
        expandedDocument, expansion, {}, true, true);
}

ProjectMidiVoiceIslandResult discoverProjectMidiVoiceInstanceHierarchy(
    const ProjectDocument& expandedDocument,
    const ProjectSoundExpansionManifest& expansion,
    const std::vector<ProjectMidiVoiceOwnerConfiguration>& configurations)
{
    auto result = discoverProjectMidiVoiceIslandsImpl(
        expandedDocument, expansion, configurations, true);
    if (!result.ok()) result.islands.clear();
    return result;
}
}
