#include "app/workbench/WorkbenchContextHelpPanel.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include <algorithm>
#include <utility>

namespace talas::app::workbench
{
namespace
{
juce::Colour stateColour(
    const std::optional<talas::app::WorkbenchSurfaceState>& state)
{
    if (!state.has_value())
        return outputColour;
    switch (state->severity())
    {
    case talas::app::WorkbenchSurfaceSeverity::Neutral: return outputColour;
    case talas::app::WorkbenchSurfaceSeverity::Progress: return controlColour;
    case talas::app::WorkbenchSurfaceSeverity::Warning: return eventColour;
    case talas::app::WorkbenchSurfaceSeverity::Error: return errorColour;
    case talas::app::WorkbenchSurfaceSeverity::Success: return audioColour;
    }
    return outputColour;
}

class ContextHelpPanelImpl final : public ContextHelpPanel
{
public:
    ContextHelpPanelImpl()
    {
        setComponentID("context-help.panel");
        describeControl(
            *this,
            "Context Help",
            "Compact help for the currently focused Workbench control. Toggle it from the View menu.");
        for (auto* label : {&heading_, &state_, &keyboard_, &support_, &next_})
        {
            addAndMakeVisible(*label);
            label->setInterceptsMouseClicks(false, false);
            label->setColour(juce::Label::textColourId, textColour);
        }
        heading_.setFont(WorkbenchType::panelTitle());
        state_.setFont(WorkbenchType::label());
        keyboard_.setFont(WorkbenchType::microStrong());
        support_.setFont(WorkbenchType::micro());
        support_.setColour(juce::Label::textColourId, secondaryTextColour);
        next_.setFont(WorkbenchType::microStrong());
        clearContent();
    }

    void setContent(WorkbenchContextHelpContent content) override
    {
        if (!content.surfaceState.has_value())
        {
            clearContent();
            return;
        }
        if (content == content_)
            return;
        const auto previousState = content_.surfaceState;
        content_ = std::move(content);
        const auto stateName = content_.surfaceState.has_value()
            ? juceString(talas::app::workbenchSurfaceStateName(
                  content_.surfaceState->kind()))
            : juce::String{"Content"};
        heading_.setText(
            "Context Help  |  " + content_.title + "  |  " + content_.role,
            juce::dontSendNotification);
        state_.setText(
            stateName + "  |  " + content_.currentValue,
            juce::dontSendNotification);
        keyboard_.setText(
            "Keyboard  " + content_.primaryKeyboardAction,
            juce::dontSendNotification);
        support_.setText(
            content_.supportOrLimitation,
            juce::dontSendNotification);
        next_.setText(
            "Next  " + content_.exactNextAction,
            juce::dontSendNotification);

        const auto accessible = content_.title + ". Role: " + content_.role
            + ". State: " + stateName + ", " + content_.currentValue
            + ". Keyboard: " + content_.primaryKeyboardAction
            + ". Support: " + content_.supportOrLimitation
            + ". Next: " + content_.exactNextAction;
        setTitle(accessible);
        setDescription(accessible);
        repaint();
        if (auto* handler = getAccessibilityHandler())
        {
            handler->notifyAccessibilityEvent(
                juce::AccessibilityEvent::titleChanged);
        }
        if (hasPublishedContent_
            && talas::app::shouldAnnounceWorkbenchSurfaceState(
                previousState, *content_.surfaceState))
        {
            ++announcementCount_;
            juce::AccessibilityHandler::postAnnouncement(
                stateName + ": " + content_.title + ". "
                    + content_.currentValue,
                content_.surfaceState->announcement()
                        == talas::app::WorkbenchSurfaceAnnouncement::Assertive
                    ? juce::AccessibilityHandler::AnnouncementPriority::high
                    : juce::AccessibilityHandler::AnnouncementPriority::low);
        }
        hasPublishedContent_ = true;
    }

    void clearContent() override
    {
        const auto empty = talas::app::workbenchSurfaceState({
            talas::app::WorkbenchSurfaceStateKind::Empty,
            "No focused control",
            "Move keyboard focus into the Workbench to inspect exact context help.",
            {},
            false,
        });
        setContent({
            "context-help.none",
            "No focused control",
            "Workbench",
            "Nothing is currently focused",
            "Press Tab",
            "Help follows stable Workbench focus identities and never changes the project.",
            "Press Tab to focus the first available control",
            empty,
        });
    }

    [[nodiscard]] bool contentContainsForSmoke(
        juce::StringRef stableIdentity,
        juce::StringRef requiredText) const override
    {
        const auto combined = heading_.getText() + " " + state_.getText()
            + " " + keyboard_.getText() + " " + support_.getText()
            + " " + next_.getText();
        return content_.stableIdentity == juce::String{stableIdentity}
            && combined.contains(juce::String{requiredText});
    }

    [[nodiscard]] std::string contentSummaryForSmoke() const override
    {
        return (content_.stableIdentity + " | " + heading_.getText()
                + " | " + keyboard_.getText())
            .toStdString();
    }

    [[nodiscard]] std::uint64_t announcementCountForSmoke()
        const noexcept override
    {
        return announcementCount_;
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::group);
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto radius = WorkbenchStyle::panelRadius();
        WorkbenchStyle::paintPanel(graphics, getLocalBounds().toFloat(), radius);
        // The surface state's colour as a bar down the left edge.
        graphics.setColour(stateColour(content_.surfaceState));
        graphics.fillRoundedRectangle(getLocalBounds().toFloat()
                                          .removeFromLeft(static_cast<float>(workbenchScaledUiPixels(4)))
                                          .reduced(0.0F, radius * 0.6F)
                                          .translated(2.0F, 0.0F),
            2.0F);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(
            workbenchScaledUiPixels(12), workbenchScaledUiPixels(7));
        auto top = bounds.removeFromTop(workbenchScaledUiPixels(22));
        heading_.setBounds(top.removeFromLeft(
            std::max(0, top.getWidth() * 56 / 100)));
        state_.setBounds(top);
        auto bottom = bounds.removeFromBottom(workbenchScaledUiPixels(22));
        keyboard_.setBounds(bottom.removeFromLeft(
            std::max(0, bottom.getWidth() * 34 / 100)));
        next_.setBounds(bottom);
        support_.setBounds(bounds);
    }

private:
    WorkbenchContextHelpContent content_;
    juce::Label heading_;
    juce::Label state_;
    juce::Label keyboard_;
    juce::Label support_;
    juce::Label next_;
    std::uint64_t announcementCount_ = 0;
    bool hasPublishedContent_ = false;
};
}

std::unique_ptr<ContextHelpPanel> makeContextHelpPanel()
{
    return std::make_unique<ContextHelpPanelImpl>();
}
}
