#pragma once

#include "talas/app/PerformanceWorkbenchModel.h"
#include "talas/project/VirtualControlMidi.h"
#include "talas/project/VirtualControlOsc.h"
#include "talas/core/NodeRegistry.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app::workbench
{
// Widgets copied from an unlocked surface or from a Sound in the Sound
// Browser, for pasting into an unlocked surface. The
// system clipboard is given their names, and anything else copied there
// later leaves none to paste.
void copyVirtualControlWidgets(std::vector<talas::project::VirtualControl> widgets, bool toSystemClipboard = true);
[[nodiscard]] std::vector<talas::project::VirtualControl> copiedVirtualControlWidgets(bool fromSystemClipboard = true);

class VirtualControlSurface : public juce::Component
{
public:
    struct ControlPresentation
    {
        talas::project::VirtualControlKind kind =
            talas::project::VirtualControlKind::Fader;
        double value = 0.0;
        bool expectedWidgetVisible = false;
    };

    struct LocalMapControlPresentation
    {
        std::string sourceEventName;
        double minimum = 0.0;
        double maximum = 0.0;
        double value = 0.0;
        bool enabled = false;
        std::string rangeText;
        std::string displayType;
    };

    // What a widget shows, for the smoke.
    struct WidgetPresentation
    {
        int sliderStyle = 0;
        bool numberShown = true;
        std::string label;
        double interval = 0.0;
        bool shown = true;
        std::uint32_t onColour = 0;
        std::string numberText;
    };

    using DocumentChangeCallback = std::function<void(bool)>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using FocusCallback = std::function<void(std::string_view)>;
    using MidiSourceNameCallback =
        std::function<std::optional<std::string>(std::string_view)>;
    using OscRouteAvailableCallback = std::function<bool(
        std::string_view,
        std::string_view)>;
    using OscEgressRouteAvailableCallback = std::function<bool(
        std::string_view,
        std::string_view)>;
    using OscProfileDetailCallback = std::function<juce::String()>;
    using ValueDispatchCallback = std::function<std::optional<juce::String>(
        std::string_view,
        double,
        talas::app::PerformanceActivitySource,
        talas::runtime::ControllerRecordingIngressEvidence)>;
    using ValueGestureFinishedCallback = std::function<void()>;
    using LocalMapValueDispatchCallback =
        std::function<std::optional<juce::String>(std::string_view, double)>;
    using LocalMapStageReadyCallback = std::function<bool()>;
    using AcceptedValueCallback =
        std::function<void(std::string_view, double)>;
    using OscEgressAuthorityCallback = std::function<void()>;

    ~VirtualControlSurface() override = default;

    virtual void refresh() = 0;
    // The surface follows the Sound loaded, here
    // one loaded alone in the project's place (its structure and node) or,
    // with none, the project itself.
    virtual void setLoadedSound(std::optional<talas::project::ProjectDocument> loaded, std::string sound) = 0;
    virtual bool focusEventValue(std::string_view eventName) = 0;
    // The presets, the camera that takes one
    // (replacing the selected one), the list numbered by program change,
    // the dice, a frozen control, and the keys: Enter the camera, r the
    // dice, the arrows the next preset up or down.
    virtual bool selectPresetForProgramChange(unsigned program) = 0;
    [[nodiscard]] virtual bool takePresetForSmoke(bool replaceSelected) = 0;
    [[nodiscard]] virtual std::vector<juce::String> presetLabelsForSmoke() const = 0;
    [[nodiscard]] virtual bool selectPresetForSmoke(int index) = 0;
    [[nodiscard]] virtual bool rollDiceForSmoke() = 0;
    [[nodiscard]] virtual bool toggleFrozenForSmoke(std::string_view eventName) = 0;
    [[nodiscard]] virtual bool presetKeyForSmoke(const juce::KeyPress& key) = 0;
    [[nodiscard]] virtual bool removePresetsBesidesDefaultForSmoke() = 0;
    // Unlocking the surface, selecting, moving
    // and resizing widgets, Clean up in columns or Fit, and Squeeze.
    [[nodiscard]] virtual bool setEditModeForSmoke(bool editing) = 0;
    [[nodiscard]] virtual bool lockDiscardingForSmoke() { return false; }
    [[nodiscard]] virtual std::optional<std::pair<int, int>> navigatorDragForSmoke(int, int) { return std::nullopt; }
    [[nodiscard]] virtual bool selectWidgetsForSmoke(std::vector<std::string> names) = 0;
    [[nodiscard]] virtual bool moveSelectionForSmoke(int dx, int dy) = 0;
    [[nodiscard]] virtual bool resizeWidgetForSmoke(std::string_view name, int dx, int dy) = 0;
    [[nodiscard]] virtual bool cleanUpForSmoke(unsigned columns, bool fit) = 0;
    // Edit > Clean up with the surface in front: the columns asked
    // for, locked or not.
    virtual void showCleanUpDialog() = 0;
    [[nodiscard]] virtual bool squeezeForSmoke(bool vertical) = 0;
    [[nodiscard]] virtual std::optional<juce::Rectangle<int>> widgetBoundsForSmoke(std::string_view name) const = 0;
    [[nodiscard]] virtual juce::String selectedValueForSmoke() const = 0;
    // The surface's texts and boxes:
    // Options > Add Widget, text pasted, a text edited, the selected ones
    // removed, and where they are.
    // The Selected Value list: a widget chosen from it.
    [[nodiscard]] virtual bool chooseSelectedValueForSmoke(std::string_view) { return false; }
    [[nodiscard]] virtual bool addDecorationForSmoke(talas::project::VirtualControlDecorationKind, std::string) { return false; }
    [[nodiscard]] virtual bool pasteTextForSmoke(std::string) { return false; }
    // Ctrl+A, Ctrl+C and Ctrl+V in the unlocked surface, the
    // system clipboard left alone.
    [[nodiscard]] virtual bool selectAllWidgetsForSmoke() { return false; }
    [[nodiscard]] virtual bool copyWidgetsForSmoke() { return false; }
    [[nodiscard]] virtual bool pasteWidgetsForSmoke() { return false; }
    [[nodiscard]] virtual bool editDecorationTextForSmoke(std::size_t, std::string) { return false; }
    [[nodiscard]] virtual bool removeSelectedDecorationsForSmoke() { return false; }
    [[nodiscard]] virtual std::vector<juce::Rectangle<int>> decorationBoundsForSmoke() const { return {}; }
    // The layouts, duplicated and merged, and the
    // selection aligned or distributed.
    [[nodiscard]] virtual std::vector<juce::String> layoutLabelsForSmoke() const = 0;
    [[nodiscard]] virtual bool selectLayoutForSmoke(std::size_t index) = 0;
    [[nodiscard]] virtual bool duplicateLayoutForSmoke() = 0;
    [[nodiscard]] virtual bool mergeLayoutForSmoke(std::size_t other) = 0;
    [[nodiscard]] virtual bool removeLayoutForSmoke() = 0;
    [[nodiscard]] virtual bool alignForSmoke(int alignment) = 0;
    [[nodiscard]] virtual bool distributeForSmoke(bool horizontal) = 0;
    [[nodiscard]] virtual bool focusEventValueForSmoke(
        std::string_view eventName) = 0;
    [[nodiscard]] virtual std::optional<std::string>
    focusedEventValueForSmoke() const = 0;
    [[nodiscard]] virtual bool beginMidiLearnForSmoke(
        std::string_view eventName) = 0;
    [[nodiscard]] virtual std::optional<std::string>
    midiMappingPresentationForSmoke(
        std::string_view eventName) const = 0;
    [[nodiscard]] virtual std::optional<std::string>
    oscMappingPresentationForSmoke(
        std::string_view eventName) const = 0;
    [[nodiscard]] virtual std::optional<std::string>
    setOscMappingForSmoke(
        std::string_view eventName,
        std::optional<std::string> address) = 0;
    [[nodiscard]] virtual std::optional<std::string>
    oscEgressMappingPresentationForSmoke(
        std::string_view eventName) const = 0;
    [[nodiscard]] virtual std::optional<std::string>
    setOscEgressMappingForSmoke(
        std::string_view eventName,
        std::optional<std::string> address) = 0;
    [[nodiscard]] virtual bool setValueForSmoke(
        std::string_view eventName,
        double value) = 0;
    [[nodiscard]] virtual bool startValueGestureForSmoke(
        std::string_view eventName,
        double value) = 0;
    [[nodiscard]] virtual std::optional<ControlPresentation>
    controlPresentationForSmoke(std::string_view eventName) const = 0;
    [[nodiscard]] virtual bool applyWidgetForSmoke(
        std::string_view eventName,
        const talas::project::VirtualControlWidget& widget,
        double minimum,
        double maximum) = 0;
    [[nodiscard]] virtual std::optional<WidgetPresentation>
    widgetPresentationForSmoke(std::string_view eventName) const = 0;
    // The preferences' Show All Digits.
    virtual void setShowAllDigits(bool shown) = 0;
    // Tab or Shift+Tab on the surface: the fader whose
    // number is selected after, none when no fader shows one.
    [[nodiscard]] virtual std::string tabToNumberForSmoke(bool forward) = 0;
    // Shift and a drag over a fraction of a fader's length: the fader's
    // value after.
    [[nodiscard]] virtual std::optional<double> fineTuneForSmoke(std::string_view eventName, double dragFraction) = 0;
    // The Control key down and a drag of the surface by the hand: whether
    // the hand showed, and the view's position after.
    [[nodiscard]] virtual std::pair<bool, juce::Point<int>> handDragForSmoke(int, int) { return {}; }
    // A numeric entry box dragged up as an invisible fader by a fraction of
    // its travel: its value after, none for another widget.
    [[nodiscard]] virtual std::optional<double> invisibleFaderForSmoke(std::string_view, double) { return std::nullopt; }
    // The Annotations' texts the surface shows above its widgets, empty
    // while there are none.
    [[nodiscard]] virtual juce::String annotationTextForSmoke() { return {}; }
    [[nodiscard]] virtual std::optional<double> widgetValueAtPositionForSmoke(
        std::string_view eventName, double position) const = 0;
    [[nodiscard]] virtual std::vector<std::string> tabsForSmoke() const = 0;
    [[nodiscard]] virtual bool selectTabForSmoke(std::string_view tab) = 0;
    [[nodiscard]] virtual bool operateButtonForSmoke(
        std::string_view eventName, bool pressed) = 0;
    [[nodiscard]] virtual bool scaledRowsScrollableForSmoke() = 0;
    [[nodiscard]] virtual std::optional<LocalMapControlPresentation>
    localMapControlPresentationForSmoke(
        std::string_view mapperNodeId,
        std::string_view eventName) const = 0;
    [[nodiscard]] virtual bool setLocalMapValueForSmoke(
        std::string_view mapperNodeId,
        std::string_view eventName,
        double value) = 0;
    [[nodiscard]] virtual bool operateLocalMapButtonForSmoke(
        std::string_view mapperNodeId,
        std::string_view eventName,
        bool pressed) = 0;
    [[nodiscard]] virtual std::optional<std::string>
    runMidiMappingWorkflowForSmoke(
        std::string_view eventName,
        talas::project::MidiControllerMessage message) = 0;
    virtual bool applyMidiController(
        talas::project::MidiControllerMessage message,
        std::string_view sourceIdentifier,
        talas::runtime::ControllerRecordingIngressEvidence recording = {},
        bool builtInControllerAccepted = false) = 0;
    virtual void cancelMidiLearn() noexcept = 0;
    virtual void finishMidiTransactionNow() = 0;
    virtual void finishMidiTransactionIfIdle() = 0;
};

[[nodiscard]] std::unique_ptr<VirtualControlSurface>
makeVirtualControlSurface(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    std::atomic<std::uint64_t>& midiUiStateToken,
    VirtualControlSurface::MidiSourceNameCallback midiSourceName,
    VirtualControlSurface::OscRouteAvailableCallback oscRouteAvailable,
    VirtualControlSurface::OscEgressRouteAvailableCallback
        oscEgressRouteAvailable,
    VirtualControlSurface::OscProfileDetailCallback oscProfileDetail,
    VirtualControlSurface::ValueDispatchCallback dispatchValue,
    VirtualControlSurface::LocalMapValueDispatchCallback dispatchLocalMapValue,
    VirtualControlSurface::LocalMapStageReadyCallback localMapStageReady,
    VirtualControlSurface::ValueGestureFinishedCallback
        onValueGestureFinished,
    VirtualControlSurface::AcceptedValueCallback onAcceptedValue,
    VirtualControlSurface::OscEgressAuthorityCallback
        beforeOscEgressAuthorityChange,
    VirtualControlSurface::OscEgressAuthorityCallback
        restoreOscEgressAuthority,
    VirtualControlSurface::DocumentChangeCallback onDocumentChange,
    VirtualControlSurface::StatusCallback onStatus,
    VirtualControlSurface::FocusCallback onFocus);
}
