#pragma once

#include "talas/project/Tool.h"
#include "talas/project/ToolFile.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace talas::app::workbench
{
// What the Tool editor asks of the Workbench:
// running the Tool, saving it, a name for something added, and the status.
struct ToolEditorHost
{
    std::function<void(const talas::project::tool::Definition&)> run;
    std::function<void(const talas::project::tool::Definition&, bool saveAs)> save;
    std::function<void(juce::String title, std::function<void(std::string)> named)> askName;
    std::function<void(const juce::String&)> status;
};

// The Tool editor: the Global Variables, the States, the
// selected state's Local Variables and Triggered Responses, the Trigger
// and Response of the one selected, the Layout of the widgets with the
// selected variable's highlighted, the Variable Settings of the selected
// variable, and Run.
class ToolEditorPanel : public juce::Component
{
public:
    ~ToolEditorPanel() override = default;

    virtual void setTool(talas::project::tool::Definition tool) = 0;
    [[nodiscard]] virtual talas::project::tool::Definition tool() const = 0;

    // The smoke's hands on the editor: a variable added (global or of the
    // state selected) and selected; a Variable Settings field set
    // (name, kind, initially, widget, labelled, left, top, width, height,
    // digits, minimum, maximum, grid, logTaper, columns, strings, down, up,
    // eventValue, access, step) and applied; a state, a triggered response.
    virtual bool addVariableForSmoke(bool global, const std::string& name) = 0;
    virtual bool selectVariableForSmoke(bool global, const std::string& name) = 0;
    virtual bool setSettingForSmoke(const std::string& field, const juce::String& value) = 0;
    virtual bool applySettingsForSmoke() = 0;
    virtual bool addStateForSmoke(const std::string& name) = 0;
    virtual bool selectStateForSmoke(const std::string& name) = 0;
    virtual bool addResponseForSmoke() = 0;
    virtual bool selectResponseForSmoke(std::size_t index) = 0;
    virtual void setTriggerForSmoke(const juce::String& trigger) = 0;
    virtual void setResponseForSmoke(const juce::String& response) = 0;
    [[nodiscard]] virtual std::optional<juce::Rectangle<int>> highlightedForSmoke() const = 0;
    [[nodiscard]] virtual std::optional<std::pair<double, double>> gridForSmoke() const = 0;
    virtual void runForSmoke() = 0;
};

[[nodiscard]] std::unique_ptr<ToolEditorPanel> makeToolEditorPanel(ToolEditorHost host);
}
