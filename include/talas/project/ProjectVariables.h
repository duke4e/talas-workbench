#pragma once

#include "talas/expr/Expression.h"

#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
struct ProjectDocument;
struct ProjectGraph;

inline constexpr std::size_t maximumProjectVariableCount = 128;
inline constexpr std::size_t maximumProjectVariableIdentifierBytes = 128;
inline constexpr std::size_t maximumProjectVariableDisplayNameBytes = 256;
inline constexpr std::size_t maximumPreparedProjectVariableCells = 2048;
inline constexpr std::size_t maximumPreparedProjectVariableReferenceUses = 4096;
inline constexpr std::size_t maximumProjectVariableDiagnostics = 64;

enum class ProjectVariableOwnerKind
{
    Project,
    SoundDefinition,
    MidiVoice,
    WorkspaceSession
};

struct ProjectVariableOwner
{
    ProjectVariableOwnerKind kind = ProjectVariableOwnerKind::Project;
    std::string definitionId;
    std::string nodeId;

    bool operator==(const ProjectVariableOwner&) const = default;
};

struct ProjectVariableDeclaration
{
    std::string variableId;
    std::string displayName;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double initialValue = 0.0;
    ProjectVariableOwner owner;
    bool snapshotEligible = true;

    bool operator==(const ProjectVariableDeclaration&) const = default;
};

struct ProjectVariableLibrary
{
    int version = 2;
    std::vector<ProjectVariableDeclaration> variables;

    bool operator==(const ProjectVariableLibrary&) const = default;
};

enum class ProjectVariableValidationCode
{
    InvalidVersion,
    UnsupportedLanguage,
    TooManyVariables,
    InvalidIdentifier,
    DuplicateDeclaration,
    InvalidDisplayName,
    InvalidDomain,
    NonFiniteInitialValue,
    InvalidOwner,
    UnknownSoundDefinition,
    UnknownMidiVoice,
    AmbiguousGlobalOwner
};

struct ProjectVariableDiagnostic
{
    ProjectVariableValidationCode code =
        ProjectVariableValidationCode::InvalidIdentifier;
    std::string path;
    std::string message;
};

struct ProjectVariableValidationResult
{
    std::vector<ProjectVariableDiagnostic> diagnostics;
    bool truncated = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty() && !truncated;
    }
};

[[nodiscard]] bool isValidProjectVariableIdentifier(
    std::string_view value) noexcept;
[[nodiscard]] bool isSupportedProjectVariableDomain(
    talas::expr::ValueDomain domain) noexcept;

[[nodiscard]] ProjectVariableValidationResult validateProjectVariableLibrary(
    const ProjectDocument& document);

// The variables a project's expressions name that no declaration does, in
// the order they first appear: those whose value is asked for as the Sound
// plays and kept in the editor's environment.
[[nodiscard]] std::vector<std::string> undeclaredProjectVariables(
    const ProjectDocument& document);

// The variables a Script binds, by the Sounds whose
// fields read them. Each event a Script sends an Input, as `filts start: 0
// s amps: x`, gives the variables its keywords name (?amps) to that Input
// and the Sounds feeding it, which are then not free: no value is asked for
// them and a class made from the example takes no parameter for them. Taken
// out of the event, the variable is free again.
[[nodiscard]] std::map<std::string, std::set<std::string>> scriptBoundVariables(const ProjectGraph& graph);

// The Sound compiler's warnings of a Script
// variable declared and assigned a value that is never read, by the Script
// that has it. They warn alone: the Sound compiles and plays.
struct ScriptCompilerWarning
{
    std::string instanceId;
    std::string message;

    bool operator==(const ScriptCompilerWarning&) const = default;
};
[[nodiscard]] std::vector<ScriptCompilerWarning> scriptCompilerWarnings(const ProjectDocument& document);

// soundDefinitionScope is ordered outermost to innermost. Resolution walks it
// from the innermost definition to the project owner, implementing the exact
// lexical shadowing contract without display-name or index fallback. An exact
// MIDI Voice island owner, when supplied, shadows the enclosing lexical scope.
// Same-Sound ancestor voice owners are ordered nearest to farthest.
[[nodiscard]] const ProjectVariableDeclaration*
findVisibleProjectVariableDeclaration(
    const ProjectDocument& document,
    std::string_view variableId,
    std::span<const std::string_view> soundDefinitionScope = {},
    const ProjectVariableOwner* midiVoiceOwner = nullptr,
    std::span<const ProjectVariableOwner> midiVoiceAncestors = {}) noexcept;
}
