#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/WorkspaceSessionState.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
enum class AnalysisTraceAvailability
{
    Available,
    MissingInstance,
    MissingDefinition,
    MissingNode,
    UnsupportedNodeType,
    MissingPort,
    IncompatiblePortKind,
};

enum class AnalysisTraceRuntimeState
{
    TargetUnavailable,
    CaptureUnavailable,
    PreparedProbeUnavailable,
    WaitingForCurrentStage,
    Live,
    RetainedNoNewData,
    RejectedEvidence,
};

struct AnalysisTraceRuntimeEvidence
{
    AnalysisTraceAvailability targetAvailability =
        AnalysisTraceAvailability::MissingNode;
    bool captureReady = false;
    bool probeBound = false;
    bool bindingReady = false;
    bool acceptedRecord = false;
    bool retainedLastGood = false;
    bool rejectedEvidence = false;
};

struct AnalysisTraceViewModel
{
    talas::project::WorkspaceAnalysisTraceState trace;
    AnalysisTraceAvailability availability =
        AnalysisTraceAvailability::MissingNode;
    std::string targetIdentity;
    std::string scopeLabel;

    [[nodiscard]] bool available() const noexcept
    {
        return availability == AnalysisTraceAvailability::Available;
    }

    bool operator==(const AnalysisTraceViewModel&) const = default;
};

struct AnalysisWorkbenchProjection
{
    std::vector<AnalysisTraceViewModel> traces;
    std::optional<std::size_t> selectedTraceIndex;
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty();
    }
};

[[nodiscard]] std::string analysisTraceTargetIdentityString(
    const talas::project::WorkspaceAnalysisTraceState& trace);

[[nodiscard]] AnalysisTraceAvailability analysisTraceAvailability(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const talas::project::WorkspaceAnalysisTraceState& trace);

[[nodiscard]] AnalysisTraceRuntimeState analysisTraceRuntimeState(
    const AnalysisTraceRuntimeEvidence& evidence) noexcept;
[[nodiscard]] std::string_view analysisTraceKindCode(
    talas::project::WorkspaceAnalysisTraceKind kind) noexcept;

[[nodiscard]] AnalysisWorkbenchProjection buildAnalysisWorkbenchProjection(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const talas::project::WorkspaceAnalysisSessionState& session);

[[nodiscard]] std::optional<talas::project::WorkspaceAnalysisTraceKind>
analysisTraceKindForPreparedOutput(
    const talas::core::PortDescriptor& output) noexcept;

[[nodiscard]] bool selectNextAnalysisTargetForNode(
    const talas::core::NodeTypeDescriptor& descriptor,
    const talas::project::WorkspaceAnalysisSessionState& session,
    talas::project::WorkspaceAnalysisTraceState& trace);

[[nodiscard]] bool addAnalysisTrace(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    talas::project::WorkspaceAnalysisSessionState& session,
    talas::project::WorkspaceAnalysisTraceState trace);

[[nodiscard]] bool removeAnalysisTrace(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::string_view traceId);

[[nodiscard]] bool moveAnalysisTrace(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::string_view traceId,
    std::size_t destinationIndex);

[[nodiscard]] bool setAnalysisTraceVisible(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::string_view traceId,
    bool visible);

[[nodiscard]] bool setAnalysisTraceVerticalScale(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::string_view traceId,
    double verticalScale);

[[nodiscard]] bool setAnalysisRefreshRateHz(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::uint32_t refreshRateHz);

[[nodiscard]] bool setAnalysisHistoryFrames(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::uint32_t historyFrames);

[[nodiscard]] std::size_t appendAnalysisSequentialHistory(
    std::span<double> storage,
    std::size_t currentValueCount,
    std::span<const double> values,
    std::size_t historyFrames) noexcept;

[[nodiscard]] bool selectAnalysisTrace(
    talas::project::WorkspaceAnalysisSessionState& session,
    std::optional<std::string_view> traceId);
}
