#include "app/workbench/WorkbenchAssetRecoveryPanel.h"
#include "app/workbench/WorkbenchFocusCoordinator.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/WorkbenchSurfaceState.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace talas::app::workbench
{
namespace
{
class AssetRecoveryPanelImpl final : public AssetRecoveryPanel,
                                     private juce::ListBoxModel
{
public:
    explicit AssetRecoveryPanelImpl(
        AssetRecoveryPanelCallbacks callbacks)
        : callbacks_(std::move(callbacks))
        , locateButton_("Locate File...")
        , searchButton_("Search Folder...")
        , applyButton_("Apply Relocations")
        , replaceButton_("Replace...")
        , ignoreButton_("Ignore")
        , cancelButton_("Cancel Search")
    {
        describeControl(
            *this,
            "Asset Recovery",
            "Resolve missing, inaccessible, moved, or changed referenced project assets without interrupting the active prepared graph.");
        title_.setText("Asset Recovery", juce::dontSendNotification);
        title_.setColour(juce::Label::textColourId, textColour);
        title_.setFont(WorkbenchType::panelTitle());
        status_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        status_.setFont(WorkbenchType::micro());
        status_.setJustificationType(
            juce::Justification::centredLeft);
        status_.setMinimumHorizontalScale(1.0F);
        confirmation_.setColour(
            juce::Label::textColourId,
            workbenchTheme.control);
        confirmation_.setFont(WorkbenchType::microStrong());
        confirmation_.setJustificationType(
            juce::Justification::centredLeft);
        confirmation_.setMinimumHorizontalScale(1.0F);
        confirmation_.setVisible(false);

        list_.setModel(this);
        list_.setRowHeight(62);
        list_.setMultipleSelectionEnabled(false);
        list_.setWantsKeyboardFocus(true);
        list_.setColour(
            juce::ListBox::backgroundColourId,
            panelColour);
        list_.setColour(
            juce::ListBox::outlineColourId,
            panelBorderColour);
        describeControl(
            list_,
            "Assets requiring recovery",
            "Use the arrow keys to select an asset. Its full path, accepted identity, source, and recovery state are announced.");

        candidateLabel_.setText(
            "Exact matches",
            juce::dontSendNotification);
        candidateLabel_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        candidateLabel_.setFont(WorkbenchType::microStrong());
        candidateLabel_.setVisible(false);
        candidateBox_.setVisible(false);
        candidateBox_.setWantsKeyboardFocus(true);
        describeControl(
            candidateBox_,
            "Choose exact matching path",
            "Select one path when several files have the exact accepted SHA-256 content identity.");

        addAndMakeVisible(title_);
        addAndMakeVisible(status_);
        addAndMakeVisible(list_);
        addChildComponent(candidateLabel_);
        addChildComponent(candidateBox_);
        addChildComponent(confirmation_);
        for (auto* button : {
                 &locateButton_,
                 &searchButton_,
                 &applyButton_,
                 &replaceButton_,
                 &ignoreButton_,
                 &cancelButton_,
             })
        {
            addAndMakeVisible(*button);
        }

        describeControl(
            locateButton_,
            "Locate File",
            "Inspect one explicitly selected local file. Exact bytes relocate; different valid bytes require replacement confirmation.");
        describeControl(
            searchButton_,
            "Search Folder",
            "Search one selected local folder recursively for exact accepted SHA-256 matches within the bounded resolver limits.");
        describeControl(
            applyButton_,
            "Apply Relocations",
            "Apply every selected exact relocation or first inspection as one undoable project edit.");
        describeControl(
            replaceButton_,
            "Replace Asset",
            "Review and explicitly confirm different content before advancing the accepted asset generation.");
        describeControl(
            ignoreButton_,
            "Ignore Asset",
            "Dismiss this row for the current local interaction without changing or saving the project.");
        describeControl(
            cancelButton_,
            "Cancel Asset Search",
            "Cooperatively cancel the active file inspection or folder search without changing the project.");

        list_.setExplicitFocusOrder(1);
        locateButton_.setExplicitFocusOrder(2);
        searchButton_.setExplicitFocusOrder(3);
        applyButton_.setExplicitFocusOrder(4);
        replaceButton_.setExplicitFocusOrder(5);
        ignoreButton_.setExplicitFocusOrder(6);
        cancelButton_.setExplicitFocusOrder(7);
        candidateBox_.setExplicitFocusOrder(8);

        candidateBox_.onChange = [this] {
            if (refreshingCandidate_ || selectedRow_ < 0)
                return;
            const auto* item = selectedItem();
            const auto selected = candidateBox_.getSelectedItemIndex();
            if (item != nullptr && selected >= 0
                && callbacks_.selectCandidate)
            {
                callbacks_.selectCandidate(
                    item->manifest.id,
                    static_cast<std::size_t>(selected));
            }
        };
        locateButton_.onClick = [this] {
            if (const auto* item = selectedItem();
                item != nullptr && callbacks_.locateFile)
            {
                callbacks_.locateFile(item->manifest.id);
            }
        };
        searchButton_.onClick = [this] {
            if (callbacks_.searchFolder)
                callbacks_.searchFolder();
        };
        applyButton_.onClick = [this] {
            if (callbacks_.applyRelocations)
                callbacks_.applyRelocations();
        };
        replaceButton_.onClick = [this] {
            if (const auto* item = selectedItem();
                item != nullptr && callbacks_.replace)
            {
                callbacks_.replace(item->manifest.id);
            }
        };
        ignoreButton_.onClick = [this] {
            if (const auto* item = selectedItem();
                item != nullptr && callbacks_.ignore)
            {
                callbacks_.ignore(item->manifest.id);
            }
        };
        cancelButton_.onClick = [this] {
            if (callbacks_.cancel)
                callbacks_.cancel();
        };
    }

    ~AssetRecoveryPanelImpl() override
    {
        list_.setModel(nullptr);
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::group);
    }

    const std::optional<talas::app::WorkbenchSurfaceState>&
    workbenchSurfaceStateForContextHelp() const noexcept override
    {
        return surfaceState_;
    }

    void setState(
        const talas::app::AssetRecoveryWorkbenchState& state) override
    {
        auto selectedId = std::string{};
        if (const auto* item = selectedItem(); item != nullptr)
            selectedId = item->manifest.id;
        state_ = state;
        projection_ = talas::app::assetRecoveryWorkbenchProjection(state_);
        const auto loading = state_.operationKind
            != talas::app::AssetRecoveryOperationKind::None;
        const auto completed = !loading && state_.items.empty();
        const auto kind = loading
            ? talas::app::WorkbenchSurfaceStateKind::Loading
            : completed
                ? talas::app::WorkbenchSurfaceStateKind::Success
                : talas::app::WorkbenchSurfaceStateKind::Content;
        surfaceState_ = talas::app::workbenchSurfaceState({
            kind,
            loading
                ? "Asset recovery in progress"
                : completed
                    ? "Assets verified"
                    : "Asset recovery required",
            projection_.statusText.empty()
                ? "Asset recovery state is available."
                : projection_.statusText,
            loading
                ? std::optional<talas::app::WorkbenchSurfaceActionEvidence>{
                      {"asset-recovery.cancel", "Cancel", projection_.canCancelSearch}}
                : projection_.canApplyRelocations
                    ? std::optional<talas::app::WorkbenchSurfaceActionEvidence>{
                          {"asset-recovery.apply", "Apply exact relocations", true}}
                    : std::nullopt,
            !loading && !completed,
        });
        status_.setText(
            juceString(projection_.statusText),
            juce::dontSendNotification);
        status_.setTitle(
            surfaceState_.has_value()
                ? juceString(talas::app::workbenchSurfaceStateName(
                      surfaceState_->kind()))
                    + ": " + juceString(surfaceState_->title())
                    + ". " + juceString(surfaceState_->explanation())
                : juceString(projection_.statusText));
        list_.updateContent();

        selectedRow_ = -1;
        if (!selectedId.empty())
        {
            const auto found = std::ranges::find(
                state_.items,
                selectedId,
                [](const auto& item) -> const std::string& {
                    return item.manifest.id;
                });
            if (found != state_.items.end())
            {
                selectedRow_ = static_cast<int>(
                    std::distance(state_.items.begin(), found));
            }
        }
        if (selectedRow_ < 0 && !state_.items.empty())
            selectedRow_ = 0;
        list_.selectRow(selectedRow_, false, true);
        refreshSelectionControls();
        resized();
        repaint();
    }

    [[nodiscard]] bool focusAsset(std::string_view assetId) override
    {
        const auto found = std::ranges::find(
            state_.items,
            assetId,
            [](const auto& item) -> const std::string& {
                return item.manifest.id;
            });
        if (found == state_.items.end())
            return false;
        selectedRow_ = static_cast<int>(
            std::distance(state_.items.begin(), found));
        list_.selectRow(selectedRow_, false, true);
        list_.grabKeyboardFocus();
        refreshSelectionControls();
        return true;
    }

    [[nodiscard]] bool hasUsableStateForSmoke() const override
    {
        return !state_.items.empty()
            && !getBounds().isEmpty()
            && !list_.getBounds().isEmpty()
            && list_.getWantsKeyboardFocus()
            && locateButton_.getWantsKeyboardFocus()
            && status_.getText().isNotEmpty();
    }

    [[nodiscard]] bool confirmationVisibleForSmoke() const override
    {
        return confirmation_.isVisible()
            && replaceButton_.getButtonText() == "Confirm Replace";
    }

    [[nodiscard]] bool exactApplyVisibleForSmoke() const override
    {
        return projection_.applyCount != 0
            && applyButton_.isEnabled()
            && !applyButton_.getBounds().isEmpty();
    }

    [[nodiscard]] bool activeOperationVisibleForSmoke() const override
    {
        return state_.searchActive
            && cancelButton_.isEnabled()
            && !locateButton_.isEnabled()
            && !searchButton_.isEnabled()
            && !applyButton_.isEnabled();
    }

    void paint(juce::Graphics& graphics) override
    {
        WorkbenchStyle::paintPanel(graphics, getLocalBounds().toFloat(), WorkbenchStyle::panelRadius());
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        title_.setBounds(bounds.removeFromTop(20));
        status_.setBounds(bounds.removeFromTop(30));
        bounds.removeFromTop(4);

        auto actions = bounds.removeFromBottom(
            talas::app::assetRecoveryLayoutMode(getWidth())
                    == talas::app::AssetRecoveryLayoutMode::Compact
                ? 94
                : 62);
        bounds.removeFromBottom(6);
        if (confirmation_.isVisible())
        {
            confirmation_.setBounds(bounds.removeFromBottom(34));
            bounds.removeFromBottom(4);
        }
        else
        {
            confirmation_.setBounds({});
        }
        if (candidateBox_.isVisible())
        {
            auto candidate = bounds.removeFromBottom(26);
            candidateLabel_.setBounds(candidate.removeFromLeft(88));
            candidate.removeFromLeft(4);
            candidateBox_.setBounds(candidate);
            bounds.removeFromBottom(4);
        }
        else
        {
            candidateLabel_.setBounds({});
            candidateBox_.setBounds({});
        }
        list_.setBounds(bounds);

        const auto placePair = [](juce::Rectangle<int> row,
                                  juce::Button& first,
                                  juce::Button& second) {
            const auto width = (row.getWidth() - 6) / 2;
            first.setBounds(row.removeFromLeft(width));
            row.removeFromLeft(6);
            second.setBounds(row);
        };
        if (talas::app::assetRecoveryLayoutMode(getWidth())
            == talas::app::AssetRecoveryLayoutMode::Compact)
        {
            placePair(actions.removeFromTop(28), locateButton_, searchButton_);
            actions.removeFromTop(5);
            placePair(actions.removeFromTop(28), applyButton_, replaceButton_);
            actions.removeFromTop(5);
            placePair(actions.removeFromTop(28), ignoreButton_, cancelButton_);
        }
        else
        {
            auto first = actions.removeFromTop(28);
            const auto width = (first.getWidth() - 12) / 3;
            locateButton_.setBounds(first.removeFromLeft(width));
            first.removeFromLeft(6);
            searchButton_.setBounds(first.removeFromLeft(width));
            first.removeFromLeft(6);
            applyButton_.setBounds(first);
            actions.removeFromTop(6);
            auto second = actions.removeFromTop(28);
            replaceButton_.setBounds(second.removeFromLeft(width));
            second.removeFromLeft(6);
            ignoreButton_.setBounds(second.removeFromLeft(width));
            second.removeFromLeft(6);
            cancelButton_.setBounds(second);
        }
    }

private:
    int getNumRows() override
    {
        return static_cast<int>(projection_.rows.size());
    }

    juce::String getNameForRow(int row) override
    {
        const auto* value = projectedRow(row);
        return value == nullptr
            ? juce::String{}
            : juceString(value->accessibleDescription);
    }

    juce::String getTooltipForRow(int row) override
    {
        const auto* value = projectedRow(row);
        if (value == nullptr)
            return {};
        auto tooltip = juceString(value->storedPath);
        if (!value->structuralSourcePath.empty())
            tooltip += "\nUsed by " + juceString(value->structuralSourcePath);
        if (!value->message.empty())
            tooltip += "\n" + juceString(value->message);
        return tooltip;
    }

    void paintListBoxItem(
        int row,
        juce::Graphics& graphics,
        int width,
        int height,
        bool selected) override
    {
        const auto* value = projectedRow(row);
        if (value == nullptr)
            return;
        if (selected)
        {
            graphics.setColour(workbenchTheme.selectedRow);
            graphics.fillRoundedRectangle(juce::Rectangle<float>{2.0F, 1.0F, static_cast<float>(width) - 4.0F, static_cast<float>(height) - 2.0F}, 5.0F);
        }
        auto bounds = juce::Rectangle<int>{0, 0, width, height}
                          .reduced(8, 3);
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::labelStrong());
        auto heading = juceString(value->assetId)
            + juceString(" · ") + juceString(value->kindLabel);
        graphics.drawText(
            heading,
            bounds.removeFromTop(18),
            juce::Justification::centredLeft,
            true);
        graphics.setColour(
            value->statusLabel == "Missing"
                    || value->statusLabel == "Inaccessible"
                    || value->statusLabel == "Changed"
                    || value->statusLabel == "Different content"
                ? errorColour
                : secondaryTextColour);
        graphics.setFont(WorkbenchType::microStrong());
        auto identity = juceString(value->statusLabel)
            + juceString(" · ") + juceString(value->generationLabel)
            + juceString(" · ") + juceString(value->shortHash);
        graphics.drawText(
            identity,
            bounds.removeFromTop(17),
            juce::Justification::centredLeft,
            true);
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        auto path = juceString(value->storedPath);
        if (!value->structuralSourcePath.empty())
            path += juceString(" · ")
                + juceString(value->structuralSourcePath);
        graphics.drawText(
            path,
            bounds,
            juce::Justification::centredLeft,
            true);
    }

    void selectedRowsChanged(int lastRowSelected) override
    {
        selectedRow_ = lastRowSelected;
        refreshSelectionControls();
    }

    [[nodiscard]] const talas::app::AssetRecoveryWorkbenchRow*
    projectedRow(int row) const
    {
        if (row < 0
            || row >= static_cast<int>(projection_.rows.size()))
        {
            return nullptr;
        }
        return &projection_.rows[static_cast<std::size_t>(row)];
    }

    [[nodiscard]] const talas::app::AssetRecoveryWorkbenchItem*
    selectedItem() const
    {
        if (selectedRow_ < 0
            || selectedRow_ >= static_cast<int>(state_.items.size()))
        {
            return nullptr;
        }
        return &state_.items[static_cast<std::size_t>(selectedRow_)];
    }

    void refreshSelectionControls()
    {
        const auto* row = projectedRow(selectedRow_);
        const auto* item = selectedItem();
        if (item == nullptr)
        {
            list_.setComponentID("asset-recovery.list");
        }
        else
        {
            const std::array focusFields{
                std::string_view{item->manifest.id}};
            list_.setComponentID(juceString(
                workbenchStableFocusItemId(
                    "asset-recovery.item", focusFields)));
        }
        if (list_.hasKeyboardFocus(false)
            && callbacks_.focusIdentityChanged)
        {
            callbacks_.focusIdentityChanged();
        }
        locateButton_.setEnabled(row != nullptr && row->canLocateFile);
        searchButton_.setEnabled(projection_.canSearchFolder);
        applyButton_.setEnabled(projection_.canApplyRelocations);
        replaceButton_.setEnabled(row != nullptr && row->canReplace);
        ignoreButton_.setEnabled(row != nullptr && row->canIgnore);
        cancelButton_.setEnabled(projection_.canCancelSearch);
        cancelButton_.setButtonText(
            state_.operationKind
                    == talas::app::AssetRecoveryOperationKind::FolderSearch
                ? "Cancel Search"
                : "Cancel Inspection");

        refreshingCandidate_ = true;
        candidateBox_.clear(juce::dontSendNotification);
        const auto canChoose = row != nullptr && item != nullptr
            && row->canChooseCandidate;
        candidateLabel_.setVisible(canChoose);
        candidateBox_.setVisible(canChoose);
        if (canChoose)
        {
            for (std::size_t index = 0;
                 index < item->candidates.size();
                 ++index)
            {
                candidateBox_.addItem(
                    juceString(item->candidates[index].asset.path),
                    static_cast<int>(index + 1));
            }
            if (item->selectedCandidateIndex.has_value())
            {
                candidateBox_.setSelectedItemIndex(
                    static_cast<int>(*item->selectedCandidateIndex),
                    juce::dontSendNotification);
            }
        }
        refreshingCandidate_ = false;

        const auto confirming = item != nullptr
            && state_.replacementConfirmationAssetId
                == std::optional<std::string>{item->manifest.id};
        confirmation_.setVisible(confirming);
        replaceButton_.setButtonText(
            confirming ? "Confirm Replace" : "Replace...");
        if (confirming && item->selectedCandidateIndex.has_value()
            && *item->selectedCandidateIndex < item->candidates.size())
        {
            const auto& candidate =
                item->candidates[*item->selectedCandidateIndex];
            const auto shortValue = [](std::string_view hash) {
                return hash.size() <= 8
                    ? std::string{hash}
                    : std::string{hash.substr(0, 8)};
            };
            confirmation_.setText(
                "Hash " + juceString(shortValue(item->manifest.contentHash))
                    + " to " + juceString(shortValue(candidate.asset.contentHash))
                    + juceString(" · generation ")
                    + juce::String(static_cast<juce::int64>(
                        item->manifest.generation + 1U)),
                juce::dontSendNotification);
            confirmation_.setTitle(
                "Replace " + juceString(item->manifest.id)
                + ": old hash "
                + juceString(shortValue(item->manifest.contentHash))
                + ", new hash "
                + juceString(shortValue(candidate.asset.contentHash))
                + ", next generation "
                + juce::String(static_cast<juce::int64>(
                    item->manifest.generation + 1U)));
        }
        else
        {
            confirmation_.setText({}, juce::dontSendNotification);
            confirmation_.setTitle({});
        }
        resized();
    }

    AssetRecoveryPanelCallbacks callbacks_;
    talas::app::AssetRecoveryWorkbenchState state_;
    std::optional<talas::app::WorkbenchSurfaceState> surfaceState_;
    talas::app::AssetRecoveryWorkbenchProjection projection_;
    int selectedRow_ = -1;
    bool refreshingCandidate_ = false;
    juce::Label title_;
    juce::Label status_;
    juce::ListBox list_;
    juce::Label candidateLabel_;
    juce::ComboBox candidateBox_;
    juce::Label confirmation_;
    juce::TextButton locateButton_;
    juce::TextButton searchButton_;
    juce::TextButton applyButton_;
    juce::TextButton replaceButton_;
    juce::TextButton ignoreButton_;
    juce::TextButton cancelButton_;
};
}

std::unique_ptr<AssetRecoveryPanel>
makeAssetRecoveryPanel(AssetRecoveryPanelCallbacks callbacks)
{
    return std::make_unique<AssetRecoveryPanelImpl>(
        std::move(callbacks));
}
}
