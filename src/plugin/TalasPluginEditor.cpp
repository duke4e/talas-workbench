#include "plugin/TalasPluginEditor.h"

#include "plugin/TalasPluginProcessor.h"

#include <algorithm>

namespace talas::plugin
{
namespace
{
using talas::app::workbench::WorkbenchStyle;
using talas::app::workbench::WorkbenchType;
using talas::app::workbench::workbenchTheme;

constexpr int margin = 14;
constexpr int headerHeight = 78;
constexpr int footerHeight = 30;
constexpr int cellWidth = 112;
constexpr int cellHeight = 134;
constexpr int maximumColumns = 6;
constexpr int maximumVisibleRows = 3;

[[nodiscard]] int columnsFor(std::size_t count) noexcept
{
    return std::clamp(static_cast<int>(count), 3, maximumColumns);
}

[[nodiscard]] int rowsFor(std::size_t count) noexcept
{
    const auto columns = columnsFor(count);
    return std::max(1, (static_cast<int>(count) + columns - 1) / columns);
}

[[nodiscard]] juce::String text(const std::string& value)
{
    return juce::String::fromUTF8(value.c_str());
}
}

struct TalasPluginEditor::Control
{
    juce::Label label;
    std::unique_ptr<juce::Slider> knob;
    std::unique_ptr<juce::ToggleButton> toggle;
    std::unique_ptr<juce::SliderParameterAttachment> knobAttachment;
    std::unique_ptr<juce::ButtonParameterAttachment> toggleAttachment;
};

// The parameters' cells, in a glass panel; scrolled when there are many.
class TalasPluginEditor::ControlGrid final : public juce::Component
{
public:
    explicit ControlGrid(TalasPluginProcessor& processor)
    {
        const auto& specs = processor.parameterSpecs();
        for (std::size_t index = 0; index < specs.size(); ++index)
        {
            auto* parameter = processor.parameterFor(index);
            if (parameter == nullptr)
                continue;
            const auto& spec = specs[index];
            auto control = std::make_unique<Control>();
            control->label.setText(text(spec.displayName), juce::dontSendNotification);
            control->label.setJustificationType(juce::Justification::centred);
            control->label.setFont(WorkbenchType::labelStrong());
            control->label.setColour(juce::Label::textColourId, workbenchTheme.secondaryText);
            control->label.setMinimumHorizontalScale(0.7F);
            addAndMakeVisible(control->label);
            if (spec.kind == PluginParameterKind::Continuous)
            {
                control->knob = std::make_unique<juce::Slider>(
                    juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
                control->knob->setTextBoxStyle(juce::Slider::TextBoxBelow, false, cellWidth - 18, 20);
                if (!spec.unit.empty())
                    control->knob->setTextValueSuffix(" " + text(spec.unit));
                control->knob->setTitle(text(spec.displayName));
                control->knob->setTooltip("!" + text(spec.eventName));
                addAndMakeVisible(*control->knob);
                control->knobAttachment = std::make_unique<juce::SliderParameterAttachment>(*parameter, *control->knob);
            }
            else
            {
                control->toggle = std::make_unique<juce::ToggleButton>(
                    spec.kind == PluginParameterKind::Momentary ? "Hold" : "On");
                control->toggle->setTitle(text(spec.displayName));
                control->toggle->setTooltip("!" + text(spec.eventName));
                addAndMakeVisible(*control->toggle);
                control->toggleAttachment =
                    std::make_unique<juce::ButtonParameterAttachment>(*parameter, *control->toggle);
                if (spec.kind == PluginParameterKind::Momentary)
                {
                    auto* button = control->toggle.get();
                    button->setClickingTogglesState(false);
                    button->onStateChange = [button, parameter] {
                        const auto down = button->isDown();
                        if (parameter->getValue() != (down ? 1.0F : 0.0F))
                            parameter->setValueNotifyingHost(down ? 1.0F : 0.0F);
                    };
                }
            }
            controls_.push_back(std::move(control));
        }
        empty_ = controls_.empty();
        setSize(columnsFor(controls_.size()) * cellWidth, rowsFor(controls_.size()) * cellHeight);
    }

    void paint(juce::Graphics& graphics) override
    {
        if (!empty_)
            return;
        graphics.setColour(workbenchTheme.secondaryText);
        graphics.setFont(WorkbenchType::body());
        graphics.drawFittedText("This plugin has no parameters: its project declares no live controls.",
            getLocalBounds().reduced(12), juce::Justification::centred, 3);
    }

    void resized() override
    {
        const auto columns = columnsFor(controls_.size());
        const auto left = std::max(0, (getWidth() - columns * cellWidth) / 2);
        for (std::size_t index = 0; index < controls_.size(); ++index)
        {
            auto& control = *controls_[index];
            const auto column = static_cast<int>(index) % columns;
            const auto row = static_cast<int>(index) / columns;
            auto cell = juce::Rectangle<int>{left + column * cellWidth, row * cellHeight, cellWidth, cellHeight}
                            .reduced(6, 4);
            control.label.setBounds(cell.removeFromTop(22));
            if (control.knob != nullptr)
                control.knob->setBounds(cell);
            else if (control.toggle != nullptr)
                control.toggle->setBounds(cell.withSizeKeepingCentre(std::min(cell.getWidth(), 84), 30));
        }
    }

private:
    std::vector<std::unique_ptr<Control>> controls_;
    bool empty_ = true;
};

juce::Point<int> TalasPluginEditor::preferredSize(std::size_t parameterCount) noexcept
{
    const auto columns = columnsFor(parameterCount);
    const auto rows = std::min(rowsFor(parameterCount), maximumVisibleRows);
    const auto width = columns * cellWidth + 4 * margin;
    const auto height = headerHeight + rows * cellHeight + 2 * margin + footerHeight + margin;
    return {width, height};
}

TalasPluginEditor::TalasPluginEditor(TalasPluginProcessor& processor)
    : juce::AudioProcessorEditor(processor), processor_(processor)
{
    setLookAndFeel(&lookAndFeel_);
    grid_ = std::make_unique<ControlGrid>(processor_);
    viewport_.setViewedComponent(grid_.get(), false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
    status_.setFont(WorkbenchType::micro());
    status_.setColour(juce::Label::textColourId, workbenchTheme.secondaryText);
    status_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(status_);
    const auto size = preferredSize(processor_.parameterSpecs().size());
    setSize(size.x, size.y);
    timerCallback();
    startTimerHz(4);
}

TalasPluginEditor::~TalasPluginEditor()
{
    stopTimer();
    viewport_.setViewedComponent(nullptr, false);
    grid_.reset();
    setLookAndFeel(nullptr);
}

void TalasPluginEditor::timerCallback()
{
    const auto status = processor_.statusText();
    if (status == shownStatus_)
        return;
    shownStatus_ = status;
    status_.setText(status, juce::dontSendNotification);
    const auto problem = !processor_.engineReady() && status != "Loading" && status != "Stopped";
    status_.setColour(juce::Label::textColourId, problem ? workbenchTheme.error : workbenchTheme.secondaryText);
}

void TalasPluginEditor::paint(juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    WorkbenchStyle::paintBackdrop(graphics, bounds);
    const auto radius = WorkbenchStyle::panelRadius();

    auto header = getLocalBounds().reduced(margin).removeFromTop(headerHeight - margin).toFloat();
    WorkbenchStyle::paintPanelShadow(graphics, header, radius);
    WorkbenchStyle::paintPanel(graphics, header, radius);
    // The signature gradient's bar, then the name drawn in it.
    auto inner = header.reduced(16.0F, 10.0F);
    graphics.setGradientFill(WorkbenchStyle::accentGradient(inner.withWidth(4.0F)));
    graphics.fillRoundedRectangle(inner.removeFromLeft(4.0F), 2.0F);
    inner.removeFromLeft(12.0F);
    const auto& identity = processor_.identity();
    auto titleArea = inner.removeFromTop(inner.getHeight() * 0.62F);
    graphics.setFont(juce::Font{juce::FontOptions{22.0F * WorkbenchType::interfaceScale()}.withStyle("Bold")});
    graphics.setGradientFill(WorkbenchStyle::accentGradient(titleArea));
    graphics.drawFittedText(text(identity.name), titleArea.toNearestInt(), juce::Justification::bottomLeft, 1);
    graphics.setColour(workbenchTheme.secondaryText);
    graphics.setFont(WorkbenchType::label());
    const auto kind = processor_.kind() == PluginKind::Instrument ? "Instrument" : "Effect";
    graphics.drawFittedText(text(identity.manufacturer) + juce::String::fromUTF8(" \xc2\xb7 ") + kind
            + juce::String::fromUTF8(" \xc2\xb7 version ") + text(identity.version),
        inner.toNearestInt(), juce::Justification::topLeft, 1);
    graphics.setColour(workbenchTheme.secondaryText.withAlpha(0.7F));
    graphics.setFont(WorkbenchType::microStrong());
    graphics.drawFittedText("TALAS", header.reduced(16.0F, 10.0F).toNearestInt(), juce::Justification::topRight, 1);

    auto body = viewport_.getBounds().toFloat().expanded(static_cast<float>(margin) * 0.5F);
    WorkbenchStyle::paintPanelShadow(graphics, body, radius);
    WorkbenchStyle::paintPanel(graphics, body, radius);
}

void TalasPluginEditor::resized()
{
    auto area = getLocalBounds().reduced(margin);
    area.removeFromTop(headerHeight);
    auto footer = area.removeFromBottom(footerHeight);
    status_.setBounds(footer.reduced(4, 2));
    area.removeFromBottom(margin / 2);
    const auto body = area.reduced(margin / 2);
    viewport_.setBounds(body);
    if (grid_ != nullptr)
    {
        const auto scroll = grid_->getHeight() > body.getHeight() ? viewport_.getScrollBarThickness() : 0;
        grid_->setTopLeftPosition(0, 0);
        grid_->setSize(std::max(grid_->getWidth(), body.getWidth() - scroll), grid_->getHeight());
    }
}
}
