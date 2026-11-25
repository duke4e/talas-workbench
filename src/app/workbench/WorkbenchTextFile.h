#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace talas::app::workbench
{
// The text file editor: a typical text editor for a text file, a new one or one opened,
// saved where it came from or, new, where a file dialog says; Evaluate,
// Find and Replace act in it as in any text.
class TextFilePanel : public juce::Component
{
public:
    using StatusCallback = std::function<void(const juce::String&)>;

    ~TextFilePanel() override = default;

    virtual void newFile() = 0;
    virtual bool open(const std::filesystem::path& path) = 0;
    // Save, asking where for a new file.
    virtual void save() = 0;
    [[nodiscard]] virtual std::optional<std::filesystem::path> file() const = 0;
    [[nodiscard]] virtual juce::String title() const = 0;

    [[nodiscard]] virtual juce::String textForSmoke() const = 0;
    virtual void setTextForSmoke(const juce::String& text) = 0;
    virtual bool saveToForSmoke(const std::filesystem::path& path) = 0;
};

[[nodiscard]] std::unique_ptr<TextFilePanel> makeTextFilePanel(TextFilePanel::StatusCallback onStatus);
}
