#include "app/workbench/WorkbenchTheme.h"

#include "talas/app/AnalysisToolsModel.h"
#include "talas/app/WorkbenchUiPolicy.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace talas::app::workbench
{
namespace
{
std::atomic<float> workbenchInterfaceScale{1.0F};
std::atomic<float> workbenchFocusOutlinePixels{2.0F};

[[nodiscard]] float scaledTypeHeight(float base) noexcept
{
    return std::max(10.0F, base * workbenchInterfaceScale.load(
        std::memory_order_relaxed));
}
}

const WorkbenchTheme workbenchTheme;
const juce::Colour& backgroundColour = workbenchTheme.canvas;
const juce::Colour& panelColour = workbenchTheme.surface;
juce::Colour panelBorderColour = workbenchTheme.outline;
const juce::Colour& textColour = workbenchTheme.primaryText;
const juce::Colour& secondaryTextColour = workbenchTheme.secondaryText;
namespace
{
juce::Colour selectionColourNow = workbenchTheme.focus;
}
const juce::Colour& selectionColour = selectionColourNow;
const juce::Colour& audioColour = workbenchTheme.audio;
const juce::Colour& controlColour = workbenchTheme.control;
const juce::Colour& eventColour = workbenchTheme.event;
const juce::Colour& dataColour = workbenchTheme.data;
const juce::Colour& outputColour = workbenchTheme.output;
const juce::Colour& errorColour = workbenchTheme.error;

void WorkbenchType::setGraySelection(bool gray) noexcept
{
    selectionColourNow = juce::Colour{talas::app::selectionHighlightColours(gray).soundInSignalFlow};
}

void WorkbenchType::setInterfaceScale(float scale) noexcept
{
    if (std::isfinite(scale) && scale >= 1.0F && scale <= 1.5F)
        workbenchInterfaceScale.store(scale, std::memory_order_relaxed);
}

float WorkbenchType::interfaceScale() noexcept
{
    return workbenchInterfaceScale.load(std::memory_order_relaxed);
}

float WorkbenchType::focusOutlinePixels() noexcept
{
    return workbenchFocusOutlinePixels.load(std::memory_order_relaxed);
}

juce::Font WorkbenchType::panelTitle()
{
    return juce::Font{juce::FontOptions{scaledTypeHeight(13.0F)}.withStyle("Bold")};
}

juce::Font WorkbenchType::bodyStrong()
{
    return juce::Font{juce::FontOptions{scaledTypeHeight(12.0F)}.withStyle("Bold")};
}

juce::Font WorkbenchType::body()
{
    return juce::Font{juce::FontOptions{scaledTypeHeight(12.0F)}};
}

juce::Font WorkbenchType::labelStrong()
{
    return juce::Font{juce::FontOptions{scaledTypeHeight(11.0F)}.withStyle("Bold")};
}

juce::Font WorkbenchType::label()
{
    return juce::Font{juce::FontOptions{scaledTypeHeight(11.0F)}};
}

juce::Font WorkbenchType::microStrong()
{
    return juce::Font{juce::FontOptions{scaledTypeHeight(10.0F)}.withStyle("Bold")};
}

juce::Font WorkbenchType::micro()
{
    return juce::Font{juce::FontOptions{scaledTypeHeight(10.0F)}};
}

juce::Font WorkbenchType::data()
{
    return juce::Font{
        juce::FontOptions{scaledTypeHeight(11.0F)}.withFeatureEnabled(
            juce::FontFeatureTag{"tnum"})};
}

WorkbenchLookAndFeel::WorkbenchLookAndFeel()
{
    setColour(juce::TextButton::buttonColourId, workbenchTheme.widgetSurface);
    setColour(juce::TextButton::buttonOnColourId, workbenchTheme.widgetHighlight);
    setColour(juce::TextButton::textColourOffId, workbenchTheme.widgetText);
    setColour(juce::TextButton::textColourOnId, workbenchTheme.widgetText);
    setColour(juce::ComboBox::backgroundColourId, workbenchTheme.widgetSurface);
    setColour(juce::ComboBox::outlineColourId, workbenchTheme.widgetOutline);
    setColour(juce::ComboBox::textColourId, workbenchTheme.widgetText);
    setColour(juce::TextEditor::backgroundColourId, workbenchTheme.widgetSurface);
    setColour(juce::TextEditor::outlineColourId, workbenchTheme.widgetOutline);
    setColour(juce::TextEditor::focusedOutlineColourId, workbenchTheme.focus);
    setColour(juce::TextEditor::textColourId, workbenchTheme.widgetText);
    setColour(juce::Slider::thumbColourId, workbenchTheme.widgetFill);
    setColour(juce::Slider::trackColourId, workbenchTheme.widgetOutline);
    setColour(juce::ToggleButton::textColourId, workbenchTheme.widgetText);
    setColour(juce::ToggleButton::tickColourId, workbenchTheme.widgetFill);
    setColour(juce::ToggleButton::tickDisabledColourId, workbenchTheme.widgetOutline);
    // Fly-by help in a yellow box.
    const auto flyBy = talas::app::flyByHelpColours();
    setColour(juce::TooltipWindow::backgroundColourId, juce::Colour{flyBy.box});
    setColour(juce::TooltipWindow::textColourId, juce::Colour{flyBy.text});
    setColour(juce::TooltipWindow::outlineColourId, juce::Colour{flyBy.outline});
}

void WorkbenchLookAndFeel::setAccessibilityPresentation(
    bool enhancedContrast,
    float focusOutlinePixels) noexcept
{
    focusOutlinePixels_ = std::clamp(
        std::max(focusOutlinePixels, enhancedContrast ? 3.0F : 2.0F),
        2.0F,
        4.0F);
    workbenchFocusOutlinePixels.store(
        focusOutlinePixels_, std::memory_order_relaxed);
    panelBorderColour = enhancedContrast
        ? workbenchTheme.interactiveOutline
        : workbenchTheme.outline;
    const auto outline = enhancedContrast
        ? juce::Colour{0xffd4e2ef}
        : workbenchTheme.widgetOutline;
    setColour(juce::ComboBox::outlineColourId, outline);
    setColour(juce::TextEditor::outlineColourId, outline);
    setColour(juce::Slider::trackColourId, outline);
    setColour(juce::ToggleButton::tickDisabledColourId, outline);
}

juce::Font WorkbenchLookAndFeel::getTextButtonFont(juce::TextButton&, int)
{
    return WorkbenchType::body();
}

// A hot parameter's name is in italics: the inspector marks its parameter
// box so.
juce::Font WorkbenchLookAndFeel::getComboBoxFont(juce::ComboBox& box)
{
    return box.getProperties()["hotField"] ? WorkbenchType::body().italicised() : WorkbenchType::body();
}

void WorkbenchLookAndFeel::setActivityCursor(std::optional<juce::MouseCursor> cursor)
{
    activityCursor_ = std::move(cursor);
}

juce::MouseCursor WorkbenchLookAndFeel::getMouseCursorFor(juce::Component& component)
{
    return activityCursor_.has_value() ? *activityCursor_ : LookAndFeel_V4::getMouseCursorFor(component);
}

juce::MouseCursor workbenchLocatingMouseCursor(int frame)
{
    constexpr int size = 24;
    juce::Image image{juce::Image::ARGB, size, size, true};
    juce::Graphics graphics{image};
    const auto look = frame % 2 == 0 ? -1.5F : 1.5F;
    for (const auto centre : {7.0F, 17.0F})
    {
        graphics.setColour(juce::Colours::white);
        graphics.fillEllipse(centre - 4.5F, 7.5F, 9.0F, 9.0F);
        graphics.setColour(juce::Colours::black);
        graphics.drawEllipse(centre - 4.5F, 7.5F, 9.0F, 9.0F, 1.5F);
        graphics.fillEllipse(centre + look - 1.5F, 10.5F, 3.0F, 3.0F);
    }
    graphics.drawLine(11.5F, 11.0F, 12.5F, 11.0F, 1.5F);
    return juce::MouseCursor{image, size / 2, size / 2};
}

juce::MouseCursor workbenchMagnifierMouseCursor()
{
    constexpr int size = 24;
    const auto [x, y] = talas::app::spectrumViewMagnifierHotSpot;
    juce::Image image{juce::Image::ARGB, size, size, true};
    juce::Graphics graphics{image};
    const auto centre = juce::Point<float>{static_cast<float>(x), static_cast<float>(y)};
    graphics.setColour(juce::Colours::white);
    graphics.drawEllipse(centre.x - 7.0F, centre.y - 7.0F, 14.0F, 14.0F, 3.0F);
    graphics.drawLine(centre.x + 5.0F, centre.y + 5.0F, 22.0F, 22.0F, 4.5F);
    graphics.setColour(juce::Colours::black);
    graphics.drawEllipse(centre.x - 7.0F, centre.y - 7.0F, 14.0F, 14.0F, 1.5F);
    graphics.drawLine(centre.x + 5.0F, centre.y + 5.0F, 22.0F, 22.0F, 2.5F);
    // The plus is drawn as its outline alone, its middle left transparent.
    for (const auto [dx, dy] : std::initializer_list<std::pair<float, float>>{{-4.0F, 0.0F}, {4.0F, 0.0F}, {0.0F, -4.0F}, {0.0F, 4.0F}})
        graphics.drawLine(centre.x + dx * 0.4F, centre.y + dy * 0.4F, centre.x + dx, centre.y + dy, 1.2F);
    return juce::MouseCursor{image, x, y};
}

std::optional<juce::MouseCursor> workbenchActivityMouseCursor(talas::app::WorkbenchCursor cursor)
{
    constexpr int size = 24;
    const auto arrow = [](bool dot) {
        juce::Image image{juce::Image::ARGB, size, size, true};
        juce::Graphics graphics{image};
        juce::Path outline;
        outline.startNewSubPath(2.0F, 2.0F);
        for (const auto [x, y] : std::initializer_list<std::pair<float, float>>{{2.0F, 19.0F}, {6.5F, 14.5F}, {10.0F, 22.0F},
                 {13.0F, 20.5F}, {9.5F, 13.5F}, {16.0F, 13.5F}})
            outline.lineTo(x, y);
        outline.closeSubPath();
        graphics.setColour(juce::Colours::white);
        graphics.strokePath(outline, juce::PathStrokeType{3.0F});
        graphics.setColour(juce::Colours::black);
        graphics.strokePath(outline, juce::PathStrokeType{1.2F});
        if (dot)
            graphics.fillEllipse(4.0F, 10.0F, 4.0F, 4.0F);
        return juce::MouseCursor{image, 2, 2};
    };
    switch (cursor)
    {
    case talas::app::WorkbenchCursor::Arrow: return std::nullopt;
    case talas::app::WorkbenchCursor::Locating: return workbenchLocatingMouseCursor(0);
    // The system's animated busy cursor while samples load.
    case talas::app::WorkbenchCursor::LoadingSamples: return juce::MouseCursor{juce::MouseCursor::WaitCursor};
    // A pen, its point at the lower left, while a file is written.
    case talas::app::WorkbenchCursor::WritingFile:
    {
        juce::Image image{juce::Image::ARGB, size, size, true};
        juce::Graphics graphics{image};
        juce::Path pen;
        pen.startNewSubPath(2.0F, 22.0F);
        for (const auto [x, y] : std::initializer_list<std::pair<float, float>>{{4.0F, 15.0F}, {17.0F, 2.0F}, {22.0F, 7.0F}, {9.0F, 20.0F}})
            pen.lineTo(x, y);
        pen.closeSubPath();
        graphics.setColour(juce::Colours::white);
        graphics.strokePath(pen, juce::PathStrokeType{3.0F});
        graphics.setColour(juce::Colours::black);
        graphics.strokePath(pen, juce::PathStrokeType{1.2F});
        graphics.drawLine(4.0F, 15.0F, 9.0F, 20.0F, 1.2F);
        return juce::MouseCursor{image, 2, 22};
    }
    case talas::app::WorkbenchCursor::Playing: return arrow(false);
    case talas::app::WorkbenchCursor::PlayingFromDisk: return arrow(true);
    case talas::app::WorkbenchCursor::WaitingForDisk:
    {
        juce::Image image{juce::Image::ARGB, size, size, true};
        juce::Graphics graphics{image};
        juce::Path disk;
        disk.startNewSubPath(3.0F, 3.0F);
        for (const auto [x, y] : std::initializer_list<std::pair<float, float>>{{18.0F, 3.0F}, {21.0F, 6.0F}, {21.0F, 21.0F}, {3.0F, 21.0F}})
            disk.lineTo(x, y);
        disk.closeSubPath();
        graphics.setColour(juce::Colours::black);
        graphics.fillPath(disk);
        graphics.setColour(juce::Colours::white);
        graphics.strokePath(disk, juce::PathStrokeType{1.0F});
        graphics.fillRect(7.0F, 3.5F, 9.0F, 5.0F);
        graphics.fillRect(6.0F, 12.0F, 12.0F, 8.0F);
        return juce::MouseCursor{image, size / 2, size / 2};
    }
    case talas::app::WorkbenchCursor::Watch:
    {
        juce::Image image{juce::Image::ARGB, size, size, true};
        juce::Graphics graphics{image};
        graphics.setColour(juce::Colours::white);
        graphics.fillEllipse(3.0F, 3.0F, 18.0F, 18.0F);
        graphics.setColour(juce::Colours::black);
        graphics.drawEllipse(3.0F, 3.0F, 18.0F, 18.0F, 2.0F);
        graphics.drawLine(12.0F, 12.0F, 12.0F, 6.0F, 1.6F);
        graphics.drawLine(12.0F, 12.0F, 16.0F, 12.0F, 1.6F);
        return juce::MouseCursor{image, size / 2, size / 2};
    }
    }
    return std::nullopt;
}

juce::Label* WorkbenchLookAndFeel::createSliderTextBox(juce::Slider& slider)
{
    auto* label = juce::LookAndFeel_V4::createSliderTextBox(slider);
    label->setFont(WorkbenchType::label());
    return label;
}

void WorkbenchLookAndFeel::drawButtonBackground(
    juce::Graphics& graphics,
    juce::Button& button,
    const juce::Colour& colour,
    bool highlighted,
    bool down)
{
    juce::LookAndFeel_V4::drawButtonBackground(
        graphics,
        button,
        colour,
        highlighted,
        down);
    drawFocusOutline(graphics, button, 5.0F);
}

void WorkbenchLookAndFeel::drawToggleButton(
    juce::Graphics& graphics,
    juce::ToggleButton& button,
    bool highlighted,
    bool down)
{
    const auto font = WorkbenchType::body();
    const auto tickWidth = font.getHeight() * 1.1F;
    drawTickBox(
        graphics,
        button,
        4.0F,
        (static_cast<float>(button.getHeight()) - tickWidth) * 0.5F,
        tickWidth,
        tickWidth,
        button.getToggleState(),
        button.isEnabled(),
        highlighted,
        down);
    graphics.setColour(button.findColour(juce::ToggleButton::textColourId));
    graphics.setFont(font);
    if (!button.isEnabled())
        graphics.setOpacity(0.5F);
    graphics.drawFittedText(
        button.getButtonText(),
        button.getLocalBounds()
            .withTrimmedLeft(juce::roundToInt(tickWidth) + 10)
            .withTrimmedRight(2),
        juce::Justification::centredLeft,
        1);
    drawFocusOutline(graphics, button, 3.0F);
}

void WorkbenchLookAndFeel::drawComboBox(
    juce::Graphics& graphics,
    int width,
    int height,
    bool down,
    int buttonX,
    int buttonY,
    int buttonWidth,
    int buttonHeight,
    juce::ComboBox& box)
{
    juce::LookAndFeel_V4::drawComboBox(
        graphics,
        width,
        height,
        down,
        buttonX,
        buttonY,
        buttonWidth,
        buttonHeight,
        box);
    drawFocusOutline(graphics, box, 3.0F);
}

void WorkbenchLookAndFeel::drawLinearSlider(
    juce::Graphics& graphics,
    int x,
    int y,
    int width,
    int height,
    float sliderPos,
    float minSliderPos,
    float maxSliderPos,
    juce::Slider::SliderStyle style,
    juce::Slider& slider)
{
    juce::LookAndFeel_V4::drawLinearSlider(
        graphics,
        x,
        y,
        width,
        height,
        sliderPos,
        minSliderPos,
        maxSliderPos,
        style,
        slider);
    drawFocusOutline(graphics, slider, 3.0F);
}

void WorkbenchLookAndFeel::drawFocusOutline(
    juce::Graphics& graphics,
    const juce::Component& component,
    float radius) const
{
    if (!component.hasKeyboardFocus(true))
        return;
    graphics.setColour(workbenchTheme.focus);
    graphics.drawRoundedRectangle(
        component.getLocalBounds().toFloat().reduced(1.5F),
        radius,
        focusOutlinePixels_);
}
}
