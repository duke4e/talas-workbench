#pragma once

#include "talas/app/AnalysisDiagnosticsModel.h"
#include "talas/app/SoundWorkbenchModel.h"
#include "talas/app/WorkbenchSurfaceState.h"
#include "talas/runtime/Runtime.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace talas::app::workbench
{
enum class WorkbenchStatusDiagnosticSeverity
{
    Info,
    Warning,
    Error,
};

struct WorkbenchStatusDiagnostic
{
    juce::String title;
    juce::String detail;
    talas::app::SoundWorkbenchDiagnosticIdentity identity;
    WorkbenchStatusDiagnosticSeverity severity =
        WorkbenchStatusDiagnosticSeverity::Info;
    talas::app::AnalysisDiagnosticIdentity navigation;
};

class DiagnosticsPanel : public juce::Component,
                         public talas::app::WorkbenchSurfaceStateSource
{
public:
    using ActivateCallback = std::function<void(
        const WorkbenchStatusDiagnostic&)>;

    ~DiagnosticsPanel() override = default;

    virtual void setCompileDiagnostics(
        std::span<const talas::runtime::Diagnostic> diagnostics) = 0;
    virtual void setRuntimeDiagnostics(
        std::vector<WorkbenchStatusDiagnostic> diagnostics) = 0;
    [[nodiscard]] virtual bool
    hasKeyboardAccessiblePreparedStateForSmoke() const = 0;
    [[nodiscard]] virtual bool runtimeDiagnosticContainsForSmoke(
        juce::StringRef titlePrefix,
        juce::StringRef detailText,
        talas::app::AnalysisNavigationKind navigationKind,
        std::uint64_t stageId) const = 0;
    [[nodiscard]] virtual bool activateFirstTargetForSmoke() = 0;
};

[[nodiscard]] std::unique_ptr<DiagnosticsPanel>
makeDiagnosticsPanel(DiagnosticsPanel::ActivateCallback onActivate);
}
