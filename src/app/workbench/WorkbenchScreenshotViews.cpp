#include "app/workbench/WorkbenchScreenshotViews.h"

#include "app/workbench/WorkbenchAudioMidiSettings.h"
#include "app/workbench/WorkbenchTheme.h"
#include "plugin/TalasPluginEditorView.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/plugin/PluginParameters.h"

#include <algorithm>
#include <memory>
#include <tuple>

namespace talas::app::workbench::screenshot_views
{
namespace
{
// A popup menu as its window draws it: the background, then each item at
// its ideal size, through the look and feel.
[[nodiscard]] juce::Image renderMenu(const juce::PopupMenu& menu, int highlighted, int maximumItems)
{
    auto& lookAndFeel = juce::LookAndFeel::getDefaultLookAndFeel();
    const juce::PopupMenu::Options options;
    const auto border = lookAndFeel.getPopupMenuBorderSizeWithOptions(options);
    std::vector<std::pair<juce::PopupMenu::Item, int>> rows;
    auto width = 0;
    for (juce::PopupMenu::MenuItemIterator iterator(menu); iterator.next() && static_cast<int>(rows.size()) < maximumItems;)
    {
        const auto& item = iterator.getItem();
        const auto measured = item.shortcutKeyDescription.isEmpty()
            ? item.text
            : item.text + "   " + item.shortcutKeyDescription;
        auto itemWidth = 80;
        auto itemHeight = 16;
        lookAndFeel.getIdealPopupMenuItemSizeWithOptions(measured, item.isSeparator, 0, itemWidth, itemHeight, options);
        width = std::max(width, itemWidth);
        rows.emplace_back(item, itemHeight);
    }
    auto height = 2 * border;
    for (const auto& row : rows)
        height += row.second;
    width += 2 * border;
    juce::Image image{juce::Image::ARGB, std::max(1, width), std::max(1, height), true};
    juce::Graphics graphics{image};
    lookAndFeel.drawPopupMenuBackgroundWithOptions(graphics, width, height, options);
    auto y = border;
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        const auto& [item, itemHeight] = rows[index];
        juce::Graphics::ScopedSaveState state(graphics);
        graphics.setOrigin({border, y});
        const auto area = juce::Rectangle<int>{width - 2 * border, itemHeight};
        if (item.isSectionHeader)
            lookAndFeel.drawPopupMenuSectionHeaderWithOptions(graphics, area, item.text, options);
        else
            lookAndFeel.drawPopupMenuItemWithOptions(
                graphics, area, static_cast<int>(index) == highlighted && item.isEnabled, item, options);
        y += itemHeight;
    }
    return image;
}

// The soft shadow a desktop window casts, then the image over it.
void drawFloating(juce::Graphics& graphics, const juce::Image& image, juce::Point<int> at, float radius = 4.0F)
{
    juce::Path shape;
    shape.addRoundedRectangle(juce::Rectangle<int>{at.x, at.y, image.getWidth(), image.getHeight()}.toFloat(), radius);
    juce::DropShadow{juce::Colours::black.withAlpha(0.55F), 22, {0, 8}}.drawForPath(graphics, shape);
    // The shadow leaves its translucent colour set, and an image is drawn at
    // the current opacity.
    graphics.setOpacity(1.0F);
    graphics.drawImageAt(image, at.x, at.y);
}

// In software, so an opaque component's image is truly opaque: a native
// image of one can come back with its alpha left unset.
[[nodiscard]] juce::Image snapshot(juce::Component& component)
{
    return component.createComponentSnapshot(component.getLocalBounds(), true, 1.0F, juce::SoftwareImageType{});
}

// An Audio & MIDI device type with named devices and none to open.
class StandInDeviceType final : public juce::AudioIODeviceType
{
public:
    StandInDeviceType()
        : juce::AudioIODeviceType("Core Audio")
    {
    }

    void scanForDevices() override {}

    [[nodiscard]] juce::StringArray getDeviceNames(bool wantInputNames) const override
    {
        return wantInputNames ? juce::StringArray{"Studio Interface"}
                              : juce::StringArray{"Studio Interface", "Built-in Output"};
    }

    [[nodiscard]] int getDefaultDeviceIndex(bool) const override
    {
        return 0;
    }

    [[nodiscard]] int getIndexOfDevice(juce::AudioIODevice*, bool) const override
    {
        return -1;
    }

    [[nodiscard]] bool hasSeparateInputsAndOutputs() const override
    {
        return true;
    }

    juce::AudioIODevice* createDevice(const juce::String&, const juce::String&) override
    {
        return nullptr;
    }
};
}

std::optional<std::string> writePng(const juce::Image& image, const std::filesystem::path& file)
{
    if (!image.isValid())
        return "screenshot " + file.filename().string() + " did not produce an image";
    const auto opaque = image.convertedToFormat(juce::Image::RGB);
    const juce::File output{juce::String::fromUTF8(file.lexically_normal().string().c_str())};
    output.deleteFile();
    auto stream = output.createOutputStream();
    if (stream == nullptr || !stream->openedOk() || !juce::PNGImageFormat{}.writeImageToStream(opaque, *stream))
        return "screenshot " + file.filename().string() + " could not be written";
    stream->flush();
    return std::nullopt;
}

juce::Image popupMenus(const std::vector<std::pair<juce::PopupMenu, int>>& menus)
{
    std::vector<juce::Image> rendered;
    auto width = 40;
    auto height = 0;
    for (const auto& [menu, highlighted] : menus)
    {
        rendered.push_back(renderMenu(menu, highlighted, 18));
        width += rendered.back().getWidth() + 40;
        height = std::max(height, rendered.back().getHeight());
    }
    juce::Image image{juce::Image::RGB, width, height + 110, true};
    juce::Graphics graphics{image};
    WorkbenchStyle::paintBackdrop(graphics, image.getBounds().toFloat());
    // The toolbar the menus drop from.
    const auto toolbar = juce::Rectangle<float>{0.0F, 0.0F, static_cast<float>(width), 46.0F};
    graphics.setGradientFill(juce::ColourGradient{juce::Colours::white.withAlpha(0.045F), 0.0F, 0.0F,
        juce::Colours::white.withAlpha(0.0F), 0.0F, toolbar.getBottom(), false});
    graphics.fillRect(toolbar);
    graphics.setColour(juce::Colours::white.withAlpha(0.06F));
    graphics.fillRect(toolbar.withTop(toolbar.getBottom() - 1.0F));
    auto x = 40;
    for (const auto& menu : rendered)
    {
        drawFloating(graphics, menu, {x, 56});
        x += menu.getWidth() + 40;
    }
    return image;
}

juce::Image helpAndAlerts()
{
    auto& lookAndFeel = juce::LookAndFeel::getDefaultLookAndFeel();
    juce::Image image{juce::Image::RGB, 1180, 430, true};
    juce::Graphics graphics{image};
    WorkbenchStyle::paintBackdrop(graphics, image.getBounds().toFloat());

    // Fly-by help under a control on a glass panel.
    const auto panel = juce::Rectangle<float>{30.0F, 30.0F, 330.0F, 370.0F};
    WorkbenchStyle::paintPanelShadow(graphics, panel, WorkbenchStyle::panelRadius());
    WorkbenchStyle::paintPanel(graphics, panel, WorkbenchStyle::panelRadius());
    WorkbenchStyle::paintPanelTitle(graphics, panel.toNearestInt().reduced(16, 12).removeFromTop(24), "Fly-by help");
    juce::TextButton target{"Learn"};
    target.setSize(90, 28);
    graphics.drawImageAt(snapshot(target), 52, 84);
    const juce::String help = "Learn MIDI: the next controller message you move maps to this Event Value. "
                              "Move it again to fine-tune the mapping.";
    const auto tip = lookAndFeel.getTooltipBounds(help, {60, 118}, juce::Rectangle<int>{40, 110, 300, 260});
    juce::Image tipImage{juce::Image::ARGB, tip.getWidth(), tip.getHeight(), true};
    {
        juce::Graphics tipGraphics{tipImage};
        lookAndFeel.drawTooltip(tipGraphics, help, tip.getWidth(), tip.getHeight());
    }
    drawFloating(graphics, tipImage, {52, 120}, 5.0F);

    // An information alert and a warning.
    auto x = 390;
    for (const auto warning : {false, true})
    {
        juce::AlertWindow alert{warning ? "Replace the sample?" : "Plugin exported",
            warning ? "Samples/showcase_low.wav has changed on disk since it was imported. Replacing it updates "
                      "every region that plays it."
                    : "Talas Showcase by Talas (Instrument, 5 parameters) is in Talas Plugins as a VST3 and an AU. "
                      "Copy a bundle into your plug-in folder and rescan in your DAW.",
            warning ? juce::MessageBoxIconType::WarningIcon : juce::MessageBoxIconType::InfoIcon};
        alert.removeFromDesktop();
        if (warning)
        {
            alert.addButton("Replace", 1);
            alert.addButton("Cancel", 0);
        }
        else
        {
            alert.addButton("OK", 1);
        }
        const auto shot = snapshot(alert);
        drawFloating(graphics, shot, {x, 30 + (warning ? 200 - shot.getHeight() / 2 : 0)},
            WorkbenchStyle::panelRadius());
        x += shot.getWidth() + 30;
        if (x > 1180)
            break;
    }
    return image;
}

juce::Image controlSheet()
{
    juce::Component sheet;
    sheet.setSize(1000, 420);
    juce::OwnedArray<juce::Component> owned;
    const auto add = [&](juce::Component* component, juce::Rectangle<int> bounds) {
        owned.add(component);
        sheet.addAndMakeVisible(component);
        component->setBounds(bounds);
        return component;
    };

    // Tabs and progress.
    auto* tabs = static_cast<juce::TabbedButtonBar*>(
        add(new juce::TabbedButtonBar{juce::TabbedButtonBar::TabsAtTop}, {30, 64, 420, 30}));
    for (const auto* name : {"Controls", "Snapshots", "Live Grid", "Mappings"})
        tabs->addTab(name, juce::Colours::transparentBlack, -1);
    tabs->setCurrentTabIndex(1);
    static double determinate = 0.62;
    static double partial = 0.35;
    auto* progress = static_cast<juce::ProgressBar*>(add(new juce::ProgressBar{determinate}, {30, 112, 420, 20}));
    progress->setTextToDisplay("Rendering partials 62%");
    static_cast<juce::ProgressBar*>(add(new juce::ProgressBar{partial}, {30, 142, 420, 8}))->setPercentageDisplay(false);

    // Buttons, toggles and a combo box.
    add(new juce::TextButton{"Audition"}, {30, 172, 96, 28});
    auto* on = static_cast<juce::TextButton*>(add(new juce::TextButton{"Edit"}, {134, 172, 72, 28}));
    on->setToggleState(true, juce::dontSendNotification);
    auto* off = add(new juce::TextButton{"Commit"}, {214, 172, 86, 28});
    off->setEnabled(false);
    auto* play = static_cast<juce::TextButton*>(add(new juce::TextButton{"Play"}, {308, 172, 70, 28}));
    play->getProperties().set(talasGlyphProperty, "play");
    play->setConnectedEdges(juce::Button::ConnectedOnRight);
    auto* stop = static_cast<juce::TextButton*>(add(new juce::TextButton{"Stop"}, {378, 172, 70, 28}));
    stop->getProperties().set(talasGlyphProperty, "stop");
    stop->setConnectedEdges(juce::Button::ConnectedOnLeft);
    auto* ticked = static_cast<juce::ToggleButton*>(add(new juce::ToggleButton{"Show"}, {30, 214, 90, 24}));
    ticked->setToggleState(true, juce::dontSendNotification);
    add(new juce::ToggleButton{"Solo"}, {124, 214, 90, 24});
    auto* combo = static_cast<juce::ComboBox*>(add(new juce::ComboBox{}, {220, 212, 230, 26}));
    combo->addItemList({juce::String::fromUTF8("Bars \xc2\xb7 Beats \xc2\xb7 Ticks"),
                           juce::String::fromUTF8("Minutes \xc2\xb7 Seconds"), "Frames"},
        1);
    combo->setSelectedId(1, juce::dontSendNotification);
    auto* field = static_cast<juce::TextEditor*>(add(new juce::TextEditor{}, {30, 252, 200, 26}));
    field->setText("Glass Pluck", false);
    auto* empty = static_cast<juce::TextEditor*>(add(new juce::TextEditor{}, {240, 252, 210, 26}));
    empty->setTextToShowWhenEmpty("Search name, category", workbenchTheme.secondaryText);
    auto* fader = static_cast<juce::Slider*>(
        add(new juce::Slider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight}, {30, 292, 420, 26}));
    fader->setRange(0.0, 1.0);
    fader->setValue(0.42, juce::dontSendNotification);
    fader->setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 22);
    fader->setNumDecimalPlacesToDisplay(2);

    // Knobs at three sizes, one disabled, and pads.
    auto knobX = 490;
    for (const auto [size, value, enabled] : std::initializer_list<std::tuple<int, double, bool>>{
             {44, 0.25, true}, {64, 0.55, true}, {88, 0.8, true}, {64, 0.4, false}})
    {
        auto* knob = static_cast<juce::Slider*>(add(new juce::Slider{juce::Slider::RotaryHorizontalVerticalDrag,
                                                        juce::Slider::NoTextBox},
            {knobX, 64 + (88 - size) / 2, size, size}));
        knob->setRange(0.0, 1.0);
        knob->setValue(value, juce::dontSendNotification);
        knob->setEnabled(enabled);
        knobX += size + 22;
    }
    auto* readout = static_cast<juce::Slider*>(add(new juce::Slider{juce::Slider::RotaryHorizontalVerticalDrag,
                                                       juce::Slider::TextBoxBelow},
        {knobX, 52, 96, 118}));
    readout->getProperties().set("talasReadout", true);
    readout->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 84, 22);
    readout->setRange(55.0, 880.0);
    readout->setValue(220.0, juce::dontSendNotification);
    readout->setTextValueSuffix(" Hz");
    readout->setNumDecimalPlacesToDisplay(1);

    juce::Image image{juce::Image::RGB, sheet.getWidth(), sheet.getHeight(), true};
    juce::Graphics graphics{image};
    WorkbenchStyle::paintBackdrop(graphics, image.getBounds().toFloat());
    const auto panel = sheet.getLocalBounds().toFloat().reduced(12.0F);
    WorkbenchStyle::paintPanelShadow(graphics, panel, WorkbenchStyle::panelRadius());
    WorkbenchStyle::paintPanel(graphics, panel, WorkbenchStyle::panelRadius());
    WorkbenchStyle::paintPanelTitle(graphics, {30, 22, 400, 24}, "Controls");
    // Pads and pills, painted as their owners paint them.
    WorkbenchStyle::paintPad(graphics, {490.0F, 200.0F, 150.0F, 44.0F}, true, true, "Open");
    WorkbenchStyle::paintPad(graphics, {652.0F, 200.0F, 150.0F, 44.0F}, false, true, "Closed");
    WorkbenchStyle::paintPad(graphics, {814.0F, 200.0F, 150.0F, 44.0F}, true, false, "On");
    auto pillX = 490.0F;
    for (const auto& [colour, label] : std::initializer_list<std::pair<juce::Colour, const char*>>{
             {workbenchTheme.fullSupport, "FULL"}, {workbenchTheme.subsetSupport, "SUBSET"},
             {workbenchTheme.placeholderSupport, "PLACEHOLDER"}, {workbenchTheme.utilitySupport, "UTILITY"},
             {workbenchTheme.control, "FADER"}, {workbenchTheme.data, "INFO"}, {workbenchTheme.error, "ERROR"}})
    {
        const auto width = juce::GlyphArrangement::getStringWidth(WorkbenchType::microStrong(), label) + 16.0F;
        WorkbenchStyle::paintPill(graphics, {pillX, 266.0F, width, 16.0F}, colour, workbenchTheme.surface, label,
            WorkbenchType::microStrong());
        pillX += width + 8.0F;
    }
    auto glyphX = 490.0F;
    for (const auto glyph : {WorkbenchStyle::Glyph::Play, WorkbenchStyle::Glyph::Pause, WorkbenchStyle::Glyph::Stop,
             WorkbenchStyle::Glyph::Record})
    {
        WorkbenchStyle::paintGlyph(graphics, {glyphX, 300.0F, 14.0F, 14.0F}, glyph,
            glyph == WorkbenchStyle::Glyph::Record ? workbenchTheme.error : workbenchTheme.primaryText);
        glyphX += 28.0F;
    }
    WorkbenchStyle::paintBrand(graphics, {620.0F, 290.0F, 200.0F, 34.0F}, true);
    WorkbenchStyle::paintBrand(graphics, {840.0F, 290.0F, 34.0F, 34.0F}, false);
    sheet.paintEntireComponent(graphics, false);
    return image;
}

juce::Image dialogWindow(juce::Component& content, const juce::String& title)
{
    constexpr int titleHeight = 28;
    juce::Image image{juce::Image::RGB, content.getWidth(), content.getHeight() + titleHeight, true};
    juce::Graphics graphics{image};
    // A dark title bar, as the system draws it over the dialog.
    graphics.setColour(juce::Colour{0xff1d1f29});
    graphics.fillRect(0, 0, image.getWidth(), titleHeight);
    graphics.setColour(juce::Colours::black.withAlpha(0.5F));
    graphics.fillRect(0, titleHeight - 1, image.getWidth(), 1);
    graphics.setColour(workbenchTheme.primaryText.withAlpha(0.85F));
    graphics.setFont(WorkbenchType::bodyStrong());
    graphics.drawText(title, juce::Rectangle<int>{0, 0, image.getWidth(), titleHeight}, juce::Justification::centred);
    graphics.setOrigin({0, titleHeight});
    // The window's own background, as the look and feel fills a dialog in
    // the theme's surface colour.
    WorkbenchStyle::paintBackdrop(graphics, content.getLocalBounds().toFloat());
    content.paintEntireComponent(graphics, false);
    return image;
}

juce::Image audioMidiSettings()
{
    juce::AudioDeviceManager manager;
    manager.addAudioDeviceType(std::make_unique<StandInDeviceType>());
    manager.setCurrentAudioDeviceType("Core Audio", false);
    talas::project::ProjectOscIngressSettings ingress;
    talas::project::ProjectOscEgressSettings egress;
    auto settings = createAudioMidiSettingsComponent(
        manager, {}, {}, [](talas::project::ProjectTransportSyncSettings) {}, ingress, egress,
        "OSC loopback profile off",
        [](talas::project::ProjectOscIngressSettings, talas::project::ProjectOscEgressSettings) {
            return juce::String{};
        });
    return dialogWindow(*settings, "Audio & MIDI Settings");
}

juce::Image pluginEditor(const talas::project::ProjectDocument& document)
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto mapping = talas::plugin::mapProjectParameters(document, registry, talas::plugin::PluginKind::Instrument);
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> parameters;
    talas::plugin::PluginEditorModel model;
    model.identity.name = document.metadata.title.empty() ? std::string{"Talas Plugin"} : document.metadata.title;
    model.identity.manufacturer = std::string{talas::plugin::defaultPluginManufacturer};
    model.identity.kind = talas::plugin::PluginKind::Instrument;
    model.kind = talas::plugin::PluginKind::Instrument;
    for (const auto& spec : mapping.parameters)
    {
        parameters.push_back(talas::plugin::makePluginParameter(spec));
        model.parameters.push_back({spec, parameters.back().get()});
    }
    model.status = [] { return juce::String{"Playing at 48.0 kHz"}; };
    model.ready = [] { return true; };
    talas::plugin::TalasPluginEditorView view{std::move(model)};
    view.resized();
    return snapshot(view);
}
}
