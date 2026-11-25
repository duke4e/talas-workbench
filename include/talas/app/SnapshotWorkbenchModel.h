#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectSnapshots.h"
#include "talas/runtime/SnapshotPerformance.h"

#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
struct SnapshotWorkbenchCaptureContext
{
    std::vector<std::string> instancePath;
    std::optional<std::string> containingDefinitionId;
    std::string selectedNodeId;
    bool exact = true;

    bool operator==(const SnapshotWorkbenchCaptureContext&) const = default;
};

struct SnapshotWorkbenchCaptureScopeOption
{
    talas::project::SnapshotScopeIdentity scope;
    std::string displayName;
    std::string scopeBreadcrumb;
    std::string accessibleDescription;

    bool operator==(const SnapshotWorkbenchCaptureScopeOption&) const = default;
};

struct SnapshotWorkbenchBankOption
{
    std::string bankId;
    std::string displayName;
    std::string scopeBreadcrumb;

    bool operator==(const SnapshotWorkbenchBankOption&) const = default;
};

struct SnapshotWorkbenchSnapshotRow
{
    std::string snapshotId;
    std::string displayName;
    std::string stateLabel;
    std::string diagnostic;
    std::string accessibleDescription;
    bool active = false;
    bool morphLeft = false;
    bool morphRight = false;
    bool recallEnabled = false;

    bool operator==(const SnapshotWorkbenchSnapshotRow&) const = default;
};

struct SnapshotWorkbenchParameterRow
{
    talas::project::SnapshotTargetIdentity target;
    std::string displayName;
    std::string identityPath;
    std::string currentValue;
    std::string kindAndDomain;
    std::string range;
    std::string diagnostic;
    std::string accessibleDescription;
    bool locked = false;
    bool numeric = false;
    bool rangeAvailable = false;

    bool operator==(const SnapshotWorkbenchParameterRow&) const = default;
};

struct SnapshotWorkbenchProjection
{
    std::string captureScopeBreadcrumb;
    std::string emptyTitle;
    std::string emptyDescription;
    std::string selectedBankId;
    std::string selectedSnapshotId;
    std::string status;
    std::vector<SnapshotWorkbenchCaptureScopeOption> captureScopes;
    std::vector<SnapshotWorkbenchBankOption> banks;
    std::vector<SnapshotWorkbenchSnapshotRow> snapshots;
    std::vector<SnapshotWorkbenchParameterRow> parameters;
    double smoothingSeconds = 0.0;
    double morphPosition = 0.0;
    std::uint64_t randomSeed = 0;
    bool transitionActive = false;
    bool transitionCoalesced = false;
    bool canRecall = false;
    bool canStore = false;
    bool canMorph = false;
    bool canRandomize = false;
    bool canRandomMerge = false;

    bool operator==(const SnapshotWorkbenchProjection&) const = default;
};

[[nodiscard]] std::vector<SnapshotWorkbenchCaptureScopeOption>
snapshotWorkbenchCaptureScopes(
    const talas::project::ProjectDocument& document,
    std::string_view selectedNodeId);

[[nodiscard]] SnapshotWorkbenchCaptureContext
snapshotWorkbenchCaptureContext(
    const talas::project::ProjectDocument& document,
    std::string_view selectedNodeId,
    std::optional<std::string_view> containingDefinitionId,
    std::string_view preferredRootInstanceId);

[[nodiscard]] std::vector<SnapshotWorkbenchCaptureScopeOption>
snapshotWorkbenchCaptureScopes(
    const talas::project::ProjectDocument& document,
    const SnapshotWorkbenchCaptureContext& context);

[[nodiscard]] SnapshotWorkbenchProjection snapshotWorkbenchProjection(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const talas::runtime::SnapshotPerformanceState& performanceState,
    std::string_view selectedInstanceId,
    std::string_view selectedBankId,
    std::string_view selectedSnapshotId,
    std::span<const talas::project::SnapshotDiagnostic> diagnostics = {},
    bool transitionActive = false,
    bool transitionCoalesced = false,
    bool performanceDocumentCurrent = true,
    const SnapshotWorkbenchCaptureContext* captureContext = nullptr);
}
