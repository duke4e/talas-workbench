#include "app/workbench/WorkbenchSoundBrowserSurface.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/VcsLayout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The surface of the Sound the Sound Browser plays,
// in a window of its own beside the project's Virtual Control Surface.
namespace talas::app::workbench
{
namespace
{
constexpr int cellWidth = 84;
constexpr int cellHeight = 220;
constexpr int cellGap = 8;

// A gate: at its maximum while held, at its minimum once let go.
class GateButton final : public juce::TextButton
{
public:
    std::function<void(bool)> onHeld;

    void mouseDown(const juce::MouseEvent& event) override
    {
        juce::TextButton::mouseDown(event);
        if (isEnabled() && onHeld)
            onHeld(true);
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        juce::TextButton::mouseUp(event);
        if (isEnabled() && onHeld)
            onHeld(false);
    }
};

// One widget: its name, which a click freezes against the dice or makes
// active again, greyed while frozen; beneath it a fader with its
// number at its top, where a value typed and Enter take effect, a toggle
// or a gate.
class SurfaceWidget final : public juce::Component
{
public:
    std::function<void(double)> onValue;
    std::function<void()> onName;

    SurfaceWidget()
    {
        addAndMakeVisible(name_);
        addChildComponent(slider_);
        addChildComponent(toggle_);
        addChildComponent(gate_);
        name_.onClick = [this] {
            if (onName)
                onName();
        };
        slider_.onValueChange = [this] {
            if (!updating_ && onValue)
                onValue(slider_.getValue());
        };
        toggle_.setClickingTogglesState(true);
        toggle_.onClick = [this] {
            if (!updating_ && onValue)
                onValue(toggle_.getToggleState() ? maximum_ : minimum_);
        };
        gate_.onHeld = [this](bool held) {
            if (onValue)
                onValue(held ? maximum_ : minimum_);
        };
    }

    void show(const talas::project::VirtualControl& control, bool frozen, bool takesChanges)
    {
        updating_ = true;
        minimum_ = control.minimum;
        maximum_ = control.maximum;
        const auto label = juceString(control.widget.label.empty() ? control.eventName : control.widget.label);
        name_.setButtonText(label);
        name_.setColour(juce::TextButton::textColourOffId, frozen ? secondaryTextColour.withAlpha(0.45F) : textColour);
        name_.setTooltip(frozen ? "Frozen against the dice; click to make it active again" : "Click to freeze it against the dice");
        using Kind = talas::project::VirtualControlKind;
        slider_.setVisible(control.kind == Kind::Fader);
        toggle_.setVisible(control.kind == Kind::Toggle);
        gate_.setVisible(control.kind == Kind::Gate);
        using Style = talas::project::VirtualControlWidgetStyle;
        slider_.setSliderStyle(control.widget.style == Style::HorizontalFader ? juce::Slider::LinearHorizontal
                : control.widget.style == Style::SmallFader                ? juce::Slider::LinearBar
                : control.widget.style == Style::Potentiometer             ? juce::Slider::RotaryHorizontalVerticalDrag
                                                                           : juce::Slider::LinearVertical);
        slider_.setRange(minimum_, maximum_, control.widget.grid > 0.0 ? control.widget.grid : 0.0);
        slider_.setTextBoxStyle(control.widget.showNumber ? juce::Slider::TextBoxAbove : juce::Slider::NoTextBox, false, cellWidth - 12, 20);
        slider_.setValue(std::clamp(control.value, minimum_, maximum_), juce::dontSendNotification);
        toggle_.setButtonText(label);
        toggle_.setToggleState(control.value >= maximum_, juce::dontSendNotification);
        gate_.setButtonText(label);
        for (auto* component : std::initializer_list<juce::Component*>{&name_, &slider_, &toggle_, &gate_})
            component->setEnabled(takesChanges);
        updating_ = false;
        resized();
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced(0.5F);
        graphics.setColour(workbenchTheme.widgetSurface);
        graphics.fillRoundedRectangle(bounds, 6.0F);
        graphics.setColour(workbenchTheme.widgetOutline.withAlpha(0.6F));
        graphics.drawRoundedRectangle(bounds, 6.0F, 1.0F);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(4);
        name_.setBounds(bounds.removeFromTop(22));
        bounds.removeFromTop(4);
        slider_.setBounds(bounds);
        toggle_.setBounds(bounds.withSizeKeepingCentre(bounds.getWidth(), std::min(bounds.getHeight(), 32)));
        gate_.setBounds(toggle_.getBounds());
    }

private:
    juce::TextButton name_;
    juce::Slider slider_;
    juce::TextButton toggle_;
    GateButton gate_;
    double minimum_ = 0.0;
    double maximum_ = 1.0;
    bool updating_ = false;
};

// Over the unlocked surface: a widget dragged by its body moves, and is
// put down where it is let go; a right-click on one asks for its type and
// range.
class EditOverlay final : public juce::Component
{
public:
    std::function<std::optional<std::size_t>(juce::Point<int>)> widgetAt;
    std::function<void(std::size_t, juce::Point<int>)> dragged;
    std::function<void(std::size_t)> dropped;
    std::function<void(std::size_t)> options;

    void paint(juce::Graphics& graphics) override
    {
        graphics.setColour(selectionColour.withAlpha(0.08F));
        graphics.fillAll();
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        dragging_ = widgetAt ? widgetAt(event.getPosition()) : std::nullopt;
        if (dragging_.has_value() && event.mods.isPopupMenu())
        {
            const auto index = *dragging_;
            dragging_.reset();
            if (options)
                options(index);
        }
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (dragging_.has_value() && dragged)
            dragged(*dragging_, event.getOffsetFromDragStart());
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragging_.has_value() && dropped)
            dropped(*dragging_);
        dragging_.reset();
    }

private:
    std::optional<std::size_t> dragging_;
};

class SoundBrowserSurfacePanelImpl final : public SoundBrowserSurfacePanel
{
public:
    explicit SoundBrowserSurfacePanelImpl(SoundBrowserSurfaceHost host)
        : host_(std::move(host))
    {
        title_.setFont(juce::Font{juce::FontOptions{16.0F}.withStyle("Bold")});
        title_.setColour(juce::Label::textColourId, textColour);
        note_.setColour(juce::Label::textColourId, secondaryTextColour);
        note_.setMinimumHorizontalScale(1.0F);
        note_.setJustificationType(juce::Justification::topLeft);
        for (auto* component : std::initializer_list<juce::Component*>{&title_, &note_, &dice_, &padlock_, &save_, &viewport_})
            addAndMakeVisible(*component);
        describeControl(dice_, "Dice", "Set every parameter that is not frozen to a random value (r).");
        describeControl(padlock_, "Padlock",
            "Unlock the surface to drag its widgets to new places, or right-click one for its type and range; lock it to play them.");
        describeControl(save_, "Save", "Save the surface's widgets, their places, types and ranges, with the Sound in its Sound file.");
        dice_.onClick = [this] { rollDice(); };
        padlock_.onClick = [this] { setEditing(!editing_); };
        save_.onClick = [this] { save(); };
        viewport_.setViewedComponent(&content_, false);
        viewport_.setScrollBarsShown(true, true);
        content_.addChildComponent(overlay_);
        overlay_.widgetAt = [this](juce::Point<int> point) -> std::optional<std::size_t> {
            for (std::size_t index = 0; index < widgets_.size(); ++index)
            {
                if (widgets_[index]->getBounds().contains(point))
                    return index;
            }
            return std::nullopt;
        };
        overlay_.dragged = [this](std::size_t index, juce::Point<int> offset) {
            if (!dragStart_.has_value())
                dragStart_ = widgets_[index]->getPosition();
            const auto to = *dragStart_ + offset;
            widgets_[index]->setTopLeftPosition(std::max(0, to.x), std::max(0, to.y));
        };
        overlay_.dropped = [this](std::size_t index) { dropWidget(index); };
        overlay_.options = [this](std::size_t index) { showWidgetOptions(index); };
        setWantsKeyboardFocus(true);
        setSize(560, 320);
        updateButtons();
    }

    void show(talas::app::SoundBrowserSurface surface) override
    {
        editing_ = false;
        surface_ = std::move(surface);
        widgets_.clear();
        for (const auto& control : surface_->widgets)
        {
            auto widget = std::make_unique<SurfaceWidget>();
            const auto name = control.eventName;
            widget->onValue = [this, name](double value) { setValue(name, value); };
            widget->onName = [this, name] { toggleFrozen(name); };
            content_.addAndMakeVisible(*widget);
            widgets_.push_back(std::move(widget));
        }
        refreshWidgets();
        updateButtons();
        layoutWidgets();
    }

    [[nodiscard]] const std::optional<talas::app::SoundBrowserSurface>& surface() const noexcept override { return surface_; }

    bool keyPressed(const juce::KeyPress& key) override
    {
        // R rolls the dice.
        if (key.getTextCharacter() == 'r' && !key.getModifiers().isAnyModifierKeyDown())
        {
            rollDice();
            return true;
        }
        return false;
    }

    void paint(juce::Graphics& graphics) override { graphics.fillAll(panelColour); }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        auto top = bounds.removeFromTop(28);
        save_.setBounds(top.removeFromRight(64));
        top.removeFromRight(6);
        padlock_.setBounds(top.removeFromRight(72));
        top.removeFromRight(6);
        dice_.setBounds(top.removeFromRight(56));
        top.removeFromRight(6);
        title_.setBounds(top);
        bounds.removeFromTop(4);
        note_.setBounds(bounds.removeFromTop(36));
        bounds.removeFromTop(4);
        viewport_.setBounds(bounds);
        layoutWidgets();
    }

private:
    [[nodiscard]] bool takesChanges() const
    {
        return surface_.has_value() && talas::app::soundBrowserSurfaceTakesChanges(*surface_);
    }

    void status(const juce::String& text) const
    {
        if (host_.status)
            host_.status(text);
    }

    void refreshWidgets()
    {
        if (!surface_.has_value())
            return;
        for (std::size_t index = 0; index < widgets_.size() && index < surface_->widgets.size(); ++index)
        {
            const auto& control = surface_->widgets[index];
            widgets_[index]->show(control, surface_->frozen.contains(control.eventName), takesChanges());
        }
    }

    void updateButtons()
    {
        const auto takes = takesChanges();
        dice_.setEnabled(takes);
        padlock_.setEnabled(takes);
        save_.setEnabled(takes);
        padlock_.setButtonText(editing_ ? "Lock" : "Unlock");
        title_.setText(surface_.has_value() ? juceString(surface_->name) : juce::String{"No Sound played"}, juce::dontSendNotification);
        note_.setText(surface_.has_value() ? juceString(talas::app::soundBrowserSurfaceNote(*surface_))
                                           : juce::String{"Play a Sound in the Sound Browser to see its surface"},
            juce::dontSendNotification);
        overlay_.setVisible(editing_);
    }

    // Where the widgets are shown: in columns across the window as long as
    // the surface has never been arranged; else each at its place, one
    // without a place after the rest, scaled into the window for Fit.
    [[nodiscard]] std::vector<talas::app::VcsRect> places() const
    {
        std::vector<talas::app::VcsRect> rects;
        if (!surface_.has_value())
            return rects;
        const auto width = std::max(1, viewport_.getMaximumVisibleWidth());
        if (surface_->arrangement == talas::project::VirtualControlLayout::Automatic)
        {
            const auto columns = static_cast<unsigned>(std::max(1, (width - cellGap) / (cellWidth + cellGap)));
            return talas::app::vcsCleanUp(surface_->widgets.size(), columns, cellWidth, cellHeight, cellGap);
        }
        for (const auto& control : surface_->widgets)
            rects.push_back(control.widget.bounds.value_or(talas::app::VcsRect{}));
        auto extent = talas::app::vcsExtent(rects);
        for (auto& rect : rects)
        {
            if (rect.width > 0)
                continue;
            rect = {extent.width + cellGap, 0, cellWidth, cellHeight};
            extent = talas::app::vcsExtent(rects);
        }
        if (surface_->arrangement == talas::project::VirtualControlLayout::Fit && !editing_)
            rects = talas::app::vcsFit(rects, width, std::max(1, viewport_.getMaximumVisibleHeight()));
        return rects;
    }

    void layoutWidgets()
    {
        const auto rects = places();
        for (std::size_t index = 0; index < widgets_.size() && index < rects.size(); ++index)
            widgets_[index]->setBounds(rects[index].left, rects[index].top, rects[index].width, rects[index].height);
        const auto extent = talas::app::vcsExtent(rects);
        content_.setSize(std::max(viewport_.getMaximumVisibleWidth(), extent.left + extent.width + cellGap),
            std::max(viewport_.getMaximumVisibleHeight(), extent.top + extent.height + cellGap));
        overlay_.setBounds(content_.getLocalBounds());
        overlay_.toFront(false);
    }

    [[nodiscard]] const talas::project::VirtualControl* widgetNamed(const std::string& name) const
    {
        if (!surface_.has_value())
            return nullptr;
        const auto found = std::ranges::find(surface_->widgets, name, &talas::project::VirtualControl::eventName);
        return found == surface_->widgets.end() ? nullptr : &*found;
    }

    void send(const talas::project::VirtualControl& control)
    {
        if (host_.sendValue && !host_.sendValue(control))
            status("The Sound no longer plays: play it again in the Sound Browser to hear its surface");
    }

    void setValue(const std::string& name, double value)
    {
        auto changed = surface_.has_value() ? talas::app::soundBrowserSurfaceValueSet(*surface_, name, value) : std::nullopt;
        if (!changed.has_value())
        {
            refreshWidgets();
            return;
        }
        surface_ = std::move(*changed);
        if (const auto* control = widgetNamed(name))
            send(*control);
    }

    void rollDice()
    {
        auto changed = surface_.has_value()
            ? talas::app::soundBrowserSurfaceDiceRolled(*surface_, static_cast<std::uint64_t>(juce::Random::getSystemRandom().nextInt64()))
            : std::nullopt;
        if (!changed.has_value())
        {
            if (surface_.has_value())
                status(juceString(talas::app::soundBrowserSurfaceNote(*surface_)));
            return;
        }
        surface_ = std::move(*changed);
        for (const auto& control : surface_->widgets)
        {
            if (!surface_->frozen.contains(control.eventName))
                send(control);
        }
        refreshWidgets();
        status("Rolled the dice on " + juceString(surface_->name));
    }

    void toggleFrozen(const std::string& name)
    {
        auto changed = surface_.has_value() ? talas::app::soundBrowserSurfaceFrozenToggled(*surface_, name) : std::nullopt;
        if (!changed.has_value())
            return;
        surface_ = std::move(*changed);
        refreshWidgets();
    }

    // Unlocked, a surface not yet arranged keeps its widgets where they are
    // shown, to be dragged from there.
    void setEditing(bool editing)
    {
        if (editing && !takesChanges())
            return;
        if (editing && surface_->arrangement != talas::project::VirtualControlLayout::Fixed && !surface_->widgets.empty())
        {
            const auto rects = places();
            std::vector<std::pair<std::string, talas::app::VcsRect>> placed;
            for (std::size_t index = 0; index < rects.size() && index < surface_->widgets.size(); ++index)
                placed.emplace_back(surface_->widgets[index].eventName, rects[index]);
            if (auto changed = talas::app::soundBrowserSurfacePlaced(*surface_, placed))
                surface_ = std::move(*changed);
        }
        editing_ = editing;
        updateButtons();
        layoutWidgets();
        status(editing_ ? juce::String{"Unlocked: drag the widgets to new places, then save them with the Sound"}
                        : juce::String{"Locked"});
    }

    void dropWidget(std::size_t index)
    {
        dragStart_.reset();
        if (!surface_.has_value() || index >= widgets_.size() || index >= surface_->widgets.size())
            return;
        const auto bounds = widgets_[index]->getBounds();
        auto changed = talas::app::soundBrowserSurfacePlaced(
            *surface_, {{surface_->widgets[index].eventName, {bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight()}}});
        if (changed.has_value())
            surface_ = std::move(*changed);
        layoutWidgets();
    }

    // A widget's type and range, asked for by a right-click on it
    // while the surface is unlocked.
    void showWidgetOptions(std::size_t index)
    {
        if (!surface_.has_value() || index >= surface_->widgets.size())
            return;
        const auto& control = surface_->widgets[index];
        auto* dialog = new juce::AlertWindow(
            "Widget: " + juceString(control.eventName), "Its type and range", juce::MessageBoxIconType::NoIcon, this);
        dialog->addComboBox("kind", {"Fader", "Gate", "Toggle"}, "Type");
        const auto shown = control.kind == talas::project::VirtualControlKind::Gate ? 1
            : control.kind == talas::project::VirtualControlKind::Toggle            ? 2
                                                                                 : 0;
        dialog->getComboBoxComponent("kind")->setSelectedItemIndex(shown, juce::dontSendNotification);
        dialog->addTextEditor("minimum", juce::String(control.minimum), "Min");
        dialog->addTextEditor("maximum", juce::String(control.maximum), "Max");
        dialog->addButton("Apply", 1, juce::KeyPress(juce::KeyPress::returnKey));
        dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        juce::Component::SafePointer<SoundBrowserSurfacePanelImpl> panel(this);
        dialog->enterModalState(true,
            juce::ModalCallbackFunction::create([panel, dialog, name = control.eventName](int result) {
                if (panel == nullptr || result != 1)
                    return;
                const auto chosen = dialog->getComboBoxComponent("kind")->getSelectedItemIndex();
                const auto kind = chosen == 1 ? talas::project::VirtualControlKind::Gate
                    : chosen == 2             ? talas::project::VirtualControlKind::Toggle
                                              : talas::project::VirtualControlKind::Fader;
                panel->setWidget(name, kind, dialog->getTextEditorContents("minimum").getDoubleValue(),
                    dialog->getTextEditorContents("maximum").getDoubleValue());
            }),
            true);
    }

    void setWidget(const std::string& name, talas::project::VirtualControlKind kind, double minimum, double maximum)
    {
        auto changed = surface_.has_value() ? talas::app::soundBrowserSurfaceWidgetSet(*surface_, name, kind, minimum, maximum)
                                            : std::nullopt;
        if (!changed.has_value())
        {
            status("A widget's range runs from a minimum below its maximum");
            return;
        }
        surface_ = std::move(*changed);
        refreshWidgets();
        // Its value, kept within the new range, reaches the Sound.
        if (const auto* control = widgetNamed(name))
            send(*control);
    }

    void save()
    {
        if (surface_.has_value() && host_.save && host_.save(*surface_))
            surface_->rearranged = false;
    }

    SoundBrowserSurfaceHost host_;
    std::optional<talas::app::SoundBrowserSurface> surface_;
    juce::Label title_;
    juce::Label note_;
    juce::TextButton dice_{"Dice"};
    juce::TextButton padlock_{"Unlock"};
    juce::TextButton save_{"Save"};
    juce::Viewport viewport_;
    juce::Component content_;
    EditOverlay overlay_;
    std::vector<std::unique_ptr<SurfaceWidget>> widgets_;
    std::optional<juce::Point<int>> dragStart_;
    bool editing_ = false;
};
}

std::unique_ptr<SoundBrowserSurfacePanel> makeSoundBrowserSurfacePanel(SoundBrowserSurfaceHost host)
{
    return std::make_unique<SoundBrowserSurfacePanelImpl>(std::move(host));
}
}
