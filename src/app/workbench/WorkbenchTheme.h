#pragma once

#include "talas/app/TalasKeyEquivalents.h"
#include "talas/app/WorkbenchUiPolicy.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>
#include <string_view>

namespace talas::app::workbench
{
// The Workbench's design tokens. Their values live in WorkbenchTheme.cpp so
// the palette can be tuned without rebuilding every panel.
struct WorkbenchTheme
{
    // The window's deep ink base and the glass panel surface above it.
    juce::Colour canvas;
    juce::Colour surface;
    juce::Colour outline;
    juce::Colour interactiveOutline;
    juce::Colour primaryText;
    juce::Colour secondaryText;
    // A selection is blue.
    juce::Colour focus;
    // Port kinds, always paired with their A/C/E/D prefix.
    juce::Colour audio;
    juce::Colour control;
    juce::Colour event;
    juce::Colour data;
    juce::Colour output;
    juce::Colour error;
    juce::Colour fullSupport;
    juce::Colour subsetSupport;
    juce::Colour placeholderSupport;
    juce::Colour utilitySupport;
    juce::Colour graphGrid;
    juce::Colour selectedRow;
    juce::Colour controlTile;
    juce::Colour widgetSurface;
    juce::Colour widgetOutline;
    juce::Colour widgetText;
    juce::Colour widgetFill;
    juce::Colour widgetHighlight;
    // The backdrop's gradient behind the panels.
    juce::Colour backdropTop;
    juce::Colour backdropBottom;
    // Raised controls (buttons, tiles) and recessed wells (fields, displays).
    juce::Colour surfaceRaised;
    juce::Colour well;
    // The signature gradient and the ink drawn on it.
    juce::Colour accentStart;
    juce::Colour accentEnd;
    juce::Colour accentText;
    // Glass: the soft top highlight, the thin light edge and the shadow.
    juce::Colour glassHighlight;
    juce::Colour glassEdge;
    juce::Colour shadow;
    // The outline the enhanced-contrast preference uses.
    juce::Colour enhancedOutline;
    juce::Colour success;
    juce::Colour warning;
};

extern const WorkbenchTheme workbenchTheme;
extern const juce::Colour& backgroundColour;
extern const juce::Colour& panelColour;
extern juce::Colour panelBorderColour;
extern const juce::Colour& textColour;
extern const juce::Colour& secondaryTextColour;
extern const juce::Colour& selectionColour;
extern const juce::Colour& audioColour;
extern const juce::Colour& controlColour;
extern const juce::Colour& eventColour;
extern const juce::Colour& dataColour;
extern const juce::Colour& outputColour;
extern const juce::Colour& errorColour;

struct WorkbenchType
{
    static void setInterfaceScale(float scale) noexcept;
    // The selection's colour, blue or gray, as the preferences ask; focus
    // outlines stay blue.
    static void setGraySelection(bool gray) noexcept;
    [[nodiscard]] static float interfaceScale() noexcept;
    [[nodiscard]] static float focusOutlinePixels() noexcept;
    [[nodiscard]] static juce::Font panelTitle();
    [[nodiscard]] static juce::Font bodyStrong();
    [[nodiscard]] static juce::Font body();
    [[nodiscard]] static juce::Font labelStrong();
    [[nodiscard]] static juce::Font label();
    [[nodiscard]] static juce::Font microStrong();
    [[nodiscard]] static juce::Font micro();
    [[nodiscard]] static juce::Font data();
};

// The shared painting recipes: the backdrop, glass panels, wells, the
// signature gradient and glows. Every area paints through these so the
// Workbench reads as one surface.
struct WorkbenchStyle
{
    [[nodiscard]] static float panelRadius() noexcept;
    [[nodiscard]] static float controlRadius() noexcept;
    // True while the enhanced-contrast preference is on: glass gives way to
    // solid fills and strong outlines.
    [[nodiscard]] static bool enhancedContrast() noexcept;
    // The signature gradient across an area, left to right.
    [[nodiscard]] static juce::ColourGradient accentGradient(
        juce::Rectangle<float> area, float alpha = 1.0F);
    // A point along the signature gradient, 0 at its start, 1 at its end.
    [[nodiscard]] static juce::Colour accentAt(float proportion);
    // The window's backdrop: the ink gradient and its two soft glows.
    static void paintBackdrop(juce::Graphics& graphics, juce::Rectangle<float> area);
    // The soft shadow a glass panel casts; painted by the panel's parent.
    static void paintPanelShadow(juce::Graphics& graphics, juce::Rectangle<float> area, float radius);
    // A glass panel: translucent fill, top sheen, highlight line, thin edge.
    static void paintPanel(juce::Graphics& graphics, juce::Rectangle<float> area,
        float radius, std::optional<juce::Colour> tint = std::nullopt);
    // A panel's title, with a short gradient bar before it.
    static void paintPanelTitle(juce::Graphics& graphics, juce::Rectangle<int> area, const juce::String& title);
    // A recessed well, for fields and LCD-like displays.
    static void paintWell(juce::Graphics& graphics, juce::Rectangle<float> area, float radius);
    // A raised tile (cards, cells): a subtle gradient and a light edge.
    static void paintTile(juce::Graphics& graphics, juce::Rectangle<float> area, float radius,
        juce::Colour base, bool raised = true);
    // A stroke with a soft glow beneath it, for wires, scopes and meters.
    static void strokeWithGlow(juce::Graphics& graphics, const juce::Path& path,
        juce::Colour colour, float width, float glowWidth);
    // A display's fine grid, every column and row line faint, the middle row
    // a little brighter.
    static void paintDisplayGrid(juce::Graphics& graphics, juce::Rectangle<float> area,
        int columns, int rows);
    // A Sound category's colour code (Sources violet, Filters aqua, ...).
    [[nodiscard]] static juce::Colour categoryColour(std::string_view category);
};

class WorkbenchLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    WorkbenchLookAndFeel();
    void setAccessibilityPresentation(
        bool enhancedContrast,
        float focusOutlinePixels) noexcept;

    juce::Font getTextButtonFont(juce::TextButton&, int) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    juce::Label* createSliderTextBox(juce::Slider& slider) override;
    // The cursor shows what Talas is busy
    // with, over every component; none, each component's own.
    void setActivityCursor(std::optional<juce::MouseCursor> cursor);
    juce::MouseCursor getMouseCursorFor(juce::Component& component) override;

    void drawButtonBackground(
        juce::Graphics& graphics,
        juce::Button& button,
        const juce::Colour& colour,
        bool highlighted,
        bool down) override;
    void drawToggleButton(
        juce::Graphics& graphics,
        juce::ToggleButton& button,
        bool highlighted,
        bool down) override;
    void drawTickBox(
        juce::Graphics& graphics,
        juce::Component& component,
        float x,
        float y,
        float width,
        float height,
        bool ticked,
        bool enabled,
        bool highlighted,
        bool down) override;
    void drawComboBox(
        juce::Graphics& graphics,
        int width,
        int height,
        bool down,
        int buttonX,
        int buttonY,
        int buttonWidth,
        int buttonHeight,
        juce::ComboBox& box) override;
    void drawLinearSlider(
        juce::Graphics& graphics,
        int x,
        int y,
        int width,
        int height,
        float sliderPos,
        float minSliderPos,
        float maxSliderPos,
        juce::Slider::SliderStyle style,
        juce::Slider& slider) override;
    void drawRotarySlider(
        juce::Graphics& graphics,
        int x,
        int y,
        int width,
        int height,
        float sliderPosProportional,
        float rotaryStartAngle,
        float rotaryEndAngle,
        juce::Slider& slider) override;
    void drawScrollbar(
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
        bool isMouseDown) override;
    void fillTextEditorBackground(
        juce::Graphics& graphics,
        int width,
        int height,
        juce::TextEditor& editor) override;
    void drawTextEditorOutline(
        juce::Graphics& graphics,
        int width,
        int height,
        juce::TextEditor& editor) override;
    void drawPopupMenuBackground(
        juce::Graphics& graphics,
        int width,
        int height) override;
    void drawPopupMenuItem(
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
        const juce::Colour* textColour) override;
    void drawTooltip(
        juce::Graphics& graphics,
        const juce::String& text,
        int width,
        int height) override;
    void drawTabButton(
        juce::TabBarButton& button,
        juce::Graphics& graphics,
        bool isMouseOver,
        bool isMouseDown) override;
    void drawTabAreaBehindFrontButton(
        juce::TabbedButtonBar& bar,
        juce::Graphics& graphics,
        int width,
        int height) override;
    // A label marked as a readout (the "talasReadout" property) is drawn as
    // a recessed LCD with lit digits; every other label as JUCE draws it.
    void drawLabel(juce::Graphics& graphics, juce::Label& label) override;
    void drawProgressBar(
        juce::Graphics& graphics,
        juce::ProgressBar& bar,
        int width,
        int height,
        double progress,
        const juce::String& textToShow) override;

private:
    void drawFocusOutline(
        juce::Graphics& graphics,
        const juce::Component& component,
        float radius) const;

    float focusOutlinePixels_ = 2.0F;
    bool enhancedContrast_ = false;
    std::optional<juce::MouseCursor> activityCursor_;
};

// The watch, the hollow arrow, the arrow partly filled (a black dot inside)
// and the floppy disk; nothing for the plain arrow.
[[nodiscard]] std::optional<juce::MouseCursor> workbenchActivityMouseCursor(talas::app::WorkbenchCursor cursor);
// The eyeglasses of a file search, their eyeballs looking
// left and right in turn as the frame counts on.
[[nodiscard]] juce::MouseCursor workbenchLocatingMouseCursor(int frame);
// The magnifying glass over a spectrum view, a transparent
// plus in its lens marking the point the zoom box is drawn from.
[[nodiscard]] juce::MouseCursor workbenchMagnifierMouseCursor();
}
