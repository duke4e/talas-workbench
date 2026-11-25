#pragma once

#include "talas/app/GraphLineContour.h"
#include "talas/app/WorkbenchUiPolicy.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/WorkspaceSessionState.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app::workbench
{
enum class GraphKeyboardCommand
{
    SelectAll,
    DuplicateSelection,
    DeleteSelection,
    FocusPrototypeSearch,
    CutSelection,
    CopySelection,
    Paste,
    // A new name for the selected Sound.
    RenameSelection,
    // Edit > Clean up of the signal flow.
    CleanUpLayout,
    // A help window on the selected Sound's class.
    DescribeSelection,
    // Get info on the selected Sound itself.
    InfoOnSelection,
    // The selected Sound as the edited
    // Sound's replaceable input.
    SetReplaceableInput,
};

struct GraphKeyboardCommandRequest
{
    std::optional<std::string> definitionId;
    GraphKeyboardCommand command = GraphKeyboardCommand::SelectAll;
    std::vector<std::string> selectedNodeIds;
    std::string expectedDocumentSnapshot;
    // Where a Paste chosen from the canvas's context menu puts the Sounds.
    std::optional<talas::project::CanvasPosition> pastePosition;
};

// What a prototype dropped on the canvas lands on: a line inserts it, a
// Sound is replaced by it, and empty canvas adds it.
// What is dropped: a prototype, or an instance of a project Sound
// definition dragged from the Sound browser.
struct GraphDropItem
{
    std::string prototypeTypeId;
    std::string soundDefinitionId;
    // A Sound or a file dragged from a Sound Browser, brought into the
    // project's Sounds as it is dropped.
    std::string browserDrag;

    bool operator==(const GraphDropItem&) const = default;
};

struct GraphDropTarget
{
    std::optional<talas::core::Connection> connection;
    std::string nodeId;
    // The input of a Sound dropped on a line that takes the line, once
    // chosen.
    std::optional<std::string> inputPortId;
    // For a Sound dropped on a Sound with inputs: whether its inputs go
    // too (Replace all), once chosen.
    std::optional<bool> replaceAll;
    // Dropped on the gray circle with a plus near the line's end: mixed with
    // the line's source, not put between the Sounds it joins.
    bool mix = false;
    // Dropped with Control or Option held: a Sound of this signal flow dropped on
    // another replaces it itself, not a copy.
    bool shared = false;

    bool operator==(const GraphDropTarget&) const = default;
};

class GraphCanvas : public juce::Component
{
public:
    using SelectionCallback = std::function<void(
        std::optional<std::string>,
        std::string,
        bool)>;
    using DocumentChangeCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using PrototypeDropCallback = std::function<void(
        std::optional<std::string>,
        GraphDropItem,
        talas::project::CanvasPosition,
        GraphDropTarget)>;
    using OpenChildCallback = std::function<void(
        std::optional<std::string>,
        std::string)>;
    using ViewChangeCallback = std::function<void(
        std::optional<std::string>,
        double,
        double,
        double)>;
    using KeyboardCommandCallback = std::function<void(
        GraphKeyboardCommandRequest)>;

    ~GraphCanvas() override = default;

    virtual void setCompactGraphProtection(bool enabled) = 0;
    virtual void setGraphViewMode(
        talas::project::WorkspaceGraphViewMode mode) = 0;
    // The lines' contour, as the context menu or Edit > View... sets it
    //.
    virtual void setLineContour(talas::app::GraphLineContour contour) = 0;
    // Large or small icons: the signal flow at the scale.
    virtual void setIconScale(float scale) = 0;
    [[nodiscard]] virtual talas::app::GraphLineContour lineContour() const = 0;
    virtual void setContextPresentation(
        juce::String context,
        bool active) = 0;
    [[nodiscard]] virtual float viewScaleForSmoke() const noexcept = 0;
    // The scroll bar at the bottom: the flow's span
    // and the view's, none while every Sound is in view; scrolling puts the
    // view's left edge at a place; the owner is told of a change.
    [[nodiscard]] virtual std::optional<talas::app::WorkbenchGraphScroll> horizontalScroll() const { return std::nullopt; }
    virtual void scrollHorizontallyTo(double) {}
    virtual void setHorizontalScrollChanged(std::function<void()>) {}
    [[nodiscard]] virtual bool graphIsFullyVisibleForSmoke() const = 0;
    [[nodiscard]] virtual std::size_t totalNodeCountForSmoke() const noexcept = 0;
    [[nodiscard]] virtual std::size_t visibleNodeCountForSmoke() const noexcept = 0;
    [[nodiscard]] virtual std::size_t accessibilityItemCountForSmoke() const noexcept = 0;
    [[nodiscard]] virtual std::size_t graphViewportMembershipCountForSmoke() const noexcept = 0;
    [[nodiscard]] virtual std::size_t graphViewportScratchCapacityForSmoke() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t structuralGraphRebuildCountForSmoke() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t accessibilityGraphRebuildCountForSmoke() const noexcept = 0;
    virtual void rebuildStructuralGraphCachesForSmoke() = 0;
    [[nodiscard]] virtual bool contextIsActiveForSmoke() const noexcept = 0;
    [[nodiscard]] virtual talas::project::WorkspaceGraphViewMode
    graphViewModeForSmoke() const noexcept = 0;
    [[nodiscard]] virtual bool portsVisibleForSmoke() const noexcept = 0;
    [[nodiscard]] virtual bool wiresVisibleForSmoke() const noexcept = 0;
    [[nodiscard]] virtual bool hasPortAccessibilityForSmoke() const = 0;
    [[nodiscard]] virtual bool semanticPortHintContainsForSmoke(
        std::string_view nodeId,
        std::string_view portId,
        bool output,
        std::string_view expected) const = 0;
    [[nodiscard]] virtual bool selectedWireEmphasisForSmoke(
        std::string_view fromNode,
        std::string_view toNode) const = 0;
    [[nodiscard]] virtual bool adapterSuggestionContainsForSmoke(
        std::string_view sourceTypeId,
        std::string_view sourcePortId,
        std::string_view destinationTypeId,
        std::string_view destinationPortId,
        std::string_view expectedAdapterTypeId) const = 0;
    [[nodiscard]] virtual bool hasCompileDiagnostic(
        std::string_view nodeId) const = 0;
    [[nodiscard]] virtual bool hasNodeForSmoke(
        std::string_view nodeId) const = 0;
    // A double-click on a Sound, its parameters
    // opened and its icon patterned; one on an empty area.
    [[nodiscard]] virtual bool doubleClickNodeForSmoke(std::string_view nodeId) = 0;
    virtual void doubleClickEmptyForSmoke() = 0;
    [[nodiscard]] virtual bool interactiveNodeGeometryMatchesForSmoke(
        std::string_view nodeId,
        talas::project::CanvasPosition position) = 0;
    [[nodiscard]] virtual bool preservedStatusContainsForSmoke(
        std::string_view nodeId,
        std::string_view status) const = 0;
    [[nodiscard]] virtual bool preservedNodeIsAccessibleForSmoke(
        std::string_view nodeId,
        std::string_view status) const = 0;
    virtual void zoomToFit(bool announce = true) = 0;
    // Zoom in or out a step about the centre (Ctrl+]
    // and Ctrl+[).
    virtual void zoomStep(bool in) = 0;
    virtual void setSessionViewState(
        double panX,
        double panY,
        double zoom) = 0;
    virtual void setSelectedNodeId(std::string nodeId) = 0;
    virtual void setSelectedNodeIds(
        const std::vector<std::string>& nodeIds,
        std::string primaryNodeId) = 0;
    virtual void refreshModel() = 0;
    virtual void focusGraph() = 0;
    [[nodiscard]] virtual bool pressNodeKeyForSmoke(
        std::string_view nodeId,
        const juce::KeyPress& key) = 0;
    [[nodiscard]] virtual bool queueNodeKeyForSmoke(
        std::string_view nodeId,
        const juce::KeyPress& key) = 0;
    virtual void drainKeyboardCommandsForSmoke() = 0;
    [[nodiscard]] virtual bool pressPortKeyForSmoke(
        std::string_view nodeId,
        std::string_view portId,
        bool output,
        const juce::KeyPress& key) = 0;
    [[nodiscard]] virtual bool pendingKeyboardConnectionForSmoke() const = 0;
    virtual void finishKeyboardNudgeForSmoke() = 0;
    // The PNG pictures of the Sound classes whose icon is one, by Sound
    // class; an invalid image for none.
    virtual void setClassIconImages(std::function<juce::Image(const std::string&)> images) = 0;
    // The Sound drawn with the replaceable input's cyan halo, if any.
    [[nodiscard]] virtual std::string replaceableInputNodeIdForSmoke() const = 0;
    // The number a line carries: its use count when it is the labelled one
    // of several (or the only one), 0 when another of them carries it.
    [[nodiscard]] virtual std::optional<std::size_t> connectionUseCountForSmoke(
        const talas::core::Connection& connection) const = 0;
    // Smoke access to the drop and context-menu paths a mouse would take.
    [[nodiscard]] virtual std::optional<juce::Point<int>>
    connectionMidpointForSmoke(
        const talas::core::Connection& connection) const = 0;
    // The gray circle with a plus on a line.
    [[nodiscard]] virtual std::optional<juce::Point<int>> connectionPlusForSmoke(const talas::core::Connection&) const
    {
        return std::nullopt;
    }
    // The point a fraction t of the way along a line.
    [[nodiscard]] virtual std::optional<juce::Point<int>>
    connectionPointForSmoke(
        const talas::core::Connection& connection,
        float t) const = 0;
    [[nodiscard]] virtual std::optional<juce::Point<int>> nodeCentreForSmoke(
        std::string_view nodeId) const = 0;
    [[nodiscard]] virtual GraphDropTarget dropTargetForSmoke(
        juce::Point<int> canvasPosition) = 0;
    [[nodiscard]] virtual bool dropPrototypeForSmoke(
        std::string_view typeId,
        juce::Point<int> canvasPosition) = 0;
    // What a Sound dragged out of the signal flow carries.
    [[nodiscard]] virtual std::string dragDescriptionForSmoke(std::string_view nodeId) const = 0;
    // A drag description, as a drag source gives it, dropped at a point.
    [[nodiscard]] virtual bool dropDescriptionForSmoke(
        const juce::String& description,
        juce::Point<int> canvasPosition,
        bool shared = false) = 0;
    // A click on the dot (or ellipsis) where a
    // Sound's lines meet it, found at its place on the canvas; `fully` as a
    // double-click.
    [[nodiscard]] virtual bool clickBranchMarkerForSmoke(
        std::string_view nodeId,
        bool fully) = 0;
    [[nodiscard]] virtual std::vector<std::string> shownNodeIdsForSmoke()
        const = 0;
    [[nodiscard]] virtual juce::PopupMenu contextMenuForSmoke(
        juce::Point<int> canvasPosition) = 0;
    [[nodiscard]] virtual bool chooseContextMenuItemForSmoke(
        juce::Point<int> canvasPosition,
        int itemId) = 0;
    virtual void setSoundDefinitionContext(
        std::optional<std::string> definitionId,
        std::string selectedNodeId = {}) = 0;
    virtual void setCompileDiagnostics(
        std::span<const talas::runtime::Diagnostic> diagnostics,
        const talas::project::ProjectDocument& sourceDocument) = 0;
};

[[nodiscard]] std::unique_ptr<GraphCanvas> makeGraphCanvas(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    GraphCanvas::SelectionCallback onSelection,
    GraphCanvas::DocumentChangeCallback onDocumentChange,
    GraphCanvas::StatusCallback onStatus,
    GraphCanvas::PrototypeDropCallback onPrototypeDrop,
    GraphCanvas::OpenChildCallback onOpenChild,
    GraphCanvas::ViewChangeCallback onViewChange,
    GraphCanvas::KeyboardCommandCallback onKeyboardCommand);
}
