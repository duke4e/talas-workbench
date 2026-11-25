#include "talas/app/SelectedSoundDocument.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/app/VcsDisplays.h"
#include "talas/app/VcsEditorWindow.h"
#include "talas/app/VcsLayout.h"
#include "talas/app/VcsPresets.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/RealtimeAnalysisCapture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression expression(std::string id, std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(text);
    return parameter;
}

talas::project::ProjectDocument project()
{
    talas::project::ProjectDocument document;
    document.virtualControls.push_back(
        {"Cutoff", talas::expr::ValueDomain::Hertz, talas::project::VirtualControlKind::Fader, 440.0, 20.0, 20000.0});
    document.virtualControls.push_back(
        {"Vol", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.25, 0.0, 1.0});
    document.graph.nodes.push_back({"osc", "talas.oscillator", {expression("frequency", "!Cutoff"), expression("amplitude", "!Vol")}});
    document.graph.nodes.push_back({"output", "talas.audioOutput", {}});
    document.graph.connections.push_back({"osc", "audioOut", "output", "audioIn"});
    return document;
}

[[nodiscard]] double valueOf(const talas::runtime::ProjectRuntimeBridge& bridge, std::string_view name)
{
    const auto values = talas::app::vcsControlValues(bridge.performanceState().currentValues);
    const auto found = std::ranges::find(values, name, &std::pair<std::string, double>::first);
    require(found != values.end(), "the surface should show " + std::string{name});
    return found->second;
}

// The camera keeps the faders as a preset with a
// suggested name; the list numbers each by its program change, Default
// first; the dice set every control at random but a frozen one; choosing a
// preset sets the faders back.
void testPresetsCameraAndDice()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = project();
    const auto made = talas::app::vcsPresetBank(document, registry, {});
    require(made.ok(), "the presets should be kept as a snapshot bank: " + made.diagnostic);
    const auto& bank = *made.bank;
    require(bank.snapshots.size() == 1 && bank.snapshots.front().displayName == "Default",
        "a Sound's presets begin with Default");
    require(std::ranges::all_of(bank.parameterPolicies, [](const talas::project::SnapshotParameterPolicy& policy) {
                return policy.locked == (policy.target.kind != talas::project::SnapshotTargetKind::EventValue);
            }),
        "the dice move the surface's controls only");
    document.snapshotLibrary.banks.push_back(bank);

    talas::runtime::ProjectRuntimeBridge bridge(std::move(document), registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    require(bridge.compileAndStage(settings).ok(), "the project should stage");
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()), "the project should play");

    const auto* kept = talas::app::findVcsPresetBank(bridge.editor().document());
    require(kept != nullptr, "the bank should be found by its identity");
    const auto current = bridge.currentSnapshotScopeValues(kept->scope);
    require(current.ok(), "the faders' values should be read");
    const auto name = talas::app::randomVcsPresetName(7);
    require(!name.empty() && name != talas::app::randomVcsPresetName(8), "a random name is suggested for a new preset");
    const auto presetId = talas::app::uniqueVcsPresetId(*kept);
    require(bridge.editor().storeSnapshot(talas::app::vcsPresetBankId, {presetId, name, current.values}).ok()
            && bridge.adoptSnapshotAuthoringEdit().ok(),
        "the camera takes a snapshot of the faders");
    kept = talas::app::findVcsPresetBank(bridge.editor().document());
    require(talas::app::vcsPresetLabels(*kept) == std::vector<std::string>{"Default [1]", name + " [2]"},
        "the list shows each preset with its MIDI program change in brackets");
    require(talas::app::vcsPresetForProgram(*kept, 2) == std::optional<std::string>{presetId}
            && !talas::app::vcsPresetForProgram(*kept, 3).has_value(),
        "a MIDI program change selects the preset it numbers");
    require(talas::app::vcsAdjacentPreset(*kept, "default", 1) == std::optional<std::string>{presetId}
            && talas::app::vcsAdjacentPreset(*kept, presetId, 1) == std::optional<std::string>{presetId},
        "the down arrow selects the next preset, staying at the last");

    const auto& vol = bridge.editor().document().virtualControls.back();
    require(bridge.editor().setSnapshotParameterPolicy(talas::app::vcsPresetBankId, talas::app::vcsFreezePolicy(*kept, vol, true)).ok()
            && bridge.adoptSnapshotAuthoringEdit().ok(),
        "clicking a parameter's name freezes it");
    kept = talas::app::findVcsPresetBank(bridge.editor().document());
    require(talas::app::vcsControlFrozen(*kept, vol), "the frozen parameter is greyed");
    require(bridge.randomizeSnapshotBank(talas::app::vcsPresetBankId, 11).ok(), "the dice should roll");
    require(valueOf(bridge, "Cutoff") != 440.0 && valueOf(bridge, "Cutoff") >= 20.0 && valueOf(bridge, "Cutoff") <= 20000.0,
        "the dice set a parameter to a random value within its range");
    require(valueOf(bridge, "Vol") == 0.25, "a frozen parameter ignores the dice");
    require(bridge.render(&outputPointer, 1, output.size()), "the dice's values should play");
    const auto recalled = bridge.recallSnapshot(talas::app::vcsPresetBankId, presetId);
    require(recalled.ok() && valueOf(bridge, "Cutoff") == 440.0,
        "selecting a preset sets the faders to its values: " + recalled.message + " " + std::to_string(valueOf(bridge, "Cutoff")));
    require(talas::app::vcsPresetsBesidesDefault(*kept) == std::vector<std::string>{presetId},
        "removing all but Default leaves Default alone");

    // Ctrl and the camera: the selected preset takes the current settings,
    // keeping its name.
    require(bridge.render(&outputPointer, 1, output.size()), "the preset's values should play");
    const auto rolledAgain = bridge.randomizeSnapshotBank(talas::app::vcsPresetBankId, 12);
    require(rolledAgain.ok(), "the dice should roll again: " + rolledAgain.message);
    const auto rolled = valueOf(bridge, "Cutoff");
    require(bridge.captureSnapshotPerformance(talas::app::vcsPresetBankId, presetId).ok()
            && bridge.adoptSnapshotAuthoringEdit().ok(),
        "Ctrl and the camera change the selected preset");
    kept = talas::app::findVcsPresetBank(bridge.editor().document());
    require(talas::app::vcsPresetLabels(*kept) == std::vector<std::string>{"Default [1]", name + " [2]"},
        "the preset keeps its name");
    require(bridge.render(&outputPointer, 1, output.size()) && bridge.recallSnapshot(talas::app::vcsPresetBankId, "default").ok()
            && valueOf(bridge, "Cutoff") == 440.0 && bridge.render(&outputPointer, 1, output.size())
            && bridge.recallSnapshot(talas::app::vcsPresetBankId, presetId).ok() && valueOf(bridge, "Cutoff") == rolled,
        "the preset recalls the settings it was updated with");

    // The presets are saved in the file that holds the Timeline, the
    // project's, and load with it.
    kept = talas::app::findVcsPresetBank(bridge.editor().document());
    const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(bridge.editor().document()));
    const auto* reopenedBank = reopened.ok() ? talas::app::findVcsPresetBank(reopened.document) : nullptr;
    require(reopenedBank != nullptr && reopenedBank->snapshots == kept->snapshots
            && talas::app::vcsPresetLabels(*reopenedBank) == talas::app::vcsPresetLabels(*kept),
        "the presets should be saved along with the Timeline");
}
}

// Clean up in columns, Fit, widgets selected one
// at a time or by a box, moved, resized from a corner, squeezed together,
// placed by typing their edges, and a surface larger than its window.
void testArrangingTheSurface()
{
    using talas::app::VcsRect;
    const auto cleaned = talas::app::vcsCleanUp(7, 3, 60, 100);
    require(cleaned.size() == 7 && cleaned[0] == VcsRect{8, 8, 60, 100} && cleaned[3] == VcsRect{8, 116, 60, 100}
            && cleaned[2].left == 8 + 2 * 68,
        "Clean up asks for the columns and lays the widgets out in that many");
    const auto extent = talas::app::vcsExtent(cleaned);
    require(extent.width == 8 + 3 * 68 - 8 && extent.height == 8 + 3 * 108 - 8, "the surface covers all its widgets");
    // With every widget selected, dragging one's bottom edge
    // up shrinks them all alike.
    const auto shrunkTogether = talas::app::vcsResizedTogether(cleaned, {0, 1, 2, 3, 4, 5, 6}, talas::app::VcsCorner::BottomRight, 0, -40);
    require(std::ranges::all_of(shrunkTogether, [](const VcsRect& rect) { return rect.height == 60 && rect.width == 60; })
            && shrunkTogether[3].top == cleaned[3].top
            && talas::app::vcsResizedTogether(cleaned, {0}, talas::app::VcsCorner::BottomRight, 0, -40)[1] == cleaned[1],
        "dragging the edge of one selected widget should resize every selected one");
    const auto fitted = talas::app::vcsFit(cleaned, extent.width / 2, extent.height / 2);
    require(talas::app::vcsExtent(fitted).width <= extent.width / 2 + 1 && talas::app::vcsExtent(fitted).height <= extent.height / 2 + 1,
        "Fit fits all the widgets into the window");
    require(talas::app::vcsSurfaceCutOff(extent, extent.width / 2, extent.height) && !talas::app::vcsSurfaceCutOff(extent, extent.width, extent.height),
        "a window smaller than the surface cuts some widgets off");
    require(talas::app::vcsCleanUpFitsWindow(extent, extent.width, extent.height)
            && !talas::app::vcsCleanUpFitsWindow(extent, extent.width / 2, extent.height),
        "Clean up on a surface the size of its window should fit it to the window, as Fit does a larger one");
    // The box at the top centre, gray for the surface, blue for the
    // window's part of it; dragging the blue box moves the view.
    const auto box = talas::app::vcsNavigator(extent, 0, 0, extent.width / 2, extent.height, 80);
    require(box.has_value() && std::max(box->surface.width, box->surface.height) == 80
            && box->surface.width == std::lround(extent.width * box->scale)
            && box->view.left == 0 && box->view.width == std::lround(extent.width / 2 * box->scale)
            && box->view.height == box->surface.height
            && !talas::app::vcsNavigator(extent, 0, 0, extent.width, extent.height, 80).has_value(),
        "the box shows the surface's shape and the part the window shows, only while some is cut off");
    const auto dragged = talas::app::vcsNavigatorDragged(*box, extent, 0, 0, extent.width / 2, extent.height, 10, 5);
    const auto pastTheEdge = talas::app::vcsNavigatorDragged(*box, extent, 0, 0, extent.width / 2, extent.height, 500, -5);
    require(dragged.first == std::lround(10 / box->scale) && dragged.second == 0
            && pastTheEdge == std::pair<int, int>{extent.width - extent.width / 2, 0},
        "dragging the blue box moves the view over the surface, never past its edges");
    require(talas::app::vcsHandDragged(extent, 40, 0, extent.width / 2, extent.height, 30, 0) == std::pair<int, int>{10, 0}
            && talas::app::vcsHandDragged(extent, 0, 0, extent.width / 2, extent.height, -500, 20)
                == std::pair<int, int>{extent.width - extent.width / 2, 0},
        "the hand pushes the surface with the pointer, never past its edges");

    auto selection = talas::app::vcsBoxSelection(cleaned, {0, 0, 140, 110});
    require(selection == std::set<std::size_t>{0, 1}, "a box drawn from an empty area selects the widgets it touches");
    selection = talas::app::vcsShiftClicked(selection, 4);
    selection = talas::app::vcsShiftClicked(selection, 0);
    require(selection == std::set<std::size_t>{1, 4}, "Shift+Click extends the selection or takes one out");
    const auto moved = talas::app::vcsMoved(cleaned, selection, -100, 10);
    require(moved[1].left == cleaned[1].left - cleaned[1].left && moved[4].left == cleaned[4].left - cleaned[1].left
            && moved[1].top == cleaned[1].top + 10 && moved[0] == cleaned[0],
        "dragging moves the selected widgets together, no further than the surface's edge");
    const auto shrunk = talas::app::vcsResized(cleaned[0], talas::app::VcsCorner::BottomRight, -20, -60);
    require(shrunk == VcsRect{8, 8, 40, 40}, "dragging the black square at the lower right corner resizes the widget");
    require(talas::app::vcsResized(cleaned[0], talas::app::VcsCorner::BottomRight, -100, -100).width == 16,
        "a widget never shrinks below its minimum");
    require(talas::app::vcsCleanUp(moved.size(), 3, 60, 100) == cleaned,
        "Clean up throws away the widgets' moves and sizes");
    // Each type of control on a row of
    // its own; 16 columns put 16 faders in one row.
    using talas::app::VcsTypeRow;
    talas::project::VirtualControl small{"Pan", talas::expr::ValueDomain::Scalar};
    small.widget.style = talas::project::VirtualControlWidgetStyle::SmallFader;
    talas::project::VirtualControl toggle{"Mute", talas::expr::ValueDomain::Scalar};
    toggle.kind = talas::project::VirtualControlKind::Toggle;
    require(talas::app::vcsTypeRow(small) == VcsTypeRow::SmallFader && talas::app::vcsTypeRow(toggle) == VcsTypeRow::Toggle
            && talas::app::vcsTypeRow(talas::project::VirtualControl{"Amp", talas::expr::ValueDomain::Scalar}) == VcsTypeRow::Fader,
        "a control's type is its row");
    const auto typed = talas::app::vcsCleanUpByType({VcsTypeRow::Toggle, VcsTypeRow::Fader, VcsTypeRow::Fader}, 4, 60, 100);
    require(typed[1] == VcsRect{8, 8, 60, 100} && typed[2] == VcsRect{76, 8, 60, 100} && typed[0] == VcsRect{8, 116, 60, 100},
        "the faders should fill their row in order and the toggle begin a row of its own");
    const auto sixteen = talas::app::vcsCleanUpByType(std::vector<VcsTypeRow>(16, VcsTypeRow::Fader), 16, 60, 100);
    require(std::ranges::all_of(sixteen, [](const VcsRect& cell) { return cell.top == 8; })
            && talas::app::vcsCleanUpByType(std::vector<VcsTypeRow>(17, VcsTypeRow::Fader), 16, 60, 100)[16].top == 116,
        "16 columns should put all 16 Amp faders in one row, a seventeenth wrapping");
    std::vector<VcsRect> spread{{0, 0, 40, 40}, {0, 60, 40, 40}, {0, 130, 40, 40}};
    const auto squeezed = talas::app::vcsSqueezed(spread, {0, 1, 2}, true);
    require(squeezed[1].top == 42 && squeezed[2].top == 84, "Squeeze Vertical squeezes out the space between them");
    std::vector<VcsRect> row{{0, 0, 40, 40}, {70, 0, 40, 40}};
    require(talas::app::vcsSqueezed(row, {0, 1}, false)[1].left == 42, "Squeeze Horizontal squishes them together");
    // The numeric field above an
    // upright fader, at a horizontal one's right.
    require(talas::app::vcsFaderNumberPlacement(true, true) == talas::app::VcsNumberPlacement::Above
            && talas::app::vcsFaderNumberPlacement(false, true) == talas::app::VcsNumberPlacement::Right
            && talas::app::vcsFaderNumberPlacement(true, false) == talas::app::VcsNumberPlacement::None,
        "an upright fader's numeric field should be above it");
    require(talas::app::vcsSelectedValueText({"Volume"}) == "Volume" && talas::app::vcsSelectedValueText({"Dur1", "Dur2"}) == "More than one",
        "Selected Value names the widget, or More than one");
    require(talas::app::vcsRectFromEdges(10, 20, 70, 120) == std::optional<VcsRect>{VcsRect{10, 20, 60, 100}}
            && talas::app::vcsRectFromSize(10, 20, 60, 100) == talas::app::vcsRectFromEdges(10, 20, 70, 120)
            && !talas::app::vcsRectFromEdges(70, 20, 10, 120).has_value(),
        "Left / Top / Rt / Bot and Left / Top / Wd / Ht place a widget to the pixel");
}

// The top layout with every control, a layout for
// each Sound with dots for how far upstream it lies, layouts merged and
// widgets removed from one, and Arrange's Align and Distribute.
void testMultilevelLayouts()
{
    talas::project::ProjectDocument document;
    for (const auto* name : {"Pitch", "Mix", "Decay"})
        document.virtualControls.push_back({name, talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.5, 0.0, 1.0});
    document.graph.nodes.push_back({"grains", "talas.oscillator", {expression("frequency", "!Pitch * 100 hz")}});
    document.graph.nodes.push_back({"resonator", "talas.reverb", {expression("mix", "!Mix"), expression("decay", "!Decay * 2")}});
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({"grains", "audioOut", "resonator", "audioIn"});
    document.graph.connections.push_back({"resonator", "audioOut", "out", "audioIn"});
    document.virtualControlLayoutPages.push_back({"Level controls", {"Mix"}});
    const auto layouts = talas::app::vcsLayouts(document);
    require(layouts.size() == 4 && layouts[0].name == "resonator" && layouts[0].eventNames.size() == 3,
        "the top layout includes all of the controls from all of the Sounds");
    require(layouts[1].eventNames == std::vector<std::string>{"Mix", "Decay"} && layouts[2].eventNames == std::vector<std::string>{"Pitch"},
        "below it, a layout for each of the individual Sounds");
    require(talas::app::vcsLayoutLabel(layouts[2]) == "..grains" && talas::app::vcsLayoutLabel(layouts[1]).front() != '.',
        "dots tell how far upstream a Sound's layout lies");
    require(layouts[3].made && layouts[3].name == "Level controls", "a layout made in the editor joins the list");
    require(talas::app::vcsAdjacentLayout(4, 0, false) == 1 && talas::app::vcsAdjacentLayout(4, 0, true) == 0,
        "Shift with the arrows switches from layout to layout");
    const auto duplicate = talas::app::vcsDuplicatedLayout(layouts[0]);
    require(duplicate.name == "resonator copy" && duplicate.eventNames == layouts[0].eventNames,
        "Duplicate makes a layout of the editor's own with the same controls");
    const auto merged = talas::app::vcsMergedLayout("Merged layout", {layouts[2], layouts[3]});
    require(merged.name == "Merged layout" && merged.eventNames == std::vector<std::string>{"Pitch", "Mix"},
        "Merge into new layout makes a layout with the controls of both");
    // Shift+Click extends the layouts chosen to merge, one clicked
    // again taken out.
    auto chosen = talas::app::vcsLayoutsShiftClicked({}, 2, 3);
    require(chosen == std::set<std::size_t>{2, 3} && talas::app::vcsLayoutsShiftClicked(chosen, 2, 1) == std::set<std::size_t>{1, 2, 3}
            && talas::app::vcsLayoutsShiftClicked(chosen, 2, 3) == std::set<std::size_t>{2} && talas::app::vcsLayoutsShiftClicked({}, 2, 2)
            == std::set<std::size_t>{2},
        "Shift+Click should extend the selection of layouts");
    require(talas::app::vcsLayoutWithout(merged, {"Pitch"}).eventNames == std::vector<std::string>{"Mix"},
        "Remove selected widgets takes them out of the layout");

    using talas::app::VcsRect;
    const std::vector<VcsRect> rects{{0, 0, 40, 40}, {60, 10, 40, 30}, {100, 5, 40, 20}};
    const auto left = talas::app::vcsAligned(rects, {0, 1, 2}, talas::app::VcsAlignment::Left);
    require(left[1].left == 0 && left[2].left == 0, "Align puts a row or column of widgets in perfect line");
    const auto centres = talas::app::vcsAligned(rects, {0, 1, 2}, talas::app::VcsAlignment::HorizontalCentres);
    require(centres[1].top + centres[1].height / 2 == 20 && centres[2].top + centres[2].height / 2 == 20,
        "Align horizontal centres lines up their centres");
    const auto spread = talas::app::vcsDistributed(rects, {0, 1, 2}, true);
    require(spread[1].left + spread[1].width / 2 == 70, "Distribute horizontal (centers) spaces them evenly");
}

// Playing a Timeline, the VCS's top layer is the
// Timeline's layout, the next its Sounds' and below them their subparts',
// each indented by its layer.
void testTheTimelineLayouts()
{
    talas::project::ProjectDocument document;
    for (const auto* name : {"Rate", "Level", "Density"})
        document.virtualControls.push_back({name, talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.5, 0.0, 1.0});
    talas::project::ProjectSoundDefinition sample;
    sample.definitionId = "sample";
    sample.displayName = "Sample";
    sample.graph.nodes = {{"gain", "talas.vca", {expression("gain", "!Level")}}};
    sample.outputs = {{"audioOut", talas::core::PortType::Audio, "gain", "audioOut"}};
    talas::project::ProjectSoundDefinition vocoder;
    vocoder.definitionId = "vocoder";
    vocoder.displayName = "Vocoder";
    vocoder.graph.nodes = {{"osc", "talas.oscillator", {expression("frequency", "!Rate * 100 hz")}}};
    talas::project::ProjectNode part;
    part.instanceId = "samplePart";
    part.soundDefinitionId = "sample";
    vocoder.graph.nodes.push_back(part);
    vocoder.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    talas::project::ProjectSoundDefinition clicks;
    clicks.definitionId = "clicks";
    clicks.displayName = "Clicks";
    clicks.graph.nodes = {{"noise", "talas.noise", {expression("amplitude", "!Density")}}};
    clicks.outputs = {{"audioOut", talas::core::PortType::Audio, "noise", "audioOut"}};
    document.soundDefinitions = {sample, vocoder, clicks};
    const talas::project::ScoreTime zero{talas::project::ScoreTimeDomain::Seconds, 0.0, 0};
    const talas::project::ScoreTime second{talas::project::ScoreTimeDomain::Seconds, 1.0, 0};
    document.timeline.score.clips = {{"a", "trk1", "vocoder", zero, second, zero, zero, zero, 1},
        {"b", "trk2", "clicks", zero, second, zero, zero, zero, 1}, {"c", "trk2", "clicks", second, second, zero, zero, zero, 1}};
    const auto layouts = talas::app::vcsLayouts(document);
    require(layouts.size() == 4 && layouts[0].name == "Timeline" && layouts[0].depth == 0 && layouts[0].eventNames.size() == 3,
        "the first page is the layout for the Timeline itself, with the live controls of each of its Sounds");
    require(layouts[1].name == "Vocoder" && layouts[1].depth == 1 && layouts[1].eventNames == std::vector<std::string>{"Rate", "Level"}
            && layouts[3].name == "Clicks" && layouts[3].depth == 1,
        "on the next layer, the individual Sounds, each once");
    require(layouts[2].name == "Sample" && layouts[2].depth == 2 && layouts[2].eventNames == std::vector<std::string>{"Level"},
        "below the Sounds, layouts for their subparts");
    require(talas::app::vcsLayoutLabel(layouts[1]) == "..Vocoder" && talas::app::vcsLayoutLabel(layouts[2]) == "....Sample",
        "the names of the layouts are indented to show what layer they are on");
}

[[nodiscard]] std::vector<std::string> presetLabels(const talas::project::ProjectDocument& document, std::string_view layout)
{
    const auto* bank = talas::app::findVcsLayoutPresetBank(document, layout);
    return bank == nullptr ? std::vector<std::string>{} : talas::app::vcsPresetLabels(*bank);
}

[[nodiscard]] const talas::project::VirtualControl& shownControl(
    const std::vector<talas::project::VirtualControl>& controls, std::string_view name)
{
    const auto found = std::ranges::find(controls, name, &talas::project::VirtualControl::eventName);
    require(found != controls.end(), "the layout should show " + std::string{name});
    return *found;
}

// A "layout" is the list of presets with the types,
// placement and ranges of the widgets. A layout made in the editor keeps
// its own (format 155): Mix a potentiometer of its own range at its own
// place while the top layout keeps the shared fader; its own presets, the
// shared ones it showed first coming along; dice and presets of its own
// moving only its controls within its ranges; all of it saved with the
// project. A layout of an older project takes the shared ones.
void testALayoutKeepsItsOwnPresetsAndWidgets()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    for (const auto* name : {"Rate", "Mix", "Decay"})
        document.virtualControls.push_back({name, talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.25, 0.0, 1.0});
    document.graph.nodes.push_back({"grains", "talas.oscillator", {expression("frequency", "!Rate * 100 hz")}});
    document.graph.nodes.push_back({"resonator", "talas.delayWithFeedback", {expression("mix", "!Mix"), expression("feedback", "!Decay * 0.5")}});
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({"grains", "audioOut", "resonator", "audioIn"});
    document.graph.connections.push_back({"resonator", "audioOut", "out", "audioIn"});
    document.virtualControlLayoutPages.push_back({"Level controls", {"Mix", "Decay"}});
    const auto older = talas::app::vcsLayoutControls(document, "Level controls");
    require(older.size() == 3 && older[1].widget == document.virtualControls[1].widget && older[1].maximum == 1.0
            && talas::app::vcsLayoutArrangement(document, "Level controls") == talas::project::VirtualControlLayout::Automatic
            && talas::app::vcsLayoutPresetBankId(document, "Level controls") == talas::app::vcsPresetBankId,
        "a layout keeping nothing of its own, as an older project's, takes the shared widgets and presets");

    talas::runtime::ProjectRuntimeBridge bridge(std::move(document), registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    const auto staged = bridge.compileAndStage(settings);
    require(staged.ok(), "the project with its layouts should stage: " + staged.message);
    bridge.setPlaying(true);
    std::array<float, 4> output{};
    auto* outputPointer = output.data();
    require(bridge.render(&outputPointer, 1, output.size()), "the project should play");
    auto& editor = bridge.editor();

    // The widgets' types, places and ranges of the layout.
    talas::project::VirtualControlWidget knob;
    knob.style = talas::project::VirtualControlWidgetStyle::Potentiometer;
    knob.grid = 0.05;
    const auto knobbed = talas::app::vcsLayoutSettingsWidgetSet(editor.document(), "Level controls", "Mix", knob, 0.0, 0.5);
    require(knobbed.has_value() && editor.setVirtualControlLayoutSettings(*knobbed).ok(),
        "the layout should keep Mix's type and range of its own");
    const talas::project::VirtualControlBounds mixPlace{0, 0, 80, 80};
    const talas::project::VirtualControlBounds decayPlace{90, 0, 40, 160};
    require(editor.setVirtualControlLayoutSettings(talas::app::vcsLayoutSettingsPlaced(editor.document(), "Level controls",
                    talas::project::VirtualControlLayout::Fixed, {{"Mix", mixPlace}, {"Decay", decayPlace}}))
                .ok(),
        "the layout should keep its widgets' places of its own");
    const auto level = talas::app::vcsLayoutControls(editor.document(), "Level controls");
    require(shownControl(level, "Mix").widget.style == talas::project::VirtualControlWidgetStyle::Potentiometer
            && shownControl(level, "Mix").maximum == 0.5 && shownControl(level, "Mix").widget.bounds == mixPlace
            && shownControl(level, "Decay").widget.bounds == decayPlace && shownControl(level, "Decay").maximum == 1.0
            && talas::app::vcsLayoutArrangement(editor.document(), "Level controls") == talas::project::VirtualControlLayout::Fixed,
        "the layout holds its own widget types, placement and ranges");
    for (const auto* other : {"", "grains"})
    {
        const auto shown = talas::app::vcsLayoutControls(editor.document(), other);
        require(shownControl(shown, "Mix").widget == talas::project::VirtualControlWidget{} && shownControl(shown, "Mix").maximum == 1.0
                && talas::app::vcsLayoutArrangement(editor.document(), other) == talas::project::VirtualControlLayout::Automatic,
            std::string{"the layout "} + (*other == '\0' ? "at the top" : other) + " keeps the shared widgets");
    }
    require(!talas::app::vcsLayoutSettingsWidgetSet(editor.document(), "Level controls", "Mix", knob, 0.0, 2.0).has_value()
            && !talas::app::vcsLayoutSettingsWidgetSet(editor.document(), "", "Mix", knob, 0.0, 0.5).has_value(),
        "a layout's range lies within the Event Value's own, and the top layout keeps the shared widgets");
    require(bridge.adoptSnapshotAuthoringEdit().ok(), "a layout's own widgets play nothing, so the stage stays current");

    // The list of presets of the layout.
    const auto shared = talas::app::vcsPresetBank(editor.document(), registry, {});
    require(shared.ok() && editor.addSnapshotBank(*shared.bank).ok() && bridge.adoptSnapshotAuthoringEdit().ok(),
        "the surface's presets should be kept: " + shared.diagnostic);
    auto current = bridge.currentSnapshotScopeValues(shared.bank->scope);
    require(current.ok() && editor.storeSnapshot(talas::app::vcsPresetBankId, {"preset2", "Shared", current.values}).ok()
            && bridge.adoptSnapshotAuthoringEdit().ok(),
        "a preset should be taken on the top layout");
    require(presetLabels(editor.document(), "Level controls") == presetLabels(editor.document(), ""),
        "until it takes one of its own, a layout shows the shared presets");
    const auto own = talas::app::vcsLayoutPresetBank(editor.document(), registry, current.values, "Level controls", {"Mix", "Decay"});
    require(own.ok() && own.bank->bankId != talas::app::vcsPresetBankId && own.bank->displayName == "Level controls"
            && editor.addSnapshotBank(*own.bank).ok()
            && editor.setVirtualControlLayoutSettings(
                   talas::app::vcsLayoutSettingsWithPresets(editor.document(), "Level controls", own.bank->bankId)).ok()
            && bridge.adoptSnapshotAuthoringEdit().ok(),
        "the layout's own presets should be kept: " + own.diagnostic);
    const auto ownId = talas::app::vcsLayoutPresetBankId(editor.document(), "Level controls");
    require(ownId == own.bank->bankId && presetLabels(editor.document(), "Level controls")
            == std::vector<std::string>{"Default [1]", "Shared [2]"},
        "the layout's own list starts with the presets it showed");
    require(bridge.randomizeSnapshotBank(ownId, 21).ok(), "the layout's dice should roll");
    require(valueOf(bridge, "Rate") == 0.25 && valueOf(bridge, "Mix") <= 0.5 && valueOf(bridge, "Decay") != 0.25,
        "the layout's dice move only its own controls, within the ranges it shows them in");
    require(bridge.render(&outputPointer, 1, output.size()), "the dice's values should play");
    current = bridge.currentSnapshotScopeValues(own.bank->scope);
    const auto rolledMix = valueOf(bridge, "Mix");
    require(current.ok() && editor.storeSnapshot(ownId, {"preset3", "Levels", current.values}).ok()
            && bridge.adoptSnapshotAuthoringEdit().ok(),
        "the camera on the layout takes a preset of its own");
    require(presetLabels(editor.document(), "Level controls") == std::vector<std::string>{"Default [1]", "Shared [2]", "Levels [3]"}
            && presetLabels(editor.document(), "") == std::vector<std::string>{"Default [1]", "Shared [2]"}
            && presetLabels(editor.document(), "grains") == presetLabels(editor.document(), ""),
        "each layout has its own list of presets");
    require(bridge.render(&outputPointer, 1, output.size()) && bridge.randomizeSnapshotBank(talas::app::vcsPresetBankId, 22).ok(),
        "the top layout's dice should roll");
    const auto rolledRate = valueOf(bridge, "Rate");
    require(bridge.render(&outputPointer, 1, output.size()) && bridge.recallSnapshot(ownId, "preset3").ok()
            && valueOf(bridge, "Mix") == rolledMix && valueOf(bridge, "Rate") == rolledRate,
        "choosing the layout's preset sets its controls and leaves the others where they are");

    // Saved with the project, and refused by an older format.
    const auto text = talas::project::serializeProjectDocument(editor.document());
    const auto reopened = talas::project::parseProjectDocument(text);
    require(reopened.ok() && reopened.document.virtualControlLayoutSettings == editor.document().virtualControlLayoutSettings
            && presetLabels(reopened.document, "Level controls") == presetLabels(editor.document(), "Level controls")
            && shownControl(talas::app::vcsLayoutControls(reopened.document, "Level controls"), "Mix").widget.bounds == mixPlace,
        "the layout's presets and widgets should be saved with the project");
    auto older154 = text;
    const auto version = older154.find("\"version\": 156");
    require(version != std::string::npos, "the project should be written in the current format");
    older154.replace(version, 14, "\"version\": 154");
    const auto refused = talas::project::parseProjectDocument(older154);
    require(!refused.ok() && std::ranges::any_of(refused.errors, [](const auto& error) {
        return error.path == "$.virtualControlLayoutSettings";
    }), "a layout's own settings require format 155");

    // Renamed, the layout keeps them; locked again, the surface asks to save
    // the layout's changes, and discarding them leaves its presets named.
    const auto renamed = talas::app::vcsLayoutSettingsRenamed(editor.document().virtualControlLayoutSettings, "Level controls", "Levels");
    require(renamed.size() == 1 && renamed.front().layout == "Levels" && renamed.front().presetBankId == ownId,
        "a layout renamed keeps its own presets and widgets");
    const auto copied = talas::app::vcsLayoutSettingsCopied(editor.document().virtualControlLayoutSettings, "Level controls", "Copy");
    require(copied.size() == 2 && copied.back().layout == "Copy" && copied.back().widgets == copied.front().widgets
            && copied.back().arrangement == copied.front().arrangement && copied.back().presetBankId.empty()
            && talas::app::vcsLayoutSettingsWithout(copied, "Level controls").size() == 1,
        "a duplicate starts with the layout's widgets, and a layout removed takes its own along");
    auto unlockedDocument = editor.document();
    const auto unlocked = talas::app::vcsSurface(unlockedDocument);
    auto moved = unlockedDocument;
    moved.virtualControlLayoutSettings = talas::app::vcsLayoutSettingsPlaced(
        unlockedDocument, "Level controls", talas::project::VirtualControlLayout::Fixed, {{"Mix", decayPlace}});
    require(talas::app::vcsSurfaceChanged(unlocked, moved) && !talas::app::vcsSurfaceChanged(unlocked, unlockedDocument),
        "a widget moved on a layout of its own is a change to the surface");
    const auto restored = talas::app::vcsSurfaceRestored(moved, unlocked);
    require(restored.virtualControlLayoutSettings == unlockedDocument.virtualControlLayoutSettings,
        "the layout's changes discarded, its widgets are as they were unlocked");
}

// Each page of the Timeline's surface is a layout
// with its presets and the places, types and ranges of its widgets: a
// Sound's page keeps its own while the Timeline's page keeps the shared
// ones, each with its own list of presets.
void testTheTimelinePagesKeepTheirOwnLayouts()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    for (const auto* name : {"Rate", "Density"})
        document.virtualControls.push_back({name, talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.5, 0.0, 1.0});
    talas::project::ProjectSoundDefinition vocoder;
    vocoder.definitionId = "vocoder";
    vocoder.displayName = "Vocoder";
    vocoder.graph.nodes = {{"osc", "talas.oscillator", {expression("frequency", "!Rate * 100 hz")}}};
    vocoder.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    talas::project::ProjectSoundDefinition clicks;
    clicks.definitionId = "clicks";
    clicks.displayName = "Clicks";
    clicks.graph.nodes = {{"noise", "talas.noise", {expression("amplitude", "!Density")}}};
    clicks.outputs = {{"audioOut", talas::core::PortType::Audio, "noise", "audioOut"}};
    document.soundDefinitions = {vocoder, clicks};
    talas::project::ScoreTrack master;
    master.trackId = "master";
    master.displayName = "Master";
    master.kind = talas::project::ScoreTrackKind::Master;
    document.timeline.score.tracks = {master};
    for (const auto* id : {"trk1", "trk2"})
    {
        talas::project::ScoreTrack track;
        track.trackId = id;
        track.displayName = id;
        track.outputTrackId = "master";
        document.timeline.score.tracks.push_back(track);
    }
    const talas::project::ScoreTime zero{talas::project::ScoreTimeDomain::Seconds, 0.0, 0};
    const talas::project::ScoreTime second{talas::project::ScoreTimeDomain::Seconds, 1.0, 0};
    document.timeline.score.clips = {{"a", "trk1", "vocoder", zero, second, zero, zero, zero, 1},
        {"b", "trk2", "clicks", zero, second, zero, zero, zero, 1}};
    const auto layouts = talas::app::vcsLayouts(document);
    require(layouts.size() == 3 && layouts[0].name == "Timeline" && layouts[1].name == "Vocoder", "the Timeline's pages");

    talas::project::ProjectEditor editor{std::move(document), registry};
    talas::project::VirtualControlWidget horizontal;
    horizontal.style = talas::project::VirtualControlWidgetStyle::HorizontalFader;
    const auto styled = talas::app::vcsLayoutSettingsWidgetSet(editor.document(), "Vocoder", "Rate", horizontal, 0.25, 0.75);
    require(styled.has_value() && editor.setVirtualControlLayoutSettings(*styled).ok()
            && editor.setVirtualControlLayoutSettings(talas::app::vcsLayoutSettingsPlaced(editor.document(), "Vocoder",
                   talas::project::VirtualControlLayout::Fixed, {{"Rate", talas::project::VirtualControlBounds{10, 20, 160, 40}}})).ok(),
        "the Sound's page should keep its widget's type, range and place");
    const auto page = shownControl(talas::app::vcsLayoutControls(editor.document(), "Vocoder"), "Rate");
    const auto timeline = shownControl(talas::app::vcsLayoutControls(editor.document(), ""), "Rate");
    require(page.widget.style == talas::project::VirtualControlWidgetStyle::HorizontalFader && page.minimum == 0.25 && page.maximum == 0.75
            && page.widget.bounds == talas::project::VirtualControlBounds{10, 20, 160, 40},
        "the Sound's page includes the placement, types and ranges of its widgets");
    require(timeline.widget == talas::project::VirtualControlWidget{} && timeline.minimum == 0.0 && timeline.maximum == 1.0,
        "the Timeline's own page keeps the shared widgets");

    const auto timelinePresets = talas::app::vcsPresetBank(editor.document(), registry, {});
    require(timelinePresets.ok(), "the Timeline's presets should be made: " + timelinePresets.diagnostic);
    const auto addedTimelinePresets = editor.addSnapshotBank(*timelinePresets.bank);
    require(addedTimelinePresets.ok(), "the Timeline's presets should be kept: " + addedTimelinePresets.message);
    const auto pagePresets = talas::app::vcsLayoutPresetBank(editor.document(), registry, {}, "Vocoder", layouts[1].eventNames);
    require(pagePresets.ok() && editor.addSnapshotBank(*pagePresets.bank).ok()
            && editor.setVirtualControlLayoutSettings(
                   talas::app::vcsLayoutSettingsWithPresets(editor.document(), "Vocoder", pagePresets.bank->bankId)).ok(),
        "the Sound's page should keep presets of its own: " + pagePresets.diagnostic);
    auto values = timelinePresets.bank->snapshots.front().values;
    require(editor.storeSnapshot(pagePresets.bank->bankId, {"preset2", "high data rate", values}).ok(),
        "a preset should be stored with the Sound's page");
    require(presetLabels(editor.document(), "Vocoder") == std::vector<std::string>{"Default [1]", "high data rate [2]"}
            && presetLabels(editor.document(), "") == std::vector<std::string>{"Default [1]"},
        "each page includes its own list of presets");
    const auto* bank = talas::app::findVcsLayoutPresetBank(editor.document(), "Vocoder");
    require(bank != nullptr && std::ranges::all_of(bank->parameterPolicies, [](const talas::project::SnapshotParameterPolicy& policy) {
        return policy.locked == (policy.target.eventName != "Rate");
    }) && std::ranges::any_of(bank->parameterPolicies, [](const talas::project::SnapshotParameterPolicy& policy) {
        return policy.target.eventName == "Rate" && policy.range == talas::project::SnapshotParameterRange{0.25, 0.75};
    }), "the page's presets and dice reach only its own controls, within its ranges");
    const auto reopened = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(editor.document()));
    require(reopened.ok() && presetLabels(reopened.document, "Vocoder") == presetLabels(editor.document(), "Vocoder")
            && reopened.document.virtualControlLayoutSettings == editor.document().virtualControlLayoutSettings,
        "the pages' presets and widgets are saved along with the Timeline");
}

// A Sound from the Prototypes is made just when it
// is taken from them, the prototype as it comes, so it has no layouts saved
// with it, where a Sound kept with its project may have some.
void testPrototypeSoundsComeWithoutLayouts()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.virtualControls.push_back({"Rate", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.5, 0.0, 1.0});
    document.virtualControls.push_back({"Pitch", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.5, 0.0, 1.0});
    talas::project::ProjectSoundDefinition vocoder;
    vocoder.definitionId = "vocoder";
    vocoder.displayName = "Vocoder";
    vocoder.graph.nodes = {{"osc", "talas.oscillator", {expression("frequency", "!Rate * 100 hz")}}};
    vocoder.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    document.soundDefinitions = {vocoder};
    // The Sound kept with its project has a layout made for it.
    document.virtualControlLayoutPages = {{"Vocoder", {"Rate"}}};

    // Taken from the Prototypes, a Sound is the prototype as it comes; a
    // copy changed in the project does not change the next one taken.
    auto first = talas::app::soundForPrototype(document, registry, "talas.oscillator");
    require(first.has_value() && first->graph.nodes.size() == 1 && first->graph.nodes.front().parameters.empty(),
        "a Sound from the Prototypes is the prototype, made as it is taken");
    first->graph.nodes.front().parameters = {expression("frequency", "!Pitch * 100 hz")};
    document.soundDefinitions.push_back(*first);
    const auto second = talas::app::soundForPrototype(document, registry, "talas.oscillator");
    require(second.has_value() && second->definitionId != first->definitionId && second->graph.nodes.front().parameters.empty(),
        "the next Sound taken from the Prototypes is made afresh, not the copy changed");
    document.soundDefinitions.push_back(*second);
    const talas::project::ScoreTime zero{talas::project::ScoreTimeDomain::Seconds, 0.0, 0};
    const talas::project::ScoreTime second1{talas::project::ScoreTimeDomain::Seconds, 1.0, 0};
    document.timeline.score.clips = {{"a", "trk1", "vocoder", zero, second1, zero, zero, zero, 1},
        {"b", "trk2", first->definitionId, zero, second1, zero, zero, zero, 1},
        {"c", "trk3", second->definitionId, zero, second1, zero, zero, zero, 1}};

    // Only the Sound kept with its project has a layout saved with it; the
    // Sounds from the Prototypes have the surface's own pages alone.
    const auto layouts = talas::app::vcsLayouts(document);
    const auto made = std::ranges::count_if(layouts, [](const talas::app::VcsLayoutEntry& layout) { return layout.made; });
    require(made == 1 && layouts.back().made && layouts.back().name == "Vocoder",
        "the Sound kept with its project has its layout saved with it");
    for (const auto& layout : layouts)
    {
        if (layout.made)
            continue;
        require(layout.name != second->displayName,
            "a Sound from the Prototypes as it comes has no layout of its own");
        if (layout.name == first->displayName)
            require(layout.eventNames == std::vector<std::string>{"Pitch"} && layout.depth == 1,
                "the copy given a control has the surface's page for it, made as it plays, not a layout saved with it");
    }
}

// The texts and boxes among the widgets,
// one added beside them, text pasted filling the surface.
void testTextsAndBoxes()
{
    using Kind = talas::project::VirtualControlDecorationKind;
    require(talas::app::vcsDecorationIndex(talas::app::vcsDecorationName(0)) == std::optional<std::size_t>{0}
            && talas::app::vcsDecorationIndex(talas::app::vcsDecorationName(11)) == std::optional<std::size_t>{11}
            && !talas::app::vcsDecorationIndex("Cutoff") && !talas::app::vcsDecorationIndex("#decoration 0"),
        "each text or box is named so that no Event Value is");
    const auto text = talas::app::vcsAddedDecoration(Kind::Text, {0, 0, 400, 119}, "A Short Sequencer");
    require(text.kind == Kind::Text && text.text == "A Short Sequencer" && text.bounds.left == 408 && text.bounds.top == 8
            && text.bounds.width > 0 && text.bounds.height > 0,
        "Add Widget > Text places a text box beside the widgets");
    const auto box = talas::app::vcsAddedDecoration(Kind::Box, {}, {});
    require(box.kind == Kind::Box && box.bounds.left == 8 && box.bounds.width > text.bounds.width,
        "a box for grouping controls");
    const auto pasted = talas::app::vcsPastedText("Lyrics", {0, 0, 640, 300});
    require(pasted.bounds == talas::project::VirtualControlBounds{0, 0, 640, 300} && pasted.text == "Lyrics",
        "pasted text is at first the full size of the surface");
    const auto relabelled = talas::app::vcsDecorationRelabelled({box, text}, 1, "A Long Sequencer");
    require(relabelled.has_value() && relabelled->at(1).text == "A Long Sequencer" && relabelled->at(1).bounds == text.bounds
            && relabelled->at(0) == box && !talas::app::vcsDecorationRelabelled({box, text}, 0, "Box").has_value()
            && !talas::app::vcsDecorationRelabelled({box, text}, 2, "None").has_value(),
        "typed into its Widget Label, a text should show the words, and a box take none");
    // The right-most menu, Options..., adds, hides and
    // removes widgets.
    const auto& options = talas::app::vcsOptionsMenuItems();
    require(options == std::vector<std::string_view>{"Add Widget", "Edit Text...", "Hide Selected Widgets",
                           "Remove Selected Texts and Boxes"},
        "Options... should hold the options for adding, hiding and removing widgets");
}

// Locked again, the surface asks whether to save the
// changes made since it was unlocked. Its widgets' kinds, ranges, looks and
// places, the arrangement, layouts, texts and dormant widgets are changes;
// the controls' values are not. Discarded, the surface is as it was
// unlocked, each control's value kept within the range it takes back.
void testLockingAsksToSaveTheChanges()
{
    talas::project::ProjectDocument document;
    document.virtualControls = {
        {"Vol", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.5, 0.0, 1.0},
        {"Pan", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.25, 0.0, 1.0},
    };
    document.virtualControlLayout = talas::project::VirtualControlLayout::Fixed;
    document.virtualControls[0].widget.bounds = talas::project::VirtualControlBounds{0, 0, 40, 120};
    const auto unlocked = talas::app::vcsSurface(document);
    auto performed = document;
    performed.virtualControls[0].value = 0.9;
    require(!talas::app::vcsSurfaceChanged(unlocked, performed), "a value played is not a change to the surface");

    auto edited = performed;
    edited.virtualControls[0].widget.bounds = talas::project::VirtualControlBounds{60, 0, 40, 120};
    edited.virtualControls[0].maximum = 0.5;
    edited.virtualControls[0].value = 0.5;
    edited.virtualControls[1].kind = talas::project::VirtualControlKind::Toggle;
    edited.virtualControlLayoutPages = {{"Mine", {"Vol"}}};
    edited.virtualControlDecorations.push_back({talas::project::VirtualControlDecorationKind::Text, "A Short Sequencer", {0, 130, 100, 20}});
    std::ranges::reverse(edited.virtualControls);
    require(talas::app::vcsSurfaceChanged(unlocked, edited), "a widget moved, a range, a kind, a layout or a text is a change");
    // Changed since it was unlocked, the surface asks to save
    // the layout's changes before showing another; unchanged or locked, not.
    require(talas::app::vcsLayoutSwitchAsksToSave(unlocked, edited) && !talas::app::vcsLayoutSwitchAsksToSave(unlocked, performed)
            && !talas::app::vcsLayoutSwitchAsksToSave(std::nullopt, edited),
        "choosing another layout should first ask to save the changes to the one shown");

    const auto restored = talas::app::vcsSurfaceRestored(edited, unlocked);
    require(!talas::app::vcsSurfaceChanged(unlocked, restored) && restored.virtualControls[0].eventName == "Vol"
            && restored.virtualControls[0].widget.bounds == document.virtualControls[0].widget.bounds
            && restored.virtualControls[0].maximum == 1.0 && restored.virtualControls[0].value == 0.5
            && restored.virtualControls[1].kind == talas::project::VirtualControlKind::Fader
            && restored.virtualControlLayoutPages.empty() && restored.virtualControlDecorations.empty(),
        "the changes discarded should leave the surface as it was unlocked, the values as they are");
    auto narrowed = edited;
    narrowed.virtualControls[1].value = 0.75;
    auto widened = unlocked;
    widened.widgets[0].maximum = 0.5;
    require(talas::app::vcsSurfaceRestored(narrowed, widened).virtualControls[0].value == 0.5,
        "a value should be kept within the range its control takes back");
}

// An OscilloscopeDisplay named waveform shows the
// waveform of its Input in the surface as an oscilloscope would, labelled
// with the Sound's name. Clicking the button with
// inward-pointing arrows several times zooms the display way out.
void testOscilloscopeDisplaysOnTheSurface()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"output", "talas.audioOutput", {}}, {"waveform", "talas.oscilloscopeDisplay", {}},
        {"osc", "talas.oscillator", {expression("frequency", "4 hz"), expression("amplitude", "1")}}};
    document.graph.connections = {{"osc", "audioOut", "waveform", "audioIn"}, {"waveform", "audioOut", "output", "audioIn"}};
    require(talas::app::vcsOscilloscopeDisplays(document) == std::vector<talas::app::VcsDisplay>{{"waveform", "waveform"}},
        "the display should take the Sound's name, waveform, as its label in the surface");

    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 64.0;
    settings.maxBlockFrames = 16;
    settings.enableRealtimeAnalysisCapture = true;
    require(bridge.compileAndStage(settings).ok(), "the project with its display should stage");
    bridge.setPlaying(true);
    const auto capture = bridge.latestAnalysisCapture();
    const auto probe = capture != nullptr ? capture->preferredProbeIndex("waveform") : std::nullopt;
    require(probe.has_value(), "the display's Input should be captured for the surface");
    std::vector<double> scratch(capture->maxPayloadValues());
    std::vector<float> heard;
    talas::app::VcsScopeHistory history;
    for (int block = 0; block < 8; ++block)
    {
        std::array<float, 16> output{};
        auto* outputPointer = output.data();
        require(bridge.render(&outputPointer, 1, output.size()), "the project should play");
        heard.insert(heard.end(), output.begin(), output.end());
        if (const auto record = capture->copyLatestRecord(*probe, scratch))
            history.add(record->absoluteSamplePosition, std::span<const double>{scratch.data(), record->valueCount});
    }
    require(history.size() >= 64, "the display should keep the frames captured block after block");
    // Sixteen frames across sixteen columns: each column one frame of the
    // Input, as heard (the display passes its Input on).
    const auto trace = talas::app::vcsScopeColumns(history.latest(16), 16, 16);
    for (std::size_t column = 0; column < trace.size(); ++column)
    {
        require(trace[column].present && trace[column].low == trace[column].high
                && std::abs(trace[column].high - heard[heard.size() - 16 + column]) < 1.0e-6,
            "the display should show the waveform of its Input");
    }

    // The inward arrows, clicked several times, zoom way
    // out, twice the time across for each click; a column then holds whole
    // cycles of the 4 hz sine (16 frames each), so its trace is the swing.
    require(talas::app::vcsScopeButtonText(talas::app::VcsScopeButton::TimeOut) == ">|<",
        "the button zooming out has inward-pointing arrows");
    talas::app::VcsScopeZoom zoom{16, 1.0};
    for (int click = 0; click < 3; ++click)
        zoom = talas::app::vcsScopeZoomed(zoom, talas::app::VcsScopeButton::TimeOut);
    require(zoom.frames == 128, "each click on the inward arrows should show twice the time");
    const auto wide = talas::app::vcsScopeColumns(history.latest(zoom.frames), zoom.frames, 4);
    require(wide.back().present && wide.back().low < -0.95 && wide.back().high > 0.95,
        "zoomed out, a column should hold whole cycles of the waveform");
    for (int click = 0; click < 20; ++click)
        zoom = talas::app::vcsScopeZoomed(zoom, talas::app::VcsScopeButton::TimeOut);
    require(zoom.frames == talas::app::maximumVcsScopeFrames
            && talas::app::vcsScopeZoomed(zoom, talas::app::VcsScopeButton::TimeIn).frames == talas::app::maximumVcsScopeFrames / 2,
        "zooming out stops at the most the display keeps; the outward arrows zoom back in");
    const auto louder = talas::app::vcsScopeZoomed(talas::app::VcsScopeZoom{}, talas::app::VcsScopeButton::AmplitudeIn);
    require(louder.amplitude == 0.5 && talas::app::vcsScopeZoomed(louder, talas::app::VcsScopeButton::AmplitudeOut).amplitude == 1.0,
        "the buttons zoom the amplitude in and out too");
    // Zoomed out past one capture, frames the captures skipped are left out,
    // the time across the display kept true.
    talas::app::VcsScopeHistory gapped(64);
    gapped.add(0, std::vector<double>(8, 0.5));
    gapped.add(16, std::vector<double>(8, -0.5));
    gapped.add(12, std::vector<double>(12, -0.5));
    const auto spread = talas::app::vcsScopeColumns(gapped.latest(24), 24, 3);
    require(gapped.size() == 24 && gapped.end() == 24 && spread[0].present && spread[0].high == 0.5 && !spread[1].present
            && spread[2].present && spread[2].low == -0.5,
        "frames between two captures should be missing, not squeezed out");
}

// The surface is global, and changes whenever a
// different Sound is loaded: the project's controls with the project
// loaded, a Sound's own and those of the Sounds feeding it with that Sound
// loaded alone (Ctrl+Space in the signal flow, or the Info menu).
void testTheSurfaceFollowsTheLoadedSound()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    for (const auto* name : {"Pitch", "Mix", "Decay"})
        document.virtualControls.push_back({name, talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.5, 0.0, 1.0});
    document.graph.nodes.push_back({"grains", "talas.oscillator", {expression("frequency", "!Pitch * 100 hz")}});
    document.graph.nodes.push_back({"resonator", "talas.delayWithFeedback", {expression("mix", "!Mix"), expression("feedback", "!Decay * 0.5")}});
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({"grains", "audioOut", "resonator", "audioIn"});
    document.graph.connections.push_back({"resonator", "audioOut", "out", "audioIn"});
    document.virtualControlLayoutPages.push_back({"Level controls", {"Mix"}});
    require(talas::app::vcsLayouts(document).front().eventNames.size() == 3, "the project loaded, the surface shows all of its controls");

    const auto grains = talas::app::documentForControlSpace(document, registry, {"grains"}, true);
    require(grains.has_value(), "Ctrl+Space in the signal flow loads the selected Sound alone");
    const auto grainsLayouts = talas::app::vcsLoadedSoundLayouts(document, *grains, "grains");
    require(grainsLayouts.front().name == "grains" && grainsLayouts.front().eventNames == std::vector<std::string>{"Pitch"},
        "loading a different Sound should change the surface to that Sound's controls");
    require(grainsLayouts.back().made && grainsLayouts.back().name == "Level controls",
        "the layouts made in the editor stay in the list");
    const auto resonator = talas::app::documentForControlSpace(document, registry, {"resonator"}, true);
    require(resonator.has_value(), "another Sound loads alone in its turn");
    const auto resonatorLayouts = talas::app::vcsLoadedSoundLayouts(document, *resonator, "resonator");
    require(resonatorLayouts.front().name == "resonator"
            && resonatorLayouts.front().eventNames == std::vector<std::string>{"Pitch", "Mix", "Decay"}
            && std::ranges::any_of(resonatorLayouts,
                [](const talas::app::VcsLayoutEntry& layout) { return layout.name == "grains" && layout.depth == 1; }),
        "a Sound loaded shows its controls and those of the Sounds feeding it, each with a layout");
    const auto shown = talas::app::documentWithInfoDisplay(document, registry, "grains", talas::app::InfoDisplay::Oscilloscope);
    require(shown.has_value() && shown->loadedSound == "grains", "Info > Oscilloscope loads the selected Sound alone");
    const auto shownLayouts = talas::app::vcsLoadedSoundLayouts(document, shown->document, shown->loadedSound);
    require(shownLayouts.front().name == "grains" && shownLayouts.front().eventNames == std::vector<std::string>{"Pitch"},
        "the surface should take the Sound the Info menu loaded, named for it rather than for its display");
    auto another = document;
    another.graph.nodes.erase(another.graph.nodes.begin());
    require(talas::app::vcsLoadedSoundLayouts(another, *grains, "grains").front().eventNames.size() == 3,
        "a Sound no longer in the project leaves the project's own surface");
}

// Unlocking the surface also opens the VCS Editor, a
// floating window that can be put wherever one likes on the screen so that
// it does not hide the surface; locking the surface closes it.
void testUnlockingOpensTheFloatingEditor()
{
    require(talas::app::vcsEditorWindowOpen(true) && !talas::app::vcsEditorWindowOpen(false),
        "unlocking the surface should open the VCS Editor, and locking it close it");
    using talas::app::VcsRect;
    const VcsRect screen{0, 0, 1440, 900};
    const VcsRect surface{100, 100, 800, 500};
    const auto first = talas::app::vcsEditorWindowPlace(surface, screen, 232, 150);
    require(!talas::app::vcsRectsOverlap(first, surface) && first.left == surface.right() + talas::app::vcsEditorWindowGap
            && first.top == surface.top && first.width == 232 && first.height == 150,
        "the editor should open beside the surface, hiding none of it");
    const VcsRect atTheRight{600, 100, 800, 500};
    const auto beside = talas::app::vcsEditorWindowPlace(atTheRight, screen, 232, 150);
    require(!talas::app::vcsRectsOverlap(beside, atTheRight) && beside.right() <= atTheRight.left,
        "with no room at the surface's right, the editor should open at its left");
    const auto covered = talas::app::vcsEditorWindowPlace(screen, screen, 232, 150);
    require(covered.left == screen.right() - 232 && covered.top == screen.top,
        "a surface filling the screen leaves the editor at the screen's right edge");
    const auto moved = talas::app::vcsEditorWindowMoved(first, screen, 300, 400);
    require(moved.left == first.left + 300 && moved.top == first.top + 400,
        "the editor should be repositioned wherever one likes on the screen");
    const auto pushed = talas::app::vcsEditorWindowMoved(moved, screen, 5000, -5000);
    require(pushed.right() == screen.right() && pushed.top == screen.top, "the editor should stay on the screen");
    require(talas::app::vcsEditorWindowPlace(surface, screen, 232, 150, moved) == moved,
        "unlocked again, the editor should open where it was put");
}

int main()
{
    try
    {
        testPresetsCameraAndDice();
        testArrangingTheSurface();
        testTextsAndBoxes();
        testMultilevelLayouts();
        testTheTimelineLayouts();
        testALayoutKeepsItsOwnPresetsAndWidgets();
        testTheTimelinePagesKeepTheirOwnLayouts();
        testPrototypeSoundsComeWithoutLayouts();
        testLockingAsksToSaveTheChanges();
        testOscilloscopeDisplaysOnTheSurface();
        testTheSurfaceFollowsTheLoadedSound();
        testUnlockingOpensTheFloatingEditor();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
