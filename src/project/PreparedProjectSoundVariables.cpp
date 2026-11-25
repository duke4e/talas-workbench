#include "talas/project/PreparedProjectSoundVariables.h"

#include "PreparedProjectVoiceScopes.h"

#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectMidiVoiceIslands.h"
#include "talas/project/ProjectSoundExpansion.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <vector>

namespace talas::project
{
namespace
{
[[nodiscard]] const PreparedProjectVariableScope* findScope(
    const PreparedProjectSoundVariableTable& table,
    std::uint32_t scopeOrdinal) noexcept
{
    const auto found = std::ranges::find(
        table.scopes,
        scopeOrdinal,
        &PreparedProjectVariableScope::scopeOrdinal);
    return found == table.scopes.end() ? nullptr : &*found;
}

[[nodiscard]] bool declarationLess(
    const ProjectSoundVariableDeclaration* left,
    const ProjectSoundVariableDeclaration* right) noexcept
{
    if (left->owner.kind != right->owner.kind)
        return left->owner.kind < right->owner.kind;
    if (left->owner.definitionId != right->owner.definitionId)
        return left->owner.definitionId < right->owner.definitionId;
    if (left->owner.nodeId != right->owner.nodeId)
        return left->owner.nodeId < right->owner.nodeId;
    return left->variableId < right->variableId;
}

PreparedProjectSoundVariablesResult prepareImpl(
    const ProjectDocument& document,
    const ProjectSoundExpansionManifest& expansion,
    const std::vector<ProjectMidiVoiceIsland>* midiVoiceIslands)
{
    PreparedProjectSoundVariablesResult result;
    const auto validation = validateProjectSoundVariableLibrary(document);
    if (!validation.ok())
    {
        result.error = validation.diagnostics.empty()
            ? "Sound-variable validation exceeded its diagnostic limit"
            : validation.diagnostics.front().message;
        return result;
    }

    result.table.scopes.reserve(expansion.scopes.size() + 1U);
    result.table.scopes.push_back({0, 0, {}, {}});
    auto orderedScopes = expansion.scopes;
    std::ranges::sort(
        orderedScopes,
        {},
        &ExpandedSoundScopeIdentity::scopeOrdinal);
    for (const auto& scope : orderedScopes)
    {
        if (scope.scopeOrdinal == 0
            || scope.parentScopeOrdinal >= scope.scopeOrdinal
            || scope.instancePath.empty()
            || scope.definitionId.empty()
            || findScope(result.table, scope.parentScopeOrdinal) == nullptr
            || findScope(result.table, scope.scopeOrdinal) != nullptr)
        {
            result.error =
                "Sound expansion contains an invalid Sound-variable scope";
            return result;
        }
        result.table.scopes.push_back({
            scope.scopeOrdinal,
            scope.parentScopeOrdinal,
            scope.definitionId,
            scope.instancePath,
        });
    }
    if (midiVoiceIslands != nullptr
        && !detail::appendPreparedProjectVoiceScopes(
            result.table.scopes, expansion, *midiVoiceIslands, result.error))
        return result;

    std::vector<const ProjectSoundVariableDeclaration*> declarations;
    declarations.reserve(document.soundVariableLibrary.variables.size());
    for (const auto& declaration : document.soundVariableLibrary.variables)
        declarations.push_back(&declaration);
    std::ranges::sort(declarations, declarationLess);

    std::size_t preparedCandidates = 0;
    const auto appendCell = [&](
        const ProjectSoundVariableDeclaration& declaration,
        std::uint32_t scopeOrdinal,
        std::vector<std::string> instancePath,
        std::optional<std::uint16_t> voiceOrdinal = std::nullopt,
        const std::vector<ProjectMidiVoiceAncestorIdentity>& ancestors = {}) {
        if (result.table.cells.size()
            >= maximumPreparedProjectSoundVariableCells)
        {
            result.error =
                "Expanded Sound-variable cells exceed prepared capacity";
            return false;
        }
        if (declaration.candidateDefinitionIds.size()
                > maximumPreparedProjectSoundVariableCandidates
            || preparedCandidates
                > maximumPreparedProjectSoundVariableCandidates
                    - declaration.candidateDefinitionIds.size())
        {
            result.error =
                "Expanded Sound-variable candidates exceed prepared capacity";
            return false;
        }
        preparedCandidates += declaration.candidateDefinitionIds.size();
        result.table.cells.push_back({
            {
                declaration.owner.kind,
                declaration.variableId,
                declaration.owner.definitionId,
                std::move(instancePath),
                declaration.owner.nodeId,
                voiceOrdinal,
                ancestors,
            },
            declaration.kind,
            declaration.candidateDefinitionIds,
            declaration.initialSelectedOrdinal,
            scopeOrdinal,
        });
        return true;
    };

    std::vector<const ProjectSoundVariableDeclaration*> voiceDeclarations;
    voiceDeclarations.reserve(declarations.size());
    for (const auto* declaration : declarations)
    {
        if (declaration->owner.kind == ProjectVariableOwnerKind::Project
            || declaration->owner.kind
                == ProjectVariableOwnerKind::WorkspaceSession)
        {
            if (!appendCell(*declaration, 0, {}))
                return result;
            continue;
        }
        if (declaration->owner.kind == ProjectVariableOwnerKind::MidiVoice)
        {
            voiceDeclarations.push_back(declaration);
            continue;
        }
        for (const auto& scope : expansion.scopes)
        {
            if (scope.definitionId != declaration->owner.definitionId)
                continue;
            if (!appendCell(
                    *declaration,
                    scope.scopeOrdinal,
                    scope.instancePath))
            {
                return result;
            }
        }
        if (midiVoiceIslands != nullptr)
        {
            for (const auto& island : *midiVoiceIslands)
            {
                for (const auto& projection : island.scopeProjections)
                {
                    const auto scope = std::ranges::find(expansion.scopes,
                        projection.sourceScopeOrdinal, &ExpandedSoundScopeIdentity::scopeOrdinal);
                    if (scope->definitionId == declaration->owner.definitionId
                        && !appendCell(*declaration, projection.scopeOrdinal, scope->instancePath,
                            std::nullopt, projection.voiceAncestors))
                        return result;
                }
            }
        }
    }

    if (midiVoiceIslands == nullptr)
        return result;

    std::vector<bool> matchedVoiceDeclarations(
        voiceDeclarations.size(),
        false);
    for (const auto& island : *midiVoiceIslands)
    {
        for (std::size_t voiceIndex = 0;
             voiceIndex < island.voiceCount;
             ++voiceIndex)
        {
            if (voiceIndex > std::numeric_limits<std::uint16_t>::max())
            {
                result.error =
                    "MIDI Voice ordinal exceeds Sound-variable identity capacity";
                return result;
            }
            for (std::size_t declarationIndex = 0;
                 declarationIndex < voiceDeclarations.size();
                 ++declarationIndex)
            {
                const auto* declaration = voiceDeclarations[declarationIndex];
                if (island.localOwnerNodeId != declaration->owner.nodeId
                    || island.definitionId != declaration->owner.definitionId)
                {
                    continue;
                }
                matchedVoiceDeclarations[declarationIndex] = true;
                if (!appendCell(
                        *declaration,
                        island.firstVoiceScopeOrdinal
                            + static_cast<std::uint32_t>(voiceIndex),
                        island.instancePath,
                        static_cast<std::uint16_t>(voiceIndex),
                        island.voiceAncestors))
                {
                    return result;
                }
            }
        }
    }
    if (std::ranges::find(matchedVoiceDeclarations, false)
        != matchedVoiceDeclarations.end())
    {
        result.error =
            "MIDI-voice Sound-variable owner did not resolve to a reachable island";
    }
    return result;
}
}

PreparedProjectSoundVariablesResult prepareProjectSoundVariables(
    const ProjectDocument& document,
    const ProjectSoundExpansionManifest& expansion,
    const std::vector<ProjectMidiVoiceIsland>& midiVoiceIslands)
{
    return prepareImpl(document, expansion, &midiVoiceIslands);
}

PreparedProjectSoundVariablesResult prepareProjectSoundVariableBase(
    const ProjectDocument& document,
    const ProjectSoundExpansionManifest& expansion)
{
    return prepareImpl(document, expansion, nullptr);
}

std::optional<std::size_t> resolvePreparedProjectSoundVariableCell(
    const PreparedProjectSoundVariableTable& table,
    std::uint32_t scopeOrdinal,
    std::string_view variableId) noexcept
{
    auto current = scopeOrdinal;
    const auto maximumDepth = std::max<std::size_t>(
        1, table.scopes.size());
    for (std::size_t depth = 0; depth <= maximumDepth; ++depth)
    {
        const auto cell = std::ranges::find_if(
            table.cells,
            [&](const PreparedProjectSoundVariableCell& candidate) {
                return candidate.scopeOrdinal == current
                    && candidate.identity.variableId == variableId;
            });
        if (cell != table.cells.end())
        {
            return static_cast<std::size_t>(
                std::distance(table.cells.begin(), cell));
        }
        if (current == 0)
            return std::nullopt;
        const auto* scope = findScope(table, current);
        if (scope == nullptr || scope->parentScopeOrdinal == current)
            return std::nullopt;
        current = scope->parentScopeOrdinal;
    }
    return std::nullopt;
}

std::optional<std::size_t> findPreparedProjectSoundVariableCell(
    const PreparedProjectSoundVariableTable& table,
    const PreparedProjectSoundVariableIdentity& identity) noexcept
{
    const auto found = std::ranges::find(
        table.cells,
        identity,
        &PreparedProjectSoundVariableCell::identity);
    if (found == table.cells.end())
        return std::nullopt;
    return static_cast<std::size_t>(
        std::distance(table.cells.begin(), found));
}
}
