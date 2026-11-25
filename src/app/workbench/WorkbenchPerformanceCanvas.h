#pragma once

#include "talas/app/PerformanceRuntimeDispatcher.h"
#include "talas/app/WorkbenchSurfaceState.h"
#include "talas/project/WorkspaceSessionState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace talas::app::workbench
{
class WorkbenchPerformanceCanvas : public juce::Component,
                                   public talas::app::WorkbenchSurfaceStateSource
{
public:
    using ProjectAuthoringCallback = std::function<void(bool)>;
    using SessionAuthoringCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using SelectionCallback = std::function<void(
        std::optional<std::string>,
        std::optional<std::string>)>;
    using AcceptedValueCallback =
        std::function<void(std::string_view, double)>;

    ~WorkbenchPerformanceCanvas() override = default;

    virtual void refresh() = 0;
    virtual void refreshRuntimeState() = 0;
    virtual void noteActivity(
        const talas::project::SnapshotTargetIdentity& target,
        talas::app::PerformanceActivitySource source) = 0;
    [[nodiscard]] virtual std::size_t visibleItemCountForSmoke()
        const noexcept = 0;
    [[nodiscard]] virtual bool controlsAccessibleForSmoke() const = 0;
    [[nodiscard]] virtual talas::app::PerformanceActivitySource
    firstControlActivityForSmoke() const noexcept = 0;
    [[nodiscard]] virtual bool setFirstControlValueForSmoke(double value) = 0;
    [[nodiscard]] virtual bool addOutputMeterForSmoke() = 0;
    [[nodiscard]] virtual bool moveFirstControlForSmoke(
        talas::project::CanvasPosition position) = 0;
    [[nodiscard]] virtual bool setCompactForSmoke(bool compact) = 0;
    [[nodiscard]] virtual bool scaledLayoutCoherentForSmoke() const = 0;
};

[[nodiscard]] std::unique_ptr<WorkbenchPerformanceCanvas>
makeWorkbenchPerformanceCanvas(
    talas::runtime::ProjectRuntimeBridge& bridge,
    talas::app::PerformanceRuntimeDispatcher& dispatcher,
    talas::project::WorkspacePerformanceSessionState& session,
    std::span<std::atomic<float>> outputPeaks,
    WorkbenchPerformanceCanvas::ProjectAuthoringCallback onProjectAuthoring,
    WorkbenchPerformanceCanvas::SessionAuthoringCallback onSessionAuthoring,
    WorkbenchPerformanceCanvas::StatusCallback onStatus,
    WorkbenchPerformanceCanvas::SelectionCallback onSelection,
    WorkbenchPerformanceCanvas::AcceptedValueCallback onAcceptedValue);
}
