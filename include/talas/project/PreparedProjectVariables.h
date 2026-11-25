#pragma once

#include "talas/expr/Expression.h"
#include "talas/project/ProjectVariables.h"
#include "talas/project/ProjectMidiVoiceIslands.h"

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

struct PreparedProjectVariableIdentity
{
    ProjectVariableOwnerKind ownerKind =
        ProjectVariableOwnerKind::Project;
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

    bool operator==(const PreparedProjectVariableIdentity&) const = default;
};

struct PreparedProjectVariableScope
{
    std::uint32_t scopeOrdinal = 0;
    std::uint32_t parentScopeOrdinal = 0;
    std::string definitionId;
    std::vector<std::string> instancePath;

    bool operator==(const PreparedProjectVariableScope&) const = default;
};

struct PreparedProjectVariableCell
{
    PreparedProjectVariableIdentity identity;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double initialValue = 0.0;
    std::uint32_t scopeOrdinal = 0;

    bool operator==(const PreparedProjectVariableCell&) const = default;
};

struct PreparedProjectVariableTable
{
    std::vector<PreparedProjectVariableScope> scopes;
    std::vector<PreparedProjectVariableCell> cells;

    bool operator==(const PreparedProjectVariableTable&) const = default;
};

struct PreparedProjectVariablesResult
{
    PreparedProjectVariableTable table;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

[[nodiscard]] PreparedProjectVariablesResult prepareProjectVariables(
    const ProjectDocument& document,
    const ProjectSoundExpansionManifest& expansion,
    const std::vector<ProjectMidiVoiceIsland>& midiVoiceIslands);

[[nodiscard]] PreparedProjectVariablesResult prepareProjectVariableBase(
    const ProjectDocument& document,
    const ProjectSoundExpansionManifest& expansion);

[[nodiscard]] std::optional<std::size_t>
resolvePreparedProjectVariableCell(
    const PreparedProjectVariableTable& table,
    std::uint32_t scopeOrdinal,
    std::string_view variableId) noexcept;

[[nodiscard]] std::optional<std::size_t>
findPreparedProjectVariableCell(
    const PreparedProjectVariableTable& table,
    const PreparedProjectVariableIdentity& identity) noexcept;
}
