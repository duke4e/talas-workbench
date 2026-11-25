#pragma once

#include "app/workbench/WorkbenchDiagnosticsPanel.h"
#include "app/workbench/WorkbenchOscEgress.h"
#include "app/workbench/WorkbenchOscIngress.h"
#include "talas/app/AnalysisDiagnosticsModel.h"
#include "talas/app/AssetRecoveryWorkbenchModel.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/RealtimeMetrics.h"

#include <cstdint>
#include <span>
#include <vector>

namespace talas::app::workbench
{
[[nodiscard]] talas::app::AnalysisDiagnosticsSnapshot
makeWorkbenchAnalysisDiagnosticsSnapshot(
    const talas::runtime::PreparedGraphStatusSnapshot& prepared,
    const talas::runtime::RealtimeMetricsSnapshotV1& metrics,
    const talas::runtime::PreparedNodeTimingSnapshot& nodeTiming,
    std::uint64_t latestDeviceCallbackNanoseconds,
    const talas::app::AssetRecoveryWorkbenchState& assets,
    const talas::app::AnalysisMonitorSourceInput& eventMonitor,
    std::span<const talas::app::AnalysisMonitorRecordInput> midiRecords,
    std::uint64_t nextMidiSequence,
    std::uint64_t midiDroppedRecords,
    const WorkbenchOscIngressTelemetry& oscIngress,
    std::span<const WorkbenchOscMonitorRecord> oscIngressRecords,
    std::uint64_t oscIngressRouteGeneration,
    const WorkbenchOscEgressTelemetry& oscEgress,
    std::span<const WorkbenchOscEgressMonitorRecord> oscEgressRecords,
    std::uint64_t oscEgressRouteGeneration);

void appendWorkbenchAnalysisDiagnostics(
    std::vector<WorkbenchStatusDiagnostic>& destination,
    const talas::app::AnalysisDiagnosticsProjection& projection);
}
