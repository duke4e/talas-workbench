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
std::atomic<bool> workbenchEnhancedContrast{false};

[[nodiscard]] float scaledTypeHeight(float base) noexcept
{
    return std::max(10.0F, base * workbenchInterfaceScale.load(
        std::memory_order_relaxed));
}

// The Talas palette: deep ink, glass surfaces, one violet-to-aqua signature
// gradient and saturated colour codes for port kinds. Text colours keep at
// least 4.5:1 against every surface they are drawn on.
[[nodiscard]] WorkbenchTheme makeWorkbenchTheme()
{
    WorkbenchTheme theme;
    theme.canvas = juce::Colour{0xff0c0e16};
    theme.surface = juce::Colour{0xff161a27};
    theme.outline = juce::Colour{0xff272d42};
    theme.interactiveOutline = juce::Colour{0xff58628a};
    theme.primaryText = juce::Colour{0xffedeff8};
    theme.secondaryText = juce::Colour{0xffa6acc6};
    theme.focus = juce::Colour{talas::app::selectionHighlightColours().soundInSignalFlow};
    theme.audio = juce::Colour{0xff2ee6c9};
    theme.control = juce::Colour{0xffffb84d};
    theme.event = juce::Colour{0xffff5fa8};
    theme.data = juce::Colour{0xff8fa3ff};
    theme.output = juce::Colour{0xff5fa8ff};
    theme.error = juce::Colour{0xffff5d6e};
    theme.fullSupport = juce::Colour{0xff16805a};
    theme.subsetSupport = juce::Colour{0xff9a6410};
    theme.placeholderSupport = juce::Colour{0xffc2475a};
    theme.utilitySupport = juce::Colour{0xff56709a};
    theme.graphGrid = juce::Colour{0xff222842};
    theme.selectedRow = juce::Colour{talas::app::selectionHighlightColours().row};
    theme.controlTile = juce::Colour{0xff1b2031};
    theme.widgetSurface = juce::Colour{0xff23293d};
    theme.widgetOutline = juce::Colour{0xff3b4463};
    theme.widgetText = juce::Colour{0xffedeff8};
    theme.widgetFill = juce::Colour{0xff9b6bff};
    theme.widgetHighlight = juce::Colour{0xff9b6bff};
    theme.backdropTop = juce::Colour{0xff151829};
    theme.backdropBottom = juce::Colour{0xff090a11};
    theme.surfaceRaised = juce::Colour{0xff23293d};
    theme.well = juce::Colour{0xff0d0f18};
    theme.accentStart = juce::Colour{0xff9b6bff};
    theme.accentEnd = juce::Colour{0xff22d8e6};
    theme.accentText = juce::Colour{0xff0a0b14};
    theme.glassHighlight = juce::Colours::white.withAlpha(0.13F);
    theme.glassEdge = juce::Colours::white.withAlpha(0.085F);
    theme.shadow = juce::Colours::black.withAlpha(0.6F);
    theme.enhancedOutline = juce::Colour{0xffd4e2ef};
    theme.success = juce::Colour{0xff3ddc97};
    theme.warning = juce::Colour{0xffffb84d};
    return theme;
}

[[nodiscard]] juce::Path roundedPath(juce::Rectangle<float> area, float radius,
    bool flatLeft = false, bool flatRight = false, bool flatTop = false, bool flatBottom = false)
{
    juce::Path path;
    path.addRoundedRectangle(area.getX(), area.getY(), area.getWidth(), area.getHeight(), radius, radius,
        !(flatLeft || flatTop), !(flatRight || flatTop), !(flatLeft || flatBottom), !(flatRight || flatBottom));
    return path;
}

// A small chevron, pointing down, centred in the area.
void drawChevron(juce::Graphics& graphics, juce::Rectangle<float> area, juce::Colour colour)
{
    const auto size = std::min(area.getWidth(), area.getHeight()) * 0.32F;
    const auto centre = area.getCentre();
    juce::Path chevron;
    chevron.startNewSubPath(centre.x - size, centre.y - size * 0.45F);
    chevron.lineTo(centre.x, centre.y + size * 0.55F);
    chevron.lineTo(centre.x + size, centre.y - size * 0.45F);
    graphics.setColour(colour);
    graphics.strokePath(chevron, juce::PathStrokeType{1.6F, juce::PathStrokeType::curved, juce::PathStrokeType::rounded});
}
}

const WorkbenchTheme workbenchTheme = makeWorkbenchTheme();
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

float WorkbenchStyle::panelRadius() noexcept
{
    return 9.0F * WorkbenchType::interfaceScale();
}

float WorkbenchStyle::controlRadius() noexcept
{
    return 5.0F * WorkbenchType::interfaceScale();
}

bool WorkbenchStyle::enhancedContrast() noexcept
{
    return workbenchEnhancedContrast.load(std::memory_order_relaxed);
}

juce::ColourGradient WorkbenchStyle::accentGradient(juce::Rectangle<float> area, float alpha)
{
    juce::ColourGradient gradient{workbenchTheme.accentStart.withMultipliedAlpha(alpha), area.getX(), area.getY(),
        workbenchTheme.accentEnd.withMultipliedAlpha(alpha), area.getRight(), area.getBottom(), false};
    gradient.addColour(0.5, accentAt(0.5F).withMultipliedAlpha(alpha));
    return gradient;
}

juce::Colour WorkbenchStyle::accentAt(float proportion)
{
    // Through a clear blue rather than the gray a straight blend would pass.
    const auto middle = juce::Colour{0xff5b9dff};
    const auto t = std::clamp(proportion, 0.0F, 1.0F);
    return t < 0.5F ? workbenchTheme.accentStart.interpolatedWith(middle, t * 2.0F)
                    : middle.interpolatedWith(workbenchTheme.accentEnd, (t - 0.5F) * 2.0F);
}

void WorkbenchStyle::paintBackdrop(juce::Graphics& graphics, juce::Rectangle<float> area)
{
    graphics.setGradientFill(juce::ColourGradient{workbenchTheme.backdropTop, area.getX(), area.getY(),
        workbenchTheme.backdropBottom, area.getX(), area.getBottom(), false});
    graphics.fillRect(area);
    if (enhancedContrast())
        return;
    const auto glow = [&](juce::Point<float> centre, float radius, juce::Colour colour) {
        graphics.setGradientFill(juce::ColourGradient{colour, centre.x, centre.y,
            colour.withAlpha(0.0F), centre.x + radius, centre.y, true});
        graphics.fillRect(area);
    };
    const auto extent = std::max(area.getWidth(), area.getHeight());
    glow({area.getX() + area.getWidth() * 0.12F, area.getY()}, extent * 0.55F,
        workbenchTheme.accentStart.withAlpha(0.13F));
    glow({area.getRight(), area.getBottom()}, extent * 0.5F, workbenchTheme.accentEnd.withAlpha(0.07F));
}

void WorkbenchStyle::paintPanelShadow(juce::Graphics& graphics, juce::Rectangle<float> area, float radius)
{
    if (area.isEmpty())
        return;
    juce::Path shape;
    shape.addRoundedRectangle(area, radius);
    juce::DropShadow{workbenchTheme.shadow.withMultipliedAlpha(enhancedContrast() ? 0.5F : 1.0F),
        juce::roundToInt(18.0F * WorkbenchType::interfaceScale()), {0, 6}}
        .drawForPath(graphics, shape);
}

void WorkbenchStyle::paintPanel(juce::Graphics& graphics, juce::Rectangle<float> area, float radius,
    std::optional<juce::Colour> tint)
{
    if (area.isEmpty())
        return;
    const auto shape = roundedPath(area, radius);
    const auto& surface = workbenchTheme.surface;
    if (enhancedContrast())
    {
        graphics.setColour(surface);
        graphics.fillPath(shape);
        graphics.setColour(panelBorderColour);
        graphics.strokePath(roundedPath(area.reduced(0.75F), radius), juce::PathStrokeType{1.5F});
        return;
    }
    graphics.setGradientFill(juce::ColourGradient{surface.interpolatedWith(juce::Colours::white, 0.035F),
        area.getX(), area.getY(), surface.interpolatedWith(juce::Colours::black, 0.14F), area.getX(),
        area.getBottom(), false});
    graphics.fillPath(shape);
    {
        juce::Graphics::ScopedSaveState clipped(graphics);
        graphics.reduceClipRegion(shape);
        if (tint.has_value())
        {
            graphics.setGradientFill(juce::ColourGradient{tint->withAlpha(0.10F), area.getX(), area.getY(),
                tint->withAlpha(0.0F), area.getX() + area.getWidth() * 0.6F, area.getY() + 80.0F, false});
            graphics.fillRect(area);
        }
        // The sheen: light falling on the top of the glass.
        const auto sheenDepth = std::min(56.0F, area.getHeight() * 0.4F);
        graphics.setGradientFill(juce::ColourGradient{juce::Colours::white.withAlpha(0.05F), area.getX(), area.getY(),
            juce::Colours::white.withAlpha(0.0F), area.getX(), area.getY() + sheenDepth, false});
        graphics.fillRect(area.withHeight(sheenDepth));
    }
    // The highlight along the top edge, fading out at the corners.
    const auto lineY = area.getY() + 1.0F;
    juce::ColourGradient highlight{workbenchTheme.glassHighlight.withAlpha(0.0F), area.getX() + radius, lineY,
        workbenchTheme.glassHighlight.withAlpha(0.0F), area.getRight() - radius, lineY, false};
    highlight.addColour(0.3, workbenchTheme.glassHighlight);
    highlight.addColour(0.7, workbenchTheme.glassHighlight);
    graphics.setGradientFill(highlight);
    graphics.fillRect(juce::Rectangle<float>{area.getX() + radius, lineY, area.getWidth() - 2.0F * radius, 1.0F});
    graphics.setColour(workbenchTheme.glassEdge);
    graphics.strokePath(roundedPath(area.reduced(0.5F), radius), juce::PathStrokeType{1.0F});
}

void WorkbenchStyle::paintPanelTitle(juce::Graphics& graphics, juce::Rectangle<int> area, const juce::String& title)
{
    const auto bar = juce::Rectangle<float>{static_cast<float>(area.getX()) - 7.0F,
        static_cast<float>(area.getCentreY()) - 6.0F, 3.0F, 12.0F};
    graphics.setGradientFill(juce::ColourGradient{workbenchTheme.accentStart, bar.getX(), bar.getY(),
        workbenchTheme.accentEnd, bar.getX(), bar.getBottom(), false});
    graphics.fillRoundedRectangle(bar, 1.5F);
    graphics.setColour(workbenchTheme.primaryText);
    graphics.setFont(WorkbenchType::panelTitle());
    graphics.drawText(title, area, juce::Justification::centredLeft);
}

void WorkbenchStyle::paintWell(juce::Graphics& graphics, juce::Rectangle<float> area, float radius)
{
    if (area.isEmpty())
        return;
    const auto shape = roundedPath(area, radius);
    graphics.setGradientFill(juce::ColourGradient{workbenchTheme.well, area.getX(), area.getY(),
        workbenchTheme.well.interpolatedWith(workbenchTheme.surface, 0.35F), area.getX(), area.getBottom(), false});
    graphics.fillPath(shape);
    if (!enhancedContrast())
    {
        // An inner shadow along the top edge sinks the well into the glass.
        juce::Graphics::ScopedSaveState clipped(graphics);
        graphics.reduceClipRegion(shape);
        graphics.setGradientFill(juce::ColourGradient{juce::Colours::black.withAlpha(0.35F), area.getX(), area.getY(),
            juce::Colours::black.withAlpha(0.0F), area.getX(), area.getY() + 8.0F, false});
        graphics.fillRect(area.withHeight(8.0F));
    }
    graphics.setColour(enhancedContrast() ? panelBorderColour : juce::Colours::white.withAlpha(0.06F));
    graphics.strokePath(roundedPath(area.reduced(0.5F), radius), juce::PathStrokeType{1.0F});
}

void WorkbenchStyle::paintTile(juce::Graphics& graphics, juce::Rectangle<float> area, float radius,
    juce::Colour base, bool raised)
{
    if (area.isEmpty())
        return;
    const auto shape = roundedPath(area, radius);
    graphics.setGradientFill(juce::ColourGradient{base.interpolatedWith(juce::Colours::white, raised ? 0.06F : 0.02F),
        area.getX(), area.getY(), base.interpolatedWith(juce::Colours::black, raised ? 0.12F : 0.04F), area.getX(),
        area.getBottom(), false});
    graphics.fillPath(shape);
    if (raised && !enhancedContrast())
    {
        graphics.setColour(juce::Colours::white.withAlpha(0.07F));
        graphics.fillRect(juce::Rectangle<float>{area.getX() + radius, area.getY() + 1.0F,
            std::max(0.0F, area.getWidth() - 2.0F * radius), 1.0F});
    }
    graphics.setColour(enhancedContrast() ? panelBorderColour : juce::Colours::white.withAlpha(0.08F));
    graphics.strokePath(roundedPath(area.reduced(0.5F), radius), juce::PathStrokeType{1.0F});
}

void WorkbenchStyle::strokeWithGlow(juce::Graphics& graphics, const juce::Path& path, juce::Colour colour,
    float width, float glowWidth)
{
    if (!enhancedContrast() && glowWidth > width)
    {
        const auto alpha = colour.getFloatAlpha();
        graphics.setColour(colour.withAlpha(0.10F * alpha));
        graphics.strokePath(path, juce::PathStrokeType{glowWidth, juce::PathStrokeType::curved,
            juce::PathStrokeType::rounded});
        graphics.setColour(colour.withAlpha(0.18F * alpha));
        graphics.strokePath(path, juce::PathStrokeType{(glowWidth + width) * 0.5F, juce::PathStrokeType::curved,
            juce::PathStrokeType::rounded});
    }
    graphics.setColour(colour);
    graphics.strokePath(path, juce::PathStrokeType{width, juce::PathStrokeType::curved,
        juce::PathStrokeType::rounded});
}

void WorkbenchStyle::paintDisplayGrid(juce::Graphics& graphics, juce::Rectangle<float> area, int columns, int rows)
{
    if (area.isEmpty())
        return;
    graphics.setColour(juce::Colours::white.withAlpha(0.045F));
    for (int column = 1; column < columns; ++column)
    {
        const auto x = area.getX() + area.getWidth() * static_cast<float>(column) / static_cast<float>(columns);
        graphics.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
    }
    for (int row = 1; row < rows; ++row)
    {
        const auto y = area.getY() + area.getHeight() * static_cast<float>(row) / static_cast<float>(rows);
        graphics.setColour(juce::Colours::white.withAlpha(row * 2 == rows ? 0.09F : 0.045F));
        graphics.drawHorizontalLine(juce::roundToInt(y), area.getX(), area.getRight());
    }
}

juce::Colour WorkbenchStyle::categoryColour(std::string_view category)
{
    const auto is = [&](std::initializer_list<std::string_view> names) {
        return std::ranges::find(names, category) != names.end();
    };
    if (category.find("Spectral") != std::string_view::npos || is({"CDP", "Frequency & Time Scaling"}))
        return juce::Colour{0xffff5fa8};
    if (is({"Sampling", "Tracking Live Input"}))
        return juce::Colour{0xffff8a5b};
    if (is({"Filters"}))
        return juce::Colour{0xff2fd3f0};
    if (is({"Effects", "Reverb, Delay, Feedback", "Distortion & Waveshaping", "Level, Compression, Expansion",
            "Spatializing"}))
        return juce::Colour{0xff6a8dff};
    if (is({"Mixing", "Mixing & Panning"}))
        return juce::Colour{0xff3ddc97};
    if (is({"Envelopes & Control Signals", "Control", "Control Utilities", "Sequencers", "Time & Duration"}))
        return juce::Colour{0xffffb84d};
    if (is({"Math", "Scripts", "Variables", "Variables & Annotation", "Annotation", "Display"}))
        return juce::Colour{0xffb5e853};
    if (is({"MIDI In", "MIDI Out", "IO"}))
        return juce::Colour{0xff8ec5ff};
    if (is({"Sources", "Oscillators", "Generators", "Xtra Sources", "Aggregate Synthesis", "Xtra"}))
        return juce::Colour{0xffa47dff};
    return juce::Colour{0xff8c95b4};
}

WorkbenchLookAndFeel::WorkbenchLookAndFeel()
{
    const auto& theme = workbenchTheme;
    setColourScheme(juce::LookAndFeel_V4::ColourScheme{
        theme.canvas, theme.surface, theme.well, theme.widgetOutline, theme.primaryText,
        theme.accentStart, theme.accentText, theme.accentStart, theme.primaryText});
    setColour(juce::ResizableWindow::backgroundColourId, theme.canvas);
    setColour(juce::DocumentWindow::textColourId, theme.primaryText);
    setColour(juce::TextButton::buttonColourId, theme.widgetSurface);
    setColour(juce::TextButton::buttonOnColourId, theme.widgetHighlight);
    setColour(juce::TextButton::textColourOffId, theme.widgetText);
    setColour(juce::TextButton::textColourOnId, theme.accentText);
    setColour(juce::ComboBox::backgroundColourId, theme.widgetSurface);
    setColour(juce::ComboBox::outlineColourId, theme.widgetOutline);
    setColour(juce::ComboBox::textColourId, theme.widgetText);
    setColour(juce::ComboBox::arrowColourId, theme.secondaryText);
    setColour(juce::ComboBox::focusedOutlineColourId, theme.focus);
    setColour(juce::TextEditor::backgroundColourId, theme.well);
    setColour(juce::TextEditor::outlineColourId, theme.widgetOutline);
    setColour(juce::TextEditor::focusedOutlineColourId, theme.focus);
    setColour(juce::TextEditor::textColourId, theme.widgetText);
    setColour(juce::TextEditor::highlightColourId, theme.accentStart.withAlpha(0.42F));
    setColour(juce::TextEditor::highlightedTextColourId, theme.primaryText);
    setColour(juce::CaretComponent::caretColourId, theme.accentEnd);
    setColour(juce::Label::textColourId, theme.primaryText);
    setColour(juce::ListBox::backgroundColourId, theme.surface);
    setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, juce::Colours::white.withAlpha(0.22F));
    setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    setColour(juce::Slider::thumbColourId, theme.widgetFill);
    setColour(juce::Slider::trackColourId, theme.widgetOutline);
    setColour(juce::Slider::backgroundColourId, theme.well);
    setColour(juce::Slider::rotarySliderFillColourId, theme.accentStart);
    setColour(juce::Slider::rotarySliderOutlineColourId, theme.well);
    setColour(juce::Slider::textBoxBackgroundColourId, theme.well);
    setColour(juce::Slider::textBoxOutlineColourId, theme.outline);
    setColour(juce::Slider::textBoxTextColourId, theme.primaryText);
    setColour(juce::ToggleButton::textColourId, theme.widgetText);
    setColour(juce::ToggleButton::tickColourId, theme.widgetFill);
    setColour(juce::ToggleButton::tickDisabledColourId, theme.widgetOutline);
    setColour(juce::PopupMenu::backgroundColourId, theme.well.interpolatedWith(theme.surface, 0.6F));
    setColour(juce::PopupMenu::textColourId, theme.primaryText);
    setColour(juce::PopupMenu::headerTextColourId, theme.secondaryText);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, theme.accentStart);
    setColour(juce::PopupMenu::highlightedTextColourId, theme.accentText);
    setColour(juce::AlertWindow::backgroundColourId, theme.surface);
    setColour(juce::AlertWindow::textColourId, theme.primaryText);
    setColour(juce::AlertWindow::outlineColourId, theme.interactiveOutline);
    setColour(juce::ProgressBar::backgroundColourId, theme.well);
    setColour(juce::ProgressBar::foregroundColourId, theme.accentStart);
    setColour(juce::TabbedButtonBar::tabOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TabbedButtonBar::frontOutlineColourId, juce::Colours::transparentBlack);
    setColour(juce::TabbedButtonBar::tabTextColourId, theme.secondaryText);
    setColour(juce::TabbedButtonBar::frontTextColourId, theme.primaryText);
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
    enhancedContrast_ = enhancedContrast;
    workbenchFocusOutlinePixels.store(
        focusOutlinePixels_, std::memory_order_relaxed);
    workbenchEnhancedContrast.store(enhancedContrast, std::memory_order_relaxed);
    panelBorderColour = enhancedContrast
        ? workbenchTheme.interactiveOutline
        : workbenchTheme.outline;
    const auto outline = enhancedContrast
        ? workbenchTheme.enhancedOutline
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

juce::Font WorkbenchLookAndFeel::getPopupMenuFont()
{
    return juce::Font{juce::FontOptions{std::max(13.0F, 13.0F * WorkbenchType::interfaceScale())}};
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
    // A slider marked as a readout shows its value as a lit LCD.
    if (slider.getProperties()["talasReadout"])
    {
        label->getProperties().set("talasReadout", true);
        label->setFont(WorkbenchType::data().boldened());
    }
    return label;
}

// A raised glass control; toggled on, it fills with the signature gradient
// and its text turns to ink. A colour a button chose for itself is kept.
void WorkbenchLookAndFeel::drawButtonBackground(
    juce::Graphics& graphics,
    juce::Button& button,
    const juce::Colour& colour,
    bool highlighted,
    bool down)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced(0.5F);
    const auto radius = std::min(WorkbenchStyle::controlRadius(), bounds.getHeight() * 0.5F);
    const auto& properties = button.getProperties();
    // A segment sits clear over its control's tray; the chosen one is a
    // gradient pill inset in it.
    if (properties[talasSegmentProperty])
    {
        const auto pill = bounds.reduced(2.5F);
        const auto pillRadius = std::max(2.0F, radius - 2.0F);
        if (button.getToggleState())
        {
            auto gradient = WorkbenchStyle::accentGradient(pill, button.isEnabled() ? 1.0F : 0.45F);
            if (highlighted || down)
            {
                for (int index = 0; index < gradient.getNumColours(); ++index)
                    gradient.setColour(index, gradient.getColour(index).brighter(down ? -0.1F : 0.1F));
            }
            graphics.setGradientFill(gradient);
            graphics.fillRoundedRectangle(pill, pillRadius);
            if (!enhancedContrast_)
            {
                graphics.setColour(juce::Colours::white.withAlpha(0.3F));
                graphics.fillRect(juce::Rectangle<float>{pill.getX() + pillRadius, pill.getY() + 1.0F,
                    std::max(0.0F, pill.getWidth() - 2.0F * pillRadius), 1.0F});
            }
        }
        else if (highlighted || down)
        {
            graphics.setColour(juce::Colours::white.withAlpha(down ? 0.04F : 0.07F));
            graphics.fillRoundedRectangle(pill, pillRadius);
        }
        drawFocusOutline(graphics, button, radius + 1.0F);
        return;
    }
    const auto shape = roundedPath(bounds, radius, button.isConnectedOnLeft(), button.isConnectedOnRight(),
        button.isConnectedOnTop(), button.isConnectedOnBottom());
    // A running transport stays lit whether or not it can be pressed again.
    const auto lit = static_cast<bool>(properties[talasLitProperty]);
    const auto enabledAlpha = button.isEnabled() || lit ? 1.0F : 0.45F;
    const auto accent = lit || (button.getToggleState() && colour == findColour(juce::TextButton::buttonOnColourId));
    if (accent)
    {
        auto gradient = WorkbenchStyle::accentGradient(bounds, enabledAlpha);
        if (highlighted || down)
        {
            for (int index = 0; index < gradient.getNumColours(); ++index)
                gradient.setColour(index, gradient.getColour(index).brighter(down ? -0.1F : 0.12F));
        }
        graphics.setGradientFill(gradient);
        graphics.fillPath(shape);
        if (!enhancedContrast_)
        {
            graphics.setColour(juce::Colours::white.withAlpha(0.28F * enabledAlpha));
            graphics.fillRect(juce::Rectangle<float>{bounds.getX() + radius, bounds.getY() + 1.0F,
                std::max(0.0F, bounds.getWidth() - 2.0F * radius), 1.0F});
        }
    }
    else
    {
        auto base = colour;
        if (down)
            base = base.darker(0.25F);
        else if (highlighted)
            base = base.brighter(0.18F);
        base = base.withMultipliedAlpha(enabledAlpha);
        graphics.setGradientFill(juce::ColourGradient{base.interpolatedWith(juce::Colours::white, 0.07F), bounds.getX(),
            bounds.getY(), base.interpolatedWith(juce::Colours::black, 0.16F), bounds.getX(), bounds.getBottom(), false});
        graphics.fillPath(shape);
        if (!enhancedContrast_ && !down)
        {
            graphics.setColour(juce::Colours::white.withAlpha(0.09F * enabledAlpha));
            graphics.fillRect(juce::Rectangle<float>{bounds.getX() + radius, bounds.getY() + 1.0F,
                std::max(0.0F, bounds.getWidth() - 2.0F * radius), 1.0F});
        }
    }
    const auto edge = enhancedContrast_
        ? button.findColour(juce::ComboBox::outlineColourId)
        : juce::Colours::white.withAlpha(highlighted ? 0.2F : (accent ? 0.0F : 0.085F));
    graphics.setColour(edge.withMultipliedAlpha(enabledAlpha));
    graphics.strokePath(shape, juce::PathStrokeType{1.0F});
    drawFocusOutline(graphics, button, radius + 1.0F);
}

void WorkbenchLookAndFeel::drawButtonText(
    juce::Graphics& graphics,
    juce::TextButton& button,
    bool highlighted,
    bool down)
{
    const auto& properties = button.getProperties();
    const auto lit = static_cast<bool>(properties[talasLitProperty]);
    const auto glyphName = properties[talasGlyphProperty].toString();
    const auto segment = static_cast<bool>(properties[talasSegmentProperty]);
    if (glyphName.isEmpty() && !lit && !segment)
    {
        juce::LookAndFeel_V4::drawButtonText(graphics, button, highlighted, down);
        return;
    }
    const auto on = lit || button.getToggleState();
    auto colour = on ? button.findColour(juce::TextButton::textColourOnId)
                     : (segment && !highlighted ? workbenchTheme.secondaryText
                                                : button.findColour(juce::TextButton::textColourOffId));
    colour = colour.withMultipliedAlpha(button.isEnabled() || lit ? 1.0F : 0.5F);
    const auto font = getTextButtonFont(button, button.getHeight());
    const auto text = button.getButtonText();
    auto area = button.getLocalBounds().reduced(6, 2).toFloat();
    if (glyphName.isEmpty())
    {
        graphics.setColour(colour);
        graphics.setFont(font);
        graphics.drawFittedText(text, area.toNearestInt(), juce::Justification::centred, 1);
        return;
    }
    const auto glyph = glyphName == "pause" ? WorkbenchStyle::Glyph::Pause
        : glyphName == "stop"               ? WorkbenchStyle::Glyph::Stop
        : glyphName == "record"             ? WorkbenchStyle::Glyph::Record
                                            : WorkbenchStyle::Glyph::Play;
    const auto glyphSize = std::round(font.getHeight() * 0.74F);
    const auto gap = std::round(font.getHeight() * 0.45F);
    const auto textWidth = std::min(area.getWidth() - glyphSize - gap,
        juce::GlyphArrangement::getStringWidth(font, text));
    const auto total = glyphSize + (text.isEmpty() ? 0.0F : gap + textWidth);
    auto row = area.withSizeKeepingCentre(std::min(total, area.getWidth()), area.getHeight());
    const auto glyphArea = row.removeFromLeft(glyphSize);
    WorkbenchStyle::paintGlyph(graphics,
        glyphArea.withSizeKeepingCentre(glyphSize, glyphSize),
        glyph,
        glyph == WorkbenchStyle::Glyph::Record && !on ? workbenchTheme.error.withMultipliedAlpha(colour.getFloatAlpha())
                                                      : colour);
    if (text.isEmpty())
        return;
    row.removeFromLeft(gap);
    graphics.setColour(colour);
    graphics.setFont(font);
    graphics.drawFittedText(text, row.toNearestInt(), juce::Justification::centredLeft, 1, 0.8F);
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

// A rounded box, a gradient fill and an ink tick when on: the tick, not the
// colour, carries the state.
void WorkbenchLookAndFeel::drawTickBox(
    juce::Graphics& graphics,
    juce::Component& component,
    float x,
    float y,
    float width,
    float height,
    bool ticked,
    bool enabled,
    bool highlighted,
    bool down)
{
    juce::ignoreUnused(down);
    const auto box = juce::Rectangle<float>{x, y, width, height}.reduced(0.5F);
    const auto alpha = enabled ? 1.0F : 0.45F;
    const auto radius = std::min(4.0F, box.getHeight() * 0.3F);
    if (ticked)
    {
        graphics.setGradientFill(WorkbenchStyle::accentGradient(box, alpha));
        graphics.fillRoundedRectangle(box, radius);
        auto tick = getTickShape(0.75F);
        graphics.setColour(workbenchTheme.accentText.withMultipliedAlpha(alpha));
        graphics.fillPath(tick, tick.getTransformToScaleToFit(box.reduced(box.getWidth() * 0.22F), true));
    }
    else
    {
        graphics.setColour(workbenchTheme.well.withMultipliedAlpha(alpha));
        graphics.fillRoundedRectangle(box, radius);
    }
    const auto outline = enhancedContrast_
        ? component.findColour(juce::ToggleButton::tickDisabledColourId)
        : (highlighted ? workbenchTheme.accentEnd.withAlpha(0.8F) : workbenchTheme.interactiveOutline);
    graphics.setColour((ticked && !enhancedContrast_ ? juce::Colours::white.withAlpha(0.18F) : outline)
                           .withMultipliedAlpha(alpha));
    graphics.drawRoundedRectangle(box, radius, 1.0F);
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
    juce::ignoreUnused(buttonX, buttonY, buttonHeight);
    const auto bounds = juce::Rectangle<float>{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)}
                            .reduced(0.5F);
    const auto radius = std::min(WorkbenchStyle::controlRadius(), bounds.getHeight() * 0.5F);
    const auto alpha = box.isEnabled() ? 1.0F : 0.45F;
    const auto base = box.findColour(juce::ComboBox::backgroundColourId).withMultipliedAlpha(alpha);
    graphics.setGradientFill(juce::ColourGradient{base.interpolatedWith(juce::Colours::white, 0.05F), 0.0F,
        bounds.getY(), base.interpolatedWith(juce::Colours::black, 0.14F), 0.0F, bounds.getBottom(), false});
    graphics.fillRoundedRectangle(bounds, radius);
    const auto outline = enhancedContrast_ ? box.findColour(juce::ComboBox::outlineColourId)
                                           : (down || box.isPopupActive() ? workbenchTheme.accentStart
                                                                          : juce::Colours::white.withAlpha(0.09F));
    graphics.setColour(outline.withMultipliedAlpha(alpha));
    graphics.drawRoundedRectangle(bounds, radius, 1.0F);
    const auto arrowWidth = static_cast<float>(std::min(buttonWidth, 20));
    drawChevron(graphics,
        juce::Rectangle<float>{bounds.getRight() - arrowWidth - 4.0F, bounds.getY(), arrowWidth, bounds.getHeight()},
        box.findColour(juce::ComboBox::arrowColourId).withMultipliedAlpha(alpha));
    drawFocusOutline(graphics, box, radius + 1.0F);
}

// A recessed track, its filled part in the signature gradient, and a bright
// thumb with a soft glow.
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
    const auto plain = style == juce::Slider::LinearHorizontal || style == juce::Slider::LinearVertical;
    if (!plain && !slider.isBar())
    {
        juce::LookAndFeel_V4::drawLinearSlider(
            graphics, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        drawFocusOutline(graphics, slider, 3.0F);
        return;
    }
    const auto alpha = slider.isEnabled() ? 1.0F : 0.45F;
    const auto area = juce::Rectangle<float>{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
        static_cast<float>(height)};
    if (slider.isBar())
    {
        WorkbenchStyle::paintWell(graphics, area.reduced(0.5F), 3.0F);
        const auto filled = slider.isHorizontal()
            ? area.withRight(sliderPos).reduced(1.5F)
            : area.withTop(sliderPos).reduced(1.5F);
        graphics.setGradientFill(WorkbenchStyle::accentGradient(area, alpha));
        graphics.fillRoundedRectangle(filled, 2.0F);
        drawFocusOutline(graphics, slider, 3.0F);
        return;
    }
    const auto horizontal = slider.isHorizontal();
    const auto trackThickness = std::max(3.0F, std::min(5.0F, (horizontal ? area.getHeight() : area.getWidth()) * 0.22F));
    const auto track = horizontal
        ? juce::Rectangle<float>{area.getX(), area.getCentreY() - trackThickness * 0.5F, area.getWidth(), trackThickness}
        : juce::Rectangle<float>{area.getCentreX() - trackThickness * 0.5F, area.getY(), trackThickness, area.getHeight()};
    graphics.setColour(workbenchTheme.well.withMultipliedAlpha(alpha));
    graphics.fillRoundedRectangle(track, trackThickness * 0.5F);
    graphics.setColour((enhancedContrast_ ? slider.findColour(juce::Slider::trackColourId)
                                          : juce::Colours::white.withAlpha(0.07F))
                           .withMultipliedAlpha(alpha));
    graphics.drawRoundedRectangle(track, trackThickness * 0.5F, 1.0F);
    const auto filled = horizontal ? track.withRight(sliderPos) : track.withTop(sliderPos);
    graphics.setGradientFill(WorkbenchStyle::accentGradient(track, alpha));
    graphics.fillRoundedRectangle(filled, trackThickness * 0.5F);
    const auto thumbSize = std::min(14.0F, (horizontal ? area.getHeight() : area.getWidth()) * 0.9F);
    const auto thumbCentre = horizontal ? juce::Point<float>{sliderPos, track.getCentreY()}
                                        : juce::Point<float>{track.getCentreX(), sliderPos};
    const auto thumb = juce::Rectangle<float>{thumbSize, thumbSize}.withCentre(thumbCentre);
    if (!enhancedContrast_)
    {
        graphics.setColour(WorkbenchStyle::accentAt(horizontal ? (sliderPos - area.getX()) / std::max(1.0F, area.getWidth())
                                                               : 1.0F - (sliderPos - area.getY()) / std::max(1.0F, area.getHeight()))
                               .withAlpha(0.35F * alpha));
        graphics.fillEllipse(thumb.expanded(3.0F));
    }
    graphics.setColour(workbenchTheme.primaryText.withMultipliedAlpha(alpha));
    graphics.fillEllipse(thumb);
    graphics.setColour(workbenchTheme.accentStart.withMultipliedAlpha(alpha));
    graphics.drawEllipse(thumb.reduced(0.5F), 1.5F);
    drawFocusOutline(graphics, slider, 3.0F);
}

// A recessed ring, the value's arc in the signature gradient, a lit pointer.
void WorkbenchLookAndFeel::drawRotarySlider(
    juce::Graphics& graphics,
    int x,
    int y,
    int width,
    int height,
    float sliderPosProportional,
    float rotaryStartAngle,
    float rotaryEndAngle,
    juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float>{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
        static_cast<float>(height)};
    WorkbenchStyle::paintKnob(
        graphics, bounds, sliderPosProportional, rotaryStartAngle, rotaryEndAngle, slider.isEnabled());
    drawFocusOutline(graphics, slider, std::min(8.0F, std::min(bounds.getWidth(), bounds.getHeight()) * 0.5F));
}

void WorkbenchLookAndFeel::drawScrollbar(
    juce::Graphics& graphics,
    juce::ScrollBar& scrollbar,
    int x,
    int y,
    int width,
    int height,
    bool isScrollbarVertical,
    int thumbStartPosition,
    int thumbSize,
    bool isMouseOver,
    bool isMouseDown)
{
    juce::ignoreUnused(scrollbar);
    if (thumbSize <= 0)
        return;
    auto thumb = isScrollbarVertical
        ? juce::Rectangle<float>{static_cast<float>(x), static_cast<float>(thumbStartPosition), static_cast<float>(width),
              static_cast<float>(thumbSize)}
        : juce::Rectangle<float>{static_cast<float>(thumbStartPosition), static_cast<float>(y),
              static_cast<float>(thumbSize), static_cast<float>(height)};
    const auto thickness = std::min(isMouseOver || isMouseDown ? 7.0F : 5.0F,
        isScrollbarVertical ? thumb.getWidth() : thumb.getHeight());
    thumb = isScrollbarVertical ? thumb.withSizeKeepingCentre(thickness, thumb.getHeight() - 2.0F)
                                : thumb.withSizeKeepingCentre(thumb.getWidth() - 2.0F, thickness);
    if (isMouseDown)
        graphics.setGradientFill(WorkbenchStyle::accentGradient(thumb));
    else
        graphics.setColour(enhancedContrast_ ? workbenchTheme.enhancedOutline
                                             : juce::Colours::white.withAlpha(isMouseOver ? 0.36F : 0.2F));
    graphics.fillRoundedRectangle(thumb, thickness * 0.5F);
}

void WorkbenchLookAndFeel::fillTextEditorBackground(
    juce::Graphics& graphics,
    int width,
    int height,
    juce::TextEditor& editor)
{
    const auto colour = editor.findColour(juce::TextEditor::backgroundColourId);
    if (colour.isTransparent())
        return;
    const auto bounds = juce::Rectangle<float>{static_cast<float>(width), static_cast<float>(height)};
    const auto radius = std::min(WorkbenchStyle::controlRadius() - 1.0F, bounds.getHeight() * 0.5F);
    if (colour == findColour(juce::TextEditor::backgroundColourId))
    {
        WorkbenchStyle::paintWell(graphics, bounds, radius);
        return;
    }
    graphics.setColour(colour);
    graphics.fillRoundedRectangle(bounds, radius);
}

// The keyboard focus keeps its strong, wide outline; at rest a field shows
// only a faint edge.
void WorkbenchLookAndFeel::drawTextEditorOutline(
    juce::Graphics& graphics,
    int width,
    int height,
    juce::TextEditor& editor)
{
    if (!editor.isEnabled())
        return;
    const auto bounds = juce::Rectangle<float>{static_cast<float>(width), static_cast<float>(height)};
    const auto radius = std::min(WorkbenchStyle::controlRadius() - 1.0F, bounds.getHeight() * 0.5F);
    if (editor.hasKeyboardFocus(true) && !editor.isReadOnly())
    {
        const auto focus = editor.findColour(juce::TextEditor::focusedOutlineColourId);
        if (!enhancedContrast_)
        {
            graphics.setColour(focus.withAlpha(0.22F));
            graphics.drawRoundedRectangle(bounds.reduced(0.5F), radius, focusOutlinePixels_ + 2.0F);
        }
        graphics.setColour(focus);
        graphics.drawRoundedRectangle(bounds.reduced(focusOutlinePixels_ * 0.5F), radius, focusOutlinePixels_);
        return;
    }
    const auto outline = editor.findColour(juce::TextEditor::outlineColourId);
    if (outline.isTransparent())
        return;
    graphics.setColour(enhancedContrast_ ? outline : outline.withMultipliedAlpha(0.55F));
    graphics.drawRoundedRectangle(bounds.reduced(0.5F), radius, 1.0F);
}

void WorkbenchLookAndFeel::drawPopupMenuBackground(
    juce::Graphics& graphics,
    int width,
    int height)
{
    const auto bounds = juce::Rectangle<float>{static_cast<float>(width), static_cast<float>(height)};
    const auto base = findColour(juce::PopupMenu::backgroundColourId);
    graphics.setGradientFill(juce::ColourGradient{base.interpolatedWith(juce::Colours::white, 0.03F), 0.0F, 0.0F,
        base.interpolatedWith(juce::Colours::black, 0.1F), 0.0F, bounds.getBottom(), false});
    graphics.fillRect(bounds);
    graphics.setColour(enhancedContrast_ ? workbenchTheme.enhancedOutline : juce::Colours::white.withAlpha(0.1F));
    graphics.drawRect(bounds, 1.0F);
}

void WorkbenchLookAndFeel::drawPopupMenuItem(
    juce::Graphics& graphics,
    const juce::Rectangle<int>& area,
    bool isSeparator,
    bool isActive,
    bool isHighlighted,
    bool isTicked,
    bool hasSubMenu,
    const juce::String& text,
    const juce::String& shortcutKeyText,
    const juce::Drawable* icon,
    const juce::Colour* textColourToUse)
{
    if (isSeparator)
    {
        auto line = area.reduced(8, 0).toFloat();
        graphics.setColour(juce::Colours::white.withAlpha(0.1F));
        graphics.fillRect(line.withY(line.getCentreY() - 0.5F).withHeight(1.0F));
        return;
    }
    auto row = area.reduced(4, 1);
    auto textColour = textColourToUse == nullptr ? findColour(juce::PopupMenu::textColourId) : *textColourToUse;
    if (isHighlighted && isActive)
    {
        graphics.setGradientFill(WorkbenchStyle::accentGradient(row.toFloat()));
        graphics.fillRoundedRectangle(row.toFloat(), 4.0F);
        textColour = findColour(juce::PopupMenu::highlightedTextColourId);
    }
    else if (!isActive)
        textColour = textColour.withMultipliedAlpha(0.45F);
    graphics.setColour(textColour);
    row.reduce(std::min(6, area.getWidth() / 20), 0);
    auto font = getPopupMenuFont();
    const auto maxFontHeight = static_cast<float>(row.getHeight()) / 1.3F;
    if (font.getHeight() > maxFontHeight)
        font.setHeight(maxFontHeight);
    graphics.setFont(font);
    const auto iconArea = row.removeFromLeft(juce::roundToInt(maxFontHeight)).toFloat();
    if (icon != nullptr)
    {
        icon->drawWithin(graphics, iconArea, juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize,
            1.0F);
        row.removeFromLeft(juce::roundToInt(maxFontHeight * 0.5F));
    }
    else if (isTicked)
    {
        auto tick = getTickShape(1.0F);
        graphics.setColour(isHighlighted && isActive ? textColour : workbenchTheme.accentEnd);
        graphics.fillPath(tick, tick.getTransformToScaleToFit(iconArea.reduced(iconArea.getWidth() / 5, 0), true));
        graphics.setColour(textColour);
    }
    if (hasSubMenu)
    {
        const auto arrowHeight = 0.6F * font.getAscent();
        const auto arrowX = static_cast<float>(row.removeFromRight(juce::roundToInt(arrowHeight)).getX());
        const auto halfHeight = static_cast<float>(row.getCentreY());
        juce::Path arrow;
        arrow.startNewSubPath(arrowX, halfHeight - arrowHeight * 0.5F);
        arrow.lineTo(arrowX + arrowHeight * 0.6F, halfHeight);
        arrow.lineTo(arrowX, halfHeight + arrowHeight * 0.5F);
        graphics.strokePath(arrow, juce::PathStrokeType{1.6F});
    }
    row.removeFromRight(3);
    graphics.drawFittedText(text, row, juce::Justification::centredLeft, 1);
    if (shortcutKeyText.isNotEmpty())
    {
        auto shortcutFont = font;
        shortcutFont.setHeight(font.getHeight() * 0.8F);
        graphics.setFont(shortcutFont);
        graphics.setColour(textColour.withMultipliedAlpha(isHighlighted && isActive ? 1.0F : 0.7F));
        graphics.drawText(shortcutKeyText, row, juce::Justification::centredRight, true);
    }
}

// Fly-by help stays a yellow box with dark text, rounded and lit like the
// rest of the glass.
void WorkbenchLookAndFeel::drawTooltip(
    juce::Graphics& graphics,
    const juce::String& text,
    int width,
    int height)
{
    const auto bounds = juce::Rectangle<float>{static_cast<float>(width), static_cast<float>(height)};
    const auto box = findColour(juce::TooltipWindow::backgroundColourId);
    graphics.setGradientFill(juce::ColourGradient{box.brighter(0.06F), 0.0F, 0.0F, box.darker(0.04F), 0.0F,
        bounds.getBottom(), false});
    graphics.fillRoundedRectangle(bounds, 5.0F);
    graphics.setColour(findColour(juce::TooltipWindow::outlineColourId));
    graphics.drawRoundedRectangle(bounds.reduced(0.5F), 5.0F, 1.0F);
    juce::AttributedString attributed;
    attributed.setJustification(juce::Justification::centredLeft);
    attributed.append(text, WorkbenchType::label(), findColour(juce::TooltipWindow::textColourId));
    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths(attributed, static_cast<float>(width) - 12.0F);
    layout.draw(graphics, bounds.reduced(6.0F, 4.0F));
}

// Tabs read as a segmented strip: the front tab lit with the gradient
// underline and bright text, the others quiet.
void WorkbenchLookAndFeel::drawTabButton(
    juce::TabBarButton& button,
    juce::Graphics& graphics,
    bool isMouseOver,
    bool isMouseDown)
{
    juce::ignoreUnused(isMouseDown);
    const auto area = button.getActiveArea().toFloat().reduced(1.0F, 2.0F);
    const auto front = button.isFrontTab();
    if (front || isMouseOver)
    {
        graphics.setColour(juce::Colours::white.withAlpha(front ? 0.07F : 0.04F));
        graphics.fillRoundedRectangle(area, 4.0F);
    }
    if (front)
    {
        graphics.setGradientFill(WorkbenchStyle::accentGradient(area));
        graphics.fillRoundedRectangle(area.withTop(area.getBottom() - 2.0F), 1.0F);
    }
    const auto colour = button.getTabBackgroundColour();
    if (!colour.isTransparent() && colour != juce::Colours::white && !front)
    {
        graphics.setColour(colour.withAlpha(0.6F));
        graphics.fillRect(area.withTop(area.getBottom() - 2.0F));
    }
    graphics.setColour(findColour(front ? juce::TabbedButtonBar::frontTextColourId
                                        : juce::TabbedButtonBar::tabTextColourId));
    graphics.setFont(front ? WorkbenchType::bodyStrong() : WorkbenchType::body());
    graphics.drawFittedText(button.getButtonText().trim(), area.toNearestInt().reduced(6, 0),
        juce::Justification::centred, 1);
    drawFocusOutline(graphics, button, 4.0F);
}

void WorkbenchLookAndFeel::drawTabAreaBehindFrontButton(
    juce::TabbedButtonBar& bar,
    juce::Graphics& graphics,
    int width,
    int height)
{
    juce::ignoreUnused(bar);
    graphics.setColour(juce::Colours::white.withAlpha(0.06F));
    graphics.fillRect(0, height - 1, width, 1);
}

void WorkbenchLookAndFeel::drawLabel(juce::Graphics& graphics, juce::Label& label)
{
    if (!label.getProperties()["talasReadout"])
    {
        juce::LookAndFeel_V4::drawLabel(graphics, label);
        return;
    }
    const auto bounds = label.getLocalBounds().toFloat().reduced(0.5F);
    WorkbenchStyle::paintWell(graphics, bounds, std::min(5.0F, bounds.getHeight() * 0.3F));
    if (label.isBeingEdited())
        return;
    const auto colour = label.isEnabled() ? workbenchTheme.accentEnd : workbenchTheme.secondaryText;
    const auto font = getLabelFont(label);
    const auto area = getLabelBorderSize(label).subtractedFrom(label.getLocalBounds());
    graphics.setFont(font);
    if (!enhancedContrast_)
    {
        // The digits' glow, a faint copy spread a pixel each way.
        graphics.setColour(colour.withAlpha(0.22F));
        for (const auto offset : {-1, 1})
        {
            graphics.drawFittedText(label.getText(), area.translated(offset, 0), label.getJustificationType(), 1, 1.0F);
            graphics.drawFittedText(label.getText(), area.translated(0, offset), label.getJustificationType(), 1, 1.0F);
        }
    }
    graphics.setColour(colour);
    graphics.drawFittedText(label.getText(), area, label.getJustificationType(), 1, 1.0F);
}

void WorkbenchLookAndFeel::drawProgressBar(
    juce::Graphics& graphics,
    juce::ProgressBar& bar,
    int width,
    int height,
    double progress,
    const juce::String& textToShow)
{
    const auto bounds = juce::Rectangle<float>{static_cast<float>(width), static_cast<float>(height)};
    const auto radius = std::min(4.0F, bounds.getHeight() * 0.5F);
    WorkbenchStyle::paintWell(graphics, bounds, radius);
    if (progress >= 0.0 && progress <= 1.0)
    {
        graphics.setGradientFill(WorkbenchStyle::accentGradient(bounds));
        graphics.fillRoundedRectangle(bounds.reduced(1.5F).withWidth(
                                          std::max(0.0F, (bounds.getWidth() - 3.0F) * static_cast<float>(progress))),
            radius);
    }
    else
    {
        // Indeterminate: a soft band of the gradient.
        graphics.setGradientFill(WorkbenchStyle::accentGradient(bounds, 0.45F));
        graphics.fillRoundedRectangle(bounds.reduced(1.5F), radius);
    }
    if (textToShow.isNotEmpty())
    {
        juce::ignoreUnused(bar);
        graphics.setFont(WorkbenchType::labelStrong());
        graphics.setColour(juce::Colours::black.withAlpha(0.6F));
        graphics.drawText(textToShow, bounds.translated(0.0F, 1.0F), juce::Justification::centred, false);
        graphics.setColour(workbenchTheme.primaryText);
        graphics.drawText(textToShow, bounds, juce::Justification::centred, false);
    }
}

// A glass panel over the backdrop, a short gradient line along its top and
// the alert's kind in a lit roundel: information and questions in the
// gradient, a warning in amber; the symbol inside says which.
void WorkbenchLookAndFeel::drawAlertBox(
    juce::Graphics& graphics,
    juce::AlertWindow& alert,
    const juce::Rectangle<int>& textArea,
    juce::TextLayout& textLayout)
{
    juce::ignoreUnused(textArea);
    const auto bounds = alert.getLocalBounds().toFloat();
    WorkbenchStyle::paintBackdrop(graphics, bounds);
    const auto panel = bounds.reduced(1.0F);
    WorkbenchStyle::paintPanel(graphics, panel, WorkbenchStyle::panelRadius());
    graphics.setGradientFill(WorkbenchStyle::accentGradient(panel.withHeight(2.0F)));
    graphics.fillRect(panel.reduced(WorkbenchStyle::panelRadius(), 0.0F).withHeight(2.0F));
    graphics.setColour(enhancedContrast_ ? workbenchTheme.enhancedOutline : workbenchTheme.interactiveOutline.withAlpha(0.6F));
    graphics.drawRoundedRectangle(panel, WorkbenchStyle::panelRadius(), 1.0F);

    auto iconSpace = 0;
    if (alert.getAlertType() != juce::MessageBoxIconType::NoIcon)
    {
        const auto warning = alert.getAlertType() == juce::MessageBoxIconType::WarningIcon;
        const auto icon = juce::Rectangle<float>{24.0F, 26.0F, 40.0F, 40.0F};
        if (warning)
        {
            if (!enhancedContrast_)
            {
                graphics.setColour(workbenchTheme.control.withAlpha(0.22F));
                graphics.fillEllipse(icon.expanded(3.0F));
            }
            graphics.setColour(workbenchTheme.control);
            graphics.fillEllipse(icon);
        }
        else
        {
            if (!enhancedContrast_)
            {
                graphics.setGradientFill(WorkbenchStyle::accentGradient(icon.expanded(3.0F), 0.22F));
                graphics.fillEllipse(icon.expanded(3.0F));
            }
            graphics.setGradientFill(WorkbenchStyle::accentGradient(icon));
            graphics.fillEllipse(icon);
        }
        graphics.setColour(workbenchTheme.accentText);
        graphics.setFont(juce::Font{juce::FontOptions{24.0F}.withStyle("Bold")});
        graphics.drawText(warning ? "!" : alert.getAlertType() == juce::MessageBoxIconType::InfoIcon ? "i" : "?",
            icon, juce::Justification::centred, false);
        iconSpace = 80;
    }
    const auto textBounds = juce::Rectangle<float>{panel.getX() + static_cast<float>(iconSpace),
        28.0F, panel.getWidth() - static_cast<float>(iconSpace) - 10.0F,
        panel.getHeight() - static_cast<float>(getAlertWindowButtonHeight()) - 34.0F};
    textLayout.draw(graphics, textBounds);
}

int WorkbenchLookAndFeel::getAlertWindowButtonHeight()
{
    return juce::roundToInt(30.0F * WorkbenchType::interfaceScale());
}

juce::Font WorkbenchLookAndFeel::getAlertWindowTitleFont()
{
    return juce::Font{juce::FontOptions{16.0F * WorkbenchType::interfaceScale()}.withStyle("Bold")};
}

juce::Font WorkbenchLookAndFeel::getAlertWindowMessageFont()
{
    return juce::Font{juce::FontOptions{13.0F * WorkbenchType::interfaceScale()}};
}

juce::Font WorkbenchLookAndFeel::getAlertWindowFont()
{
    return WorkbenchType::body();
}

void WorkbenchLookAndFeel::drawLevelMeter(juce::Graphics& graphics, int width, int height, float level)
{
    const auto bounds = juce::Rectangle<float>{static_cast<float>(width), static_cast<float>(height)};
    WorkbenchStyle::paintWell(graphics, bounds, std::min(3.0F, bounds.getHeight() * 0.5F));
    // Twelve segments, lit up to the level, each in the meter gradient's
    // colour at its place.
    constexpr int segments = 12;
    const auto inner = bounds.reduced(2.0F);
    const auto segmentWidth = inner.getWidth() / static_cast<float>(segments);
    const auto lit = std::clamp(std::pow(std::max(0.0F, level), 0.5F), 0.0F, 1.0F) * static_cast<float>(segments);
    for (int segment = 0; segment < segments; ++segment)
    {
        const auto at = static_cast<float>(segment) / static_cast<float>(segments - 1);
        const auto colour = at < 0.72F ? workbenchTheme.audio.interpolatedWith(workbenchTheme.control, at / 0.72F)
                                       : workbenchTheme.control.interpolatedWith(workbenchTheme.error, (at - 0.72F) / 0.28F);
        const auto cell = juce::Rectangle<float>{inner.getX() + segmentWidth * static_cast<float>(segment), inner.getY(),
            segmentWidth, inner.getHeight()}
                              .reduced(1.0F, 0.0F);
        graphics.setColour(static_cast<float>(segment) < lit ? colour : colour.withAlpha(0.1F));
        graphics.fillRoundedRectangle(cell, 1.0F);
    }
}

void WorkbenchLookAndFeel::fillResizableWindowBackground(
    juce::Graphics& graphics,
    int width,
    int height,
    const juce::BorderSize<int>& border,
    juce::ResizableWindow& window)
{
    const auto colour = window.getBackgroundColour();
    if (colour != workbenchTheme.surface && colour != workbenchTheme.canvas)
    {
        juce::LookAndFeel_V4::fillResizableWindowBackground(graphics, width, height, border, window);
        return;
    }
    WorkbenchStyle::paintBackdrop(graphics,
        juce::Rectangle<float>{static_cast<float>(width), static_cast<float>(height)});
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
