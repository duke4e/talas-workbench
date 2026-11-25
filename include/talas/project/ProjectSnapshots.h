#pragma once

#include "talas/expr/Expression.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::core
{
class NodeRegistry;
}

namespace talas::project
{
struct ProjectDocument;

inline constexpr std::size_t maximumSnapshotBankCount = 64;
inline constexpr std::size_t maximumSnapshotsPerBank = 128;
inline constexpr std::size_t maximumSnapshotValuesPerSnapshot = 512;
inline constexpr std::size_t maximumSnapshotPoliciesPerBank = 512;
inline constexpr std::size_t maximumSnapshotIdentifierBytes = 128;
inline constexpr std::size_t maximumSnapshotNameBytes = 256;
inline constexpr double maximumSnapshotSmoothingSeconds = 30.0;

enum class SnapshotScopeKind
{
    Project,
    Definition,
    Node
};

struct SnapshotScopeIdentity
{
    SnapshotScopeKind kind = SnapshotScopeKind::Project;
    std::vector<std::string> instancePath;
    std::optional<std::string> containingDefinitionId;
    std::string definitionId;
    std::string nodeId;

    bool operator==(const SnapshotScopeIdentity&) const = default;
};

enum class SnapshotTargetKind
{
    EventValue,
    NodeParameter,
    SoundParameter,
    Variable,
    SoundVariable
};

enum class SnapshotVariableOwnerKind
{
    Project,
    WorkspaceSession
};

struct SnapshotTargetIdentity
{
    SnapshotTargetKind kind = SnapshotTargetKind::EventValue;
    SnapshotVariableOwnerKind variableOwnerKind =
        SnapshotVariableOwnerKind::Project;
    std::vector<std::string> instancePath;
    std::optional<std::string> containingDefinitionId;
    std::string nodeId;
    std::string definitionId;
    std::string parameterId;
    std::string variableId;
    std::string eventName;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;

    bool operator==(const SnapshotTargetIdentity&) const = default;
};

enum class SnapshotValueKind
{
    Number,
    Choice,
    Boolean
};

struct SnapshotValue
{
    SnapshotValueKind kind = SnapshotValueKind::Number;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double number = 0.0;
    std::string choice;
    bool boolean = false;

    bool operator==(const SnapshotValue&) const = default;
};

struct SnapshotValueEntry
{
    SnapshotTargetIdentity target;
    SnapshotValue value;

    bool operator==(const SnapshotValueEntry&) const = default;
};

struct SnapshotParameterRange
{
    double minimum = 0.0;
    double maximum = 1.0;

    bool operator==(const SnapshotParameterRange&) const = default;
};

struct SnapshotParameterPolicy
{
    SnapshotTargetIdentity target;
    bool locked = false;
    std::optional<SnapshotParameterRange> range;

    bool operator==(const SnapshotParameterPolicy&) const = default;
};

struct ProjectSnapshot
{
    std::string snapshotId;
    std::string displayName;
    std::vector<SnapshotValueEntry> values;

    bool operator==(const ProjectSnapshot&) const = default;
};

struct ProjectSnapshotBank
{
    std::string bankId;
    std::string displayName;
    SnapshotScopeIdentity scope;
    double smoothingSeconds = 0.0;
    std::vector<SnapshotParameterPolicy> parameterPolicies;
    std::vector<ProjectSnapshot> snapshots;

    bool operator==(const ProjectSnapshotBank&) const = default;
};

struct ProjectSnapshotLibraryState
{
    std::uint32_t schemaVersion = 1;
    std::vector<ProjectSnapshotBank> banks;

    bool operator==(const ProjectSnapshotLibraryState&) const = default;
};

enum class SnapshotDiagnosticCode
{
    MissingNode,
    MissingDefinition,
    MissingSoundInstance,
    MissingParameter,
    AliasAvailable,
    KindChanged,
    DomainChanged,
    InvalidChoice,
    IneligibleParameter,
    ScopeMismatch,
    DuplicateResolvedTarget,
    MissingRandomRange,
    PreparedTargetUnavailable,
    StalePreparedGeneration,
    TopologyMismatch,
    StageFailed,
    CapacityExceeded
};

enum class SnapshotDiagnosticSeverity
{
    Warning,
    Error
};

struct SnapshotDiagnostic
{
    SnapshotDiagnosticCode code = SnapshotDiagnosticCode::MissingParameter;
    SnapshotDiagnosticSeverity severity =
        SnapshotDiagnosticSeverity::Warning;
    std::string path;
    std::string targetIdentity;
    std::string message;

    bool operator==(const SnapshotDiagnostic&) const = default;
};

inline constexpr std::size_t maximumSnapshotDiagnosticCount = 256;
inline constexpr std::size_t maximumResolvedSnapshotTargetCount = 8192;

[[nodiscard]] std::string snapshotTargetIdentityString(
    const SnapshotTargetIdentity& target);

[[nodiscard]] bool isCanonicalSnapshotTargetIdentityString(
    std::string_view identity) noexcept;

[[nodiscard]] std::string qualifiedSnapshotSoundParameterIdentity(
    const std::vector<std::string>& instancePath,
    std::string_view definitionId,
    std::string_view parameterId);

[[nodiscard]] bool snapshotTargetBelongsToScope(
    const SnapshotScopeIdentity& scope,
    const SnapshotTargetIdentity& target);

[[nodiscard]] std::vector<SnapshotDiagnostic>
snapshotLibraryDiagnostics(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

// Save as... on a Sound file or a Timeline: the
// surface's obsolete layouts and presets, which are kept in case their
// controls are revived: the widgets kept for controls the surface does not
// have, the layouts' places for them, and the presets' values and freezes of
// Sounds, parameters and Event Values the project no longer has.
struct ObsoleteVirtualControlMaterial
{
    std::size_t widgets = 0;
    std::size_t layoutEntries = 0;
    std::size_t presetValues = 0;

    [[nodiscard]] bool empty() const noexcept
    {
        return widgets == 0 && layoutEntries == 0 && presetValues == 0;
    }
    bool operator==(const ObsoleteVirtualControlMaterial&) const = default;
};

[[nodiscard]] ObsoleteVirtualControlMaterial obsoleteVirtualControlMaterial(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry);
// Removes it, a layout left without a control going with it, the rest of
// the presets and layouts kept; answers what it removed.
ObsoleteVirtualControlMaterial removeObsoleteVirtualControlMaterial(
    ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

struct SnapshotOperationResult
{
    std::vector<SnapshotValueEntry> values;
    std::vector<SnapshotDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const
    {
        return diagnostics.empty();
    }
};

struct SnapshotScopeTarget
{
    SnapshotTargetIdentity target;
    SnapshotValueKind valueKind = SnapshotValueKind::Number;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::vector<std::string> choices;
    std::optional<SnapshotValue> defaultValue;
    std::string defaultUnavailableReason;
    std::optional<SnapshotParameterRange> initialRandomRange;

    bool operator==(const SnapshotScopeTarget&) const = default;
};

struct SnapshotScopeResolutionResult
{
    std::vector<SnapshotScopeTarget> targets;
    std::vector<SnapshotDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const
    {
        return diagnostics.empty();
    }
};

[[nodiscard]] SnapshotScopeResolutionResult resolveSnapshotScopeTargets(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const SnapshotScopeIdentity& scope,
    talas::expr::EvaluationContext evaluationContext = {});

[[nodiscard]] SnapshotScopeResolutionResult resolveAllSnapshotTargets(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    talas::expr::EvaluationContext evaluationContext = {});

[[nodiscard]] SnapshotOperationResult initializeSnapshotScopeValues(
    const std::vector<SnapshotScopeTarget>& targets);

[[nodiscard]] SnapshotOperationResult captureSnapshotScopeValues(
    const std::vector<SnapshotScopeTarget>& targets,
    const std::vector<SnapshotValueEntry>& currentValues);

struct SnapshotRandomizationTarget
{
    SnapshotValueEntry current;
    bool locked = false;
    std::optional<SnapshotParameterRange> range;
    std::vector<std::string> choices;

    bool operator==(const SnapshotRandomizationTarget&) const = default;
};

[[nodiscard]] SnapshotOperationResult morphSnapshotValues(
    const ProjectSnapshot& left,
    const ProjectSnapshot& right,
    const std::vector<SnapshotValueEntry>& current,
    const std::vector<SnapshotParameterPolicy>& policies,
    double amount);

[[nodiscard]] SnapshotOperationResult randomizeSnapshotValues(
    const std::vector<SnapshotRandomizationTarget>& targets,
    std::uint64_t seed);

[[nodiscard]] SnapshotOperationResult randomMergeSnapshotValues(
    const std::vector<SnapshotRandomizationTarget>& targets,
    std::uint64_t seed,
    double amount);
}
