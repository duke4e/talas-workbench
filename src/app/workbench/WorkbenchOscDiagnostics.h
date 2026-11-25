#pragma once

#include "app/workbench/WorkbenchDiagnosticsPanel.h"
#include "app/workbench/WorkbenchOscIngress.h"
#include "app/workbench/WorkbenchOscEgress.h"
#include "talas/project/ProjectDocument.h"

#include <span>
#include <vector>

namespace talas::app::workbench
{
void appendOscIngressDiagnostics(
    std::vector<WorkbenchStatusDiagnostic>& diagnostics,
    const talas::project::ProjectOscIngressSettings& settings,
    const WorkbenchOscIngressTelemetry& telemetry,
    std::span<const WorkbenchOscMonitorRecord> records);
void appendOscEgressDiagnostics(
    std::vector<WorkbenchStatusDiagnostic>& diagnostics,
    const talas::project::ProjectOscEgressSettings& settings,
    const WorkbenchOscEgressTelemetry& telemetry,
    std::span<const WorkbenchOscEgressMonitorRecord> records);
}
