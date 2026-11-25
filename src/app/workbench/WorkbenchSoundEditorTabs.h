#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace talas::app::workbench
{
// The Sound editors open, one tab each across the
// top of the Sound editor pane: a click on a tab brings its editor to the
// front, as it was; its x closes that editor. The one in front is lit.
class SoundEditorTabs final : public juce::Component
{
public:
    struct Tab
    {
        std::string definitionId;
        juce::String title;
        bool front = false;

        bool operator==(const Tab&) const = default;
    };

    SoundEditorTabs();

    std::function<void(const std::string&)> onSelect;
    std::function<void(const std::string&)> onClose;

    void setTabs(std::vector<Tab> tabs);
    [[nodiscard]] const std::vector<Tab>& tabs() const noexcept { return tabs_; }
    // The width all the tabs take.
    [[nodiscard]] int preferredWidth() const;
    // A click on a tab, on its close box or not.
    void clickTab(std::size_t index, bool onCloseBox);

    void paint(juce::Graphics& graphics) override;
    void mouseUp(const juce::MouseEvent& event) override;

private:
    [[nodiscard]] juce::Rectangle<int> tabBounds(std::size_t index) const;
    [[nodiscard]] juce::Rectangle<int> closeBounds(std::size_t index) const;
    [[nodiscard]] int tabWidth(std::size_t index) const;

    std::vector<Tab> tabs_;
};
}
