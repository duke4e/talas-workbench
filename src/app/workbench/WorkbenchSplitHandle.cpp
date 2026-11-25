#include "app/workbench/WorkbenchSplitHandle.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include <utility>

namespace talas::app::workbench
{
namespace
{
class WorkspaceSplitHandleImpl final : public WorkspaceSplitHandle
{
public:
    WorkspaceSplitHandleImpl(
        ChangeCallback onChange,
        ResetCallback onReset)
        : onChange_(std::move(onChange))
        , onReset_(std::move(onReset))
    {
        setWantsKeyboardFocus(true);
        describeControl(
            *this,
            "Structure pane divider",
            "Drag to resize the two Structure graph panes. Use arrow keys for precise adjustment; double-click to restore an even split.");
        updateCursor();
    }

    void setVerticalDivider(bool vertical) override
    {
        if (verticalDivider_ == vertical)
            return;
        verticalDivider_ = vertical;
        updateCursor();
        repaint();
    }

    // Between glass panels the divider is a short grip; focused, it becomes
    // the full-length focus line.
    void paint(juce::Graphics& graphics) override
    {
        const auto focused = hasKeyboardFocus(false);
        const auto bounds = getLocalBounds().toFloat();
        if (focused)
        {
            graphics.setColour(workbenchTheme.focus);
            if (verticalDivider_)
                graphics.drawLine(bounds.getCentreX(), 2.0F, bounds.getCentreX(), bounds.getBottom() - 2.0F, 2.0F);
            else
                graphics.drawLine(2.0F, bounds.getCentreY(), bounds.getRight() - 2.0F, bounds.getCentreY(), 2.0F);
            return;
        }
        graphics.setColour(WorkbenchStyle::enhancedContrast()
                ? (isMouseOver() ? workbenchTheme.enhancedOutline : panelBorderColour)
                : juce::Colours::white.withAlpha(isMouseOver() ? 0.4F : 0.16F));
        const auto grip = verticalDivider_
            ? bounds.withSizeKeepingCentre(3.0F, std::min(36.0F, bounds.getHeight() - 4.0F))
            : bounds.withSizeKeepingCentre(std::min(36.0F, bounds.getWidth() - 4.0F), 3.0F);
        graphics.fillRoundedRectangle(grip, 1.5F);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();
        publish(event);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        publish(event);
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (onReset_)
            onReset_();
    }

    void mouseEnter(const juce::MouseEvent&) override
    {
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        repaint();
    }

    void focusGained(FocusChangeType) override
    {
        repaint();
    }

    void focusLost(FocusChangeType) override
    {
        repaint();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        auto target = getBounds().getCentre();
        const auto step = key.getModifiers().isShiftDown() ? 1 : 16;
        if (verticalDivider_)
        {
            if (key.getKeyCode() == juce::KeyPress::leftKey)
                target.x -= step;
            else if (key.getKeyCode() == juce::KeyPress::rightKey)
                target.x += step;
            else
                return false;
        }
        else
        {
            if (key.getKeyCode() == juce::KeyPress::upKey)
                target.y -= step;
            else if (key.getKeyCode() == juce::KeyPress::downKey)
                target.y += step;
            else
                return false;
        }
        if (onChange_)
            onChange_(target);
        return true;
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::slider);
    }

private:
    void publish(const juce::MouseEvent& event)
    {
        if (onChange_ && getParentComponent() != nullptr)
        {
            onChange_(
                event.getEventRelativeTo(
                    getParentComponent()).getPosition());
        }
    }

    void updateCursor()
    {
        setMouseCursor(
            verticalDivider_
                ? juce::MouseCursor::LeftRightResizeCursor
                : juce::MouseCursor::UpDownResizeCursor);
    }

    ChangeCallback onChange_;
    ResetCallback onReset_;
    bool verticalDivider_ = true;
};
}

std::unique_ptr<WorkspaceSplitHandle>
makeWorkspaceSplitHandle(
    WorkspaceSplitHandle::ChangeCallback onChange,
    WorkspaceSplitHandle::ResetCallback onReset)
{
    return std::make_unique<WorkspaceSplitHandleImpl>(
        std::move(onChange),
        std::move(onReset));
}
}
