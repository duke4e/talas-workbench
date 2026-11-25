#pragma once

#include "talas/project/PreparedProjectVariables.h"
#include "talas/project/ProjectSoundExpansion.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace talas::project::detail
{
// Shared by scalar and Sound-variable preparation so both tables describe the
// same lexical tree. Structural expansion scopes have already been validated.
inline bool appendPreparedProjectVoiceScopes(
    std::vector<PreparedProjectVariableScope>& prepared,
    const ProjectSoundExpansionManifest& expansion,
    const std::vector<ProjectMidiVoiceIsland>& islands,
    std::string& error)
{
    struct Context
    {
        std::uint32_t sourceScope;
        std::vector<ProjectMidiVoiceAncestorIdentity> ancestors;
    };
    std::unordered_map<std::uint32_t, Context> contexts;
    for (const auto& scope : prepared)
        contexts.emplace(scope.scopeOrdinal, Context{scope.scopeOrdinal, {}});
    const auto source = [&](std::uint32_t ordinal) {
        return std::ranges::find(expansion.scopes, ordinal,
            &ExpandedSoundScopeIdentity::scopeOrdinal);
    };
    const auto fail = [&](const char* message) { error = message; return false; };
    std::size_t addedScopes = 0;
    std::unordered_set<std::uint64_t> projectionKeys;
    for (std::size_t index = 0; index < islands.size(); ++index)
    {
        const auto& island = islands[index];
        const auto parent = contexts.find(island.parentScopeOrdinal);
        const auto sourceOrdinal = island.voiceAncestors.empty()
            ? island.parentScopeOrdinal : island.sourceScopeOrdinal;
        const auto origin = source(sourceOrdinal);
        const auto validSource = sourceOrdinal == 0
            ? island.definitionId.empty() && island.instancePath.empty()
            : origin != expansion.scopes.end()
                && origin->definitionId == island.definitionId
                && origin->instancePath == island.instancePath;
        if (island.flatOwnerNodeId.empty() || island.localOwnerNodeId.empty()
            || island.voiceCount == 0 || island.voiceCount > maximumPreparedMidiVoiceSlotsPerOwner
            || island.voiceAncestors.size() >= maximumPreparedMidiVoiceNestingDepth
            || !validSource || parent == contexts.end()
            || parent->second.ancestors != island.voiceAncestors
            || parent->second.sourceScope != sourceOrdinal
            || (island.sourceScopeOrdinal != 0 && island.sourceScopeOrdinal != sourceOrdinal)
            || island.firstVoiceScopeOrdinal <= island.parentScopeOrdinal
            || island.voiceCount - 1U
                > std::numeric_limits<std::uint32_t>::max() - island.firstVoiceScopeOrdinal
            || island.voiceCount > maximumPreparedMidiVoiceHierarchyScopes - addedScopes)
            return fail("MIDI Voice island contains an invalid prepared scope or ancestry");
        for (std::size_t previous = 0; previous < index; ++previous)
        {
            const auto& other = islands[previous];
            if (other.voiceAncestors == island.voiceAncestors
                && (other.flatOwnerNodeId == island.flatOwnerNodeId
                    || (other.localOwnerNodeId == island.localOwnerNodeId
                        && other.definitionId == island.definitionId
                        && other.instancePath == island.instancePath)))
                return fail("MIDI Voice island duplicates a prepared owner identity");
        }
        addedScopes += island.voiceCount;
        for (std::size_t voice = 0; voice < island.voiceCount; ++voice)
        {
            const auto ordinal = island.firstVoiceScopeOrdinal + static_cast<std::uint32_t>(voice);
            auto ancestors = island.voiceAncestors;
            ancestors.push_back({island.localOwnerNodeId, island.definitionId,
                island.instancePath, static_cast<std::uint16_t>(voice)});
            if (!contexts.emplace(ordinal, Context{sourceOrdinal, std::move(ancestors)}).second)
                return fail("MIDI Voice island duplicates a prepared voice scope");
            prepared.push_back({ordinal, island.parentScopeOrdinal,
                island.definitionId, island.instancePath});
        }
        for (const auto& projection : island.scopeProjections)
        {
            const auto original = source(projection.sourceScopeOrdinal);
            const auto projectedParent = contexts.find(projection.parentScopeOrdinal);
            if (original == expansion.scopes.end() || projectedParent == contexts.end()
                || projection.scopeOrdinal <= projection.parentScopeOrdinal
                || original->parentScopeOrdinal != projectedParent->second.sourceScope
                || projection.voiceAncestors != projectedParent->second.ancestors
                || projection.voiceAncestors.size() != island.voiceAncestors.size() + 1U
                || !std::equal(island.voiceAncestors.begin(), island.voiceAncestors.end(),
                    projection.voiceAncestors.begin())
                || addedScopes >= maximumPreparedMidiVoiceHierarchyScopes)
                return fail("MIDI Voice Sound projection contains an invalid parent or ancestry");
            const auto& owner = projection.voiceAncestors.back();
            if (owner.ownerNodeId != island.localOwnerNodeId
                || owner.definitionId != island.definitionId || owner.instancePath != island.instancePath
                || owner.voiceOrdinal >= island.voiceCount
                || !projectionKeys.insert((static_cast<std::uint64_t>(projection.parentScopeOrdinal) << 32U)
                    | projection.sourceScopeOrdinal).second
                || !contexts.emplace(projection.scopeOrdinal,
                    Context{projection.sourceScopeOrdinal, projection.voiceAncestors}).second)
                return fail("MIDI Voice Sound projection duplicates or misidentifies its owner");
            ++addedScopes;
            prepared.push_back({projection.scopeOrdinal, projection.parentScopeOrdinal,
                original->definitionId, original->instancePath});
        }
    }
    return true;
}
}
