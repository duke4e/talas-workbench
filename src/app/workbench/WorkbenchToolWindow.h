#pragma once

#include "talas/project/Tool.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace talas::app::workbench
{
// What a running Tool asks of the Workbench:
// the Event Values it writes, the Sounds it asks to play or stop, and the
// status.
struct ToolRunHost
{
    std::function<void(const talas::project::tool::EventWrite&)> writeEventValue;
    std::function<void(const talas::project::tool::SoundRequest&)> sound;
    std::function<void(const juce::String&)> status;
    // The audio channels the preferences show.
    std::function<std::size_t()> displayableAudioChannels;
    // The project's sample file a script names.
    std::function<std::optional<talas::project::smalltalk::SampleFileInfo>(std::string_view)> sampleFile;
};

// The Tool window: the Tool running, each visible
// variable shown by its widget where the Layout put it, its label beside
// it, the window's title, background and size from WindowTitle,
// BackgroundColor and the window extents, and with Debug the values of
// every variable.
class ToolRunPanel : public juce::Component
{
public:
    ~ToolRunPanel() override = default;

    // Starts the Tool; false, with the status, when it cannot.
    virtual bool run(talas::project::tool::Definition tool) = 0;
    [[nodiscard]] virtual juce::String title() const = 0;
    // Equal minimum and maximum extents make a window that cannot be
    // resized.
    [[nodiscard]] virtual bool fixedSize() const = 0;

    // The smoke's hands: the widgets shown (variable and kind), a button's
    // mouse, a fader or text set, the value a widget shows, the machine's
    // state, time passing, and the debugger's lines.
    [[nodiscard]] virtual std::vector<std::pair<std::string, talas::project::tool::WidgetKind>> widgetsForSmoke() const = 0;
    virtual bool pressForSmoke(const std::string& variable, talas::project::tool::ButtonEvent event) = 0;
    virtual bool setForSmoke(const std::string& variable, const juce::String& value) = 0;
    [[nodiscard]] virtual juce::String shownForSmoke(const std::string& variable) const = 0;
    [[nodiscard]] virtual std::string stateForSmoke() const = 0;
    virtual void advanceForSmoke(double milliseconds) = 0;
    [[nodiscard]] virtual std::vector<std::string> debugLinesForSmoke() const = 0;
};

[[nodiscard]] std::unique_ptr<ToolRunPanel> makeToolRunPanel(ToolRunHost host);
}
