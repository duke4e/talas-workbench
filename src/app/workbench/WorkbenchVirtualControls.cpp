#include "app/workbench/WorkbenchVirtualControls.h"
#include "app/workbench/WorkbenchFocusCoordinator.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "app/workbench/WorkbenchVcsDisplays.h"
#include "app/workbench/WorkbenchVcsEditorWindow.h"
#include "talas/app/AutomationLaneEditing.h"
#include "talas/app/TalasFiles.h"
#include "talas/app/VcsLayout.h"
#include "talas/app/VcsPresets.h"
#include "talas/app/VirtualControlWidgetModel.h"
#include "talas/expr/Expression.h"
#include "talas/project/MidiDefaultMap.h"
#include "talas/project/ProjectGlobalMap.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectLocalMapControlDiscovery.h"
#include "talas/project/VirtualControlMidi.h"
#include "talas/runtime/PreparedMidiMapper.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace talas::app::workbench
{
namespace
{
struct CopiedWidgets
{
    std::vector<talas::project::VirtualControl> widgets;
    juce::String names;
};

CopiedWidgets& copiedWidgets()
{
    static CopiedWidgets copied;
    return copied;
}
}

void copyVirtualControlWidgets(std::vector<talas::project::VirtualControl> widgets, bool toSystemClipboard)
{
    auto& copied = copiedWidgets();
    juce::StringArray names;
    for (const auto& widget : widgets)
        names.add(juceString(widget.eventName));
    copied.widgets = std::move(widgets);
    copied.names = "Widgets: " + names.joinIntoString(", ");
    if (toSystemClipboard)
        juce::SystemClipboard::copyTextToClipboard(copied.names);
}

std::vector<talas::project::VirtualControl> copiedVirtualControlWidgets(bool fromSystemClipboard)
{
    const auto& copied = copiedWidgets();
    if (fromSystemClipboard && juce::SystemClipboard::getTextFromClipboard() != copied.names)
        return {};
    return copied.widgets;
}

namespace
{
class MomentaryGateButton final : public juce::TextButton
{
public:
    explicit MomentaryGateButton(const juce::String& text)
        : juce::TextButton(text)
    {
    }

    std::function<void()> onPressed;
    std::function<void()> onReleased;

    void mouseDown(const juce::MouseEvent& event) override
    {
        juce::TextButton::mouseDown(event);
        if (event.mods.isLeftButtonDown() && onPressed)
            onPressed();
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        juce::TextButton::mouseUp(event);
        if (onReleased)
            onReleased();
    }
};

// Shift held while dragging a fader:
// fine-tuning, the value moving a tenth as far as the pointer from where
// the drag began, on the fader's grid.
// Command or Control held over a numeric entry box, a small fader:
// vertical movements as if of an invisible fader.
class FineTuningSlider final : public juce::Slider
{
public:
    std::function<double(double startValue, double dragFraction)> fineTuned;
    std::function<double(double startValue, double dragFraction)> invisibleFader;

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.mods.isShiftDown() && fineTuned && isEnabled())
        {
            beginFineTuning();
            fineStartPosition_ = event.position;
            return;
        }
        if ((event.mods.isCommandDown() || event.mods.isCtrlDown()) && getSliderStyle() == juce::Slider::LinearBar
            && invisibleFader && isEnabled())
        {
            beginFineTuning();
            invisible_ = true;
            fineStartPosition_ = event.position;
            return;
        }
        juce::Slider::mouseDown(event);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (!fineStartValue_)
        {
            juce::Slider::mouseDrag(event);
            return;
        }
        if (invisible_)
        {
            invisibleFaderTo(static_cast<double>(fineStartPosition_.y - event.position.y) / talas::app::vcsInvisibleFaderPixels);
            return;
        }
        const auto horizontal = getSliderStyle() == juce::Slider::LinearHorizontal || getSliderStyle() == juce::Slider::LinearBar;
        const auto length = static_cast<float>(std::max(1, horizontal ? getWidth() : getHeight()));
        const auto travel = horizontal ? event.position.x - fineStartPosition_.x : fineStartPosition_.y - event.position.y;
        fineTuneTo(static_cast<double>(travel / length));
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (!fineStartValue_)
        {
            juce::Slider::mouseUp(event);
            return;
        }
        endFineTuning();
    }

    void beginFineTuning()
    {
        fineStartValue_ = getValue();
        if (onDragStart)
            onDragStart();
    }

    void fineTuneTo(double dragFraction)
    {
        if (fineStartValue_ && fineTuned)
            setValue(fineTuned(*fineStartValue_, dragFraction), juce::sendNotificationSync);
    }

    void invisibleFaderTo(double dragFraction)
    {
        if (fineStartValue_ && invisibleFader)
            setValue(invisibleFader(*fineStartValue_, dragFraction), juce::sendNotificationSync);
    }

    void endFineTuning()
    {
        fineStartValue_.reset();
        invisible_ = false;
        if (onDragEnd)
            onDragEnd();
    }

private:
    bool invisible_ = false;
    std::optional<double> fineStartValue_;
    juce::Point<float> fineStartPosition_;
};

// Show All Digits in Double Floating Point Numbers: the surface's numbers
// with twelve significant digits, or six.
std::atomic<bool> virtualControlNumbersShowAllDigits{false};

// A long name abbreviated to fit above its fader;
// the label's text, its name and mappings, stays whole.
class VcsNameLabel final : public juce::Label
{
public:
    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(findColour(backgroundColourId));
        const auto text = getText();
        const auto split = text.indexOf(juceString(" · "));
        const auto name = split < 0 ? text : text.substring(0, split);
        const auto area = getBorderSize().subtractedFrom(getLocalBounds());
        auto shown = name;
        for (auto characters = static_cast<std::size_t>(name.length());
             characters > 1 && juce::GlyphArrangement::getStringWidthInt(getFont(), shown) > area.getWidth(); --characters)
            shown = juceString(talas::app::vcsAbbreviatedName(name.toStdString(), characters - 1));
        graphics.setColour(findColour(textColourId));
        graphics.setFont(getFont());
        graphics.drawFittedText(shown + (split < 0 ? juce::String{} : text.substring(split)), area, getJustificationType(), 1, 1.0F);
    }
};

class VirtualControlRow final : public juce::Component
{
public:
    using BeginCallback = std::function<void()>;
    using ValueCallback = std::function<void(std::string_view, double, talas::expr::ValueDomain)>;
    using EndCallback = std::function<void()>;
    using RemoveCallback = std::function<void(std::string_view)>;
    using MidiLearnCallback = std::function<void(std::string_view)>;
    using MidiUnmapCallback = std::function<void(std::string_view)>;
    using OscMappingCallback = std::function<void(
        std::string_view,
        std::optional<talas::project::OscControlMapping>)>;
    using OscEgressMappingCallback = std::function<void(
        std::string_view,
        std::optional<talas::project::OscEgressMapping>)>;
    using FocusCallback = std::function<void(std::string_view)>;
    using WidgetCallback = std::function<void(std::string_view)>;

    VirtualControlRow(talas::project::VirtualControl control,
                      std::optional<std::string> connectedMidiSourceName,
                      bool oscRouteAvailable,
                      bool oscEgressRouteAvailable,
                      juce::String oscProfileDetail,
                      BeginCallback onBegin,
                      ValueCallback onValue,
                      EndCallback onEnd,
                      RemoveCallback onRemove,
                      MidiLearnCallback onMidiLearn,
                      MidiUnmapCallback onMidiUnmap,
                      OscMappingCallback onOscMapping,
                      OscEgressMappingCallback onOscEgressMapping,
                      FocusCallback onFocus,
                      WidgetCallback onEditWidget)
        : control_(std::move(control))
        , onBegin_(std::move(onBegin))
        , onValue_(std::move(onValue))
        , onEnd_(std::move(onEnd))
        , onRemove_(std::move(onRemove))
        , onMidiLearn_(std::move(onMidiLearn))
        , onMidiUnmap_(std::move(onMidiUnmap))
        , onOscMapping_(std::move(onOscMapping))
        , onOscEgressMapping_(std::move(onOscEgressMapping))
        , onFocus_(std::move(onFocus))
        , onEditWidget_(std::move(onEditWidget))
        , gateButton_("Gate")
        , removeButton_("Remove")
        , midiLearnButton_("Learn")
    {
        const auto focusIdentity = [&](std::string_view part) {
            const std::array fields{
                std::string_view{control_.eventName}, part};
            return juceString(workbenchStableFocusItemId(
                "virtual-control", fields));
        };
        setComponentID(focusIdentity("row"));
        valueSlider_.setComponentID(focusIdentity("value"));
        toggleButton_.setComponentID(focusIdentity("toggle"));
        gateButton_.setComponentID(focusIdentity("gate"));
        removeButton_.setComponentID(focusIdentity("remove"));
        midiLearnButton_.setComponentID(focusIdentity("midi-learn"));
        oscAddress_.setComponentID(focusIdentity("osc-address"));
        oscMappingButton_.setComponentID(focusIdentity("osc-map"));
        oscEgressAddress_.setComponentID(
            focusIdentity("osc-egress-address"));
        oscEgressMappingButton_.setComponentID(
            focusIdentity("osc-egress-map"));
        addAndMakeVisible(nameLabel_);
        addAndMakeVisible(valueSlider_);
        addAndMakeVisible(toggleButton_);
        addAndMakeVisible(gateButton_);
        addAndMakeVisible(removeButton_);
        addAndMakeVisible(midiLearnButton_);
        addAndMakeVisible(oscAddress_);
        addAndMakeVisible(oscMappingButton_);
        addAndMakeVisible(oscEgressAddress_);
        addAndMakeVisible(oscEgressMappingButton_);
        addAndMakeVisible(widgetButton_);
        widgetButton_.setButtonText(juce::String::fromUTF8("…"));
        widgetButton_.setComponentID(focusIdentity("widget"));
        describeControl(
            widgetButton_,
            "Widget options for " + juceString(control_.eventName),
            "Widget type, numeric field, label, colours, Min / Max / Grid, taper and tab.");
        widgetButton_.onClick = [this] {
            if (onEditWidget_)
                onEditWidget_(control_.eventName);
        };

        const auto labelText = juceString(talas::app::virtualControlLabel(control_))
            + juceString(" · ") + domainName();
        juce::String mappingText;
        if (control_.midiMapping.has_value())
        {
            if (control_.midiMapping->sourceMode
                == talas::project::MidiControlSourceMode::LegacyAnyInput)
            {
                mappingText = juceString(
                    "Legacy any input · Re-learn recommended · Ch ");
            }
            else if (connectedMidiSourceName.has_value())
            {
                mappingText = juceString(*connectedMidiSourceName)
                    + juceString(" (")
                    + juceString(control_.midiMapping->sourceIdentifier)
                    + juceString(") · Ch ");
            }
            else
            {
                mappingText = juceString("Input unavailable · ")
                    + juceString(control_.midiMapping->sourceIdentifier)
                    + juceString(" · Ch ");
            }
            mappingText +=
                juce::String(control_.midiMapping->channel)
                + " CC "
                + juce::String(control_.midiMapping->controller);
        }
        const auto oscText = control_.oscMapping.has_value()
            ? (oscRouteAvailable
                   ? juce::String{"OSC in "}
                   : juce::String{"OSC in unavailable: "})
                + juceString(control_.oscMapping->address)
            : juce::String{"OSC in unmapped"};
        const auto oscEgressText = control_.oscEgressMapping.has_value()
            ? (oscEgressRouteAvailable
                   ? juce::String{"OSC out "}
                   : juce::String{"OSC out unavailable: "})
                + juceString(control_.oscEgressMapping->address)
            : juce::String{"OSC out unmapped"};
        const auto visibleLabel = labelText
            + (mappingText.isEmpty()
                   ? juce::String{}
                   : juceString(" · ") + mappingText)
            + juceString(" · ") + oscText
            + juceString(" · ") + oscEgressText;
        nameLabel_.setText(visibleLabel, juce::dontSendNotification);
        nameLabel_.setTooltip(
            visibleLabel + juceString(" · ") + oscProfileDetail);
        nameLabel_.setColour(juce::Label::textColourId, textColour);
        nameLabel_.setFont(WorkbenchType::bodyStrong());
        nameLabel_.setJustificationType(juce::Justification::centredLeft);
        describeControl(
            nameLabel_,
            juceString(control_.eventName)
                + ", "
                + domainName()
                + " Event Value"
                + (mappingText.isEmpty()
                       ? juce::String{}
                       : ", mapped to " + mappingText)
                + ", " + oscText
                + ", " + oscEgressText
                + ", " + oscProfileDetail);
        describeControl(
            valueSlider_,
            juceString(control_.eventName) + " value",
            "Current Event Value. Use arrow keys for precise changes.");
        describeControl(
            toggleButton_,
            juceString(control_.eventName) + " toggle");
        describeControl(
            gateButton_,
            juceString(control_.eventName) + " momentary gate");
        describeControl(
            removeButton_,
            "Delete " + juceString(control_.eventName),
            "Remove this Event Value control.");
        describeControl(
            midiLearnButton_,
            control_.midiMapping.has_value()
                ? "Unmap MIDI from " + juceString(control_.eventName)
                : "Learn MIDI for " + juceString(control_.eventName),
            control_.midiMapping.has_value()
                ? "Remove " + mappingText
                    + ". The Event Value remains unchanged."
                : "Map the next MIDI controller message to this Event Value.");
        valueSlider_.setRange(control_.minimum, control_.maximum, std::max(0.0, control_.widget.grid));
        // The widget's type and taper.
        {
            using Style = talas::project::VirtualControlWidgetStyle;
            const auto style = control_.widget.style;
            valueSlider_.setSliderStyle(style == Style::Fader ? juce::Slider::LinearVertical
                    : style == Style::SmallFader                ? juce::Slider::LinearBar
                    : style == Style::Potentiometer             ? juce::Slider::RotaryHorizontalVerticalDrag
                                                                : juce::Slider::LinearHorizontal);
            if (control_.widget.taper == talas::project::VirtualControlTaper::Log && control_.minimum > 0.0)
                valueSlider_.setSkewFactorFromMidPoint(std::sqrt(control_.minimum * control_.maximum));
            if (control_.widget.onColour.has_value())
            {
                const auto on = juce::Colour{0xff000000u | *control_.widget.onColour};
                gateButton_.setColour(juce::TextButton::buttonOnColourId, on);
                toggleButton_.setColour(juce::ToggleButton::tickColourId, on);
            }
            if (control_.widget.offColour.has_value())
            {
                const auto off = juce::Colour{0xff000000u | *control_.widget.offColour};
                gateButton_.setColour(juce::TextButton::buttonColourId, off);
                toggleButton_.setColour(juce::ToggleButton::tickDisabledColourId, off);
            }
            // Button (fill): the button itself is the colour, its
            // label none, the On colour while it is held.
            gateButton_.setButtonText(style == Style::ButtonFill ? juce::String{} : juce::String{"Gate"});
        }
        valueSlider_.textFromValueFunction = [](double value) {
            talas::app::WorkbenchPreferences preferences;
            preferences.showAllDigits = virtualControlNumbersShowAllDigits.load(std::memory_order_relaxed);
            return juce::String{talas::app::displayedNumber(value, preferences)};
        };
        valueSlider_.setTextBoxStyle(
            juce::Slider::TextBoxRight,
            false,
            64,
            20);
        valueSlider_.setValue(control_.value, juce::dontSendNotification);
        // A number typed into the fader's field moves it there, on its grid
        // within its range.
        valueSlider_.valueFromTextFunction = [this](const juce::String& text) {
            return talas::app::quantizedVirtualControlValue(control_, text.getDoubleValue());
        };
        valueSlider_.fineTuned = [this](double start, double fraction) {
            return talas::app::vcsFineTunedValue(control_, start, fraction);
        };
        valueSlider_.invisibleFader = [this](double start, double fraction) {
            return talas::app::vcsInvisibleFaderValue(control_, start, fraction);
        };
        valueSlider_.onDragStart = [this] { beginValueChange(); };
        valueSlider_.onValueChange = [this] {
            const auto standaloneEdit = !changing_;
            if (standaloneEdit)
                beginValueChange();
            publishValue(valueSlider_.getValue());
            if (standaloneEdit)
                endValueChange();
        };
        valueSlider_.onDragEnd = [this] { endValueChange(); };
        toggleButton_.setButtonText("Enabled");
        toggleButton_.setToggleState(control_.value >= (control_.minimum + control_.maximum) * 0.5, juce::dontSendNotification);
        toggleButton_.onClick = [this] {
            beginValueChange();
            publishValue(talas::app::virtualControlButtonValue(control_, true).value_or(control_.value));
            toggleButton_.setToggleState(control_.value >= (control_.minimum + control_.maximum) * 0.5,
                juce::dontSendNotification);
            endValueChange();
        };
        gateButton_.onPressed = [this] {
            beginValueChange();
            publishValue(talas::app::virtualControlButtonValue(control_, true).value_or(control_.value));
            // Button (fill) lights with its On colour while held.
            gateButton_.setToggleState(true, juce::dontSendNotification);
        };
        gateButton_.onReleased = [this] {
            publishValue(talas::app::virtualControlButtonValue(control_, false).value_or(control_.value));
            gateButton_.setToggleState(false, juce::dontSendNotification);
            endValueChange();
        };
        removeButton_.setTooltip("Remove Event Value");
        removeButton_.onClick = [this] { onRemove_(control_.eventName); };
        midiLearnButton_.setButtonText(
            control_.midiMapping.has_value()
                ? "Unmap Ch"
                    + juce::String(control_.midiMapping->channel)
                    + " CC"
                    + juce::String(control_.midiMapping->controller)
                : "Learn");
        midiTooltip_ =
            control_.midiMapping.has_value()
                ? "Unmap " + mappingText + " from "
                    + juceString(control_.eventName)
                : "Learn a MIDI controller";
        midiLearnButton_.setTooltip(midiTooltip_);
        midiLearnButton_.onClick = [this] {
            if (control_.midiMapping.has_value())
                onMidiUnmap_(control_.eventName);
            else
                onMidiLearn_(control_.eventName);
        };

        oscAddress_.setTextToShowWhenEmpty(
            "/synth/gain",
            secondaryTextColour);
        if (control_.oscMapping.has_value())
        {
            oscAddress_.setText(
                juceString(control_.oscMapping->address),
                false);
        }
        oscAddress_.setInputRestrictions(
            static_cast<int>(talas::project::maximumOscAddressBytes));
        oscAddress_.setTooltip(
            "Exact OSC address. Press Return to apply.");
        describeControl(
            oscAddress_,
            juceString(control_.eventName) + " OSC address",
            control_.oscMapping.has_value()
                ? "Mapped to "
                    + juceString(control_.oscMapping->address)
                : "OSC unmapped. Enter an exact address such as /synth/gain.");
        oscAddress_.onReturnKey = [this] { submitOscAddress(); };
        oscMappingButton_.setButtonText(
            control_.oscMapping.has_value() ? "Clear" : "Set");
        oscMappingButton_.setTooltip(
            control_.oscMapping.has_value()
                ? "Remove the exact OSC mapping"
                : "Set the exact OSC mapping");
        oscMappingButton_.onClick = [this] {
            if (control_.oscMapping.has_value())
                onOscMapping_(control_.eventName, std::nullopt);
            else
                submitOscAddress();
        };
        oscEgressAddress_.setTextToShowWhenEmpty(
            "/feedback/gain",
            secondaryTextColour);
        if (control_.oscEgressMapping.has_value())
        {
            oscEgressAddress_.setText(
                juceString(control_.oscEgressMapping->address),
                false);
        }
        oscEgressAddress_.setInputRestrictions(
            static_cast<int>(talas::project::maximumOscAddressBytes));
        oscEgressAddress_.setTooltip(
            "Exact OSC feedback address. Press Return to apply.");
        describeControl(
            oscEgressAddress_,
            juceString(control_.eventName) + " OSC feedback address",
            control_.oscEgressMapping.has_value()
                ? "Persisted control-state feedback uses "
                    + juceString(control_.oscEgressMapping->address)
                : "OSC feedback unmapped. Enter an exact address such as /feedback/gain.");
        oscEgressAddress_.onReturnKey = [this] {
            submitOscEgressAddress();
        };
        oscEgressMappingButton_.setButtonText(
            control_.oscEgressMapping.has_value() ? "Clear" : "Set");
        oscEgressMappingButton_.setTooltip(
            control_.oscEgressMapping.has_value()
                ? "Remove the exact OSC feedback mapping"
                : "Set the exact OSC feedback mapping");
        oscEgressMappingButton_.onClick = [this] {
            if (control_.oscEgressMapping.has_value())
                onOscEgressMapping_(control_.eventName, std::nullopt);
            else
                submitOscEgressAddress();
        };
        updateControlVisibility();
    }

    [[nodiscard]] const std::string& eventName() const
    {
        return control_.eventName;
    }

    [[nodiscard]] std::optional<juce::String>
    focusedComponentIdentity() const
    {
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        if (focused == nullptr
            || (focused != this && !isParentOf(focused)))
        {
            return std::nullopt;
        }
        for (auto* current = focused; current != nullptr;
             current = current->getParentComponent())
        {
            if (current->getComponentID().isNotEmpty())
                return current->getComponentID();
            if (current == this)
                break;
        }
        return std::nullopt;
    }

    bool restoreFocusedComponent(const juce::String& identity)
    {
        for (auto* component : std::array<juce::Component*, 10>{
                 &valueSlider_, &toggleButton_, &gateButton_,
                 &removeButton_, &midiLearnButton_, &oscAddress_,
                 &oscMappingButton_, &oscEgressAddress_,
                 &oscEgressMappingButton_, this})
        {
            if (component->getComponentID() == identity
                && component->isVisible() && component->isEnabled())
            {
                component->grabKeyboardFocus();
                return component->hasKeyboardFocus(true);
            }
        }
        return false;
    }

    [[nodiscard]] std::string midiMappingPresentationForSmoke() const
    {
        return nameLabel_.getText().toStdString()
            + "|" + midiLearnButton_.getButtonText().toStdString()
            + "|" + midiTooltip_.toStdString();
    }

    [[nodiscard]] std::string oscMappingPresentationForSmoke() const
    {
        return nameLabel_.getText().toStdString()
            + "|" + oscAddress_.getText().toStdString()
            + "|" + oscMappingButton_.getButtonText().toStdString();
    }

    [[nodiscard]] std::string
    oscEgressMappingPresentationForSmoke() const
    {
        return nameLabel_.getText().toStdString()
            + "|" + oscEgressAddress_.getText().toStdString()
            + "|" + oscEgressMappingButton_.getButtonText().toStdString();
    }

    [[nodiscard]] bool setValueForSmoke(double value)
    {
        if (!std::isfinite(value)
            || value < control_.minimum || value > control_.maximum)
        {
            return false;
        }
        beginValueChange();
        publishValue(value);
        endValueChange();
        setExternalValue(value);
        return true;
    }

    [[nodiscard]] bool startValueGestureForSmoke(double value)
    {
        if (!std::isfinite(value)
            || value < control_.minimum || value > control_.maximum)
        {
            return false;
        }
        beginValueChange();
        publishValue(value);
        return true;
    }

    [[nodiscard]] VirtualControlSurface::ControlPresentation
    controlPresentationForSmoke() const noexcept
    {
        const auto visible = control_.kind
                == talas::project::VirtualControlKind::Gate
            ? gateButton_.isVisible()
            : control_.kind == talas::project::VirtualControlKind::Toggle
                ? toggleButton_.isVisible()
                : valueSlider_.isVisible();
        return {control_.kind, control_.value, visible};
    }

    [[nodiscard]] bool operateButtonForSmoke(bool pressed)
    {
        if (control_.kind == talas::project::VirtualControlKind::Gate)
        {
            if (pressed) gateButton_.onPressed();
            else gateButton_.onReleased();
            return control_.value == (pressed ? control_.maximum
                                               : control_.minimum);
        }
        if (control_.kind == talas::project::VirtualControlKind::Toggle
            && pressed)
        {
            const auto next = !toggleButton_.getToggleState();
            toggleButton_.setToggleState(next, juce::dontSendNotification);
            toggleButton_.onClick();
            return control_.value == (next ? control_.maximum
                                           : control_.minimum);
        }
        return false;
    }

    // Clicking a parameter's name freezes it against
    // the dice, its name greyed; clicking again makes it active.
    void setFreezing(std::function<void(std::string_view)> onFreeze, bool frozen)
    {
        onFreeze_ = std::move(onFreeze);
        nameLabel_.setColour(juce::Label::textColourId, frozen ? secondaryTextColour.withAlpha(0.55F) : textColour);
        if (!freezeListening_)
        {
            nameLabel_.addMouseListener(this, false);
            freezeListening_ = true;
        }
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (event.eventComponent == &nameLabel_ && onFreeze_)
            onFreeze_(control_.eventName);
    }

    void setExternalValue(double value)
    {
        control_.value = value;
        valueSlider_.setValue(value, juce::dontSendNotification);
        toggleButton_.setToggleState(value >= (control_.minimum + control_.maximum) * 0.5, juce::dontSendNotification);
    }

    void focusPrimaryControl()
    {
        if (valueSlider_.isVisible())
            valueSlider_.grabKeyboardFocus();
        else if (toggleButton_.isVisible())
            toggleButton_.grabKeyboardFocus();
        else if (gateButton_.isVisible())
            gateButton_.grabKeyboardFocus();
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto bounds = getLocalBounds().toFloat();
        graphics.setColour(workbenchTheme.controlTile);
        graphics.fillRoundedRectangle(bounds, 6.0F);
        graphics.setColour(panelBorderColour);
        graphics.drawRoundedRectangle(bounds.reduced(0.5F), 6.0F, 1.0F);
    }

    void resized() override
    {
        const auto scaled = [](int pixels) {
            return workbenchScaledUiPixels(pixels);
        };
        auto bounds = getLocalBounds().reduced(scaled(6));
        using Style = talas::project::VirtualControlWidgetStyle;
        const auto upright = control_.kind == talas::project::VirtualControlKind::Fader
            && (control_.widget.style == Style::Fader || control_.widget.style == Style::Potentiometer);
        // A vertical fader or a pot stands in its own column.
        const auto column = upright ? bounds.removeFromLeft(scaled(48)) : juce::Rectangle<int>{};
        if (upright)
            bounds.removeFromLeft(scaled(4));
        auto topRow = bounds.removeFromTop(scaled(24));
        removeButton_.setBounds(topRow.removeFromRight(scaled(54)));
        topRow.removeFromRight(scaled(2));
        widgetButton_.setBounds(topRow.removeFromRight(scaled(22)));
        topRow.removeFromRight(scaled(2));
        nameLabel_.setBounds(topRow);
        bounds.removeFromTop(scaled(3));
        auto controlRow = bounds.removeFromTop(scaled(24));
        const auto midiButtonWidth =
            scaled(control_.midiMapping.has_value() ? 90 : 46);
        midiLearnButton_.setBounds(
            controlRow.removeFromRight(midiButtonWidth));
        controlRow.removeFromRight(scaled(4));
        // An upright fader's numeric field on top.
        const auto placement = talas::app::vcsFaderNumberPlacement(upright, control_.widget.showNumber);
        valueSlider_.setTextBoxStyle(
            placement == talas::app::VcsNumberPlacement::None    ? juce::Slider::NoTextBox
                : placement == talas::app::VcsNumberPlacement::Above ? juce::Slider::TextBoxAbove
                                                                  : juce::Slider::TextBoxRight,
            false,
            upright ? scaled(46) : scaled(64),
            scaled(20));
        valueSlider_.setBounds(upright ? column : controlRow);
        toggleButton_.setBounds(controlRow);
        gateButton_.setBounds(controlRow);
        bounds.removeFromTop(scaled(3));
        auto oscRow = bounds.removeFromTop(scaled(24));
        oscMappingButton_.setBounds(oscRow.removeFromRight(scaled(46)));
        oscRow.removeFromRight(scaled(4));
        oscAddress_.setBounds(oscRow);
        bounds.removeFromTop(scaled(3));
        auto oscEgressRow = bounds.removeFromTop(scaled(24));
        oscEgressMappingButton_.setBounds(
            oscEgressRow.removeFromRight(scaled(46)));
        oscEgressRow.removeFromRight(scaled(4));
        oscEgressAddress_.setBounds(oscEgressRow);
    }

    void focusOfChildComponentChanged(FocusChangeType) override
    {
        if (hasKeyboardFocus(true) && onFocus_)
            onFocus_(control_.eventName);
    }

    [[nodiscard]] const talas::project::VirtualControl& control() const noexcept
    {
        return control_;
    }

    [[nodiscard]] VirtualControlSurface::WidgetPresentation widgetPresentationForSmoke() const
    {
        VirtualControlSurface::WidgetPresentation presentation;
        presentation.sliderStyle = static_cast<int>(valueSlider_.getSliderStyle());
        presentation.numberShown = valueSlider_.getTextBoxPosition() != juce::Slider::NoTextBox;
        presentation.label = nameLabel_.getText().toStdString();
        presentation.interval = valueSlider_.getInterval();
        presentation.shown = isVisible();
        presentation.onColour = gateButton_.findColour(juce::TextButton::buttonOnColourId).getARGB() & 0xFFFFFFu;
        if (valueSlider_.textFromValueFunction)
            presentation.numberText = valueSlider_.textFromValueFunction(valueSlider_.getValue()).toStdString();
        return presentation;
    }

    void refreshNumberText()
    {
        valueSlider_.updateText();
    }

    // A fader's number shown on the surface, to type a new value into.
    [[nodiscard]] bool showsNumber() const
    {
        return isVisible() && valueSlider_.isVisible() && valueSlider_.getTextBoxPosition() != juce::Slider::NoTextBox;
    }

    [[nodiscard]] bool typingNumber() const
    {
        const auto* focused = juce::Component::getCurrentlyFocusedComponent();
        return focused != nullptr && valueSlider_.isParentOf(focused);
    }

    void typeNumber()
    {
        valueSlider_.showTextBox();
    }

    // A Shift+drag over a fraction of the fader's length, for the smoke.
    [[nodiscard]] double fineTuneForSmoke(double dragFraction)
    {
        valueSlider_.beginFineTuning();
        valueSlider_.fineTuneTo(dragFraction);
        valueSlider_.endFineTuning();
        return valueSlider_.getValue();
    }

    // The numeric entry box dragged as an invisible fader, or nothing for
    // a widget that is not one.
    [[nodiscard]] std::optional<double> invisibleFaderForSmoke(double dragFraction)
    {
        if (valueSlider_.getSliderStyle() != juce::Slider::LinearBar)
            return std::nullopt;
        valueSlider_.beginFineTuning();
        valueSlider_.invisibleFaderTo(dragFraction);
        valueSlider_.endFineTuning();
        return valueSlider_.getValue();
    }

    // A value the slider's position stands for, on the widget's grid.
    [[nodiscard]] double valueAtPositionForSmoke(double position) const
    {
        // The slider's own range: its skew and its interval.
        const auto range = valueSlider_.getNormalisableRange();
        return range.snapToLegalValue(range.convertFrom0to1(position));
    }

private:
    void submitOscAddress()
    {
        const auto address = oscAddress_.getText().toStdString();
        onOscMapping_(
            control_.eventName,
            talas::project::OscControlMapping{address});
    }

    void submitOscEgressAddress()
    {
        const auto address = oscEgressAddress_.getText().toStdString();
        onOscEgressMapping_(
            control_.eventName,
            talas::project::OscEgressMapping{address});
    }

    juce::String domainName() const
    {
        switch (control_.domain)
        {
        case talas::expr::ValueDomain::Scalar: return "scalar";
        case talas::expr::ValueDomain::Hertz: return "hertz";
        case talas::expr::ValueDomain::Seconds: return "seconds";
        case talas::expr::ValueDomain::NoteNumber: return "note number";
        case talas::expr::ValueDomain::Samples: return "samples";
        }
        return {};
    }

    void updateControlVisibility()
    {
        valueSlider_.setVisible(control_.kind == talas::project::VirtualControlKind::Fader);
        toggleButton_.setVisible(control_.kind == talas::project::VirtualControlKind::Toggle);
        gateButton_.setVisible(control_.kind == talas::project::VirtualControlKind::Gate);
    }

    void beginValueChange()
    {
        if (changing_)
            return;
        changing_ = true;
        onBegin_();
    }

    void publishValue(double value)
    {
        control_.value = value;
        onValue_(control_.eventName, value, control_.domain);
    }

    void endValueChange()
    {
        if (!changing_)
            return;
        changing_ = false;
        onEnd_();
    }

    talas::project::VirtualControl control_;
    BeginCallback onBegin_;
    ValueCallback onValue_;
    EndCallback onEnd_;
    RemoveCallback onRemove_;
    MidiLearnCallback onMidiLearn_;
    MidiUnmapCallback onMidiUnmap_;
    OscMappingCallback onOscMapping_;
    OscEgressMappingCallback onOscEgressMapping_;
    FocusCallback onFocus_;
    WidgetCallback onEditWidget_;
    VcsNameLabel nameLabel_;
    std::function<void(std::string_view)> onFreeze_;
    bool freezeListening_ = false;
    FineTuningSlider valueSlider_;
    juce::ToggleButton toggleButton_;
    MomentaryGateButton gateButton_;
    juce::TextButton removeButton_;
    juce::TextButton midiLearnButton_;
    juce::TextEditor oscAddress_;
    juce::TextButton oscMappingButton_;
    juce::TextEditor oscEgressAddress_;
    juce::TextButton oscEgressMappingButton_;
    juce::TextButton widgetButton_;
    juce::String midiTooltip_;
    bool changing_ = false;
};

class LocalMapControlRow final : public juce::Component
{
public:
    using DispatchCallback = VirtualControlSurface::LocalMapValueDispatchCallback;
    using StatusCallback = std::function<void(const juce::String&)>;

    LocalMapControlRow(talas::project::ProjectLocalMapControl control,
                       std::string sourceEventName,
                       double normalizedValue,
                       bool enabled,
                       DispatchCallback dispatch,
                       StatusCallback status)
        : control_(std::move(control))
        , sourceEventName_(std::move(sourceEventName))
        , dispatch_(std::move(dispatch))
        , status_(std::move(status))
        , gateButton_("Gate")
        , bounds_(control_.range.value_or(std::array<double, 2>{0.0, 1.0}))
    {
        const std::array fields{
            std::string_view{control_.flatMapperNodeId},
            std::string_view{control_.eventName}};
        const auto identity = juceString(workbenchStableFocusItemId(
            "local-map-control", fields));
        setComponentID(identity);
        slider_.setComponentID(identity + "-value");
        toggleButton_.setComponentID(identity + "-toggle");
        gateButton_.setComponentID(identity + "-gate");
        addAndMakeVisible(label_);
        addAndMakeVisible(rangeLabel_);
        addAndMakeVisible(slider_);
        addAndMakeVisible(toggleButton_);
        addAndMakeVisible(gateButton_);
        const auto path = control_.mapperInstancePath.empty()
            ? control_.mapperLocalNodeId
            : control_.mapperInstancePath.back() + "/"
                + control_.mapperLocalNodeId;
        const auto caption = "Local " + juceString(control_.eventName)
            + " / " + juceString(path);
        const auto sourceLabel = control_.channel
            ? juceString(control_.sourceName) + juceString(" · Ch ")
                + juce::String(*control_.channel)
            : juceString(sourceEventName_);
        const auto rangeText = sourceLabel
            + ": " + juce::String(bounds_[0], 3)
            + " to " + juce::String(bounds_[1], 3)
            + (control_.grid
                ? juceString(" · grid ") + juce::String(*control_.grid, 3)
                : juce::String{})
            + (control_.taper == talas::project::MidiMapperMapTaper::Log
                ? juceString(" · log") : juce::String{});
        label_.setText(caption, juce::dontSendNotification);
        label_.setColour(juce::Label::textColourId, textColour);
        label_.setFont(WorkbenchType::bodyStrong());
        label_.setJustificationType(juce::Justification::centredLeft);
        label_.setTooltip(caption + " / " + juceString(control_.sourceName)
            + " / " + rangeText);
        describeControl(label_, caption);
        rangeLabel_.setText(rangeText, juce::dontSendNotification);
        rangeLabel_.setColour(juce::Label::textColourId, secondaryTextColour);
        rangeLabel_.setFont(WorkbenchType::body());
        rangeLabel_.setJustificationType(juce::Justification::centredLeft);
        describeControl(rangeLabel_, rangeText);
        describeControl(slider_,
            juceString(control_.eventName) + " local Map value",
            "Controls this MIDIMapper Input Sound through "
                + juceString(control_.sourceName) + " over " + rangeText);
        slider_.setRange(bounds_[0], bounds_[1],
            control_.grid.value_or(0.0));
        if (control_.displayAs
            == talas::project::MidiMapperMapDisplayAs::SmallFader)
            slider_.setSliderStyle(juce::Slider::LinearBar);
        if (control_.displayAs == talas::project::MidiMapperMapDisplayAs::Rotary
            || control_.displayAs == talas::project::MidiMapperMapDisplayAs::Pot)
            slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider_.setNumDecimalPlacesToDisplay(3);
        slider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
        slider_.setEnabled(enabled);
        toggleButton_.setButtonText("Enabled");
        toggleButton_.setEnabled(enabled);
        gateButton_.setEnabled(enabled);
        describeControl(toggleButton_,
            juceString(control_.eventName) + " local Map toggle");
        describeControl(gateButton_,
            juceString(control_.eventName) + " local Map momentary gate");
        const auto display = control_.displayAs;
        slider_.setVisible(display == talas::project::MidiMapperMapDisplayAs::Default
            || display == talas::project::MidiMapperMapDisplayAs::Fader
            || display == talas::project::MidiMapperMapDisplayAs::SmallFader
            || display == talas::project::MidiMapperMapDisplayAs::Rotary
            || display == talas::project::MidiMapperMapDisplayAs::Pot);
        toggleButton_.setVisible(
            display == talas::project::MidiMapperMapDisplayAs::Toggle);
        gateButton_.setVisible(
            display == talas::project::MidiMapperMapDisplayAs::Gate);
        setExternalNormalizedValue(normalizedValue);
        slider_.onValueChange = [this] {
            const auto value = slider_.getValue();
            if (!setValue(value))
                slider_.setValue(currentValue(), juce::dontSendNotification);
        };
        toggleButton_.onClick = [this] {
            if (!setValue(toggleButton_.getToggleState()
                    ? bounds_[1] : bounds_[0]))
                toggleButton_.setToggleState(normalizedValue_ >= 0.5,
                    juce::dontSendNotification);
        };
        gateButton_.onPressed = [this] { (void)setValue(bounds_[1]); };
        gateButton_.onReleased = [this] { (void)setValue(bounds_[0]); };
    }

    [[nodiscard]] const talas::project::ProjectLocalMapControl& control() const noexcept
    {
        return control_;
    }

    [[nodiscard]] const std::string& sourceEventName() const noexcept
    {
        return sourceEventName_;
    }

    [[nodiscard]] double normalizedValue() const noexcept
    {
        return normalizedValue_;
    }

    [[nodiscard]] double currentValue() const noexcept
    {
        const auto mapped = talas::project::projectMidiMapperTaperValue(
            normalizedValue_, bounds_[0], bounds_[1], control_.taper);
        return quantizeValue(mapped.value_or(bounds_[0]));
    }

    [[nodiscard]] bool enabled() const noexcept
    {
        return slider_.isEnabled();
    }

    [[nodiscard]] std::string displayType() const
    {
        switch (control_.displayAs)
        {
        case talas::project::MidiMapperMapDisplayAs::Default:
        case talas::project::MidiMapperMapDisplayAs::Fader: return "fader";
        case talas::project::MidiMapperMapDisplayAs::SmallFader:
            return "smallFader";
        case talas::project::MidiMapperMapDisplayAs::Toggle: return "toggle";
        case talas::project::MidiMapperMapDisplayAs::Gate: return "gate";
        case talas::project::MidiMapperMapDisplayAs::Nothing: return "nothing";
        case talas::project::MidiMapperMapDisplayAs::Rotary: return "rotary";
        case talas::project::MidiMapperMapDisplayAs::Pot: return "pot";
        }
        return {};
    }

    [[nodiscard]] std::string rangeText() const
    {
        return rangeLabel_.getText().toStdString();
    }

    [[nodiscard]] bool setValue(double value)
    {
        if (!slider_.isEnabled() || !std::isfinite(value)
            || value < bounds_[0] || value > bounds_[1])
        {
            return false;
        }
        if ((control_.displayAs == talas::project::MidiMapperMapDisplayAs::Toggle
                || control_.displayAs == talas::project::MidiMapperMapDisplayAs::Gate)
            && value != bounds_[0] && value != bounds_[1])
            return false;
        value = quantizeValue(value);
        const auto normalized = talas::project::projectMidiMapperTaperInverse(
            value, bounds_[0], bounds_[1], control_.taper);
        if (!normalized) return false;
        if (const auto rejected = dispatch_(sourceEventName_, *normalized))
        {
            status_(*rejected);
            return false;
        }
        normalizedValue_ = *normalized;
        slider_.setValue(value, juce::dontSendNotification);
        toggleButton_.setToggleState(*normalized >= 0.5,
            juce::dontSendNotification);
        gateButton_.setToggleState(*normalized >= 0.5,
            juce::dontSendNotification);
        return true;
    }

    [[nodiscard]] bool operateButtonForSmoke(bool pressed)
    {
        if (!enabled()) return false;
        if (control_.displayAs
            == talas::project::MidiMapperMapDisplayAs::Toggle)
        {
            if (!pressed) return false;
            const auto next = !toggleButton_.getToggleState();
            toggleButton_.setToggleState(next, juce::dontSendNotification);
            toggleButton_.onClick();
            return normalizedValue_ == (next ? 1.0 : 0.0);
        }
        if (control_.displayAs == talas::project::MidiMapperMapDisplayAs::Gate)
        {
            if (pressed) gateButton_.onPressed();
            else gateButton_.onReleased();
            return normalizedValue_ == (pressed ? 1.0 : 0.0);
        }
        return false;
    }

    void setExternalNormalizedValue(double normalized)
    {
        normalizedValue_ = std::clamp(normalized, 0.0, 1.0);
        slider_.setValue(currentValue(), juce::dontSendNotification);
        toggleButton_.setToggleState(normalizedValue_ >= 0.5,
            juce::dontSendNotification);
        gateButton_.setToggleState(normalizedValue_ >= 0.5,
            juce::dontSendNotification);
    }

    void focusPrimaryControl()
    {
        if (slider_.isVisible()) slider_.grabKeyboardFocus();
        else if (toggleButton_.isVisible()) toggleButton_.grabKeyboardFocus();
        else if (gateButton_.isVisible()) gateButton_.grabKeyboardFocus();
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto bounds = getLocalBounds().toFloat();
        graphics.setColour(workbenchTheme.controlTile);
        graphics.fillRoundedRectangle(bounds, 6.0F);
        graphics.setColour(panelBorderColour);
        graphics.drawRoundedRectangle(bounds.reduced(0.5F), 6.0F, 1.0F);
    }

    void resized() override
    {
        const auto scaled = [](int pixels) {
            return workbenchScaledUiPixels(pixels);
        };
        auto bounds = getLocalBounds().reduced(scaled(6));
        label_.setBounds(bounds.removeFromTop(scaled(24)));
        bounds.removeFromTop(scaled(3));
        rangeLabel_.setBounds(bounds.removeFromTop(scaled(20)));
        bounds.removeFromTop(scaled(3));
        slider_.setTextBoxStyle(
            juce::Slider::TextBoxRight, false, scaled(64), scaled(20));
        slider_.setBounds(bounds.removeFromTop(scaled(24)));
        toggleButton_.setBounds(slider_.getBounds());
        gateButton_.setBounds(slider_.getBounds());
    }

private:
    [[nodiscard]] double quantizeValue(double value) const noexcept
    {
        if (!control_.grid) return value;
        const auto steps = std::round((value - bounds_[0]) / *control_.grid);
        return std::clamp(bounds_[0] + steps * *control_.grid,
            bounds_[0], bounds_[1]);
    }

    talas::project::ProjectLocalMapControl control_;
    std::string sourceEventName_;
    DispatchCallback dispatch_;
    StatusCallback status_;
    juce::Label label_;
    juce::Label rangeLabel_;
    juce::Slider slider_;
    juce::ToggleButton toggleButton_;
    MomentaryGateButton gateButton_;
    std::array<double, 2> bounds_;
    double normalizedValue_ = 0.0;
};

// Edit mode's overlay on the surface: faint
// squares round every widget, black squares at the selected widget's
// corners; a click selects a widget (Shift adds or removes one), a drag
// moves the selection or resizes from a corner, and a drag from an empty
// area draws a box that selects what it touches.
// The surface's texts and boxes, beneath
// its widgets.
class VcsDecorationLayer final : public juce::Component
{
public:
    VcsDecorationLayer()
    {
        setInterceptsMouseClicks(false, false);
    }

    std::vector<std::pair<talas::project::VirtualControlDecoration, juce::Rectangle<int>>> shown;

    void paint(juce::Graphics& graphics) override
    {
        for (const auto& [decoration, bounds] : shown)
        {
            if (decoration.kind == talas::project::VirtualControlDecorationKind::Box)
            {
                graphics.setColour(secondaryTextColour.withAlpha(0.7F));
                graphics.drawRoundedRectangle(bounds.toFloat().reduced(1.0F), 6.0F, 1.5F);
                continue;
            }
            graphics.setColour(textColour);
            graphics.setFont(WorkbenchType::panelTitle());
            graphics.drawFittedText(juceString(decoration.text), bounds.reduced(4), juce::Justification::topLeft,
                std::max(1, bounds.getHeight() / 16));
        }
    }
};

class VcsEditOverlay final : public juce::Component
{
public:
    std::function<std::vector<std::pair<std::string, juce::Rectangle<int>>>()> widgets;
    std::function<const std::set<std::string>&()> selection;
    std::function<void(std::set<std::string>)> select;
    // A drag that moved (dx, dy) or resized the one widget from a corner.
    std::function<void(int, int)> moved;
    std::function<void(const std::string&, talas::app::VcsCorner, int, int)> resized;

    void paint(juce::Graphics& graphics) override
    {
        if (!widgets)
            return;
        for (const auto& [name, bounds] : widgets())
        {
            graphics.setColour(juce::Colours::white.withAlpha(0.35F));
            graphics.drawRect(bounds.expanded(2), 1);
            if (!selection || !selection().contains(name))
                continue;
            graphics.setColour(selectionColour);
            graphics.drawRect(bounds.expanded(2), 2);
            graphics.setColour(juce::Colours::black);
            for (const auto corner : corners(bounds))
                graphics.fillRect(corner);
        }
        if (box_.has_value())
        {
            graphics.setColour(selectionColour.withAlpha(0.6F));
            graphics.drawRect(*box_, 1);
        }
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        start_ = event.getPosition();
        corner_.reset();
        box_.reset();
        widget_.clear();
        if (!widgets || !selection || !select)
            return;
        const auto all = widgets();
        for (const auto& [name, bounds] : all)
        {
            if (!selection().contains(name))
                continue;
            const auto squares = corners(bounds);
            for (std::size_t index = 0; index < squares.size(); ++index)
            {
                if (squares[index].expanded(2).contains(start_))
                {
                    corner_ = static_cast<talas::app::VcsCorner>(index);
                    widget_ = name;
                    return;
                }
            }
        }
        const auto hit = std::ranges::find_if(all, [this](const auto& entry) { return entry.second.contains(start_); });
        if (hit == all.end())
        {
            box_ = juce::Rectangle<int>{start_, start_};
            if (!event.mods.isShiftDown())
                select({});
            return;
        }
        auto chosen = selection();
        if (event.mods.isShiftDown())
        {
            if (!chosen.erase(hit->first))
                chosen.insert(hit->first);
        }
        else if (!chosen.contains(hit->first))
            chosen = {hit->first};
        widget_ = hit->first;
        select(std::move(chosen));
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (box_.has_value())
        {
            box_ = juce::Rectangle<int>{start_, event.getPosition()};
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        const auto offset = event.getPosition() - start_;
        if (box_.has_value())
        {
            const auto box = juce::Rectangle<int>{start_, event.getPosition()};
            box_.reset();
            std::set<std::string> chosen = event.mods.isShiftDown() && selection ? selection() : std::set<std::string>{};
            for (const auto& [name, bounds] : widgets())
            {
                if (bounds.intersects(box))
                    chosen.insert(name);
            }
            select(std::move(chosen));
            repaint();
            return;
        }
        if (offset.isOrigin() || widget_.empty())
            return;
        if (corner_.has_value() && resized)
            resized(widget_, *corner_, offset.x, offset.y);
        else if (moved)
            moved(offset.x, offset.y);
    }

private:
    [[nodiscard]] static std::array<juce::Rectangle<int>, 4> corners(juce::Rectangle<int> bounds)
    {
        constexpr int size = 7;
        return {juce::Rectangle<int>{bounds.getX() - size / 2, bounds.getY() - size / 2, size, size},
            juce::Rectangle<int>{bounds.getRight() - size / 2, bounds.getY() - size / 2, size, size},
            juce::Rectangle<int>{bounds.getX() - size / 2, bounds.getBottom() - size / 2, size, size},
            juce::Rectangle<int>{bounds.getRight() - size / 2, bounds.getBottom() - size / 2, size, size}};
    }

    juce::Point<int> start_;
    std::optional<talas::app::VcsCorner> corner_;
    std::optional<juce::Rectangle<int>> box_;
    std::string widget_;
};

class CallbackTabBar final : public juce::TabbedButtonBar
{
public:
    CallbackTabBar()
        : juce::TabbedButtonBar(juce::TabbedButtonBar::TabsAtTop)
    {
    }

    std::function<void(int)> onTabChanged;

    void currentTabChanged(int index, const juce::String&) override
    {
        if (onTabChanged)
            onTabChanged(index);
    }
};

// A surface larger than its window shows a box at its top centre: gray
// for the whole surface, blue for the part the window
// shows; dragging the box moves the view over the surface.
class VcsNavigatorBox final : public juce::Component, private juce::Timer
{
public:
    static constexpr int boxSize = 72;

    explicit VcsNavigatorBox(juce::Viewport& viewport) : viewport_(viewport)
    {
        setTitle("Surface overview");
        setDescription("Drag to see another part of a surface larger than its window");
        startTimerHz(10);
    }

    [[nodiscard]] std::optional<talas::app::VcsNavigator> navigator() const
    {
        const auto* content = viewport_.getViewedComponent();
        if (content == nullptr)
            return std::nullopt;
        return talas::app::vcsNavigator(extent(), viewport_.getViewPositionX(), viewport_.getViewPositionY(),
            viewport_.getMaximumVisibleWidth(), viewport_.getMaximumVisibleHeight(), boxSize);
    }

    // The view moved by (dx, dy) of the box's pixels from where a drag began.
    std::optional<std::pair<int, int>> dragBy(int dx, int dy)
    {
        const auto shown = navigator();
        if (!shown)
            return std::nullopt;
        const auto moved = talas::app::vcsNavigatorDragged(*shown, extent(), start_.x, start_.y,
            viewport_.getMaximumVisibleWidth(), viewport_.getMaximumVisibleHeight(), dx, dy);
        viewport_.setViewPosition(moved.first, moved.second);
        repaint();
        return moved;
    }

    void beginDrag() { start_ = viewport_.getViewPosition(); }

    void paint(juce::Graphics& graphics) override
    {
        const auto shown = navigator();
        if (!shown)
            return;
        const auto box = [&](const talas::app::VcsRect& rect) {
            return juce::Rectangle<int>{rect.left, rect.top, rect.width, rect.height}.translated(
                (getWidth() - shown->surface.width) / 2, 0);
        };
        graphics.setColour(juce::Colours::grey.withAlpha(0.7F));
        graphics.fillRect(box(shown->surface));
        graphics.setColour(juce::Colour{0xff4f8cff}.withAlpha(0.8F));
        graphics.fillRect(box(shown->view));
    }

    void mouseDown(const juce::MouseEvent&) override { beginDrag(); }
    void mouseDrag(const juce::MouseEvent& event) override
    {
        (void) dragBy(event.getDistanceFromDragStartX(), event.getDistanceFromDragStartY());
    }

private:
    [[nodiscard]] talas::app::VcsRect extent() const
    {
        const auto* content = viewport_.getViewedComponent();
        return {0, 0, content == nullptr ? 0 : content->getWidth(), content == nullptr ? 0 : content->getHeight()};
    }

    void timerCallback() override
    {
        const auto shown = navigator().has_value();
        if (shown != isVisible())
            setVisible(shown);
        else if (shown)
            repaint();
    }

    juce::Viewport& viewport_;
    juce::Point<int> start_;
};

class VirtualControlSurfaceImpl final : public VirtualControlSurface,
                                        private juce::AsyncUpdater
{
public:
    using DocumentChangeCallback = std::function<void(bool)>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using FocusCallback = std::function<void(std::string_view)>;

    VirtualControlSurfaceImpl(talas::runtime::ProjectRuntimeBridge& bridge,
                          const talas::core::NodeRegistry& registry,
                          std::atomic<std::uint64_t>& midiUiStateToken,
                          MidiSourceNameCallback midiSourceName,
                          OscRouteAvailableCallback oscRouteAvailable,
                          OscEgressRouteAvailableCallback oscEgressRouteAvailable,
                          OscProfileDetailCallback oscProfileDetail,
                          ValueDispatchCallback dispatchValue,
                          LocalMapValueDispatchCallback dispatchLocalMapValue,
                          LocalMapStageReadyCallback localMapStageReady,
                          ValueGestureFinishedCallback onValueGestureFinished,
                          AcceptedValueCallback onAcceptedValue,
                          OscEgressAuthorityCallback beforeOscEgressAuthorityChange,
                          OscEgressAuthorityCallback restoreOscEgressAuthority,
                          DocumentChangeCallback onDocumentChange,
                          StatusCallback onStatus,
                          FocusCallback onFocus)
        : bridge_(bridge)
        , registry_(registry)
        , midiUiStateToken_(midiUiStateToken)
        , midiSourceName_(std::move(midiSourceName))
        , oscRouteAvailable_(std::move(oscRouteAvailable))
        , oscEgressRouteAvailable_(std::move(oscEgressRouteAvailable))
        , oscProfileDetail_(std::move(oscProfileDetail))
        , dispatchValue_(std::move(dispatchValue))
        , dispatchLocalMapValue_(std::move(dispatchLocalMapValue))
        , localMapStageReady_(std::move(localMapStageReady))
        , onValueGestureFinished_(std::move(onValueGestureFinished))
        , onAcceptedValue_(std::move(onAcceptedValue))
        , beforeOscEgressAuthorityChange_(
              std::move(beforeOscEgressAuthorityChange))
        , restoreOscEgressAuthority_(
              std::move(restoreOscEgressAuthority))
        , onDocumentChange_(std::move(onDocumentChange))
        , onStatus_(std::move(onStatus))
        , onFocus_(std::move(onFocus))
        , addButton_("Add")
    {
        describeControl(
            *this,
            "Event Values",
            "Create and operate live Event Value controls for the current project.");
        setFocusContainerType(
            juce::Component::FocusContainerType::none);
        addAndMakeVisible(nameInput_);
        addAndMakeVisible(domainInput_);
        addAndMakeVisible(kindInput_);
        addAndMakeVisible(addButton_);
        addAndMakeVisible(viewport_);
        addChildComponent(navigator_);
        addChildComponent(tabs_);
        tabs_.setTitle("Virtual Control Surface tabs");
        viewport_.setViewedComponent(&controlsContent_, false);
        viewport_.setScrollBarsShown(true, true);
        controlsContent_.addMouseListener(this, false);
        annotations_.setColour(juce::Label::textColourId, secondaryTextColour);
        annotations_.setFont(WorkbenchType::body());
        annotations_.setJustificationType(juce::Justification::topLeft);
        addChildComponent(annotations_);
        addChildComponent(displays_);
        nameInput_.setTextToShowWhenEmpty("Event name", secondaryTextColour);
        nameInput_.setFont(WorkbenchType::body());
        describeControl(
            nameInput_,
            "Event Value name",
            "Name of the Event Value control to add.");
        describeControl(
            domainInput_,
            "Event Value domain",
            "Unit domain for the new Event Value control.");
        describeControl(
            kindInput_,
            "Event Value control kind",
            "Choose a fader, momentary gate, or toggle.");
        nameInput_.onReturnKey = [this] { addControl(); };
        domainInput_.addItem("scalar", 1);
        domainInput_.addItem("hertz", 2);
        domainInput_.addItem("seconds", 3);
        domainInput_.addItem("note number", 4);
        domainInput_.addItem("samples", 5);
        domainInput_.setSelectedId(1, juce::dontSendNotification);
        kindInput_.addItem("fader", 1);
        kindInput_.addItem("gate", 2);
        kindInput_.addItem("toggle", 3);
        kindInput_.setSelectedId(1, juce::dontSendNotification);
        addButton_.setTooltip("Add Event Value");
        addButton_.onClick = [this] { addControl(); };
        for (auto* button : {&camera_, &dice_, &presetMenu_, &padlock_, &arrange_, &options_})
            addAndMakeVisible(*button);
        describeControl(padlock_, "Padlock", "Unlock the surface to move, resize and arrange its widgets; lock it to play them.");
        describeControl(arrange_, "Arrange", "Clean up the surface in columns or fit it to the window, or squeeze the selected widgets together.");
        padlock_.onClick = [this] { (void) setEditMode(!editMode_); };
        addAndMakeVisible(layoutList_);
        addAndMakeVisible(layoutMenu_);
        describeControl(layoutList_, "Layouts", "The surface's layouts: the top one with every control, one for each Sound, and those made here (Shift with the arrows).");
        describeControl(layoutMenu_, "Layout menu", "Duplicate, rename, remove or merge layouts, or remove the selected widgets from one.");
        layoutList_.onChange = [this] {
            const auto index = layoutList_.getSelectedItemIndex();
            if (index < 0)
                return;
            // Shift+Click chooses more layouts to merge.
            if (juce::ModifierKeys::currentModifiers.isShiftDown())
            {
                layoutsToMerge_ = talas::app::vcsLayoutsShiftClicked(
                    std::move(layoutsToMerge_), currentLayout_, static_cast<std::size_t>(index));
                layoutList_.setSelectedItemIndex(static_cast<int>(currentLayout_), juce::dontSendNotification);
                onStatus_(juce::String{static_cast<int>(layoutsToMerge_.size())} + " layouts chosen to merge");
                return;
            }
            layoutsToMerge_.clear();
            // Leaving a changed layout asks to save it.
            if (editMode_ && talas::app::vcsLayoutSwitchAsksToSave(unlockedSurface_, bridge_.editor().document()))
            {
                askToSaveBeforeSwitching(static_cast<std::size_t>(index));
                return;
            }
            (void) selectLayout(static_cast<std::size_t>(index));
        };
        layoutMenu_.onClick = [this] { showLayoutMenu(); };
        arrange_.onClick = [this] { showArrangeMenu(); };
        options_.onClick = [this] { showOptionsMenu(); };
        describeControl(options_, "Options", "Add a text or a box to the unlocked surface, edit a text, or remove the selected ones.");
        editOverlay_.widgets = [this] { return shownWidgetBounds(); };
        editOverlay_.selection = [this]() -> const std::set<std::string>& { return selectedWidgets_; };
        editOverlay_.select = [this](std::set<std::string> chosen) {
            selectedWidgets_ = std::move(chosen);
            editOverlay_.repaint();
            refreshSelectedValue();
            onStatus_(juceString("Selected Value: " + selectedValueText()));
        };
        // The VCS Editor's Selected Value: the
        // selected widget's name, its list every widget to select one by.
        selectedValue_.setTitle("Selected Value");
        selectedValue_.setTooltip("The selected widget; choose one from the list to select it");
        selectedValue_.onChange = [this] {
            const auto index = selectedValue_.getSelectedItemIndex();
            if (index < 0 || static_cast<std::size_t>(index) >= selectedValueNames_.size())
                return;
            selectedWidgets_ = {selectedValueNames_[static_cast<std::size_t>(index)]};
            editOverlay_.repaint();
            if (editorWindow_ != nullptr)
                editorWindow_->refresh();
            onStatus_(juceString("Selected Value: " + selectedValueText()));
        };
        addChildComponent(selectedValue_);
        editOverlay_.moved = [this](int dx, int dy) { (void) moveSelection(dx, dy); };
        editOverlay_.resized = [this](const std::string& name, talas::app::VcsCorner corner, int dx, int dy) {
            (void) resizeWidget(name, corner, dx, dy);
        };
        controlsContent_.addAndMakeVisible(decorationLayer_);
        controlsContent_.addChildComponent(editOverlay_);
        addAndMakeVisible(presetList_);
        describeControl(camera_, "Camera", "Take a snapshot of the current fader settings as a preset; with Ctrl, replace the selected preset (Enter).");
        describeControl(presetList_, "Presets", "Choose a preset; the up and down arrows select the next one. The numbers in brackets are MIDI program changes.");
        describeControl(dice_, "Dice", "Set every parameter that is not frozen to a random value (r).");
        describeControl(presetMenu_, "Preset menu", "Rename or remove the selected preset, or remove all presets but Default.");
        camera_.onClick = [this] { (void) takePreset(juce::ModifierKeys::currentModifiers.isCtrlDown()); };
        dice_.onClick = [this] { (void) rollDice(); };
        presetMenu_.onClick = [this] { showPresetMenu(); };
        presetList_.onChange = [this] {
            const auto index = presetList_.getSelectedItemIndex();
            if (!refreshingPresets_ && index >= 0)
                (void) selectPreset(index);
        };
        refresh();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        // Tab and Shift+Tab: the next or previous fader's number, selected
        // to type a new value.
        if (!editMode_ && key.getKeyCode() == juce::KeyPress::tabKey && !key.getModifiers().isCommandDown()
            && !key.getModifiers().isCtrlDown())
            return tabToNumber(!key.getModifiers().isShiftDown());
        if (editMode_)
        {
            const auto code = key.getKeyCode();
            if ((code == juce::KeyPress::backspaceKey || code == juce::KeyPress::deleteKey) && !selectedDecorations().empty())
                return removeSelectedDecorations();
            // Every widget selected, the selected ones copied, and widgets
            // or text pasted into the unlocked surface.
            if (key.getModifiers().isCommandDown() && (code == 'a' || code == 'A'))
                return selectAllWidgets();
            if (key.getModifiers().isCommandDown() && (code == 'c' || code == 'C'))
                return copySelectedWidgets(true);
            if (key.getModifiers().isCommandDown() && (code == 'v' || code == 'V'))
            {
                if (auto widgets = copiedVirtualControlWidgets(); !widgets.empty())
                    return pasteWidgets(std::move(widgets));
                return pasteText(juce::SystemClipboard::getTextFromClipboard().toStdString());
            }
        }
        return presetKey(key);
    }

    bool tabToNumber(bool forward)
    {
        std::vector<VirtualControlRow*> numbered;
        std::optional<std::size_t> current;
        for (auto* row : rows_)
        {
            if (!row->showsNumber())
                continue;
            if (row->typingNumber() || (!current && row->eventName() == typedNumber_))
                current = numbered.size();
            numbered.push_back(row);
        }
        const auto next = talas::app::vcsTabbedNumber(numbered.size(), current, forward);
        if (!next)
            return false;
        typedNumber_ = numbered[*next]->eventName();
        numbered[*next]->typeNumber();
        return true;
    }

    [[nodiscard]] juce::String annotationTextForSmoke() override
    {
        return annotations_.isVisible() ? annotations_.getText() : juce::String{};
    }

    [[nodiscard]] std::optional<double> invisibleFaderForSmoke(std::string_view eventName, double dragFraction) override
    {
        const auto found = std::ranges::find_if(
            rows_, [&](const VirtualControlRow* row) { return row->eventName() == eventName; });
        return found == rows_.end() ? std::nullopt : (*found)->invisibleFaderForSmoke(dragFraction);
    }

    [[nodiscard]] std::pair<bool, juce::Point<int>> handDragForSmoke(int dx, int dy) override
    {
        modifierKeysChanged(juce::ModifierKeys{juce::ModifierKeys::ctrlModifier});
        const auto shown = hand_ && controlsContent_.getMouseCursor() == juce::MouseCursor{juce::MouseCursor::DraggingHandCursor};
        handStart_ = viewport_.getViewPosition();
        pushSurface(dx, dy);
        modifierKeysChanged(juce::ModifierKeys{});
        return {shown && !hand_, viewport_.getViewPosition()};
    }

    // With the Control key down the cursor is a hand
    // that pushes the surface around in its window; the widgets take no
    // clicks meanwhile.
    void modifierKeysChanged(const juce::ModifierKeys& modifiers) override
    {
        const auto on = modifiers.isCtrlDown();
        if (hand_ == on)
            return;
        hand_ = on;
        controlsContent_.setInterceptsMouseClicks(true, !on);
        controlsContent_.setMouseCursor(on ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        if (hand_ && event.eventComponent == &controlsContent_)
            handStart_ = viewport_.getViewPosition();
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (hand_ && event.eventComponent == &controlsContent_)
        {
            const auto offset = event.getEventRelativeTo(this).getOffsetFromDragStart();
            pushSurface(offset.x, offset.y);
        }
    }

    void pushSurface(int dx, int dy)
    {
        const auto [left, top] = talas::app::vcsHandDragged(extent(), handStart_.x, handStart_.y,
            viewport_.getMaximumVisibleWidth(), viewport_.getMaximumVisibleHeight(), dx, dy);
        viewport_.setViewPosition(left, top);
    }

    [[nodiscard]] talas::app::VcsRect extent() const
    {
        return {0, 0, controlsContent_.getWidth(), controlsContent_.getHeight()};
    }

    [[nodiscard]] std::optional<double> fineTuneForSmoke(std::string_view eventName, double dragFraction) override
    {
        const auto found = std::ranges::find_if(
            rows_, [&](const VirtualControlRow* row) { return row->eventName() == eventName; });
        if (found == rows_.end())
            return std::nullopt;
        return (*found)->fineTuneForSmoke(dragFraction);
    }

    [[nodiscard]] std::string tabToNumberForSmoke(bool forward) override
    {
        return tabToNumber(forward) ? typedNumber_ : std::string{};
    }

    [[nodiscard]] bool chooseSelectedValueForSmoke(std::string_view name) override
    {
        refreshSelectedValue();
        const auto found = std::ranges::find(selectedValueNames_, name);
        if (!editMode_ || found == selectedValueNames_.end())
            return false;
        selectedValue_.setSelectedItemIndex(static_cast<int>(found - selectedValueNames_.begin()), juce::sendNotificationSync);
        return selectedWidgets_ == std::set<std::string>{std::string{name}};
    }

    [[nodiscard]] bool addDecorationForSmoke(talas::project::VirtualControlDecorationKind kind, std::string text) override
    {
        return addDecoration(kind, std::move(text));
    }

    [[nodiscard]] bool pasteTextForSmoke(std::string text) override
    {
        return pasteText(std::move(text));
    }

    [[nodiscard]] bool selectAllWidgetsForSmoke() override
    {
        return selectAllWidgets();
    }

    [[nodiscard]] bool copyWidgetsForSmoke() override
    {
        return copySelectedWidgets(false);
    }

    [[nodiscard]] bool pasteWidgetsForSmoke() override
    {
        return pasteWidgets(copiedVirtualControlWidgets(false));
    }

    [[nodiscard]] bool editDecorationTextForSmoke(std::size_t index, std::string text) override
    {
        return editDecorationText(index, std::move(text));
    }

    [[nodiscard]] bool removeSelectedDecorationsForSmoke() override
    {
        return removeSelectedDecorations();
    }

    [[nodiscard]] std::vector<juce::Rectangle<int>> decorationBoundsForSmoke() const override
    {
        return decorationRects_;
    }

    bool selectPresetForProgramChange(unsigned program) override
    {
        const auto* bank = talas::app::findVcsPresetBank(bridge_.editor().document());
        if (bank == nullptr || !talas::app::vcsPresetForProgram(*bank, program).has_value())
            return false;
        return selectPreset(static_cast<int>(program) - 1);
    }

    [[nodiscard]] bool takePresetForSmoke(bool replaceSelected) override
    {
        return takePreset(replaceSelected);
    }

    [[nodiscard]] std::vector<juce::String> presetLabelsForSmoke() const override
    {
        std::vector<juce::String> labels;
        for (int index = 0; index < presetList_.getNumItems(); ++index)
            labels.push_back(presetList_.getItemText(index));
        return labels;
    }

    [[nodiscard]] bool selectPresetForSmoke(int index) override
    {
        return selectPreset(index);
    }

    [[nodiscard]] bool rollDiceForSmoke() override
    {
        return rollDice();
    }

    [[nodiscard]] bool toggleFrozenForSmoke(std::string_view eventName) override
    {
        return toggleFrozen(eventName);
    }

    [[nodiscard]] bool presetKeyForSmoke(const juce::KeyPress& key) override
    {
        return presetKey(key);
    }

    [[nodiscard]] bool removePresetsBesidesDefaultForSmoke() override
    {
        return removePresetsBesidesDefault();
    }

    // A smoke locking the surface saves its changes unless it discards them.
    [[nodiscard]] bool setEditModeForSmoke(bool editing) override
    {
        lockAnswer_ = true;
        const auto set = setEditMode(editing);
        lockAnswer_.reset();
        return set;
    }

    [[nodiscard]] std::optional<std::pair<int, int>> navigatorDragForSmoke(int dx, int dy) override
    {
        navigator_.beginDrag();
        return navigator_.dragBy(dx, dy);
    }

    [[nodiscard]] bool lockDiscardingForSmoke() override
    {
        lockAnswer_ = false;
        const auto locked = setEditMode(false);
        lockAnswer_.reset();
        return locked;
    }

    [[nodiscard]] bool selectWidgetsForSmoke(std::vector<std::string> names) override
    {
        if (!editMode_)
            return false;
        selectedWidgets_ = {names.begin(), names.end()};
        return true;
    }

    [[nodiscard]] bool moveSelectionForSmoke(int dx, int dy) override
    {
        return moveSelection(dx, dy);
    }

    [[nodiscard]] bool resizeWidgetForSmoke(std::string_view name, int dx, int dy) override
    {
        return resizeWidget(std::string{name}, talas::app::VcsCorner::BottomRight, dx, dy);
    }

    [[nodiscard]] bool cleanUpForSmoke(unsigned columns, bool fit) override
    {
        return cleanUp(columns, fit);
    }

    [[nodiscard]] bool squeezeForSmoke(bool vertical) override
    {
        return squeeze(vertical);
    }

    [[nodiscard]] std::optional<juce::Rectangle<int>> widgetBoundsForSmoke(std::string_view name) const override
    {
        for (auto* row : rows_)
        {
            if (row->eventName() == name && row->isVisible())
                return row->getBounds();
        }
        return std::nullopt;
    }

    [[nodiscard]] juce::String selectedValueForSmoke() const override
    {
        return juceString(selectedValueText());
    }

    [[nodiscard]] std::vector<juce::String> layoutLabelsForSmoke() const override
    {
        std::vector<juce::String> labels;
        for (int index = 0; index < layoutList_.getNumItems(); ++index)
            labels.push_back(layoutList_.getItemText(index));
        return labels;
    }

    [[nodiscard]] bool selectLayoutForSmoke(std::size_t index) override
    {
        return selectLayout(index);
    }

    [[nodiscard]] bool duplicateLayoutForSmoke() override
    {
        return duplicateLayout();
    }

    [[nodiscard]] bool mergeLayoutForSmoke(std::size_t other) override
    {
        return mergeLayoutWith(other);
    }

    [[nodiscard]] bool removeLayoutForSmoke() override
    {
        return removeLayout();
    }

    [[nodiscard]] bool alignForSmoke(int alignment) override
    {
        return alignSelection(static_cast<talas::app::VcsAlignment>(alignment));
    }

    [[nodiscard]] bool distributeForSmoke(bool horizontal) override
    {
        return distributeSelection(horizontal);
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::group);
    }

    ~VirtualControlSurfaceImpl() override
    {
        cancelPendingUpdate();
        for (const auto& token : {valueGestureToken_, midiTransactionToken_})
        {
            if (token.has_value())
                bridge_.editor().endOwnedHistoryTransaction(*token);
        }
    }

    void refresh() override
    {
        // A row's value can apply a routed controller's queued event, whose
        // refresh would delete the row within its own callback; the
        // rebuilding waits until the row is done.
        if (sendingRowValue_)
        {
            if (!refreshDeferred_)
            {
                refreshDeferred_ = true;
                juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(this);
                juce::MessageManager::callAsync([safeThis] {
                    if (safeThis == nullptr)
                        return;
                    safeThis->refreshDeferred_ = false;
                    safeThis->refresh();
                });
            }
            return;
        }
        // The Annotations' texts at the top.
        {
            juce::StringArray texts;
            for (const auto& text : talas::app::vcsAnnotationTexts(bridge_.editor().document()))
                texts.add(juceString(text));
            annotations_.setText(texts.joinIntoString("\n"), juce::dontSendNotification);
            annotations_.setVisible(!texts.isEmpty());
        }
        std::optional<std::string> focusedEvent;
        std::optional<juce::String> focusedIdentity;
        std::optional<juce::String> focusedLocalIdentity;
        std::unordered_map<std::string, double> localValues;
        const auto stageGeneration = bridge_.latestStageGeneration();
        const auto preserveLocalValues = localMapStageReady_()
            && localStageGeneration_ == stageGeneration;
        for (const auto* row : localRows_)
        {
            if (row == nullptr)
                continue;
            if (preserveLocalValues)
            {
                localValues.emplace(row->getComponentID().toStdString(),
                    row->normalizedValue());
            }
            if (row->hasKeyboardFocus(true))
                focusedLocalIdentity = row->getComponentID();
        }
        for (const auto* row : rows_)
        {
            if (row != nullptr && row->hasKeyboardFocus(true))
            {
                focusedEvent = row->eventName();
                focusedIdentity = row->focusedComponentIdentity();
                break;
            }
        }
        // Rebuilding deletes a row mid-gesture, so its drag end never comes.
        if (valueGestureToken_.has_value())
        {
            bridge_.editor().endOwnedHistoryTransaction(*valueGestureToken_);
            valueGestureToken_.reset();
            juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(
                this);
            juce::MessageManager::callAsync([safeThis] {
                if (safeThis != nullptr)
                    safeThis->onValueGestureFinished_();
            });
        }
        rows_.clear(true);
        localRows_.clear(true);
        // Only the Timeline's live controls show here: an Event Value an
        // Automated lane drives is left off.
        const auto automated = talas::app::automatedEventNames(bridge_.editor().document().timeline.score);
        // The controls alphabetically, as the layout shows them.
        const auto controls = talas::app::vcsLayoutControls(bridge_.editor().document(), ownLayout());
        for (const auto index : talas::app::vcsAlphabeticalOrder(controls))
        {
            const auto& control = controls[index];
            if (std::ranges::find(automated, control.eventName) != automated.end())
                continue;
            std::optional<std::string> connectedMidiSourceName;
            if (control.midiMapping.has_value()
                && control.midiMapping->sourceMode
                    == talas::project::MidiControlSourceMode::ExactInput)
            {
                connectedMidiSourceName = midiSourceName_(
                    control.midiMapping->sourceIdentifier);
            }
            auto* row = rows_.add(new VirtualControlRow(
                control,
                std::move(connectedMidiSourceName),
                control.oscMapping.has_value()
                    && oscRouteAvailable_(
                        control.eventName,
                        control.oscMapping->address),
                control.oscEgressMapping.has_value()
                    && oscEgressRouteAvailable_(
                        control.eventName,
                        control.oscEgressMapping->address),
                oscProfileDetail_(),
                [this] {
                    finishMidiTransaction();
                    beginValueGesture();
                },
                [this](std::string_view eventName, double value, talas::expr::ValueDomain) {
                    sendingRowValue_ = true;
                    const auto rejected = dispatchValue_(
                        eventName,
                        value,
                        talas::app::PerformanceActivitySource::UserInterface,
                        {});
                    sendingRowValue_ = false;
                    if (rejected.has_value())
                    {
                        onStatus_(*rejected);
                        return;
                    }
                    if (onAcceptedValue_)
                        onAcceptedValue_(eventName, value);
                },
                [this] { finishValueGesture(); },
                [this](std::string_view eventName) { removeControl(eventName); },
                [this](std::string_view eventName) { beginMidiLearn(eventName); },
                [this](std::string_view eventName) { clearMidiMapping(eventName); },
                [this](
                    std::string_view eventName,
                    std::optional<talas::project::OscControlMapping> mapping) {
                    setOscMapping(eventName, std::move(mapping));
                },
                [this](
                    std::string_view eventName,
                    std::optional<talas::project::OscEgressMapping> mapping) {
                    setOscEgressMapping(eventName, std::move(mapping));
                },
                [this](std::string_view eventName) {
                    noteFocusedEventValue(eventName);
                },
                [this](std::string_view eventName) { editWidget(eventName); }));
            const auto* presets = presetBank();
            row->setFreezing([this](std::string_view eventName) { (void) toggleFrozen(eventName); },
                presets != nullptr && talas::app::vcsControlFrozen(*presets, control));
            controlsContent_.addAndMakeVisible(row);
        }
        const auto discovered = talas::project::discoverProjectLocalMapControls(
            bridge_.editor().document(), registry_);
        for (const auto& control : discovered.controls)
        {
            if (control.displayAs
                    == talas::project::MidiMapperMapDisplayAs::Nothing
                || !control.domain.has_value()
                || (control.range.has_value()
                    && (*control.range)[0] >= (*control.range)[1])
                || (!control.range.has_value()
                    && *control.domain != talas::expr::ValueDomain::Scalar))
            {
                continue;
            }
            const auto sourceName =
                talas::runtime::resolvePreparedMidiMapperLocalSource(
                    control.sourceName, control.channel);
            if (!sourceName.has_value()
                || !control.sourceName.starts_with("MIDIController"))
            {
                continue;
            }
            double normalized = 0.0;
            const auto sourceControl = std::ranges::find(
                bridge_.editor().document().virtualControls,
                *sourceName,
                &talas::project::VirtualControl::eventName);
            if (sourceControl != bridge_.editor().document().virtualControls.end()
                && sourceControl->domain == talas::expr::ValueDomain::Scalar)
            {
                normalized = sourceControl->value;
            }
            const std::array fields{
                std::string_view{control.flatMapperNodeId},
                std::string_view{control.eventName}};
            const auto identity = workbenchStableFocusItemId(
                "local-map-control", fields);
            if (const auto previous = localValues.find(identity);
                previous != localValues.end())
            {
                normalized = previous->second;
            }
            auto* row = localRows_.add(new LocalMapControlRow(
                control, *sourceName, normalized,
                localMapStageReady_()
                    && bridge_.realtimeEventValueIdForLatestStage(
                        *sourceName, talas::expr::ValueDomain::Scalar).has_value(),
                [this](std::string_view source, double value) {
                    const auto rejected = dispatchLocalMapValue_(source, value);
                    if (!rejected.has_value())
                    {
                        for (auto* sibling : localRows_)
                        {
                            if (sibling->sourceEventName() == source)
                                sibling->setExternalNormalizedValue(value);
                        }
                    }
                    return rejected;
                },
                onStatus_));
            controlsContent_.addAndMakeVisible(row);
        }
        if (focusedEventName_.has_value()
            && std::ranges::none_of(
                rows_,
                [&](const auto* row) {
                    return row != nullptr
                        && row->eventName()
                            == *focusedEventName_;
                }))
        {
            focusedEventName_.reset();
        }
        // Each OscilloscopeDisplay, named for its Sound.
        const auto displaysShown = displays_.isVisible();
        displays_.setDisplays(talas::app::vcsOscilloscopeDisplays(bridge_.editor().document()));
        if (displays_.isVisible() != displaysShown)
            resized();
        refreshTabs();
        refreshLayoutList();
        refreshPresetList();
        showPerformanceValues();
        layoutRows();
        if (focusedEvent.has_value() && isShowing())
        {
            const auto found = std::ranges::find_if(
                rows_,
                [&](const auto* row) {
                    return row != nullptr
                        && row->eventName() == *focusedEvent;
                });
            if (found == rows_.end() || !focusedIdentity.has_value()
                || !(*found)->restoreFocusedComponent(*focusedIdentity))
            {
                (void)focusEventValue(*focusedEvent);
            }
        }
        if (focusedLocalIdentity.has_value() && isShowing())
        {
            const auto found = std::ranges::find_if(
                localRows_, [&](const auto* row) {
                    return row != nullptr
                        && row->getComponentID() == *focusedLocalIdentity;
                });
            if (found != localRows_.end())
                (*found)->focusPrimaryControl();
        }
        if (auto* handler = getAccessibilityHandler())
        {
            handler->notifyAccessibilityEvent(
                juce::AccessibilityEvent::structureChanged);
        }
        localStageGeneration_ = localMapStageReady_()
            ? stageGeneration : 0;
        repaint();
    }

    bool focusEventValue(std::string_view eventName) override
    {
        const auto found = std::ranges::find_if(
            rows_,
            [&](const auto* row) {
                return row != nullptr
                    && row->eventName() == eventName;
            });
        if (found == rows_.end())
            return false;
        noteFocusedEventValue(eventName);
        if (isShowing())
            (*found)->focusPrimaryControl();
        return true;
    }

    [[nodiscard]] bool focusEventValueForSmoke(
        std::string_view eventName) override
    {
        return focusEventValue(eventName);
    }

    [[nodiscard]] std::optional<std::string>
    focusedEventValueForSmoke() const override
    {
        return focusedEventName_;
    }

    [[nodiscard]] bool beginMidiLearnForSmoke(
        std::string_view eventName) override
    {
        if (std::ranges::none_of(
                rows_,
                [&](const auto* row) {
                    return row != nullptr
                        && row->eventName() == eventName;
                }))
        {
            return false;
        }
        beginMidiLearn(eventName);
        return true;
    }

    [[nodiscard]] std::optional<std::string>
    midiMappingPresentationForSmoke(
        std::string_view eventName) const override
    {
        const auto found = std::ranges::find_if(
            rows_,
            [&](const auto* row) {
                return row != nullptr
                    && row->eventName() == eventName;
            });
        if (found == rows_.end())
            return std::nullopt;
        return (*found)->midiMappingPresentationForSmoke();
    }

    [[nodiscard]] std::optional<std::string>
    oscMappingPresentationForSmoke(
        std::string_view eventName) const override
    {
        const auto found = std::ranges::find_if(
            rows_,
            [&](const auto* row) {
                return row != nullptr
                    && row->eventName() == eventName;
            });
        if (found == rows_.end())
            return std::nullopt;
        return (*found)->oscMappingPresentationForSmoke();
    }

    [[nodiscard]] std::optional<std::string>
    setOscMappingForSmoke(
        std::string_view eventName,
        std::optional<std::string> address) override
    {
        const auto result = bridge_.editor().setVirtualControlOscMapping(
            eventName,
            address.has_value()
                ? std::optional<talas::project::OscControlMapping>{
                      talas::project::OscControlMapping{*address}}
                : std::nullopt);
        if (!result.ok())
            return result.message;
        onDocumentChange_(false);
        refresh();
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string>
    oscEgressMappingPresentationForSmoke(
        std::string_view eventName) const override
    {
        const auto found = std::ranges::find_if(
            rows_,
            [&](const auto* row) {
                return row != nullptr
                    && row->eventName() == eventName;
            });
        if (found == rows_.end())
            return std::nullopt;
        return (*found)->oscEgressMappingPresentationForSmoke();
    }

    [[nodiscard]] std::optional<std::string>
    setOscEgressMappingForSmoke(
        std::string_view eventName,
        std::optional<std::string> address) override
    {
        const auto result =
            bridge_.editor().setVirtualControlOscEgressMapping(
                eventName,
                address.has_value()
                    ? std::optional<talas::project::OscEgressMapping>{
                          talas::project::OscEgressMapping{*address}}
                    : std::nullopt);
        if (!result.ok())
            return result.message;
        onDocumentChange_(false);
        refresh();
        return std::nullopt;
    }

    [[nodiscard]] bool setValueForSmoke(
        std::string_view eventName,
        double value) override
    {
        const auto found = std::ranges::find_if(
            rows_,
            [&](const auto* row) {
                return row != nullptr && row->eventName() == eventName;
            });
        return found != rows_.end() && (*found)->setValueForSmoke(value);
    }

    [[nodiscard]] bool startValueGestureForSmoke(
        std::string_view eventName,
        double value) override
    {
        const auto found = std::ranges::find_if(
            rows_,
            [&](const auto* row) {
                return row != nullptr && row->eventName() == eventName;
            });
        return found != rows_.end()
            && (*found)->startValueGestureForSmoke(value);
    }

    [[nodiscard]] std::optional<ControlPresentation>
    controlPresentationForSmoke(
        std::string_view eventName) const override
    {
        const auto found = std::ranges::find_if(
            rows_, [&](const VirtualControlRow* row) {
                return row->eventName() == eventName;
            });
        if (found == rows_.end()) return std::nullopt;
        return (*found)->controlPresentationForSmoke();
    }

    [[nodiscard]] bool applyWidgetForSmoke(
        std::string_view eventName,
        const talas::project::VirtualControlWidget& widget,
        double minimum,
        double maximum) override
    {
        return applyWidget(eventName, widget, minimum, maximum);
    }

    void setShowAllDigits(bool shown) override
    {
        virtualControlNumbersShowAllDigits.store(shown, std::memory_order_relaxed);
        for (auto* row : rows_)
            row->refreshNumberText();
    }

    [[nodiscard]] std::optional<WidgetPresentation>
    widgetPresentationForSmoke(std::string_view eventName) const override
    {
        const auto found = std::ranges::find_if(
            rows_, [&](const VirtualControlRow* row) { return row->eventName() == eventName; });
        if (found == rows_.end())
            return std::nullopt;
        return (*found)->widgetPresentationForSmoke();
    }

    [[nodiscard]] std::optional<double> widgetValueAtPositionForSmoke(
        std::string_view eventName, double position) const override
    {
        const auto found = std::ranges::find_if(
            rows_, [&](const VirtualControlRow* row) { return row->eventName() == eventName; });
        if (found == rows_.end())
            return std::nullopt;
        return (*found)->valueAtPositionForSmoke(position);
    }

    [[nodiscard]] std::vector<std::string> tabsForSmoke() const override
    {
        return tabs_.isVisible() ? tabNames_ : std::vector<std::string>{};
    }

    [[nodiscard]] bool selectTabForSmoke(std::string_view tab) override
    {
        const auto found = std::ranges::find(tabNames_, tab);
        if (found == tabNames_.end())
            return false;
        tabs_.setCurrentTabIndex(static_cast<int>(found - tabNames_.begin()), true);
        return currentTab_ == tab;
    }

    [[nodiscard]] bool operateButtonForSmoke(
        std::string_view eventName, bool pressed) override
    {
        const auto found = std::ranges::find_if(
            rows_, [&](const VirtualControlRow* row) {
                return row->eventName() == eventName;
            });
        return found != rows_.end()
            && (*found)->operateButtonForSmoke(pressed);
    }

    [[nodiscard]] std::optional<LocalMapControlPresentation>
    localMapControlPresentationForSmoke(
        std::string_view mapperNodeId,
        std::string_view eventName) const override
    {
        const auto found = std::ranges::find_if(
            localRows_, [&](const auto* row) {
                return row != nullptr
                    && row->control().flatMapperNodeId == mapperNodeId
                    && row->control().eventName == eventName;
            });
        if (found == localRows_.end())
            return std::nullopt;
        const auto& row = **found;
        return LocalMapControlPresentation{
            row.sourceEventName(),
            row.control().range.value_or(std::array<double, 2>{0.0, 1.0})[0],
            row.control().range.value_or(std::array<double, 2>{0.0, 1.0})[1],
            row.currentValue(),
            row.enabled(),
            row.rangeText(),
            row.displayType()};
    }

    [[nodiscard]] bool setLocalMapValueForSmoke(
        std::string_view mapperNodeId,
        std::string_view eventName,
        double value) override
    {
        const auto found = std::ranges::find_if(
            localRows_, [&](const auto* row) {
                return row != nullptr
                    && row->control().flatMapperNodeId == mapperNodeId
                    && row->control().eventName == eventName;
            });
        return found != localRows_.end() && (*found)->setValue(value);
    }

    [[nodiscard]] bool operateLocalMapButtonForSmoke(
        std::string_view mapperNodeId,
        std::string_view eventName,
        bool pressed) override
    {
        const auto found = std::ranges::find_if(
            localRows_, [&](const auto* row) {
                return row != nullptr
                    && row->control().flatMapperNodeId == mapperNodeId
                    && row->control().eventName == eventName;
            });
        return found != localRows_.end()
            && (*found)->operateButtonForSmoke(pressed);
    }

    [[nodiscard]] bool scaledRowsScrollableForSmoke() override
    {
        if (controlsContent_.getHeight() <= viewport_.getHeight())
            return true;
        const auto& vertical = viewport_.getVerticalScrollBar();
        return vertical.isVisible()
            && vertical.getMaximumRangeLimit() > viewport_.getHeight();
    }

    [[nodiscard]] std::optional<std::string>
    runMidiMappingWorkflowForSmoke(
        std::string_view eventName,
        talas::project::MidiControllerMessage message) override
    {
        const auto presentation = [&]() {
            return midiMappingPresentationForSmoke(eventName);
        };
        if (!presentation().has_value())
            return "Event Value MIDI smoke target was unavailable";

        beginMidiLearn(eventName);
        if (!midiLearnActive()
            || !applyMidiController(message, "smoke-midi-input", {}, false))
        {
            return "Event Value MIDI learn did not consume the selected CC source";
        }
        const auto mapped = presentation();
        const auto mappedSuffix =
            "|Unmap Ch" + std::to_string(message.channel)
            + " CC" + std::to_string(message.controller)
            + "|Unmap Smoke MIDI Input (smoke-midi-input) · Ch "
            + std::to_string(message.channel)
            + " CC " + std::to_string(message.controller)
            + " from " + std::string(eventName);
        if (!mapped.has_value()
            || mapped->find(mappedSuffix) == std::string::npos
            || mapped->find("smoke-midi-input") == std::string::npos
            || midiLearnActive())
        {
            return "mapped Event Value did not expose its exact source and Unmap action: "
                + mapped.value_or("<missing>");
        }

        clearMidiMapping(eventName);
        const auto unmapped = presentation();
        if (!unmapped.has_value()
            || !unmapped->ends_with(
                "|Learn|Learn a MIDI controller"))
        {
            return "explicit Event Value MIDI unmap did not restore the Learn state";
        }

        beginMidiLearn(eventName);
        if (!applyMidiController(message, "smoke-midi-input", {}, false))
        {
            return "Event Value MIDI mapping could not be restored for realtime staging";
        }
        return std::nullopt;
    }

    bool applyMidiController(
        talas::project::MidiControllerMessage message,
        std::string_view sourceIdentifier,
        talas::runtime::ControllerRecordingIngressEvidence recording,
        bool builtInControllerAccepted) override
    {
        if (midiLearnEventName_.has_value())
        {
            if (sourceIdentifier.empty())
                return true;
            const auto result = bridge_.editor().setVirtualControlMidiMapping(
                *midiLearnEventName_,
                talas::project::MidiControlMapping{
                    message.channel,
                    message.controller,
                    talas::project::MidiControlSourceMode::ExactInput,
                    std::string(sourceIdentifier)});
            if (!result.ok())
            {
                onStatus_(juceString(result.message));
                return true;
            }
            onStatus_(juceString(result.message));
            midiLearnEventName_.reset();
            onDocumentChange_(false);
            refresh();
            publishMidiLearnState(false);
            return true;
        }

        bool localFeedbackAccepted = false;
        if (builtInControllerAccepted && localMapStageReady_())
        {
            auto source = std::string("cc");
            if (message.controller < 10) source += '0';
            source += std::to_string(message.controller);
            const auto defaultChannel =
                bridge_.defaultMidiChannelForLatestStage();
            auto taggedSource = source + "_ch";
            if (message.channel < 10) taggedSource += '0';
            taggedSource += std::to_string(message.channel);
            for (auto* row : localRows_)
            {
                if ((defaultChannel == message.channel
                        && row->sourceEventName() == source)
                    || row->sourceEventName() == taggedSource)
                {
                    row->setExternalNormalizedValue(
                        static_cast<double>(message.value) / 127.0);
                    localFeedbackAccepted = true;
                }
            }
        }
        bool builtInFeedbackAccepted = false;
        if (builtInControllerAccepted
            && bridge_.defaultMidiChannelForLatestStage()
                == message.channel)
        {
            const auto normalized =
                static_cast<double>(message.value) / 127.0;
            const auto binary = message.value >= 64 ? 1.0 : 0.0;
            const auto globalMap = talas::project::projectGlobalMapEntries(bridge_.editor().document());
            for (auto* row : rows_)
            {
                const auto builtIn =
                    talas::project::parseBuiltInMidiControllerName(
                        row->eventName());
                // ADR-0024: a global map name carries the controller's
                // value, as the map scales it, unless the project maps it
                // itself.
                const auto mapped = builtIn.has_value() ? globalMap.end()
                    : std::ranges::find(globalMap, row->eventName(), &talas::project::GlobalMapEntry::eventName);
                if (mapped != globalMap.end() && !mapped->pitchBend
                    && mapped->controller == message.controller
                    && mapped->channel.value_or(static_cast<std::uint8_t>(message.channel)) == message.channel)
                {
                    if (const auto value = talas::project::globalMapValue(*mapped, normalized))
                        row->setExternalValue(*value);
                    builtInFeedbackAccepted = true;
                    continue;
                }
                if (!builtIn || builtIn->number != message.controller)
                    continue;
                row->setExternalValue(
                    builtIn->kind == talas::project::VirtualControlKind::Fader
                        ? normalized : binary);
                builtInFeedbackAccepted = true;
            }
        }
        const auto mapped = talas::project::mapMidiController(
            bridge_.editor().document(), message, sourceIdentifier);
        if (!mapped.has_value())
            return localFeedbackAccepted || builtInFeedbackAccepted;

        beginMidiTransaction();
        const auto rejected = dispatchValue_(
            mapped->eventName,
            mapped->value,
            talas::app::PerformanceActivitySource::Midi,
            recording);
        if (rejected.has_value())
        {
            onStatus_(*rejected);
            return localFeedbackAccepted || builtInFeedbackAccepted;
        }
        for (auto* row : rows_)
        {
            if (row->eventName() == mapped->eventName)
            {
                row->setExternalValue(mapped->value);
                break;
            }
        }
        midiLastUpdateMs_ = juce::Time::getMillisecondCounter();
        if (onAcceptedValue_)
            onAcceptedValue_(mapped->eventName, mapped->value);
        return true;
    }

    void cancelMidiLearn() noexcept override
    {
        midiLearnEventName_.reset();
        publishMidiLearnState(false);
    }

    void finishMidiTransactionNow() override
    {
        finishMidiTransaction();
    }

    void finishMidiTransactionIfIdle() override
    {
        if (midiTransactionToken_.has_value()
            && juce::Time::getMillisecondCounter() - midiLastUpdateMs_ >= 250)
            finishMidiTransaction();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
        graphics.setColour(panelBorderColour);
        graphics.drawRect(getLocalBounds(), 1);
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::panelTitle());
        graphics.drawText("Event Values", getLocalBounds().reduced(14).removeFromTop(24), juce::Justification::centredLeft);
    }

    void resized() override
    {
        const auto scaled = [](int pixels) {
            return workbenchScaledUiPixels(pixels);
        };
        auto bounds = getLocalBounds().reduced(scaled(8));
        auto header = bounds.removeFromTop(scaled(24));
        addButton_.setBounds(header.removeFromRight(scaled(48)));
        header.removeFromRight(scaled(6));
        kindInput_.setBounds(header.removeFromRight(scaled(80)));
        header.removeFromRight(scaled(5));
        domainInput_.setBounds(header.removeFromRight(scaled(105)));
        header.removeFromRight(scaled(5));
        nameInput_.setBounds(header);
        bounds.removeFromTop(scaled(4));
        // The camera, the preset list and the dice at the upper left.
        auto presets = bounds.removeFromTop(scaled(24));
        camera_.setBounds(presets.removeFromLeft(scaled(64)));
        presets.removeFromLeft(scaled(4));
        padlock_.setBounds(presets.removeFromRight(scaled(60)));
        presets.removeFromRight(scaled(4));
        layoutMenu_.setBounds(presets.removeFromRight(scaled(60)));
        presets.removeFromRight(scaled(4));
        layoutList_.setBounds(presets.removeFromRight(std::min(scaled(160), presets.getWidth() / 3)));
        presets.removeFromRight(scaled(4));
        options_.setBounds(presets.removeFromRight(scaled(64)));
        presets.removeFromRight(scaled(4));
        arrange_.setBounds(presets.removeFromRight(scaled(64)));
        presets.removeFromRight(scaled(4));
        presetMenu_.setBounds(presets.removeFromRight(scaled(28)));
        presets.removeFromRight(scaled(4));
        dice_.setBounds(presets.removeFromRight(scaled(48)));
        presets.removeFromRight(scaled(4));
        presetList_.setBounds(presets);
        // Unlocked, the Selected Value takes the presets' place.
        selectedValue_.setBounds(presets);
        selectedValue_.setVisible(editMode_);
        presetList_.setVisible(!editMode_);
        bounds.removeFromTop(scaled(4));
        if (annotations_.isVisible())
        {
            const auto lines = juce::StringArray::fromLines(annotations_.getText()).size();
            annotations_.setBounds(bounds.removeFromTop(scaled(16) * std::max(1, lines) + scaled(4)));
        }
        if (displays_.isVisible())
            displays_.setBounds(bounds.removeFromTop(VcsDisplayStrip::preferredHeight() + scaled(4)).withTrimmedBottom(scaled(4)));
        if (tabs_.isVisible())
        {
            tabs_.setBounds(bounds.removeFromTop(scaled(24)));
            bounds.removeFromTop(scaled(2));
        }
        viewport_.setBounds(bounds);
        navigator_.setBounds(bounds.getCentreX() - VcsNavigatorBox::boxSize / 2, bounds.getY() + scaled(2),
            VcsNavigatorBox::boxSize, VcsNavigatorBox::boxSize);
        navigator_.toFront(false);
        layoutRows();
    }

private:
    // Arranging the surface.
    [[nodiscard]] std::vector<std::pair<std::string, juce::Rectangle<int>>> shownWidgetBounds() const
    {
        std::vector<std::pair<std::string, juce::Rectangle<int>>> shown;
        for (auto* row : rows_)
        {
            if (row->isVisible())
                shown.emplace_back(row->eventName(), row->getBounds());
        }
        for (std::size_t index = 0; index < decorationRects_.size(); ++index)
            shown.emplace_back(talas::app::vcsDecorationName(index), decorationRects_[index]);
        return shown;
    }

    void refreshSelectedValue()
    {
        selectedValueNames_.clear();
        selectedValue_.clear(juce::dontSendNotification);
        for (const auto& [name, bounds] : shownWidgetBounds())
        {
            selectedValueNames_.push_back(name);
            const auto index = talas::app::vcsDecorationIndex(name);
            selectedValue_.addItem(index.has_value() ? juceString("Text or box " + std::to_string(*index + 1)) : juceString(name),
                static_cast<int>(selectedValueNames_.size()));
        }
        selectedValue_.setText(juceString(selectedValueText()), juce::dontSendNotification);
        if (editorWindow_ != nullptr)
            editorWindow_->refresh();
    }

    // The VCS Editor's controls act on this surface.
    [[nodiscard]] std::unique_ptr<VcsEditorWindow> openEditorWindow()
    {
        juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(this);
        VcsEditorActions actions;
        actions.selectedValue = [safeThis] { return safeThis != nullptr ? juceString(safeThis->selectedValueText()) : juce::String{}; };
        actions.editSelected = [safeThis] {
            if (safeThis != nullptr && safeThis->selectedWidgets_.size() == 1)
                safeThis->editWidget(*safeThis->selectedWidgets_.begin());
        };
        actions.arrange = [safeThis](juce::Component& at) { if (safeThis != nullptr) safeThis->showArrangeMenu(&at); };
        actions.options = [safeThis](juce::Component& at) { if (safeThis != nullptr) safeThis->showOptionsMenu(&at); };
        actions.layout = [safeThis](juce::Component& at) { if (safeThis != nullptr) safeThis->showLayoutMenu(&at); };
        actions.lock = [safeThis] { if (safeThis != nullptr) (void) safeThis->setEditMode(false); };
        actions.moved = [safeThis](const talas::app::VcsRect& place) { if (safeThis != nullptr) safeThis->editorPlace_ = place; };
        return openVcsEditorWindow(*this, std::move(actions), editorPlace_);
    }

    [[nodiscard]] std::string selectedValueText() const
    {
        std::vector<std::string> names{selectedWidgets_.begin(), selectedWidgets_.end()};
        return talas::app::vcsSelectedValueText(names);
    }

    [[nodiscard]] static talas::app::VcsRect rectOf(const juce::Rectangle<int>& bounds) noexcept
    {
        return {bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight()};
    }

    // Every shown widget's place as it is on the screen, kept as an edit;
    // the layout becomes the one given.
    bool commitPlaces(talas::project::VirtualControlLayout layout,
        const std::vector<std::pair<std::string, talas::app::VcsRect>>& places, const juce::String& status)
    {
        std::vector<std::pair<std::string, std::optional<talas::project::VirtualControlBounds>>> edits;
        auto decorations = bridge_.editor().document().virtualControlDecorations;
        for (const auto& [name, rect] : places)
        {
            if (const auto index = talas::app::vcsDecorationIndex(name); index.has_value())
            {
                if (*index < decorations.size())
                    decorations[*index].bounds = rect;
                continue;
            }
            edits.emplace_back(name, rect);
        }
        // A layout but the top one keeps its widgets' places of its own.
        const auto own = ownLayout();
        if (const auto arranged = own.empty() ? bridge_.editor().arrangeVirtualControls(layout, edits)
                                              : bridge_.editor().setVirtualControlLayoutSettings(
                                                    talas::app::vcsLayoutSettingsPlaced(bridge_.editor().document(), own, layout, edits));
            !arranged.ok())
        {
            onStatus_(juceString(arranged.message));
            return false;
        }
        if (const auto placed = bridge_.editor().setVirtualControlDecorations(std::move(decorations)); !placed.ok())
        {
            onStatus_(juceString(placed.message));
            return false;
        }
        onDocumentChange_(false);
        refresh();
        onStatus_(status);
        return true;
    }

    [[nodiscard]] std::vector<std::pair<std::string, talas::app::VcsRect>> placesOnScreen() const
    {
        std::vector<std::pair<std::string, talas::app::VcsRect>> places;
        for (const auto& [name, bounds] : shownWidgetBounds())
            places.emplace_back(name, rectOf(bounds));
        return places;
    }

    // The padlock: unlocked, the surface is in Edit mode, its widgets
    // kept where they are from then on; locked again, it plays, asking
    // first whether to save the changes made to it.
    bool setEditMode(bool editing)
    {
        if (editing && talas::app::vcsLayoutArrangement(bridge_.editor().document(), ownLayout()) != talas::project::VirtualControlLayout::Fixed
            && !commitPlaces(talas::project::VirtualControlLayout::Fixed, placesOnScreen(), "Unlocked the surface"))
            return false;
        if (editing && !editMode_)
            unlockedSurface_ = talas::app::vcsSurface(bridge_.editor().document());
        if (!editing && editMode_ && unlockedSurface_.has_value()
            && talas::app::vcsSurfaceChanged(*unlockedSurface_, bridge_.editor().document()))
        {
            if (lockAnswer_.has_value())
                return lockSaving(*lockAnswer_);
            askToSaveChanges();
            return true;
        }
        return applyEditMode(editing);
    }

    // Save keeps the layout's changes, Discard takes them
    // back, each then showing the layout chosen, still unlocked; Cancel
    // stays on the layout shown.
    void askToSaveBeforeSwitching(std::size_t index)
    {
        layoutList_.setSelectedItemIndex(static_cast<int>(currentLayout_), juce::dontSendNotification);
        const auto options = juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Change the layout")
            .withMessage("Save the changes made to this layout?")
            .withButton("Save")
            .withButton("Discard")
            .withButton("Cancel")
            .withAssociatedComponent(this);
        juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(this);
        juce::NativeMessageBox::showAsync(options, [safeThis, index](int choice) {
            if (safeThis == nullptr || (choice != 0 && choice != 1))
                return;
            if (choice == 1 && safeThis->unlockedSurface_.has_value())
            {
                auto restored = talas::app::vcsSurfaceRestored(safeThis->bridge_.editor().document(), *safeThis->unlockedSurface_);
                if (const auto applied = safeThis->bridge_.editor().applyDocumentEdit(std::move(restored)); !applied.ok())
                    return;
                safeThis->onDocumentChange_(false);
                safeThis->refresh();
            }
            safeThis->unlockedSurface_ = talas::app::vcsSurface(safeThis->bridge_.editor().document());
            (void) safeThis->selectLayout(index);
        });
    }

    // Save, Discard, or Cancel to stay unlocked.
    void askToSaveChanges()
    {
        const auto options = juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Lock the Virtual Control Surface")
            .withMessage("Save the changes made to the surface since it was unlocked?")
            .withButton("Save")
            .withButton("Discard")
            .withButton("Cancel")
            .withAssociatedComponent(this);
        juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(this);
        juce::NativeMessageBox::showAsync(options, [safeThis](int choice) {
            if (safeThis != nullptr && (choice == 0 || choice == 1))
                (void) safeThis->lockSaving(choice == 0);
        });
    }

    bool lockSaving(bool save)
    {
        if (!save && unlockedSurface_.has_value())
        {
            auto restored = talas::app::vcsSurfaceRestored(bridge_.editor().document(), *unlockedSurface_);
            if (const auto applied = bridge_.editor().applyDocumentEdit(std::move(restored)); !applied.ok())
            {
                onStatus_("The surface's changes were not discarded: " + juceString(applied.message));
                return false;
            }
            onDocumentChange_(false);
            refresh();
        }
        return applyEditMode(false);
    }

    bool applyEditMode(bool editing)
    {
        if (!editing)
            unlockedSurface_.reset();
        editMode_ = editing;
        if (!editing)
            selectedWidgets_.clear();
        padlock_.setButtonText(editing ? "Lock" : "Unlock");
        // Unlocking opens the VCS Editor, locking closes it.
        if (!talas::app::vcsEditorWindowOpen(editing))
            editorWindow_.reset();
        else if (editorWindow_ == nullptr)
            editorWindow_ = openEditorWindow();
        layoutRows();
        refreshSelectedValue();
        resized();
        onStatus_(editing ? "Edit mode: drag widgets to move them, their corners to resize them" : "Locked the surface");
        return true;
    }

    bool moveSelection(int dx, int dy)
    {
        if (!editMode_ || selectedWidgets_.empty())
            return false;
        auto places = placesOnScreen();
        std::vector<talas::app::VcsRect> rects;
        std::set<std::size_t> chosen;
        for (std::size_t index = 0; index < places.size(); ++index)
        {
            rects.push_back(places[index].second);
            if (selectedWidgets_.contains(places[index].first))
                chosen.insert(index);
        }
        const auto moved = talas::app::vcsMoved(rects, chosen, dx, dy);
        for (std::size_t index = 0; index < places.size(); ++index)
            places[index].second = moved[index];
        return commitPlaces(talas::project::VirtualControlLayout::Fixed, places, "Moved the selected widgets");
    }

    bool resizeWidget(const std::string& name, talas::app::VcsCorner corner, int dx, int dy)
    {
        if (!editMode_)
            return false;
        auto places = placesOnScreen();
        const auto found = std::ranges::find(places, name, &std::pair<std::string, talas::app::VcsRect>::first);
        if (found == places.end())
            return false;
        // Dragging one of several selected widgets resizes
        // them all alike.
        std::vector<talas::app::VcsRect> rects;
        std::set<std::size_t> resized;
        for (std::size_t index = 0; index < places.size(); ++index)
        {
            rects.push_back(places[index].second);
            if (places[index].first == name || (selectedWidgets_.contains(name) && selectedWidgets_.contains(places[index].first)))
                resized.insert(index);
        }
        rects = talas::app::vcsResizedTogether(std::move(rects), resized, corner, dx, dy);
        for (std::size_t index = 0; index < places.size(); ++index)
            places[index].second = rects[index];
        return commitPlaces(talas::project::VirtualControlLayout::Fixed, places,
            juceString(resized.size() > 1 ? "Resized the selected widgets" : "Resized " + name));
    }

    // Clean up: the widgets in the columns asked for, or Fit,
    // their arrangement scaled with the window.
    bool cleanUp(unsigned columns, bool fit)
    {
        auto places = placesOnScreen();
        // The texts and boxes stay where they are.
        std::erase_if(places, [](const auto& place) { return talas::app::vcsDecorationIndex(place.first).has_value(); });
        if (places.empty())
            return false;
        // On a surface no larger than its window, Clean up
        // fits the widgets to the window as Fit does.
        std::vector<talas::app::VcsRect> before;
        for (const auto& place : places)
            before.push_back(place.second);
        const auto fitted = fit
            || talas::app::vcsCleanUpFitsWindow(talas::app::vcsExtent(before), viewport_.getMaximumVisibleWidth(),
                viewport_.getMaximumVisibleHeight());
        if (!fit)
        {
            // Each type of control on a
            // row of its own.
            const auto controls = talas::app::vcsLayoutControls(bridge_.editor().document(), ownLayout());
            std::vector<talas::app::VcsTypeRow> types;
            for (const auto& place : places)
            {
                const auto control = std::ranges::find(controls, place.first, &talas::project::VirtualControl::eventName);
                types.push_back(control == controls.end() ? talas::app::VcsTypeRow::Fader : talas::app::vcsTypeRow(*control));
            }
            const auto cells = talas::app::vcsCleanUpByType(types, columns, workbenchScaledUiPixels(192), workbenchScaledUiPixels(119));
            for (std::size_t index = 0; index < places.size(); ++index)
                places[index].second = cells[index];
        }
        return commitPlaces(fitted ? talas::project::VirtualControlLayout::Fit : talas::project::VirtualControlLayout::Fixed, places,
            fit ? "Fitted the surface to its window"
                : "Cleaned up the surface in " + juce::String(columns) + " columns" + (fitted ? ", fitted to its window" : ""));
    }

    // Arrange > Squeeze Vertical or Horizontal.
    bool squeeze(bool vertical)
    {
        if (!editMode_ || selectedWidgets_.size() < 2)
            return false;
        auto places = placesOnScreen();
        std::vector<talas::app::VcsRect> rects;
        std::set<std::size_t> chosen;
        for (std::size_t index = 0; index < places.size(); ++index)
        {
            rects.push_back(places[index].second);
            if (selectedWidgets_.contains(places[index].first))
                chosen.insert(index);
        }
        const auto squeezed = talas::app::vcsSqueezed(rects, chosen, vertical);
        for (std::size_t index = 0; index < places.size(); ++index)
            places[index].second = squeezed[index];
        return commitPlaces(talas::project::VirtualControlLayout::Fixed, places,
            vertical ? "Squeezed the selection vertically" : "Squeezed the selection horizontally");
    }

    bool alignSelection(talas::app::VcsAlignment alignment)
    {
        return rearrangeSelection([alignment](const auto& rects, const auto& chosen) {
            return talas::app::vcsAligned(rects, chosen, alignment);
        }, "Aligned the selected widgets");
    }

    bool distributeSelection(bool horizontal)
    {
        return rearrangeSelection([horizontal](const auto& rects, const auto& chosen) {
            return talas::app::vcsDistributed(rects, chosen, horizontal);
        }, "Distributed the selected widgets");
    }

    bool rearrangeSelection(
        const std::function<std::vector<talas::app::VcsRect>(const std::vector<talas::app::VcsRect>&, const std::set<std::size_t>&)>& arrange,
        const juce::String& status)
    {
        if (!editMode_ || selectedWidgets_.size() < 2)
            return false;
        auto places = placesOnScreen();
        std::vector<talas::app::VcsRect> rects;
        std::set<std::size_t> chosen;
        for (std::size_t index = 0; index < places.size(); ++index)
        {
            rects.push_back(places[index].second);
            if (selectedWidgets_.contains(places[index].first))
                chosen.insert(index);
        }
        const auto arranged = arrange(rects, chosen);
        for (std::size_t index = 0; index < places.size(); ++index)
            places[index].second = arranged[index];
        return commitPlaces(talas::project::VirtualControlLayout::Fixed, places, status);
    }

    // The layouts of a multilevel surface: the
    // list at the upper right, Shift with the arrows, and the Layout menu;
    // those of the Sound loaded alone, if one is.
    [[nodiscard]] std::vector<talas::app::VcsLayoutEntry> shownLayouts() const
    {
        return loaded_.has_value() ? talas::app::vcsLoadedSoundLayouts(bridge_.editor().document(), loaded_->first, loaded_->second)
                                   : talas::app::vcsLayouts(bridge_.editor().document());
    }

    // The layout shown, keeping its own presets and widgets; empty for the project's top layout.
    [[nodiscard]] std::string ownLayout() const
    {
        const auto layouts = shownLayouts();
        const auto top = (currentLayout_ == 0 && !loaded_.has_value()) || currentLayout_ >= layouts.size();
        return top ? std::string{} : layouts[currentLayout_].name;
    }

    void setLoadedSound(std::optional<talas::project::ProjectDocument> loaded, std::string sound) override
    {
        // Another Sound loaded shows its top layout; the same one keeps the layout shown.
        if (loaded_.has_value() != loaded.has_value() || (loaded_.has_value() && loaded_->second != sound))
            currentLayout_ = 0;
        loaded_ = loaded.has_value() ? std::optional{std::pair{std::move(*loaded), std::move(sound)}} : std::nullopt;
    }

    [[nodiscard]] bool rowShown(const VirtualControlRow& row) const
    {
        if (row.control().widget.tab != currentTab_)
            return false;
        const auto layouts = shownLayouts();
        if ((currentLayout_ == 0 && !loaded_.has_value()) || currentLayout_ >= layouts.size())
            return true;
        const auto& names = layouts[currentLayout_].eventNames;
        return std::ranges::find(names, row.control().eventName) != names.end();
    }

    void refreshLayoutList()
    {
        const auto layouts = shownLayouts();
        layoutList_.clear(juce::dontSendNotification);
        for (std::size_t index = 0; index < layouts.size(); ++index)
            layoutList_.addItem(juceString(talas::app::vcsLayoutLabel(layouts[index])), static_cast<int>(index) + 1);
        currentLayout_ = std::min(currentLayout_, layouts.empty() ? std::size_t{0} : layouts.size() - 1);
        layoutList_.setSelectedItemIndex(static_cast<int>(currentLayout_), juce::dontSendNotification);
    }

    bool selectLayout(std::size_t index)
    {
        const auto layouts = shownLayouts();
        if (index >= layouts.size())
            return false;
        currentLayout_ = index;
        layoutList_.setSelectedItemIndex(static_cast<int>(index), juce::dontSendNotification);
        refresh(); // the layout's own widgets and presets
        onStatus_(juceString("Layout " + layouts[index].name));
        return true;
    }

    // With the layouts' own settings following them, as one edit.
    bool setLayoutPages(std::vector<talas::project::VirtualControlLayoutPage> pages, const juce::String& status,
        std::optional<std::vector<talas::project::VirtualControlLayoutSettings>> settings = std::nullopt)
    {
        if (const auto set = settings.has_value() ? bridge_.editor().setVirtualControlLayoutSettings(std::move(*settings), std::move(pages))
                                                  : bridge_.editor().setVirtualControlLayoutPages(std::move(pages));
            !set.ok())
        {
            onStatus_(juceString(set.message));
            return false;
        }
        onDocumentChange_(false);
        refresh();
        onStatus_(status);
        return true;
    }

    // Duplicate: the current layout's controls as a layout of the
    // editor's own.
    bool duplicateLayout()
    {
        const auto layouts = shownLayouts();
        if (currentLayout_ >= layouts.size())
            return false;
        auto pages = bridge_.editor().document().virtualControlLayoutPages;
        pages.push_back(talas::app::vcsDuplicatedLayout(layouts[currentLayout_]));
        const auto index = layouts.size();
        auto settings = talas::app::vcsLayoutSettingsCopied(
            bridge_.editor().document().virtualControlLayoutSettings, ownLayout(), pages.back().name);
        if (!setLayoutPages(std::move(pages), "Duplicated the layout", std::move(settings)))
            return false;
        return selectLayout(index);
    }

    [[nodiscard]] std::optional<std::size_t> currentPageIndex() const
    {
        const auto layouts = shownLayouts();
        if (currentLayout_ >= layouts.size() || !layouts[currentLayout_].made)
            return std::nullopt;
        const auto made = std::ranges::count_if(layouts.begin(), layouts.begin() + static_cast<long>(currentLayout_),
            [](const talas::app::VcsLayoutEntry& entry) { return entry.made; });
        return static_cast<std::size_t>(made);
    }

    bool renameLayout(const std::string& name)
    {
        const auto page = currentPageIndex();
        if (!page.has_value() || name.empty())
            return false;
        auto pages = bridge_.editor().document().virtualControlLayoutPages;
        auto settings = talas::app::vcsLayoutSettingsRenamed(bridge_.editor().document().virtualControlLayoutSettings, pages[*page].name, name);
        pages[*page].name = name;
        return setLayoutPages(std::move(pages), "Renamed the layout", std::move(settings));
    }

    bool removeLayout()
    {
        const auto page = currentPageIndex();
        if (!page.has_value())
            return false;
        auto pages = bridge_.editor().document().virtualControlLayoutPages;
        auto settings = talas::app::vcsLayoutSettingsWithout(bridge_.editor().document().virtualControlLayoutSettings, pages[*page].name);
        pages.erase(pages.begin() + static_cast<long>(*page));
        currentLayout_ = 0;
        return setLayoutPages(std::move(pages), "Removed the layout", std::move(settings));
    }

    // Merge into new layout: the controls of the layouts chosen
    // with Shift+Click in the list.
    bool mergeChosenLayouts()
    {
        const auto layouts = shownLayouts();
        std::vector<talas::app::VcsLayoutEntry> chosen;
        for (const auto index : layoutsToMerge_)
        {
            if (index < layouts.size())
                chosen.push_back(layouts[index]);
        }
        if (chosen.size() < 2)
            return false;
        auto pages = bridge_.editor().document().virtualControlLayoutPages;
        pages.push_back(talas::app::vcsMergedLayout("Merged layout", chosen));
        layoutsToMerge_.clear();
        if (!setLayoutPages(std::move(pages), "Merged the layouts into a new one"))
            return false;
        return selectLayout(layouts.size());
    }

    // Merge into new layout: this layout's controls and another's.
    bool mergeLayoutWith(std::size_t other)
    {
        const auto layouts = shownLayouts();
        if (currentLayout_ >= layouts.size() || other >= layouts.size())
            return false;
        auto pages = bridge_.editor().document().virtualControlLayoutPages;
        pages.push_back(talas::app::vcsMergedLayout("Merged layout", {layouts[currentLayout_], layouts[other]}));
        const auto index = layouts.size();
        if (!setLayoutPages(std::move(pages), "Merged the layouts into a new one"))
            return false;
        return selectLayout(index);
    }

    // Remove selected widgets, from a layout of the editor's own.
    bool removeSelectedWidgetsFromLayout()
    {
        const auto page = currentPageIndex();
        if (!page.has_value() || selectedWidgets_.empty())
            return false;
        auto pages = bridge_.editor().document().virtualControlLayoutPages;
        pages[*page] = talas::app::vcsLayoutWithout(pages[*page], selectedWidgets_);
        selectedWidgets_.clear();
        return setLayoutPages(std::move(pages), "Removed the selected widgets from the layout");
    }

    void showLayoutMenu(juce::Component* at = nullptr)
    {
        juce::PopupMenu menu;
        const auto page = currentPageIndex();
        menu.addItem(1, "Duplicate");
        menu.addItem(2, "Rename...", page.has_value());
        menu.addItem(3, "Remove", page.has_value());
        juce::PopupMenu merge;
        const auto layouts = shownLayouts();
        for (std::size_t index = 0; index < layouts.size(); ++index)
        {
            if (index != currentLayout_)
                merge.addItem(100 + static_cast<int>(index), juceString(talas::app::vcsLayoutLabel(layouts[index])));
        }
        menu.addItem(5, "Merge into New Layout", layoutsToMerge_.size() > 1);
        menu.addSubMenu("Merge into New Layout With", merge, layouts.size() > 1);
        menu.addItem(4, "Remove Selected Widgets", page.has_value() && editMode_ && !selectedWidgets_.empty());
        juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(at != nullptr ? at : &layoutMenu_), [safeThis](int result) {
            if (safeThis == nullptr || result == 0)
                return;
            if (result >= 100)
                (void) safeThis->mergeLayoutWith(static_cast<std::size_t>(result - 100));
            else if (result == 1)
                (void) safeThis->duplicateLayout();
            else if (result == 3)
                (void) safeThis->removeLayout();
            else if (result == 4)
                (void) safeThis->removeSelectedWidgetsFromLayout();
            else if (result == 5)
                (void) safeThis->mergeChosenLayouts();
            else if (result == 2)
            {
                auto* window = new juce::AlertWindow("Rename Layout", {}, juce::MessageBoxIconType::NoIcon, safeThis.getComponent());
                window->addTextEditor("name", safeThis->layoutList_.getText().trimCharactersAtStart("."));
                window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
                window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
                window->enterModalState(true,
                    juce::ModalCallbackFunction::create([safeThis, window](int answer) {
                        if (safeThis != nullptr && answer == 1)
                            (void) safeThis->renameLayout(window->getTextEditorContents("name").trim().toStdString());
                    }),
                    true);
            }
        });
    }

    // The texts and boxes: Options > Add
    // Widget, the text typed for a Text, and those selected removed.
    [[nodiscard]] std::vector<std::size_t> selectedDecorations() const
    {
        std::vector<std::size_t> chosen;
        for (const auto& name : selectedWidgets_)
        {
            if (const auto index = talas::app::vcsDecorationIndex(name); index.has_value())
                chosen.push_back(*index);
        }
        return chosen;
    }

    bool setDecorations(std::vector<talas::project::VirtualControlDecoration> decorations, const juce::String& status)
    {
        if (const auto set = bridge_.editor().setVirtualControlDecorations(std::move(decorations)); !set.ok())
        {
            onStatus_(juceString(set.message));
            return false;
        }
        onDocumentChange_(false);
        refresh();
        onStatus_(status);
        return true;
    }

    bool addDecoration(talas::project::VirtualControlDecorationKind kind, std::string text)
    {
        if (!editMode_)
            return false;
        std::vector<talas::app::VcsRect> widgets;
        for (auto* row : rows_)
        {
            if (row->isVisible())
                widgets.push_back(rectOf(row->getBounds()));
        }
        auto decorations = bridge_.editor().document().virtualControlDecorations;
        decorations.push_back(talas::app::vcsAddedDecoration(kind, talas::app::vcsExtent(widgets), std::move(text)));
        selectedWidgets_ = {talas::app::vcsDecorationName(decorations.size() - 1)};
        return setDecorations(std::move(decorations),
            kind == talas::project::VirtualControlDecorationKind::Box ? "Added a box" : "Added a text");
    }

    // Text pasted into the unlocked surface: a text the size of the surface.
    // Ctrl+A: every widget shown selected.
    bool selectAllWidgets()
    {
        if (!editMode_)
            return false;
        std::set<std::string> all;
        for (const auto& [name, bounds] : shownWidgetBounds())
        {
            if (!talas::app::vcsDecorationIndex(name).has_value())
                all.insert(name);
        }
        if (all.empty())
            return false;
        editOverlay_.select(std::move(all));
        return true;
    }

    // Ctrl+C: the selected widgets, each with its type, its place
    // and size as shown, and its range.
    bool copySelectedWidgets(bool toSystemClipboard)
    {
        if (!editMode_)
            return false;
        const auto controls = talas::app::vcsLayoutControls(bridge_.editor().document(), ownLayout());
        std::vector<talas::project::VirtualControl> copied;
        for (const auto& [name, bounds] : shownWidgetBounds())
        {
            const auto control = std::ranges::find(controls, name, &talas::project::VirtualControl::eventName);
            if (!selectedWidgets_.contains(name) || control == controls.end())
                continue;
            auto widget = *control;
            widget.widget.bounds = rectOf(bounds);
            widget.midiMapping.reset();
            widget.oscMapping.reset();
            widget.oscEgressMapping.reset();
            copied.push_back(std::move(widget));
        }
        if (copied.empty())
            return false;
        onStatus_("Copied " + juce::String{static_cast<int>(copied.size())} + (copied.size() == 1 ? " widget" : " widgets"));
        copyVirtualControlWidgets(std::move(copied), toSystemClipboard);
        return true;
    }

    // Ctrl+V: the copied widgets onto the controls of their
    // Event Values and into the layout shown, if it was made here; those
    // with no control kept dormant.
    bool pasteWidgets(std::vector<talas::project::VirtualControl> widgets)
    {
        if (!editMode_ || widgets.empty())
            return false;
        const auto layouts = shownLayouts();
        const auto page = currentLayout_ < layouts.size() && layouts[currentLayout_].made ? layouts[currentLayout_].name : std::string{};
        const auto& controls = bridge_.editor().document().virtualControls;
        std::set<std::string> placed;
        for (const auto& widget : widgets)
        {
            if (std::ranges::find(controls, widget.eventName, &talas::project::VirtualControl::eventName) != controls.end())
                placed.insert(widget.eventName);
        }
        const auto dormant = widgets.size() - placed.size();
        if (const auto pasted = bridge_.editor().pasteVirtualControlWidgets(std::move(widgets), page); !pasted.ok())
        {
            onStatus_(juceString(pasted.message));
            return false;
        }
        selectedWidgets_ = std::move(placed);
        onDocumentChange_(false);
        refresh();
        onStatus_("Pasted " + juce::String{static_cast<int>(selectedWidgets_.size())} + " widgets"
            + (dormant == 0 ? juce::String{} : ", " + juce::String{static_cast<int>(dormant)} + " kept dormant"));
        return true;
    }

    bool pasteText(std::string text)
    {
        if (!editMode_ || text.empty())
            return false;
        auto decorations = bridge_.editor().document().virtualControlDecorations;
        decorations.push_back(talas::app::vcsPastedText(std::move(text), rectOf(controlsContent_.getLocalBounds())));
        selectedWidgets_ = {talas::app::vcsDecorationName(decorations.size() - 1)};
        return setDecorations(std::move(decorations), "Pasted the text into the surface");
    }

    bool editDecorationText(std::size_t index, std::string text)
    {
        auto decorations = talas::app::vcsDecorationRelabelled(bridge_.editor().document().virtualControlDecorations, index, std::move(text));
        return decorations.has_value() && setDecorations(std::move(*decorations), "Changed the text");
    }

    bool removeSelectedDecorations()
    {
        auto chosen = selectedDecorations();
        if (!editMode_ || chosen.empty())
            return false;
        auto decorations = bridge_.editor().document().virtualControlDecorations;
        std::ranges::sort(chosen, std::greater<>{});
        for (const auto index : chosen)
        {
            if (index < decorations.size())
                decorations.erase(decorations.begin() + static_cast<std::ptrdiff_t>(index));
        }
        std::erase_if(selectedWidgets_, [](const std::string& name) { return talas::app::vcsDecorationIndex(name).has_value(); });
        return setDecorations(std::move(decorations), "Removed the selected texts and boxes");
    }

    void askForText(std::optional<std::size_t> editing)
    {
        const auto& decorations = bridge_.editor().document().virtualControlDecorations;
        auto* window = new juce::AlertWindow(editing ? "Edit Text" : "Add Text", "The text the surface shows",
            juce::MessageBoxIconType::NoIcon, this);
        // The text is typed into its Widget Label.
        window->addTextEditor("text",
            editing && *editing < decorations.size() ? juceString(decorations[*editing].text) : juce::String{}, "Widget Label");
        window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(this);
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window, editing](int answer) {
                if (safeThis == nullptr || answer != 1)
                    return;
                auto text = window->getTextEditorContents("text").toStdString();
                if (editing)
                    (void) safeThis->editDecorationText(*editing, std::move(text));
                else
                    (void) safeThis->addDecoration(talas::project::VirtualControlDecorationKind::Text, std::move(text));
            }),
            true);
    }

    void showOptionsMenu(juce::Component* at = nullptr)
    {
        const auto chosen = selectedDecorations();
        const auto& decorations = bridge_.editor().document().virtualControlDecorations;
        const auto oneText = chosen.size() == 1 && chosen.front() < decorations.size()
            && decorations[chosen.front()].kind == talas::project::VirtualControlDecorationKind::Text;
        juce::PopupMenu menu;
        juce::PopupMenu add;
        add.addItem(1, "Text...", editMode_);
        add.addItem(2, "Box", editMode_);
        // Options... adds, hides and removes widgets.
        const auto& items = talas::app::vcsOptionsMenuItems();
        menu.addSubMenu(juceString(std::string{items[0]}), add);
        menu.addItem(3, juceString(std::string{items[1]}), editMode_ && oneText);
        menu.addItem(5, juceString(std::string{items[2]}), currentPageIndex().has_value() && editMode_ && !selectedWidgets_.empty());
        menu.addItem(4, juceString(std::string{items[3]}), editMode_ && !chosen.empty());
        juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(at != nullptr ? at : &options_), [safeThis, chosen](int result) {
            if (safeThis == nullptr || result == 0)
                return;
            if (result == 1)
                safeThis->askForText(std::nullopt);
            else if (result == 2)
                (void) safeThis->addDecoration(talas::project::VirtualControlDecorationKind::Box, {});
            else if (result == 3 && !chosen.empty())
                safeThis->askForText(chosen.front());
            else if (result == 4)
                (void) safeThis->removeSelectedDecorations();
            else if (result == 5)
                (void) safeThis->removeSelectedWidgetsFromLayout();
        });
    }

    void showArrangeMenu(juce::Component* at = nullptr)
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Clean Up...");
        menu.addItem(2, "Squeeze Vertical", editMode_ && selectedWidgets_.size() > 1);
        menu.addItem(3, "Squeeze Horizontal", editMode_ && selectedWidgets_.size() > 1);
        // Align and Distribute.
        juce::PopupMenu align;
        const std::array<const char*, 6> alignments{"Left Edges", "Right Edges", "Top Edges", "Bottom Edges",
            "Horizontal Centers", "Vertical Centers"};
        for (std::size_t index = 0; index < alignments.size(); ++index)
            align.addItem(10 + static_cast<int>(index), alignments[index], editMode_ && selectedWidgets_.size() > 1);
        menu.addSubMenu("Align", align);
        juce::PopupMenu distribute;
        distribute.addItem(20, "Horizontal (centers)", editMode_ && selectedWidgets_.size() > 2);
        distribute.addItem(21, "Vertical (centers)", editMode_ && selectedWidgets_.size() > 2);
        menu.addSubMenu("Distribute", distribute);
        juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(at != nullptr ? at : &arrange_), [safeThis](int result) {
            if (safeThis == nullptr || result == 0)
                return;
            if (result >= 10 && result < 16)
            {
                (void) safeThis->alignSelection(static_cast<talas::app::VcsAlignment>(result - 10));
                return;
            }
            if (result == 20 || result == 21)
            {
                (void) safeThis->distributeSelection(result == 20);
                return;
            }
            if (result != 1)
            {
                (void) safeThis->squeeze(result == 2);
                return;
            }
            safeThis->showCleanUpDialog();
        });
    }

    void showCleanUpDialog() override
    {
        juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(this);
        auto* window = new juce::AlertWindow("Clean Up", "How many columns? Fit scales the widgets with the window instead.",
            juce::MessageBoxIconType::NoIcon, this);
        window->addTextEditor("columns", "4", "Columns");
        window->addButton("Clean up", 1, juce::KeyPress{juce::KeyPress::returnKey});
        window->addButton("Fit", 2);
        window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
        window->enterModalState(true,
            juce::ModalCallbackFunction::create([safeThis, window](int answer) {
                if (safeThis == nullptr || answer == 0)
                    return;
                const auto columns = static_cast<unsigned>(std::max(1, window->getTextEditorContents("columns").getIntValue()));
                (void) safeThis->cleanUp(columns, answer == 2);
            }),
            true);
    }

    // The presets, kept as a snapshot bank of
    // the project's values; taking, choosing and rolling them needs the
    // project playing.
    [[nodiscard]] bool presetsPlayable()
    {
        if (bridge_.snapshotPerformanceDocumentCurrent())
            return true;
        onStatus_("Play the project before taking, choosing or rolling presets");
        return false;
    }

    void finishPresetEdit(const juce::String& status)
    {
        (void) bridge_.adoptSnapshotAuthoringEdit();
        onDocumentChange_(false);
        refresh();
        onStatus_(status);
    }

    // The layout's own list of presets, or the shared one until it has one.
    [[nodiscard]] const talas::project::ProjectSnapshotBank* presetBank() const
    {
        return talas::app::findVcsLayoutPresetBank(bridge_.editor().document(), ownLayout());
    }

    [[nodiscard]] std::string presetBankId() const { return talas::app::vcsLayoutPresetBankId(bridge_.editor().document(), ownLayout()); }

    bool ensurePresetBank()
    {
        // A layout but the top one takes presets of its own the first time.
        const auto own = ownLayout();
        if (presetBank() != nullptr && (own.empty() || presetBankId() != talas::app::vcsPresetBankId))
            return true;
        const auto current = bridge_.currentSnapshotScopeValues({});
        const auto values = current.ok() ? current.values : std::vector<talas::project::SnapshotValueEntry>{};
        auto made = own.empty() ? talas::app::vcsPresetBank(bridge_.editor().document(), registry_, values)
                                : talas::app::vcsLayoutPresetBank(bridge_.editor().document(), registry_, values, own,
                                      shownLayouts()[currentLayout_].eventNames);
        if (!made.ok())
        {
            onStatus_(juceString(made.diagnostic));
            return false;
        }
        const auto bankId = made.bank->bankId;
        auto added = bridge_.editor().addSnapshotBank(std::move(*made.bank));
        if (added.ok() && !own.empty())
            added = bridge_.editor().setVirtualControlLayoutSettings(
                talas::app::vcsLayoutSettingsWithPresets(bridge_.editor().document(), own, bankId));
        if (!added.ok())
        {
            onStatus_(juceString(added.message));
            return false;
        }
        (void) bridge_.adoptSnapshotAuthoringEdit();
        return true;
    }

    // The camera: the faders as a new preset with a suggested name,
    // or with Ctrl the selected preset replaced.
    bool takePreset(bool replaceSelected)
    {
        if (!presetsPlayable() || !ensurePresetBank())
            return false;
        const auto* bank = presetBank();
        const auto selected = presetList_.getSelectedItemIndex();
        if (replaceSelected && selected >= 0 && static_cast<std::size_t>(selected) < bank->snapshots.size())
        {
            const auto snapshotId = bank->snapshots[static_cast<std::size_t>(selected)].snapshotId;
            const auto captured = bridge_.captureSnapshotPerformance(presetBankId(), snapshotId);
            if (!captured.ok())
            {
                onStatus_(juceString(captured.message));
                return false;
            }
            finishPresetEdit("Replaced the preset with the current settings");
            return true;
        }
        const auto values = bridge_.currentSnapshotScopeValues(bank->scope);
        const auto presetId = talas::app::uniqueVcsPresetId(*bank);
        if (!values.ok() || presetId.empty())
        {
            onStatus_("The current settings cannot be kept as a preset");
            return false;
        }
        const auto name = talas::app::randomVcsPresetName(
            static_cast<std::uint64_t>(juce::Time::getHighResolutionTicks()) ^ ++diceRolls_);
        if (const auto stored = bridge_.editor().storeSnapshot(presetBankId(), {presetId, name, values.values});
            !stored.ok())
        {
            onStatus_(juceString(stored.message));
            return false;
        }
        finishPresetEdit(juceString("Took the preset " + name));
        presetList_.setSelectedItemIndex(presetList_.getNumItems() - 1, juce::dontSendNotification);
        return true;
    }

    bool selectPreset(int index)
    {
        const auto* bank = presetBank();
        if (bank == nullptr || index < 0 || static_cast<std::size_t>(index) >= bank->snapshots.size() || !presetsPlayable())
            return false;
        const auto recalled = bridge_.recallSnapshot(presetBankId(), bank->snapshots[static_cast<std::size_t>(index)].snapshotId);
        if (!recalled.ok())
        {
            onStatus_(juceString(recalled.message));
            return false;
        }
        presetList_.setSelectedItemIndex(index, juce::dontSendNotification);
        showPerformanceValues();
        onStatus_(juceString("Preset " + bank->snapshots[static_cast<std::size_t>(index)].displayName));
        return true;
    }

    // The dice: every control not frozen at random.
    bool rollDice()
    {
        if (!presetsPlayable() || !ensurePresetBank())
            return false;
        const auto seed = static_cast<std::uint64_t>(juce::Time::getHighResolutionTicks()) + ++diceRolls_;
        const auto rolled = bridge_.randomizeSnapshotBank(presetBankId(), seed);
        if (!rolled.ok())
        {
            onStatus_(juceString(rolled.message));
            return false;
        }
        refresh();
        onStatus_("Rolled the dice");
        return true;
    }

    bool toggleFrozen(std::string_view eventName)
    {
        const auto& controls = bridge_.editor().document().virtualControls;
        const auto control = std::ranges::find(controls, eventName, &talas::project::VirtualControl::eventName);
        if (control == controls.end() || !ensurePresetBank())
            return false;
        const auto frozen = !talas::app::vcsControlFrozen(*presetBank(), *control);
        const auto policy = talas::app::vcsFreezePolicy(*presetBank(), *control, frozen);
        if (const auto set = bridge_.editor().setSnapshotParameterPolicy(presetBankId(), policy); !set.ok())
        {
            onStatus_(juceString(set.message));
            return false;
        }
        finishPresetEdit(juceString(std::string{eventName}) + (frozen ? " is frozen against the dice" : " is active again"));
        return true;
    }

    // Enter takes a preset, r rolls the dice, the arrows choose the next
    // preset up or down.
    bool presetKey(const juce::KeyPress& key)
    {
        if (key.getModifiers().isShiftDown()
            && (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey))
            return selectLayout(talas::app::vcsAdjacentLayout(static_cast<std::size_t>(layoutList_.getNumItems()), currentLayout_,
                key.getKeyCode() == juce::KeyPress::upKey));
        if (key.getKeyCode() == juce::KeyPress::returnKey)
            return takePreset(key.getModifiers().isCtrlDown());
        if (key.getTextCharacter() == 'r' && !key.getModifiers().isAnyModifierKeyDown())
            return rollDice();
        if (key.getKeyCode() == juce::KeyPress::upKey || key.getKeyCode() == juce::KeyPress::downKey)
        {
            const auto* bank = presetBank();
            if (bank == nullptr)
                return false;
            const auto selected = presetList_.getSelectedItemIndex();
            const auto current = selected >= 0 && static_cast<std::size_t>(selected) < bank->snapshots.size()
                ? bank->snapshots[static_cast<std::size_t>(selected)].snapshotId
                : std::string{};
            const auto next = talas::app::vcsAdjacentPreset(*bank, current, key.getKeyCode() == juce::KeyPress::upKey ? -1 : 1);
            const auto found = next.has_value()
                ? std::ranges::find(bank->snapshots, *next, &talas::project::ProjectSnapshot::snapshotId)
                : bank->snapshots.end();
            return found != bank->snapshots.end() && selectPreset(static_cast<int>(found - bank->snapshots.begin()));
        }
        return false;
    }

    bool removePresetsBesidesDefault()
    {
        const auto* bank = presetBank();
        if (bank == nullptr)
            return false;
        for (const auto& snapshotId : talas::app::vcsPresetsBesidesDefault(*bank))
        {
            if (const auto removed = bridge_.editor().removeSnapshot(presetBankId(), snapshotId); !removed.ok())
            {
                onStatus_(juceString(removed.message));
                return false;
            }
        }
        finishPresetEdit("Removed every preset but Default");
        return true;
    }

    // The preset menu: rename or remove the selected preset, or
    // remove every one but Default.
    void showPresetMenu()
    {
        juce::PopupMenu menu;
        const auto* bank = presetBank();
        const auto selected = presetList_.getSelectedItemIndex();
        const auto has = bank != nullptr && selected >= 0 && static_cast<std::size_t>(selected) < bank->snapshots.size();
        menu.addItem(1, "Rename Preset...", has);
        menu.addItem(2, "Remove Preset", has && bank->snapshots[static_cast<std::size_t>(selected)].snapshotId != talas::app::vcsDefaultPresetId);
        menu.addItem(3, "Remove All but Default", bank != nullptr && bank->snapshots.size() > 1);
        juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options{}.withTargetComponent(&presetMenu_), [safeThis, selected](int result) {
            if (safeThis == nullptr || result == 0)
                return;
            const auto* current = safeThis->presetBank();
            if (current == nullptr)
                return;
            if (result == 3)
            {
                (void) safeThis->removePresetsBesidesDefault();
                return;
            }
            if (selected < 0 || static_cast<std::size_t>(selected) >= current->snapshots.size())
                return;
            const auto snapshot = current->snapshots[static_cast<std::size_t>(selected)];
            if (result == 2)
            {
                if (const auto removed = safeThis->bridge_.editor().removeSnapshot(safeThis->presetBankId(), snapshot.snapshotId);
                    !removed.ok())
                    safeThis->onStatus_(juceString(removed.message));
                else
                    safeThis->finishPresetEdit("Removed the preset");
                return;
            }
            auto* window = new juce::AlertWindow("Rename Preset", {}, juce::MessageBoxIconType::NoIcon, safeThis.getComponent());
            window->addTextEditor("name", juceString(snapshot.displayName));
            window->addButton("OK", 1, juce::KeyPress{juce::KeyPress::returnKey});
            window->addButton("Cancel", 0, juce::KeyPress{juce::KeyPress::escapeKey});
            window->enterModalState(true,
                juce::ModalCallbackFunction::create([safeThis, window, snapshotId = snapshot.snapshotId](int answer) {
                    if (safeThis == nullptr || answer != 1)
                        return;
                    const auto name = window->getTextEditorContents("name").trim().toStdString();
                    if (const auto renamed = safeThis->bridge_.editor().renameSnapshot(safeThis->presetBankId(), snapshotId, name);
                        !renamed.ok())
                        safeThis->onStatus_(juceString(renamed.message));
                    else
                        safeThis->finishPresetEdit("Renamed the preset");
                }),
                true);
        });
    }

    void refreshPresetList()
    {
        const juce::ScopedValueSetter<bool> refreshing{refreshingPresets_, true};
        const auto previous = presetList_.getSelectedItemIndex();
        presetList_.clear(juce::dontSendNotification);
        const auto* bank = presetBank();
        if (bank == nullptr)
        {
            presetList_.setTextWhenNothingSelected("No presets");
            return;
        }
        const auto labels = talas::app::vcsPresetLabels(*bank);
        for (std::size_t index = 0; index < labels.size(); ++index)
            presetList_.addItem(juceString(labels[index]), static_cast<int>(index) + 1);
        const auto& state = bridge_.performanceState();
        const auto active = state.activeBankId == presetBankId()
            ? std::ranges::find(bank->snapshots, state.activeSnapshotId, &talas::project::ProjectSnapshot::snapshotId)
            : bank->snapshots.end();
        presetList_.setSelectedItemIndex(active != bank->snapshots.end() ? static_cast<int>(active - bank->snapshots.begin())
                                                                         : std::min(previous, presetList_.getNumItems() - 1),
            juce::dontSendNotification);
    }

    // A preset chosen or the dice rolled show their values on the faders.
    void showPerformanceValues()
    {
        if (!bridge_.snapshotPerformanceDocumentCurrent() || bridge_.performanceState().activeBankId != presetBankId())
            return;
        for (const auto& [eventName, value] : talas::app::vcsControlValues(bridge_.performanceState().currentValues))
        {
            for (auto* row : rows_)
            {
                if (row->eventName() == eventName)
                    row->setExternalValue(value);
            }
        }
    }

    void noteFocusedEventValue(std::string_view eventName)
    {
        const auto focused = std::string(eventName);
        if (focusedEventName_ == focused)
            return;
        focusedEventName_ = focused;
        if (onFocus_)
            onFocus_(*focusedEventName_);
    }

    void handleAsyncUpdate() override
    {
        refresh();
    }

    void beginMidiLearn(std::string_view eventName)
    {
        finishMidiTransaction();
        midiLearnEventName_ = std::string(eventName);
        publishMidiLearnState(true);
        onStatus_("Move a MIDI controller to map " + juceString(eventName));
    }

    void clearMidiMapping(std::string_view eventName)
    {
        finishMidiTransaction();
        const auto result =
            bridge_.editor().setVirtualControlMidiMapping(
                eventName, std::nullopt);
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        if (midiLearnEventName_ == eventName)
        {
            midiLearnEventName_.reset();
            publishMidiLearnState(false);
        }
        onDocumentChange_(false);
        refresh();
        onStatus_("MIDI unmapped from " + juceString(eventName));
    }

    void setOscMapping(
        std::string_view eventName,
        std::optional<talas::project::OscControlMapping> mapping)
    {
        finishMidiTransaction();
        const auto mappedAddress = mapping.has_value()
            ? juceString(mapping->address)
            : juce::String{};
        const auto result = bridge_.editor().setVirtualControlOscMapping(
            eventName, std::move(mapping));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }
        onDocumentChange_(false);
        refresh();
        onStatus_(mappedAddress.isNotEmpty()
            ? (oscRouteAvailable_(
                   eventName, mappedAddress.toStdString())
                   ? "OSC " + mappedAddress + " mapped to "
                       + juceString(eventName)
                   : "OSC " + mappedAddress
                       + " saved; route unavailable until its exact Event Value stage is active")
            : "OSC unmapped from " + juceString(eventName));
    }

    void setOscEgressMapping(
        std::string_view eventName,
        std::optional<talas::project::OscEgressMapping> mapping)
    {
        finishMidiTransaction();
        const auto mappedAddress = mapping.has_value()
            ? juceString(mapping->address)
            : juce::String{};
        beforeOscEgressAuthorityChange_();
        const auto result =
            bridge_.editor().setVirtualControlOscEgressMapping(
                eventName, std::move(mapping));
        if (!result.ok())
        {
            restoreOscEgressAuthority_();
            onStatus_(juceString(result.message));
            return;
        }
        onDocumentChange_(false);
        refresh();
        onStatus_(mappedAddress.isNotEmpty()
            ? (oscEgressRouteAvailable_(
                   eventName, mappedAddress.toStdString())
                   ? "OSC feedback " + mappedAddress + " mapped from "
                       + juceString(eventName)
                   : "OSC feedback " + mappedAddress
                       + " saved; route unavailable until its exact Event Value stage is active")
            : "OSC feedback unmapped from " + juceString(eventName));
    }

    [[nodiscard]] bool midiLearnActive() const noexcept
    {
        return (midiUiStateToken_.load(std::memory_order_acquire)
                & 1U)
            != 0;
    }

    void publishMidiLearnState(bool active) noexcept
    {
        const auto current =
            midiUiStateToken_.load(std::memory_order_relaxed);
        const auto nextGeneration =
            (current & ~std::uint64_t{1}) + 2;
        midiUiStateToken_.store(
            nextGeneration | static_cast<std::uint64_t>(active),
            std::memory_order_release);
    }

    void beginMidiTransaction()
    {
        auto& editor = bridge_.editor();
        if (midiTransactionToken_.has_value()
            && editor.ownsHistoryTransaction(*midiTransactionToken_))
        {
            return;
        }
        // A value gesture already open, such as a slider drag, records the
        // burst too; otherwise the burst gets its own history step.
        if (editor.hasValueHistoryTransaction())
            return;
        midiTransactionToken_ = editor.beginValueHistoryTransaction();
    }

    void finishMidiTransaction()
    {
        if (!midiTransactionToken_.has_value())
            return;
        bridge_.editor().endOwnedHistoryTransaction(*midiTransactionToken_);
        midiTransactionToken_.reset();
        onValueGestureFinished_();
    }

    void beginValueGesture()
    {
        valueGestureToken_ = bridge_.editor().beginValueHistoryTransaction();
    }

    void finishValueGesture()
    {
        if (!valueGestureToken_.has_value())
            return;
        bridge_.editor().endOwnedHistoryTransaction(*valueGestureToken_);
        valueGestureToken_.reset();
        onValueGestureFinished_();
    }

    void addControl()
    {
        auto eventName = nameInput_.getText().trim().toStdString();
        if (eventName.empty())
            eventName = nextEventName();

        auto control = talas::project::makeDefaultVirtualControl(
            std::move(eventName),
            selectedDomain(),
            selectedKind());
        const auto result = bridge_.editor().addVirtualControl(std::move(control));
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }

        nameInput_.clear();
        onDocumentChange_(false);
        refresh();
        onStatus_(
            result.message.empty()
                ? juce::String{"Event Value added"}
                : juceString(result.message));
    }

    void removeControl(std::string_view eventName)
    {
        const auto control = std::ranges::find(
            bridge_.editor().document().virtualControls,
            eventName,
            &talas::project::VirtualControl::eventName);
        const auto revokedEgress = control
                != bridge_.editor().document().virtualControls.end()
            && control->oscEgressMapping.has_value();
        if (revokedEgress)
            beforeOscEgressAuthorityChange_();
        const auto result = bridge_.editor().removeVirtualControl(eventName);
        if (!result.ok())
        {
            if (revokedEgress)
                restoreOscEgressAuthority_();
            onStatus_(juceString(result.message));
            return;
        }

        if (midiLearnEventName_ == eventName)
        {
            midiLearnEventName_.reset();
            publishMidiLearnState(false);
        }

        onDocumentChange_(false);
        triggerAsyncUpdate();
        onStatus_("Event Value removed");
    }

    std::string nextEventName() const
    {
        const auto& controls = bridge_.editor().document().virtualControls;
        for (std::size_t number = 1;; ++number)
        {
            const auto candidate = "Event" + std::to_string(number);
            const auto found = std::ranges::find_if(controls, [&](const talas::project::VirtualControl& control) {
                return control.eventName == candidate;
            });
            if (found == controls.end())
                return candidate;
        }
    }

    talas::expr::ValueDomain selectedDomain() const
    {
        switch (domainInput_.getSelectedId())
        {
        case 2: return talas::expr::ValueDomain::Hertz;
        case 3: return talas::expr::ValueDomain::Seconds;
        case 4: return talas::expr::ValueDomain::NoteNumber;
        case 5: return talas::expr::ValueDomain::Samples;
        default: return talas::expr::ValueDomain::Scalar;
        }
    }

    talas::project::VirtualControlKind selectedKind() const
    {
        switch (kindInput_.getSelectedId())
        {
        case 2: return talas::project::VirtualControlKind::Gate;
        case 3: return talas::project::VirtualControlKind::Toggle;
        default: return talas::project::VirtualControlKind::Fader;
        }
    }

    // The surface's tabs (multilevel layouts): the widgets
    // of the chosen tab.
    void refreshTabs()
    {
        tabNames_ = talas::app::virtualControlTabs(talas::app::vcsLayoutControls(bridge_.editor().document(), ownLayout()));
        if (std::ranges::find(tabNames_, currentTab_) == tabNames_.end())
            currentTab_ = tabNames_.empty() ? std::string{} : tabNames_.front();
        tabs_.onTabChanged = nullptr;
        tabs_.clearTabs();
        for (const auto& name : tabNames_)
            tabs_.addTab(name.empty() ? juce::String{"Main"} : juceString(name), workbenchTheme.controlTile, -1);
        const auto current = std::ranges::find(tabNames_, currentTab_);
        if (current != tabNames_.end())
            tabs_.setCurrentTabIndex(static_cast<int>(current - tabNames_.begin()), false);
        tabs_.onTabChanged = [this](int index) {
            if (index >= 0 && index < static_cast<int>(tabNames_.size()))
            {
                currentTab_ = tabNames_[static_cast<std::size_t>(index)];
                layoutRows();
            }
        };
        const auto tabbed = tabNames_.size() > 1;
        if (tabs_.isVisible() != tabbed)
        {
            tabs_.setVisible(tabbed);
            resized();
        }
    }

    void layoutRows()
    {
        const auto rowWidth = workbenchScaledUiPixels(200);
        const auto rowHeight = workbenchScaledUiPixels(119);
        const auto gutter = workbenchScaledUiPixels(8);
        const auto availableWidth = viewport_.getWidth();
        const auto layout = talas::app::vcsLayoutArrangement(bridge_.editor().document(), ownLayout());
        decorationRects_.clear();
        if (layout != talas::project::VirtualControlLayout::Automatic)
        {
            layoutArrangedRows(layout, rowWidth, rowHeight, gutter);
            return;
        }
        // Each type of control on a row of its own, its
        // controls alphabetically from left to right; the Sound's own
        // controls after them.
        std::set<talas::app::VcsTypeRow> types;
        for (auto* row : rows_)
            if (rowShown(*row))
                types.insert(talas::app::vcsTypeRow(row->control()));
        std::map<talas::app::VcsTypeRow, int> across;
        int widest = localRows_.size();
        for (auto* row : rows_)
        {
            const auto shown = rowShown(*row);
            row->setVisible(shown);
            if (!shown)
                continue;
            const auto type = talas::app::vcsTypeRow(row->control());
            const auto line = static_cast<int>(std::distance(types.begin(), types.find(type)));
            row->setBounds(across[type]++ * rowWidth, line * rowHeight, rowWidth - gutter, rowHeight);
            widest = std::max(widest, across[type]);
        }
        const auto lines = static_cast<int>(types.size()) + (localRows_.isEmpty() ? 0 : 1);
        controlsContent_.setSize(std::max(availableWidth, widest * rowWidth), std::max(1, lines) * rowHeight);
        for (int index = 0; index < localRows_.size(); ++index)
            localRows_[index]->setBounds(index * rowWidth, static_cast<int>(types.size()) * rowHeight, rowWidth - gutter, rowHeight);
        placeEditOverlay();
    }

    // An arranged surface: each widget at its
    // place, one without a place after the rest; Fit scales them all into
    // the window, Fixed keeps them, the window showing part of a larger
    // surface.
    void layoutArrangedRows(talas::project::VirtualControlLayout layout, int rowWidth, int rowHeight, int gutter)
    {
        std::vector<VirtualControlRow*> shown;
        std::vector<talas::app::VcsRect> rects;
        for (auto* row : rows_)
        {
            const auto visible = rowShown(*row);
            row->setVisible(visible);
            if (!visible)
                continue;
            shown.push_back(row);
            rects.push_back(row->control().widget.bounds.value_or(talas::app::VcsRect{}));
        }
        auto extent = talas::app::vcsExtent(rects);
        for (auto& rect : rects)
        {
            if (rect.width > 0)
                continue;
            rect = {extent.width + gutter, 0, rowWidth - gutter, rowHeight};
            extent = talas::app::vcsExtent(rects);
        }
        for (int index = 0; index < localRows_.size(); ++index)
            localRows_[index]->setBounds(extent.width + gutter + index * rowWidth, 0, rowWidth - gutter, rowHeight);
        // The texts and boxes lie among the widgets, fitted with them.
        for (const auto& decoration : bridge_.editor().document().virtualControlDecorations)
            rects.push_back(decoration.bounds);
        if (layout == talas::project::VirtualControlLayout::Fit && !editMode_)
            rects = talas::app::vcsFit(rects, std::max(1, viewport_.getMaximumVisibleWidth()), std::max(1, viewport_.getMaximumVisibleHeight()));
        for (std::size_t index = 0; index < shown.size(); ++index)
            shown[index]->setBounds(rects[index].left, rects[index].top, rects[index].width, rects[index].height);
        for (std::size_t index = shown.size(); index < rects.size(); ++index)
            decorationRects_.emplace_back(rects[index].left, rects[index].top, rects[index].width, rects[index].height);
        const auto fitted = talas::app::vcsExtent(rects);
        controlsContent_.setSize(std::max(viewport_.getMaximumVisibleWidth(), fitted.width + static_cast<int>(localRows_.size()) * rowWidth),
            std::max(rowHeight, fitted.height));
        placeEditOverlay();
    }

    void placeEditOverlay()
    {
        const auto& decorations = bridge_.editor().document().virtualControlDecorations;
        decorationLayer_.shown.clear();
        for (std::size_t index = 0; index < decorationRects_.size() && index < decorations.size(); ++index)
            decorationLayer_.shown.emplace_back(decorations[index], decorationRects_[index]);
        decorationLayer_.setBounds(controlsContent_.getLocalBounds());
        decorationLayer_.toBack();
        decorationLayer_.repaint();
        if (editMode_)
            refreshSelectedValue();
        editOverlay_.setVisible(editMode_);
        editOverlay_.setBounds(controlsContent_.getLocalBounds());
        editOverlay_.toFront(false);
        editOverlay_.repaint();
    }

    // Applies the VCS Editor's options: Min / Max first, then the rest.
    bool applyWidget(std::string_view eventName, const talas::project::VirtualControlWidget& widget, double minimum,
        double maximum)
    {
        const auto control = std::ranges::find(
            bridge_.editor().document().virtualControls, eventName, &talas::project::VirtualControl::eventName);
        if (control == bridge_.editor().document().virtualControls.end())
            return false;
        // On a layout but the top one, the widget and its range are the layout's own.
        const auto own = ownLayout();
        const auto set = talas::app::vcsLayoutSettingsWidgetSet(bridge_.editor().document(), own, eventName, widget, minimum, maximum);
        if (!own.empty() && (!set.has_value() || !bridge_.editor().setVirtualControlLayoutSettings(*set).ok()))
        {
            onStatus_("A layout's widget needs a range within the Event Value's own that suits it");
            refresh();
            return false;
        }
        if (own.empty() && (control->minimum != minimum || control->maximum != maximum))
        {
            // A grid wider than the new range would refuse it; keep it within.
            auto narrowed = control->widget;
            narrowed.grid = std::min(narrowed.grid, std::max(0.0, maximum - minimum));
            narrowed.taper = minimum > 0.0 ? narrowed.taper : talas::project::VirtualControlTaper::Linear;
            if (narrowed != control->widget)
                (void)bridge_.editor().setVirtualControlWidget(eventName, narrowed);
            if (const auto ranged = bridge_.editor().setVirtualControlRange(eventName, minimum, maximum); !ranged.ok())
            {
                onStatus_(juceString(ranged.message));
                return false;
            }
        }
        if (const auto shared = own.empty() ? bridge_.editor().setVirtualControlWidget(eventName, widget) : talas::project::EditResult{};
            !shared.ok())
        {
            onStatus_(juceString(shared.message));
            refresh();
            return false;
        }
        onDocumentChange_(false);
        refresh();
        onStatus_("Widget options set for " + juceString(eventName));
        return true;
    }

    // The VCS Editor's fields for one widget.
    void editWidget(std::string_view eventName)
    {
        const auto controls = talas::app::vcsLayoutControls(bridge_.editor().document(), ownLayout());
        const auto control = std::ranges::find(controls, eventName, &talas::project::VirtualControl::eventName);
        if (control == controls.end() || std::exchange(widgetDialogOpen_, true))
            return;
        juce::Component::SafePointer<VirtualControlSurfaceImpl> safeThis(this);
        showVcsWidgetDialog(*this, *control,
            [safeThis](const std::string& name, talas::project::VirtualControlWidget widget, double minimum, double maximum) {
                if (safeThis != nullptr)
                    (void) safeThis->applyWidget(name, widget, minimum, maximum);
            },
            [safeThis](const juce::String& status) { if (safeThis != nullptr) safeThis->onStatus_(status); },
            [safeThis] { if (safeThis != nullptr) safeThis->widgetDialogOpen_ = false; });
    }

    talas::runtime::ProjectRuntimeBridge& bridge_;
    const talas::core::NodeRegistry& registry_;
    std::atomic<std::uint64_t>& midiUiStateToken_;
    MidiSourceNameCallback midiSourceName_;
    OscRouteAvailableCallback oscRouteAvailable_;
    OscEgressRouteAvailableCallback oscEgressRouteAvailable_;
    OscProfileDetailCallback oscProfileDetail_;
    ValueDispatchCallback dispatchValue_;
    LocalMapValueDispatchCallback dispatchLocalMapValue_;
    LocalMapStageReadyCallback localMapStageReady_;
    ValueGestureFinishedCallback onValueGestureFinished_;
    AcceptedValueCallback onAcceptedValue_;
    OscEgressAuthorityCallback beforeOscEgressAuthorityChange_;
    OscEgressAuthorityCallback restoreOscEgressAuthority_;
    DocumentChangeCallback onDocumentChange_;
    StatusCallback onStatus_;
    FocusCallback onFocus_;
    juce::TextEditor nameInput_;
    juce::ComboBox domainInput_;
    juce::ComboBox kindInput_;
    juce::TextButton addButton_;
    juce::Viewport viewport_;
    VcsNavigatorBox navigator_{viewport_};
    juce::Component controlsContent_;
    // The Annotations' texts, a row above the widgets.
    juce::Label annotations_;
    // The hand while the Control key is down, and the view where its drag
    // began.
    bool hand_ = false;
    juce::Point<int> handStart_;
    juce::OwnedArray<VirtualControlRow> rows_;
    juce::OwnedArray<LocalMapControlRow> localRows_;
    CallbackTabBar tabs_;
    std::vector<std::string> tabNames_;
    std::string currentTab_;
    std::uint64_t localStageGeneration_ = 0;
    std::optional<std::string> focusedEventName_;
    std::optional<std::string> midiLearnEventName_;
    std::uint32_t midiLastUpdateMs_ = 0;
    std::optional<talas::project::ProjectEditor::HistoryTransactionToken>
        valueGestureToken_;
    std::optional<talas::project::ProjectEditor::HistoryTransactionToken>
        midiTransactionToken_;
    bool sendingRowValue_ = false;
    bool refreshDeferred_ = false;
    juce::TextButton camera_{"Camera"};
    juce::ComboBox presetList_;
    juce::TextButton dice_{"Dice"};
    juce::TextButton presetMenu_{"..."};
    juce::TextButton padlock_{"Unlock"};
    // The fader whose number Tab selected last.
    std::string typedNumber_;
    juce::TextButton arrange_{"Arrange"};
    juce::TextButton options_{"Options"};
    juce::ComboBox selectedValue_;
    std::vector<std::string> selectedValueNames_;
    VcsDecorationLayer decorationLayer_;
    std::vector<juce::Rectangle<int>> decorationRects_;
    juce::ComboBox layoutList_;
    juce::TextButton layoutMenu_{"Layout"};
    std::size_t currentLayout_ = 0;
    // The layouts Shift+Clicked in the list, to merge.
    std::set<std::size_t> layoutsToMerge_;
    VcsEditOverlay editOverlay_;
    bool editMode_ = false;
    std::optional<talas::app::VcsSurface> unlockedSurface_;
    std::optional<bool> lockAnswer_;
    // The Sound loaded alone: its structure and node.
    std::optional<std::pair<talas::project::ProjectDocument, std::string>> loaded_;
    VcsDisplayStrip displays_{bridge_};
    // Where the VCS Editor was put, and the editor while the surface is unlocked.
    std::optional<talas::app::VcsRect> editorPlace_;
    std::unique_ptr<VcsEditorWindow> editorWindow_;
    bool widgetDialogOpen_ = false;
    std::set<std::string> selectedWidgets_;
    bool refreshingPresets_ = false;
    std::uint64_t diceRolls_ = 0;
};

}

std::unique_ptr<VirtualControlSurface>
makeVirtualControlSurface(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    std::atomic<std::uint64_t>& midiUiStateToken,
    VirtualControlSurface::MidiSourceNameCallback midiSourceName,
    VirtualControlSurface::OscRouteAvailableCallback oscRouteAvailable,
    VirtualControlSurface::OscEgressRouteAvailableCallback
        oscEgressRouteAvailable,
    VirtualControlSurface::OscProfileDetailCallback oscProfileDetail,
    VirtualControlSurface::ValueDispatchCallback dispatchValue,
    VirtualControlSurface::LocalMapValueDispatchCallback dispatchLocalMapValue,
    VirtualControlSurface::LocalMapStageReadyCallback localMapStageReady,
    VirtualControlSurface::ValueGestureFinishedCallback
        onValueGestureFinished,
    VirtualControlSurface::AcceptedValueCallback onAcceptedValue,
    VirtualControlSurface::OscEgressAuthorityCallback
        beforeOscEgressAuthorityChange,
    VirtualControlSurface::OscEgressAuthorityCallback
        restoreOscEgressAuthority,
    VirtualControlSurface::DocumentChangeCallback onDocumentChange,
    VirtualControlSurface::StatusCallback onStatus,
    VirtualControlSurface::FocusCallback onFocus)
{
    return std::make_unique<VirtualControlSurfaceImpl>(
        bridge,
        registry,
        midiUiStateToken,
        std::move(midiSourceName),
        std::move(oscRouteAvailable),
        std::move(oscEgressRouteAvailable),
        std::move(oscProfileDetail),
        std::move(dispatchValue),
        std::move(dispatchLocalMapValue),
        std::move(localMapStageReady),
        std::move(onValueGestureFinished),
        std::move(onAcceptedValue),
        std::move(beforeOscEgressAuthorityChange),
        std::move(restoreOscEgressAuthority),
        std::move(onDocumentChange),
        std::move(onStatus),
        std::move(onFocus));
}
}
