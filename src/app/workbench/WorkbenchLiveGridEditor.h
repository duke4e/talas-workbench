#pragma once

#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace talas::app::workbench
{
// The Live Grid editor, the Compiled Sound Grid: Live Sets of 128 numbered
// boxes, a target put into a box from its menu, a program channel per grid,
// and Live Tools that step through boxes. Every change is one project edit
// through ProjectEditor.
class LiveGridEditorPanel : public juce::Component
{
public:
    using DocumentChangeCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;
    // The project's MIDI input identifiers, for program mappings.
    using MidiInputsCallback = std::function<std::vector<std::string>()>;

    enum BoxMenuItem
    {
        boxRename = 1,
        boxClear,
        boxAddToolStep,
        boxQuantizeImmediate,
        boxQuantizeNextBeat,
        boxQuantizeNextBar,
        // Target choices follow, in liveGridTargetChoices order.
        boxTargetBase = 100,
    };

    ~LiveGridEditorPanel() override = default;

    virtual void refresh() = 0;
    // The grid's On box: checked, the grid plays
    // its compiled Sounds and a click in a box loads and starts its Sound;
    // unchecked, a box's menu fills it and a double-click edits its Sound.
    using OnCallback = std::function<bool(const std::string& liveSetId, bool on)>;
    using CellCallback = std::function<bool(const std::string& liveSetId, const std::string& cellId)>;
    using SoundCallback = std::function<void(const std::string& soundDefinitionId)>;
    virtual void setGridHandlers(OnCallback on, CellCallback start, SoundCallback edit) = 0;
    [[nodiscard]] virtual bool gridOn() const = 0;
    // A new grid, selected (File > New...).
    virtual bool addGrid() = 0;
    virtual bool setOnForSmoke(bool on) = 0;
    virtual bool doubleClickBoxForSmoke(std::uint32_t row, std::uint32_t column) = 0;
    [[nodiscard]] virtual std::string selectedLiveSetId() const = 0;
    [[nodiscard]] virtual std::string selectedToolId() const = 0;
    // Smoke access to what the buttons and menus do.
    virtual bool newLiveSetForSmoke() = 0;
    virtual bool renameLiveSetForSmoke(const juce::String& name) = 0;
    virtual bool deleteLiveSetForSmoke() = 0;
    [[nodiscard]] virtual juce::PopupMenu boxMenuForSmoke(
        std::uint32_t row,
        std::uint32_t column) = 0;
    virtual bool chooseBoxMenuItemForSmoke(
        std::uint32_t row,
        std::uint32_t column,
        int itemId) = 0;
    virtual bool renameBoxForSmoke(
        std::uint32_t row,
        std::uint32_t column,
        const juce::String& name) = 0;
    virtual bool setProgramChannelForSmoke(int channel) = 0;
    virtual bool newToolForSmoke() = 0;
    virtual bool deleteToolForSmoke() = 0;
    [[nodiscard]] virtual juce::String boxTextForSmoke(
        std::uint32_t row,
        std::uint32_t column) const = 0;
};

[[nodiscard]] std::unique_ptr<LiveGridEditorPanel> makeLiveGridEditorPanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    LiveGridEditorPanel::DocumentChangeCallback onDocumentChange,
    LiveGridEditorPanel::StatusCallback onStatus,
    LiveGridEditorPanel::MidiInputsCallback midiInputs);

// A window for a panel that stays the owner's; closing hides it, unless
// `mayClose` says to stay open for now.
[[nodiscard]] std::unique_ptr<juce::DocumentWindow> makeToolWindow(
    const juce::String& title,
    juce::Component& content,
    std::function<void()> onClose = {},
    std::function<bool()> mayClose = {});
// Whether a component lies in such a window, as the Class Editor's fields
// do.
[[nodiscard]] bool isInToolWindow(const juce::Component& component);
}
