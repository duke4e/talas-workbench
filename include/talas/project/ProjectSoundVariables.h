#pragma once

#include "talas/project/ProjectVariables.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
struct ProjectDocument;

inline constexpr std::size_t maximumProjectSoundVariableCount = 64;
inline constexpr std::size_t maximumProjectSoundVariableCandidates = 32;
inline constexpr std::size_t maximumProjectSoundVariableCandidateReferences = 512;
inline constexpr std::size_t maximumPreparedProjectSoundVariableCells = 512;
inline constexpr std::size_t maximumPreparedProjectSoundVariableCandidates = 2048;
inline constexpr std::size_t maximumProjectSoundVariableDiagnostics = 64;

enum class ProjectSoundVariableKind
{
    Sound,
    SoundCollection
};

struct ProjectSoundVariableDeclaration
{
    std::string variableId;
    std::string displayName;
    ProjectVariableOwner owner;
    ProjectSoundVariableKind kind = ProjectSoundVariableKind::Sound;
    std::vector<std::string> candidateDefinitionIds;
    std::uint16_t initialSelectedOrdinal = 0;
    bool snapshotEligible = true;

    bool operator==(const ProjectSoundVariableDeclaration&) const = default;
};

struct ProjectSoundVariableLibrary
{
    int version = 1;
    std::vector<ProjectSoundVariableDeclaration> variables;

    bool operator==(const ProjectSoundVariableLibrary&) const = default;
};

enum class ProjectSoundVariableValidationCode
{
    InvalidVersion,
    TooManyVariables,
    TooManyCandidateReferences,
    InvalidIdentifier,
    DuplicateDeclaration,
    AmbiguousGlobalOwner,
    InvalidDisplayName,
    InvalidKind,
    InvalidOwner,
    UnknownOwnerSoundDefinition,
    UnknownMidiVoice,
    TooManyCandidates,
    InvalidCandidateDefinition,
    DuplicateCandidateDefinition,
    UnknownCandidateDefinition,
    InvalidInitialSelection
};

struct ProjectSoundVariableDiagnostic
{
    ProjectSoundVariableValidationCode code =
        ProjectSoundVariableValidationCode::InvalidIdentifier;
    std::string path;
    std::string message;
};

struct ProjectSoundVariableValidationResult
{
    std::vector<ProjectSoundVariableDiagnostic> diagnostics;
    bool truncated = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty() && !truncated;
    }
};

[[nodiscard]] ProjectSoundVariableValidationResult
validateProjectSoundVariableLibrary(const ProjectDocument& document);

[[nodiscard]] const ProjectSoundVariableDeclaration*
findVisibleProjectSoundVariableDeclaration(
    const ProjectDocument& document,
    std::string_view variableId,
    std::span<const std::string_view> soundDefinitionScope = {},
    const ProjectVariableOwner* midiVoiceOwner = nullptr) noexcept;
}
