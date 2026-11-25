#pragma once

#include "talas/app/SoundFileWindowModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace talas::app::workbench
{
// A custom prototype strip: a Sound file's Sounds by category, its collections
// then General. A double-click or Return copies the selected Sound into the
// project's Sounds to build on, Ctrl+Space plays it and Ctrl+K stops it.
struct CustomPrototypesHost
{
    std::function<void(const std::string&)> add;
    std::function<void(const std::string&)> play;
    std::function<void()> stop;
};

class CustomPrototypesPanel : public juce::Component
{
public:
    ~CustomPrototypesPanel() override = default;

    virtual void setStrip(juce::String title, std::vector<talas::app::CustomPrototypeCategory> strip) = 0;
    // The rows shown: a category's name, or two spaces and a Sound's.
    [[nodiscard]] virtual std::vector<std::string> rowsForSmoke() const = 0;
    virtual bool selectRowForSmoke(std::size_t row) = 0;
    virtual bool keyForSmoke(const juce::KeyPress& key) = 0;
};

[[nodiscard]] std::unique_ptr<CustomPrototypesPanel> makeCustomPrototypesPanel(CustomPrototypesHost host);
}
