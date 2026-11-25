#pragma once

#include "talas/expr/Expression.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace talas::project
{
struct ProjectDocument;

inline constexpr std::size_t maximumProjectLiveSets = 16;
inline constexpr std::uint32_t projectLiveSetRows = 8;
inline constexpr std::uint32_t projectLiveSetColumns = 16;
static_assert(projectLiveSetColumns <= 32U);
inline constexpr std::size_t maximumProjectLiveSetRoutes =
    projectLiveSetColumns * (projectLiveSetColumns - 1U) / 2U;
inline constexpr std::size_t maximumProjectLiveSetCells = 1024;
inline constexpr std::size_t maximumProjectLiveSetMappingsPerCell = 8;
inline constexpr std::size_t maximumProjectLiveSetIdentifierBytes = 128;
inline constexpr std::size_t maximumProjectLiveSetDisplayNameBytes = 256;
inline constexpr std::size_t maximumProjectLiveSetDiagnostics = 256;
inline constexpr std::size_t maximumProjectLiveSetInspectedRecords = 8192;
inline constexpr std::uint64_t maximumProjectLiveSetQuantizationTicks =
    32U * 960U;

inline constexpr std::size_t maximumProjectLiveTools = 32;
inline constexpr std::size_t maximumProjectLiveToolStates = 32;
inline constexpr std::size_t maximumProjectLiveToolVariables = 64;
inline constexpr std::size_t maximumProjectLiveToolTransitions = 256;
inline constexpr std::size_t maximumProjectLiveToolCommandsPerTransition = 8;
inline constexpr std::size_t maximumProjectLiveToolGuardBytes = 4096;
inline constexpr std::size_t maximumPreparedLiveToolInstructions = 16'384;
inline constexpr std::size_t maximumPreparedLiveToolVariables =
    maximumProjectLiveTools * maximumProjectLiveToolVariables;
inline constexpr std::size_t maximumRealtimeLiveToolTriggerQueue = 64;

enum class ProjectLiveSetQuantizationKind
{
    Immediate,
    NextBeat,
    NextBar,
    TickInterval,
};

struct ProjectLiveSetQuantization
{
    ProjectLiveSetQuantizationKind kind =
        ProjectLiveSetQuantizationKind::Immediate;
    std::uint64_t tickInterval = 0;

    bool operator==(const ProjectLiveSetQuantization&) const = default;
};

struct ProjectLiveSetSoundTarget
{
    std::string soundDefinitionId;
    std::optional<std::string> sourceInstanceId;

    bool operator==(const ProjectLiveSetSoundTarget&) const = default;
};

struct ProjectLiveSetSnapshotTarget
{
    std::string bankId;
    std::string snapshotId;

    bool operator==(const ProjectLiveSetSnapshotTarget&) const = default;
};

struct ProjectLiveSetScoreRegionTarget
{
    std::string markerId;

    bool operator==(const ProjectLiveSetScoreRegionTarget&) const = default;
};

struct ProjectLiveSetStopTarget
{
    bool operator==(const ProjectLiveSetStopTarget&) const = default;
};

struct ProjectLiveSetTrackMuteTarget
{
    bool muted = true;

    bool operator==(const ProjectLiveSetTrackMuteTarget&) const = default;
};

using ProjectLiveSetCellTarget = std::variant<
    ProjectLiveSetSoundTarget,
    ProjectLiveSetSnapshotTarget,
    ProjectLiveSetScoreRegionTarget,
    ProjectLiveSetTrackMuteTarget,
    ProjectLiveSetStopTarget>;

struct ProjectLiveSetMidiProgramMapping
{
    std::int32_t channel = 1;
    std::int32_t program = 0;
    std::string sourceIdentifier;

    bool operator==(const ProjectLiveSetMidiProgramMapping&) const = default;
};

struct ProjectLiveSetOscMapping
{
    std::string address;

    bool operator==(const ProjectLiveSetOscMapping&) const = default;
};

// Explicitly records that this cell is available through the shared
// Workbench activation surface. Keyboard shortcuts remain session/UI policy
// and are intentionally not persisted in the project.
struct ProjectLiveSetWorkbenchMapping
{
    bool operator==(const ProjectLiveSetWorkbenchMapping&) const = default;
};

using ProjectLiveSetTriggerMapping = std::variant<
    ProjectLiveSetMidiProgramMapping,
    ProjectLiveSetOscMapping,
    ProjectLiveSetWorkbenchMapping>;

struct ProjectLiveSetCell
{
    std::string cellId;
    std::string displayName;
    std::uint32_t row = 0;
    std::uint32_t column = 0;
    ProjectLiveSetQuantization quantization;
    ProjectLiveSetCellTarget target = ProjectLiveSetStopTarget{};
    std::vector<ProjectLiveSetTriggerMapping> mappings;

    bool operator==(const ProjectLiveSetCell&) const = default;
};

// A routed track feeds each downstream track instead of the master output.
// Tracks with no route continue to feed the master output.
struct ProjectLiveSetRoute
{
    std::uint32_t fromColumn = 0;
    std::uint32_t toColumn = 0;

    bool operator==(const ProjectLiveSetRoute&) const = default;
};

struct ProjectLiveSet
{
    std::string liveSetId;
    std::string displayName;
    std::vector<ProjectLiveSetCell> cells;
    std::vector<ProjectLiveSetRoute> routes;

    bool operator==(const ProjectLiveSet&) const = default;
};

enum class ProjectLiveToolVariableKind
{
    Number,
    Boolean,
};

struct ProjectLiveToolState
{
    std::string stateId;
    std::string displayName;

    bool operator==(const ProjectLiveToolState&) const = default;
};

struct ProjectLiveToolVariable
{
    std::string variableId;
    std::string displayName;
    ProjectLiveToolVariableKind kind = ProjectLiveToolVariableKind::Number;
    double initialNumber = 0.0;
    bool initialBoolean = false;
    std::optional<std::string> stateId;

    bool operator==(const ProjectLiveToolVariable&) const = default;
};

enum class ProjectLiveToolTriggerKind
{
    Manual,
    EventValue,
    VariableChange,
    MidiProgram,
    Osc,
    PeriodicTicks,
};

struct ProjectLiveToolTrigger
{
    ProjectLiveToolTriggerKind kind = ProjectLiveToolTriggerKind::Manual;
    std::string eventName;
    talas::expr::ValueDomain eventDomain = talas::expr::ValueDomain::Scalar;
    std::string variableId;
    ProjectLiveSetMidiProgramMapping midiProgram;
    ProjectLiveSetOscMapping osc;
    std::uint64_t periodTicks = 0;

    bool operator==(const ProjectLiveToolTrigger&) const = default;
};

enum class ProjectLiveToolCommandKind
{
    ActivateCell,
    Stop,
    SetProjectVariable,
    SetWorkspaceVariable,
    SetEventValue,
};

struct ProjectLiveToolCommand
{
    ProjectLiveToolCommandKind kind = ProjectLiveToolCommandKind::Stop;
    std::string liveSetId;
    std::string cellId;
    std::string variableId;
    std::string eventName;
    talas::expr::ValueDomain eventDomain = talas::expr::ValueDomain::Scalar;
    double value = 0.0;

    bool operator==(const ProjectLiveToolCommand&) const = default;
};

struct ProjectLiveToolTransition
{
    std::string transitionId;
    std::string fromStateId;
    std::string toStateId;
    ProjectLiveToolTrigger trigger;
    std::string guardExpression;
    std::vector<ProjectLiveToolCommand> commands;

    bool operator==(const ProjectLiveToolTransition&) const = default;
};

struct ProjectLiveTool
{
    std::string toolId;
    std::string displayName;
    std::string initialStateId;
    std::vector<ProjectLiveToolState> states;
    std::vector<ProjectLiveToolVariable> variables;
    std::vector<ProjectLiveToolTransition> transitions;

    bool operator==(const ProjectLiveTool&) const = default;
};

struct ProjectLiveSetLibrary
{
    std::uint32_t schemaVersion = 1;
    std::vector<ProjectLiveSet> liveSets;
    std::vector<ProjectLiveTool> tools;

    bool operator==(const ProjectLiveSetLibrary&) const = default;
};

enum class ProjectLiveSetValidationCode
{
    CapacityExceeded,
    InvalidIdentifier,
    InvalidDisplayName,
    DuplicateIdentifier,
    DuplicateCellPosition,
    InvalidQuantization,
    InvalidTarget,
    UnknownTarget,
    InvalidMapping,
    DuplicateMapping,
    InvalidTool,
    InvalidState,
    InvalidVariable,
    InvalidTransition,
    InvalidCommand,
    InvalidRoute,
    RouteCycle,
};

struct ProjectLiveSetDiagnostic
{
    ProjectLiveSetValidationCode code =
        ProjectLiveSetValidationCode::InvalidTarget;
    std::string path;
    std::string message;

    bool operator==(const ProjectLiveSetDiagnostic&) const = default;
};

struct ProjectLiveSetValidationResult
{
    std::vector<ProjectLiveSetDiagnostic> diagnostics;
    bool truncated = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty() && !truncated;
    }
};

[[nodiscard]] bool isValidProjectLiveSetIdentifier(
    std::string_view value) noexcept;
// Live Tool guards use the fixed VariablesV1 dialect. The project's
// definitions are available to them only when they are valid there, so
// definitions written for a newer language cannot invalidate every guard.
[[nodiscard]] const talas::expr::ExpressionDefinitionLibrary& liveToolGuardDefinitions(
    const ProjectDocument& document);

[[nodiscard]] ProjectLiveSetValidationResult validateProjectLiveSetLibrary(
    const ProjectDocument& document);
}
