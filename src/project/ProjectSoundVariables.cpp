#include "talas/project/ProjectSoundVariables.h"

#include "JsonValue.h"
#include "talas/project/ProjectDocument.h"

#include <algorithm>
#include <memory>
#include <ranges>
#include <unordered_set>

namespace talas::project
{
namespace
{
void appendDiagnostic(
    ProjectSoundVariableValidationResult& result,
    ProjectSoundVariableValidationCode code,
    std::string path,
    std::string message)
{
    if (result.truncated)
        return;
    if (result.diagnostics.size() >= maximumProjectSoundVariableDiagnostics)
    {
        result.truncated = true;
        return;
    }
    result.diagnostics.push_back({code, std::move(path), std::move(message)});
}

[[nodiscard]] bool containsControlCharacter(std::string_view value) noexcept
{
    return std::ranges::any_of(value, [](unsigned char character) {
        return character < 0x20U || character == 0x7fU;
    });
}

[[nodiscard]] const ProjectSoundDefinition* findSoundDefinition(
    const ProjectDocument& document,
    std::string_view definitionId) noexcept
{
    const auto found = std::ranges::find(
        document.soundDefinitions,
        definitionId,
        &ProjectSoundDefinition::definitionId);
    return found == document.soundDefinitions.end()
        ? nullptr
        : std::addressof(*found);
}

[[nodiscard]] const ProjectNode* findNode(
    const ProjectGraph& graph,
    std::string_view nodeId) noexcept
{
    const auto found = std::ranges::find(
        graph.nodes,
        nodeId,
        &ProjectNode::instanceId);
    return found == graph.nodes.end() ? nullptr : std::addressof(*found);
}

[[nodiscard]] std::string declarationIdentity(
    const ProjectSoundVariableDeclaration& declaration)
{
    std::string result;
    result.reserve(
        declaration.variableId.size()
        + declaration.owner.definitionId.size()
        + declaration.owner.nodeId.size() + 4U);
    switch (declaration.owner.kind)
    {
    case ProjectVariableOwnerKind::Project: result += "p:"; break;
    case ProjectVariableOwnerKind::SoundDefinition: result += "s:"; break;
    case ProjectVariableOwnerKind::MidiVoice: result += "v:"; break;
    case ProjectVariableOwnerKind::WorkspaceSession: result += "w:"; break;
    }
    result += declaration.owner.definitionId;
    result += ':';
    result += declaration.owner.nodeId;
    result += ':';
    result += declaration.variableId;
    return result;
}

void validateOwner(
    ProjectSoundVariableValidationResult& result,
    const ProjectDocument& document,
    const ProjectSoundVariableDeclaration& declaration,
    const std::string& path)
{
    switch (declaration.owner.kind)
    {
    case ProjectVariableOwnerKind::Project:
        if (!declaration.owner.definitionId.empty()
            || !declaration.owner.nodeId.empty())
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::InvalidOwner,
                path + ".owner",
                "Project-owned Sound variables accept kind only");
        }
        return;
    case ProjectVariableOwnerKind::SoundDefinition:
        if (!isValidProjectVariableIdentifier(
                declaration.owner.definitionId))
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::InvalidOwner,
                path + ".owner.definitionId",
                "Sound-owned variables require an exact definition ID");
        }
        else if (findSoundDefinition(
                     document,
                     declaration.owner.definitionId) == nullptr)
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::UnknownOwnerSoundDefinition,
                path + ".owner.definitionId",
                "Sound-variable owner references an unknown definition");
        }
        if (!declaration.owner.nodeId.empty())
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::InvalidOwner,
                path + ".owner.nodeId",
                "Sound-definition variables cannot name a MIDI Voice node");
        }
        return;
    case ProjectVariableOwnerKind::MidiVoice:
    {
        if (declaration.snapshotEligible)
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::InvalidOwner,
                path + ".snapshotEligible",
                "MIDI-voice Sound variables cannot be snapshot eligible");
        }
        const ProjectNode* ownerNode = nullptr;
        if (!isValidProjectVariableIdentifier(declaration.owner.nodeId))
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::InvalidOwner,
                path + ".owner.nodeId",
                "MIDI-voice Sound variables require an exact owner node ID");
        }
        else if (declaration.owner.definitionId.empty())
        {
            ownerNode = findNode(document.graph, declaration.owner.nodeId);
        }
        else if (!isValidProjectVariableIdentifier(
                     declaration.owner.definitionId))
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::InvalidOwner,
                path + ".owner.definitionId",
                "Definition-local MIDI Voice owners require an exact definition ID");
        }
        else if (const auto* definition = findSoundDefinition(
                     document,
                     declaration.owner.definitionId))
        {
            ownerNode = findNode(definition->graph, declaration.owner.nodeId);
        }
        else
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::UnknownOwnerSoundDefinition,
                path + ".owner.definitionId",
                "MIDI Voice owner references an unknown Sound definition");
        }
        if (ownerNode == nullptr || ownerNode->typeId != "talas.midiVoice")
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::UnknownMidiVoice,
                path + ".owner.nodeId",
                "Sound-variable owner must reference an exact talas.midiVoice node");
        }
        return;
    }
    case ProjectVariableOwnerKind::WorkspaceSession:
        if (!declaration.owner.definitionId.empty()
            || !declaration.owner.nodeId.empty())
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::InvalidOwner,
                path + ".owner",
                "Workspace-session Sound variables accept kind only");
        }
        return;
    }
}
}

ProjectSoundVariableValidationResult validateProjectSoundVariableLibrary(
    const ProjectDocument& document)
{
    ProjectSoundVariableValidationResult result;
    result.diagnostics.reserve(maximumProjectSoundVariableDiagnostics);
    const auto& library = document.soundVariableLibrary;
    if (library.version != 1)
    {
        appendDiagnostic(
            result,
            ProjectSoundVariableValidationCode::InvalidVersion,
            "$.soundVariableLibrary.version",
            "Sound-variable library version must be exactly 1");
    }
    if (library.variables.size() > maximumProjectSoundVariableCount)
    {
        appendDiagnostic(
            result,
            ProjectSoundVariableValidationCode::TooManyVariables,
            "$.soundVariableLibrary.variables",
            "Sound-variable library exceeds its fixed declaration limit");
        return result;
    }
    std::size_t totalCandidates = 0;
    for (const auto& declaration : library.variables)
    {
        if (declaration.candidateDefinitionIds.size()
            > maximumProjectSoundVariableCandidates)
        {
            continue;
        }
        if (totalCandidates
            > maximumProjectSoundVariableCandidateReferences
                - declaration.candidateDefinitionIds.size())
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::TooManyCandidateReferences,
                "$.soundVariableLibrary.variables",
                "Sound-variable candidates exceed the aggregate fixed limit");
            return result;
        }
        totalCandidates += declaration.candidateDefinitionIds.size();
    }

    std::unordered_set<std::string> identities;
    std::unordered_set<std::string> globalIds;
    identities.reserve(library.variables.size());
    globalIds.reserve(library.variables.size());
    for (std::size_t index = 0;
         index < library.variables.size() && !result.truncated;
         ++index)
    {
        const auto& declaration = library.variables[index];
        const auto path = "$.soundVariableLibrary.variables["
            + std::to_string(index) + "]";
        if (!isValidProjectVariableIdentifier(declaration.variableId))
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::InvalidIdentifier,
                path + ".variableId",
                "Sound-variable ID must be a bounded expression identifier");
        }
        if (!identities.insert(declarationIdentity(declaration)).second)
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::DuplicateDeclaration,
                path + ".variableId",
                "Sound-variable ID is duplicated in the same owner scope");
        }
        if ((declaration.owner.kind == ProjectVariableOwnerKind::Project
             || declaration.owner.kind
                    == ProjectVariableOwnerKind::WorkspaceSession)
            && !globalIds.insert(declaration.variableId).second)
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::AmbiguousGlobalOwner,
                path + ".variableId",
                "Project and workspace Sound variables cannot share one visible ID");
        }
        if (declaration.displayName.empty()
            || declaration.displayName.size()
                > maximumProjectVariableDisplayNameBytes
            || !json::isValidUtf8(declaration.displayName)
            || containsControlCharacter(declaration.displayName))
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::InvalidDisplayName,
                path + ".displayName",
                "Sound-variable display name must be bounded non-empty UTF-8 text");
        }
        switch (declaration.kind)
        {
        case ProjectSoundVariableKind::Sound:
        case ProjectSoundVariableKind::SoundCollection:
            break;
        default:
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::InvalidKind,
                path + ".kind",
                "Sound-variable kind is invalid");
            break;
        }
        validateOwner(result, document, declaration, path);
        if (declaration.candidateDefinitionIds.size()
            > maximumProjectSoundVariableCandidates)
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::TooManyCandidates,
                path + ".candidateDefinitionIds",
                "Sound variable exceeds its per-declaration candidate limit");
            continue;
        }
        std::unordered_set<std::string> candidates;
        candidates.reserve(declaration.candidateDefinitionIds.size());
        for (std::size_t candidateIndex = 0;
             candidateIndex < declaration.candidateDefinitionIds.size();
             ++candidateIndex)
        {
            const auto& definitionId =
                declaration.candidateDefinitionIds[candidateIndex];
            const auto candidatePath = path + ".candidateDefinitionIds["
                + std::to_string(candidateIndex) + "]";
            if (!isValidProjectVariableIdentifier(definitionId))
            {
                appendDiagnostic(
                    result,
                    ProjectSoundVariableValidationCode::InvalidCandidateDefinition,
                    candidatePath,
                    "Candidate must be an exact bounded Sound definition ID");
            }
            else if (!candidates.insert(definitionId).second)
            {
                appendDiagnostic(
                    result,
                    ProjectSoundVariableValidationCode::DuplicateCandidateDefinition,
                    candidatePath,
                    "Candidate Sound definition is duplicated");
            }
            else if (findSoundDefinition(document, definitionId) == nullptr)
            {
                appendDiagnostic(
                    result,
                    ProjectSoundVariableValidationCode::UnknownCandidateDefinition,
                    candidatePath,
                    "Candidate references an unknown Sound definition");
            }
        }
        if ((declaration.candidateDefinitionIds.empty()
                && declaration.initialSelectedOrdinal != 0)
            || (!declaration.candidateDefinitionIds.empty()
                && declaration.initialSelectedOrdinal
                    >= declaration.candidateDefinitionIds.size()))
        {
            appendDiagnostic(
                result,
                ProjectSoundVariableValidationCode::InvalidInitialSelection,
                path + ".initialSelectedOrdinal",
                "Initial Sound selection is outside the candidate list");
        }
    }
    return result;
}

const ProjectSoundVariableDeclaration*
findVisibleProjectSoundVariableDeclaration(
    const ProjectDocument& document,
    std::string_view variableId,
    std::span<const std::string_view> soundDefinitionScope,
    const ProjectVariableOwner* midiVoiceOwner) noexcept
{
    if (!isValidProjectVariableIdentifier(variableId))
        return nullptr;
    if (midiVoiceOwner != nullptr
        && midiVoiceOwner->kind == ProjectVariableOwnerKind::MidiVoice)
    {
        const auto voice = std::ranges::find_if(
            document.soundVariableLibrary.variables,
            [&](const ProjectSoundVariableDeclaration& declaration) {
                return declaration.variableId == variableId
                    && declaration.owner == *midiVoiceOwner;
            });
        if (voice != document.soundVariableLibrary.variables.end())
            return std::addressof(*voice);
    }
    for (auto scope = soundDefinitionScope.rbegin();
         scope != soundDefinitionScope.rend();
         ++scope)
    {
        const auto found = std::ranges::find_if(
            document.soundVariableLibrary.variables,
            [&](const ProjectSoundVariableDeclaration& declaration) {
                return declaration.variableId == variableId
                    && declaration.owner.kind
                        == ProjectVariableOwnerKind::SoundDefinition
                    && declaration.owner.definitionId == *scope;
            });
        if (found != document.soundVariableLibrary.variables.end())
            return std::addressof(*found);
    }
    const auto global = std::ranges::find_if(
        document.soundVariableLibrary.variables,
        [&](const ProjectSoundVariableDeclaration& declaration) {
            return declaration.variableId == variableId
                && (declaration.owner.kind
                        == ProjectVariableOwnerKind::Project
                    || declaration.owner.kind
                        == ProjectVariableOwnerKind::WorkspaceSession);
        });
    return global == document.soundVariableLibrary.variables.end()
        ? nullptr
        : std::addressof(*global);
}
}
