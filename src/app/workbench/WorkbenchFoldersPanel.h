#pragma once

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/TalasFiles.h"
#include "talas/project/ProjectPathEncoding.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
// Edit > Preferences' Frequently Used Folders: the upper list shows the folders inside the one
// opened, Open goes into the one selected there and Up out again; Add puts
// it in the lower list, the frequently used folders, and Remove takes the
// one selected there out.
class FrequentlyUsedFoldersPanel final : public juce::Component
{
public:
    using Changed = std::function<void(talas::app::WorkbenchPreferences)>;

    FrequentlyUsedFoldersPanel(talas::app::WorkbenchPreferences preferences, std::filesystem::path start, Changed changed)
        : preferences_(std::move(preferences)), changed_(std::move(changed))
    {
        current_.setColour(juce::Label::textColourId, secondaryTextColour);
        addAndMakeVisible(current_);
        upper_.setModel(&upperRows_);
        upperRows_.onDoubleClick = [this](int) { openSelected(); };
        addAndMakeVisible(upper_);
        lower_.setModel(&lowerRows_);
        addAndMakeVisible(lower_);
        for (auto* button : {&up_, &open_, &add_, &remove_})
            addAndMakeVisible(*button);
        up_.onClick = [this] { open(folder_.parent_path()); };
        open_.onClick = [this] { openSelected(); };
        add_.onClick = [this] { addSelected(); };
        remove_.onClick = [this] { removeSelected(); };
        describeControl(open_, "Open", "Show the folders inside the folder selected above.");
        describeControl(add_, "Add", "Add the folder selected above to the frequently used folders.");
        describeControl(remove_, "Remove", "Take the frequently used folder selected below out of the list.");
        open(std::move(start));
        refreshLower();
        setSize(460, 420);
    }

    void open(std::filesystem::path folder)
    {
        if (folder.empty())
            return;
        folder_ = std::move(folder);
        upperRows_.names.clear();
        upperFolders_ = talas::app::foldersInside(folder_);
        for (const auto& inside : upperFolders_)
            upperRows_.names.add(juceString(talas::project::pathToUtf8(inside.filename())));
        current_.setText(juceString(talas::project::pathToUtf8(folder_)), juce::dontSendNotification);
        upper_.updateContent();
        upper_.deselectAllRows();
        upper_.repaint();
    }

    // Add: the folder selected in the upper list, else the one open.
    void addSelected()
    {
        const auto row = upper_.getSelectedRow();
        const auto folder = row >= 0 && static_cast<std::size_t>(row) < upperFolders_.size()
            ? upperFolders_[static_cast<std::size_t>(row)]
            : folder_;
        preferences_ = talas::app::withFrequentlyUsedFolder(std::move(preferences_), folder);
        refreshLower();
        if (changed_)
            changed_(preferences_);
    }

    void removeSelected()
    {
        const auto row = lower_.getSelectedRow();
        if (row < 0)
            return;
        preferences_ = talas::app::withoutFrequentlyUsedFolder(std::move(preferences_), static_cast<std::size_t>(row));
        refreshLower();
        if (changed_)
            changed_(preferences_);
    }

    void selectUpper(int row) { upper_.selectRow(row); }
    void selectLower(int row) { lower_.selectRow(row); }
    void openSelected()
    {
        const auto row = upper_.getSelectedRow();
        if (row >= 0 && static_cast<std::size_t>(row) < upperFolders_.size())
            open(upperFolders_[static_cast<std::size_t>(row)]);
    }

    [[nodiscard]] const std::vector<std::filesystem::path>& upperFolders() const noexcept { return upperFolders_; }
    [[nodiscard]] const talas::app::WorkbenchPreferences& preferences() const noexcept { return preferences_; }

    void paint(juce::Graphics& graphics) override { graphics.fillAll(backgroundColour); }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(10);
        current_.setBounds(bounds.removeFromTop(22));
        auto lowerArea = bounds.removeFromBottom(bounds.getHeight() * 2 / 5);
        auto upperButtons = bounds.removeFromBottom(30);
        upper_.setBounds(bounds.reduced(0, 4));
        up_.setBounds(upperButtons.removeFromLeft(70).reduced(2));
        open_.setBounds(upperButtons.removeFromLeft(70).reduced(2));
        add_.setBounds(upperButtons.removeFromLeft(70).reduced(2));
        auto lowerButtons = lowerArea.removeFromBottom(30);
        lower_.setBounds(lowerArea.reduced(0, 4));
        remove_.setBounds(lowerButtons.removeFromLeft(80).reduced(2));
    }

private:
    struct Rows final : juce::ListBoxModel
    {
        juce::StringArray names;
        std::function<void(int)> onDoubleClick;

        int getNumRows() override { return names.size(); }
        void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool selected) override
        {
            if (selected)
                graphics.fillAll(workbenchTheme.selectedRow);
            graphics.setColour(textColour);
            graphics.setFont(WorkbenchType::body());
            graphics.drawText(names[row], 6, 0, width - 8, height, juce::Justification::centredLeft, true);
        }
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
        {
            if (onDoubleClick)
                onDoubleClick(row);
        }
    };

    void refreshLower()
    {
        lowerRows_.names.clear();
        for (const auto& folder : preferences_.frequentlyUsedFolders)
            lowerRows_.names.add(juceString(folder));
        lower_.updateContent();
        lower_.repaint();
    }

    talas::app::WorkbenchPreferences preferences_;
    Changed changed_;
    std::filesystem::path folder_;
    std::vector<std::filesystem::path> upperFolders_;
    Rows upperRows_;
    Rows lowerRows_;
    juce::Label current_;
    juce::ListBox upper_{"Folders"};
    juce::ListBox lower_{"Frequently used folders"};
    juce::TextButton up_{"Up"};
    juce::TextButton open_{"Open"};
    juce::TextButton add_{"Add"};
    juce::TextButton remove_{"Remove"};
};
}
