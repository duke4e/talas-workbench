#include "app/workbench/WorkbenchComponentReplacementPanel.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/WorkbenchSurfaceState.h"

#include <algorithm>
#include <array>
#include <utility>

namespace talas::app::workbench
{
namespace
{
class ComponentReplacementPanelImpl final
    : public ComponentReplacementPanel
{
public:
    explicit ComponentReplacementPanelImpl(
        ComponentReplacementPanelCallbacks callbacks)
        : callbacks_(std::move(callbacks))
        , chooseButton_("Choose another...")
        , confirmButton_("Confirm replacement")
        , cancelButton_("Cancel")
    {
        describeControl(
            *this,
            "Component replacement preview",
            "Review one exact local component candidate before isolated activation and graph staging.");
        setFocusContainerType(
            juce::Component::FocusContainerType::none);

        headline_.setColour(juce::Label::textColourId, textColour);
        headline_.setFont(WorkbenchType::labelStrong());
        headline_.setJustificationType(juce::Justification::centredLeft);
        headline_.setMinimumHorizontalScale(1.0F);

        status_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        status_.setFont(WorkbenchType::microStrong());
        status_.setJustificationType(juce::Justification::topLeft);
        status_.setMinimumHorizontalScale(1.0F);

        details_.setMultiLine(true);
        details_.setReadOnly(true);
        details_.setScrollbarsShown(true);
        details_.setCaretVisible(false);
        details_.setFont(WorkbenchType::label());
        details_.setColour(
            juce::TextEditor::backgroundColourId,
            workbenchTheme.controlTile);
        details_.setColour(
            juce::TextEditor::outlineColourId,
            panelBorderColour);
        details_.setColour(
            juce::TextEditor::textColourId,
            secondaryTextColour);
        describeControl(
            details_,
            "Exact component replacement details",
            "Copyable full bundle, version, signer, content, permission, interface, state, provenance, and resource transition details.");

        describeControl(
            chooseButton_,
            "Choose another component bundle",
            "Close this candidate and inspect one explicitly selected local bundle directory.");
        describeControl(
            confirmButton_,
            "Confirm exact component replacement",
            "Confirm this exact candidate once. Approval remains local to this Workbench session.");
        describeControl(
            cancelButton_,
            "Cancel component replacement",
            "Cancel inspection, probing, activation, or the retained staged replacement without changing project bytes.");

        details_.setExplicitFocusOrder(1);
        chooseButton_.setExplicitFocusOrder(2);
        confirmButton_.setExplicitFocusOrder(3);
        cancelButton_.setExplicitFocusOrder(4);

        addAndMakeVisible(headline_);
        addAndMakeVisible(status_);
        addAndMakeVisible(details_);
        addAndMakeVisible(chooseButton_);
        addAndMakeVisible(confirmButton_);
        addAndMakeVisible(cancelButton_);

        chooseButton_.onClick = [this] {
            if (callbacks_.choose)
                callbacks_.choose();
        };
        confirmButton_.onClick = [this] {
            if (callbacks_.confirm)
                callbacks_.confirm();
        };
        cancelButton_.onClick = [this] {
            if (callbacks_.cancel)
                callbacks_.cancel();
        };
        setPresentation({});
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

    void setPresentation(
        const ComponentReplacementPresentation& presentation) override
    {
        presentation_ = presentation;
        const auto surfaceKind = [](ComponentReplacementPresentationState state) {
            switch (state)
            {
            case ComponentReplacementPresentationState::Inspecting:
            case ComponentReplacementPresentationState::Probing:
            case ComponentReplacementPresentationState::Activating:
            case ComponentReplacementPresentationState::Staging:
                return talas::app::WorkbenchSurfaceStateKind::Loading;
            case ComponentReplacementPresentationState::Succeeded:
                return talas::app::WorkbenchSurfaceStateKind::Success;
            case ComponentReplacementPresentationState::Failed:
                return talas::app::WorkbenchSurfaceStateKind::Error;
            case ComponentReplacementPresentationState::Hidden:
            case ComponentReplacementPresentationState::Cancelled:
                return talas::app::WorkbenchSurfaceStateKind::Empty;
            case ComponentReplacementPresentationState::AwaitingOwnerApproval:
            case ComponentReplacementPresentationState::AwaitingConfirmation:
                return talas::app::WorkbenchSurfaceStateKind::Content;
            }
            return talas::app::WorkbenchSurfaceStateKind::Unavailable;
        };
        const auto kind = surfaceKind(presentation_.state);
        const auto retainsContent = kind == talas::app::WorkbenchSurfaceStateKind::Content
            || kind == talas::app::WorkbenchSurfaceStateKind::Error;
        surfaceState_ = talas::app::workbenchSurfaceState({
            kind,
            presentation_.headline.isNotEmpty()
                ? presentation_.headline.toStdString()
                : "Component replacement",
            presentation_.status.isNotEmpty()
                ? presentation_.status.toStdString()
                : "No component replacement is active.",
            presentation_.canConfirm
                ? std::optional<talas::app::WorkbenchSurfaceActionEvidence>{
                      {"component-replacement.confirm",
                       presentation_.confirmLabel.isNotEmpty()
                           ? presentation_.confirmLabel.toStdString()
                           : "Confirm replacement",
                       true}}
                : presentation_.canCancel
                    ? std::optional<talas::app::WorkbenchSurfaceActionEvidence>{
                          {"component-replacement.cancel", "Cancel", true}}
                    : std::nullopt,
            retainsContent,
        });
        setVisible(presentation_.visible());
        headline_.setText(
            presentation_.headline,
            juce::dontSendNotification);
        status_.setText(
            presentation_.status,
            juce::dontSendNotification);
        status_.setTitle(
            surfaceState_.has_value()
                ? juceString(talas::app::workbenchSurfaceStateName(
                      surfaceState_->kind()))
                    + ": " + presentation_.status
                : presentation_.status);
        status_.setColour(
            juce::Label::textColourId,
            statusColour(presentation_.state));
        const auto statusAlreadyShown = presentation_.status.isNotEmpty()
            && presentation_.details.containsIgnoreCase(
                presentation_.status);
        const auto fullDetails = presentation_.details
            + (presentation_.status.isNotEmpty() && !statusAlreadyShown
                    ? "\n\nStatus: " + presentation_.status
                    : juce::String{});
        details_.setText(fullDetails, false);
        details_.setCaretPosition(0);
        details_.setTitle(
            "Exact component replacement details: "
            + fullDetails.replaceCharacters("\n\r", "  "));
        chooseButton_.setVisible(presentation_.canChoose);
        chooseButton_.setEnabled(presentation_.canChoose);
        confirmButton_.setVisible(presentation_.canConfirm);
        confirmButton_.setEnabled(presentation_.canConfirm);
        confirmButton_.setButtonText(
            presentation_.confirmLabel.isNotEmpty()
                ? presentation_.confirmLabel
                : "Confirm replacement");
        cancelButton_.setVisible(presentation_.canCancel);
        cancelButton_.setEnabled(presentation_.canCancel);
        cancelButton_.setButtonText(
            presentation_.state
                        == ComponentReplacementPresentationState::Succeeded
                    || presentation_.state
                        == ComponentReplacementPresentationState::Failed
                    || presentation_.state
                        == ComponentReplacementPresentationState::Cancelled
                ? "Close"
                : "Cancel");
        resized();
        repaint();
    }

    [[nodiscard]] bool showsAccessiblePreviewForSmoke(
        const juce::String& expectedText,
        bool confirmationExpected) const override
    {
        return isVisible() && details_.isReadOnly()
            && details_.getText().contains(expectedText)
            && details_.getTitle().contains(expectedText)
            && confirmButton_.isVisible() == confirmationExpected
            && confirmButton_.isEnabled() == confirmationExpected
            && cancelButton_.isVisible();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
        graphics.setColour(panelBorderColour);
        graphics.drawRect(getLocalBounds(), 1);
    }

    void resized() override
    {
        auto content = getLocalBounds().reduced(8);
        headline_.setBounds(content.removeFromTop(18));
        content.removeFromTop(4);
        status_.setBounds(content.removeFromTop(32));
        content.removeFromTop(5);

        const auto visibleButtonCount = static_cast<int>(
            chooseButton_.isVisible())
            + static_cast<int>(confirmButton_.isVisible())
            + static_cast<int>(cancelButton_.isVisible());
        const auto compact = getWidth() < 300;
        const auto actionHeight = visibleButtonCount == 0
            ? 0
            : (compact && confirmButton_.isVisible() ? 56 : 24);
        auto actions = actionHeight == 0
            ? juce::Rectangle<int>{}
            : content.removeFromBottom(actionHeight);
        if (actionHeight != 0)
            content.removeFromBottom(6);
        details_.setBounds(content);

        chooseButton_.setBounds({});
        confirmButton_.setBounds({});
        cancelButton_.setBounds({});
        if (actionHeight == 0)
            return;

        if (compact && confirmButton_.isVisible())
        {
            confirmButton_.setBounds(actions.removeFromTop(24));
            actions.removeFromTop(8);
        }
        const auto buttons = std::array<juce::TextButton*, 3>{
            &chooseButton_,
            &confirmButton_,
            &cancelButton_,
        };
        auto remaining = visibleButtonCount
            - (compact && confirmButton_.isVisible() ? 1 : 0);
        for (auto* button : buttons)
        {
            if (!button->isVisible()
                || (compact && button == &confirmButton_))
            {
                continue;
            }
            const auto width = remaining > 1
                ? (actions.getWidth() - 6 * (remaining - 1)) / remaining
                : actions.getWidth();
            button->setBounds(actions.removeFromLeft(width));
            --remaining;
            if (remaining > 0)
                actions.removeFromLeft(6);
        }
    }

private:
    [[nodiscard]] static juce::Colour statusColour(
        ComponentReplacementPresentationState state)
    {
        switch (state)
        {
        case ComponentReplacementPresentationState::AwaitingOwnerApproval:
        case ComponentReplacementPresentationState::AwaitingConfirmation:
            return workbenchTheme.control;
        case ComponentReplacementPresentationState::Succeeded:
            return workbenchTheme.fullSupport;
        case ComponentReplacementPresentationState::Failed:
            return errorColour;
        case ComponentReplacementPresentationState::Cancelled:
        case ComponentReplacementPresentationState::Hidden:
        case ComponentReplacementPresentationState::Inspecting:
        case ComponentReplacementPresentationState::Probing:
        case ComponentReplacementPresentationState::Activating:
        case ComponentReplacementPresentationState::Staging:
            return secondaryTextColour;
        }
        return secondaryTextColour;
    }

    ComponentReplacementPanelCallbacks callbacks_;
    ComponentReplacementPresentation presentation_;
    std::optional<talas::app::WorkbenchSurfaceState> surfaceState_;
    juce::Label headline_;
    juce::Label status_;
    juce::TextEditor details_;
    juce::TextButton chooseButton_;
    juce::TextButton confirmButton_;
    juce::TextButton cancelButton_;
};
}

std::unique_ptr<ComponentReplacementPanel>
makeComponentReplacementPanel(
    ComponentReplacementPanelCallbacks callbacks)
{
    return std::make_unique<ComponentReplacementPanelImpl>(
        std::move(callbacks));
}
}
