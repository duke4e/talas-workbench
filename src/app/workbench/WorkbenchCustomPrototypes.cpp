#include "app/workbench/WorkbenchCustomPrototypes.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include <optional>

namespace talas::app::workbench
{
namespace
{
class CustomPrototypesPanelImpl final : public CustomPrototypesPanel, private juce::ListBoxModel
{
public:
    explicit CustomPrototypesPanelImpl(CustomPrototypesHost host)
        : host_(std::move(host))
    {
        title_.setColour(juce::Label::textColourId, secondaryTextColour);
        list_.setModel(this);
        list_.setRowHeight(22);
        list_.setColour(juce::ListBox::backgroundColourId, panelColour);
        list_.setWantsKeyboardFocus(true);
        addAndMakeVisible(title_);
        addAndMakeVisible(list_);
        setSize(320, 420);
    }

    void setStrip(juce::String title, std::vector<talas::app::CustomPrototypeCategory> strip) override
    {
        title_.setText(title, juce::dontSendNotification);
        rows_.clear();
        for (const auto& category : strip)
        {
            rows_.push_back({category.name, {}});
            for (const auto& sound : category.sounds)
                rows_.push_back({sound.name, sound.definitionId});
        }
        list_.updateContent();
        list_.deselectAllRows();
        repaint();
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(6);
        title_.setBounds(bounds.removeFromTop(22));
        list_.setBounds(bounds);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

    std::vector<std::string> rowsForSmoke() const override
    {
        std::vector<std::string> rows;
        for (const auto& row : rows_)
            rows.push_back(row.definitionId.empty() ? row.name : "  " + row.name);
        return rows;
    }

    bool selectRowForSmoke(std::size_t row) override
    {
        if (row >= rows_.size())
            return false;
        list_.selectRow(static_cast<int>(row));
        return true;
    }

    bool keyForSmoke(const juce::KeyPress& key) override
    {
        return keyPressed(key);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto selected = selectedSound();
        const auto mods = key.getModifiers();
        if (key.getKeyCode() == juce::KeyPress::returnKey && selected.has_value())
        {
            host_.add(*selected);
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::spaceKey && mods.isCtrlDown() && selected.has_value())
        {
            host_.play(*selected);
            return true;
        }
        if (mods.isCommandDown() && juce::CharacterFunctions::toUpperCase(static_cast<juce::juce_wchar>(key.getKeyCode())) == 'K')
        {
            host_.stop();
            return true;
        }
        return false;
    }

private:
    struct Row
    {
        std::string name;
        std::string definitionId;
    };

    [[nodiscard]] std::optional<std::string> selectedSound() const
    {
        const auto row = list_.getSelectedRow();
        if (row < 0 || row >= static_cast<int>(rows_.size()) || rows_[static_cast<std::size_t>(row)].definitionId.empty())
            return std::nullopt;
        return rows_[static_cast<std::size_t>(row)].definitionId;
    }

    int getNumRows() override
    {
        return static_cast<int>(rows_.size());
    }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        if (row < 0 || row >= static_cast<int>(rows_.size()))
            return;
        const auto& shown = rows_[static_cast<std::size_t>(row)];
        const auto category = shown.definitionId.empty();
        if (selected && !category)
            graphics.fillAll(workbenchTheme.selectedRow);
        graphics.setColour(category ? secondaryTextColour : textColour);
        graphics.setFont(category ? WorkbenchType::labelStrong() : WorkbenchType::body());
        graphics.drawText(juceString(shown.name), category ? 6 : 20, 0, width - 26, height, juce::Justification::centredLeft);
    }

    void addRow(int row)
    {
        if (row >= 0 && row < static_cast<int>(rows_.size()) && !rows_[static_cast<std::size_t>(row)].definitionId.empty())
            host_.add(rows_[static_cast<std::size_t>(row)].definitionId);
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        addRow(row);
    }

    void returnKeyPressed(int row) override
    {
        addRow(row);
    }

    CustomPrototypesHost host_;
    juce::Label title_;
    juce::ListBox list_;
    std::vector<Row> rows_;
};
}

std::unique_ptr<CustomPrototypesPanel> makeCustomPrototypesPanel(CustomPrototypesHost host)
{
    return std::make_unique<CustomPrototypesPanelImpl>(std::move(host));
}
}
