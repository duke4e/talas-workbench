#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/TalasFiles.h"
#include "talas/project/ProjectGlobalMap.h"

#include <cmath>
#include <filesystem>
#include <fstream>

namespace talas::app::workbench
{
// The VCS Editor's widget type, numeric field,
// label, colours, Min / Max / Grid and taper, and a surface of tabs.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runVcsWidgetSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "VCS widget smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectDocument fixture;
    talas::project::VirtualControl nn;
    nn.eventName = "NN1";
    nn.minimum = 12.0;
    nn.maximum = 72.0;
    nn.value = 60.0;
    talas::project::VirtualControl start;
    start.eventName = "Start";
    start.value = 1.0 / 3.0;
    talas::project::VirtualControl sw;
    sw.eventName = "sw03";
    sw.kind = talas::project::VirtualControlKind::Gate;
    fixture.virtualControls = {nn, start, sw};
    // An Annotation's text tops the surface.
    talas::project::ProjectParameterExpression note;
    note.parameterId = "text";
    note.text = "A short sequencer";
    fixture.graph.nodes.push_back({"note", "talas.annotation", {note}});
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("VCS widget fixture was rejected: " + replaced.message);
    documentChanged();
    auto& surface = *virtualControlSurface_;
    if (!surface.tabsForSmoke().empty())
        return finish("a surface of one tab should show no tabs");
    if (surface.annotationTextForSmoke() != "A short sequencer")
        return finish("the Annotation's text did not top the surface: " + surface.annotationTextForSmoke().toStdString());

    // Show All Digits shows the surface's
    // numbers with twelve significant digits, six without it.
    surface.setShowAllDigits(false);
    const auto six = surface.widgetPresentationForSmoke("Start");
    surface.setShowAllDigits(true);
    const auto twelve = surface.widgetPresentationForSmoke("Start");
    surface.setShowAllDigits(preferences_.showAllDigits);
    if (!six || !twelve || six->numberText != "0.333333" || twelve->numberText != "0.333333333333")
        return finish("Show All Digits did not show the surface's numbers with twelve digits");

    // Tab selects the first fader's number,
    // then each next one's, round from the last; Shift+Tab goes back. The
    // gate shows no number.
    const auto first = surface.tabToNumberForSmoke(true);
    const auto second = surface.tabToNumberForSmoke(true);
    const auto round = surface.tabToNumberForSmoke(true);
    const auto back = surface.tabToNumberForSmoke(false);
    if (first != "NN1" || second != "Start" || round != "NN1" || back != "Start")
        return finish("Tab did not move between the faders' numbers: " + first + " " + second + " " + round + " " + back);

    // Shift and a drag fine-tune a fader, the
    // value moving a tenth as far as the pointer: half Start's length, a
    // twentieth of its range.
    const auto tuned = surface.fineTuneForSmoke("Start", 0.5);
    if (!tuned || std::abs(*tuned - (1.0 / 3.0 + 0.05)) > 1.0e-9)
        return finish("Shift and a drag did not fine-tune Start");
    // The Control key turns the cursor into a hand that
    // pushes the surface, here no further than it reaches.
    if (const auto [hand, view] = surface.handDragForSmoke(-5000, -5000); !hand || view.x < 0 || view.y < 0)
        return finish("the Control key did not show the hand that pushes the surface");

    // NN faders numbered, on a grid of 1; Start a horizontal fader
    // without its number; sw03 a green button.
    talas::project::VirtualControlWidget numbered;
    numbered.style = talas::project::VirtualControlWidgetStyle::Fader;
    numbered.grid = 1.0;
    numbered.label = "Note 1";
    if (!surface.applyWidgetForSmoke("NN1", numbered, 12.0, 72.0))
        return finish("the VCS Editor's options were refused for NN1");
    const auto nnShown = surface.widgetPresentationForSmoke("NN1");
    if (!nnShown || nnShown->sliderStyle != static_cast<int>(juce::Slider::LinearVertical) || !nnShown->numberShown
        || nnShown->interval != 1.0 || !nnShown->label.starts_with("Note 1"))
        return finish("NN1 did not become a numbered vertical fader on a grid of 1 labelled Note 1");
    const auto stepped = surface.widgetValueAtPositionForSmoke("NN1", 0.51);
    if (!stepped || *stepped != 43.0)
        return finish("NN1's fader did not fall on its half steps");
    // NN1 as a numeric entry box, dragged up a quarter of the
    // invisible fader with Command held, moves a quarter of its range.
    auto entryBox = numbered;
    entryBox.style = talas::project::VirtualControlWidgetStyle::SmallFader;
    if (!surface.applyWidgetForSmoke("NN1", entryBox, 12.0, 72.0))
        return finish("NN1 could not become a numeric entry box");
    const auto entryStart = surface.invisibleFaderForSmoke("NN1", 0.0);
    const auto entryMoved = surface.invisibleFaderForSmoke("NN1", 0.25);
    if (!entryStart || !entryMoved || std::abs(*entryMoved - std::min(72.0, *entryStart + 15.0)) > 1e-9
        || !surface.applyWidgetForSmoke("NN1", numbered, 12.0, 72.0))
        return finish("Command and a drag did not move the numeric entry box as an invisible fader");
    talas::project::VirtualControlWidget horizontal;
    horizontal.style = talas::project::VirtualControlWidgetStyle::HorizontalFader;
    horizontal.showNumber = false;
    horizontal.taper = talas::project::VirtualControlTaper::Log;
    if (!surface.applyWidgetForSmoke("Start", horizontal, 0.01, 1.0))
        return finish("Start's options were refused");
    const auto startShown = surface.widgetPresentationForSmoke("Start");
    const auto startMiddle = surface.widgetValueAtPositionForSmoke("Start", 0.5);
    if (!startShown || startShown->sliderStyle != static_cast<int>(juce::Slider::LinearHorizontal)
        || startShown->numberShown || !startMiddle || std::abs(*startMiddle - 0.1) > 1e-6)
        return finish("Start did not become a horizontal fader without a number on a log taper");
    talas::project::VirtualControlWidget green;
    green.onColour = 0x00ff00u;
    green.offColour = 0x004000u;
    if (!surface.applyWidgetForSmoke("sw03", green, 0.0, 1.0)
        || surface.widgetPresentationForSmoke("sw03")->onColour != 0x00ff00u)
        return finish("sw03 did not turn green");
    const auto& controls = projectBridge_.editor().document().virtualControls;
    if (controls[0].widget != numbered || controls[1].minimum != 0.01 || controls[2].widget != green)
        return finish("the widget options were not kept in the project");
    undoProject();
    if (projectBridge_.editor().document().virtualControls[2].widget != talas::project::VirtualControlWidget{})
        return finish("one Undo did not take back sw03's colours");

    // Widgets on tabs.
    auto tabbed = horizontal;
    tabbed.tab = "Timing";
    if (!surface.applyWidgetForSmoke("Start", tabbed, 0.01, 1.0))
        return finish("Start could not move to a tab");
    if (surface.tabsForSmoke() != std::vector<std::string>{"", "Timing"})
        return finish("the surface did not show its main and Timing tabs");
    if (!surface.widgetPresentationForSmoke("NN1")->shown || surface.widgetPresentationForSmoke("Start")->shown)
        return finish("the main tab should show NN1 and not Start");
    if (!surface.selectTabForSmoke("Timing") || surface.widgetPresentationForSmoke("NN1")->shown
        || !surface.widgetPresentationForSmoke("Start")->shown)
        return finish("the Timing tab should show Start alone");
    if (surface.applyWidgetForSmoke("NN1", [] {
            talas::project::VirtualControlWidget bad;
            bad.grid = 100.0;
            return bad;
        }(), 12.0, 72.0))
        return finish("a grid wider than the range was accepted");
    return finish();
}
}

namespace talas::app::workbench
{
// Tools > Global Map edits the project's
// global map, the Default map's text shown when it has none.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runGlobalMapSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "global map smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    talas::project::ProjectParameterExpression level;
    level.parameterId = "value";
    level.expression = "!Brightness";
    talas::project::ProjectDocument fixture;
    talas::project::reconcileProjectPresentation(fixture);
    if (const auto replaced = projectBridge_.editor().replaceDocument(fixture); !replaced.ok())
        return finish("global map fixture was rejected: " + replaced.message);
    documentChanged();
    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowGlobalMap;
    if (!listed || !commandManager_.invokeDirectly(commandShowGlobalMap, false) || globalMapPanel_ == nullptr)
        return finish("the Tools menu did not open the Global Map");
    auto& panel = *globalMapPanel_;
    if (panel.textForSmoke() != juce::String{talas::project::defaultGlobalMapText()})
        return finish("a project without a global map should show the Default map");
    panel.setTextForSmoke("!Brightness is: (`MIDIController74 min: 0 max: 10 grid: 1; displayAs: #toggle).\n");
    if (!panel.applyForSmoke()
        || projectBridge_.editor().document().globalMap.find("MIDIController74") == std::string::npos)
        return finish("Apply did not keep the project's global map");
    // A Sound reading the map's name then shows it as the map displays it.
    if (const auto added = projectBridge_.editor().addNode({"level", "talas.constant", {level}}, {0, 0}); !added.ok())
        return finish("a Sound reading !Brightness was refused: " + added.message);
    documentChanged();
    const auto& controls = projectBridge_.editor().document().virtualControls;
    const auto brightness = std::ranges::find(controls, "Brightness", &talas::project::VirtualControl::eventName);
    if (brightness == controls.end() || brightness->kind != talas::project::VirtualControlKind::Toggle)
        return finish("the map's displayAs: #toggle did not show !Brightness as a toggle");
    panel.setTextForSmoke("!Brightness is: `Nothing nn");
    if (panel.applyForSmoke() || projectBridge_.editor().document().globalMap.empty())
        return finish("a map that does not parse was applied");
    if (!panel.useDefaultForSmoke() || !projectBridge_.editor().document().globalMap.empty())
        return finish("Use Default did not return to the Default map");
    undoProject();
    if (projectBridge_.editor().document().globalMap.find("MIDIController74") == std::string::npos)
        return finish("one Undo did not bring the project's map back");
    // A global map file opens in the editor, and
    // Choose global map... makes one the project's.
    const auto folder = std::filesystem::temp_directory_path() / "talas-global-map-smoke";
    const auto studio = talas::app::defaultGlobalMapFile(folder);
    if (!studio.has_value() || !panel.openFile(*studio) || panel.textForSmoke() != juce::String{talas::project::defaultGlobalMapText()})
        return finish("the Default global map file did not open in the editor");
    const auto chosen = folder / "Global Maps" / "Studio.map";
    {
        std::ofstream output(chosen, std::ios::binary);
        output << "!Brightness is: `MIDIController75.\n";
    }
    if (!useGlobalMapFile(chosen) || projectBridge_.editor().document().globalMap.find("MIDIController75") == std::string::npos)
        return finish("the global map chosen did not become the project's");
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    return finish();
}
}
