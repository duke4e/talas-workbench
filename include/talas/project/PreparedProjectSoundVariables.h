#pragma once

#include "talas/project/PreparedProjectVariables.h"
#include "talas/project/ProjectSoundVariables.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
struct ProjectDocument;
struct ProjectMidiVoiceIsland;
struct ProjectSoundExpansionManifest;

struct PreparedProjectSoundVariableIdentity
{
    ProjectVariableOwnerKind ownerKind = ProjectVariableOwnerKind::Project;
    std::string variableId;
    std::string definitionId;
    std::vector<std::string> instancePath;
    std::string ownerNodeId;
    std::optional<std::uint16_t> voiceOrdinal;
    std::vector<ProjectMidiVoiceAncestorIdentity> voiceAncestors;

    [[nodiscard]] bool isVoiceLocal() const noexcept
    {
        return ownerKind == ProjectVariableOwnerKind::MidiVoice || !voiceAncestors.empty();
    }

    bool operator==(const PreparedProjectSoundVariableIdentity&) const = default;
};

struct PreparedProjectSoundVariableCell
{
    PreparedProjectSoundVariableIdentity identity;
    ProjectSoundVariableKind kind = ProjectSoundVariableKind::Sound;
    std::vector<std::string> candidateDefinitionIds;
    std::uint16_t initialSelectedOrdinal = 0;
    std::uint32_t scopeOrdinal = 0;

    bool operator==(const PreparedProjectSoundVariableCell&) const = default;
};

struct PreparedProjectSoundVariableTable
{
    std::vector<PreparedProjectVariableScope> scopes;
    std::vector<PreparedProjectSoundVariableCell> cells;

    bool operator==(const PreparedProjectSoundVariableTable&) const = default;
};

struct PreparedProjectSoundVariablesResult
{
    PreparedProjectSoundVariableTable table;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] PreparedProjectSoundVariablesResult prepareProjectSoundVariables(
    const ProjectDocument& document,
    const ProjectSoundExpansionManifest& expansion,
    const std::vector<ProjectMidiVoiceIsland>& midiVoiceIslands);

[[nodiscard]] PreparedProjectSoundVariablesResult
prepareProjectSoundVariableBase(
    const ProjectDocument& document,
    const ProjectSoundExpansionManifest& expansion);

[[nodiscard]] std::optional<std::size_t>
resolvePreparedProjectSoundVariableCell(
    const PreparedProjectSoundVariableTable& table,
    std::uint32_t scopeOrdinal,
    std::string_view variableId) noexcept;

[[nodiscard]] std::optional<std::size_t>
findPreparedProjectSoundVariableCell(
    const PreparedProjectSoundVariableTable& table,
    const PreparedProjectSoundVariableIdentity& identity) noexcept;
}
