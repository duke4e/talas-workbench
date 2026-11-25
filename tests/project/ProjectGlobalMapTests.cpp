#include "talas/core/BuiltinNodes.h"
#include "talas/expr/Expression.h"
#include "talas/project/MidiDefaultMap.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectGlobalMap.h"
#include "talas/project/ProjectJson.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

using talas::project::GlobalMapEntry;

// ADR-0024: the Default map, written in the global map's syntax, is the
// map a project without its own hears.
void testDefaultMapText()
{
    const auto parsed = talas::project::parseProjectGlobalMap(talas::project::defaultGlobalMapText());
    require(parsed.ok() && parsed.entries.size() == talas::project::defaultMidiMapControllers.size() + 1,
        "the Default map's text should hold each of its names: " + parsed.error);
    for (std::size_t index = 0; index < talas::project::defaultMidiMapControllers.size(); ++index)
    {
        const auto& expected = talas::project::defaultMidiMapControllers[index];
        const auto& entry = parsed.entries[index];
        require(entry.eventName == expected.eventName && entry.controller == expected.controller && !entry.pitchBend
                && talas::project::globalMapControlKind(entry) == expected.kind && entry.minimum == 0.0
                && entry.maximum == 1.0 && !entry.channel.has_value(),
            "!" + std::string{expected.eventName} + " should follow its controller as the Default map does");
    }
    require(parsed.entries.back().eventName == "PitchBend" && parsed.entries.back().pitchBend
            && parsed.entries.back().minimum == -1.0,
        "!PitchBend should follow the pitch wheel from -1 to 1");
    // The Default map
    // gives the controllers memorable names such as !Frequency, on
    // MIDIController18.
    const auto frequency = std::ranges::find(parsed.entries, std::string{"Frequency"}, &GlobalMapEntry::eventName);
    require(frequency != parsed.entries.end() && frequency->controller == 18 && !frequency->pitchBend,
        "the Default map should name controller 18 !Frequency");
    talas::project::ProjectDocument document;
    require(talas::project::projectGlobalMapEntries(document) == parsed.entries,
        "a project without a global map should hear the Default map");
}

// The options of an association.
void testOptions()
{
    const auto parsed = talas::project::parseProjectGlobalMap(
        "\"My studio\"\n"
        "!Volume is: (`MIDIController07 channel: 5; min: 0.5 max: 0.75 grid: 0.05; taper: #log).\n"
        "\n"
        "!Brightness is: (`MIDIController64 displayAs: #toggle).\n"
        "!Hidden is: (`MIDIController3 displayAs: #nothing).\n"
        "!Wheel is: `MIDIPitchBend.\n");
    require(parsed.ok() && parsed.entries.size() == 4, "the map should read its four associations: " + parsed.error);
    const auto& volume = parsed.entries[0];
    require(volume.controller == 7 && volume.channel == std::optional<std::uint8_t>{5} && volume.minimum == 0.5
            && volume.maximum == 0.75 && volume.grid == 0.05 && volume.taper == talas::project::MidiMapperMapTaper::Log,
        "channel:, min: max: grid: and taper: should be read");
    require(talas::project::globalMapControlKind(parsed.entries[1]) == talas::project::VirtualControlKind::Toggle
            && parsed.entries[2].displayAs == talas::project::MidiMapperMapDisplayAs::Nothing
            && parsed.entries[3].pitchBend,
        "displayAs: should give the control's kind");
    GlobalMapEntry linear;
    linear.minimum = 100.0;
    linear.maximum = 500.0;
    linear.grid = 50.0;
    require(*talas::project::globalMapValue(linear, 0.0) == 100.0 && *talas::project::globalMapValue(linear, 0.3) == 200.0
            && *talas::project::globalMapValue(linear, 1.0) == 500.0,
        "a controller should be scaled to the range and kept on the grid");
    GlobalMapEntry log;
    log.minimum = 100.0;
    log.maximum = 10000.0;
    log.taper = talas::project::MidiMapperMapTaper::Log;
    require(std::abs(*talas::project::globalMapValue(log, 0.5) - 1000.0) < 1e-9, "a log taper should move by ratios");
    for (const auto* bad : {"!A is: `MIDIKeyNumber.", "!A is: `MIDIController200.", "Volume is: `MIDIController07.",
             "!A is: `MIDIController07.\n!A is: `MIDIController08.", "!A is: (`MIDIPitchBend min: 0 max: 1)."})
    {
        require(!talas::project::parseProjectGlobalMap(bad).ok(), std::string{"a map line should be refused: "} + bad);
    }
    // One controller number on two channels, two names.
    const auto channels = talas::project::parseProjectGlobalMap(
        "!Length is: (`MIDIController07 channel: 16).\n!Depth is: (`MIDIController07 channel: 15).\n");
    require(channels.ok() && channels.entries.size() == 2 && channels.entries[0].eventName == "Length"
            && channels.entries[0].controller == 7 && channels.entries[0].channel == std::optional<std::uint8_t>{16}
            && channels.entries[1].eventName == "Depth" && channels.entries[1].controller == 7
            && channels.entries[1].channel == std::optional<std::uint8_t>{15},
        "a controller on a channel should be a name of its own");
    const auto lined = talas::project::parseProjectGlobalMap("!A is: `MIDIController07.\n!B is: nonsense\n");
    require(!lined.ok() && lined.line == 2, "an error should name its line");
}

// Any legal variable name
// is another Event Source, which takes a range and a display as any
// source does; Escape learning a controller never names an Event Value of
// another source.
void testOtherEventSources()
{
    const auto parsed = talas::project::parseProjectGlobalMap(
        "!Brightness is: (`Glide min: 0 max: 10 grid: 1).\n!Cutoff is: (`Knob3 displayAs: #toggle).\n");
    require(parsed.ok() && parsed.entries.size() == 2 && parsed.entries[0].source == "Glide"
            && parsed.entries[0].minimum == 0.0 && parsed.entries[0].maximum == 10.0 && parsed.entries[0].grid == 1.0
            && parsed.entries[1].source == "Knob3"
            && talas::project::globalMapControlKind(parsed.entries[1]) == talas::project::VirtualControlKind::Toggle,
        "any legal variable name should be a source in the global map: " + parsed.error);
    require(*talas::project::globalMapValue(parsed.entries[0], 0.36) == 4.0,
        "another source's options should range its control");
    for (const auto* bad : {"!A is: `3Knob.", "!A is: `Knob_3.", "!A is: `MIDIKeyDown.", "!A is: (`Glide channel: 2)."})
    {
        require(!talas::project::parseProjectGlobalMap(bad).ok(), std::string{"a source should be refused: "} + bad);
    }
    talas::project::ProjectDocument document;
    document.globalMap = "!Brightness is: `MIDIController00.\n!Glow is: `Glide.\n";
    const auto entries = talas::project::projectGlobalMapEntries(document);
    require(entries.size() == 2 && entries[1].source == "Glide" && entries[1].controller == 0,
        "the project's map should keep another source");
    require(talas::project::EventValueLearning::controller(document, 0, 1) == "!Brightness",
        "controller 0 should name the Event Value MIDI reaches, not one of another source");
}

// A Val field's value with its Event Values given by name.
struct NamedValues
{
    std::vector<std::pair<std::string, double>> values;

    static bool resolve(void* user, std::string_view name, talas::expr::ExpressionValue& value) noexcept
    {
        const auto& named = static_cast<const NamedValues*>(user)->values;
        const auto found = std::ranges::find(named, name, &std::pair<std::string, double>::first);
        if (found == named.end())
            return false;
        value = {found->second, talas::expr::ValueDomain::Scalar};
        return true;
    }
};

double fieldValue(const std::string& source, const NamedValues& values,
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar)
{
    const auto parsed = talas::expr::parse(source, talas::expr::ExpressionLanguageVersion::ValV1);
    require(parsed.ok(), source + " should parse: "
        + (parsed.diagnostics.empty() ? std::string{} : parsed.diagnostics.front().message));
    talas::expr::EvaluationContext context;
    context.user = const_cast<NamedValues*>(&values);
    context.resolveEvent = NamedValues::resolve;
    const auto evaluated = talas::expr::evaluate(*parsed.expression, context, {domain});
    require(evaluated.ok(), source + " should evaluate: "
        + (evaluated.diagnostics.empty() ? std::string{} : evaluated.diagnostics.front().message));
    return evaluated.value.value;
}

const talas::project::ProjectParameterExpression& field(
    const talas::project::ProjectGraph& graph, std::string_view nodeId, std::string_view parameterId)
{
    const auto node = std::ranges::find(graph.nodes, nodeId, &talas::project::ProjectNode::instanceId);
    require(node != graph.nodes.end(), "the graph should hold " + std::string{nodeId});
    const auto parameter = std::ranges::find(node->parameters, parameterId,
        &talas::project::ProjectParameterExpression::parameterId);
    require(parameter != node->parameters.end() && parameter->expression.has_value(),
        std::string{nodeId} + " should have a field " + std::string{parameterId});
    return *parameter;
}

const talas::project::VirtualControl* control(const talas::project::ProjectDocument& document, std::string_view name)
{
    const auto found = std::ranges::find(document.virtualControls, name, &talas::project::VirtualControl::eventName);
    return found == document.virtualControls.end() ? nullptr : &*found;
}

// After Expand the red Event Values of the expanded
// Sound's fields have turned into blue Event Sources, the system names of
// the MIDI controllers the global map maps them to, for which the red names
// are mnemonics. Each Event Value the map holds is written as its source's
// expression (the range, taper and grid carried over), which gives the
// value the map gives the Event Value, so the Sound plays as before; its
// source's Virtual Control starts where the Event Value's was. Names the
// map does not hold (the Virtual Control Surface's alone), a controller on
// a channel of its own and the Sound's own Event Values stay red, as do the
// fields of a project whose language is not Val, and Inline keeps the
// names.
void testExpandTurnsEventValuesIntoEventSources()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    document.globalMap = "!Volume is: `MIDIController07.\n"
                         "!Cutoff is: (`MIDIController21 min: 100 max: 8000; taper: #log).\n"
                         "!Swell is: (`MIDIController24 min: 0 max: 2; taper: #log).\n"
                         "!Steps is: (`MIDIController22 min: -2 max: 2 grid: 0.5).\n"
                         "!Bend is: `MIDIPitchBend.\n"
                         "!Glow is: (`Glide min: 0 max: 10).\n"
                         "!Solo is: (`MIDIController23 channel: 5).\n";
    require(talas::project::parseProjectGlobalMap(document.globalMap).ok(), "the reverb's global map should read");
    const auto amplitudeText = std::string{
        "((!Volume * 0.5) + (!Steps * 0.01) + (!Bend * 0.01) + (!Glow * 0.01) + (!Swell * 0.01)) "
        "+ (!Solo * 0.01) + (!Private * 0.01) + (!Gain * 0.01)"};
    // The HarmonicResonator Sound, res, and the reverb holding it.
    talas::project::ProjectSoundDefinition res;
    res.definitionId = "res";
    res.displayName = "res";
    res.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "!Cutoff hz"}, {"amplitude", amplitudeText}}}};
    res.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    talas::project::ProjectSoundDefinition reverb;
    reverb.definitionId = "diy";
    reverb.displayName = "DIY reverb";
    talas::project::ProjectNode inner;
    inner.instanceId = "res1";
    inner.soundDefinitionId = "res";
    reverb.graph.nodes = {inner};
    reverb.outputs = {{"audioOut", talas::core::PortType::Audio, "res1", "audioOut"}};
    // !Gain is the reverb's own parameter, not the map's.
    talas::project::ProjectSoundExposedParameter gain;
    gain.parameterId = "gain";
    gain.displayName = "Gain";
    gain.defaultValue = {"gain", "1.0"};
    gain.bindings = {{talas::project::ProjectSoundParameterTarget::EventValue, {}, {}, "Gain",
        talas::expr::ValueDomain::Scalar}};
    reverb.exposedParameters = {gain};
    document.soundDefinitions = {res, reverb};
    talas::project::ProjectNode example;
    example.instanceId = "example";
    example.soundDefinitionId = "res";
    document.graph.nodes = {example, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"example", "audioOut", "out", "audioIn"}};
    require(talas::project::synchronizeProjectEventValueControls(document, registry).ok(),
        "the reverb's Event Values should have Virtual Controls");
    const auto setControl = [&](std::string_view name, double value) {
        const auto found = std::ranges::find(document.virtualControls, name, &talas::project::VirtualControl::eventName);
        require(found != document.virtualControls.end(), "!" + std::string{name} + " should have a Virtual Control");
        found->value = value;
    };
    setControl("Volume", 0.8);
    setControl("Cutoff", 1000.0);
    setControl("Steps", 1.0);
    setControl("Bend", -0.25);
    setControl("Glow", 5.0);
    setControl("Swell", 0.5);

    // The Sound file window: the copy shows the sources, the Sound keeps its
    // names.
    talas::project::ProjectEditor window(document, registry);
    const auto expanded = window.expandSoundDefinition("diy", "diyExpanded", "DIY reverb expanded");
    require(expanded.ok(), "DIY reverb should expand: " + expanded.message);
    const auto& copy = window.document().soundDefinitions.back();
    const auto& frequency = field(copy.graph, "res1_osc", "frequency");
    const auto& amplitude = field(copy.graph, "res1_osc", "amplitude");
    require(frequency.expression->find("`MIDIController21") != std::string::npos
            && frequency.expression->find("!Cutoff") == std::string::npos,
        "!Cutoff should have become `MIDIController21, not " + *frequency.expression);
    for (const auto* source : {"(`MIDIController07)", "`MIDIController22", "(`MIDIPitchBend)", "`Glide", "`MIDIController24"})
    {
        require(amplitude.expression->find(source) != std::string::npos,
            std::string{"the expanded amplitude should read "} + source + ": " + *amplitude.expression);
    }
    for (const auto* red : {"!Volume", "!Steps", "!Bend", "!Glow", "!Swell"})
    {
        require(amplitude.expression->find(red) == std::string::npos,
            std::string{""} + red + " should have turned into its Event Source");
    }
    for (const auto* red : {"!Solo", "!Private", "!Gain"})
    {
        require(amplitude.expression->find(red) != std::string::npos,
            std::string{red} + " (not a source a field names, the Surface's alone or the Sound's own) should stay red");
    }
    require(field(window.document().soundDefinitions[0].graph, "osc", "amplitude").expression == amplitudeText,
        "the Sound expanded should keep its Event Values");
    // The source expressions give the values the map gives the Event Values.
    const auto entries = talas::project::projectGlobalMapEntries(document);
    const auto entry = [&](std::string_view name) {
        return *std::ranges::find(entries, name, &GlobalMapEntry::eventName);
    };
    for (const auto raw : {0.0, 0.1, 0.37, 0.5, 0.81, 1.0})
    {
        const auto bend = raw * 2.0 - 1.0;
        const NamedValues sources{{{"cc07", raw}, {"cc21", raw}, {"cc22", raw}, {"cc24", raw},
            {"MIDIPitchBend", bend}, {"Glide", raw}, {"Solo", 0.3}, {"Private", 0.2}, {"Gain", 0.7}}};
        const NamedValues names{{{"Volume", *talas::project::globalMapValue(entry("Volume"), raw)},
            {"Cutoff", *talas::project::globalMapValue(entry("Cutoff"), raw)},
            {"Steps", *talas::project::globalMapValue(entry("Steps"), raw)},
            {"Swell", *talas::project::globalMapValue(entry("Swell"), raw)},
            {"Bend", *talas::project::globalMapValue(entry("Bend"), bend)},
            {"Glow", *talas::project::globalMapValue(entry("Glow"), raw)}, {"Solo", 0.3}, {"Private", 0.2},
            {"Gain", 0.7}}};
        require(std::abs(fieldValue(*amplitude.expression, sources) - fieldValue(amplitudeText, names)) < 1.0e-12,
            "the expanded amplitude should give what the Event Values gave at " + std::to_string(raw));
        const auto cutoff = fieldValue(*frequency.expression, sources, talas::expr::ValueDomain::Hertz);
        require(std::abs(cutoff - fieldValue("!Cutoff hz", names, talas::expr::ValueDomain::Hertz)) < 1.0e-9 * cutoff,
            "the expanded frequency should give what !Cutoff gave at " + std::to_string(raw));
    }
    // Each source's Virtual Control starts where its Event Value's was.
    const auto starts = [&](std::string_view name, double value, double minimum, double maximum) {
        const auto* found = control(window.document(), name);
        require(found != nullptr && std::abs(found->value - value) < 1.0e-12 && found->minimum == minimum
                && found->maximum == maximum,
            "the Virtual Control of " + std::string{name} + " should start at " + std::to_string(value)
                + (found == nullptr ? std::string{", not be missing"} : ", not " + std::to_string(found->value)));
    };
    starts("cc07", 0.8, 0.0, 1.0);
    starts("cc21", (std::log(1000.0) - std::log(100.0)) / (std::log(8000.0) - std::log(100.0)), 0.0, 1.0);
    starts("cc22", 0.75, 0.0, 1.0);
    starts("MIDIPitchBend", -0.25, -1.0, 1.0);
    starts("Glide", 0.5, 0.0, 1.0);
    require(std::abs(*talas::project::globalMapValue(entry("Swell"), control(window.document(), "cc24")->value) - 0.5)
            < 1.0e-12,
        "the log taper's source should start where !Swell was");
    require(window.undo().ok() && window.document().soundDefinitions.size() == 2 && control(window.document(), "cc07") == nullptr,
        "Expand should be one undo step");

    // Action > Expand in a signal flow: the project's and a Sound's.
    talas::project::ProjectEditor project(document, registry);
    const auto graphBefore = project.document().graph;
    const auto replaced = project.expandSoundInstance("example");
    require(replaced.ok() && field(project.document().graph, "example_osc", "amplitude").expression->find("(`MIDIController07)")
                != std::string::npos
            && control(project.document(), "cc07") != nullptr && control(project.document(), "cc07")->value == 0.8,
        "Expand in the project's signal flow should show the Event Sources: " + replaced.message);
    require(project.undo().ok() && project.document().graph == graphBefore, "Expand should be one undo step");
    talas::project::ProjectEditor editor(document, registry);
    const auto edited = editor.expandSoundInstance("diy", "res1");
    const auto& diy = editor.document().soundDefinitions[1];
    require(edited.ok() && field(diy.graph, "res1_osc", "amplitude").expression->find("`MIDIPitchBend") != std::string::npos
            && field(diy.graph, "res1_osc", "amplitude").expression->find("!Gain") != std::string::npos,
        "Expand in a Sound editor should show the sources, the Sound's own !Gain kept: "
            + edited.message);
    // Inline keeps the names, as does a project whose fields are not Val.
    talas::project::ProjectEditor inliner(document, registry);
    require(inliner.inlineSoundInstance("example").ok()
            && field(inliner.document().graph, "example_osc", "amplitude").expression == amplitudeText,
        "Inline should keep the Event Values");
    auto older = document;
    older.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV3;
    older.soundDefinitions[0].graph.nodes[0].parameters[1].expression = "!Volume * 0.5";
    older.soundDefinitions[0].graph.nodes[0].parameters[0].expression = "440 hz";
    older.virtualControls.clear();
    talas::project::ProjectEditor legacy(older, registry);
    require(legacy.expandSoundDefinition("diy", "diyExpanded", "DIY reverb expanded").ok()
            && field(legacy.document().soundDefinitions.back().graph, "res1_osc", "amplitude").expression
                == std::optional<std::string>{"!Volume * 0.5"},
        "a field that is not Val should keep its Event Values");
}

// The global map is the project's, saved in format 98 and edited as one
// undo step.
// The widgets written with their # or without,
// displayAs: toggle, gate and nothing, and #rotary and #pot, knobs in the
// Virtual Control Surface.
void testDocumentedWidgetSpellings()
{
    using talas::project::MidiMapperMapDisplayAs;
    const auto text = std::string{"!Brightness is: (`MIDIController64 displayAs: toggle).\n"
                                  "!Button is: (`MIDIController65 displayAs: gate).\n"
                                  "!Hidden is: (`MIDIController66 displayAs: nothing).\n"
                                  "!Fader5 is: (`MIDIController15 displayAs: #rotary).\n"
                                  "!Fader6 is: (`MIDIController16 displayAs: #pot).\n"};
    const auto parsed = talas::project::parseProjectGlobalMap(text);
    require(parsed.ok() && parsed.entries.size() == 5, "the widget lines should read: " + parsed.error);
    require(parsed.entries[0].displayAs == MidiMapperMapDisplayAs::Toggle
            && parsed.entries[1].displayAs == MidiMapperMapDisplayAs::Gate
            && parsed.entries[2].displayAs == MidiMapperMapDisplayAs::Nothing
            && parsed.entries[3].displayAs == MidiMapperMapDisplayAs::Rotary
            && parsed.entries[4].displayAs == MidiMapperMapDisplayAs::Pot,
        "displayAs: toggle, gate, nothing, #rotary and #pot should give their widgets");
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    document.globalMap = text;
    talas::project::ProjectNode node;
    node.instanceId = "sum";
    node.typeId = "talas.constant";
    node.parameters.push_back({"value", "!Fader5 + !Fader6 + !Brightness + !Button + !Hidden"});
    document.graph.nodes.push_back(std::move(node));
    const auto synchronized
        = talas::project::synchronizeProjectEventValueControls(document, talas::core::makeBuiltinNodeRegistry());
    const auto control = [&](std::string_view name) {
        const auto found = std::ranges::find(document.virtualControls, name, &talas::project::VirtualControl::eventName);
        return found == document.virtualControls.end() ? nullptr : &*found;
    };
    using talas::project::VirtualControlWidgetStyle;
    require(synchronized.ok() && control("Fader5") != nullptr
            && control("Fader5")->widget.style == VirtualControlWidgetStyle::Potentiometer
            && control("Fader6")->widget.style == VirtualControlWidgetStyle::Potentiometer
            && control("Brightness")->kind == talas::project::VirtualControlKind::Toggle
            && control("Button")->kind == talas::project::VirtualControlKind::Gate && control("Hidden") == nullptr,
        "the Virtual Control Surface should show knobs, a check box and a button, and hide !Hidden");
}

void testProjectGlobalMap()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectEditor editor(talas::project::ProjectDocument{}, registry);
    const std::string map = "!Brightness is: (`MIDIController74 min: 100 max: 1000).\n";
    require(editor.setGlobalMap(map).ok() && editor.document().globalMap == map, "the global map should be set");
    require(talas::project::projectGlobalMapEntries(editor.document()).size() == 1
            && talas::project::projectGlobalMapEntries(editor.document()).front().eventName == "Brightness",
        "a project's own map should replace the Default map");
    require(!editor.setGlobalMap("!Broken is: `Nothing nn").ok() && editor.document().globalMap == map,
        "a map that does not parse should be refused");
    require(editor.undo().ok() && editor.document().globalMap.empty(), "one undo should take the map back");
    require(editor.setGlobalMap(talas::project::defaultGlobalMapText()).ok() && editor.document().globalMap.empty(),
        "the Default map's own text should keep the Default map");
    (void)editor.setGlobalMap(map);
    const auto saved = talas::project::serializeProjectDocument(editor.document());
    require(saved.find("\"version\": 156") != std::string::npos && saved.find("\"globalMap\"") != std::string::npos,
        "the current format should save the global map");
    const auto loaded = talas::project::parseProjectDocument(saved);
    require(loaded.ok() && loaded.document.globalMap == map, "the global map should load as saved");
    auto older = saved;
    older.replace(older.find("\"version\": 156"), 13, "\"version\": 97");
    require(!talas::project::parseProjectDocument(older).ok(), "format 97 should refuse a global map");
    require(talas::project::serializeProjectDocument(talas::project::ProjectDocument{}).find("\"globalMap\"") == std::string::npos,
        "a project of the Default map should save no global map");
    // A Virtual Control of the map's name with its own MIDI mapping keeps it.
    auto document = editor.document();
    talas::project::VirtualControl control;
    control.eventName = "Brightness";
    control.midiMapping = talas::project::MidiControlMapping{1, 20};
    document.virtualControls.push_back(control);
    require(talas::project::projectGlobalMapEntries(document).empty(),
        "ADR-0024: a name the project maps itself should leave the global map");
}
}

// Escape in a field, then
// one key, types !KeyPitch; two keys at once !KeyDown, three !KeyVelocity,
// each once the last key is up; a controller its name in the map, the
// Default map's !Volume for controller 7, a project map's own name, else
// !cc23.
void testEscapeLearnsEventValues()
{
    talas::project::EventValueLearning learning;
    require(!learning.key(true).has_value() && learning.key(false) == std::optional<std::string>{"!KeyPitch"},
        "one key should type !KeyPitch");
    require(!learning.key(true).has_value() && !learning.key(true).has_value() && !learning.key(false).has_value()
            && learning.key(false) == std::optional<std::string>{"!KeyDown"},
        "two keys at once should type !KeyDown");
    (void)learning.key(true);
    (void)learning.key(true);
    (void)learning.key(true);
    (void)learning.key(false);
    (void)learning.key(false);
    require(learning.key(false) == std::optional<std::string>{"!KeyVelocity"},
        "three keys at once should type !KeyVelocity");
    require(!learning.key(false).has_value(), "a key let go before Escape should type nothing");
    // With the preferences asking for
    // the older names, one key types !Pitch; two and three keys as before.
    talas::project::EventValueLearning manual;
    require(!manual.key(true, true).has_value() && manual.key(false, true) == std::optional<std::string>{"!Pitch"},
        "one key should type !Pitch when the preferences ask for it");
    (void)manual.key(true, true);
    (void)manual.key(true, true);
    (void)manual.key(false, true);
    require(manual.key(false, true) == std::optional<std::string>{"!KeyDown"},
        "two keys should still type !KeyDown");
    (void)manual.key(true, true);
    (void)manual.key(true, true);
    (void)manual.key(true, true);
    (void)manual.key(false, true);
    (void)manual.key(false, true);
    require(manual.key(false, true) == std::optional<std::string>{"!KeyVelocity"},
        "three keys should still type !KeyVelocity");
    talas::project::ProjectDocument document;
    require(talas::project::EventValueLearning::controller(document, 7, 1) == "!Volume",
        "moving controller 7 should type the Default map's !Volume");
    require(talas::project::EventValueLearning::controller(document, 23, 1) == "!cc23",
        "a controller no map names should type its own name");
    // A space and s after the name typed make the fader's
    // value a number of seconds.
    const auto seconds = talas::expr::parse(
        talas::project::EventValueLearning::controller(document, 23, 1) + " s", talas::expr::ExpressionLanguageVersion::ValV1);
    talas::expr::EvaluationContext context;
    context.resolveEvent = [](void*, std::string_view name, talas::expr::ExpressionValue& value) noexcept {
        if (name != "cc23")
            return false;
        value = {0.25, talas::expr::ValueDomain::Scalar};
        return true;
    };
    const auto attack = seconds.ok() ? talas::expr::evaluate(*seconds.expression, context, {talas::expr::ValueDomain::Seconds})
                                     : talas::expr::EvaluationResult{};
    require(seconds.ok() && attack.ok() && std::abs(attack.value.value - 0.25) < 1.0e-12
            && attack.value.domain == talas::expr::ValueDomain::Seconds,
        "the fader's name and s should give its value in seconds");
    document.globalMap = "!Brightness is: (`MIDIController74 channel: 3).";
    require(talas::project::EventValueLearning::controller(document, 74, 3) == "!Brightness"
            && talas::project::EventValueLearning::controller(document, 74, 1) == "!cc74",
        "a project's map should name its controller on its channel");
}

// DisplayAs: #fader shows a controller as a
// fader and #smallFader as a small numeric box that is a fader as well;
// Event Values show as faders, small faders, gates or toggles.
void testFadersAndSmallFaders()
{
    const auto text = std::string{"!Brightness is: (`MIDIController64 displayAs: #fader).\n"
                                  "!Detail is: (`MIDIController65 displayAs: #smallFader).\n"};
    const auto parsed = talas::project::parseProjectGlobalMap(text);
    require(parsed.ok() && parsed.entries[0].displayAs == talas::project::MidiMapperMapDisplayAs::Fader
            && parsed.entries[1].displayAs == talas::project::MidiMapperMapDisplayAs::SmallFader,
        "#fader and #smallFader should be read");
    talas::project::ProjectDocument document;
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    document.globalMap = text;
    talas::project::ProjectNode node;
    node.instanceId = "sum";
    node.typeId = "talas.constant";
    node.parameters.push_back({"value", "!Brightness + !Detail + !Feedback"});
    document.graph.nodes.push_back(std::move(node));
    const auto synchronized = talas::project::synchronizeProjectEventValueControls(document, talas::core::makeBuiltinNodeRegistry());
    const auto control = [&](std::string_view name) {
        return &*std::ranges::find(document.virtualControls, name, &talas::project::VirtualControl::eventName);
    };
    require(synchronized.ok() && control("Brightness")->kind == talas::project::VirtualControlKind::Fader
            && control("Brightness")->widget.style != talas::project::VirtualControlWidgetStyle::SmallFader
            && control("Detail")->kind == talas::project::VirtualControlKind::Fader
            && control("Detail")->widget.style == talas::project::VirtualControlWidgetStyle::SmallFader,
        "a #fader should show as a fader and a #smallFader as a small fader");
    require(control("Feedback")->kind == talas::project::VirtualControlKind::Fader && control("Feedback")->widget.showNumber
            && control("Feedback")->widget.style == talas::project::VirtualControlWidgetStyle::Default,
        "an Event Value no map names should show as a fader with its value in the numeric field");
}

int main()
{
    try
    {
        testDefaultMapText();
        testOptions();
        testProjectGlobalMap();
        testDocumentedWidgetSpellings();
        testEscapeLearnsEventValues();
        testFadersAndSmallFaders();
        testOtherEventSources();
        testExpandTurnsEventValuesIntoEventSources();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "project global map tests passed\n";
    return 0;
}
