#include "talas/app/VirtualControlWidgetModel.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"
#include "talas/core/BuiltinNodes.h"

#include <cmath>
#include <iostream>
#include <optional>
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

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-9;
}

talas::project::VirtualControl control(double minimum, double maximum)
{
    talas::project::VirtualControl made;
    made.eventName = "NN1";
    made.minimum = minimum;
    made.maximum = maximum;
    made.value = minimum;
    return made;
}

// Min / Max / Grid; the grid is the step size, 0
// continuous; "set the grid size to 1" restricts NN faders to half steps.
// Taper, linear or logarithmic.
void testGridAndTaper()
{
    auto nn = control(12.0, 72.0);
    require(near(talas::app::virtualControlValueAt(nn, 0.5), 42.0) && near(talas::app::virtualControlValueAt(nn, 0.51), 42.6),
        "a fader of grid 0 should be continuous between its minimum and maximum");
    nn.widget.grid = 1.0;
    require(near(talas::app::virtualControlValueAt(nn, 0.51), 43.0) && near(talas::app::quantizedVirtualControlValue(nn, 60.4), 60.0),
        "a grid of 1 should keep the fader on whole half steps");
    nn.widget.grid = 25.0;
    require(near(talas::app::quantizedVirtualControlValue(nn, 72.0), 62.0),
        "a grid's last step should stay within the maximum");
    auto frequency = control(100.0, 10000.0);
    frequency.widget.taper = talas::project::VirtualControlTaper::Log;
    require(near(talas::app::virtualControlValueAt(frequency, 0.5), 1000.0)
            && near(talas::app::virtualControlPositionOf(frequency, 1000.0), 0.5),
        "a log taper should move by equal ratios");
    // A log fader's grid in (0,1) steps its travel by that
    // fraction.
    frequency.widget.grid = 0.25;
    require(near(talas::app::virtualControlValueAt(frequency, 0.3), 100.0 * std::pow(100.0, 0.25))
            && near(talas::app::quantizedVirtualControlValue(frequency, 950.0), 1000.0)
            && near(talas::app::virtualControlValueAt(frequency, 0.95), 10000.0),
        "a log fader's grid should be a fraction of its travel");
    auto bipolar = control(-1.0, 1.0);
    bipolar.widget.taper = talas::project::VirtualControlTaper::Log;
    require(near(talas::app::virtualControlValueAt(bipolar, 0.75), 0.5), "a log taper below 0 should stay linear");
}

void testLabelsTabsAndText()
{
    auto labelled = control(0.0, 1.0);
    require(talas::app::virtualControlLabel(labelled) == "NN1", "a widget's label should default to its Event Value's name");
    labelled.widget.label = "First note";
    require(talas::app::virtualControlLabel(labelled) == "First note", "a Widget Label should replace the name");
    require(talas::app::vcsAbbreviatedName("Frequency", 9) == "Frequency" && talas::app::vcsAbbreviatedName("Frequency", 6) == "Frqncy"
            && talas::app::vcsAbbreviatedName("Frequency", 4) == "Frqn" && talas::app::vcsAbbreviatedName("Amp", 1) == "A",
        "a long name should be abbreviated to fit, its vowels going from the end first");
    auto a = control(0.0, 1.0);
    a.widget.tab = "Reverb";
    auto b = control(0.0, 1.0);
    auto c = control(0.0, 1.0);
    c.widget.tab = "Grains";
    require(talas::app::virtualControlTabs({a, b, c, a}) == std::vector<std::string>{"", "Reverb", "Grains"},
        "the surface's tabs should be the main one first, then each in the order of its first widget");
    require(talas::app::parseVirtualControlWidgetStyle("potentiometer") == talas::project::VirtualControlWidgetStyle::Potentiometer
            && !talas::app::parseVirtualControlWidgetStyle("slider").has_value()
            && talas::app::virtualControlColourText(0x00ff00u) == "#00ff00"
            && talas::app::parseVirtualControlColour("#00FF00") == std::optional<std::optional<std::uint32_t>>{0x00ff00u}
            && talas::app::parseVirtualControlColour("") == std::optional<std::optional<std::uint32_t>>{std::optional<std::uint32_t>{}}
            && !talas::app::parseVirtualControlColour("green").has_value(),
        "styles and colours should read and write as the editor's fields show them");
}

// The VCS Editor's options are edits of the project, saved in format 97.
void testWidgetEditsAndFormat()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectEditor editor(talas::project::ProjectDocument{}, registry);
    talas::project::VirtualControl nn = control(12.0, 72.0);
    nn.value = 60.0;
    if (!editor.addVirtualControl(nn).ok())
        throw std::runtime_error("add a control");
    talas::project::VirtualControlWidget widget;
    widget.style = talas::project::VirtualControlWidgetStyle::HorizontalFader;
    widget.showNumber = false;
    widget.label = "Start";
    widget.onColour = 0x00ff00u;
    widget.offColour = 0x004000u;
    widget.grid = 1.0;
    widget.tab = "Sequencer";
    require(editor.setVirtualControlWidget("NN1", widget).ok() && editor.document().virtualControls.front().widget == widget,
        "the VCS Editor should set a widget's options");
    require(editor.undo().ok() && editor.document().virtualControls.front().widget == talas::project::VirtualControlWidget{},
        "one undo should take the options back");
    require(editor.redo().ok(), "redo the options");
    auto badGrid = widget;
    badGrid.grid = 100.0;
    auto badTaper = widget;
    badTaper.taper = talas::project::VirtualControlTaper::Log;
    auto badLabel = widget;
    badLabel.label = "a\nb";
    require(!editor.setVirtualControlWidget("NN1", badGrid).ok() && !editor.setVirtualControlWidget("Missing", widget).ok()
            && !editor.setVirtualControlWidget("NN1", badLabel).ok(),
        "a grid beyond the range, an unknown control and a label with a line break should be refused");
    require(editor.setVirtualControlRange("NN1", 48.0, 59.0).ok() && editor.document().virtualControls.front().value == 59.0
            && !editor.setVirtualControlRange("NN1", 2.0, 1.0).ok()
            && !editor.setVirtualControlRange("NN1", 0.0, 0.5).ok(),
        "Min / Max should keep the value within them and stay wider than the grid");
    (void)editor.setVirtualControlRange("NN1", 1.0, 72.0);
    require(editor.setVirtualControlWidget("NN1", badTaper).ok(), "a log taper above 0 should be accepted");
    const auto saved = talas::project::serializeProjectDocument(editor.document());
    require(saved.find("\"version\": 156") != std::string::npos && saved.find("\"widget\"") != std::string::npos
            && saved.find("\"onColour\": \"#00ff00\"") != std::string::npos,
        "the current format should save the widget options");
    const auto loaded = talas::project::parseProjectDocument(saved);
    require(loaded.ok() && loaded.document.virtualControls.front().widget == badTaper,
        "the options should load as they were saved");
    auto older = saved;
    older.replace(older.find("\"version\": 156"), 13, "\"version\": 96");
    require(!talas::project::parseProjectDocument(older).ok(), "format 96 should refuse widget options");
    talas::project::ProjectDocument plain;
    plain.virtualControls.push_back(control(0.0, 1.0));
    require(talas::project::serializeProjectDocument(plain).find("\"widget\"") == std::string::npos,
        "a widget of default options should save nothing new");
}

// Format 125: an arranged surface keeps each
// widget's place and whether it fits the window, as one undoable edit; an
// older project cannot carry them.
void testArrangedWidgetsKeepTheirPlaces()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectEditor editor(talas::project::ProjectDocument{}, registry);
    require(editor.addVirtualControl(control(12.0, 72.0)).ok(), "add a control");
    const talas::project::VirtualControlBounds place{10, 20, 60, 120};
    require(editor.arrangeVirtualControls(talas::project::VirtualControlLayout::Fixed, {{"NN1", place}}).ok()
            && editor.document().virtualControls.front().widget.bounds == std::optional{place}
            && editor.document().virtualControlLayout == talas::project::VirtualControlLayout::Fixed,
        "an unlocked surface keeps its widgets where they were put");
    require(!editor.arrangeVirtualControls(talas::project::VirtualControlLayout::Fixed, {{"NN1", talas::project::VirtualControlBounds{0, 0, 0, 10}}}).ok()
            && !editor.arrangeVirtualControls(talas::project::VirtualControlLayout::Fixed, {{"Missing", place}}).ok(),
        "a widget of no size or an unknown one is refused");
    const auto saved = talas::project::serializeProjectDocument(editor.document());
    require(saved.find("\"bounds\": [10, 20, 60, 120]") != std::string::npos && saved.find("\"virtualControlLayout\": \"fixed\"") != std::string::npos,
        "the current format writes a widget's place and the layout");
    const auto loaded = talas::project::parseProjectDocument(saved);
    require(loaded.ok() && loaded.document.virtualControls.front().widget == editor.document().virtualControls.front().widget
            && loaded.document.virtualControlLayout == talas::project::VirtualControlLayout::Fixed,
        "the arrangement loads as it was saved");
    auto older = saved;
    older.replace(older.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 124");
    require(!talas::project::parseProjectDocument(older).ok(), "format 124 cannot carry an arrangement");
    require(editor.undo().ok() && !editor.document().virtualControls.front().widget.bounds.has_value()
            && editor.document().virtualControlLayout == talas::project::VirtualControlLayout::Automatic,
        "one undo takes the arrangement back, the surface automatic again");
}

// Format 126: the layouts made in the editor are
// kept with the project, as one undoable edit; an older project has none.
void testMadeLayoutsAreKept()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectEditor editor(talas::project::ProjectDocument{}, registry);
    require(editor.addVirtualControl(control(12.0, 72.0)).ok(), "add a control");
    require(editor.setVirtualControlLayoutPages({{"Level controls", {"NN1"}}}).ok()
            && editor.document().virtualControlLayoutPages.size() == 1,
        "a layout renamed Level controls is kept");
    require(!editor.setVirtualControlLayoutPages({{"", {"NN1"}}}).ok(), "a layout needs a name");
    const auto saved = talas::project::serializeProjectDocument(editor.document());
    require(saved.find("\"virtualControlLayoutPages\"") != std::string::npos, "the current format writes the layouts");
    const auto loaded = talas::project::parseProjectDocument(saved);
    require(loaded.ok() && loaded.document.virtualControlLayoutPages == editor.document().virtualControlLayoutPages,
        "the layouts load as they were saved");
    auto older = saved;
    older.replace(older.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 125");
    require(!talas::project::parseProjectDocument(older).ok(), "format 125 cannot carry layouts");
    require(editor.undo().ok() && editor.document().virtualControlLayoutPages.empty(), "one undo takes the layout back");
}

// Format 137: the surface's texts and
// boxes, and a Button (fill), are kept with the project.
void testTextsBoxesAndFilledButtons()
{
    using Kind = talas::project::VirtualControlDecorationKind;
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectEditor editor(talas::project::ProjectDocument{}, registry);
    auto sw = control(0.0, 1.0);
    sw.eventName = "sw03";
    sw.kind = talas::project::VirtualControlKind::Gate;
    require(editor.addVirtualControl(sw).ok(), "add a button");
    auto filled = sw.widget;
    filled.style = talas::project::VirtualControlWidgetStyle::ButtonFill;
    filled.onColour = 0x00ff00u;
    filled.offColour = 0x006400u;
    require(editor.setVirtualControlWidget("sw03", filled).ok(), "the widget type Button (fill), green on, dark green off");
    require(talas::app::parseVirtualControlWidgetStyle("buttonFill") == talas::project::VirtualControlWidgetStyle::ButtonFill
            && talas::app::virtualControlWidgetStyleName(talas::project::VirtualControlWidgetStyle::ButtonFill) == "buttonFill",
        "the VCS Editor names the type");
    const std::vector<talas::project::VirtualControlDecoration> decorations{
        {Kind::Text, "A Short Sequencer", {0, 0, 200, 32}}, {Kind::Box, {}, {10, 40, 240, 160}}};
    require(editor.setVirtualControlDecorations(decorations).ok() && editor.document().virtualControlDecorations == decorations,
        "a text labelling the surface and a box grouping its controls");
    require(!editor.setVirtualControlDecorations({{Kind::Box, {}, {0, 0, 0, 10}}}).ok()
            && !editor.setVirtualControlDecorations(std::vector<talas::project::VirtualControlDecoration>(65, decorations.front())).ok()
            && editor.setVirtualControlDecorations({{Kind::Text, "Lyrics\nfor the performer", {0, 0, 300, 120}}}).ok(),
        "a text or box has some size, a surface at most 64, a text new lines");
    require(editor.undo().ok() && editor.document().virtualControlDecorations == decorations, "one undo a step");
    const auto saved = talas::project::serializeProjectDocument(editor.document());
    require(saved.find("\"virtualControlDecorations\"") != std::string::npos && saved.find("\"buttonFill\"") != std::string::npos,
        "the current format writes the texts, boxes and the filled button");
    const auto loaded = talas::project::parseProjectDocument(saved);
    require(loaded.ok() && loaded.document.virtualControlDecorations == decorations
            && loaded.document.virtualControls.front().widget.style == talas::project::VirtualControlWidgetStyle::ButtonFill,
        "they load as they were saved");
    auto older = saved;
    older.replace(older.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 136");
    require(!talas::project::parseProjectDocument(older).ok(), "format 136 cannot carry texts, boxes or a filled button");
}

// Format 142: widgets copied and pasted into
// the surface take their controls' type, place, size and range and join the
// layout made in the editor; one with no control sleeps, out of sight, and
// a control of its name added later takes it.
void testPastedWidgetsAndDormantOnes()
{
    using Style = talas::project::VirtualControlWidgetStyle;
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectEditor editor(talas::project::ProjectDocument{}, registry);
    require(editor.addVirtualControl(control(12.0, 72.0)).ok() && editor.setVirtualControlLayoutPages({{"Pitch", {}}}).ok(),
        "add a control and a layout of none");
    auto rotary = control(0.0, 127.0);
    rotary.widget.style = Style::Potentiometer;
    rotary.widget.bounds = talas::project::VirtualControlBounds{10, 20, 60, 60};
    auto rate = control(0.0, 4.0);
    rate.eventName = "Rate";
    rate.value = 1.0;
    rate.widget.style = Style::HorizontalFader;
    rate.widget.bounds = talas::project::VirtualControlBounds{80, 20, 160, 32};
    rate.midiMapping = talas::project::MidiControlMapping{};
    require(editor.pasteVirtualControlWidgets({rotary, rate}, "Pitch").ok(), "paste the copied widgets");
    const auto& pasted = editor.document().virtualControls.front();
    require(pasted.widget.style == Style::Potentiometer && pasted.widget.bounds == rotary.widget.bounds
            && pasted.minimum == 0.0 && pasted.maximum == 127.0 && pasted.value == 12.0,
        "a pasted widget should give its control its type, place, size and range");
    require(editor.document().virtualControlLayoutPages.front().eventNames == std::vector<std::string>{"NN1"}
            && editor.document().virtualControlLayout == talas::project::VirtualControlLayout::Fixed,
        "the widgets should be pasted into the current layout, at their places");
    require(editor.document().virtualControls.size() == 1 && editor.document().dormantVirtualControls.size() == 1
            && editor.document().dormantVirtualControls.front().eventName == "Rate"
            && !editor.document().dormantVirtualControls.front().midiMapping.has_value(),
        "a widget with no control should be kept dormant, out of sight, its widget alone");
    const auto saved = talas::project::serializeProjectDocument(editor.document());
    const auto loaded = talas::project::parseProjectDocument(saved);
    require(saved.find("\"dormantVirtualControls\"") != std::string::npos && loaded.ok()
            && loaded.document.dormantVirtualControls.size() == 1
            && loaded.document.dormantVirtualControls.front().widget == rate.widget,
        "the dormant widget is saved with the project");
    auto older = saved;
    older.replace(older.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 141");
    require(!talas::project::parseProjectDocument(older).ok(), "format 141 cannot carry dormant widgets");
    auto later = control(0.0, 1.0);
    later.eventName = "Rate";
    require(editor.addVirtualControl(later).ok(), "add a control named Rate");
    const auto& woken = editor.document().virtualControls.back();
    require(woken.widget.style == Style::HorizontalFader && woken.widget.bounds == rate.widget.bounds && woken.maximum == 4.0
            && editor.document().dormantVirtualControls.empty(),
        "a control added later should take the dormant widget's type, place, size and range");
    require(editor.undo().ok() && editor.undo().ok() && editor.document().dormantVirtualControls.empty()
            && !editor.document().virtualControls.front().widget.bounds.has_value(),
        "one undo takes the paste back");
    auto unsuitable = control(1.0, 0.5);
    require(!editor.pasteVirtualControlWidgets({unsuitable}).ok() && !editor.pasteVirtualControlWidgets({}).ok(),
        "a widget whose range is empty, or nothing, is not pasted");
}

// A number typed into the fader's field sets it, and
// the fader jumps there; it stays on the grid within the range.
void testTypingANumberSetsTheFader()
{
    auto level = control(0.0, 10.0);
    level.widget.grid = 0.5;
    const auto typed = talas::app::quantizedVirtualControlValue(level, 3.3);
    require(near(typed, 3.5) && near(talas::app::virtualControlPositionOf(level, typed), 0.35),
        "a typed number should set the fader, and its position should jump to it");
    require(near(talas::app::quantizedVirtualControlValue(level, 42.0), 10.0)
            && near(talas::app::quantizedVirtualControlValue(level, -3.0), 0.0),
        "a typed number beyond the range should stop at its ends");
}

// The surface's
// Event Values left to right alphabetically, a new fader at the end only
// when its name sorts last.
void testControlsAreShownAlphabetically()
{
    std::vector<talas::project::VirtualControl> controls(4);
    controls[0].eventName = "Volume";
    controls[1].eventName = "bandwidth";
    controls[2].eventName = "Amp";
    controls[3].eventName = "Pan";
    require(talas::app::vcsAlphabeticalOrder(controls) == std::vector<std::size_t>{2, 1, 3, 0},
        "the controls should be ordered alphabetically, case aside");
    require(talas::app::vcsAlphabeticalOrder({}).empty(), "no controls, no order");
    // The controls of two Sounds whose names begin with Amp
    // are intermingled, whichever Sound each came from.
    controls[0].eventName = "Freq1";
    controls[1].eventName = "Amp01";
    controls[2].eventName = "AmpF1";
    controls[3].eventName = "Amp02";
    require(talas::app::vcsAlphabeticalOrder(controls) == std::vector<std::size_t>{1, 3, 2, 0},
        "two Sounds' Amp controls should be intermingled");
}

// Shift held while dragging a fader
// fine-tunes it, the value moving a tenth as far as the pointer.
void testShiftFineTunesTheFader()
{
    auto level = control(0.0, 10.0);
    require(near(talas::app::vcsFineTunedValue(level, 5.0, 0.5), 5.5),
        "half the fader's length with Shift held should move it a twentieth of its range");
    require(near(talas::app::vcsFineTunedValue(level, 5.0, -0.25), 4.75), "dragging down fine-tunes it down");
    require(near(talas::app::vcsFineTunedValue(level, 9.9, 1.0), 10.0), "fine-tuning stops at the range's end");
    // The Spectral Analysis tool's Unpitched fader fine-tunes alike.
    require(near(talas::app::fineTunedFaderValue(0.0, 1.0, 0.4, 0.5), 0.45)
            && near(talas::app::fineTunedFaderValue(0.0, 1.0, 0.98, 1.0), 1.0),
        "Shift should fine-tune the Unpitched fader a tenth as far, within its range");
    level.widget.grid = 0.5;
    require(near(talas::app::vcsFineTunedValue(level, 5.0, 0.2), 5.0) && near(talas::app::vcsFineTunedValue(level, 5.0, 0.5), 5.5),
        "fine-tuning keeps to the fader's grid");
}

// The text of an
// Annotation anywhere in the flow, a Sound's own too, tops the surface.
void testAnnotationsExplainTheSurface()
{
    talas::project::ProjectDocument document;
    const auto annotation = [](std::string id, std::string text) {
        talas::project::ProjectNode node{std::move(id), "talas.annotation", {}};
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = "text";
        parameter.text = std::move(text);
        node.parameters.push_back(parameter);
        return node;
    };
    talas::project::ProjectSoundDefinition inner;
    inner.definitionId = "inner";
    inner.graph.nodes = {annotation("note", "Play a C to start"), annotation("again", "Play a C to start")};
    document.soundDefinitions = {inner};
    talas::project::ProjectNode instance;
    instance.instanceId = "part";
    instance.soundDefinitionId = "inner";
    document.graph.nodes = {annotation("top", "A short sequencer"), {"osc", "talas.oscillator", {}}, instance, annotation("blank", "")};
    require(talas::app::vcsAnnotationTexts(document) == std::vector<std::string>{"A short sequencer", "Play a C to start"},
        "the surface should show each Annotation's text in the flow and its Sounds, once, empty ones left out");
}

// Command or Control and a
// vertical drag in a numeric entry box move it as an invisible fader would.
void testANumericEntryBoxIsAnInvisibleFader()
{
    auto level = control(0.0, 10.0);
    require(near(talas::app::vcsInvisibleFaderValue(level, 5.0, 0.25), 7.5)
            && near(talas::app::vcsInvisibleFaderValue(level, 5.0, -0.5), 0.0)
            && near(talas::app::vcsInvisibleFaderValue(level, 5.0, 2.0), 10.0),
        "a quarter of the invisible fader up should move a quarter of the range, within it");
    level.widget.grid = 1.0;
    require(near(talas::app::vcsInvisibleFaderValue(level, 5.0, 0.12), 6.0), "the invisible fader keeps to the grid");
}

// Tab selects the number at
// the top of the first fader, then moves between the numeric fields, round
// again from the last; Shift+Tab goes back.
void testTabMovesBetweenTheNumbers()
{
    require(talas::app::vcsTabbedNumber(3, std::nullopt, true) == std::optional<std::size_t>{0},
        "Tab with no number being typed selects the first fader's number");
    require(talas::app::vcsTabbedNumber(3, 0, true) == std::optional<std::size_t>{1}
            && talas::app::vcsTabbedNumber(3, 2, true) == std::optional<std::size_t>{0},
        "Tab moves to the next numeric field, from the last round to the first");
    require(talas::app::vcsTabbedNumber(3, 0, false) == std::optional<std::size_t>{2}
            && talas::app::vcsTabbedNumber(3, std::nullopt, false) == std::optional<std::size_t>{2},
        "Shift+Tab moves back");
    require(!talas::app::vcsTabbedNumber(0, std::nullopt, true), "a surface without numbers has none to select");
}

// A gate is on while it is held, like
// !Bypass, and off once released; a toggle switches on until switched off.
void testGatesAndTogglesAsTheirButtonsPress()
{
    auto gate = control(0.0, 1.0);
    gate.kind = talas::project::VirtualControlKind::Gate;
    require(talas::app::virtualControlButtonValue(gate, true) == std::optional<double>{1.0}
            && talas::app::virtualControlButtonValue(gate, false) == std::optional<double>{0.0},
        "a gate should be on while held and off once released");
    auto toggle = control(0.0, 1.0);
    toggle.kind = talas::project::VirtualControlKind::Toggle;
    const auto on = talas::app::virtualControlButtonValue(toggle, true);
    require(on == std::optional<double>{1.0} && !talas::app::virtualControlButtonValue(toggle, false).has_value(),
        "a toggle should switch on and stay on after release");
    toggle.value = *on;
    require(talas::app::virtualControlButtonValue(toggle, true) == std::optional<double>{0.0},
        "a toggle should switch off on its next press");
    require(!talas::app::virtualControlButtonValue(control(0.0, 1.0), true).has_value(), "a fader has no button");
}
}

int main()
{
    try
    {
        testGridAndTaper();
        testLabelsTabsAndText();
        testWidgetEditsAndFormat();
        testArrangedWidgetsKeepTheirPlaces();
        testMadeLayoutsAreKept();
        testTextsBoxesAndFilledButtons();
        testPastedWidgetsAndDormantOnes();
        testTypingANumberSetsTheFader();
        testTabMovesBetweenTheNumbers();
        testShiftFineTunesTheFader();
        testANumericEntryBoxIsAnInvisibleFader();
        testAnnotationsExplainTheSurface();
        testControlsAreShownAlphabetically();
        testGatesAndTogglesAsTheirButtonsPress();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "virtual control widget model tests passed\n";
    return 0;
}
