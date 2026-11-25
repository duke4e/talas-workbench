#pragma once

#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace talas::app::workbench
{
// The global map editor: a text editor of the
// project's global map, the Default map's text when the project has none;
// Apply keeps it, Use Default returns to the Default map. A global map file
// opened shows that file's text, Save writing it back
// and Apply making it the project's.
class GlobalMapEditorPanel : public juce::Component
{
public:
    using DocumentChangeCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;

    ~GlobalMapEditorPanel() override = default;

    // Shows the project's map again.
    virtual void refresh() = 0;
    virtual bool openFile(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual juce::String textForSmoke() const = 0;
    virtual void setTextForSmoke(const juce::String& text) = 0;
    [[nodiscard]] virtual bool applyForSmoke() = 0;
    [[nodiscard]] virtual bool useDefaultForSmoke() = 0;
};

[[nodiscard]] std::unique_ptr<GlobalMapEditorPanel> makeGlobalMapEditorPanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    GlobalMapEditorPanel::DocumentChangeCallback onDocumentChange,
    GlobalMapEditorPanel::StatusCallback onStatus);
}
