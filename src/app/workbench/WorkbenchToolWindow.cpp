#include "app/workbench/WorkbenchToolWindow.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"

#include <algorithm>
#include <charconv>
#include <cmath>

// The Tool window: the widgets of a
// running Tool.
namespace talas::app::workbench
{
namespace
{
namespace tool = talas::project::tool;
namespace st = talas::project::smalltalk;

// A value as a widget shows it, a number to the digits asked for.
[[nodiscard]] juce::String valueText(const st::Value& value, std::optional<int> digits)
{
    switch (value.kind)
    {
    case st::Value::Kind::Number:
        return digits.has_value() ? juce::String{value.number, *digits} : juce::String{value.number};
    case st::Value::Kind::Boolean:
        return value.boolean ? "true" : "false";
    case st::Value::Kind::String:
    case st::Value::Kind::Symbol:
    case st::Value::Kind::Expression:
        return juceString(value.text);
    case st::Value::Kind::Array:
    {
        juce::StringArray parts;
        if (value.elements != nullptr)
        {
            for (const auto& element : *value.elements)
                parts.add(valueText(element, digits));
        }
        return "(" + parts.joinIntoString(" ") + ")";
    }
    case st::Value::Kind::Nil:
        return "nil";
    default:
        return "an object";
    }
}

[[nodiscard]] st::Value typedValue(const juce::String& text)
{
    const auto trimmed = text.trim().toStdString();
    double number = 0.0;
    const auto [end, failed] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), number);
    if (!trimmed.empty() && failed == std::errc{} && end == trimmed.data() + trimmed.size())
        return st::Value::of(number);
    return st::Value::string(text.toStdString());
}

class ToolRunPanelImpl;

// A button's mouse, as the four buttons take it.
class ToolButton final : public juce::Component
{
public:
    ToolButton(std::string name, std::function<void(tool::ButtonEvent)> pressed)
        : name_(std::move(name))
        , pressed_(std::move(pressed))
    {
    }

    bool down = false;

    void paint(juce::Graphics& graphics) override
    {
        graphics.setColour(down ? selectionColour : workbenchTheme.controlTile);
        graphics.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1.0F), 4.0F);
        graphics.setColour(textColour);
        graphics.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0F), 4.0F, 1.0F);
        graphics.setFont(WorkbenchType::label());
        graphics.drawFittedText(juceString(name_), getLocalBounds().reduced(4), juce::Justification::centred, 1);
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        inside_ = true;
        pressed_(tool::ButtonEvent::Press);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (inside_ && !getLocalBounds().contains(event.getPosition()))
        {
            inside_ = false;
            pressed_(tool::ButtonEvent::Leave);
        }
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (inside_ && getLocalBounds().contains(event.getPosition()))
            pressed_(tool::ButtonEvent::Release);
        inside_ = false;
    }

private:
    std::string name_;
    std::function<void(tool::ButtonEvent)> pressed_;
    bool inside_ = false;
};

// A Meter: the value's magnitude within its range, not to be changed.
class ToolMeter final : public juce::Component
{
public:
    double fraction = 0.0;

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(workbenchTheme.controlTile);
        graphics.setColour(selectionColour);
        const auto area = getLocalBounds().toFloat().reduced(2.0F);
        if (getHeight() > getWidth())
            graphics.fillRect(area.withTrimmedTop(area.getHeight() * static_cast<float>(1.0 - fraction)));
        else
            graphics.fillRect(area.withWidth(area.getWidth() * static_cast<float>(fraction)));
        graphics.setColour(panelBorderColour);
        graphics.drawRect(getLocalBounds());
    }
};

// A View: what the Tool's responses drew in it, and where
// the cursor lies in it.
class ToolView final : public juce::Component
{
public:
    std::function<std::vector<tool::Drawing>()> drawings;
    std::function<void(double, double)> cursor;
    juce::Point<int> origin;

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(juce::Colours::white);
        if (!drawings)
            return;
        // The drawings are in the window's pixels.
        graphics.addTransform(juce::AffineTransform::translation(static_cast<float>(-origin.x), static_cast<float>(-origin.y)));
        for (const auto& drawing : drawings())
        {
            graphics.setColour(juce::Colour::fromFloatRGBA(static_cast<float>(drawing.colour[0]), static_cast<float>(drawing.colour[1]),
                static_cast<float>(drawing.colour[2]), 1.0F));
            const auto point = [&](std::size_t index) {
                return index < drawing.points.size()
                    ? juce::Point<float>{static_cast<float>(drawing.points[index].first + drawing.offset.first),
                          static_cast<float>(drawing.points[index].second + drawing.offset.second)}
                    : juce::Point<float>{};
            };
            const auto width = static_cast<float>(drawing.lineWidth);
            switch (drawing.shape)
            {
            case tool::Drawing::Shape::Line:
                graphics.drawLine({point(0), point(1)}, width);
                break;
            case tool::Drawing::Shape::String:
                graphics.setFont(WorkbenchType::label());
                graphics.drawSingleLineText(juceString(drawing.text), static_cast<int>(point(0).x), static_cast<int>(point(0).y));
                break;
            case tool::Drawing::Shape::Rectangle:
                graphics.fillRect(juce::Rectangle<float>{point(0), point(1)});
                break;
            case tool::Drawing::Shape::Outline:
                graphics.drawRect(juce::Rectangle<float>{point(0), point(1)}, width);
                break;
            case tool::Drawing::Shape::Dot:
            {
                const auto radius = static_cast<float>(drawing.diameter) / 2.0F;
                graphics.fillEllipse(point(0).x - radius, point(0).y - radius, 2.0F * radius, 2.0F * radius);
                break;
            }
            case tool::Drawing::Shape::Arc:
            {
                juce::Path arc;
                const auto box = juce::Rectangle<float>{point(0), point(1)};
                // Degrees from hard right, clockwise, as JUCE's from twelve o'clock.
                const auto start = juce::degreesToRadians(static_cast<float>(drawing.startAngle) + 90.0F);
                arc.addCentredArc(box.getCentreX(), box.getCentreY(), box.getWidth() / 2.0F, box.getHeight() / 2.0F, 0.0F, start,
                    start + juce::degreesToRadians(static_cast<float>(drawing.sweepAngle)), true);
                graphics.strokePath(arc, juce::PathStrokeType{width});
                break;
            }
            case tool::Drawing::Shape::Polygon:
            case tool::Drawing::Shape::Polyline:
            {
                juce::Path path;
                for (std::size_t index = 0; index < drawing.points.size(); ++index)
                    index == 0 ? path.startNewSubPath(point(index)) : path.lineTo(point(index));
                if (drawing.shape == tool::Drawing::Shape::Polygon)
                {
                    path.closeSubPath();
                    graphics.fillPath(path);
                }
                else
                    graphics.strokePath(path, juce::PathStrokeType{width});
                break;
            }
            case tool::Drawing::Shape::Invalidate:
                break;
            }
        }
    }

    void mouseMove(const juce::MouseEvent& event) override
    {
        if (cursor)
            cursor(event.position.x + static_cast<float>(origin.x), event.position.y + static_cast<float>(origin.y));
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        mouseMove(event);
    }
};

class ToolRunPanelImpl final : public ToolRunPanel, private juce::Timer
{
public:
    explicit ToolRunPanelImpl(ToolRunHost host)
        : host_(std::move(host))
    {
        debug_.setMultiLine(true);
        debug_.setReadOnly(true);
        debug_.setFont(WorkbenchType::data());
        addChildComponent(debug_);
        setSize(480, 360);
    }

    ~ToolRunPanelImpl() override
    {
        stopTimer();
    }

    bool run(tool::Definition definition) override
    {
        stopTimer();
        definition_ = std::move(definition);
        machine_ = std::make_unique<tool::Machine>(definition_);
        if (host_.displayableAudioChannels)
            machine_->setPlayback({0.0, 0.0, false, host_.displayableAudioChannels()});
        if (host_.sampleFile)
            machine_->setSampleFiles(host_.sampleFile);
        // A View takes its rectangle in the window.
        const auto placeViews = [&](const std::vector<tool::Variable>& variables) {
            for (const auto& variable : variables)
            {
                if (variable.widget.kind == tool::WidgetKind::View || variable.kind == tool::Kind::View)
                    machine_->setViewBounds(variable.name, variable.widget.left, variable.widget.top, variable.widget.width,
                        variable.widget.height);
            }
        };
        placeViews(definition_.variables);
        for (const auto& state : definition_.states)
            placeViews(state.variables);
        writes_ = 0;
        requests_ = 0;
        if (!machine_->start())
        {
            host_.status("The Tool did not start: " + juceString(machine_->error()));
            return false;
        }
        if (const auto extent = machine_->openingExtent())
            setSize(static_cast<int>(extent->first), static_cast<int>(extent->second));
        fixedSize_ = machine_->fixedExtent();
        shownState_.clear();
        afterChange();
        last_ = juce::Time::getMillisecondCounterHiRes();
        startTimer(20);
        host_.status("Running " + title());
        return true;
    }

    [[nodiscard]] juce::String title() const override
    {
        // WindowTitle, if the Tool defines it.
        if (machine_ != nullptr)
        {
            if (const auto named = machine_->variable("WindowTitle");
                named.has_value() && (named->kind == st::Value::Kind::String || named->kind == st::Value::Kind::Symbol))
                return juceString(named->text);
        }
        return "Tool";
    }

    [[nodiscard]] bool fixedSize() const override
    {
        return fixedSize_;
    }

    [[nodiscard]] std::vector<std::pair<std::string, tool::WidgetKind>> widgetsForSmoke() const override
    {
        std::vector<std::pair<std::string, tool::WidgetKind>> shown;
        for (const auto& widget : widgets_)
            shown.emplace_back(widget.name, widget.kind);
        return shown;
    }

    bool pressForSmoke(const std::string& variable, tool::ButtonEvent event) override
    {
        return press(variable, event);
    }

    bool setForSmoke(const std::string& variable, const juce::String& value) override
    {
        return set(variable, typedValue(value));
    }

    [[nodiscard]] juce::String shownForSmoke(const std::string& variable) const override
    {
        const auto found = std::ranges::find(widgets_, variable, &Widget::name);
        if (found == widgets_.end())
            return {};
        if (auto* label = dynamic_cast<juce::Label*>(found->component.get()))
            return label->getText();
        if (auto* editor = dynamic_cast<juce::TextEditor*>(found->component.get()))
            return editor->getText();
        if (auto* slider = dynamic_cast<juce::Slider*>(found->component.get()))
            return juce::String{slider->getValue()};
        if (auto* meter = dynamic_cast<ToolMeter*>(found->component.get()))
            return juce::String{meter->fraction};
        if (auto* button = dynamic_cast<ToolButton*>(found->component.get()))
            return button->down ? "down" : "up";
        for (const auto& radio : found->radios)
        {
            if (radio->getToggleState())
                return radio->getButtonText();
        }
        return {};
    }

    [[nodiscard]] std::string stateForSmoke() const override
    {
        return machine_ == nullptr ? std::string{} : std::string{machine_->state()};
    }

    void advanceForSmoke(double milliseconds) override
    {
        advance(milliseconds);
    }

    [[nodiscard]] std::vector<std::string> debugLinesForSmoke() const override
    {
        std::vector<std::string> lines;
        if (!debug_.isVisible())
            return lines;
        for (const auto& line : juce::StringArray::fromLines(debug_.getText()))
            lines.push_back(line.toStdString());
        return lines;
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(background());
    }

    void resized() override
    {
        debug_.setBounds(getLocalBounds().removeFromBottom(std::min(120, getHeight() / 3)));
    }

private:
    struct Widget
    {
        std::string name;
        tool::WidgetKind kind = tool::WidgetKind::None;
        std::optional<int> digits;
        std::optional<double> minimum;
        std::optional<double> maximum;
        std::unique_ptr<juce::Component> component;
        std::unique_ptr<juce::Label> label;
        std::vector<std::unique_ptr<juce::ToggleButton>> radios;
    };

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        advance(now - last_);
        last_ = now;
    }

    void advance(double milliseconds)
    {
        if (machine_ == nullptr || machine_->stopped())
            return;
        if (!machine_->advance(milliseconds))
            host_.status("The Tool stopped: " + juceString(machine_->error()));
        afterChange();
    }

    bool press(const std::string& variable, tool::ButtonEvent event)
    {
        if (machine_ == nullptr)
            return false;
        const auto pressed = machine_->press(variable, event);
        if (!pressed)
            host_.status(juceString(machine_->error()));
        for (auto& widget : widgets_)
        {
            if (widget.name == variable)
            {
                if (auto* button = dynamic_cast<ToolButton*>(widget.component.get()))
                {
                    // A stick stays down; the others are down while held.
                    button->down = widget.kind == tool::WidgetKind::StickButton || event == tool::ButtonEvent::Press;
                    button->repaint();
                }
            }
        }
        afterChange();
        return pressed;
    }

    bool set(const std::string& variable, const st::Value& value)
    {
        if (machine_ == nullptr)
            return false;
        const auto done = machine_->set(variable, value);
        if (!done)
            host_.status(juceString(machine_->error()));
        afterChange();
        return done;
    }

    [[nodiscard]] juce::Colour background() const
    {
        // BackgroundColor, a colour's name or transparent.
        if (machine_ != nullptr)
        {
            if (const auto colour = machine_->variable("BackgroundColor");
                colour.has_value() && (colour->kind == st::Value::Kind::String || colour->kind == st::Value::Kind::Symbol))
            {
                if (colour->text == "transparent" || colour->text == "Transparent")
                    return juce::Colours::transparentBlack;
                if (const auto named = tool::colourNamed(colour->text))
                    return juce::Colour{*named};
                return juce::Colours::findColourForName(juceString(colour->text), panelColour);
            }
        }
        return panelColour;
    }

    // The Event Values written and the Sounds asked for since last time,
    // the widgets the state shows, and their values.
    void afterChange()
    {
        if (machine_ == nullptr)
            return;
        const auto& writes = machine_->eventWrites();
        for (; writes_ < writes.size(); ++writes_)
            host_.writeEventValue(writes[writes_]);
        const auto& requests = machine_->soundRequests();
        for (; requests_ < requests.size(); ++requests_)
            host_.sound(requests[requests_]);
        if (shownState_ != machine_->state())
        {
            shownState_ = std::string{machine_->state()};
            buildWidgets();
        }
        refreshWidgets();
        if (machine_->stopped())
        {
            stopTimer();
            host_.status("The Tool reached its TerminalState and stopped");
        }
        repaint();
    }

    void buildWidgets()
    {
        widgets_.clear();
        const auto add = [&](const tool::Variable& variable) {
            if (variable.widget.kind == tool::WidgetKind::None)
                return;
            Widget widget;
            widget.name = variable.name;
            widget.kind = variable.widget.kind;
            widget.digits = variable.widget.digits;
            widget.minimum = variable.minimum;
            widget.maximum = variable.maximum;
            const auto bounds = juce::Rectangle<double>{variable.widget.left, variable.widget.top, variable.widget.width,
                variable.widget.height}
                                    .toNearestInt();
            const auto name = variable.name;
            switch (variable.widget.kind)
            {
            case tool::WidgetKind::ReadOnlyText:
            {
                auto label = std::make_unique<juce::Label>();
                label->setColour(juce::Label::textColourId, textColour);
                widget.component = std::move(label);
                break;
            }
            case tool::WidgetKind::Text:
            {
                // A value typed in and accepted with Return.
                auto editor = std::make_unique<juce::TextEditor>();
                auto* raw = editor.get();
                editor->onReturnKey = [this, name, raw] { (void)set(name, typedValue(raw->getText())); };
                widget.component = std::move(editor);
                break;
            }
            case tool::WidgetKind::Meter:
                widget.component = std::make_unique<ToolMeter>();
                break;
            case tool::WidgetKind::Fader:
            case tool::WidgetKind::FaderNoNumber:
            {
                auto slider = std::make_unique<juce::Slider>(
                    bounds.getHeight() > bounds.getWidth() ? juce::Slider::LinearVertical : juce::Slider::LinearHorizontal,
                    variable.widget.kind == tool::WidgetKind::Fader ? juce::Slider::TextBoxRight : juce::Slider::NoTextBox);
                const auto low = variable.minimum.value_or(0.0);
                const auto high = std::max(variable.maximum.value_or(1.0), low + 1.0e-9);
                slider->setRange(low, high, variable.grid.value_or(0.0));
                if (variable.widget.logTaper && low > 0.0)
                    slider->setSkewFactorFromMidPoint(std::sqrt(low * high));
                if (variable.widget.digits.has_value())
                    slider->setNumDecimalPlacesToDisplay(*variable.widget.digits);
                auto* raw = slider.get();
                slider->onValueChange = [this, name, raw] { (void)set(name, st::Value::of(raw->getValue())); };
                widget.component = std::move(slider);
                break;
            }
            case tool::WidgetKind::RadioButtons:
            {
                // An array of strings in columns, one chosen.
                auto group = std::make_unique<juce::Component>();
                const auto columns = std::max(1, variable.widget.columns);
                const auto rows = std::max<int>(1, (static_cast<int>(variable.widget.strings.size()) + columns - 1) / columns);
                for (std::size_t index = 0; index < variable.widget.strings.size(); ++index)
                {
                    auto radio = std::make_unique<juce::ToggleButton>(juceString(variable.widget.strings[index]));
                    radio->setRadioGroupId(1);
                    const auto text = variable.widget.strings[index];
                    radio->onClick = [this, name, text] { (void)set(name, st::Value::string(text)); };
                    const auto column = static_cast<int>(index) % columns;
                    const auto row = static_cast<int>(index) / columns;
                    radio->setBounds(column * bounds.getWidth() / columns, row * bounds.getHeight() / rows,
                        bounds.getWidth() / columns, bounds.getHeight() / rows);
                    group->addAndMakeVisible(*radio);
                    widget.radios.push_back(std::move(radio));
                }
                widget.component = std::move(group);
                break;
            }
            case tool::WidgetKind::Button:
            case tool::WidgetKind::TriggerButton:
            case tool::WidgetKind::ToggleButton:
            case tool::WidgetKind::StickButton:
                widget.component = std::make_unique<ToolButton>(name, [this, name](tool::ButtonEvent event) { (void)press(name, event); });
                break;
            case tool::WidgetKind::View:
            {
                auto view = std::make_unique<ToolView>();
                view->drawings = [this, name] { return machine_ == nullptr ? std::vector<tool::Drawing>{} : machine_->drawings(name); };
                view->cursor = [this, name](double x, double y) {
                    if (machine_ != nullptr)
                        machine_->setCursorPoint(name, x, y);
                };
                view->origin = bounds.getPosition();
                widget.component = std::move(view);
                break;
            }
            case tool::WidgetKind::None:
                return;
            }
            widget.component->setBounds(bounds);
            addAndMakeVisible(*widget.component);
            // The lower Visible box shows the variable's name.
            if (variable.widget.labelled)
            {
                widget.label = std::make_unique<juce::Label>();
                widget.label->setText(juceString(variable.name), juce::dontSendNotification);
                widget.label->setColour(juce::Label::textColourId, secondaryTextColour);
                widget.label->setFont(WorkbenchType::micro());
                widget.label->setBounds(bounds.getX(), bounds.getY() - 16, std::max(60, bounds.getWidth()), 16);
                addAndMakeVisible(*widget.label);
            }
            widgets_.push_back(std::move(widget));
        };
        for (const auto* variable : tool::shownVariables(definition_, shownState_))
            add(*variable);
    }

    void refreshWidgets()
    {
        for (auto& widget : widgets_)
        {
            const auto value = machine_->variable(widget.name).value_or(st::Value::nil());
            if (auto* label = dynamic_cast<juce::Label*>(widget.component.get()))
                label->setText(valueText(value, widget.digits), juce::dontSendNotification);
            else if (auto* editor = dynamic_cast<juce::TextEditor*>(widget.component.get()))
            {
                if (!editor->hasKeyboardFocus(true))
                    editor->setText(valueText(value, widget.digits), false);
            }
            else if (auto* slider = dynamic_cast<juce::Slider*>(widget.component.get()))
            {
                if (value.kind == st::Value::Kind::Number && !slider->isMouseButtonDown())
                    slider->setValue(value.number, juce::dontSendNotification);
            }
            else if (auto* meter = dynamic_cast<ToolMeter*>(widget.component.get()))
            {
                const auto low = widget.minimum.value_or(0.0);
                const auto high = widget.maximum.value_or(1.0);
                meter->fraction = value.kind == st::Value::Kind::Number && high > low
                    ? std::clamp((value.number - low) / (high - low), 0.0, 1.0)
                    : 0.0;
                meter->repaint();
            }
            else if (auto* button = dynamic_cast<ToolButton*>(widget.component.get()))
            {
                if (widget.kind == tool::WidgetKind::StickButton || widget.kind == tool::WidgetKind::ToggleButton)
                {
                    button->down = value.kind == st::Value::Kind::Number && value.number != 0.0;
                    button->repaint();
                }
            }
            else if (auto* view = dynamic_cast<ToolView*>(widget.component.get()))
                view->repaint();
            for (const auto& radio : widget.radios)
            {
                radio->setToggleState((value.kind == st::Value::Kind::String || value.kind == st::Value::Kind::Symbol)
                        && radio->getButtonText() == juceString(value.text),
                    juce::dontSendNotification);
            }
        }
        // Debug shows every variable's value as it changes.
        const auto debug = machine_->variable("Debug");
        const auto debugging = debug.has_value() && debug->kind == st::Value::Kind::Boolean && debug->boolean;
        debug_.setVisible(debugging);
        if (debugging)
        {
            juce::StringArray lines;
            const auto list = [&](const std::vector<tool::Variable>& variables) {
                for (const auto& variable : variables)
                {
                    if (const auto value = machine_->variable(variable.name))
                        lines.add(juceString(variable.name) + ": " + valueText(*value, std::nullopt));
                }
            };
            list(definition_.variables);
            if (const auto state = std::ranges::find(definition_.states, shownState_, &tool::State::name); state != definition_.states.end())
                list(state->variables);
            debug_.setText(lines.joinIntoString("\n"), false);
        }
    }

    ToolRunHost host_;
    tool::Definition definition_;
    std::unique_ptr<tool::Machine> machine_;
    std::vector<Widget> widgets_;
    std::string shownState_;
    bool fixedSize_ = false;
    std::size_t writes_ = 0;
    std::size_t requests_ = 0;
    double last_ = 0.0;
    juce::TextEditor debug_;
};
}

std::unique_ptr<ToolRunPanel> makeToolRunPanel(ToolRunHost host)
{
    return std::make_unique<ToolRunPanelImpl>(std::move(host));
}
}
