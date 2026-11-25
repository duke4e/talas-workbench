#include "app/workbench/WorkbenchSnapshotPanel.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/LiveGridWorkbenchModel.h"
#include "talas/app/SoundWorkbenchModel.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSnapshots.h"
#include "talas/runtime/SnapshotPerformance.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
class SnapshotPerformanceSurface final : public juce::Component,
                                         private juce::AsyncUpdater
{
private:
    class SnapshotRows final : public juce::ListBoxModel
    {
    public:
        explicit SnapshotRows(SnapshotPerformanceSurface& owner)
            : owner_(owner)
        {
        }

        int getNumRows() override
        {
            return static_cast<int>(
                owner_.projection_.snapshots.size());
        }

        juce::String getNameForRow(int row) override
        {
            const auto* item = owner_.snapshotRow(row);
            return item == nullptr
                ? juce::String{}
                : juceString(item->accessibleDescription);
        }

        juce::String getTooltipForRow(int row) override
        {
            const auto* item = owner_.snapshotRow(row);
            if (item == nullptr)
                return {};
            return item->diagnostic.empty()
                ? juceString(item->displayName)
                    + juceString(" · ") + juceString(item->snapshotId)
                : juceString(item->diagnostic);
        }

        void paintListBoxItem(
            int row,
            juce::Graphics& graphics,
            int width,
            int height,
            bool selected) override
        {
            const auto* item = owner_.snapshotRow(row);
            if (item == nullptr)
                return;
            if (selected)
            {
                graphics.setColour(workbenchTheme.selectedRow);
                graphics.fillRoundedRectangle(juce::Rectangle<float>{2.0F, 1.0F, static_cast<float>(width) - 4.0F, static_cast<float>(height) - 2.0F}, 5.0F);
            }
            auto bounds = juce::Rectangle<int>{0, 0, width, height}
                .reduced(7, 3);
            auto state = bounds.removeFromRight(76);
            graphics.setColour(textColour);
            graphics.setFont(WorkbenchType::bodyStrong());
            graphics.drawText(
                juceString(item->displayName),
                bounds.removeFromTop(17),
                juce::Justification::centredLeft,
                true);
            graphics.setColour(
                item->diagnostic.empty()
                    ? secondaryTextColour
                    : workbenchTheme.error);
            graphics.setFont(WorkbenchType::micro());
            graphics.drawText(
                item->diagnostic.empty()
                    ? juceString(item->snapshotId)
                    : juceString(item->diagnostic),
                bounds,
                juce::Justification::centredLeft,
                true);
            if (!item->stateLabel.empty())
            {
                auto badge = state.withSizeKeepingCentre(
                    std::min(68, state.getWidth()),
                    18);
                graphics.setColour(
                    item->active
                        ? workbenchTheme.focus
                        : workbenchTheme.widgetSurface);
                graphics.fillRoundedRectangle(
                    badge.toFloat(),
                    4.0F);
                graphics.setColour(
                    item->active
                        ? backgroundColour
                        : textColour);
                graphics.setFont(WorkbenchType::microStrong());
                graphics.drawText(
                    juceString(item->stateLabel),
                    badge,
                    juce::Justification::centred,
                    true);
            }
        }

        void selectedRowsChanged(int row) override
        {
            owner_.selectSnapshotRow(row);
        }

        void listBoxItemDoubleClicked(
            int row,
            const juce::MouseEvent&) override
        {
            owner_.selectSnapshotRow(row);
            owner_.recallSelectedSnapshot();
        }

        void returnKeyPressed(int row) override
        {
            owner_.selectSnapshotRow(row);
            owner_.recallSelectedSnapshot();
        }

    private:
        SnapshotPerformanceSurface& owner_;
    };

    class PolicyRows final : public juce::ListBoxModel
    {
    public:
        explicit PolicyRows(SnapshotPerformanceSurface& owner)
            : owner_(owner)
        {
        }

        int getNumRows() override
        {
            return static_cast<int>(
                owner_.projection_.parameters.size());
        }

        juce::String getNameForRow(int row) override
        {
            const auto* item = owner_.parameterRow(row);
            return item == nullptr
                ? juce::String{}
                : juceString(item->accessibleDescription);
        }

        juce::String getTooltipForRow(int row) override
        {
            const auto* item = owner_.parameterRow(row);
            return item == nullptr
                ? juce::String{}
                : juceString(item->accessibleDescription);
        }

        void paintListBoxItem(
            int row,
            juce::Graphics& graphics,
            int width,
            int height,
            bool selected) override
        {
            const auto* item = owner_.parameterRow(row);
            if (item == nullptr)
                return;
            if (selected)
            {
                graphics.setColour(workbenchTheme.selectedRow);
                graphics.fillRoundedRectangle(juce::Rectangle<float>{2.0F, 1.0F, static_cast<float>(width) - 4.0F, static_cast<float>(height) - 2.0F}, 5.0F);
            }
            auto bounds = juce::Rectangle<int>{0, 0, width, height}
                .reduced(7, 3);
            auto lock = bounds.removeFromRight(58);
            graphics.setColour(textColour);
            graphics.setFont(WorkbenchType::labelStrong());
            graphics.drawText(
                juceString(item->displayName),
                bounds.removeFromTop(16),
                juce::Justification::centredLeft,
                true);
            graphics.setColour(
                item->diagnostic.empty()
                    ? secondaryTextColour
                    : workbenchTheme.error);
            graphics.setFont(WorkbenchType::micro());
        const auto detail = item->diagnostic.empty()
                ? juceString(item->currentValue)
                    + (item->range.empty()
                           ? juceString(" · no range")
                           : juceString(" · ")
                               + juceString(item->range))
                : juceString(item->diagnostic);
            graphics.drawText(
                detail,
                bounds,
                juce::Justification::centredLeft,
                true);
            graphics.setColour(
                item->locked
                    ? workbenchTheme.focus
                    : secondaryTextColour);
            graphics.setFont(WorkbenchType::microStrong());
            graphics.drawText(
                item->locked ? "LOCKED" : "UNLOCKED",
                lock,
                juce::Justification::centredRight,
                true);
        }

        void selectedRowsChanged(int row) override
        {
            owner_.selectPolicyRow(row);
        }

    private:
        SnapshotPerformanceSurface& owner_;
    };

    class PolicyListBox final : public juce::ListBox
    {
    public:
        std::function<void()> onToggleLock;

        bool keyPressed(const juce::KeyPress& key) override
        {
            if (key.getKeyCode() == juce::KeyPress::spaceKey
                && onToggleLock)
            {
                onToggleLock();
                return true;
            }
            return juce::ListBox::keyPressed(key);
        }
    };

    enum class InlineEdit
    {
        None,
        RenameSnapshot,
        CloneSnapshot,
        RenameBank,
        CloneBank
    };

    enum class DeleteConfirmation
    {
        None,
        Snapshot,
        Bank
    };

public:
    using DocumentChangeCallback = std::function<void(bool)>;
    using StatusCallback = std::function<void(const juce::String&)>;

    SnapshotPerformanceSurface(
        talas::runtime::ProjectRuntimeBridge& bridge,
        const talas::core::NodeRegistry& registry,
        DocumentChangeCallback onDocumentChange,
        StatusCallback onStatus)
        : bridge_(bridge)
        , registry_(registry)
        , onDocumentChange_(std::move(onDocumentChange))
        , onStatus_(std::move(onStatus))
        , snapshotRows_(*this)
        , policyRows_(*this)
        , createBankButton_("Create Snapshot Bank")
        , recallButton_("Recall")
        , storeButton_("Store")
        , moreButton_("More")
        , morphSlider_(juce::Slider::LinearHorizontal,
                       juce::Slider::TextBoxRight)
        , randomizeButton_("Randomize")
        , mergeSlider_(juce::Slider::LinearHorizontal,
                       juce::Slider::TextBoxRight)
        , mergeButton_("Random Merge")
        , policyToggle_("Parameter Policies")
        , lockButton_("Lock")
        , applyRangeButton_("Apply Range")
        , inlineCommitButton_("Apply")
        , inlineCancelButton_("Cancel")
        , deleteConfirmButton_("Delete")
        , deleteCancelButton_("Cancel")
    {
        scopeLabel_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        scopeLabel_.setFont(WorkbenchType::label());
        statusLabel_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        statusLabel_.setFont(WorkbenchType::micro());
        statusLabel_.setJustificationType(
            juce::Justification::centredLeft);
        emptyTitle_.setColour(
            juce::Label::textColourId,
            textColour);
        emptyTitle_.setFont(WorkbenchType::bodyStrong());
        emptyTitle_.setJustificationType(
            juce::Justification::centred);
        emptyDescription_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        emptyDescription_.setFont(WorkbenchType::label());
        emptyDescription_.setJustificationType(
            juce::Justification::centred);
        morphTitle_.setText("Morph", juce::dontSendNotification);
        smoothingCaption_.setText(
            "Smoothing",
            juce::dontSendNotification);
        randomTitle_.setText(
            "Deterministic Random",
            juce::dontSendNotification);
        seedCaption_.setText(
            "Seed",
            juce::dontSendNotification);
        for (auto* label : {
                 &morphTitle_,
                 &randomTitle_,
                 &inlineLabel_,
                 &deleteLabel_,
             })
        {
            label->setColour(
                juce::Label::textColourId,
                textColour);
            label->setFont(WorkbenchType::labelStrong());
        }
        for (auto* label : {
                 &smoothingCaption_,
                 &seedCaption_,
             })
        {
            label->setColour(
                juce::Label::textColourId,
                secondaryTextColour);
            label->setFont(WorkbenchType::label());
        }
        smoothingEditor_.setTextToShowWhenEmpty(
            "Smoothing seconds",
            secondaryTextColour);
        seedEditor_.setTextToShowWhenEmpty(
            "Seed: decimal or 0x hex",
            secondaryTextColour);
        rangeMinimum_.setTextToShowWhenEmpty(
            "Minimum",
            secondaryTextColour);
        rangeMaximum_.setTextToShowWhenEmpty(
            "Maximum",
            secondaryTextColour);
        inlineEditor_.setTextToShowWhenEmpty(
            "Display name",
            secondaryTextColour);
        for (auto* editor : {
                 &smoothingEditor_,
                 &seedEditor_,
                 &rangeMinimum_,
                 &rangeMaximum_,
                 &inlineEditor_,
             })
        {
            editor->setFont(WorkbenchType::body());
        }

        snapshotList_.setModel(&snapshotRows_);
        snapshotList_.setRowHeight(42);
        snapshotList_.setMultipleSelectionEnabled(false);
        snapshotList_.setWantsKeyboardFocus(true);
        snapshotList_.setColour(
            juce::ListBox::backgroundColourId,
            panelColour);
        snapshotList_.setColour(
            juce::ListBox::outlineColourId,
            panelBorderColour);
        policyList_.setModel(&policyRows_);
        policyList_.setRowHeight(42);
        policyList_.setMultipleSelectionEnabled(false);
        policyList_.setWantsKeyboardFocus(true);
        policyList_.setColour(
            juce::ListBox::backgroundColourId,
            panelColour);
        policyList_.setColour(
            juce::ListBox::outlineColourId,
            panelBorderColour);

        morphSlider_.setRange(0.0, 100.0, 0.1);
        morphSlider_.setNumDecimalPlacesToDisplay(1);
        morphSlider_.setTextValueSuffix(" %");
        morphSlider_.setDoubleClickReturnValue(true, 50.0);
        mergeSlider_.setRange(0.0, 1.0, 0.01);
        mergeSlider_.setValue(0.5, juce::dontSendNotification);
        mergeSlider_.setNumDecimalPlacesToDisplay(2);
        mergeSlider_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            52,
            24);

        for (auto* component : {
                 static_cast<juce::Component*>(&scopeLabel_),
                 static_cast<juce::Component*>(&captureScopeBox_),
                 static_cast<juce::Component*>(&bankBox_),
                 static_cast<juce::Component*>(&statusLabel_),
                 static_cast<juce::Component*>(&emptyTitle_),
                 static_cast<juce::Component*>(&emptyDescription_),
                 static_cast<juce::Component*>(&createBankButton_),
                 static_cast<juce::Component*>(&snapshotList_),
                 static_cast<juce::Component*>(&recallButton_),
                 static_cast<juce::Component*>(&storeButton_),
                 static_cast<juce::Component*>(&moreButton_),
                 static_cast<juce::Component*>(&morphTitle_),
                 static_cast<juce::Component*>(&morphLeftBox_),
                 static_cast<juce::Component*>(&morphRightBox_),
                 static_cast<juce::Component*>(&morphSlider_),
                 static_cast<juce::Component*>(&smoothingCaption_),
                 static_cast<juce::Component*>(&smoothingEditor_),
                 static_cast<juce::Component*>(&randomTitle_),
                 static_cast<juce::Component*>(&seedCaption_),
                 static_cast<juce::Component*>(&seedEditor_),
                 static_cast<juce::Component*>(&randomizeButton_),
                 static_cast<juce::Component*>(&mergeSlider_),
                 static_cast<juce::Component*>(&mergeButton_),
                 static_cast<juce::Component*>(&policyToggle_),
                 static_cast<juce::Component*>(&policyList_),
                 static_cast<juce::Component*>(&lockButton_),
                 static_cast<juce::Component*>(&rangeMinimum_),
                 static_cast<juce::Component*>(&rangeMaximum_),
                 static_cast<juce::Component*>(&applyRangeButton_),
                 static_cast<juce::Component*>(&inlineLabel_),
                 static_cast<juce::Component*>(&inlineEditor_),
                 static_cast<juce::Component*>(&inlineCommitButton_),
                 static_cast<juce::Component*>(&inlineCancelButton_),
                 static_cast<juce::Component*>(&deleteLabel_),
                 static_cast<juce::Component*>(&deleteConfirmButton_),
                 static_cast<juce::Component*>(&deleteCancelButton_),
             })
        {
            addAndMakeVisible(*component);
        }

        describeControl(
            *this,
            "Snapshot Performance",
            "Store, recall, morph, randomize, and explicitly capture typed snapshot banks without writing live actions into project history.");
        describeControl(
            captureScopeBox_,
            "New snapshot bank scope",
            "Choose the exact project, selected node, or selected "
            "Sound definition scope for the next bank.");
        describeControl(bankBox_,
            "Snapshot bank",
            "Choose an ordered bank. The scope breadcrumb names "
            "exactly what each snapshot captures.");
        describeControl(createBankButton_,
            "Create Snapshot Bank",
            "Create one bank from the selected project, node, or Sound "
            "definition "
            "scope with an initial snapshot and declared typed random ranges.");
        describeControl(snapshotList_,
            "Snapshots",
            "Use arrow keys to select. Press Return to recall. Active "
            "and morph endpoint states are announced as text.");
        describeControl(recallButton_,
            "Recall selected snapshot",
            "Recall is a live performance action and does not edit the project.");
        describeControl(
            storeButton_,
            "Store new snapshot",
            "Store the complete current typed values for this bank as "
            "one project edit.");
        describeControl(moreButton_,
            "More snapshot commands",
            "Open secondary snapshot and bank authoring commands.");
        describeControl(
            morphLeftBox_,
            "Morph endpoint A");
        describeControl(
            morphRightBox_,
            "Morph endpoint B");
        describeControl(
            morphSlider_,
            "Snapshot morph percentage",
            "Move continuously between endpoints A and B. Updates are coalesced on the message thread.");
        describeControl(
            smoothingEditor_,
            "Bank smoothing seconds");
        describeControl(
            seedEditor_,
            "Random seed",
            "Enter a decimal or hexadecimal seed for reproducible randomization.");
        describeControl(
            randomizeButton_,
            "Randomize snapshot bank",
            "Apply deterministic typed values within each unlocked "
            "parameter range without editing the project.");
        describeControl(mergeSlider_,
            "Random merge amount",
            "Blend current live values with deterministic randomized values.");
        describeControl(
            mergeButton_,
            "Apply random merge",
            "Apply the seeded random merge as a live performance action.");
        describeControl(
            policyToggle_,
            "Parameter Policies",
            "Show or hide per-parameter lock and random-range controls.");
        describeControl(
            policyList_,
            "Snapshot parameter policies",
            "Rows announce target identity, kind, domain, current value, lock, range, and diagnostic. Press Space to toggle lock.");
        describeControl(
            lockButton_,
            "Toggle selected snapshot parameter lock");
        describeControl(
            rangeMinimum_,
            "Selected parameter random minimum");
        describeControl(
            rangeMaximum_,
            "Selected parameter random maximum");
        describeControl(
            applyRangeButton_,
            "Apply selected parameter random range");

        bankBox_.onChange = [this] { selectBank(); };
        captureScopeBox_.onChange = [this] {
            if (refreshing_)
                return;
            const auto index = captureScopeBox_.getSelectedId() - 1;
            if (index < 0
                || static_cast<std::size_t>(index)
                    >= projection_.captureScopes.size())
            {
                return;
            }
            selectedCaptureScope_ = projection_.captureScopes[
                static_cast<std::size_t>(index)].scope;
            refreshEmptyCapturePresentation();
        };
        recallButton_.onClick = [this] {
            recallSelectedSnapshot();
        };
        storeButton_.onClick = [this] { storeSnapshot(); };
        moreButton_.onClick = [this] { showMoreMenu(); };
        createBankButton_.onClick = [this] { createBank(); };
        morphLeftBox_.onChange = [this] {
            if (!refreshing_)
                morphLeftSnapshotId_ = selectedComboSnapshot(
                    morphLeftBox_);
        };
        morphRightBox_.onChange = [this] {
            if (!refreshing_)
                morphRightSnapshotId_ = selectedComboSnapshot(
                    morphRightBox_);
        };
        morphSlider_.onDragStart = [this] { beginMorph(); };
        morphSlider_.onValueChange = [this] { queueMorph(); };
        morphSlider_.onDragEnd = [this] { endMorph(); };
        smoothingEditor_.onReturnKey = [this] {
            applySmoothing();
        };
        randomizeButton_.onClick = [this] { randomize(false); };
        mergeButton_.onClick = [this] { randomize(true); };
        policyToggle_.onClick = [this] {
            policiesExpanded_ = !policiesExpanded_;
            resized();
            if (onPreferredHeightChanged)
                onPreferredHeightChanged();
        };
        policyList_.onToggleLock = [this] {
            toggleSelectedLock();
        };
        lockButton_.onClick = [this] { toggleSelectedLock(); };
        applyRangeButton_.onClick = [this] { applySelectedRange(); };
        inlineCommitButton_.onClick = [this] { commitInlineEdit(); };
        inlineEditor_.onReturnKey = [this] { commitInlineEdit(); };
        inlineCancelButton_.onClick = [this] { cancelInlineEdit(); };
        deleteConfirmButton_.onClick = [this] { confirmDelete(); };
        deleteCancelButton_.onClick = [this] { cancelDelete(); };
        refresh();
    }

    ~SnapshotPerformanceSurface() override
    {
        cancelPendingUpdate();
        snapshotList_.setModel(nullptr);
        policyList_.setModel(nullptr);
    }

    std::function<void()> onPreferredHeightChanged;

    void setSelectedInstanceId(std::string instanceId)
    {
        setCaptureContext(talas::app::snapshotWorkbenchCaptureContext(
            bridge_.editor().document(),
            instanceId,
            std::nullopt,
            {}));
    }

    void setCaptureContext(
        talas::app::SnapshotWorkbenchCaptureContext context)
    {
        if (captureContext_ != context)
            selectedCaptureScope_.reset();
        captureContext_ = std::move(context);
        refresh();
    }

    void refreshRuntimeState()
    {
        const auto transition = bridge_.snapshotTransitionActive();
        const auto coalesced = bridge_.snapshotTransitionCoalesced();
        if (transition == lastTransitionActive_
            && coalesced == lastTransitionCoalesced_)
        {
            return;
        }
        refresh();
    }

    [[nodiscard]] int preferredHeight() const noexcept
    {
        const auto editHeight = inlineEdit_ != InlineEdit::None
                || deleteConfirmation_ != DeleteConfirmation::None
            ? 58
            : 0;
        return 468 + editHeight + (policiesExpanded_ ? 192 : 0);
    }

    [[nodiscard]] bool hasAccessibleSnapshotWorkflow() const
    {
        return captureScopeBox_.getTitle().isNotEmpty()
            && bankBox_.getTitle().isNotEmpty()
            && snapshotList_.getTitle().isNotEmpty()
            && morphSlider_.getTitle().isNotEmpty()
            && seedEditor_.getTitle().isNotEmpty()
            && policyList_.getTitle().isNotEmpty();
    }

    [[nodiscard]] bool emptyStateIsUsableForSmoke() const
    {
        return projection_.banks.empty()
            && emptyTitle_.isVisible()
            && emptyDescription_.isVisible()
            && createBankButton_.isVisible()
            && captureScopeBox_.isVisible()
            && !emptyTitle_.getBounds().isEmpty()
            && !emptyDescription_.getBounds().isEmpty()
            && !createBankButton_.getBounds().isEmpty()
            && !captureScopeBox_.getBounds().isEmpty()
            && createBankButton_.getTitle().isNotEmpty()
            && captureScopeBox_.getTitle().isNotEmpty();
    }

    [[nodiscard]] bool offersDefinitionCaptureScopeForSmoke() const
    {
        return std::ranges::any_of(
            projection_.captureScopes,
            [](const auto& option) {
                return option.scope.kind
                    == talas::project::SnapshotScopeKind::Definition;
            });
    }

    [[nodiscard]] bool offersExactNestedNodeScopeForSmoke() const
    {
        return std::ranges::any_of(
            projection_.captureScopes,
            [](const auto& option) {
                return option.scope.kind
                        == talas::project::SnapshotScopeKind::Node
                    && !option.scope.instancePath.empty()
                    && option.scope.containingDefinitionId.has_value();
            });
    }

    [[nodiscard]] bool selectDefinitionCaptureScopeForSmoke()
    {
        const auto found = std::ranges::find_if(
            projection_.captureScopes,
            [](const auto& option) {
                return option.scope.kind
                    == talas::project::SnapshotScopeKind::Definition;
            });
        if (found == projection_.captureScopes.end())
            return false;
        const auto itemId = static_cast<int>(
            std::distance(projection_.captureScopes.begin(), found) + 1);
        captureScopeBox_.setSelectedId(
            itemId,
            juce::sendNotificationSync);
        return selectedCaptureScope_.has_value()
            && *selectedCaptureScope_ == found->scope
            && scopeLabel_.getText().contains("Definition /");
    }

    [[nodiscard]] bool populatedStateIsUsableForSmoke() const
    {
        return !projection_.banks.empty()
            && projection_.snapshots.size() >= 2
            && snapshotList_.isVisible()
            && !snapshotList_.getBounds().isEmpty()
            && snapshotList_.getSelectedRow() >= 0
            && !recallButton_.getBounds().isEmpty()
            && !storeButton_.getBounds().isEmpty()
            && !morphSlider_.getBounds().isEmpty()
            && !seedEditor_.getBounds().isEmpty()
            && !randomizeButton_.getBounds().isEmpty()
            && policyList_.isVisible()
            && !policyList_.getBounds().isEmpty()
            && snapshotList_.getWantsKeyboardFocus()
            && policyList_.getWantsKeyboardFocus()
            && recallButton_.getTitle().isNotEmpty()
            && storeButton_.getTitle().isNotEmpty()
            && randomizeButton_.getTitle().isNotEmpty();
    }

    [[nodiscard]] std::optional<std::string>
    runCompleteWorkflowForSmoke()
    {
        if (!projection_.banks.empty())
            return "Snapshot smoke fixture did not start empty";

        createBank();
        const auto* bank = selectedBank();
        if (bank == nullptr || bank->snapshots.size() != 1)
            return "Create Snapshot Bank did not create one initial snapshot";
        if (!projection_.canStore)
            return "New snapshot bank did not enable Store";
        if (projection_.canRandomize
            || std::ranges::none_of(
                projection_.parameters,
                [](const auto& parameter) {
                    return parameter.numeric
                        && !parameter.rangeAvailable;
                }))
        {
            return "Missing numeric ranges did not keep Randomize unavailable";
        }

        for (std::size_t attempts = 0;
             !projection_.canRandomize
                 && attempts < projection_.parameters.size();
             ++attempts)
        {
            const auto missing = std::ranges::find_if(
                projection_.parameters,
                [](const auto& parameter) {
                    return parameter.numeric
                        && !parameter.rangeAvailable;
                });
            if (missing == projection_.parameters.end())
                break;
            const auto row = static_cast<int>(std::distance(
                projection_.parameters.begin(),
                missing));
            policyList_.selectRow(row, false, true);
            rangeMinimum_.setText("0", false);
            rangeMaximum_.setText("1", false);
            applySelectedRange();
        }
        if (!projection_.canRandomize)
        {
            std::string detail = projection_.status;
            for (const auto& parameter : projection_.parameters)
            {
                if (!parameter.rangeAvailable
                    || !parameter.diagnostic.empty())
                {
                    if (!detail.empty())
                        detail += "; ";
                    detail += parameter.identityPath + " ["
                        + (parameter.rangeAvailable
                               ? "range"
                               : "no range")
                        + "] " + parameter.diagnostic;
                }
            }
            return "Explicit snapshot policy ranges did not enable "
                   "deterministic "
                   "randomization: " +
                   detail;
        }

        storeSnapshot();
        bank = selectedBank();
        if (bank == nullptr || bank->snapshots.size() != 2)
            return "Store did not append a second snapshot";
        if (!projection_.canMorph)
            return "Two compatible snapshots did not enable Morph";

        selectedSnapshotId_ = bank->snapshots.front().snapshotId;
        refresh();
        const auto recalled = bridge_.recallSnapshot(
            selectedBankId_,
            selectedSnapshotId_);
        handleActionResult(recalled, "Recalled smoke snapshot");
        if (!recalled.ok())
        {
            return "Recall failed: "
                + (recalled.message.empty()
                       ? std::string{"no diagnostic"}
                       : recalled.message);
        }
        if (bridge_.performanceState().activeSnapshotId
            != selectedSnapshotId_)
        {
            return "Recall did not activate the selected snapshot";
        }
        std::array<float, 128> transitionLeft{};
        std::array<float, 128> transitionRight{};
        std::array<float*, 2> transitionChannels{
            transitionLeft.data(),
            transitionRight.data(),
        };
        if (!bridge_.render(
                transitionChannels.data(),
                transitionChannels.size(),
                transitionLeft.size()))
        {
            return "Recalled snapshot overlay did not activate";
        }
        (void)bridge_.collectRetired();

        const auto documentBeforeLiveActions =
            talas::project::serializeProjectDocument(
                bridge_.editor().document());
        morphLeftSnapshotId_ = bank->snapshots.front().snapshotId;
        morphRightSnapshotId_ = bank->snapshots.back().snapshotId;
        beginMorph();
        morphSlider_.setValue(50.0, juce::sendNotificationSync);
        endMorph();
        if (std::abs(bridge_.performanceState().morphPosition - 0.5)
            > 1.0e-9)
        {
            return "Morph slider did not publish its 50 percent midpoint";
        }
        transitionLeft.fill(0.0F);
        transitionRight.fill(0.0F);
        if (!bridge_.render(
                transitionChannels.data(),
                transitionChannels.size(),
                transitionLeft.size()))
        {
            return "Morphed snapshot overlay did not activate";
        }
        (void)bridge_.collectRetired();

        seedEditor_.setText("0x2a", false);
        const auto randomized = bridge_.randomizeSnapshotBank(
            selectedBankId_,
            42);
        handleActionResult(
            randomized,
            "Randomized smoke snapshot bank");
        if (!randomized.ok())
        {
            return "Randomize failed: "
                + (randomized.message.empty()
                       ? std::string{"no diagnostic"}
                       : randomized.message);
        }
        if (bridge_.performanceState().lastRandomSeed != 42)
            return "Randomize did not retain the reproducible seed";
        if (talas::project::serializeProjectDocument(
                bridge_.editor().document())
            != documentBeforeLiveActions)
        {
            return "Recall, morph, or randomize mutated the project document";
        }
        policiesExpanded_ = true;
        refresh();
        return {};
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8, 4);
        scopeLabel_.setBounds(bounds.removeFromTop(18));
        bounds.removeFromTop(3);
        const auto selectorBounds = bounds.removeFromTop(24);
        const auto empty = projection_.banks.empty();
        captureScopeBox_.setBounds(empty ? selectorBounds
                                         : juce::Rectangle<int>{});
        bankBox_.setBounds(empty ? juce::Rectangle<int>{}
                                 : selectorBounds);
        bounds.removeFromTop(4);
        statusLabel_.setBounds(bounds.removeFromTop(18));
        bounds.removeFromTop(4);

        emptyTitle_.setVisible(empty);
        emptyDescription_.setVisible(empty);
        createBankButton_.setVisible(empty);
        captureScopeBox_.setVisible(empty);
        bankBox_.setVisible(!empty);
        setBankControlsVisible(!empty);
        if (empty)
        {
            emptyTitle_.setBounds(bounds.removeFromTop(28));
            emptyDescription_.setBounds(bounds.removeFromTop(58));
            auto action = bounds.removeFromTop(26);
            createBankButton_.setBounds(
                action.withSizeKeepingCentre(
                    std::min(174, action.getWidth()),
                    26));
            return;
        }

        snapshotList_.setBounds(bounds.removeFromTop(118));
        bounds.removeFromTop(5);
        auto actions = bounds.removeFromTop(24);
        recallButton_.setBounds(actions.removeFromLeft(72));
        actions.removeFromLeft(5);
        storeButton_.setBounds(actions.removeFromLeft(64));
        actions.removeFromLeft(5);
        moreButton_.setBounds(actions.removeFromLeft(62));
        bounds.removeFromTop(4);
        layoutInlineOrDelete(bounds);
        bounds.removeFromTop(8);

        morphTitle_.setBounds(bounds.removeFromTop(18));
        auto endpoints = bounds.removeFromTop(24);
        morphLeftBox_.setBounds(endpoints.removeFromLeft(
            std::max(88, endpoints.getWidth() / 2 - 3)));
        endpoints.removeFromLeft(6);
        morphRightBox_.setBounds(endpoints);
        bounds.removeFromTop(4);
        morphSlider_.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(4);
        auto smoothing = bounds.removeFromTop(24);
        smoothingCaption_.setBounds(
            smoothing.removeFromLeft(76));
        smoothingEditor_.setBounds(smoothing);
        bounds.removeFromTop(8);

        randomTitle_.setBounds(bounds.removeFromTop(18));
        auto seed = bounds.removeFromTop(24);
        seedCaption_.setBounds(seed.removeFromLeft(42));
        seedEditor_.setBounds(seed);
        bounds.removeFromTop(4);
        auto random = bounds.removeFromTop(24);
        randomizeButton_.setBounds(random.removeFromLeft(86));
        bounds.removeFromTop(4);
        auto merge = bounds.removeFromTop(24);
        mergeButton_.setBounds(merge.removeFromRight(104));
        merge.removeFromRight(5);
        mergeSlider_.setBounds(merge);
        bounds.removeFromTop(8);

        policyToggle_.setBounds(bounds.removeFromTop(24));
        if (!policiesExpanded_)
        {
            policyList_.setBounds({});
            lockButton_.setBounds({});
            rangeMinimum_.setBounds({});
            rangeMaximum_.setBounds({});
            applyRangeButton_.setBounds({});
            return;
        }
        bounds.removeFromTop(4);
        policyList_.setBounds(bounds.removeFromTop(118));
        bounds.removeFromTop(4);
        auto policy = bounds.removeFromTop(24);
        lockButton_.setBounds(policy.removeFromLeft(68));
        policy.removeFromLeft(5);
        rangeMinimum_.setBounds(policy.removeFromLeft(
            std::max(58, policy.getWidth() / 3)));
        policy.removeFromLeft(5);
        rangeMaximum_.setBounds(policy.removeFromLeft(
            std::max(58, policy.getWidth() / 2)));
        bounds.removeFromTop(4);
        applyRangeButton_.setBounds(bounds.removeFromTop(24));
    }

private:
    void setBankControlsVisible(bool visible)
    {
        for (auto* component : {
                 static_cast<juce::Component*>(&snapshotList_),
                 static_cast<juce::Component*>(&recallButton_),
                 static_cast<juce::Component*>(&storeButton_),
                 static_cast<juce::Component*>(&moreButton_),
                 static_cast<juce::Component*>(&morphTitle_),
                 static_cast<juce::Component*>(&morphLeftBox_),
                 static_cast<juce::Component*>(&morphRightBox_),
                 static_cast<juce::Component*>(&morphSlider_),
                 static_cast<juce::Component*>(&smoothingCaption_),
                 static_cast<juce::Component*>(&smoothingEditor_),
                 static_cast<juce::Component*>(&randomTitle_),
                 static_cast<juce::Component*>(&seedCaption_),
                 static_cast<juce::Component*>(&seedEditor_),
                 static_cast<juce::Component*>(&randomizeButton_),
                 static_cast<juce::Component*>(&mergeSlider_),
                 static_cast<juce::Component*>(&mergeButton_),
                 static_cast<juce::Component*>(&policyToggle_),
                 static_cast<juce::Component*>(&policyList_),
                 static_cast<juce::Component*>(&lockButton_),
                 static_cast<juce::Component*>(&rangeMinimum_),
                 static_cast<juce::Component*>(&rangeMaximum_),
                 static_cast<juce::Component*>(&applyRangeButton_),
             })
        {
            component->setVisible(visible);
        }
        inlineLabel_.setVisible(
            visible && inlineEdit_ != InlineEdit::None);
        inlineEditor_.setVisible(
            visible && inlineEdit_ != InlineEdit::None);
        inlineCommitButton_.setVisible(
            visible && inlineEdit_ != InlineEdit::None);
        inlineCancelButton_.setVisible(
            visible && inlineEdit_ != InlineEdit::None);
        deleteLabel_.setVisible(
            visible
            && deleteConfirmation_ != DeleteConfirmation::None);
        deleteConfirmButton_.setVisible(
            visible
            && deleteConfirmation_ != DeleteConfirmation::None);
        deleteCancelButton_.setVisible(
            visible
            && deleteConfirmation_ != DeleteConfirmation::None);
        policyList_.setVisible(visible && policiesExpanded_);
        lockButton_.setVisible(visible && policiesExpanded_);
        rangeMinimum_.setVisible(visible && policiesExpanded_);
        rangeMaximum_.setVisible(visible && policiesExpanded_);
        applyRangeButton_.setVisible(visible && policiesExpanded_);
    }

    void layoutInlineOrDelete(juce::Rectangle<int>& bounds)
    {
        if (inlineEdit_ != InlineEdit::None)
        {
            inlineLabel_.setBounds(bounds.removeFromTop(18));
            auto edit = bounds.removeFromTop(24);
            inlineCancelButton_.setBounds(edit.removeFromRight(58));
            edit.removeFromRight(5);
            inlineCommitButton_.setBounds(edit.removeFromRight(58));
            edit.removeFromRight(5);
            inlineEditor_.setBounds(edit);
            bounds.removeFromTop(4);
        }
        else if (deleteConfirmation_ != DeleteConfirmation::None)
        {
            deleteLabel_.setBounds(bounds.removeFromTop(22));
            auto confirmation = bounds.removeFromTop(24);
            deleteConfirmButton_.setBounds(
                confirmation.removeFromLeft(70));
            confirmation.removeFromLeft(5);
            deleteCancelButton_.setBounds(
                confirmation.removeFromLeft(62));
            bounds.removeFromTop(4);
        }
    }

    const talas::app::SnapshotWorkbenchSnapshotRow* snapshotRow(
        int row) const
    {
        return row < 0
                || static_cast<std::size_t>(row)
                    >= projection_.snapshots.size()
            ? nullptr
            : &projection_.snapshots[static_cast<std::size_t>(row)];
    }

    const talas::app::SnapshotWorkbenchParameterRow* parameterRow(
        int row) const
    {
        return row < 0
                || static_cast<std::size_t>(row)
                    >= projection_.parameters.size()
            ? nullptr
            : &projection_.parameters[static_cast<std::size_t>(row)];
    }

    void refresh()
    {
        refreshing_ = true;
        const auto performanceDocumentCurrent =
            bridge_.snapshotPerformanceDocumentCurrent();
        projection_ = talas::app::snapshotWorkbenchProjection(
            bridge_.editor().document(),
            registry_,
            bridge_.performanceState(),
            captureContext_.selectedNodeId,
            selectedBankId_,
            selectedSnapshotId_,
            actionDiagnostics_,
            bridge_.snapshotTransitionActive(),
            bridge_.snapshotTransitionCoalesced(),
            performanceDocumentCurrent,
            &captureContext_);
        selectedBankId_ = projection_.selectedBankId;
        selectedSnapshotId_ = projection_.selectedSnapshotId;
        lastTransitionActive_ = projection_.transitionActive;
        lastTransitionCoalesced_ = projection_.transitionCoalesced;
        statusLabel_.setText(
            juceString(projection_.status),
            juce::dontSendNotification);
        emptyTitle_.setText(
            juceString(projection_.emptyTitle),
            juce::dontSendNotification);
        rebuildCaptureScopeBox();
        rebuildBankBox();
        refreshEmptyCapturePresentation();
        snapshotList_.updateContent();
        const auto selectedSnapshot = std::ranges::find(
            projection_.snapshots,
            selectedSnapshotId_,
            &talas::app::SnapshotWorkbenchSnapshotRow::snapshotId);
        snapshotList_.selectRow(
            selectedSnapshot == projection_.snapshots.end()
                ? -1
                : static_cast<int>(std::distance(
                    projection_.snapshots.begin(),
                    selectedSnapshot)),
            false,
            true);
        rebuildEndpointBoxes();
        morphSlider_.setValue(
            projection_.morphPosition * 100.0,
            juce::dontSendNotification);
        smoothingEditor_.setText(
            juce::String(projection_.smoothingSeconds, 4),
            false);
        if (!seedEditor_.hasKeyboardFocus(true))
        {
            seedEditor_.setText(
                "0x" + juce::String::toHexString(
                    static_cast<juce::int64>(projection_.randomSeed)),
                false);
        }
        recallButton_.setEnabled(projection_.canRecall);
        storeButton_.setEnabled(projection_.canStore);
        morphLeftBox_.setEnabled(projection_.canMorph);
        morphRightBox_.setEnabled(projection_.canMorph);
        morphSlider_.setEnabled(projection_.canMorph);
        randomizeButton_.setEnabled(projection_.canRandomize);
        mergeButton_.setEnabled(projection_.canRandomMerge);
        createBankButton_.setEnabled(
            performanceDocumentCurrent
            && !projection_.captureScopes.empty());
        policyList_.updateContent();
        if (!projection_.parameters.empty()
            && policyList_.getSelectedRow() < 0)
        {
            policyList_.selectRow(0, false, true);
        }
        refreshSelectedPolicyEditors();
        refreshing_ = false;
        setBankControlsVisible(!projection_.banks.empty());
        resized();
        if (onPreferredHeightChanged)
            onPreferredHeightChanged();
    }

    void rebuildBankBox()
    {
        bankBox_.clear(juce::dontSendNotification);
        int selected = 0;
        for (std::size_t index = 0;
             index < projection_.banks.size();
             ++index)
        {
            const auto& bank = projection_.banks[index];
            bankBox_.addItem(
                juceString(bank.displayName)
                    + juceString(" · ")
                    + juceString(bank.scopeBreadcrumb),
                static_cast<int>(index + 1));
            if (bank.bankId == selectedBankId_)
                selected = static_cast<int>(index + 1);
        }
        bankBox_.setSelectedId(selected, juce::dontSendNotification);
    }

    void rebuildCaptureScopeBox()
    {
        captureScopeBox_.clear(juce::dontSendNotification);
        int selected = 0;
        for (std::size_t index = 0;
             index < projection_.captureScopes.size();
             ++index)
        {
            const auto& option = projection_.captureScopes[index];
            captureScopeBox_.addItem(
                juceString(option.displayName),
                static_cast<int>(index + 1));
            if (selectedCaptureScope_.has_value()
                && option.scope == *selectedCaptureScope_)
            {
                selected = static_cast<int>(index + 1);
            }
        }
        if (selected == 0 && !projection_.captureScopes.empty())
            selected = 1;
        captureScopeBox_.setSelectedId(
            selected,
            juce::dontSendNotification);
        if (selected > 0)
        {
            selectedCaptureScope_ = projection_.captureScopes[
                static_cast<std::size_t>(selected - 1)].scope;
        }
    }

    void refreshEmptyCapturePresentation()
    {
        if (!projection_.banks.empty())
        {
            scopeLabel_.setText(
                juceString(projection_.captureScopeBreadcrumb),
                juce::dontSendNotification);
            emptyDescription_.setText(
                juceString(projection_.emptyDescription),
                juce::dontSendNotification);
            return;
        }
        const auto* option = selectedCaptureScopeOption();
        const auto breadcrumb = option == nullptr
            ? std::string{"Project / Root"}
            : option->scopeBreadcrumb;
        scopeLabel_.setText(
            juceString("New bank · ") + juceString(breadcrumb),
            juce::dontSendNotification);
        emptyDescription_.setText(
            "Create a bank to capture " + juceString(breadcrumb)
                + " as one complete typed scope.",
            juce::dontSendNotification);
    }

    const talas::app::SnapshotWorkbenchCaptureScopeOption*
    selectedCaptureScopeOption() const
    {
        if (!selectedCaptureScope_.has_value())
            return nullptr;
        const auto found = std::ranges::find(
            projection_.captureScopes,
            *selectedCaptureScope_,
            &talas::app::SnapshotWorkbenchCaptureScopeOption::scope);
        return found == projection_.captureScopes.end()
            ? nullptr
            : std::addressof(*found);
    }

    void rebuildEndpointBoxes()
    {
        const auto state = bridge_.performanceState();
        if (morphLeftSnapshotId_.empty())
            morphLeftSnapshotId_ = state.morphLeftSnapshotId;
        if (morphRightSnapshotId_.empty())
            morphRightSnapshotId_ = state.morphRightSnapshotId;
        const auto endpointAvailable = [this](std::string_view id) {
            const auto found = std::ranges::find(
                projection_.snapshots,
                id,
                &talas::app::SnapshotWorkbenchSnapshotRow::snapshotId);
            return found != projection_.snapshots.end()
                && found->recallEnabled;
        };
        const auto firstAvailable = std::ranges::find_if(
            projection_.snapshots,
            &talas::app::SnapshotWorkbenchSnapshotRow::recallEnabled);
        const auto secondAvailable = firstAvailable
                == projection_.snapshots.end()
            ? projection_.snapshots.end()
            : std::ranges::find_if(
                  std::next(firstAvailable),
                  projection_.snapshots.end(),
                  &talas::app::SnapshotWorkbenchSnapshotRow::recallEnabled);
        if (!endpointAvailable(morphLeftSnapshotId_))
        {
            morphLeftSnapshotId_ = firstAvailable
                    == projection_.snapshots.end()
                ? std::string{}
                : firstAvailable->snapshotId;
        }
        if (!endpointAvailable(morphRightSnapshotId_))
        {
            morphRightSnapshotId_ = secondAvailable
                    == projection_.snapshots.end()
                ? morphLeftSnapshotId_
                : secondAvailable->snapshotId;
        }
        for (auto* box : {&morphLeftBox_, &morphRightBox_})
            box->clear(juce::dontSendNotification);
        int left = 0;
        int right = 0;
        for (std::size_t index = 0;
             index < projection_.snapshots.size();
             ++index)
        {
            const auto& snapshot = projection_.snapshots[index];
            const auto itemId = static_cast<int>(index + 1);
            morphLeftBox_.addItem(
                juceString("A · ")
                    + juceString(snapshot.displayName),
                itemId);
            morphRightBox_.addItem(
                juceString("B · ")
                    + juceString(snapshot.displayName),
                itemId);
            morphLeftBox_.setItemEnabled(
                itemId,
                snapshot.recallEnabled);
            morphRightBox_.setItemEnabled(
                itemId,
                snapshot.recallEnabled);
            if (snapshot.snapshotId == morphLeftSnapshotId_)
                left = itemId;
            if (snapshot.snapshotId == morphRightSnapshotId_)
                right = itemId;
        }
        morphLeftBox_.setSelectedId(left, juce::dontSendNotification);
        morphRightBox_.setSelectedId(right, juce::dontSendNotification);
    }

    std::string selectedComboSnapshot(const juce::ComboBox& box) const
    {
        const auto index = box.getSelectedId() - 1;
        return index < 0
                || static_cast<std::size_t>(index)
                    >= projection_.snapshots.size()
            ? std::string{}
            : projection_.snapshots[static_cast<std::size_t>(index)]
                  .snapshotId;
    }

    void selectBank()
    {
        if (refreshing_)
            return;
        const auto index = bankBox_.getSelectedId() - 1;
        if (index < 0
            || static_cast<std::size_t>(index)
                >= projection_.banks.size())
        {
            return;
        }
        selectedBankId_ =
            projection_.banks[static_cast<std::size_t>(index)].bankId;
        selectedSnapshotId_.clear();
        morphLeftSnapshotId_.clear();
        morphRightSnapshotId_.clear();
        actionDiagnostics_.clear();
        refresh();
    }

    void selectSnapshotRow(int row)
    {
        if (refreshing_)
            return;
        const auto* snapshot = snapshotRow(row);
        if (snapshot == nullptr)
            return;
        selectedSnapshotId_ = snapshot->snapshotId;
        recallButton_.setEnabled(snapshot->recallEnabled);
    }

    void selectPolicyRow(int)
    {
        if (!refreshing_)
            refreshSelectedPolicyEditors();
    }

    void refreshSelectedPolicyEditors()
    {
        const auto* row = parameterRow(policyList_.getSelectedRow());
        const auto available = row != nullptr;
        lockButton_.setEnabled(available);
        applyRangeButton_.setEnabled(available && row->numeric);
        rangeMinimum_.setEnabled(available && row->numeric);
        rangeMaximum_.setEnabled(available && row->numeric);
        if (!available)
            return;
        lockButton_.setButtonText(row->locked ? "Unlock" : "Lock");
        const auto* bank = selectedBank();
        if (bank == nullptr)
        {
            rangeMinimum_.clear();
            rangeMaximum_.clear();
            return;
        }
        const auto policy = std::ranges::find(
            bank->parameterPolicies,
            row->target,
            &talas::project::SnapshotParameterPolicy::target);
        if (policy != bank->parameterPolicies.end()
            && policy->range.has_value())
        {
            rangeMinimum_.setText(
                juce::String(policy->range->minimum, 8),
                false);
            rangeMaximum_.setText(
                juce::String(policy->range->maximum, 8),
                false);
        }
        else
        {
            rangeMinimum_.clear();
            rangeMaximum_.clear();
        }
    }

    const talas::project::ProjectSnapshotBank* selectedBank() const
    {
        const auto& banks =
            bridge_.editor().document().snapshotLibrary.banks;
        const auto found = std::ranges::find(
            banks,
            selectedBankId_,
            &talas::project::ProjectSnapshotBank::bankId);
        return found == banks.end() ? nullptr : std::addressof(*found);
    }

    std::optional<std::vector<talas::project::SnapshotValueEntry>>
    valuesForScope(const talas::project::SnapshotScopeIdentity& scope)
    {
        auto captured = bridge_.currentSnapshotScopeValues(scope);
        if (!captured.ok())
        {
            actionDiagnostics_ = std::move(captured.diagnostics);
            announce(
                bridge_.snapshotPerformanceDocumentCurrent()
                    ? "Snapshot scope has incomplete current values"
                    : "Stage the current project before storing snapshot values");
            return std::nullopt;
        }
        return std::move(captured.values);
    }

    std::optional<std::vector<talas::project::SnapshotParameterPolicy>>
    initialPoliciesForScope(
        const talas::project::SnapshotScopeIdentity& scope)
    {
        talas::expr::EvaluationContext evaluation;
        const auto resolved = talas::project::resolveSnapshotScopeTargets(
            bridge_.editor().document(),
            registry_,
            scope,
            evaluation);
        if (!resolved.ok())
        {
            actionDiagnostics_ = resolved.diagnostics;
            announce("Snapshot scope cannot initialize parameter policies");
            return std::nullopt;
        }
        std::vector<talas::project::SnapshotParameterPolicy> policies;
        for (const auto& target : resolved.targets)
        {
            if (!target.initialRandomRange.has_value())
                continue;
            policies.push_back({
                target.target,
                false,
                target.initialRandomRange,
            });
        }
        return policies;
    }

    std::string uniqueBankId() const
    {
        const auto& banks =
            bridge_.editor().document().snapshotLibrary.banks;
        for (std::size_t ordinal = 1;
             ordinal <= talas::project::maximumSnapshotBankCount;
             ++ordinal)
        {
            const auto candidate = "bank" + std::to_string(ordinal);
            if (std::ranges::find(
                    banks,
                    candidate,
                    &talas::project::ProjectSnapshotBank::bankId)
                == banks.end())
            {
                return candidate;
            }
        }
        return {};
    }

    std::string uniqueSnapshotId(
        const talas::project::ProjectSnapshotBank& bank) const
    {
        for (std::size_t ordinal = 1;
             ordinal <= talas::project::maximumSnapshotsPerBank;
             ++ordinal)
        {
            const auto candidate = "snapshot"
                + std::to_string(ordinal);
            if (std::ranges::find(
                    bank.snapshots,
                    candidate,
                    &talas::project::ProjectSnapshot::snapshotId)
                == bank.snapshots.end())
            {
                return candidate;
            }
        }
        return {};
    }

    void createBank()
    {
        const auto* selectedScope = selectedCaptureScopeOption();
        if (selectedScope == nullptr)
        {
            announce("Choose a valid snapshot capture scope");
            return;
        }
        auto scope = selectedScope->scope;
        const auto values = valuesForScope(scope);
        const auto policies = initialPoliciesForScope(scope);
        const auto bankId = uniqueBankId();
        if (!values.has_value()
            || !policies.has_value()
            || bankId.empty())
        {
            if (bankId.empty())
                announce("Snapshot bank capacity reached");
            return;
        }
        talas::project::ProjectSnapshotBank bank;
        bank.bankId = bankId;
        bank.displayName = "Snapshot Bank "
            + std::to_string(
                bridge_.editor().document()
                    .snapshotLibrary.banks.size() + 1);
        bank.scope = std::move(scope);
        bank.parameterPolicies = *policies;
        bank.snapshots.push_back({
            "snapshot1",
            "Snapshot 1",
            *values,
        });
        const auto result = bridge_.editor().addSnapshotBank(
            std::move(bank));
        if (!result.ok())
        {
            announce(juceString(result.message));
            return;
        }
        selectedBankId_ = bankId;
        selectedSnapshotId_ = "snapshot1";
        finishSnapshotAuthoringEdit("Created snapshot bank");
    }

    void storeSnapshot()
    {
        const auto* bank = selectedBank();
        if (bank == nullptr)
            return;
        const auto values = valuesForScope(bank->scope);
        const auto snapshotId = uniqueSnapshotId(*bank);
        if (!values.has_value() || snapshotId.empty())
        {
            if (snapshotId.empty())
                announce("Snapshot capacity reached for this bank");
            return;
        }
        const auto displayName = "Snapshot "
            + std::to_string(bank->snapshots.size() + 1);
        const auto result = bridge_.editor().storeSnapshot(
            bank->bankId,
            {snapshotId, displayName, *values});
        if (!result.ok())
        {
            announce(juceString(result.message));
            return;
        }
        selectedSnapshotId_ = snapshotId;
        finishSnapshotAuthoringEdit("Stored new snapshot");
    }

    void captureSelectedSnapshot()
    {
        if (selectedBankId_.empty() || selectedSnapshotId_.empty())
            return;
        const auto result = bridge_.captureSnapshotPerformance(
            selectedBankId_,
            selectedSnapshotId_);
        handleActionResult(result, "Captured current performance state");
        if (result.ok())
            onDocumentChange_(true);
    }

    void recallSelectedSnapshot()
    {
        if (!projection_.canRecall)
            return;
        handleActionResult(
            bridge_.recallSnapshot(
                selectedBankId_,
                selectedSnapshotId_),
            "Recalled " + juceString(selectedSnapshotId_));
    }

    void beginMorph()
    {
        if (morphGestureActive_ || !projection_.canMorph)
            return;
        const auto result = bridge_.beginSnapshotMorph(
            selectedBankId_,
            morphLeftSnapshotId_,
            morphRightSnapshotId_);
        if (!result.ok())
        {
            handleActionResult(result, {});
            return;
        }
        morphGestureActive_ = true;
        actionDiagnostics_.clear();
    }

    void queueMorph()
    {
        if (refreshing_ || !projection_.canMorph)
            return;
        if (!morphGestureActive_)
        {
            beginMorph();
            standaloneMorphPending_ = morphGestureActive_;
        }
        if (!morphGestureActive_)
            return;
        pendingMorphAmount_ = morphSlider_.getValue() / 100.0;
        morphUpdatePending_ = true;
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override
    {
        publishPendingMorph();
    }

    void publishPendingMorph()
    {
        if (!morphUpdatePending_ || !morphGestureActive_)
            return;
        morphUpdatePending_ = false;
        const auto result = bridge_.updateSnapshotMorph(
            pendingMorphAmount_);
        actionDiagnostics_ = result.diagnostics;
        if (!result.ok())
            announce(juceString(result.message));
        if (standaloneMorphPending_)
        {
            standaloneMorphPending_ = false;
            bridge_.endSnapshotMorph();
            morphGestureActive_ = false;
        }
        refresh();
    }

    void endMorph()
    {
        cancelPendingUpdate();
        publishPendingMorph();
        if (morphGestureActive_)
            bridge_.endSnapshotMorph();
        morphGestureActive_ = false;
        standaloneMorphPending_ = false;
        refresh();
        announce("Morph gesture complete");
    }

    std::optional<std::uint64_t> randomSeed() const
    {
        auto text = seedEditor_.getText().trim().toStdString();
        auto base = 10;
        if (text.starts_with("0x") || text.starts_with("0X"))
        {
            text.erase(0, 2);
            base = 16;
        }
        std::uint64_t seed = 0;
        const auto parsed = std::from_chars(
            text.data(),
            text.data() + text.size(),
            seed,
            base);
        return !text.empty()
                && parsed.ec == std::errc{}
                && parsed.ptr == text.data() + text.size()
            ? std::optional<std::uint64_t>{seed}
            : std::nullopt;
    }

    void randomize(bool merge)
    {
        const auto seed = randomSeed();
        if (!seed.has_value())
        {
            announce("Random seed must be decimal or 0x hexadecimal");
            return;
        }
        const auto result = merge
            ? bridge_.randomMergeSnapshotBank(
                selectedBankId_,
                *seed,
                mergeSlider_.getValue())
            : bridge_.randomizeSnapshotBank(
                selectedBankId_,
                *seed);
        handleActionResult(
            result,
            merge ? "Applied deterministic random merge"
                  : "Randomized snapshot bank deterministically");
    }

    void applySmoothing()
    {
        const auto* bank = selectedBank();
        if (bank == nullptr)
            return;
        const auto value = finiteNumber(smoothingEditor_.getText());
        if (!value.has_value()
            || *value < 0.0
            || *value > talas::project::maximumSnapshotSmoothingSeconds)
        {
            announce("Smoothing must be between 0 and 30 seconds");
            return;
        }
        const auto result = bridge_.editor().setSnapshotBankSmoothing(
            bank->bankId,
            *value);
        if (!result.ok())
        {
            announce(juceString(result.message));
            return;
        }
        finishSnapshotAuthoringEdit("Updated snapshot smoothing");
    }

    void toggleSelectedLock()
    {
        const auto* bank = selectedBank();
        const auto* row = parameterRow(policyList_.getSelectedRow());
        if (bank == nullptr || row == nullptr)
            return;
        auto policy = talas::project::SnapshotParameterPolicy{};
        policy.target = row->target;
        const auto existing = std::ranges::find(
            bank->parameterPolicies,
            row->target,
            &talas::project::SnapshotParameterPolicy::target);
        if (existing != bank->parameterPolicies.end())
            policy = *existing;
        policy.locked = !row->locked;
        const auto result = bridge_.editor().setSnapshotParameterPolicy(
            bank->bankId,
            std::move(policy));
        if (!result.ok())
        {
            announce(juceString(result.message));
            return;
        }
        finishSnapshotAuthoringEdit(
            row->locked ? "Unlocked snapshot target"
                        : "Locked snapshot target");
    }

    static std::optional<double> finiteNumber(
        const juce::String& text)
    {
        const auto source = text.trim().toStdString();
        double value = 0.0;
        const auto parsed = std::from_chars(
            source.data(),
            source.data() + source.size(),
            value,
            std::chars_format::general);
        return !source.empty()
                && parsed.ec == std::errc{}
                && parsed.ptr == source.data() + source.size()
                && std::isfinite(value)
            ? std::optional<double>{value}
            : std::nullopt;
    }

    void applySelectedRange()
    {
        const auto* bank = selectedBank();
        const auto* row = parameterRow(policyList_.getSelectedRow());
        const auto minimum = finiteNumber(rangeMinimum_.getText());
        const auto maximum = finiteNumber(rangeMaximum_.getText());
        if (bank == nullptr || row == nullptr)
            return;
        if (!minimum.has_value()
            || !maximum.has_value()
            || *minimum > *maximum)
        {
            announce("Random range needs finite minimum and maximum values in order");
            return;
        }
        talas::project::SnapshotParameterPolicy policy;
        policy.target = row->target;
        policy.locked = row->locked;
        policy.range = talas::project::SnapshotParameterRange{
            *minimum,
            *maximum,
        };
        const auto result = bridge_.editor().setSnapshotParameterPolicy(
            bank->bankId,
            std::move(policy));
        if (!result.ok())
        {
            announce(juceString(result.message));
            return;
        }
        finishSnapshotAuthoringEdit("Updated snapshot random range");
    }

    void showMoreMenu()
    {
        const auto* bank = selectedBank();
        if (bank == nullptr)
            return;
        const auto snapshotIndex = std::ranges::find(
            bank->snapshots,
            selectedSnapshotId_,
            &talas::project::ProjectSnapshot::snapshotId);
        const auto hasSnapshot = snapshotIndex != bank->snapshots.end();
        juce::PopupMenu menu;
        menu.addItem(1, "Capture Current", hasSnapshot);
        menu.addItem(2, "Rename Snapshot", hasSnapshot);
        menu.addItem(3, "Clone Snapshot", hasSnapshot);
        menu.addItem(4, "Initialize Snapshot");
        menu.addItem(5, "Move Snapshot Up",
                     hasSnapshot && snapshotIndex != bank->snapshots.begin());
        menu.addItem(6, "Move Snapshot Down",
                     hasSnapshot
                         && std::next(snapshotIndex) != bank->snapshots.end());
        menu.addItem(7, "Delete Snapshot", hasSnapshot);
        menu.addSeparator();
        juce::PopupMenu newBankMenu;
        for (std::size_t index = 0;
             index < projection_.captureScopes.size();
             ++index)
        {
            const auto& option = projection_.captureScopes[index];
            newBankMenu.addItem(
                static_cast<int>(100 + index),
                juceString(option.displayName),
                true,
                selectedCaptureScope_.has_value()
                    && option.scope == *selectedCaptureScope_);
        }
        menu.addSubMenu(
            "New Snapshot Bank",
            newBankMenu,
            !projection_.captureScopes.empty());
        menu.addItem(21, "Rename Bank");
        menu.addItem(22, "Clone Bank");
        const auto& banks =
            bridge_.editor().document().snapshotLibrary.banks;
        const auto bankIndex = std::ranges::find(
            banks,
            bank->bankId,
            &talas::project::ProjectSnapshotBank::bankId);
        menu.addItem(23, "Move Bank Up",
                     bankIndex != banks.begin());
        menu.addItem(24, "Move Bank Down",
                     std::next(bankIndex) != banks.end());
        menu.addItem(25, "Delete Bank");
        juce::Component::SafePointer<SnapshotPerformanceSurface>
            safeThis(this);
        menu.showMenuAsync(
            juce::PopupMenu::Options{}.withTargetComponent(moreButton_),
            [safeThis](int result) {
                if (safeThis != nullptr)
                    safeThis->handleMoreMenu(result);
            });
    }

    void handleMoreMenu(int result)
    {
        if (result >= 100
            && static_cast<std::size_t>(result - 100)
                < projection_.captureScopes.size())
        {
            selectedCaptureScope_ = projection_.captureScopes[
                static_cast<std::size_t>(result - 100)].scope;
            createBank();
            return;
        }
        switch (result)
        {
        case 1:
            captureSelectedSnapshot();
            break;
        case 2:
            beginInlineEdit(InlineEdit::RenameSnapshot);
            break;
        case 3:
            beginInlineEdit(InlineEdit::CloneSnapshot);
            break;
        case 4:
            initializeSnapshot();
            break;
        case 5:
            moveSnapshot(-1);
            break;
        case 6:
            moveSnapshot(1);
            break;
        case 7:
            beginDelete(DeleteConfirmation::Snapshot);
            break;
        case 21:
            beginInlineEdit(InlineEdit::RenameBank);
            break;
        case 22:
            beginInlineEdit(InlineEdit::CloneBank);
            break;
        case 23:
            moveBank(-1);
            break;
        case 24:
            moveBank(1);
            break;
        case 25:
            beginDelete(DeleteConfirmation::Bank);
            break;
        default:
            break;
        }
    }

    void beginInlineEdit(InlineEdit edit)
    {
        inlineEdit_ = edit;
        deleteConfirmation_ = DeleteConfirmation::None;
        juce::String text;
        const auto* bank = selectedBank();
        if (bank != nullptr)
        {
            if (edit == InlineEdit::RenameBank
                || edit == InlineEdit::CloneBank)
            {
                text = juceString(bank->displayName);
            }
            else
            {
                const auto snapshot = std::ranges::find(
                    bank->snapshots,
                    selectedSnapshotId_,
                    &talas::project::ProjectSnapshot::snapshotId);
                if (snapshot != bank->snapshots.end())
                    text = juceString(snapshot->displayName);
            }
        }
        inlineLabel_.setText(
            edit == InlineEdit::RenameSnapshot
                ? "Rename selected snapshot"
                : edit == InlineEdit::CloneSnapshot
                ? "Name the cloned snapshot"
                : edit == InlineEdit::RenameBank
                ? "Rename selected bank"
                : "Name the cloned bank",
            juce::dontSendNotification);
        inlineEditor_.setText(text, false);
        setBankControlsVisible(true);
        resized();
        inlineEditor_.grabKeyboardFocus();
        inlineEditor_.selectAll();
        if (onPreferredHeightChanged)
            onPreferredHeightChanged();
    }

    void cancelInlineEdit()
    {
        inlineEdit_ = InlineEdit::None;
        setBankControlsVisible(true);
        resized();
        if (onPreferredHeightChanged)
            onPreferredHeightChanged();
    }

    void commitInlineEdit()
    {
        const auto name = inlineEditor_.getText().trim().toStdString();
        const auto* bank = selectedBank();
        if (name.empty() || bank == nullptr)
        {
            announce("A non-empty display name is required");
            return;
        }
        talas::project::EditResult result;
        std::string nextBank = selectedBankId_;
        std::string nextSnapshot = selectedSnapshotId_;
        switch (inlineEdit_)
        {
        case InlineEdit::RenameSnapshot:
            result = bridge_.editor().renameSnapshot(
                bank->bankId,
                selectedSnapshotId_,
                name);
            break;
        case InlineEdit::CloneSnapshot:
            nextSnapshot = uniqueSnapshotId(*bank);
            result = bridge_.editor().cloneSnapshot(
                bank->bankId,
                selectedSnapshotId_,
                nextSnapshot,
                name);
            break;
        case InlineEdit::RenameBank:
            result = bridge_.editor().renameSnapshotBank(
                bank->bankId,
                name);
            break;
        case InlineEdit::CloneBank:
            nextBank = uniqueBankId();
            result = bridge_.editor().cloneSnapshotBank(
                bank->bankId,
                nextBank,
                name);
            nextSnapshot.clear();
            break;
        case InlineEdit::None:
            return;
        }
        if (!result.ok())
        {
            announce(juceString(result.message));
            return;
        }
        selectedBankId_ = nextBank;
        selectedSnapshotId_ = nextSnapshot;
        inlineEdit_ = InlineEdit::None;
        finishSnapshotAuthoringEdit("Updated snapshot library");
    }

    void initializeSnapshot()
    {
        const auto* bank = selectedBank();
        if (bank == nullptr)
            return;
        const auto id = uniqueSnapshotId(*bank);
        if (id.empty())
        {
            announce("Snapshot capacity reached for this bank");
            return;
        }
        const auto result = bridge_.editor().initializeSnapshot(
            bank->bankId,
            id,
            "Initialized " + std::to_string(bank->snapshots.size() + 1));
        if (!result.ok())
        {
            announce(juceString(result.message));
            return;
        }
        selectedSnapshotId_ = id;
        finishSnapshotAuthoringEdit("Initialized snapshot from scope defaults");
    }

    void moveSnapshot(int delta)
    {
        const auto* bank = selectedBank();
        if (bank == nullptr)
            return;
        const auto found = std::ranges::find(
            bank->snapshots,
            selectedSnapshotId_,
            &talas::project::ProjectSnapshot::snapshotId);
        if (found == bank->snapshots.end())
            return;
        const auto index = static_cast<std::ptrdiff_t>(
            std::distance(bank->snapshots.begin(), found));
        const auto destination = index + delta;
        if (destination < 0
            || destination
                >= static_cast<std::ptrdiff_t>(bank->snapshots.size()))
        {
            return;
        }
        const auto result = bridge_.editor().moveSnapshot(
            bank->bankId,
            selectedSnapshotId_,
            static_cast<std::size_t>(destination));
        if (!result.ok())
        {
            announce(juceString(result.message));
            return;
        }
        finishSnapshotAuthoringEdit("Reordered snapshot");
    }

    void moveBank(int delta)
    {
        const auto& banks =
            bridge_.editor().document().snapshotLibrary.banks;
        const auto found = std::ranges::find(
            banks,
            selectedBankId_,
            &talas::project::ProjectSnapshotBank::bankId);
        if (found == banks.end())
            return;
        const auto index = static_cast<std::ptrdiff_t>(
            std::distance(banks.begin(), found));
        const auto destination = index + delta;
        if (destination < 0
            || destination
                >= static_cast<std::ptrdiff_t>(banks.size()))
        {
            return;
        }
        const auto result = bridge_.editor().moveSnapshotBank(
            selectedBankId_,
            static_cast<std::size_t>(destination));
        if (!result.ok())
        {
            announce(juceString(result.message));
            return;
        }
        finishSnapshotAuthoringEdit("Reordered snapshot bank");
    }

    void beginDelete(DeleteConfirmation confirmation)
    {
        deleteConfirmation_ = confirmation;
        inlineEdit_ = InlineEdit::None;
        deleteLabel_.setText(
            confirmation == DeleteConfirmation::Snapshot
                ? "Delete selected snapshot? This project edit is undoable."
                : "Delete selected bank? This project edit is undoable.",
            juce::dontSendNotification);
        setBankControlsVisible(true);
        resized();
        deleteCancelButton_.grabKeyboardFocus();
        if (onPreferredHeightChanged)
            onPreferredHeightChanged();
    }

    void cancelDelete()
    {
        deleteConfirmation_ = DeleteConfirmation::None;
        setBankControlsVisible(true);
        resized();
        if (onPreferredHeightChanged)
            onPreferredHeightChanged();
    }

    void confirmDelete()
    {
        const auto* bank = selectedBank();
        if (bank == nullptr)
            return;
        talas::project::EditResult result;
        if (deleteConfirmation_ == DeleteConfirmation::Snapshot)
        {
            result = bridge_.editor().removeSnapshot(
                bank->bankId,
                selectedSnapshotId_);
            selectedSnapshotId_.clear();
        }
        else if (deleteConfirmation_ == DeleteConfirmation::Bank)
        {
            result = bridge_.editor().removeSnapshotBank(
                bank->bankId);
            selectedBankId_.clear();
            selectedSnapshotId_.clear();
        }
        else
        {
            return;
        }
        if (!result.ok())
        {
            announce(juceString(result.message));
            return;
        }
        deleteConfirmation_ = DeleteConfirmation::None;
        finishSnapshotAuthoringEdit("Deleted snapshot library item");
    }

    void finishSnapshotAuthoringEdit(const juce::String& message)
    {
        actionDiagnostics_.clear();
        const auto adopted = bridge_.adoptSnapshotAuthoringEdit();
        onDocumentChange_(adopted.ok());
        if (!adopted.ok())
        {
            actionDiagnostics_ = adopted.diagnostics;
            announce(message +
                     juceString(" · press Play to stage the edited project before "
                                "live snapshot actions"));
        }
        else
        {
            announce(message);
        }
        refresh();
    }

    void handleActionResult(
        const talas::runtime::SnapshotPerformanceActionResult& result,
        const juce::String& success)
    {
        actionDiagnostics_ = result.diagnostics;
        if (result.ok())
        {
            if (success.isNotEmpty())
                announce(success);
        }
        else
        {
            announce(
                result.message.empty()
                    ? juce::String{"Snapshot action failed"}
                    : juceString(result.message));
        }
        refresh();
    }

    void announce(const juce::String& message)
    {
        statusLabel_.setText(message, juce::dontSendNotification);
        onStatus_(message);
    }

    talas::runtime::ProjectRuntimeBridge& bridge_;
    const talas::core::NodeRegistry& registry_;
    DocumentChangeCallback onDocumentChange_;
    StatusCallback onStatus_;
    SnapshotRows snapshotRows_;
    PolicyRows policyRows_;
    talas::app::SnapshotWorkbenchProjection projection_;
    std::vector<talas::project::SnapshotDiagnostic> actionDiagnostics_;
    talas::app::SnapshotWorkbenchCaptureContext captureContext_;
    std::string selectedBankId_;
    std::string selectedSnapshotId_;
    std::string morphLeftSnapshotId_;
    std::string morphRightSnapshotId_;
    std::optional<talas::project::SnapshotScopeIdentity>
        selectedCaptureScope_;
    juce::Label scopeLabel_;
    juce::ComboBox captureScopeBox_;
    juce::ComboBox bankBox_;
    juce::Label statusLabel_;
    juce::Label emptyTitle_;
    juce::Label emptyDescription_;
    juce::TextButton createBankButton_;
    juce::ListBox snapshotList_;
    juce::TextButton recallButton_;
    juce::TextButton storeButton_;
    juce::TextButton moreButton_;
    juce::Label morphTitle_;
    juce::ComboBox morphLeftBox_;
    juce::ComboBox morphRightBox_;
    juce::Slider morphSlider_;
    juce::Label smoothingCaption_;
    juce::TextEditor smoothingEditor_;
    juce::Label randomTitle_;
    juce::Label seedCaption_;
    juce::TextEditor seedEditor_;
    juce::TextButton randomizeButton_;
    juce::Slider mergeSlider_;
    juce::TextButton mergeButton_;
    juce::TextButton policyToggle_;
    PolicyListBox policyList_;
    juce::TextButton lockButton_;
    juce::TextEditor rangeMinimum_;
    juce::TextEditor rangeMaximum_;
    juce::TextButton applyRangeButton_;
    juce::Label inlineLabel_;
    juce::TextEditor inlineEditor_;
    juce::TextButton inlineCommitButton_;
    juce::TextButton inlineCancelButton_;
    juce::Label deleteLabel_;
    juce::TextButton deleteConfirmButton_;
    juce::TextButton deleteCancelButton_;
    InlineEdit inlineEdit_ = InlineEdit::None;
    DeleteConfirmation deleteConfirmation_ =
        DeleteConfirmation::None;
    double pendingMorphAmount_ = 0.0;
    bool policiesExpanded_ = false;
    bool morphGestureActive_ = false;
    bool morphUpdatePending_ = false;
    bool standaloneMorphPending_ = false;
    bool lastTransitionActive_ = false;
    bool lastTransitionCoalesced_ = false;
    bool refreshing_ = false;
};

class PerformancePanelImpl final : public PerformancePanel
{
private:
    class ControlRow final : public juce::Component
    {
      public:
        using CommitCallback =
            std::function<void(const talas::app::SoundPerformanceControlIdentity&,
                talas::project::ProjectParameterExpression)>;
        using StructureFocusCallback = std::function<void(
            const talas::app::SoundPerformanceControlIdentity&)>;
        using CanvasCallback = std::function<void(
            const talas::app::SoundPerformanceControlIdentity&,
            std::string_view)>;

        ControlRow(talas::app::SoundPerformanceControl control,
            CommitCallback onCommit,
            StructureFocusCallback onStructureFocus,
            CanvasCallback onCanvas,
            bool canvasEligible)
            : control_(std::move(control)), onCommit_(std::move(onCommit)),
              onStructureFocus_(std::move(onStructureFocus)),
              onCanvas_(std::move(onCanvas)),
              applyButton_("Apply"), structureButton_("Structure"),
              canvasButton_("Canvas")
        {
            nameLabel_.setText(
                juceString(control_.displayName),
                juce::dontSendNotification);
            nameLabel_.setColour(
                juce::Label::textColourId,
                textColour);
            nameLabel_.setFont(WorkbenchType::bodyStrong());
            identityLabel_.setText(
                juceString(control_.identity.instanceId)
                    + " / "
                    + juceString(control_.identity.parameterId),
                juce::dontSendNotification);
            identityLabel_.setColour(
                juce::Label::textColourId,
                secondaryTextColour);
            identityLabel_.setFont(WorkbenchType::micro());
            addAndMakeVisible(nameLabel_);
            addAndMakeVisible(identityLabel_);
            addAndMakeVisible(textValue_);
            addAndMakeVisible(choiceValue_);
            addAndMakeVisible(booleanValue_);
            addAndMakeVisible(applyButton_);
            addAndMakeVisible(structureButton_);
            addAndMakeVisible(canvasButton_);

            describeControl(
                *this,
                juceString(control_.displayName)
                    + " Performance control",
                "Stable control "
                    + juceString(control_.identity.instanceId)
                    + "."
                    + juceString(control_.identity.parameterId)
                    + "; editing it writes one sparse Sound-instance override.");
            const auto stableIdentity =
                juceString(control_.identity.instanceId)
                + "."
                + juceString(control_.identity.parameterId);
            const auto valueHelp =
                "Performance value for stable control "
                + stableIdentity
                + ".";
            describeControl(
                textValue_,
                juceString(control_.displayName) + " value",
                valueHelp);
            describeControl(
                choiceValue_,
                juceString(control_.displayName) + " choice",
                valueHelp);
            describeControl(
                booleanValue_,
                juceString(control_.displayName) + " toggle",
                valueHelp);
            describeControl(
                applyButton_,
                "Apply " + juceString(control_.displayName),
                "Commits the edited value for stable control " +
                    stableIdentity + ".");
            describeControl(structureButton_,
                "Show " + juceString(control_.displayName) + " in Structure",
                "Selects the exact owning Sound instance and exposed "
                "parameter without changing project topology.");
            describeControl(canvasButton_,
                "Add " + juceString(control_.displayName)
                    + " to Performance canvas",
                "Adds one portable control for this exact concrete Sound-instance parameter without changing Structure.");
            canvasButton_.setEnabled(canvasEligible);
            textValue_.setFont(WorkbenchType::body());
            textValue_.onReturnKey = [this] { commit(); };
            choiceValue_.onChange = [this] { commit(); };
            booleanValue_.onClick = [this] { commit(); };
            applyButton_.onClick = [this] { commit(); };
            structureButton_.onClick = [this]
            {
                if (onStructureFocus_)
                    onStructureFocus_(control_.identity);
            };
            canvasButton_.onClick = [this]
            {
                if (onCanvas_)
                    onCanvas_(control_.identity, control_.displayName);
            };
            refreshValue();
        }

        [[nodiscard]] bool hasAccessibleEditorControls() const
        {
            return textValue_.getTitle().isNotEmpty() &&
                   choiceValue_.getTitle().isNotEmpty() &&
                   booleanValue_.getTitle().isNotEmpty() &&
                   applyButton_.getTitle().isNotEmpty() &&
                   structureButton_.getTitle().isNotEmpty() &&
                   canvasButton_.getTitle().isNotEmpty() &&
                   textValue_.getDescription().contains(
                       juceString(control_.identity.parameterId));
        }

        void paint(juce::Graphics& graphics) override
        {
            graphics.setColour(workbenchTheme.controlTile);
            graphics.fillRoundedRectangle(
                getLocalBounds().toFloat(),
                6.0F);
            graphics.setColour(panelBorderColour);
            graphics.drawRoundedRectangle(
                getLocalBounds().toFloat().reduced(0.5F),
                6.0F,
                1.0F);
        }

        void resized() override
        {
            auto bounds = getLocalBounds().reduced(7, 5);
            auto labels = bounds.removeFromTop(26);
            nameLabel_.setBounds(labels.removeFromTop(15));
            identityLabel_.setBounds(labels);
            bounds.removeFromTop(3);
            auto value = bounds.removeFromTop(24);
            canvasButton_.setBounds(value.removeFromRight(58));
            value.removeFromRight(4);
            structureButton_.setBounds(value.removeFromRight(72));
            value.removeFromRight(4);
            if (applyButton_.isVisible())
            {
                applyButton_.setBounds(value.removeFromRight(54));
                value.removeFromRight(4);
            }
            else
            {
                applyButton_.setBounds({});
            }
            textValue_.setBounds(value);
            choiceValue_.setBounds(value);
            booleanValue_.setBounds(value);
        }

    private:
        void refreshValue()
        {
            textValue_.setVisible(false);
            choiceValue_.setVisible(false);
            booleanValue_.setVisible(false);
            applyButton_.setVisible(false);
            switch (control_.kind)
            {
            case talas::core::ParameterKind::Expression:
                textValue_.setText(
                    juceString(control_.value.expression.value_or("")),
                    juce::dontSendNotification);
                textValue_.setVisible(true);
                applyButton_.setVisible(true);
                break;
            case talas::core::ParameterKind::Resource:
                textValue_.setText(
                    juceString(control_.value.resource.value_or("")),
                    juce::dontSendNotification);
                textValue_.setVisible(true);
                applyButton_.setVisible(true);
                break;
            case talas::core::ParameterKind::Text:
                textValue_.setText(
                    juceString(control_.value.text.value_or("")),
                    juce::dontSendNotification);
                textValue_.setVisible(true);
                applyButton_.setVisible(true);
                break;
            case talas::core::ParameterKind::Choice:
                choiceValue_.clear(juce::dontSendNotification);
                for (std::size_t index = 0;
                     index < control_.choices.size();
                     ++index)
                {
                    choiceValue_.addItem(
                        juceString(control_.choices[index]),
                        static_cast<int>(index + 1));
                }
                choiceValue_.setText(
                    juceString(control_.value.choice.value_or("")),
                    juce::dontSendNotification);
                choiceValue_.setVisible(true);
                break;
            case talas::core::ParameterKind::Boolean:
                booleanValue_.setButtonText(
                    juceString(control_.displayName));
                booleanValue_.setToggleState(
                    control_.value.boolean.value_or(false),
                    juce::dontSendNotification);
                booleanValue_.setVisible(true);
                break;
            }
            resized();
        }

        void commit()
        {
            talas::project::ProjectParameterExpression value;
            value.parameterId = control_.identity.parameterId;
            switch (control_.kind)
            {
            case talas::core::ParameterKind::Expression:
                value.expression = textValue_.getText().toStdString();
                break;
            case talas::core::ParameterKind::Resource:
                value.resource = textValue_.getText().toStdString();
                break;
            case talas::core::ParameterKind::Text:
                value.text = textValue_.getText().toStdString();
                break;
            case talas::core::ParameterKind::Choice:
                value.choice = choiceValue_.getText().toStdString();
                break;
            case talas::core::ParameterKind::Boolean:
                value.boolean = booleanValue_.getToggleState();
                break;
            }
            onCommit_(control_.identity, std::move(value));
        }

        talas::app::SoundPerformanceControl control_;
        CommitCallback onCommit_;
        StructureFocusCallback onStructureFocus_;
        CanvasCallback onCanvas_;
        juce::Label nameLabel_;
        juce::Label identityLabel_;
        juce::TextEditor textValue_;
        juce::ComboBox choiceValue_;
        juce::ToggleButton booleanValue_;
        juce::TextButton applyButton_;
        juce::TextButton structureButton_;
        juce::TextButton canvasButton_;
    };

    class LiveGridCellButton final : public juce::TextButton
    {
      public:
        using ActivateCallback =
            std::function<void(std::string_view, std::string_view)>;

        LiveGridCellButton(
            talas::app::LiveGridCellAction action,
            ActivateCallback onActivate)
            : action_(std::move(action)),
              onActivate_(std::move(onActivate))
        {
            setClickingTogglesState(false);
            setWantsKeyboardFocus(true);
            onClick = [this] {
                if (onActivate_)
                    onActivate_(action_.liveSetId, action_.cellId);
            };
            refreshPresentation();
        }

        [[nodiscard]] const talas::app::LiveGridCellAction& action() const noexcept
        {
            return action_;
        }

        void setAction(talas::app::LiveGridCellAction action)
        {
            action_ = std::move(action);
            refreshPresentation();
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            if (isEnabled()
                && (key == juce::KeyPress::returnKey
                    || key == juce::KeyPress::spaceKey))
            {
                if (onActivate_)
                    onActivate_(action_.liveSetId, action_.cellId);
                return true;
            }
            return juce::TextButton::keyPressed(key);
        }

        void performPointerClickForSmoke(
            const juce::MouseEvent& down,
            const juce::MouseEvent& up)
        {
            mouseDown(down);
            mouseUp(up);
        }

        // A glass tile coloured by what the cell launches (a Sound violet, a
        // snapshot aqua, a Score region pink, a stop or mute red), lit while
        // active or queued; its name and state written out, never colour alone.
        void paintButton(juce::Graphics& graphics, bool highlighted, bool down) override
        {
            const auto& summary = action_.targetSummary;
            const auto kind = summary.starts_with("Sound") ? workbenchTheme.accentStart
                : summary.starts_with("Snapshot")         ? workbenchTheme.accentEnd
                : summary.starts_with("Score")            ? workbenchTheme.event
                                                          : workbenchTheme.error;
            const auto lit = getToggleState();
            const auto alpha = isEnabled() ? 1.0F : 0.55F;
            const auto bounds = getLocalBounds().toFloat().reduced(1.5F);
            auto base = workbenchTheme.controlTile.interpolatedWith(kind, lit ? 0.42F : 0.12F);
            if (down)
                base = base.darker(0.2F);
            else if (highlighted)
                base = base.brighter(0.15F);
            if (lit && !WorkbenchStyle::enhancedContrast())
            {
                graphics.setColour(kind.withAlpha(0.3F));
                graphics.fillRoundedRectangle(bounds.expanded(1.0F), 7.0F);
            }
            WorkbenchStyle::paintTile(graphics, bounds, 6.0F, base.withMultipliedAlpha(alpha));
            graphics.setColour(kind.withMultipliedAlpha(alpha));
            graphics.fillRoundedRectangle(bounds.withWidth(3.0F).reduced(0.0F, 5.0F), 1.5F);
            auto text = bounds.reduced(10.0F, 6.0F).toNearestInt();
            graphics.setColour(workbenchTheme.primaryText.withMultipliedAlpha(alpha));
            graphics.setFont(WorkbenchType::labelStrong());
            graphics.drawFittedText(juceString(action_.displayName), text.removeFromTop(text.getHeight() / 2),
                juce::Justification::centredLeft, 1);
            graphics.setColour((lit ? workbenchTheme.primaryText : workbenchTheme.secondaryText).withMultipliedAlpha(alpha));
            graphics.setFont(WorkbenchType::micro());
            graphics.drawFittedText(juceString(talas::app::liveGridCellStateLabel(action_.state)), text,
                juce::Justification::centredLeft, 1);
            if (hasKeyboardFocus(true))
            {
                graphics.setColour(workbenchTheme.focus);
                graphics.drawRoundedRectangle(bounds, 6.0F, WorkbenchType::focusOutlinePixels());
            }
        }

      private:
        void refreshPresentation()
        {
            const auto state = talas::app::liveGridCellStateLabel(action_.state);
            setButtonText(
                juceString(action_.displayName) + "\n" + juceString(state));
            setComponentID(juceString(action_.componentId));
            setTitle(
            "Live Grid " + juceString(action_.displayName)
                + juceString(" · ")
                + juceString(state));
            setDescription(
                juceString(action_.targetSummary) + ". "
                + juceString(action_.quantizationSummary) + ". "
                + juceString(action_.mappingSummary) + ". "
                + juceString(action_.reason));
            setEnabled(action_.canActivate);
            setToggleState(
                action_.state == talas::app::LiveGridCellState::Active
                    || action_.state == talas::app::LiveGridCellState::Queued,
                juce::dontSendNotification);
        }

        talas::app::LiveGridCellAction action_;
        ActivateCallback onActivate_;
    };

public:
    using DocumentChangeCallback = std::function<void(bool)>;
    using StatusCallback = std::function<void(const juce::String&)>;

    PerformancePanelImpl(
        talas::runtime::ProjectRuntimeBridge& bridge,
        const talas::core::NodeRegistry& registry,
        DocumentChangeCallback onDocumentChange,
        StatusCallback onStatus,
        StructureFocusCallback onStructureFocus,
        CanvasSelectionCallback onCanvasSelection,
        talas::project::WorkspacePerformanceSessionState& session,
        SessionChangeCallback onSessionChange,
        MidiSourceIdentifiersCallback midiSourceIdentifiers,
        OscAvailabilityCallback oscAvailable)
        : bridge_(bridge), onDocumentChange_(std::move(onDocumentChange)),
          onStatus_(std::move(onStatus)),
          onStructureFocus_(std::move(onStructureFocus)),
          onCanvasSelection_(std::move(onCanvasSelection)),
          session_(session), onSessionChange_(std::move(onSessionChange)),
          midiSourceIdentifiers_(std::move(midiSourceIdentifiers)),
          oscAvailable_(std::move(oscAvailable)),
          controlsTab_("Controls"), snapshotsTab_("Snapshots"),
          liveGridTab_("Live Grid"),
          snapshotSurface_(bridge, registry, onDocumentChange_, onStatus_),
          stopLiveGridButton_("Stop")
    {
        describeControl(
            *this,
            "Performance Panel",
            "Controls the selected reusable Sound instance through "
            "stable exposed parameter identities.");
        title_.setText("Performance", juce::dontSendNotification);
        title_.setColour(juce::Label::textColourId, textColour);
        title_.setFont(WorkbenchType::panelTitle());
        contextLabel_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        contextLabel_.setFont(WorkbenchType::label());
        emptyLabel_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        emptyLabel_.setFont(WorkbenchType::body());
        emptyLabel_.setJustificationType(
            juce::Justification::centred);
        emptyLabel_.setText(
            "Select a reusable Sound instance in Structure to expose its stable Performance controls.",
            juce::dontSendNotification);
        addAndMakeVisible(title_);
        addAndMakeVisible(controlsTab_);
        addAndMakeVisible(snapshotsTab_);
        addAndMakeVisible(liveGridTab_);
        addAndMakeVisible(contextLabel_);
        addAndMakeVisible(emptyLabel_);
        viewport_.setViewedComponent(&rowsContainer_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
        snapshotViewport_.setViewedComponent(
            &snapshotSurface_,
            false);
        snapshotViewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(snapshotViewport_);
        liveGridViewport_.setViewedComponent(&liveGridContainer_, false);
        liveGridViewport_.setScrollBarsShown(true, true);
        addAndMakeVisible(liveGridViewport_);
        addAndMakeVisible(liveGridSetSelector_);
        addAndMakeVisible(stopLiveGridButton_);
        addAndMakeVisible(liveToolSelector_);
        addAndMakeVisible(liveToolStateLabel_);
        addAndMakeVisible(liveToolManualButton_);
        addAndMakeVisible(liveToolVariableSelector_);
        addAndMakeVisible(liveToolVariableValue_);
        addAndMakeVisible(liveToolVariableToggle_);
        addAndMakeVisible(liveToolVariableApply_);
        selectedItemTitle_.setFont(WorkbenchType::bodyStrong());
        selectedItemTitle_.setColour(
            juce::Label::textColourId, textColour);
        selectedItemIdentity_.setFont(WorkbenchType::micro());
        selectedItemIdentity_.setColour(
            juce::Label::textColourId, secondaryTextColour);
        selectedItemName_.setFont(WorkbenchType::body());
        selectedItemKind_.addItem("Knob", 1);
        selectedItemKind_.addItem("Fader", 2);
        selectedItemKind_.addItem("Gate", 3);
        selectedItemKind_.addItem("Toggle", 4);
        selectedItemKind_.addItem("Macro", 5);
        selectedItemVisible_.setButtonText("Visible on canvas");
        selectedItemApply_.setButtonText("Apply");
        selectedItemRemove_.setButtonText("Hide");
        selectedItemStructure_.setButtonText("Structure");
        modulationTitle_.setText(
            "Macro modulation", juce::dontSendNotification);
        modulationTitle_.setFont(WorkbenchType::label());
        modulationTitle_.setColour(
            juce::Label::textColourId, secondaryTextColour);
        modulationDepth_.setText("0.25", false);
        modulationAdd_.setButtonText("Add");
        modulationClear_.setButtonText("Clear");
        modulationSummary_.setFont(WorkbenchType::micro());
        modulationSummary_.setColour(
            juce::Label::textColourId, secondaryTextColour);
        modulationSummary_.setJustificationType(
            juce::Justification::topLeft);
        for (auto* component : std::array<juce::Component*, 13>{
                 &selectedItemTitle_,
                 &selectedItemIdentity_,
                 &selectedItemName_,
                 &selectedItemKind_,
                 &selectedItemVisible_,
                 &selectedItemApply_,
                 &selectedItemRemove_,
                 &selectedItemStructure_,
                 &modulationTitle_,
                 &modulationTarget_,
                 &modulationDepth_,
                 &modulationAdd_,
                 &modulationClear_})
        {
            rowsContainer_.addAndMakeVisible(component);
        }
        rowsContainer_.addAndMakeVisible(modulationSummary_);
        describeControl(
            selectedItemName_,
            "Selected Performance control name",
            "Edits the portable display name without changing Structure.");
        describeControl(
            selectedItemKind_,
            "Selected Performance control kind",
            "Chooses knob, fader, gate, toggle, or macro presentation.");
        describeControl(
            selectedItemVisible_,
            "Selected Performance control visibility",
            "Shows or hides the selected stable target on the portable canvas.");
        describeControl(
            selectedItemStructure_,
            "Show selected Performance target in Structure",
            "Focuses the exact owning Sound instance and exposed parameter without changing topology.");
        describeControl(
            modulationTarget_,
            "Macro modulation destination",
            "Selects one exact available Performance target.");
        describeControl(
            modulationDepth_,
            "Macro modulation depth",
            "Signed modulation depth from minus one to one.");
        selectedItemApply_.onClick = [this] {
            applySelectedPerformanceControl();
        };
        selectedItemRemove_.onClick = [this] {
            hideOrRemoveSelectedPerformanceItem();
        };
        selectedItemStructure_.onClick = [this] {
            showSelectedPerformanceItemInStructure();
        };
        modulationAdd_.onClick = [this] {
            addSelectedModulationSlot();
        };
        modulationClear_.onClick = [this] {
            clearSelectedModulationSlots();
        };
        controlsTab_.setClickingTogglesState(false);
        snapshotsTab_.setClickingTogglesState(false);
        liveGridTab_.setClickingTogglesState(false);
        controlsTab_.onClick = [this] {
            setPerformanceMode(
                talas::project::WorkspacePerformanceSessionState::Mode::Controls);
        };
        snapshotsTab_.onClick = [this] {
            setPerformanceMode(
                talas::project::WorkspacePerformanceSessionState::Mode::Snapshots);
        };
        liveGridTab_.onClick = [this] {
            setPerformanceMode(
                talas::project::WorkspacePerformanceSessionState::Mode::LiveGrid);
        };
        describeControl(
            controlsTab_,
            "Performance Controls tab",
            "Edit stable exposed parameters on the selected reusable "
            "Sound instance.");
        describeControl(snapshotsTab_,
            "Performance Snapshots tab",
            "Store, recall, morph, randomize, and capture typed snapshot banks.");
        describeControl(liveGridTab_,
            "Performance Live Grid tab",
            "Launches exact prepared Sound, Snapshot, Score-region, and Stop cells through one shared authority.");
        describeControl(liveGridSetSelector_,
            "Selected Live Set",
            "Chooses the one exact Live Set projected into the stable 8 by 16 launch grid.");
        describeControl(stopLiveGridButton_,
            "Stop Live Grid",
            "Dispatches the first exact available Stop cell through the same Live Grid authority.");
        liveToolManualButton_.setButtonText("Trigger");
        liveToolVariableToggle_.setButtonText("Value");
        liveToolVariableApply_.setButtonText("Apply");
        liveToolStateLabel_.setColour(
            juce::Label::textColourId, secondaryTextColour);
        liveToolStateLabel_.setFont(WorkbenchType::micro());
        liveToolVariableValue_.setInputRestrictions(32, "-+.0123456789eE");
        describeControl(liveToolSelector_,
            "Selected Live Tool",
            "Chooses one bounded finite-state Live Tool page from the accepted project definition.");
        describeControl(liveToolStateLabel_,
            "Live Tool state",
            "Shows the exact callback-published state and definition identity for the selected tool.");
        describeControl(liveToolManualButton_,
            "Trigger selected Live Tool",
            "Dispatches one generation-bound manual trigger through the shared realtime Live Tool authority.");
        describeControl(liveToolVariableSelector_,
            "Selected Live Tool variable",
            "Chooses one bounded local numeric or boolean variable in the selected tool.");
        describeControl(liveToolVariableApply_,
            "Apply Live Tool variable",
            "Dispatches one generation-bound variable-change trigger; rejected values leave runtime state unchanged.");
        stopLiveGridButton_.onClick = [this] {
            activateStopCell();
        };
        liveGridSetSelector_.onChange = [this] {
            selectLiveGridSetFromSelector();
        };
        liveToolSelector_.onChange = [this] {
            selectLiveToolFromSelector();
        };
        liveToolVariableSelector_.onChange = [this] {
            refreshSelectedLiveToolVariable();
        };
        liveToolManualButton_.onClick = [this] {
            (void)triggerSelectedLiveTool();
        };
        liveToolVariableApply_.onClick = [this] {
            (void)applySelectedLiveToolVariable();
        };
        setWantsKeyboardFocus(true);
        snapshotSurface_.onPreferredHeightChanged = [this] {
            resized();
        };
        refreshLiveGrid();
        refreshLiveTools();
        updateTabVisibility();
    }

    void setSelectedInstanceId(std::string instanceId) override
    {
        setSelectionContext(
            instanceId,
            talas::app::snapshotWorkbenchCaptureContext(
                bridge_.editor().document(),
                instanceId,
                std::nullopt,
                {}));
    }

    void setSelectionContext(
        std::string selectedInstanceId,
        talas::app::SnapshotWorkbenchCaptureContext captureContext) override
    {
        selectedInstanceId_ = std::move(selectedInstanceId);
        captureContext_ = captureContext;
        snapshotSurface_.setCaptureContext(
            std::move(captureContext));
        refresh();
    }

    void setSelectedPerformanceItem(
        std::optional<std::string> controlId,
        std::optional<std::string> meterId) override
    {
        selectedPerformanceControlId_ = std::move(controlId);
        selectedPerformanceMeterId_ = std::move(meterId);
        refresh();
    }

    void refresh() override
    {
        rows_.clear(true);
        const auto controls = talas::app::soundPerformanceControls(
            bridge_.editor().document(),
            selectedInstanceId_);
        for (auto control : controls)
        {
            const auto canvasEligible = control.kind
                == talas::core::ParameterKind::Expression;
            auto* row = rows_.add(new ControlRow(
                std::move(control),
                [this](const auto& identity, auto value)
                {
                    const auto result =
                        bridge_.editor().setParameter(identity.instanceId,
                            std::move(value));
                    if (!result.ok())
                    {
                        onStatus_(juceString(result.message));
                        refresh();
                        return;
                    }
                    onDocumentChange_(false);
                    onStatus_("Updated Performance control " +
                              juceString(identity.parameterId) + " on " +
                              juceString(identity.instanceId));
                },
                onStructureFocus_,
                [this](const auto& identity, std::string_view displayName) {
                    (void)addSoundControlToCanvas(identity, displayName);
                },
                canvasEligible));
            rowsContainer_.addAndMakeVisible(row);
        }
        refreshSelectedPerformanceItem();
        contextLabel_.setText(
            selectedInstanceId_.empty()
                ? juce::String{"No Sound instance selected"}
                : "Instance / " + juceString(selectedInstanceId_),
            juce::dontSendNotification);
        refreshLiveGrid();
        refreshLiveTools();
        updateTabVisibility();
    }

    void refreshRuntimeState() override
    {
        snapshotSurface_.refreshRuntimeState();
        refreshLiveGrid();
        refreshLiveTools();
    }

    void synchronizeLiveToolSessionState() override
    {
        refreshLiveTools();
    }

    [[nodiscard]] int controlCount() const noexcept override
    {
        return rows_.size();
    }

    [[nodiscard]] bool controlsHaveAccessibleEditors() const override
    {
        return std::ranges::all_of(
            rows_,
            [](const auto* row) {
                return row != nullptr
                    && row->hasAccessibleEditorControls();
            });
    }

    [[nodiscard]] bool
    selectedPerformanceItemEditorAccessibleForSmoke() const override
    {
        return selectedItemEditorVisible_
            && selectedItemTitle_.isVisible()
            && selectedItemName_.getTitle().isNotEmpty()
            && selectedItemKind_.getTitle().isNotEmpty()
            && selectedItemVisible_.getTitle().isNotEmpty()
            && selectedItemIdentity_.getText().isNotEmpty();
    }

    [[nodiscard]] bool
    addFirstSoundControlToCanvasForSmoke() override
    {
        const auto controls = talas::app::soundPerformanceControls(
            bridge_.editor().document(), selectedInstanceId_);
        const auto eligible = std::ranges::find_if(
            controls, [](const auto& control) {
                return control.kind == talas::core::ParameterKind::Expression;
            });
        return eligible != controls.end()
            && addSoundControlToCanvas(
                eligible->identity, eligible->displayName);
    }

    [[nodiscard]] bool focusFirstControlInStructureForSmoke() override
    {
        const auto controls =
            talas::app::soundPerformanceControls(bridge_.editor().document(),
                selectedInstanceId_);
        if (controls.empty() || !onStructureFocus_)
            return false;
        onStructureFocus_(controls.front().identity);
        return true;
    }

    [[nodiscard]] bool snapshotWorkflowIsAccessible() const override
    {
        return snapshotSurface_.hasAccessibleSnapshotWorkflow();
    }

    void showSnapshotsForSmoke() override
    {
        setPerformanceMode(
            talas::project::WorkspacePerformanceSessionState::Mode::Snapshots);
    }

    [[nodiscard]] bool snapshotEmptyStateIsUsableForSmoke() const override
    {
        return session_.mode
                    == talas::project::WorkspacePerformanceSessionState::Mode::Snapshots
            && snapshotViewport_.isVisible()
            && !snapshotViewport_.getBounds().isEmpty()
            && snapshotSurface_.emptyStateIsUsableForSmoke();
    }

    [[nodiscard]] bool snapshotOffersDefinitionScopeForSmoke() const override
    {
        return snapshotSurface_.offersDefinitionCaptureScopeForSmoke();
    }

    [[nodiscard]] bool snapshotDefinitionScopeIsSelectableForSmoke() override
    {
        return snapshotSurface_.selectDefinitionCaptureScopeForSmoke();
    }

    [[nodiscard]] bool snapshotOffersExactNestedNodeScopeForSmoke() const override
    {
        return snapshotSurface_.offersExactNestedNodeScopeForSmoke();
    }

    [[nodiscard]] std::optional<std::string>
    runSnapshotWorkflowForSmoke() override
    {
        showSnapshotsForSmoke();
        return snapshotSurface_.runCompleteWorkflowForSmoke();
    }

    [[nodiscard]] bool snapshotPopulatedStateIsUsableForSmoke() const override
    {
        return session_.mode
                    == talas::project::WorkspacePerformanceSessionState::Mode::Snapshots
            && snapshotViewport_.isVisible()
            && snapshotViewport_.getWidth() >= 220
            && snapshotViewport_.getHeight() >= 240
            && snapshotSurface_.populatedStateIsUsableForSmoke();
    }

    void showLiveGridForSmoke() override
    {
        setPerformanceMode(
            talas::project::WorkspacePerformanceSessionState::Mode::LiveGrid);
    }

    [[nodiscard]] bool liveGridWorkflowIsAccessibleForSmoke() const override
    {
        return session_.mode
                == talas::project::WorkspacePerformanceSessionState::Mode::LiveGrid
            && liveGridViewport_.isVisible()
            && !liveGridViewport_.getBounds().isEmpty()
            && liveGridSetSelector_.isVisible()
            && liveGridSetSelector_.getNumItems() >= 2
            && liveGridSetSelector_.getTitle().isNotEmpty()
            && stopLiveGridButton_.isVisible()
            && stopLiveGridButton_.getTitle().isNotEmpty()
            && std::ranges::all_of(
                liveGridCells_, [](const auto* cell) {
                    return cell != nullptr
                        && cell->getComponentID().isNotEmpty()
                        && cell->getTitle().isNotEmpty()
                        && cell->getDescription().isNotEmpty();
                })
            && liveGridCellsOccupyUniquePositions();
    }

    [[nodiscard]] bool activateFirstLiveGridCellForSmoke() override
    {
        if (liveGridCells_.isEmpty())
            return false;
        auto* first = liveGridCells_.getFirst();
        if (first == nullptr || !first->isEnabled())
            return false;
        const auto point = first->getLocalBounds().getCentre().toFloat();
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent down{
            juce::Desktop::getInstance().getMainMouseSource(),
            point,
            juce::ModifierKeys::leftButtonModifier,
            juce::MouseInputSource::defaultPressure,
            juce::MouseInputSource::defaultOrientation,
            juce::MouseInputSource::defaultRotation,
            juce::MouseInputSource::defaultTiltX,
            juce::MouseInputSource::defaultTiltY,
            first,
            first,
            now,
            point,
            now,
            1,
            false,
        };
        const juce::MouseEvent up{
            juce::Desktop::getInstance().getMainMouseSource(),
            point,
            {},
            juce::MouseInputSource::defaultPressure,
            juce::MouseInputSource::defaultOrientation,
            juce::MouseInputSource::defaultRotation,
            juce::MouseInputSource::defaultTiltX,
            juce::MouseInputSource::defaultTiltY,
            first,
            first,
            now,
            point,
            now,
            1,
            false,
        };
        first->performPointerClickForSmoke(down, up);
        return true;
    }

    [[nodiscard]] bool activateSelectedLiveGridCellWithNativeKeyForSmoke()
        override
    {
        const auto selected = selectedLiveGridButton();
        if (selected == nullptr)
            return false;
        if (selected->isShowing())
            selected->grabKeyboardFocus();
        return selected->keyPressed(
            juce::KeyPress{juce::KeyPress::returnKey});
    }

    [[nodiscard]] bool navigateToSparseLiveGridCellWithNativeKeyForSmoke()
        override
    {
        auto* selected = selectedLiveGridButton();
        if (selected == nullptr)
            return false;
        const auto before = selected->getComponentID();
        if (selected->isShowing())
            selected->grabKeyboardFocus();
        const auto right = juce::KeyPress{juce::KeyPress::rightKey};
        if (selected->keyPressed(right) || !keyPressed(right))
        {
            return false;
        }
        selected = selectedLiveGridButton();
        return selected != nullptr
            && selected->getComponentID() != before
            && liveGridViewport_.getViewArea().intersects(
                selected->getBounds());
    }

    [[nodiscard]] bool liveGridActiveCellIsVisibleForSmoke() const override
    {
        const auto* active = activeLiveGridButton();
        return active != nullptr
            && liveGridViewport_.getViewArea().intersects(active->getBounds());
    }

    [[nodiscard]] bool liveGridActiveCellSurvivesResizeForSmoke() override
    {
        const auto originalBounds = getBounds();
        setSize(900, std::max(320, getHeight()));
        setSize(320, std::max(320, getHeight()));
        const auto visible = liveGridActiveCellIsVisibleForSmoke();
        setBounds(originalBounds);
        return visible;
    }

    [[nodiscard]] bool liveToolWorkflowIsAccessibleForSmoke() const override
    {
        return session_.mode
                == talas::project::WorkspacePerformanceSessionState::Mode::LiveGrid
            && liveToolSelector_.isVisible()
            && liveToolSelector_.getNumItems() > 0
            && liveToolStateLabel_.isVisible()
            && liveToolStateLabel_.getText().isNotEmpty()
            && liveToolManualButton_.isVisible()
            && liveToolManualButton_.getTitle().isNotEmpty()
            && liveToolVariableSelector_.isVisible()
            && liveToolVariableSelector_.getNumItems() > 0
            && liveToolVariableApply_.isVisible()
            && liveToolVariableApply_.getTitle().isNotEmpty();
    }

    [[nodiscard]] bool triggerSelectedLiveToolForSmoke() override
    {
        return triggerSelectedLiveTool();
    }

    [[nodiscard]] bool applySelectedLiveToolVariableForSmoke() override
    {
        liveToolVariableToggle_.setToggleState(
            true, juce::dontSendNotification);
        return applySelectedLiveToolVariable();
    }

    [[nodiscard]] bool
    applyRejectedSelectedLiveToolVariableForSmoke() override
    {
        liveToolVariableToggle_.setToggleState(
            false, juce::dontSendNotification);
        return applySelectedLiveToolVariable();
    }

    [[nodiscard]] bool liveToolDiagnosticContainsForSmoke(
        std::string_view text) const override
    {
        return liveToolStateLabel_.getText().contains(
            juceString(text));
    }

    [[nodiscard]] bool liveToolStatePersistedForSmoke(
        std::string_view expectedStateId,
        bool expectedBoolean) const override
    {
        if (!session_.selectedLiveToolId.has_value())
            return false;
        const auto saved = std::ranges::find(
            session_.liveToolStates,
            *session_.selectedLiveToolId,
            &talas::project::WorkspacePerformanceSessionState::
                LiveToolState::toolId);
        return saved != session_.liveToolStates.end()
            && saved->stateId == expectedStateId
            && saved->variables.size() == 1
            && saved->variables.front().kind
                == talas::project::ProjectLiveToolVariableKind::Boolean
            && saved->variables.front().boolean == expectedBoolean;
    }

    void refreshLiveToolRestoreInterleavingForSmoke(
        std::function<void()> afterInitialRuntimeRead) override
    {
        refreshLiveTools(std::move(afterInitialRuntimeRead));
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (session_.mode
            != talas::project::WorkspacePerformanceSessionState::Mode::LiveGrid)
        {
            return false;
        }
        if (key == juce::KeyPress::returnKey
            || key == juce::KeyPress::spaceKey)
        {
            return activateSelectedLiveGridCell();
        }
        if (key == juce::KeyPress::leftKey)
            return moveLiveGridSelection(0, -1);
        else if (key == juce::KeyPress::rightKey)
            return moveLiveGridSelection(0, 1);
        else if (key == juce::KeyPress::upKey)
            return moveLiveGridSelection(-1, 0);
        else if (key == juce::KeyPress::downKey)
            return moveLiveGridSelection(1, 0);
        else
            return false;
    }

    void paint(juce::Graphics& graphics) override
    {
        WorkbenchStyle::paintPanel(graphics, getLocalBounds().toFloat(), WorkbenchStyle::panelRadius());
    }

    void resized() override
    {
        const auto previousLiveGridViewportSize =
            juce::Point<int>{
                liveGridViewport_.getWidth(), liveGridViewport_.getHeight()};
        auto bounds = getLocalBounds().reduced(10);
        title_.setBounds(bounds.removeFromTop(20));
        auto tabs = bounds.removeFromTop(24);
        const auto tabWidth = std::max(68, (tabs.getWidth() - 8) / 3);
        controlsTab_.setBounds(tabs.removeFromLeft(tabWidth));
        tabs.removeFromLeft(4);
        snapshotsTab_.setBounds(tabs.removeFromLeft(tabWidth));
        tabs.removeFromLeft(4);
        liveGridTab_.setBounds(tabs);
        bounds.removeFromTop(5);
        contextLabel_.setBounds(bounds.removeFromTop(20));
        bounds.removeFromTop(4);
        emptyLabel_.setBounds(bounds);
        viewport_.setBounds(bounds);
        snapshotViewport_.setBounds(bounds);
        auto liveGridBounds = bounds;
        stopLiveGridButton_.setBounds(
            liveGridBounds.removeFromBottom(30).removeFromRight(82));
        liveGridBounds.removeFromBottom(5);
        liveGridSetSelector_.setBounds(
            liveGridBounds.removeFromTop(28));
        liveGridBounds.removeFromTop(5);
        liveToolSelector_.setBounds(
            liveGridBounds.removeFromTop(28));
        liveGridBounds.removeFromTop(4);
        auto toolStateRow = liveGridBounds.removeFromTop(24);
        liveToolManualButton_.setBounds(
            toolStateRow.removeFromRight(76));
        toolStateRow.removeFromRight(5);
        liveToolStateLabel_.setBounds(toolStateRow);
        liveGridBounds.removeFromTop(4);
        auto toolVariableRow = liveGridBounds.removeFromTop(26);
        liveToolVariableApply_.setBounds(
            toolVariableRow.removeFromRight(62));
        toolVariableRow.removeFromRight(4);
        const auto valueWidth = std::min(94, toolVariableRow.getWidth() / 3);
        auto valueBounds = toolVariableRow.removeFromRight(valueWidth);
        liveToolVariableValue_.setBounds(valueBounds);
        liveToolVariableToggle_.setBounds(valueBounds);
        toolVariableRow.removeFromRight(4);
        liveToolVariableSelector_.setBounds(toolVariableRow);
        liveGridBounds.removeFromTop(5);
        liveGridViewport_.setBounds(liveGridBounds);
        const auto snapshotWidth = std::max(
            220,
            bounds.getWidth() - 12);
        snapshotSurface_.setSize(
            snapshotWidth,
            std::max(
                bounds.getHeight(),
                snapshotSurface_.preferredHeight()));
        const auto width = std::max(120, bounds.getWidth() - 12);
        const auto authoringHeight = selectedItemEditorVisible_ ? 246 : 0;
        rowsContainer_.setSize(
            width,
            std::max(
                bounds.getHeight(),
                authoringHeight + rows_.size() * 68));
        constexpr int cellWidth = 104;
        constexpr int cellHeight = 58;
        constexpr int cellGap = 6;
        liveGridContainer_.setSize(
            static_cast<int>(talas::project::projectLiveSetColumns)
                    * (cellWidth + cellGap)
                + cellGap,
            static_cast<int>(talas::project::projectLiveSetRows)
                    * (cellHeight + cellGap)
                + cellGap);
        for (auto* cell : liveGridCells_)
        {
            const auto& action = cell->action();
            cell->setBounds(
                cellGap + static_cast<int>(action.column)
                        * (cellWidth + cellGap),
                cellGap + static_cast<int>(action.row)
                        * (cellHeight + cellGap),
                cellWidth,
                cellHeight);
        }
        if (juce::Point<int>{
                liveGridViewport_.getWidth(), liveGridViewport_.getHeight()}
            != previousLiveGridViewportSize)
        {
            if (auto* active = activeLiveGridButton(); active != nullptr)
                scrollLiveGridButtonIntoView(*active, false);
        }
        auto rowBounds = rowsContainer_.getLocalBounds();
        if (selectedItemEditorVisible_)
        {
            auto editor = rowBounds.removeFromTop(authoringHeight).reduced(2);
            selectedItemTitle_.setBounds(editor.removeFromTop(20));
            selectedItemIdentity_.setBounds(editor.removeFromTop(18));
            editor.removeFromTop(4);
            auto nameRow = editor.removeFromTop(26);
            selectedItemStructure_.setBounds(nameRow.removeFromRight(66));
            nameRow.removeFromRight(4);
            selectedItemRemove_.setBounds(nameRow.removeFromRight(54));
            nameRow.removeFromRight(4);
            selectedItemApply_.setBounds(nameRow.removeFromRight(54));
            nameRow.removeFromRight(4);
            selectedItemName_.setBounds(nameRow);
            editor.removeFromTop(4);
            auto kindRow = editor.removeFromTop(26);
            selectedItemVisible_.setBounds(kindRow.removeFromRight(132));
            kindRow.removeFromRight(4);
            selectedItemKind_.setBounds(kindRow);
            editor.removeFromTop(8);
            modulationTitle_.setBounds(editor.removeFromTop(18));
            auto modulationRow = editor.removeFromTop(26);
            modulationAdd_.setBounds(modulationRow.removeFromRight(44));
            modulationRow.removeFromRight(4);
            modulationDepth_.setBounds(modulationRow.removeFromRight(54));
            modulationRow.removeFromRight(4);
            modulationTarget_.setBounds(modulationRow);
            editor.removeFromTop(4);
            modulationClear_.setBounds(
                editor.removeFromTop(24).removeFromRight(52));
            modulationSummary_.setBounds(editor);
            rowBounds.removeFromTop(6);
        }
        for (auto* row : rows_)
        {
            row->setBounds(
                rowBounds.removeFromTop(64).reduced(0, 2));
            rowBounds.removeFromTop(4);
        }
    }

private:
    static int kindItemId(
        talas::project::ProjectPerformanceControlKind kind) noexcept
    {
        return static_cast<int>(kind) + 1;
    }

    static std::optional<talas::project::ProjectPerformanceControlKind>
    kindForItemId(int itemId) noexcept
    {
        if (itemId < 1 || itemId > 5)
            return std::nullopt;
        return static_cast<talas::project::ProjectPerformanceControlKind>(
            itemId - 1);
    }

    void setSelectedItemComponentsVisible(bool visible)
    {
        for (auto* component : std::array<juce::Component*, 14>{
                 &selectedItemTitle_,
                 &selectedItemIdentity_,
                 &selectedItemName_,
                 &selectedItemKind_,
                 &selectedItemVisible_,
                 &selectedItemApply_,
                 &selectedItemRemove_,
                 &selectedItemStructure_,
                 &modulationTitle_,
                 &modulationTarget_,
                 &modulationDepth_,
                 &modulationAdd_,
                 &modulationClear_,
                 &modulationSummary_})
        {
            component->setVisible(visible);
        }
    }

    void refreshSelectedPerformanceItem()
    {
        selectedItemEditorVisible_ = false;
        selectedModulationTargets_.clear();
        setSelectedItemComponentsVisible(false);
        if (!selectedPerformanceControlId_.has_value()
            && selectedPerformanceMeterId_.has_value())
        {
            const auto& meters = bridge_.editor().document()
                                     .presentation.performanceMeters;
            const auto meter = std::ranges::find(
                meters,
                *selectedPerformanceMeterId_,
                &talas::project::ProjectPerformanceMeterPresentation::meterId);
            if (meter == meters.end())
            {
                selectedPerformanceMeterId_.reset();
                return;
            }
            selectedItemEditorVisible_ = true;
            setSelectedItemComponentsVisible(true);
            selectedItemTitle_.setText(
                "Canvas / Output " + juce::String(meter->outputChannel)
                    + " meter",
                juce::dontSendNotification);
            selectedItemIdentity_.setText(
                juceString(meter->meterId), juce::dontSendNotification);
            selectedItemName_.setText(
                "Output " + juce::String(meter->outputChannel), false);
            selectedItemName_.setEnabled(false);
            selectedItemKind_.setSelectedId(0, juce::dontSendNotification);
            selectedItemKind_.setText("Meter", juce::dontSendNotification);
            selectedItemKind_.setEnabled(false);
            selectedItemVisible_.setToggleState(
                meter->visible, juce::dontSendNotification);
            selectedItemApply_.setEnabled(true);
            selectedItemRemove_.setButtonText("Remove");
            selectedItemStructure_.setEnabled(false);
            modulationTarget_.setEnabled(false);
            modulationDepth_.setEnabled(false);
            modulationAdd_.setEnabled(false);
            modulationClear_.setEnabled(false);
            modulationSummary_.setText(
                "Meters project existing bounded output peaks.",
                juce::dontSendNotification);
            return;
        }
        if (!selectedPerformanceControlId_.has_value())
            return;
        const auto& document = bridge_.editor().document();
        const auto* selected =
            talas::project::findProjectPerformanceControlPresentationById(
                document, *selectedPerformanceControlId_);
        if (selected == nullptr)
        {
            selectedPerformanceControlId_.reset();
            return;
        }
        selectedItemEditorVisible_ = true;
        setSelectedItemComponentsVisible(true);
        selectedItemName_.setEnabled(true);
        selectedItemKind_.setEnabled(true);
        selectedItemApply_.setEnabled(true);
        selectedItemTitle_.setText(
            "Canvas / " + juceString(selected->displayName),
            juce::dontSendNotification);
        selectedItemIdentity_.setText(
            juceString(talas::project::snapshotTargetIdentityString(
                selected->target)),
            juce::dontSendNotification);
        selectedItemName_.setText(
            juceString(selected->displayName), false);
        selectedItemKind_.setSelectedId(
            kindItemId(selected->kind), juce::dontSendNotification);
        selectedItemVisible_.setToggleState(
            selected->visible, juce::dontSendNotification);
        selectedItemRemove_.setButtonText(
            selected->target.kind
                    == talas::project::SnapshotTargetKind::EventValue
                ? "Hide"
                : "Remove");
        selectedItemStructure_.setEnabled(
            selected->target.kind
                == talas::project::SnapshotTargetKind::SoundParameter
            && !selected->target.instancePath.empty());
        modulationTarget_.clear(juce::dontSendNotification);
        for (const auto& candidate :
             document.presentation.performanceControls)
        {
            if (candidate.target == selected->target)
                continue;
            selectedModulationTargets_.push_back(candidate.target);
            modulationTarget_.addItem(
                juceString(candidate.displayName),
                static_cast<int>(selectedModulationTargets_.size()));
        }
        if (!selectedModulationTargets_.empty())
            modulationTarget_.setSelectedId(1, juce::dontSendNotification);
        const auto macro = selected->kind
            == talas::project::ProjectPerformanceControlKind::Macro;
        modulationTarget_.setEnabled(macro);
        modulationDepth_.setEnabled(macro);
        modulationAdd_.setEnabled(macro);
        modulationClear_.setEnabled(
            macro && !selected->modulationSlots.empty());
        juce::StringArray slots;
        for (const auto& slot : selected->modulationSlots)
        {
            slots.add(
                juceString(talas::project::snapshotTargetIdentityString(
                    slot.target))
                + "  " + juce::String(slot.depth, 3));
        }
        modulationSummary_.setText(
            slots.isEmpty()
                ? juce::String{"No modulation slots"}
                : slots.joinIntoString("\n"),
            juce::dontSendNotification);
    }

    void finishSelectedPerformanceEdit(const juce::String& message)
    {
        const auto adopted = bridge_.adoptSnapshotAuthoringEdit();
        if (!adopted.ok() && !adopted.message.empty())
            onStatus_(juceString(adopted.message));
        onDocumentChange_(adopted.ok());
        if (adopted.ok())
            onStatus_(message);
        refresh();
    }

    void applySelectedPerformanceControl()
    {
        if (selectedPerformanceMeterId_.has_value()
            && !selectedPerformanceControlId_.has_value())
        {
            const auto result = bridge_.editor().setPerformanceMeterVisible(
                *selectedPerformanceMeterId_,
                selectedItemVisible_.getToggleState());
            if (!result.ok())
            {
                onStatus_(juceString(result.message));
                return;
            }
            finishSelectedPerformanceEdit(
                "Updated portable Performance meter");
            return;
        }
        if (!selectedPerformanceControlId_.has_value())
            return;
        const auto kind = kindForItemId(selectedItemKind_.getSelectedId());
        const auto displayName = selectedItemName_.getText().trim().toStdString();
        if (!kind.has_value() || displayName.empty()
            || displayName.size()
                > talas::project::maximumPerformanceDisplayNameBytes)
        {
            onStatus_("Performance control requires a name and visual kind");
            return;
        }
        auto& editor = bridge_.editor();
        editor.beginHistoryTransaction();
        auto result = editor.setPerformanceControlDisplayName(
            *selectedPerformanceControlId_, displayName);
        if (result.ok())
            result = editor.setPerformanceControlKind(
                *selectedPerformanceControlId_, *kind);
        if (result.ok())
            result = editor.setPerformanceControlVisible(
                *selectedPerformanceControlId_,
                selectedItemVisible_.getToggleState());
        editor.endHistoryTransaction();
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            refresh();
            return;
        }
        finishSelectedPerformanceEdit("Updated portable Performance control");
    }

    void hideOrRemoveSelectedPerformanceItem()
    {
        if (selectedPerformanceMeterId_.has_value()
            && !selectedPerformanceControlId_.has_value())
        {
            const auto result = bridge_.editor().removePerformanceMeter(
                *selectedPerformanceMeterId_);
            if (!result.ok())
            {
                onStatus_(juceString(result.message));
                return;
            }
            selectedPerformanceMeterId_.reset();
            finishSelectedPerformanceEdit(
                "Removed portable Performance meter");
            return;
        }
        if (!selectedPerformanceControlId_.has_value())
            return;
        const auto& document = bridge_.editor().document();
        const auto* selected =
            talas::project::findProjectPerformanceControlPresentationById(
                document, *selectedPerformanceControlId_);
        if (selected == nullptr)
            return;
        const auto eventValue = selected->target.kind
            == talas::project::SnapshotTargetKind::EventValue;
        const auto result = eventValue
            ? bridge_.editor().setPerformanceControlVisible(
                *selectedPerformanceControlId_, false)
            : bridge_.editor().removePerformanceControl(
                *selectedPerformanceControlId_);
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        selectedPerformanceControlId_.reset();
        finishSelectedPerformanceEdit(
            eventValue
                ? "Hid portable Performance control"
                : "Removed portable Performance control");
    }

    void addSelectedModulationSlot()
    {
        if (!selectedPerformanceControlId_.has_value())
            return;
        const auto selectedTarget = modulationTarget_.getSelectedId();
        if (selectedTarget <= 0
            || static_cast<std::size_t>(selectedTarget)
                > selectedModulationTargets_.size())
        {
            onStatus_("Choose an exact modulation destination");
            return;
        }
        const auto depthText =
            modulationDepth_.getText().trim().toStdString();
        double depth = 0.0;
        const auto parsed = std::from_chars(
            depthText.data(),
            depthText.data() + depthText.size(),
            depth,
            std::chars_format::general);
        if (depthText.empty()
            || parsed.ec != std::errc{}
            || parsed.ptr != depthText.data() + depthText.size()
            || !std::isfinite(depth)
            || depth < -1.0 || depth > 1.0)
        {
            onStatus_("Modulation depth must be a number from -1 to 1");
            return;
        }
        const auto* selected =
            talas::project::findProjectPerformanceControlPresentationById(
                bridge_.editor().document(),
                *selectedPerformanceControlId_);
        if (selected == nullptr)
            return;
        auto slots = selected->modulationSlots;
        slots.push_back({
            selectedModulationTargets_[
                static_cast<std::size_t>(selectedTarget - 1)],
            depth});
        const auto result =
            bridge_.editor().setPerformanceControlModulationSlots(
                *selectedPerformanceControlId_, std::move(slots));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        finishSelectedPerformanceEdit("Added exact macro modulation slot");
    }

    void showSelectedPerformanceItemInStructure()
    {
        if (!selectedPerformanceControlId_.has_value()
            || !onStructureFocus_)
        {
            return;
        }
        const auto* selected =
            talas::project::findProjectPerformanceControlPresentationById(
                bridge_.editor().document(),
                *selectedPerformanceControlId_);
        if (selected == nullptr
            || selected->target.kind
                != talas::project::SnapshotTargetKind::SoundParameter
            || selected->target.instancePath.empty())
        {
            return;
        }
        onStructureFocus_({
            selected->target.instancePath.back(),
            selected->target.definitionId,
            selected->target.parameterId,
        });
    }

    bool addSoundControlToCanvas(
        const talas::app::SoundPerformanceControlIdentity& identity,
        std::string_view displayName)
    {
        if (!captureContext_.exact)
        {
            onStatus_(
                "Performance canvas target is ambiguous in the current hierarchy path");
            return false;
        }
        talas::project::SnapshotTargetIdentity target;
        target.kind = talas::project::SnapshotTargetKind::SoundParameter;
        target.instancePath = captureContext_.instancePath;
        target.instancePath.push_back(identity.instanceId);
        target.definitionId = identity.definitionId;
        target.parameterId = identity.parameterId;
        if (const auto* existing =
                talas::project::findProjectPerformanceControlPresentation(
                    bridge_.editor().document(), target))
        {
            selectedPerformanceControlId_ = existing->controlId;
            selectedPerformanceMeterId_.reset();
            onCanvasSelection_(
                selectedPerformanceControlId_, std::nullopt);
            onStatus_("Exact Sound parameter is already on the Performance canvas");
            refresh();
            return true;
        }
        const auto result = bridge_.editor().addPerformanceControl(
            target,
            talas::project::ProjectPerformanceControlKind::Knob,
            std::string{displayName});
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return false;
        }
        if (const auto* added =
                talas::project::findProjectPerformanceControlPresentation(
                    bridge_.editor().document(), target))
        {
            selectedPerformanceControlId_ = added->controlId;
            selectedPerformanceMeterId_.reset();
            onCanvasSelection_(
                selectedPerformanceControlId_, std::nullopt);
        }
        finishSelectedPerformanceEdit(
            "Added exact Sound parameter to the Performance canvas");
        return true;
    }

    void clearSelectedModulationSlots()
    {
        if (!selectedPerformanceControlId_.has_value())
            return;
        const auto result =
            bridge_.editor().setPerformanceControlModulationSlots(
                *selectedPerformanceControlId_, {});
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        finishSelectedPerformanceEdit("Cleared macro modulation slots");
    }

    void setPerformanceMode(
        talas::project::WorkspacePerformanceSessionState::Mode mode)
    {
        if (session_.mode == mode)
        {
            updateTabVisibility();
            return;
        }
        session_.mode = mode;
        if (onSessionChange_)
            onSessionChange_();
        updateTabVisibility();
    }

    [[nodiscard]] talas::app::LiveGridSourceContext workbenchLiveGridSource()
    {
        talas::app::LiveGridSourceContext source;
        source.sourceGeneration = bridge_.latestStageGeneration();
        source.routeGeneration =
            bridge_.editor().documentAuthorityGeneration();
        source.sourceAvailable = source.sourceGeneration != 0
            && source.routeGeneration != 0;
        if (source.sourceAvailable)
        {
            bridge_.publishLiveSetSourceAuthority(
                source.kind,
                source.sourceGeneration,
                source.routeGeneration);
        }
        return source;
    }

    [[nodiscard]] talas::app::LiveGridProjectionEnvironment
    liveGridEnvironment(
        const std::vector<std::string>& midiIdentifiers,
        std::string_view projectedLiveSetId = {}) const
    {
        return {
            .connectedMidiSourceIdentifiers = midiIdentifiers,
            .oscListening = oscAvailable_ && oscAvailable_(),
            .queuedLiveSetId = session_.queuedLiveSetId.has_value()
                ? std::string_view{*session_.queuedLiveSetId}
                : std::string_view{},
            .queuedCellId = session_.queuedLiveSetCellId.has_value()
                ? std::string_view{*session_.queuedLiveSetCellId}
                : std::string_view{},
            .queuedRequestGeneration =
                session_.queuedLiveSetRequestGeneration,
            .projectedLiveSetId = projectedLiveSetId,
        };
    }

    void reconcileLiveGridSetSelector()
    {
        const auto& liveSets =
            bridge_.editor().document().liveSetLibrary.liveSets;
        const auto selected = session_.selectedLiveSetId.has_value()
            ? std::ranges::find(
                  liveSets,
                  *session_.selectedLiveSetId,
                  &talas::project::ProjectLiveSet::liveSetId)
            : liveSets.end();
        if (selected == liveSets.end())
        {
            if (liveSets.empty())
            {
                session_.selectedLiveSetId.reset();
                session_.selectedLiveSetCellId.reset();
            }
            else
            {
                session_.selectedLiveSetId = liveSets.front().liveSetId;
                session_.selectedLiveSetCellId = liveSets.front().cells.empty()
                    ? std::optional<std::string>{}
                    : std::optional<std::string>{
                          liveSets.front().cells.front().cellId};
            }
        }

        liveGridSetSelector_.clear(juce::dontSendNotification);
        auto selectedItem = 0;
        for (std::size_t index = 0; index < liveSets.size(); ++index)
        {
            const auto itemId = static_cast<int>(index) + 1;
            liveGridSetSelector_.addItem(
                juceString(liveSets[index].displayName.empty()
                        ? liveSets[index].liveSetId
                        : liveSets[index].displayName),
                itemId);
            if (session_.selectedLiveSetId
                == std::optional<std::string>{liveSets[index].liveSetId})
            {
                selectedItem = itemId;
            }
        }
        liveGridSetSelector_.setSelectedId(
            selectedItem, juce::dontSendNotification);
        liveGridSetSelector_.setEnabled(!liveSets.empty());
    }

    void selectLiveGridSetFromSelector()
    {
        const auto& liveSets =
            bridge_.editor().document().liveSetLibrary.liveSets;
        const auto index = liveGridSetSelector_.getSelectedId() - 1;
        if (index < 0 || static_cast<std::size_t>(index) >= liveSets.size())
            return;
        const auto& selected = liveSets[static_cast<std::size_t>(index)];
        session_.selectedLiveSetId = selected.liveSetId;
        const auto retained = session_.selectedLiveSetCellId.has_value()
            ? std::ranges::find(
                  selected.cells,
                  *session_.selectedLiveSetCellId,
                  &talas::project::ProjectLiveSetCell::cellId)
            : selected.cells.end();
        session_.selectedLiveSetCellId = retained != selected.cells.end()
            ? session_.selectedLiveSetCellId
            : selected.cells.empty()
                ? std::optional<std::string>{}
                : std::optional<std::string>{
                      selected.cells.front().cellId};
        if (onSessionChange_)
            onSessionChange_();
        refreshLiveGrid();
    }

    [[nodiscard]] const talas::project::ProjectLiveTool*
    selectedLiveToolDefinition() const
    {
        if (!session_.selectedLiveToolId.has_value())
            return nullptr;
        const auto& tools =
            bridge_.editor().document().liveSetLibrary.tools;
        const auto found = std::ranges::find(
            tools,
            *session_.selectedLiveToolId,
            &talas::project::ProjectLiveTool::toolId);
        return found == tools.end() ? nullptr : &*found;
    }

    void reconcileLiveToolSelector()
    {
        const auto& tools =
            bridge_.editor().document().liveSetLibrary.tools;
        if (session_.selectedLiveToolId.has_value()
            && std::ranges::none_of(tools, [&](const auto& tool) {
                   return tool.toolId == *session_.selectedLiveToolId;
               }))
        {
            session_.selectedLiveToolId.reset();
        }
        if (!session_.selectedLiveToolId.has_value() && !tools.empty())
            session_.selectedLiveToolId = tools.front().toolId;

        auto selectedItem = 0;
        const auto sameItems = liveToolSelector_.getNumItems()
                == static_cast<int>(tools.size())
            && [&] {
                for (std::size_t index = 0; index < tools.size(); ++index)
                {
                    const auto expected = juceString(
                        tools[index].displayName.empty()
                            ? tools[index].toolId
                            : tools[index].displayName);
                    if (liveToolSelector_.getItemText(
                            static_cast<int>(index)) != expected)
                    {
                        return false;
                    }
                }
                return true;
            }();
        if (!sameItems)
            liveToolSelector_.clear(juce::dontSendNotification);
        for (std::size_t index = 0; index < tools.size(); ++index)
        {
            const auto itemId = static_cast<int>(index) + 1;
            if (!sameItems)
            {
                liveToolSelector_.addItem(
                    juceString(tools[index].displayName.empty()
                            ? tools[index].toolId
                            : tools[index].displayName),
                    itemId);
            }
            if (session_.selectedLiveToolId
                == std::optional<std::string>{tools[index].toolId})
            {
                selectedItem = itemId;
            }
        }
        liveToolSelector_.setSelectedId(
            selectedItem, juce::dontSendNotification);
        liveToolSelector_.setEnabled(!tools.empty());
    }

    void selectLiveToolFromSelector()
    {
        const auto& tools =
            bridge_.editor().document().liveSetLibrary.tools;
        const auto index = liveToolSelector_.getSelectedId() - 1;
        if (index < 0 || static_cast<std::size_t>(index) >= tools.size())
            return;
        session_.selectedLiveToolId =
            tools[static_cast<std::size_t>(index)].toolId;
        selectedLiveToolVariableId_.reset();
        if (onSessionChange_)
            onSessionChange_();
        refreshLiveTools();
    }

    [[nodiscard]] std::optional<talas::runtime::PreparedLiveToolRuntimeState>
    restoredLiveToolState(
        const talas::runtime::PreparedLiveTools& prepared) const
    {
        talas::runtime::PreparedLiveToolRuntimeState result;
        if (!talas::runtime::initializePreparedLiveToolRuntimeState(
                prepared, result))
        {
            return std::nullopt;
        }
        auto restoredAny = false;
        const auto& definitions =
            bridge_.editor().document().liveSetLibrary.tools;
        for (std::size_t toolIndex = 0;
             toolIndex < prepared.tools().size();
             ++toolIndex)
        {
            const auto& tool = prepared.tools()[toolIndex];
            const auto definition = std::ranges::find(
                definitions,
                tool.toolId,
                &talas::project::ProjectLiveTool::toolId);
            const auto saved = std::ranges::find(
                session_.liveToolStates,
                tool.toolId,
                &talas::project::WorkspacePerformanceSessionState::
                    LiveToolState::toolId);
            if (definition == definitions.end()
                || saved == session_.liveToolStates.end()
                || saved->definitionFingerprint
                    != talas::project::workspaceSessionLiveToolDefinitionFingerprint(
                        *definition)
                || saved->variables.size() != tool.variables.size())
            {
                continue;
            }
            const auto state = std::ranges::find(
                tool.stateIds, saved->stateId);
            if (state == tool.stateIds.end())
                continue;
            auto exactVariables = true;
            const auto offset = result.variableOffsets[toolIndex];
            for (std::size_t variableIndex = 0;
                 variableIndex < tool.variables.size();
                 ++variableIndex)
            {
                const auto& declaration = tool.variables[variableIndex];
                const auto value = std::ranges::find(
                    saved->variables,
                    declaration.variableId,
                    &talas::project::WorkspacePerformanceSessionState::
                        LiveToolVariable::variableId);
                if (value == saved->variables.end()
                    || value->kind != declaration.kind
                    || !std::isfinite(value->number)
                    || (declaration.kind
                            == talas::project::ProjectLiveToolVariableKind::Boolean
                        ? value->number != 0.0
                        : value->boolean))
                {
                    exactVariables = false;
                    break;
                }
                result.variables[offset + variableIndex] = {
                    value->number, value->boolean};
            }
            if (!exactVariables)
                continue;
            result.stateIndices[toolIndex] = static_cast<std::uint8_t>(
                std::distance(tool.stateIds.begin(), state));
            restoredAny = true;
        }
        return restoredAny ? std::optional{result} : std::nullopt;
    }

    void saveLiveToolRuntimeState(
        const talas::runtime::PreparedLiveTools& prepared,
        const talas::runtime::PreparedLiveToolRuntimeState& state)
    {
        std::vector<talas::project::WorkspacePerformanceSessionState::
            LiveToolState> saved;
        saved.reserve(prepared.tools().size());
        const auto& definitions =
            bridge_.editor().document().liveSetLibrary.tools;
        for (std::size_t toolIndex = 0;
             toolIndex < prepared.tools().size();
             ++toolIndex)
        {
            const auto& tool = prepared.tools()[toolIndex];
            const auto definition = std::ranges::find(
                definitions,
                tool.toolId,
                &talas::project::ProjectLiveTool::toolId);
            if (definition == definitions.end()
                || state.stateIndices[toolIndex] >= tool.stateIds.size())
            {
                continue;
            }
            talas::project::WorkspacePerformanceSessionState::LiveToolState item;
            item.toolId = tool.toolId;
            item.stateId = tool.stateIds[state.stateIndices[toolIndex]];
            item.definitionFingerprint =
                talas::project::workspaceSessionLiveToolDefinitionFingerprint(
                    *definition);
            item.variables.reserve(tool.variables.size());
            const auto offset = state.variableOffsets[toolIndex];
            for (std::size_t variableIndex = 0;
                 variableIndex < tool.variables.size();
                 ++variableIndex)
            {
                const auto& declaration = tool.variables[variableIndex];
                const auto& value = state.variables[offset + variableIndex];
                item.variables.push_back({
                    declaration.variableId,
                    declaration.kind,
                    value.number,
                    value.boolean,
                });
            }
            saved.push_back(std::move(item));
        }
        session_.liveToolStates = std::move(saved);
    }

    void refreshSelectedLiveToolVariable()
    {
        const auto prepared = bridge_.latestPreparedLiveTools();
        const auto state = bridge_.realtimeLiveToolState();
        const auto* definition = selectedLiveToolDefinition();
        if (prepared == nullptr || !state.has_value()
            || definition == nullptr)
        {
            selectedLiveToolVariableIsBoolean_ = false;
            liveToolVariableValue_.setVisible(false);
            liveToolVariableToggle_.setVisible(false);
            liveToolVariableApply_.setEnabled(false);
            return;
        }
        const auto toolIndex = prepared->findTool(definition->toolId);
        if (!toolIndex.has_value())
            return;
        const auto& tool = prepared->tools()[*toolIndex];
        const auto selectedIndex = liveToolVariableSelector_.getSelectedId() - 1;
        if (selectedIndex < 0
            || static_cast<std::size_t>(selectedIndex)
                >= tool.variables.size())
        {
            selectedLiveToolVariableIsBoolean_ = false;
            liveToolVariableValue_.setVisible(false);
            liveToolVariableToggle_.setVisible(false);
            liveToolVariableApply_.setEnabled(false);
            return;
        }
        const auto variableIndex = static_cast<std::size_t>(selectedIndex);
        selectedLiveToolVariableId_ = tool.variables[variableIndex].variableId;
        const auto& variable = tool.variables[variableIndex];
        const auto value = state->variables[
            state->variableOffsets[*toolIndex] + variableIndex];
        const auto boolean = variable.kind
            == talas::project::ProjectLiveToolVariableKind::Boolean;
        selectedLiveToolVariableIsBoolean_ = boolean;
        const auto showLiveGrid = session_.mode
            == talas::project::WorkspacePerformanceSessionState::Mode::LiveGrid;
        liveToolVariableToggle_.setVisible(showLiveGrid && boolean);
        liveToolVariableValue_.setVisible(showLiveGrid && !boolean);
        liveToolVariableToggle_.setToggleState(
            value.boolean, juce::dontSendNotification);
        if (!liveToolVariableValue_.hasKeyboardFocus(true))
            liveToolVariableValue_.setText(
                juce::String(value.number, 8), false);
        const auto currentState = state->stateIndices[*toolIndex];
        liveToolVariableApply_.setEnabled(std::ranges::any_of(
            tool.transitions, [&](const auto& transition) {
                return transition.fromStateIndex == currentState
                    && transition.trigger.kind
                        == talas::project::ProjectLiveToolTriggerKind::VariableChange
                    && transition.trigger.variableId == variable.variableId;
            }));
    }

    void refreshLiveTools(
        std::function<void()> afterInitialRuntimeRead = {})
    {
        const auto previousSession = session_;
        reconcileLiveToolSelector();
        const auto prepared = bridge_.latestPreparedLiveTools();
        auto runtimeState = bridge_.realtimeLiveToolState();
        if (afterInitialRuntimeRead)
            afterInitialRuntimeRead();
        if (prepared == nullptr || !runtimeState.has_value())
        {
            liveToolStateLabel_.setText(
                "Live Tools unavailable until the current project is staged",
                juce::dontSendNotification);
            liveToolManualButton_.setEnabled(false);
            liveToolVariableSelector_.clear(
                juce::dontSendNotification);
            liveToolVariableSelector_.setEnabled(false);
            liveToolVariableApply_.setEnabled(false);
            pendingLiveToolRestore_.reset();
            requestedLiveToolRestoreStageId_ = 0;
            pendingLiveToolRestoreRequestGeneration_ = 0;
            lastSavedLiveToolRuntimeState_.reset();
            if (session_ != previousSession && onSessionChange_)
                onSessionChange_();
            return;
        }

        if (requestedLiveToolRestoreStageId_ != runtimeState->stageId)
        {
            requestedLiveToolRestoreStageId_ = runtimeState->stageId;
            lastSavedLiveToolRuntimeState_.reset();
            pendingLiveToolRestore_ = restoredLiveToolState(*prepared);
            pendingLiveToolRestoreRequestGeneration_ = 0;
            if (pendingLiveToolRestore_.has_value()
                && !bridge_.requestLiveToolStateRestore(
                    *pendingLiveToolRestore_,
                    &pendingLiveToolRestoreRequestGeneration_))
            {
                pendingLiveToolRestore_.reset();
                pendingLiveToolRestoreRequestGeneration_ = 0;
                onStatus_("Live Tool session restore was rejected by the current stage authority");
            }
        }
        if (pendingLiveToolRestore_.has_value())
        {
            if (pendingLiveToolRestoreRequestGeneration_ != 0
                && bridge_.appliedLiveToolStateRestoreGeneration()
                    >= pendingLiveToolRestoreRequestGeneration_)
            {
                if (auto acknowledgedState =
                        bridge_.realtimeLiveToolState())
                {
                    runtimeState = std::move(acknowledgedState);
                    pendingLiveToolRestore_.reset();
                    pendingLiveToolRestoreRequestGeneration_ = 0;
                }
                else
                {
                    runtimeState = pendingLiveToolRestore_;
                }
            }
            else
                runtimeState = pendingLiveToolRestore_;
        }
        if (!pendingLiveToolRestore_.has_value()
            && (!lastSavedLiveToolRuntimeState_.has_value()
                || *lastSavedLiveToolRuntimeState_ != *runtimeState))
        {
            saveLiveToolRuntimeState(*prepared, *runtimeState);
            lastSavedLiveToolRuntimeState_ = runtimeState;
        }

        const auto* definition = selectedLiveToolDefinition();
        const auto toolIndex = definition == nullptr
            ? std::optional<std::size_t>{}
            : prepared->findTool(definition->toolId);
        if (!toolIndex.has_value())
        {
            liveToolStateLabel_.setText(
                "No Live Tool selected", juce::dontSendNotification);
            liveToolManualButton_.setEnabled(false);
            liveToolVariableSelector_.clear(
                juce::dontSendNotification);
            liveToolVariableSelector_.setEnabled(false);
            selectedLiveToolVariableId_.reset();
            selectedLiveToolVariableIsBoolean_ = false;
            liveToolVariableValue_.setVisible(false);
            liveToolVariableToggle_.setVisible(false);
            liveToolVariableApply_.setEnabled(false);
            return;
        }
        const auto& tool = prepared->tools()[*toolIndex];
        const auto stateIndex = runtimeState->stateIndices[*toolIndex];
        const auto stateName = stateIndex < definition->states.size()
            ? definition->states[stateIndex].displayName
            : tool.stateIds[stateIndex];
        const auto telemetry = bridge_.realtimeLiveToolTelemetry();
        const auto hasIssue = telemetry.queueOverflow != 0
            || telemetry.staleAuthority != 0
            || telemetry.rejectedTriggers != 0;
        const auto stateDescription =
            "State: " + juceString(stateName.empty()
                    ? tool.stateIds[stateIndex]
                    : stateName)
                + juceString(" · ") + juceString(definition->toolId)
                + (!hasIssue
                        ? juce::String{}
                        : juceString(" · held/rejected q/s/r ")
                            + juce::String(telemetry.queueOverflow)
                            + "/" + juce::String(telemetry.staleAuthority)
                            + "/" + juce::String(
                                telemetry.rejectedTriggers)
                            + juceString(
                                " · reasons no-match/invalid/stale/guard/command/init ")
                            + juce::String(telemetry.noMatchingTransition)
                            + "/" + juce::String(telemetry.invalidTrigger)
                            + "/" + juce::String(
                                telemetry.staleTriggerAuthority)
                            + "/" + juce::String(telemetry.guardRejected)
                            + "/" + juce::String(telemetry.commandRejected)
                            + "/" + juce::String(
                                telemetry.stateInitializationRejected));
        liveToolStateLabel_.setText(
            stateDescription, juce::dontSendNotification);
        liveToolStateLabel_.setTooltip(stateDescription);
        liveToolManualButton_.setEnabled(std::ranges::any_of(
            tool.transitions, [&](const auto& transition) {
                return transition.fromStateIndex == stateIndex
                    && transition.trigger.kind
                        == talas::project::ProjectLiveToolTriggerKind::Manual;
            }));

        const auto retainedVariableId = selectedLiveToolVariableId_;
        auto selectedVariable = 0;
        const auto sameVariableItems =
            liveToolVariableSelector_.getNumItems()
                == static_cast<int>(tool.variables.size())
            && [&] {
                for (std::size_t variableIndex = 0;
                     variableIndex < tool.variables.size();
                     ++variableIndex)
                {
                    const auto source = std::ranges::find(
                        definition->variables,
                        tool.variables[variableIndex].variableId,
                        &talas::project::ProjectLiveToolVariable::variableId);
                    const auto expected =
                        source != definition->variables.end()
                                && !source->displayName.empty()
                            ? juceString(source->displayName)
                            : juceString(
                                tool.variables[variableIndex].variableId);
                    if (liveToolVariableSelector_.getItemText(
                            static_cast<int>(variableIndex)) != expected)
                    {
                        return false;
                    }
                }
                return true;
            }();
        if (!sameVariableItems)
            liveToolVariableSelector_.clear(juce::dontSendNotification);
        for (std::size_t variableIndex = 0;
             variableIndex < tool.variables.size();
             ++variableIndex)
        {
            const auto itemId = static_cast<int>(variableIndex) + 1;
            const auto source = std::ranges::find(
                definition->variables,
                tool.variables[variableIndex].variableId,
                &talas::project::ProjectLiveToolVariable::variableId);
            if (!sameVariableItems)
            {
                liveToolVariableSelector_.addItem(
                    source != definition->variables.end()
                            && !source->displayName.empty()
                        ? juceString(source->displayName)
                        : juceString(
                            tool.variables[variableIndex].variableId),
                    itemId);
            }
            if (retainedVariableId
                == std::optional<std::string>{
                    tool.variables[variableIndex].variableId})
            {
                selectedVariable = itemId;
            }
        }
        if (selectedVariable == 0 && !tool.variables.empty())
            selectedVariable = 1;
        liveToolVariableSelector_.setSelectedId(
            selectedVariable, juce::dontSendNotification);
        liveToolVariableSelector_.setEnabled(!tool.variables.empty());
        refreshSelectedLiveToolVariable();
        if (session_ != previousSession && onSessionChange_)
            onSessionChange_();
    }

    [[nodiscard]] bool triggerSelectedLiveTool()
    {
        const auto* tool = selectedLiveToolDefinition();
        if (tool == nullptr)
            return false;
        const auto sourceGeneration = bridge_.latestStageGeneration();
        const auto routeGeneration =
            bridge_.editor().documentAuthorityGeneration();
        bridge_.publishLiveToolSourceAuthority(
            talas::runtime::RealtimeLiveToolSourceKind::Workbench,
            sourceGeneration,
            routeGeneration);
        const auto result = bridge_.requestLiveToolTrigger({
            .toolId = tool->toolId,
            .triggerKind = talas::project::ProjectLiveToolTriggerKind::Manual,
            .sourceKind = talas::runtime::RealtimeLiveToolSourceKind::Workbench,
            .sourceGeneration = sourceGeneration,
            .routeGeneration = routeGeneration,
        });
        onStatus_(result.ok()
                ? "Queued Live Tool trigger " + juceString(tool->toolId)
                : "Live Tool rejected: " + juceString(result.message));
        return result.ok();
    }

    [[nodiscard]] bool applySelectedLiveToolVariable()
    {
        const auto* tool = selectedLiveToolDefinition();
        const auto prepared = bridge_.latestPreparedLiveTools();
        if (tool == nullptr || prepared == nullptr)
            return false;
        const auto toolIndex = prepared->findTool(tool->toolId);
        const auto selectedIndex = liveToolVariableSelector_.getSelectedId() - 1;
        if (!toolIndex.has_value() || selectedIndex < 0
            || static_cast<std::size_t>(selectedIndex)
                >= prepared->tools()[*toolIndex].variables.size())
        {
            return false;
        }
        const auto& variable =
            prepared->tools()[*toolIndex].variables[
                static_cast<std::size_t>(selectedIndex)];
        double number = 0.0;
        auto boolean = false;
        if (variable.kind
            == talas::project::ProjectLiveToolVariableKind::Boolean)
        {
            boolean = liveToolVariableToggle_.getToggleState();
        }
        else
        {
            const auto text = liveToolVariableValue_.getText().toStdString();
            const auto parsed = std::from_chars(
                text.data(), text.data() + text.size(), number,
                std::chars_format::general);
            if (parsed.ec != std::errc{}
                || parsed.ptr != text.data() + text.size()
                || !std::isfinite(number))
            {
                onStatus_("Live Tool variable requires one exact finite number");
                return false;
            }
        }
        const auto generation = bridge_.latestStageGeneration();
        const auto result = bridge_.requestLiveToolTrigger({
            .toolId = tool->toolId,
            .triggerKind =
                talas::project::ProjectLiveToolTriggerKind::VariableChange,
            .sourceKind = talas::runtime::RealtimeLiveToolSourceKind::Variable,
            .identifier = variable.variableId,
            .variableNumber = number,
            .variableBoolean = boolean,
            .hasVariableValue = true,
            .sourceGeneration = generation,
            .routeGeneration = generation,
        });
        onStatus_(result.ok()
                ? "Queued Live Tool variable " + juceString(variable.variableId)
                : "Live Tool variable rejected: "
                    + juceString(result.message));
        return result.ok();
    }

    [[nodiscard]] std::optional<talas::app::LiveGridCellAction>
    firstAvailableStopAction(
        const talas::app::LiveGridSourceContext& source,
        const talas::app::LiveGridProjectionEnvironment& environment) const
    {
        for (const auto& liveSet :
             bridge_.editor().document().liveSetLibrary.liveSets)
        {
            for (const auto& cell : liveSet.cells)
            {
                if (!std::holds_alternative<
                        talas::project::ProjectLiveSetStopTarget>(cell.target))
                {
                    continue;
                }
                auto action = talas::app::liveGridCellAction(
                    bridge_, liveSet.liveSetId, cell.cellId, source, environment);
                if (action.canActivate)
                    return action;
            }
        }
        return std::nullopt;
    }

    void refreshLiveGrid()
    {
        const auto midiIdentifiers = midiSourceIdentifiers_
            ? midiSourceIdentifiers_()
            : std::vector<std::string>{};
        const auto source = workbenchLiveGridSource();
        const auto previousSession = session_;
        const auto telemetry = bridge_.realtimeLiveSetTelemetry();
        const auto prepared = bridge_.latestPreparedLiveSet();
        const auto activeIdentityChanged =
            telemetry.activeRequestGeneration != 0
            && telemetry.activeRequestGeneration
                != observedLiveGridActiveRequestGeneration_;
        if (prepared != nullptr && telemetry.activeCellIndexPlusOne != 0
            && telemetry.activeCellIndexPlusOne <= prepared->cells().size())
        {
            const auto& active = prepared->cells()[
                telemetry.activeCellIndexPlusOne - 1U];
            session_.activeLiveSetId = active.liveSetId;
            session_.activeLiveSetCellId = active.cellId;
            if (activeIdentityChanged)
            {
                session_.selectedLiveSetId = active.liveSetId;
                session_.selectedLiveSetCellId = active.cellId;
            }
        }
        else if (telemetry.activeCellIndexPlusOne == 0)
        {
            session_.activeLiveSetId.reset();
            session_.activeLiveSetCellId.reset();
        }
        if (session_.queuedLiveSetRequestGeneration != 0
            && telemetry.terminalRequestGeneration
                >= session_.queuedLiveSetRequestGeneration)
        {
            if (telemetry.terminalRequestGeneration
                    == session_.queuedLiveSetRequestGeneration
                && telemetry.terminalOutcome
                    != talas::runtime::RealtimeLiveSetTerminalOutcome::Completed)
            {
                onStatus_(telemetry.terminalOutcome
                            == talas::runtime::RealtimeLiveSetTerminalOutcome::StaleAuthority
                        ? "Live Grid request became stale before execution"
                        : "Live Grid request failed before execution");
            }
            session_.queuedLiveSetId.reset();
            session_.queuedLiveSetCellId.reset();
            session_.queuedLiveSetRequestGeneration = 0;
        }
        observedLiveGridActiveRequestGeneration_ =
            telemetry.activeRequestGeneration;
        reconcileLiveGridSetSelector();
        const auto projectedLiveSetId = session_.selectedLiveSetId.has_value()
            ? std::string_view{*session_.selectedLiveSetId}
            : std::string_view{};
        const auto environment = liveGridEnvironment(
            midiIdentifiers, projectedLiveSetId);
        const auto projection = talas::app::liveGridProjection(
            bridge_, source, environment);

        const auto sameStructure = projection.size()
                == static_cast<std::size_t>(liveGridCells_.size())
            && std::ranges::equal(
                projection,
                liveGridCells_,
                {},
                &talas::app::LiveGridCellAction::componentId,
                [](const auto* button) -> std::string_view {
                    return button == nullptr
                        ? std::string_view{}
                        : button->action().componentId;
                });
        if (sameStructure)
        {
            for (std::size_t index = 0; index < projection.size(); ++index)
                liveGridCells_[static_cast<int>(index)]->setAction(
                    projection[index]);
        }
        else
        {
            juce::String focusedIdentity;
            if (auto* focused = juce::Component::getCurrentlyFocusedComponent();
                focused != nullptr && liveGridContainer_.isParentOf(focused))
            {
                focusedIdentity = focused->getComponentID();
            }
            liveGridCells_.clear(true);
            for (auto action : projection)
            {
                auto* button = liveGridCells_.add(new LiveGridCellButton(
                    std::move(action),
                    [this](std::string_view liveSetId,
                           std::string_view cellId) {
                        (void)activateLiveGridCell(liveSetId, cellId);
                    }));
                liveGridContainer_.addAndMakeVisible(button);
            }
            if (focusedIdentity.isNotEmpty())
            {
                const auto replacement = std::ranges::find_if(
                    liveGridCells_, [&](const auto* button) {
                        return button != nullptr
                            && button->getComponentID() == focusedIdentity;
                    });
                if (replacement != liveGridCells_.end())
                    (*replacement)->grabKeyboardFocus();
            }
        }
        if ((!session_.selectedLiveSetId.has_value()
                || !session_.selectedLiveSetCellId.has_value())
            && !liveGridCells_.isEmpty())
        {
            const auto& first = liveGridCells_.getFirst()->action();
            session_.selectedLiveSetId = first.liveSetId;
            session_.selectedLiveSetCellId = first.cellId;
        }
        stopLiveGridButton_.setEnabled(
            firstAvailableStopAction(source, environment).has_value());
        if (session_ != previousSession && onSessionChange_)
            onSessionChange_();
        resized();
        if (activeIdentityChanged)
        {
            if (auto* active = selectedLiveGridButton(); active != nullptr)
                scrollLiveGridButtonIntoView(*active, false);
        }
    }

    [[nodiscard]] bool activateLiveGridCell(
        std::string_view liveSetId,
        std::string_view cellId) override
    {
        const auto midiIdentifiers = midiSourceIdentifiers_
            ? midiSourceIdentifiers_()
            : std::vector<std::string>{};
        auto result = talas::app::dispatchLiveGridCell(
            bridge_,
            liveSetId,
            cellId,
            workbenchLiveGridSource(),
            liveGridEnvironment(midiIdentifiers));
        session_.selectedLiveSetId = std::string{liveSetId};
        session_.selectedLiveSetCellId = std::string{cellId};
        if (result.activation.ok())
        {
            session_.queuedLiveSetId = std::string{liveSetId};
            session_.queuedLiveSetCellId = std::string{cellId};
            session_.queuedLiveSetRequestGeneration =
                result.activation.requestGeneration;
            onStatus_(
                "Queued Live Grid cell " + juceString(liveSetId) + " / "
                + juceString(cellId) + " at frame "
                + juce::String(result.activation.resolvedFrame));
        }
        else
        {
            onStatus_(
                "Live Grid rejected " + juceString(liveSetId) + " / "
                + juceString(cellId) + ": "
                + juceString(result.action.reason));
        }
        if (onSessionChange_)
            onSessionChange_();
        refreshLiveGrid();
        return result.activation.ok();
    }

    [[nodiscard]] bool activateSelectedLiveGridCell()
    {
        return session_.selectedLiveSetId.has_value()
            && session_.selectedLiveSetCellId.has_value()
            && activateLiveGridCell(
                *session_.selectedLiveSetId,
                *session_.selectedLiveSetCellId);
    }

    void activateStopCell()
    {
        const auto midiIdentifiers = midiSourceIdentifiers_
            ? midiSourceIdentifiers_()
            : std::vector<std::string>{};
        const auto source = workbenchLiveGridSource();
        const auto stop = firstAvailableStopAction(
            source, liveGridEnvironment(midiIdentifiers));
        if (!stop.has_value())
        {
            onStatus_("No exact prepared Stop cell is available");
            return;
        }
        (void)activateLiveGridCell(stop->liveSetId, stop->cellId);
    }

    [[nodiscard]] LiveGridCellButton* selectedLiveGridButton() const
    {
        const auto selected = std::ranges::find_if(
            liveGridCells_, [&](const auto* button) {
                return button != nullptr
                    && session_.selectedLiveSetId
                        == std::optional<std::string>{button->action().liveSetId}
                    && session_.selectedLiveSetCellId
                        == std::optional<std::string>{button->action().cellId};
            });
        return selected == liveGridCells_.end() ? nullptr : *selected;
    }

    [[nodiscard]] LiveGridCellButton* activeLiveGridButton() const
    {
        if (!session_.activeLiveSetId.has_value()
            || !session_.activeLiveSetCellId.has_value())
        {
            return nullptr;
        }
        const auto active = std::ranges::find_if(
            liveGridCells_, [&](const auto* button) {
                return button != nullptr
                    && session_.activeLiveSetId
                        == std::optional<std::string>{button->action().liveSetId}
                    && session_.activeLiveSetCellId
                        == std::optional<std::string>{button->action().cellId};
            });
        return active == liveGridCells_.end() ? nullptr : *active;
    }

    [[nodiscard]] bool liveGridCellsOccupyUniquePositions() const
    {
        for (int left = 0; left < liveGridCells_.size(); ++left)
        {
            if (liveGridCells_[left] == nullptr)
                return false;
            for (int right = left + 1; right < liveGridCells_.size(); ++right)
            {
                if (liveGridCells_[right] != nullptr
                    && liveGridCells_[left]->action().row
                        == liveGridCells_[right]->action().row
                    && liveGridCells_[left]->action().column
                        == liveGridCells_[right]->action().column)
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] std::uint32_t selectedLiveGridRow() const
    {
        const auto selected = std::ranges::find_if(
            liveGridCells_, [&](const auto* button) {
                return button != nullptr
                    && session_.selectedLiveSetId
                        == std::optional<std::string>{
                            button->action().liveSetId}
                    && session_.selectedLiveSetCellId
                        == std::optional<std::string>{
                            button->action().cellId};
            });
        return selected == liveGridCells_.end()
            ? 0
            : (*selected)->action().row;
    }

    [[nodiscard]] std::uint32_t selectedLiveGridColumn() const
    {
        const auto selected = std::ranges::find_if(
            liveGridCells_, [&](const auto* button) {
                return button != nullptr
                    && session_.selectedLiveSetId
                        == std::optional<std::string>{
                            button->action().liveSetId}
                    && session_.selectedLiveSetCellId
                        == std::optional<std::string>{
                            button->action().cellId};
            });
        return selected == liveGridCells_.end()
            ? 0
            : (*selected)->action().column;
    }

    [[nodiscard]] bool selectLiveGridPosition(
        std::uint32_t row,
        std::uint32_t column)
    {
        const auto selected = std::ranges::find_if(
            liveGridCells_, [&](const auto* button) {
                return button != nullptr && button->action().row == row
                    && button->action().column == column;
            });
        if (selected == liveGridCells_.end())
            return false;
        const auto& action = (*selected)->action();
        session_.selectedLiveSetId = action.liveSetId;
        session_.selectedLiveSetCellId = action.cellId;
        scrollLiveGridButtonIntoView(**selected, true);
        if (onSessionChange_)
            onSessionChange_();
        return true;
    }

    void scrollLiveGridButtonIntoView(
        LiveGridCellButton& button,
        bool focus)
    {
        if (focus && button.isShowing())
            button.grabKeyboardFocus();
        const auto bounds = button.getBounds();
        const auto view = liveGridViewport_.getViewArea();
        liveGridViewport_.setViewPosition(
            std::clamp(bounds.getCentreX() - view.getWidth() / 2,
                0,
                std::max(0, liveGridContainer_.getWidth() - view.getWidth())),
            std::clamp(bounds.getCentreY() - view.getHeight() / 2,
                0,
                std::max(0,
                    liveGridContainer_.getHeight() - view.getHeight())));
    }

    [[nodiscard]] bool moveLiveGridSelection(int rowDelta, int columnDelta)
    {
        auto row = static_cast<int>(selectedLiveGridRow());
        auto column = static_cast<int>(selectedLiveGridColumn());
        const auto attempts = rowDelta != 0
            ? static_cast<int>(talas::project::projectLiveSetRows)
            : static_cast<int>(talas::project::projectLiveSetColumns);
        for (int attempt = 0; attempt < attempts; ++attempt)
        {
            row = (row + rowDelta
                      + static_cast<int>(talas::project::projectLiveSetRows))
                % static_cast<int>(talas::project::projectLiveSetRows);
            column = (column + columnDelta
                         + static_cast<int>(talas::project::projectLiveSetColumns))
                % static_cast<int>(talas::project::projectLiveSetColumns);
            if (selectLiveGridPosition(
                    static_cast<std::uint32_t>(row),
                    static_cast<std::uint32_t>(column)))
            {
                return true;
            }
        }
        return false;
    }

    void updateTabVisibility()
    {
        const auto showControls = session_.mode
            == talas::project::WorkspacePerformanceSessionState::Mode::Controls;
        const auto showSnapshots = session_.mode
            == talas::project::WorkspacePerformanceSessionState::Mode::Snapshots;
        const auto showLiveGrid = session_.mode
            == talas::project::WorkspacePerformanceSessionState::Mode::LiveGrid;
        controlsTab_.setToggleState(
            showControls,
            juce::dontSendNotification);
        snapshotsTab_.setToggleState(
            showSnapshots,
            juce::dontSendNotification);
        liveGridTab_.setToggleState(
            showLiveGrid,
            juce::dontSendNotification);
        contextLabel_.setVisible(showControls);
        const auto hasControlContent = !rows_.isEmpty()
            || selectedItemEditorVisible_;
        emptyLabel_.setVisible(
            showControls && !hasControlContent);
        viewport_.setVisible(
            showControls && hasControlContent);
        snapshotViewport_.setVisible(showSnapshots);
        liveGridViewport_.setVisible(showLiveGrid);
        liveGridSetSelector_.setVisible(showLiveGrid);
        stopLiveGridButton_.setVisible(showLiveGrid);
        liveToolSelector_.setVisible(showLiveGrid);
        liveToolStateLabel_.setVisible(showLiveGrid);
        liveToolManualButton_.setVisible(showLiveGrid);
        liveToolVariableSelector_.setVisible(showLiveGrid);
        liveToolVariableValue_.setVisible(
            showLiveGrid && !selectedLiveToolVariableIsBoolean_);
        liveToolVariableToggle_.setVisible(
            showLiveGrid && selectedLiveToolVariableIsBoolean_);
        liveToolVariableApply_.setVisible(showLiveGrid);
        resized();
    }

    talas::runtime::ProjectRuntimeBridge& bridge_;
    DocumentChangeCallback onDocumentChange_;
    StatusCallback onStatus_;
    StructureFocusCallback onStructureFocus_;
    CanvasSelectionCallback onCanvasSelection_;
    talas::project::WorkspacePerformanceSessionState& session_;
    SessionChangeCallback onSessionChange_;
    MidiSourceIdentifiersCallback midiSourceIdentifiers_;
    OscAvailabilityCallback oscAvailable_;
    std::string selectedInstanceId_;
    std::optional<std::string> selectedPerformanceControlId_;
    std::optional<std::string> selectedPerformanceMeterId_;
    talas::app::SnapshotWorkbenchCaptureContext captureContext_;
    juce::Label title_;
    juce::TextButton controlsTab_;
    juce::TextButton snapshotsTab_;
    juce::TextButton liveGridTab_;
    juce::Label contextLabel_;
    juce::Label emptyLabel_;
    juce::Label selectedItemTitle_;
    juce::Label selectedItemIdentity_;
    juce::TextEditor selectedItemName_;
    juce::ComboBox selectedItemKind_;
    juce::ToggleButton selectedItemVisible_;
    juce::TextButton selectedItemApply_;
    juce::TextButton selectedItemRemove_;
    juce::TextButton selectedItemStructure_;
    juce::Label modulationTitle_;
    juce::ComboBox modulationTarget_;
    juce::TextEditor modulationDepth_;
    juce::TextButton modulationAdd_;
    juce::TextButton modulationClear_;
    juce::Label modulationSummary_;
    std::vector<talas::project::SnapshotTargetIdentity>
        selectedModulationTargets_;
    juce::Viewport viewport_;
    juce::Component rowsContainer_;
    juce::OwnedArray<ControlRow> rows_;
    juce::Viewport snapshotViewport_;
    SnapshotPerformanceSurface snapshotSurface_;
    juce::Viewport liveGridViewport_;
    juce::Component liveGridContainer_;
    juce::OwnedArray<LiveGridCellButton> liveGridCells_;
    juce::ComboBox liveGridSetSelector_;
    juce::TextButton stopLiveGridButton_;
    juce::ComboBox liveToolSelector_;
    juce::Label liveToolStateLabel_;
    juce::TextButton liveToolManualButton_;
    juce::ComboBox liveToolVariableSelector_;
    juce::TextEditor liveToolVariableValue_;
    juce::ToggleButton liveToolVariableToggle_;
    juce::TextButton liveToolVariableApply_;
    std::optional<std::string> selectedLiveToolVariableId_;
    std::optional<talas::runtime::PreparedLiveToolRuntimeState>
        pendingLiveToolRestore_;
    std::optional<talas::runtime::PreparedLiveToolRuntimeState>
        lastSavedLiveToolRuntimeState_;
    std::uint64_t requestedLiveToolRestoreStageId_ = 0;
    std::uint64_t pendingLiveToolRestoreRequestGeneration_ = 0;
    bool selectedLiveToolVariableIsBoolean_ = false;
    std::uint64_t observedLiveGridActiveRequestGeneration_ = 0;
    bool selectedItemEditorVisible_ = false;
};

}

std::unique_ptr<PerformancePanel> makePerformancePanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    PerformancePanel::DocumentChangeCallback onDocumentChange,
    PerformancePanel::StatusCallback onStatus,
    PerformancePanel::StructureFocusCallback onStructureFocus,
    PerformancePanel::CanvasSelectionCallback onCanvasSelection,
    talas::project::WorkspacePerformanceSessionState& session,
    PerformancePanel::SessionChangeCallback onSessionChange,
    PerformancePanel::MidiSourceIdentifiersCallback midiSourceIdentifiers,
    PerformancePanel::OscAvailabilityCallback oscAvailable)
{
    return std::make_unique<PerformancePanelImpl>(
        bridge,
        registry,
        std::move(onDocumentChange),
        std::move(onStatus),
        std::move(onStructureFocus),
        std::move(onCanvasSelection),
        session,
        std::move(onSessionChange),
        std::move(midiSourceIdentifiers),
        std::move(oscAvailable));
}
}
