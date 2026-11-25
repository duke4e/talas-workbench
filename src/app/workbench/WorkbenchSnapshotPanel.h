#pragma once

#include "talas/app/SnapshotWorkbenchModel.h"
#include "talas/app/SoundWorkbenchModel.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/WorkspaceSessionState.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace talas::app::workbench
{
class PerformancePanel : public juce::Component
{
public:
    using DocumentChangeCallback = std::function<void(bool)>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using StructureFocusCallback =
        std::function<void(const talas::app::SoundPerformanceControlIdentity&)>;
    using CanvasSelectionCallback = std::function<void(
        std::optional<std::string>,
        std::optional<std::string>)>;
    using SessionChangeCallback = std::function<void()>;
    using MidiSourceIdentifiersCallback =
        std::function<std::vector<std::string>()>;
    using OscAvailabilityCallback = std::function<bool()>;

    ~PerformancePanel() override = default;

    virtual void setSelectedInstanceId(std::string instanceId) = 0;
    virtual void setSelectionContext(
        std::string selectedInstanceId,
        talas::app::SnapshotWorkbenchCaptureContext captureContext) = 0;
    virtual void setSelectedPerformanceItem(
        std::optional<std::string> controlId,
        std::optional<std::string> meterId) = 0;
    virtual void refresh() = 0;
    virtual void refreshRuntimeState() = 0;
    virtual void synchronizeLiveToolSessionState() = 0;
    [[nodiscard]] virtual int controlCount() const noexcept = 0;
    [[nodiscard]] virtual bool controlsHaveAccessibleEditors() const = 0;
    [[nodiscard]] virtual bool
    selectedPerformanceItemEditorAccessibleForSmoke() const = 0;
    [[nodiscard]] virtual bool
    addFirstSoundControlToCanvasForSmoke() = 0;
    [[nodiscard]] virtual bool focusFirstControlInStructureForSmoke() = 0;
    [[nodiscard]] virtual bool snapshotWorkflowIsAccessible() const = 0;
    virtual void showSnapshotsForSmoke() = 0;
    [[nodiscard]] virtual bool snapshotEmptyStateIsUsableForSmoke() const = 0;
    [[nodiscard]] virtual bool snapshotOffersDefinitionScopeForSmoke() const = 0;
    [[nodiscard]] virtual bool snapshotDefinitionScopeIsSelectableForSmoke() = 0;
    [[nodiscard]] virtual bool snapshotOffersExactNestedNodeScopeForSmoke() const = 0;
    [[nodiscard]] virtual std::optional<std::string>
    runSnapshotWorkflowForSmoke() = 0;
    [[nodiscard]] virtual bool snapshotPopulatedStateIsUsableForSmoke() const = 0;
    // Loads and starts a Live Grid box, as a click in the grid here does.
    [[nodiscard]] virtual bool activateLiveGridCell(std::string_view liveSetId, std::string_view cellId) = 0;
    virtual void showLiveGridForSmoke() = 0;
    [[nodiscard]] virtual bool liveGridWorkflowIsAccessibleForSmoke() const = 0;
    [[nodiscard]] virtual bool activateFirstLiveGridCellForSmoke() = 0;
    [[nodiscard]] virtual bool
    activateSelectedLiveGridCellWithNativeKeyForSmoke() = 0;
    [[nodiscard]] virtual bool
    navigateToSparseLiveGridCellWithNativeKeyForSmoke() = 0;
    [[nodiscard]] virtual bool liveGridActiveCellIsVisibleForSmoke() const = 0;
    [[nodiscard]] virtual bool
    liveGridActiveCellSurvivesResizeForSmoke() = 0;
    [[nodiscard]] virtual bool liveToolWorkflowIsAccessibleForSmoke() const = 0;
    [[nodiscard]] virtual bool triggerSelectedLiveToolForSmoke() = 0;
    [[nodiscard]] virtual bool
    applyRejectedSelectedLiveToolVariableForSmoke() = 0;
    [[nodiscard]] virtual bool applySelectedLiveToolVariableForSmoke() = 0;
    [[nodiscard]] virtual bool liveToolDiagnosticContainsForSmoke(
        std::string_view text) const = 0;
    [[nodiscard]] virtual bool liveToolStatePersistedForSmoke(
        std::string_view expectedStateId,
        bool expectedBoolean) const = 0;
    virtual void refreshLiveToolRestoreInterleavingForSmoke(
        std::function<void()> afterInitialRuntimeRead) = 0;
};

[[nodiscard]] std::unique_ptr<PerformancePanel> makePerformancePanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    PerformancePanel::DocumentChangeCallback onDocumentChange,
    PerformancePanel::StatusCallback onStatus,
    PerformancePanel::StructureFocusCallback onStructureFocus,
    PerformancePanel::CanvasSelectionCallback onCanvasSelection,
    talas::project::WorkspacePerformanceSessionState& session,
    PerformancePanel::SessionChangeCallback onSessionChange,
    PerformancePanel::MidiSourceIdentifiersCallback midiSourceIdentifiers,
    PerformancePanel::OscAvailabilityCallback oscAvailable);
} // namespace talas::app::workbench
