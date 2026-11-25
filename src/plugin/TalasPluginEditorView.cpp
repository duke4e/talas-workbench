#include "plugin/TalasPluginEditorView.h"

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
constexpr int cellWidth = 116;
constexpr int cellHeight = 144;
constexpr int cellGap = 8;
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

// A toggle or a gate as a pad: lit while on (or, for a gate, while held),
// its state written inside.
class PadButton final : public juce::Button
{
public:
    PadButton(const juce::String& name, bool momentary)
        : juce::Button(name), momentary_(momentary)
    {
    }

    void paintButton(juce::Graphics& graphics, bool highlighted, bool down) override
    {
        const auto on = getToggleState() || (momentary_ && down);
        auto area = getLocalBounds().toFloat().reduced(1.0F);
        if (highlighted && !on && !WorkbenchStyle::enhancedContrast())
        {
            graphics.setColour(juce::Colours::white.withAlpha(0.04F));
            graphics.fillRoundedRectangle(area.expanded(1.0F), 9.0F);
        }
        WorkbenchStyle::paintPad(graphics, area, on, isEnabled(),
            momentary_ ? (on ? "Held" : "Hold") : (on ? "On" : "Off"));
        if (hasKeyboardFocus(false))
        {
            graphics.setColour(workbenchTheme.focus);
            graphics.drawRoundedRectangle(area, std::min(8.0F, area.getHeight() * 0.22F),
                WorkbenchType::focusOutlinePixels());
        }
    }

private:
    bool momentary_ = false;
};
}

std::unique_ptr<juce::RangedAudioParameter> makePluginParameter(const PluginParameterSpec& spec)
{
    const auto id = juce::ParameterID{text(spec.eventName), 1};
    const auto name = text(spec.displayName);
    if (spec.kind != PluginParameterKind::Continuous)
    {
        return std::make_unique<juce::AudioParameterBool>(
            id, name, spec.defaultValue >= spec.maximum, juce::AudioParameterBoolAttributes{}.withLabel(text(spec.unit)));
    }
    // The spec's own mapping, so a log taper and a grid look in the host as
    // they do on the Virtual Control Surface.
    juce::NormalisableRange<float> range{
        static_cast<float>(spec.minimum),
        static_cast<float>(spec.maximum),
        [spec](float, float, float normalized) {
            return static_cast<float>(parameterValueFromNormalized(spec, normalized));
        },
        [spec](float, float, float value) {
            return static_cast<float>(parameterValueToNormalized(spec, value));
        },
        [spec](float, float, float value) {
            return static_cast<float>(clampParameterValue(spec, value));
        }};
    const auto decimals = spec.step >= 1.0 ? 0 : (spec.maximum - spec.minimum) >= 100.0 ? 1 : 3;
    auto attributes = juce::AudioParameterFloatAttributes{}
                          .withLabel(text(spec.unit))
                          .withStringFromValueFunction([decimals](float value, int) {
                              return juce::String(value, decimals);
                          })
                          .withValueFromStringFunction([](const juce::String& entered) {
                              return entered.getFloatValue();
                          });
    return std::make_unique<juce::AudioParameterFloat>(
        id, name, range, static_cast<float>(spec.defaultValue), attributes);
}

struct TalasPluginEditorView::Control
{
    juce::Label label;
    std::unique_ptr<juce::Slider> knob;
    std::unique_ptr<PadButton> pad;
    std::unique_ptr<juce::SliderParameterAttachment> knobAttachment;
    std::unique_ptr<juce::ButtonParameterAttachment> padAttachment;
};

// The parameters' cells, in a glass panel; scrolled when there are many.
class TalasPluginEditorView::ControlGrid final : public juce::Component
{
public:
    explicit ControlGrid(const std::vector<PluginEditorParameter>& parameters)
    {
        for (const auto& [spec, parameter] : parameters)
        {
            if (parameter == nullptr)
                continue;
            auto control = std::make_unique<Control>();
            control->label.setText(text(spec.displayName), juce::dontSendNotification);
            control->label.setJustificationType(juce::Justification::centred);
            control->label.setFont(WorkbenchType::labelStrong());
            control->label.setColour(juce::Label::textColourId, workbenchTheme.primaryText);
            control->label.setMinimumHorizontalScale(0.7F);
            control->label.setInterceptsMouseClicks(false, false);
            addAndMakeVisible(control->label);
            if (spec.kind == PluginParameterKind::Continuous)
            {
                control->knob = std::make_unique<juce::Slider>(
                    juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
                // The value as a lit readout under the knob.
                control->knob->getProperties().set("talasReadout", true);
                control->knob->setTextBoxStyle(juce::Slider::TextBoxBelow, false, cellWidth - 26, 22);
                if (!spec.unit.empty())
                    control->knob->setTextValueSuffix(" " + text(spec.unit));
                control->knob->setTitle(text(spec.displayName));
                control->knob->setTooltip("!" + text(spec.eventName));
                addAndMakeVisible(*control->knob);
                control->knobAttachment = std::make_unique<juce::SliderParameterAttachment>(*parameter, *control->knob);
            }
            else
            {
                const auto momentary = spec.kind == PluginParameterKind::Momentary;
                control->pad = std::make_unique<PadButton>(momentary ? "Hold" : "On", momentary);
                control->pad->setTitle(text(spec.displayName));
                control->pad->setTooltip("!" + text(spec.eventName));
                control->pad->setClickingTogglesState(!momentary);
                addAndMakeVisible(*control->pad);
                control->padAttachment =
                    std::make_unique<juce::ButtonParameterAttachment>(*parameter, *control->pad);
                if (momentary)
                {
                    auto* button = control->pad.get();
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
        if (empty_)
        {
            graphics.setColour(workbenchTheme.secondaryText);
            graphics.setFont(WorkbenchType::body());
            graphics.drawFittedText("This plugin has no parameters: its project declares no live controls.",
                getLocalBounds().reduced(12), juce::Justification::centred, 3);
            return;
        }
        // Each control on a tile capped with the gradient.
        for (std::size_t index = 0; index < controls_.size(); ++index)
        {
            const auto tile = cellBounds(index).toFloat();
            WorkbenchStyle::paintTile(graphics, tile, 7.0F, workbenchTheme.controlTile);
            juce::Graphics::ScopedSaveState clipped(graphics);
            juce::Path shape;
            shape.addRoundedRectangle(tile, 7.0F);
            graphics.reduceClipRegion(shape);
            graphics.setGradientFill(WorkbenchStyle::accentGradient(tile.withHeight(2.0F)));
            graphics.fillRect(tile.withHeight(2.0F));
        }
    }

    void resized() override
    {
        for (std::size_t index = 0; index < controls_.size(); ++index)
        {
            auto& control = *controls_[index];
            auto cell = cellBounds(index).reduced(8, 6);
            control.label.setBounds(cell.removeFromTop(22));
            if (control.knob != nullptr)
                control.knob->setBounds(cell.reduced(2, 0));
            else if (control.pad != nullptr)
                control.pad->setBounds(cell.withSizeKeepingCentre(cell.getWidth(), std::min(cell.getHeight(), 46)));
        }
    }

private:
    [[nodiscard]] juce::Rectangle<int> cellBounds(std::size_t index) const
    {
        const auto columns = columnsFor(controls_.size());
        const auto left = std::max(0, (getWidth() - columns * cellWidth) / 2);
        const auto column = static_cast<int>(index) % columns;
        const auto row = static_cast<int>(index) / columns;
        return juce::Rectangle<int>{left + column * cellWidth, row * cellHeight, cellWidth, cellHeight}
            .reduced(cellGap / 2);
    }

    std::vector<std::unique_ptr<Control>> controls_;
    bool empty_ = true;
};

juce::Point<int> TalasPluginEditorView::preferredSize(std::size_t parameterCount) noexcept
{
    const auto columns = columnsFor(parameterCount);
    const auto rows = std::min(rowsFor(parameterCount), maximumVisibleRows);
    const auto width = columns * cellWidth + 4 * margin;
    // The margins around the panels, the gap above the footer and the
    // body panel's own inset.
    const auto height = headerHeight + rows * cellHeight + 3 * margin + footerHeight + margin / 2;
    return {width, height};
}

TalasPluginEditorView::TalasPluginEditorView(PluginEditorModel model)
    : model_(std::move(model))
{
    grid_ = std::make_unique<ControlGrid>(model_.parameters);
    viewport_.setViewedComponent(grid_.get(), false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
    status_.setFont(WorkbenchType::label());
    status_.setColour(juce::Label::textColourId, workbenchTheme.secondaryText);
    status_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(status_);
    const auto size = preferredSize(model_.parameters.size());
    setSize(size.x, size.y);
    timerCallback();
    startTimerHz(4);
}

TalasPluginEditorView::~TalasPluginEditorView()
{
    stopTimer();
    viewport_.setViewedComponent(nullptr, false);
    grid_.reset();
}

void TalasPluginEditorView::timerCallback()
{
    const auto status = model_.status ? model_.status() : juce::String{};
    if (status == shownStatus_)
        return;
    shownStatus_ = status;
    status_.setText(status, juce::dontSendNotification);
    const auto ready = model_.ready && model_.ready();
    problem_ = !ready && status != "Loading" && status != "Stopped";
    status_.setColour(juce::Label::textColourId, problem_ ? workbenchTheme.error : workbenchTheme.secondaryText);
    repaint(status_.getBounds().withX(0).withRight(status_.getX()));
}

void TalasPluginEditorView::paint(juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds().toFloat();
    WorkbenchStyle::paintBackdrop(graphics, bounds);
    const auto radius = WorkbenchStyle::panelRadius();

    auto header = getLocalBounds().reduced(margin).removeFromTop(headerHeight - margin).toFloat();
    WorkbenchStyle::paintPanelShadow(graphics, header, radius);
    WorkbenchStyle::paintPanel(graphics, header, radius);
    // The Talas mark, then the name in the gradient over its maker, kind and
    // version; the kind again as a pill at the right.
    auto inner = header.reduced(14.0F, 10.0F);
    const auto markSize = std::min(38.0F, inner.getHeight());
    WorkbenchStyle::paintBrand(graphics, inner.removeFromLeft(markSize).withSizeKeepingCentre(markSize, markSize), false);
    inner.removeFromLeft(12.0F);
    const auto kind = model_.kind == PluginKind::Instrument ? juce::String{"Instrument"} : juce::String{"Effect"};
    const auto pillFont = WorkbenchType::microStrong();
    const auto pillWidth = juce::GlyphArrangement::getStringWidth(pillFont, kind.toUpperCase()) + 16.0F;
    const auto pill = inner.removeFromRight(pillWidth).withSizeKeepingCentre(pillWidth, 18.0F);
    WorkbenchStyle::paintPill(graphics, pill, workbenchTheme.accentStart, workbenchTheme.surface, kind.toUpperCase(),
        pillFont);
    inner.removeFromRight(10.0F);
    auto titleArea = inner.removeFromTop(inner.getHeight() * 0.6F);
    graphics.setFont(juce::Font{juce::FontOptions{22.0F * WorkbenchType::interfaceScale()}.withStyle("Bold")});
    graphics.setGradientFill(WorkbenchStyle::accentGradient(titleArea.withWidth(std::min(titleArea.getWidth(), 260.0F))));
    graphics.drawFittedText(text(model_.identity.name), titleArea.toNearestInt(), juce::Justification::bottomLeft, 1);
    graphics.setColour(workbenchTheme.secondaryText);
    graphics.setFont(WorkbenchType::label());
    graphics.drawFittedText(text(model_.identity.manufacturer) + juce::String::fromUTF8(" \xc2\xb7 ") + kind
            + juce::String::fromUTF8(" \xc2\xb7 version ") + text(model_.identity.version),
        inner.toNearestInt(), juce::Justification::topLeft, 1);

    auto body = viewport_.getBounds().toFloat().expanded(static_cast<float>(margin) * 0.5F);
    WorkbenchStyle::paintPanelShadow(graphics, body, radius);
    WorkbenchStyle::paintPanel(graphics, body, radius);

    // A lamp before the status: lit when the project plays, red when it
    // cannot; the words say which.
    const auto lamp = juce::Rectangle<float>{7.0F, 7.0F}.withCentre(
        {static_cast<float>(status_.getX()) - 6.0F, static_cast<float>(status_.getBounds().getCentreY())});
    const auto ready = model_.ready && model_.ready();
    const auto lampColour = problem_ ? workbenchTheme.error : ready ? workbenchTheme.success : workbenchTheme.secondaryText;
    if (ready && !WorkbenchStyle::enhancedContrast())
    {
        graphics.setColour(lampColour.withAlpha(0.3F));
        graphics.fillEllipse(lamp.expanded(3.0F));
    }
    graphics.setColour(lampColour);
    graphics.fillEllipse(lamp);
}

void TalasPluginEditorView::resized()
{
    auto area = getLocalBounds().reduced(margin);
    area.removeFromTop(headerHeight);
    auto footer = area.removeFromBottom(footerHeight);
    status_.setBounds(footer.reduced(4, 2).withTrimmedLeft(14));
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
