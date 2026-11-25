#pragma once

#include "talas/expr/Expression.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectSnapshots.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::core
{
class NodeRegistry;
}

namespace talas::runtime
{
class PreparedRuntimeGraph;

inline constexpr std::size_t maximumSnapshotDirectEventValueUpdates =
    talas::project::maximumSnapshotValuesPerSnapshot;

struct SnapshotDirectEventValueUpdate
{
    std::uint32_t targetIndex = 0;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;

    bool operator==(const SnapshotDirectEventValueUpdate&) const = default;
};

struct SnapshotDirectVariableUpdate
{
    std::uint32_t targetIndex = 0;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;

    bool operator==(const SnapshotDirectVariableUpdate&) const = default;
};

enum class SnapshotDirectTransactionResult : std::uint8_t
{
    Accepted,
    NotReady,
    InvalidTargetOrPayload,
    CapacityExceeded,
    QueueFull,
    StaleStageOrGeneration
};

struct SnapshotDirectTransaction
{
    std::uint64_t stageId = 0;
    std::uint64_t generation = 0;
    std::size_t frameOffset = 0;
    std::uint64_t rampFrames = 0;
    std::span<const SnapshotDirectEventValueUpdate> updates;
    std::span<const SnapshotDirectVariableUpdate> variableUpdates;
};

struct SnapshotExecutionTopologyFingerprint
{
    std::string canonicalProjection;

    bool operator==(
        const SnapshotExecutionTopologyFingerprint&) const = default;
};

enum class SnapshotPerformanceActionOutcome : std::uint8_t
{
    Rejected,
    GesturePrepared,
    AppliedDirect,
    StagedOverlay,
    CoalescedOverlay,
    Reset,
    AdoptedAuthoringEdit,
    CapturedProjectEdit
};

struct SnapshotPerformanceActionResult
{
    SnapshotPerformanceActionOutcome outcome =
        SnapshotPerformanceActionOutcome::Rejected;
    std::vector<talas::project::SnapshotDiagnostic> diagnostics;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return outcome
            != SnapshotPerformanceActionOutcome::Rejected;
    }
};

struct SnapshotPerformanceState
{
    std::vector<talas::project::SnapshotValueEntry> currentValues;
    std::string activeBankId;
    std::string activeSnapshotId;
    std::string morphLeftSnapshotId;
    std::string morphRightSnapshotId;
    std::vector<talas::project::SnapshotValueEntry>
        morphGestureStartValues;
    double morphPosition = 0.0;
    std::uint64_t lastRandomSeed = 0;
    std::uint64_t projectRevision = 0;
    bool morphActive = false;

    bool operator==(const SnapshotPerformanceState&) const = default;
};

[[nodiscard]] SnapshotExecutionTopologyFingerprint
snapshotExecutionTopologyFingerprint(
    const talas::project::ProjectDocument& document);
[[nodiscard]] SnapshotExecutionTopologyFingerprint
snapshotExecutionTopologyFingerprint(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

[[nodiscard]] SnapshotExecutionTopologyFingerprint
snapshotPreparedExecutionTopologyFingerprint(
    const PreparedRuntimeGraph& graph);

[[nodiscard]] std::uint64_t snapshotSmoothingFrames(
    double smoothingSeconds,
    double sampleRate) noexcept;
}
