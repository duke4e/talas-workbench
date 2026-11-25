#include "app/workbench/WorkbenchPrototypePanel.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/DescriptorPresentationModel.h"
#include "talas/app/PrototypeBrowserModel.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
juce::String supportBadgeText(talas::core::NodeSupportLevel level)
{
    switch (level)
    {
    case talas::core::NodeSupportLevel::FullRuntime:
        return "FULL";
    case talas::core::NodeSupportLevel::FunctionalSubset:
        return "SUBSET";
    case talas::core::NodeSupportLevel::Placeholder:
        return "PLACEHOLDER";
    case talas::core::NodeSupportLevel::Utility:
        return "UTILITY";
    }
    return "UTILITY";
}

juce::Colour supportBadgeColour(talas::core::NodeSupportLevel level)
{
    switch (level)
    {
    case talas::core::NodeSupportLevel::FullRuntime:
        return workbenchTheme.fullSupport;
    case talas::core::NodeSupportLevel::FunctionalSubset:
        return workbenchTheme.subsetSupport;
    case talas::core::NodeSupportLevel::Placeholder:
        return workbenchTheme.placeholderSupport;
    case talas::core::NodeSupportLevel::Utility:
        return workbenchTheme.utilitySupport;
    }
    return workbenchTheme.utilitySupport;
}

class PrototypePanelImpl final : public PrototypePanel,
                                 private juce::ListBoxModel
{
private:
    struct VisibleRow
    {
        std::optional<std::size_t> nodeIndex;
        std::string category;
    };

public:
    using AddCallback = std::function<void(const talas::core::NodeTypeDescriptor&)>;

    PrototypePanelImpl(const talas::core::NodeRegistry& registry, AddCallback onAdd)
        : nodeTypes_(talas::app::prototypePalette(registry))
        , onAdd_(std::move(onAdd))
        , favoriteButton_("Favorite")
    {
        list_.setModel(this);
        list_.setRowHeight(38);
        list_.setColour(juce::ListBox::backgroundColourId, panelColour);
        searchInput_.setTextToShowWhenEmpty(
            "Search name, category, or type ID",
            secondaryTextColour);
        searchInput_.setFont(WorkbenchType::body());
        scopeBox_.addItem("All", 1);
        scopeBox_.addItem("Favorites", 2);
        scopeBox_.addItem("Recent", 3);
        scopeBox_.setSelectedId(1, juce::dontSendNotification);
        maturityBox_.addItem("All maturity", 1);
        maturityBox_.addItem("Full", 2);
        maturityBox_.addItem("Subset", 3);
        maturityBox_.addItem("Placeholder", 4);
        maturityBox_.addItem("Utility", 5);
        maturityBox_.setSelectedId(1, juce::dontSendNotification);
        rateBox_.addItem("All rates", 1);
        rateBox_.addItem("Constant", 2);
        rateBox_.addItem("Block", 3);
        rateBox_.addItem("Sample", 4);
        rateBox_.addItem("Event", 5);
        rateBox_.addItem("Frame", 6);
        rateBox_.addItem("Variable", 7);
        rateBox_.setSelectedId(1, juce::dontSendNotification);
        originBox_.addItem("All origins", 1);
        originBox_.addItem("Built-in", 2);
        originBox_.addItem("Project", 3);
        originBox_.addItem("Preserved", 4);
        originBox_.addItem("Isolated", 5);
        originBox_.addItem("External", 6);
        originBox_.setSelectedId(1, juce::dontSendNotification);
        detail_.setMultiLine(true);
        detail_.setReadOnly(true);
        detail_.setScrollbarsShown(true);
        detail_.setCaretVisible(false);
        detail_.setFont(WorkbenchType::micro());
        detail_.setColour(
            juce::TextEditor::backgroundColourId,
            workbenchTheme.controlTile);
        detail_.setColour(
            juce::TextEditor::outlineColourId,
            panelBorderColour);
        detail_.setColour(
            juce::TextEditor::textColourId,
            secondaryTextColour);
        resultsLabel_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        resultsLabel_.setFont(WorkbenchType::micro());
        resultsLabel_.setJustificationType(
            juce::Justification::centredRight);
        describeControl(
            list_,
            "Sound prototypes",
            "Browse category groups, then press Return to add a prototype to the graph. Search and filters narrow the visible catalog.");
        describeControl(
            searchInput_,
            "Search prototypes",
            "Filter by name, category, type identifier, support, rate, vendor, origin, capability, or limitation. Every query token must match.");
        describeControl(
            scopeBox_,
            "Prototype collection",
            "Show all prototypes, favorites, or recently added prototypes.");
        describeControl(
            maturityBox_,
            "Prototype maturity",
            "Filter prototypes by verified runtime maturity.");
        describeControl(
            rateBox_,
            "Prototype signal rate",
            "Filter prototypes by any declared input or output signal rate.");
        describeControl(
            originBox_,
            "Prototype origin",
            "Filter prototypes by explicit descriptor provenance.");
        describeControl(
            favoriteButton_,
            "Favorite selected prototype",
            "Keep or remove the selected prototype in the Favorites collection.");
        describeControl(
            detail_,
            "Selected prototype capabilities",
            "Descriptor-derived vendor, origin, support, execution, latency, resource, fan-in, output-layout, and limitation details.");
        // The Sounds whose names hold the partial name typed,
        // in a pull-down list; the one chosen is selected in the prototypes.
        describeControl(matchesBox_, "Matching prototypes",
            "The prototypes whose names contain the text typed; choose one to select it in the list.");
        matchesBox_.setTextWhenNothingSelected("Matches");
        matchesBox_.setTextWhenNoChoicesAvailable("No matches");
        matchesBox_.onChange = [this] { selectMatch(matchesBox_.getText().toStdString()); };
        for (auto* component : {
                 static_cast<juce::Component*>(&matchesBox_),
                 static_cast<juce::Component*>(&searchInput_),
                 static_cast<juce::Component*>(&scopeBox_),
                 static_cast<juce::Component*>(&maturityBox_),
                 static_cast<juce::Component*>(&rateBox_),
                 static_cast<juce::Component*>(&originBox_),
                 static_cast<juce::Component*>(&favoriteButton_),
                 static_cast<juce::Component*>(&resultsLabel_),
                 static_cast<juce::Component*>(&list_),
                 static_cast<juce::Component*>(&detail_),
             })
        {
            addAndMakeVisible(*component);
        }
        searchInput_.onTextChange = [this] { rebuildVisibleRows(); };
        scopeBox_.onChange = [this] { rebuildVisibleRows(); };
        maturityBox_.onChange = [this] { rebuildVisibleRows(); };
        rateBox_.onChange = [this] { rebuildVisibleRows(); };
        originBox_.onChange = [this] { rebuildVisibleRows(); };
        favoriteButton_.onClick = [this] { toggleSelectedFavorite(); };
        rebuildVisibleRows();
    }

    [[nodiscard]] bool descriptorDiscoveryReadyForSmoke(
        std::string_view typeId,
        const juce::String& expectedDetail) override
    {
        searchInput_.setText(
            juceString(typeId),
            juce::sendNotificationSync);
        rateBox_.setSelectedId(4, juce::sendNotificationSync);
        originBox_.setSelectedId(2, juce::sendNotificationSync);
        bool found = false;
        for (int row = 0; row < getNumRows(); ++row)
        {
            const auto* descriptor = descriptorForRow(row);
            if (descriptor == nullptr || descriptor->typeId != typeId)
                continue;
            list_.selectRow(row);
            found = detail_.getText().contains(expectedDetail)
                && detail_.getTitle().contains(expectedDetail)
                && !detail_.getBounds().isEmpty()
                && rateBox_.getTitle().isNotEmpty()
                && originBox_.getTitle().isNotEmpty();
            break;
        }
        searchInput_.setText({}, juce::sendNotificationSync);
        rateBox_.setSelectedId(1, juce::sendNotificationSync);
        originBox_.setSelectedId(1, juce::sendNotificationSync);
        return found;
    }

    void focusSearch() override
    {
        searchFocusRequested_ = true;
        if (searchInput_.isShowing())
            searchInput_.grabKeyboardFocus();
        searchInput_.selectAll();
    }

    [[nodiscard]] bool searchFocusedForSmoke() const override
    {
        return searchFocusRequested_
            && searchInput_.getWantsKeyboardFocus();
    }

    void paint(juce::Graphics& graphics) override
    {
        WorkbenchStyle::paintPanel(graphics, getLocalBounds().toFloat(), WorkbenchStyle::panelRadius());
        WorkbenchStyle::paintPanelTitle(graphics, getLocalBounds().reduced(14).removeFromTop(24), "Prototypes");
    }

    void resized() override
    {
        auto content = getLocalBounds().reduced(6).withTrimmedTop(34);
        // As a strip across the top, the
        // search and filters sit at the left, the list and the description
        // beside them.
        const auto strip = getWidth() > 2 * getHeight();
        auto controls = strip ? content.removeFromLeft(std::min(300, content.getWidth() / 3)) : content;
        auto searchRow = controls.removeFromTop(24);
        matchesBox_.setBounds(searchRow.removeFromRight(searchRow.getWidth() * 2 / 5));
        searchRow.removeFromRight(4);
        searchInput_.setBounds(searchRow);
        controls.removeFromTop(5);
        auto filters = controls.removeFromTop(24);
        scopeBox_.setBounds(filters.removeFromLeft(
            std::max(82, filters.getWidth() / 2 - 3)));
        filters.removeFromLeft(6);
        maturityBox_.setBounds(filters);
        controls.removeFromTop(5);
        auto semanticFilters = controls.removeFromTop(24);
        rateBox_.setBounds(semanticFilters.removeFromLeft(
            std::max(82, semanticFilters.getWidth() / 2 - 3)));
        semanticFilters.removeFromLeft(6);
        originBox_.setBounds(semanticFilters);
        controls.removeFromTop(5);
        auto favoriteRow = controls.removeFromTop(24);
        favoriteButton_.setBounds(favoriteRow.removeFromLeft(78));
        favoriteRow.removeFromLeft(6);
        resultsLabel_.setBounds(favoriteRow);
        if (strip)
        {
            content.removeFromLeft(6);
            detail_.setBounds(content.removeFromRight(std::clamp(content.getWidth() / 3, 180, 360)));
            content.removeFromRight(6);
            list_.setBounds(content);
            return;
        }
        content = controls;
        content.removeFromTop(5);
        detail_.setBounds(content.removeFromBottom(
            std::clamp(content.getHeight() / 3, 86, 132)));
        content.removeFromBottom(5);
        list_.setBounds(content);
    }

private:
    int getNumRows() override
    {
        return static_cast<int>(visibleRows_.size());
    }

    juce::String getNameForRow(int row) override
    {
        const auto* visible = visibleRow(row);
        if (visible == nullptr)
            return {};
        if (!visible->nodeIndex.has_value())
        {
            const auto collapsed = collapsedCategories_.contains(
                visible->category);
            return juceString(visible->category)
                + (collapsed ? ", collapsed group" : ", expanded group");
        }
        const auto& descriptor = nodeTypes_[*visible->nodeIndex];
        return juceString(descriptor.displayName)
            + ", "
            + juceString(descriptor.category)
            + ", "
            + supportBadgeText(descriptor.supportLevel)
            + ", "
            + juceString(descriptor.provenance.vendor)
            + ", "
            + juceString(talas::app::descriptorOriginLabel(
                descriptor.provenance.origin));
    }

    void paintListBoxItem(int row, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        const auto* visible = visibleRow(row);
        if (visible == nullptr)
            return;
        if (!visible->nodeIndex.has_value())
        {
            graphics.setColour(juce::Colours::white.withAlpha(0.035F));
            graphics.fillRoundedRectangle(juce::Rectangle<float>{1.0F, 2.0F, static_cast<float>(width) - 2.0F,
                                              static_cast<float>(height) - 4.0F},
                4.0F);
            // The category's colour code, as its Sounds carry it in the
            // signal flow.
            graphics.setColour(WorkbenchStyle::categoryColour(visible->category));
            graphics.fillEllipse(juce::Rectangle<float>{7.0F, 7.0F}.withCentre(
                {12.0F, static_cast<float>(height) * 0.5F}));
            graphics.setColour(textColour);
            graphics.setFont(WorkbenchType::labelStrong());
            const auto collapsed = collapsedCategories_.contains(
                visible->category);
            graphics.drawText(
                juceString(visible->category),
                22,
                0,
                width - 64,
                height,
                juce::Justification::centredLeft);
            graphics.setColour(secondaryTextColour);
            graphics.setFont(WorkbenchType::micro());
            graphics.drawText(
                collapsed ? "Show" : "Hide",
                width - 44,
                0,
                36,
                height,
                juce::Justification::centredRight);
            return;
        }
        const auto& descriptor = nodeTypes_[*visible->nodeIndex];
        if (selected)
        {
            graphics.setColour(workbenchTheme.selectedRow);
            graphics.fillRoundedRectangle(juce::Rectangle<float>{1.0F, 1.0F, static_cast<float>(width) - 2.0F,
                                              static_cast<float>(height) - 2.0F},
                4.0F);
        }
        graphics.setColour(WorkbenchStyle::categoryColour(descriptor.category).withAlpha(selected ? 1.0F : 0.7F));
        graphics.fillRoundedRectangle(juce::Rectangle<float>{2.0F, 6.0F, 2.5F, static_cast<float>(height) - 12.0F}, 1.2F);
        const auto badgeWidth = juce::jlimit(
            22,
            72,
            juce::roundToInt(
                22.0F
                + static_cast<float>(std::max(0, width - 168))
                    * 1.25F));
        const auto compact = badgeWidth < 72;
        const auto badgeBounds = juce::Rectangle<int>{width - badgeWidth - 7, 7, badgeWidth, 18};
        graphics.setColour(supportBadgeColour(descriptor.supportLevel));
        graphics.fillRoundedRectangle(badgeBounds.toFloat(), 4.0F);
        graphics.setColour(juce::Colours::white);
        graphics.setFont(WorkbenchType::microStrong());
        graphics.drawText(
            compact
                ? supportBadgeText(descriptor.supportLevel).substring(0, 1)
                : supportBadgeText(descriptor.supportLevel),
            badgeBounds,
            juce::Justification::centred);

        // The entry larger or smaller as the prototypes are zoomed.
        const auto zoom = static_cast<float>(height) / static_cast<float>(talas::app::prototypeRowHeight(0));
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::bodyStrong().withHeight(WorkbenchType::bodyStrong().getHeight() * zoom));
        graphics.drawText(
            juceString(descriptor.displayName),
            8,
            juce::roundToInt(3.0F * zoom),
            width - badgeWidth - 24,
            juce::roundToInt(17.0F * zoom),
            juce::Justification::centredLeft);
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro().withHeight(WorkbenchType::micro().getHeight() * zoom));
        if (zoom >= 0.8F)
            graphics.drawText(
                juceString(descriptor.category)
                    + juceString(" · ")
                    + juceString(descriptor.provenance.vendor),
                8,
                juce::roundToInt(20.0F * zoom),
                width - badgeWidth - 24,
                juce::roundToInt(14.0F * zoom),
                juce::Justification::centredLeft);
    }

    juce::String getTooltipForRow(int row) override
    {
        const auto* descriptor = descriptorForRow(row);
        if (descriptor == nullptr)
            return {};
        return juceString(
            talas::app::prototypeCapabilityTooltip(*descriptor));
    }

    // A click on a prototype's icon selects it, on a
    // category's name shows or hides its prototypes.
    void listBoxItemClicked(
        int row,
        const juce::MouseEvent&) override
    {
        const auto* visible = visibleRow(row);
        if (visible == nullptr)
            return;
        const auto click = talas::app::prototypeListClick({visible->nodeIndex, visible->category});
        if (click.selected.has_value())
            list_.selectRow(row);
        if (click.toggledCategory.has_value())
            toggleCategory(*click.toggledCategory);
        refreshFavoriteButton();
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        const auto* visible = visibleRow(row);
        if (visible == nullptr)
            return;
        if (!visible->nodeIndex.has_value())
        {
            toggleCategory(visible->category);
            return;
        }
        openPrototype(*visible->nodeIndex);
    }

    void returnKeyPressed(int row) override
    {
        const auto* visible = visibleRow(row);
        if (visible == nullptr)
            return;
        if (!visible->nodeIndex.has_value())
        {
            toggleCategory(visible->category);
            return;
        }
        addPrototype(*visible->nodeIndex);
    }

    void selectedRowsChanged(int) override
    {
        refreshFavoriteButton();
        refreshDetail();
    }

    juce::var getDragSourceDescription(const juce::SparseSet<int>& selectedRows) override
    {
        if (selectedRows.size() != 1)
            return {};
        const auto row = selectedRows[0];
        const auto* descriptor = descriptorForRow(row);
        if (descriptor == nullptr)
            return {};
        return juceString(prototypeDragPrefix)
            + juceString(descriptor->typeId);
    }

    bool mayDragToExternalWindows() const override
    {
        return false;
    }

    [[nodiscard]] const VisibleRow* visibleRow(int row) const
    {
        if (row < 0 || row >= static_cast<int>(visibleRows_.size()))
            return nullptr;
        return &visibleRows_[static_cast<std::size_t>(row)];
    }

    [[nodiscard]] const talas::core::NodeTypeDescriptor* descriptorForRow(
        int row) const
    {
        const auto* visible = visibleRow(row);
        if (visible == nullptr || !visible->nodeIndex.has_value())
            return nullptr;
        return &nodeTypes_[*visible->nodeIndex];
    }

    [[nodiscard]] talas::app::PrototypeBrowserScope selectedScope() const
    {
        switch (scopeBox_.getSelectedId())
        {
        case 2:
            return talas::app::PrototypeBrowserScope::Favorites;
        case 3:
            return talas::app::PrototypeBrowserScope::Recent;
        default:
            return talas::app::PrototypeBrowserScope::All;
        }
    }

    [[nodiscard]] talas::app::PrototypeMaturityFilter selectedMaturity() const
    {
        switch (maturityBox_.getSelectedId())
        {
        case 2:
            return talas::app::PrototypeMaturityFilter::Full;
        case 3:
            return talas::app::PrototypeMaturityFilter::Subset;
        case 4:
            return talas::app::PrototypeMaturityFilter::Placeholder;
        case 5:
            return talas::app::PrototypeMaturityFilter::Utility;
        default:
            return talas::app::PrototypeMaturityFilter::All;
        }
    }

    [[nodiscard]] talas::app::PrototypeRateFilter selectedRate() const
    {
        switch (rateBox_.getSelectedId())
        {
        case 2: return talas::app::PrototypeRateFilter::Constant;
        case 3: return talas::app::PrototypeRateFilter::Block;
        case 4: return talas::app::PrototypeRateFilter::Sample;
        case 5: return talas::app::PrototypeRateFilter::Event;
        case 6: return talas::app::PrototypeRateFilter::Frame;
        case 7: return talas::app::PrototypeRateFilter::VariableOrUnknown;
        default: return talas::app::PrototypeRateFilter::All;
        }
    }

    [[nodiscard]] talas::app::PrototypeOriginFilter selectedOrigin() const
    {
        switch (originBox_.getSelectedId())
        {
        case 2: return talas::app::PrototypeOriginFilter::BuiltIn;
        case 3: return talas::app::PrototypeOriginFilter::Project;
        case 4: return talas::app::PrototypeOriginFilter::PreservedComponent;
        case 5: return talas::app::PrototypeOriginFilter::IsolatedComponent;
        case 6: return talas::app::PrototypeOriginFilter::External;
        default: return talas::app::PrototypeOriginFilter::All;
        }
    }

    void rebuildVisibleRows()
    {
        const auto selectedTypeId = [&]() -> std::string {
            if (const auto* selected = descriptorForRow(
                    list_.getSelectedRow()))
            {
                return selected->typeId;
            }
            return {};
        }();
        visibleRows_.clear();
        std::vector<std::size_t> matches;
        matches.reserve(nodeTypes_.size());
        const auto query = searchInput_.getText().trim().toStdString();
        const auto scope = selectedScope();
        const auto maturityFilter = selectedMaturity();
        const auto rateFilter = selectedRate();
        const auto originFilter = selectedOrigin();
        const auto appendIfMatching = [&](std::size_t index) {
            const auto& descriptor = nodeTypes_[index];
            if (talas::app::prototypeBrowserMatches(
                    talas::app::prototypeBrowserEntry(descriptor),
                    query,
                    scope,
                    maturityFilter,
                    favoriteTypeIds_,
                    recentTypeIds_,
                    rateFilter,
                    originFilter))
            {
                matches.push_back(index);
            }
        };

        if (scope == talas::app::PrototypeBrowserScope::Recent)
        {
            for (const auto& typeId : recentTypeIds_)
            {
                const auto found = std::ranges::find_if(
                    nodeTypes_,
                    [&](const auto& descriptor) {
                        return descriptor.typeId == typeId;
                    });
                if (found != nodeTypes_.end())
                    appendIfMatching(static_cast<std::size_t>(
                        std::distance(nodeTypes_.begin(), found)));
            }
        }
        else
        {
            for (std::size_t index = 0; index < nodeTypes_.size(); ++index)
                appendIfMatching(index);
        }

        std::string currentCategory;
        const auto searching = !query.empty();
        // Some Sounds appear in more than one category.
        std::vector<talas::app::PrototypePaletteRow> rows;
        if (scope == talas::app::PrototypeBrowserScope::Recent)
        {
            for (const auto index : matches)
                rows.push_back({index, nodeTypes_[index].category});
        }
        else
            rows = talas::app::prototypePaletteRows(nodeTypes_, matches);
        for (const auto& row : rows)
        {
            const auto category = talas::app::prototypeBrowserGroupName(scope, row.category);
            if (category != currentCategory)
            {
                currentCategory = category;
                visibleRows_.push_back({std::nullopt, category});
            }
            if (searching || !collapsedCategories_.contains(category))
                visibleRows_.push_back({row.index, category});
        }
        matchesBox_.clear(juce::dontSendNotification);
        int choiceId = 1;
        for (const auto& name : talas::app::prototypeFindChoices(nodeTypes_, query))
            matchesBox_.addItem(juce::String{name}, choiceId++);
        visibleResultCount_ = matches.size();
        resultsLabel_.setText(
            juce::String(static_cast<int>(visibleResultCount_))
                + (visibleResultCount_ == 1 ? " result" : " results"),
            juce::dontSendNotification);
        list_.updateContent();
        bool restoredSelection = false;
        if (!selectedTypeId.empty())
        {
            for (int row = 0; row < static_cast<int>(visibleRows_.size()); ++row)
            {
                const auto* descriptor = descriptorForRow(row);
                if (descriptor != nullptr
                    && descriptor->typeId == selectedTypeId)
                {
                    list_.selectRow(row);
                    restoredSelection = true;
                    break;
                }
            }
        }
        if (!restoredSelection)
            list_.deselectAllRows();
        refreshFavoriteButton();
        refreshDetail();
        repaint();
    }

    // The first row of the prototype named, selected and shown.
    void selectMatch(const std::string& name)
    {
        for (int row = 0; row < static_cast<int>(visibleRows_.size()); ++row)
        {
            const auto* descriptor = descriptorForRow(row);
            if (descriptor != nullptr && descriptor->displayName == name)
            {
                list_.selectRow(row);
                list_.scrollToEnsureRowIsOnscreen(row);
                return;
            }
        }
    }

    void toggleCategory(const std::string& category)
    {
        if (collapsedCategories_.contains(category))
            collapsedCategories_.erase(category);
        else
            collapsedCategories_.insert(category);
        rebuildVisibleRows();
    }

    void addPrototype(std::size_t nodeIndex)
    {
        if (nodeIndex >= nodeTypes_.size())
            return;
        talas::app::recordRecentPrototype(
            recentTypeIds_,
            nodeTypes_[nodeIndex].typeId);
        onAdd_(nodeTypes_[nodeIndex]);
        if (selectedScope() == talas::app::PrototypeBrowserScope::Recent)
            rebuildVisibleRows();
    }

    void setPlayer(PlayCallback onPlay) override { onPlay_ = std::move(onPlay); }
    void setOpener(OpenCallback onOpen) override { onOpen_ = std::move(onOpen); }
    void setCopier(CopyCallback onCopy) override { onCopy_ = std::move(onCopy); }

    void openPrototype(std::size_t nodeIndex)
    {
        if (nodeIndex >= nodeTypes_.size())
            return;
        if (!onOpen_)
            return addPrototype(nodeIndex);
        talas::app::recordRecentPrototype(recentTypeIds_, nodeTypes_[nodeIndex].typeId);
        onOpen_(nodeTypes_[nodeIndex]);
    }

    bool doubleClickForSmoke(std::string_view typeId) override
    {
        const auto found = std::ranges::find(nodeTypes_, typeId, &talas::core::NodeTypeDescriptor::typeId);
        if (found == nodeTypes_.end())
            return false;
        openPrototype(static_cast<std::size_t>(found - nodeTypes_.begin()));
        return true;
    }

    // Ctrl+Space plays the selected prototype.
    bool keyPressed(const juce::KeyPress& key) override
    {
        // Ctrl+C copies the prototype selected.
        if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'c' || key.getKeyCode() == 'C') && onCopy_)
        {
            if (const auto* descriptor = descriptorForRow(list_.getSelectedRow()))
                onCopy_(*descriptor);
            return true;
        }
        if (key.getKeyCode() != juce::KeyPress::spaceKey || !key.getModifiers().isCtrlDown() || !onPlay_)
            return false;
        if (const auto* descriptor = descriptorForRow(list_.getSelectedRow()))
            onPlay_(*descriptor);
        return true;
    }

    bool keyForSmoke(const juce::KeyPress& key) override { return keyPressed(key); }

    [[nodiscard]] std::optional<std::string> focusedPrototype() const override
    {
        if (!hasKeyboardFocus(true) && !focusedForSmoke_)
            return std::nullopt;
        const auto* descriptor = descriptorForRow(list_.getSelectedRow());
        return descriptor == nullptr ? std::nullopt : std::optional<std::string>{descriptor->typeId};
    }

    bool zoomIfFocused(bool in) override
    {
        if (!hasKeyboardFocus(true) && !focusedForSmoke_)
            return false;
        zoomStep_ = talas::app::zoomedPrototypeStep(zoomStep_, in);
        list_.setRowHeight(talas::app::prototypeRowHeight(zoomStep_));
        list_.repaint();
        return true;
    }

    [[nodiscard]] int rowHeightForSmoke() const override
    {
        return list_.getRowHeight();
    }

    bool selectForSmoke(std::string_view typeId) override
    {
        focusedForSmoke_ = !typeId.empty();
        for (int row = 0; row < list_.getListBoxModel()->getNumRows(); ++row)
        {
            if (const auto* descriptor = descriptorForRow(row); descriptor != nullptr && descriptor->typeId == typeId)
            {
                list_.selectRow(row);
                return true;
            }
        }
        return false;
    }

    void toggleSelectedFavorite()
    {
        const auto* descriptor = descriptorForRow(list_.getSelectedRow());
        if (descriptor == nullptr)
            return;
        if (favoriteTypeIds_.contains(descriptor->typeId))
            favoriteTypeIds_.erase(descriptor->typeId);
        else
            favoriteTypeIds_.insert(descriptor->typeId);
        if (selectedScope() == talas::app::PrototypeBrowserScope::Favorites)
            rebuildVisibleRows();
        else
            refreshFavoriteButton();
    }

    void refreshFavoriteButton()
    {
        const auto* descriptor = descriptorForRow(list_.getSelectedRow());
        const auto favorite = descriptor != nullptr
            && favoriteTypeIds_.contains(descriptor->typeId);
        favoriteButton_.setEnabled(descriptor != nullptr);
        favoriteButton_.setToggleState(
            favorite,
            juce::dontSendNotification);
        favoriteButton_.setButtonText(
            favorite ? "Favorited" : "Favorite");
    }

    void refreshDetail()
    {
        const auto* descriptor = descriptorForRow(list_.getSelectedRow());
        if (descriptor == nullptr)
        {
            detail_.setText(
                visibleResultCount_ == 0
                    ? "No prototype matches every active filter."
                    : "Select a prototype to inspect its exact capabilities.",
                false);
            detail_.setTitle("Selected prototype capabilities");
            return;
        }
        const auto text = juceString(
            talas::app::descriptorNodeCapabilitySummary(*descriptor));
        detail_.setText(text, false);
        detail_.setCaretPosition(0);
        detail_.setTitle(
            "Selected prototype capabilities: "
            + text.replaceCharacters("\n\r", "  "));
    }

    std::vector<talas::core::NodeTypeDescriptor> nodeTypes_;
    std::vector<VisibleRow> visibleRows_;
    std::unordered_set<std::string> favoriteTypeIds_;
    std::unordered_set<std::string> collapsedCategories_;
    std::vector<std::string> recentTypeIds_;
    std::size_t visibleResultCount_ = 0;
    bool searchFocusRequested_ = false;
    AddCallback onAdd_;
    OpenCallback onOpen_;
    CopyCallback onCopy_;
    PlayCallback onPlay_;
    // Smokes run off screen, where nothing has the keyboard focus.
    int zoomStep_ = 0;
    bool focusedForSmoke_ = false;
    juce::TextEditor searchInput_;
    juce::ComboBox matchesBox_;
    juce::ComboBox scopeBox_;
    juce::ComboBox maturityBox_;
    juce::ComboBox rateBox_;
    juce::ComboBox originBox_;
    juce::TextButton favoriteButton_;
    juce::Label resultsLabel_;
    juce::ListBox list_;
    juce::TextEditor detail_;
};

}

std::unique_ptr<PrototypePanel> makePrototypePanel(
    const talas::core::NodeRegistry& registry,
    PrototypePanel::AddCallback onAdd)
{
    return std::make_unique<PrototypePanelImpl>(
        registry,
        std::move(onAdd));
}
}
