#pragma once

#include "talas/app/TalasKeyEquivalents.h"
#include "talas/app/WorkbenchUiPolicy.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace talas::app::workbench
{
struct WorkbenchTheme
{
    juce::Colour canvas{0xff1c2026};
    juce::Colour surface{0xff252c35};
    juce::Colour outline{0xff3a4654};
    juce::Colour interactiveOutline{0xff607184};
    juce::Colour primaryText{0xffe8edf2};
    juce::Colour secondaryText{0xffaab7c4};
    // A selection is blue.
    juce::Colour focus{talas::app::selectionHighlightColours().soundInSignalFlow};
    juce::Colour audio{0xff4ec7b5};
    juce::Colour control{0xfff0b35b};
    juce::Colour event{0xffd97dbb};
    juce::Colour data{0xff8da8e8};
    juce::Colour output{0xff7fa7ff};
    juce::Colour error{0xfff06a6a};
    juce::Colour fullSupport{0xff287a55};
    juce::Colour subsetSupport{0xff8c5b12};
    juce::Colour placeholderSupport{0xffa94f58};
    juce::Colour utilitySupport{0xff57708c};
    juce::Colour graphGrid{0xff222933};
    juce::Colour selectedRow{talas::app::selectionHighlightColours().row};
    juce::Colour controlTile{0xff202831};
    juce::Colour widgetSurface{0xff263238};
    juce::Colour widgetOutline{0xff8e989b};
    juce::Colour widgetText{0xffffffff};
    juce::Colour widgetFill{0xff42a2c8};
    juce::Colour widgetHighlight{0xff181f22};
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

class WorkbenchLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    WorkbenchLookAndFeel();
    void setAccessibilityPresentation(
        bool enhancedContrast,
        float focusOutlinePixels) noexcept;

    juce::Font getTextButtonFont(juce::TextButton&, int) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
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

private:
    void drawFocusOutline(
        juce::Graphics& graphics,
        const juce::Component& component,
        float radius) const;

    float focusOutlinePixels_ = 2.0F;
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
