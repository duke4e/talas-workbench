#include "app/workbench/WorkbenchExpressionsLibrary.h"

#include "app/workbench/WorkbenchTheme.h"

#include <algorithm>
#include <vector>

namespace talas::app::workbench
{
namespace
{
class ExpressionsLibraryPanelImpl final : public ExpressionsLibraryPanel,
                                          private juce::ListBoxModel
{
public:
    explicit ExpressionsLibraryPanelImpl(std::function<std::vector<talas::app::ExpressionLibraryEntry>()> ownFiles)
        : ownFiles_(std::move(ownFiles))
    {
        setTitle("Expressions Library");
        search_.setTextToShowWhenEmpty("Find expressions", secondaryTextColour);
        search_.setTitle("Find expressions");
        search_.onTextChange = [this] { filter(); };
        list_.setModel(this);
        list_.setRowHeight(22);
        list_.setTitle("Expressions");
        detail_.setMultiLine(true);
        detail_.setReadOnly(true);
        detail_.setTitle("Expression and comment");
        addAndMakeVisible(search_);
        addAndMakeVisible(list_);
        addAndMakeVisible(detail_);
        setSize(460, 420);
        filter();
    }

    void setQueryForSmoke(juce::String query) override
    {
        search_.setText(query, juce::sendNotificationSync);
        filter();
    }

    [[nodiscard]] int visibleEntryCountForSmoke() const override
    {
        return static_cast<int>(entries_.size());
    }

    [[nodiscard]] juce::String dragDescriptionForSmoke(std::string_view name) const override
    {
        const auto found = std::ranges::find(entries_, name, &talas::app::ExpressionLibraryEntry::name);
        return found == entries_.end()
            ? juce::String{}
            : juce::String{talas::app::expressionLibraryDragDescription(*found)};
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        search_.setBounds(bounds.removeFromTop(26));
        bounds.removeFromTop(6);
        detail_.setBounds(bounds.removeFromBottom(90));
        bounds.removeFromBottom(6);
        list_.setBounds(bounds);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

private:
    void filter()
    {
        const auto own = ownFiles_ ? ownFiles_() : std::vector<talas::app::ExpressionLibraryEntry>{};
        entries_ = talas::app::findExpressionLibraryEntries(search_.getText().toStdString(), own);
        list_.updateContent();
        list_.deselectAllRows();
        detail_.setText({}, false);
        list_.repaint();
    }

    int getNumRows() override
    {
        return static_cast<int>(entries_.size());
    }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        if (row < 0 || row >= static_cast<int>(entries_.size()))
            return;
        if (selected)
        {
            graphics.setColour(workbenchTheme.selectedRow);
            graphics.fillRoundedRectangle(juce::Rectangle<float>{2.0F, 1.0F, static_cast<float>(width) - 4.0F, static_cast<float>(height) - 2.0F}, 5.0F);
        }
        const auto& entry = entries_[static_cast<std::size_t>(row)];
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(juce::String{entry.category}, 6, 0, 120, height, juce::Justification::centredLeft);
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::body());
        graphics.drawText(juce::String{entry.name}, 130, 0, width - 136, height, juce::Justification::centredLeft);
    }

    void selectedRowsChanged(int row) override
    {
        if (row < 0 || row >= static_cast<int>(entries_.size()))
        {
            detail_.setText({}, false);
            return;
        }
        const auto& entry = entries_[static_cast<std::size_t>(row)];
        detail_.setText(juce::String{entry.comment} + "\n\nExpression:\n" + juce::String{entry.expression}, false);
    }

    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override
    {
        if (rows.size() != 1 || rows[0] < 0 || rows[0] >= static_cast<int>(entries_.size()))
            return {};
        return juce::String{talas::app::expressionLibraryDragDescription(entries_[static_cast<std::size_t>(rows[0])])};
    }

    juce::TextEditor search_;
    juce::ListBox list_;
    juce::TextEditor detail_;
    std::vector<talas::app::ExpressionLibraryEntry> entries_;
    std::function<std::vector<talas::app::ExpressionLibraryEntry>()> ownFiles_;
};
}

std::unique_ptr<ExpressionsLibraryPanel> makeExpressionsLibraryPanel(
    std::function<std::vector<talas::app::ExpressionLibraryEntry>()> ownFiles)
{
    return std::make_unique<ExpressionsLibraryPanelImpl>(std::move(ownFiles));
}
}
