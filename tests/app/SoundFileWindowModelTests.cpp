#include "talas/app/DefaultSounds.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/app/VariableValuePrompt.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectVariables.h"
#include "talas/project/VariableLiterals.h"

#include <algorithm>
#include <iostream>
#include <map>
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

using Ids = std::vector<std::string>;

std::optional<talas::project::VariableLiteral> typedLiteral(std::string_view typed)
{
    auto literal = talas::project::parseVariableLiteral(typed);
    require(literal.has_value(), "the literal should parse: " + std::string{typed});
    return literal;
}

// A default Sound is kept for Variables and a default collection for
// SoundCollectionVariables. A Sound asks for the Variables no declaration
// names; Set default Sound takes the Sound selected, a prototype or a Sound
// of the signal flow with the Sounds feeding it; the Default Sound and
// Default Collection buttons give each Variable asked for the default.
void testDefaultSoundsForVariables()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"lead", "talas.variable", {}}, {"group", "talas.soundCollectionVariable", {}},
        {"noise", "talas.noise", {}}, {"filter", "talas.filter", {}}, {"named", "talas.variable", {}}};
    for (const auto& [node, variable] : std::vector<std::pair<std::size_t, std::string>>{{0, "Variable"}, {1, "inputs"}, {4, "source"}})
    {
        talas::project::ProjectParameterExpression id;
        id.parameterId = "variableId";
        id.text = variable;
        document.graph.nodes[node].parameters.push_back(id);
    }
    document.graph.connections = {{"noise", "audioOut", "filter", "audioIn"}};
    talas::project::ProjectSoundVariableDeclaration declared;
    declared.variableId = "source";
    declared.displayName = "source";
    document.soundVariableLibrary.variables.push_back(declared);
    const auto asked = talas::app::undeclaredSoundVariables(document);
    require(asked == std::vector<talas::app::UndeclaredSoundVariable>{{"Variable", false}, {"inputs", true}},
        "playing should ask for ?Variable and the collection, not a declared Variable");

    const auto factory = talas::app::factoryDefaultSound(registry);
    require(factory.graph.nodes.size() == 1 && factory.graph.nodes.front().typeId == "talas.oscillator" && factory.outputs.size() == 1,
        "a default Sound should be kept from the start");
    // Set default Sound on a Sound of the signal flow takes it and the Sounds
    // feeding it.
    const auto filtered = talas::app::defaultSoundOfNode(document, document.graph, "filter", registry);
    require(filtered.graph.nodes.size() == 2 && filtered.graph.connections.size() == 1 && filtered.outputs.size() == 1
            && filtered.outputs.front().nodeId == "filter",
        "the default Sound should be the Sound selected, with its inputs");
    talas::project::ProjectGraph prototype;
    prototype.nodes.push_back({"noise1", "talas.noise", {}});
    const auto fromPrototype = talas::app::defaultSoundOfNode(document, prototype, "noise1", registry);
    require(fromPrototype.graph.nodes.size() == 1 && fromPrototype.graph.nodes.front().typeId == "talas.noise",
        "a prototype selected should become the default Sound");

    const auto collection = std::vector<talas::project::ProjectSoundDefinition>{filtered, fromPrototype};
    const auto environment = talas::app::defaultSoundEnvironment(asked, fromPrototype, collection);
    require(environment.size() == 2 && environment[0].first == "Variable" && environment[0].second.size() == 1
            && environment[0].second.front().graph.nodes.front().typeId == "talas.noise" && environment[1].first == "inputs"
            && environment[1].second.size() == 2,
        "Default Sound gives a Variable the default Sound and Default Collection a collection the default collection");
    require(talas::app::defaultSoundEnvironment(asked, fromPrototype, {})[1].second.size() == 1,
        "with no collection set, a collection takes the default Sound");
}

// Each icon in the Sound file
// window is one of its Sounds, named as the Sound is.
void testTheIconsAreTheSounds()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition bucket;
    bucket.definitionId = "bucket";
    bucket.displayName = "Bucket Vocoder";
    talas::project::ProjectSoundDefinition plain;
    plain.definitionId = "plain";
    document.soundDefinitions = {bucket, plain};
    require(talas::app::soundFileIcons(document)
            == std::vector<talas::app::SoundFileIcon>{{"bucket", "Bucket Vocoder", false}, {"plain", "plain", false}},
        "an icon for each Sound, by its name");
    const auto editing = talas::app::soundFileIcons(document, std::nullopt, std::string{"plain"});
    require(!editing[0].beingEdited && editing[1].beingEdited,
        "the Sound open in the Sound editor should be marked as being edited");
}

// A double-click on a
// Sound's icon opens it in a Sound editor, on a collection opens its level.
void testDoubleClickingAnIcon()
{
    require(talas::app::soundFileDoubleClick({"bucket", "Bucket Vocoder", false}) == talas::app::SoundFileDoubleClick::EditSound,
        "a double-click on a Sound opens its Sound editor");
    require(talas::app::soundFileDoubleClick({"drums", "Drums", true}) == talas::app::SoundFileDoubleClick::OpenCollection,
        "a double-click on a collection shows the Sounds in it");
}

// One editor at a time on a Sound; a double-click
// on the Sound already being edited brings its Sound editor to the front
// rather than opening it again, while another Sound's opens its own.
void testDoubleClickingTheSoundBeingEdited()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition bucket;
    bucket.definitionId = "bucket";
    bucket.displayName = "Bucket Vocoder";
    talas::project::ProjectSoundDefinition plain;
    plain.definitionId = "plain";
    document.soundDefinitions = {bucket, plain};
    const auto icons = talas::app::soundFileIcons(document, std::nullopt, std::string{"bucket"});
    require(talas::app::soundFileDoubleClick(icons[0]) == talas::app::SoundFileDoubleClick::BringEditorToFront,
        "a double-click on the Sound being edited should bring its editor to the front");
    require(talas::app::soundFileDoubleClick(icons[1]) == talas::app::SoundFileDoubleClick::EditSound,
        "a double-click on another Sound should open its own editor");
    const auto none = talas::app::soundFileIcons(document);
    require(talas::app::soundFileDoubleClick(none[0]) == talas::app::SoundFileDoubleClick::EditSound,
        "with nothing being edited, a double-click opens the Sound's editor");
}

// The Sound editor's upper half is the
// signal flow diagram of the Sound being edited: that Sound's own Sounds
// and lines, not the project's or another Sound's.
void testTheSignalFlowOfTheSoundBeingEdited()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"projectTone", "talas.oscillator", {}}};
    talas::project::ProjectSoundDefinition bucket;
    bucket.definitionId = "bucket";
    bucket.displayName = "Bucket Vocoder";
    bucket.graph.nodes = {{"noise", "talas.noise", {}}, {"vocoder", "talas.vocoder", {}}};
    bucket.graph.connections = {{"noise", "audioOut", "vocoder", "audioIn"}};
    talas::project::ProjectSoundDefinition plain;
    plain.definitionId = "plain";
    plain.graph.nodes = {{"sine", "talas.oscillator", {}}};
    document.soundDefinitions = {bucket, plain};
    const auto* edited = talas::app::soundEditorSignalFlow(document, std::string{"bucket"});
    require(edited == &document.soundDefinitions[0].graph && edited->nodes.size() == 2
            && edited->nodes[1].instanceId == "vocoder" && edited->connections.size() == 1,
        "the Sound editor should show the signal flow of the Sound being edited");
    require(talas::app::soundEditorSignalFlow(document, std::string{"plain"}) == &document.soundDefinitions[1].graph,
        "another Sound's editor shows that Sound's own signal flow");
    require(talas::app::soundEditorSignalFlow(document, std::nullopt) == &document.graph,
        "editing no Sound of the file, the signal flow is the project's own");
    require(talas::app::soundEditorSignalFlow(document, std::string{"missing"}) == nullptr,
        "a Sound the project does not have has no signal flow to show");
}

// A click selects a Sound; Shift adds to the selection or
// takes from it; a box drawn round Sounds selects them all, with Shift
// added to or taken from the selection.
// The Sound file window asks for a
// lifted Sound's variables each time it plays it; the Sound editor keeps
// the values given in its environment.
void testTheSoundFileWindowAsksForVariablesEachTime()
{
    talas::project::ProjectDocument lifted;
    talas::project::ProjectParameterExpression frequency;
    frequency.parameterId = "frequency";
    frequency.expression = "?freq";
    lifted.graph.nodes = {{"tone", "talas.oscillator", {frequency}}};
    lifted.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    const std::vector<std::pair<std::string, double>> given{{"freq", 440.0}};
    using talas::app::VariablePlayOrigin;
    require(talas::app::variablesToAskFor(lifted, {}, VariablePlayOrigin::SoundEditor) == Ids{"freq"}
            && talas::app::variablesToAskFor(lifted, given, VariablePlayOrigin::SoundEditor).empty()
            && talas::app::variablesToAskFor(lifted, given, VariablePlayOrigin::SoundFileWindow) == Ids{"freq"},
        "the Sound file window should ask for the variables every time, the Sound editor only once");
}

void testSelectingSounds()
{
    require(talas::app::soundFileClick({"a", "b"}, "c", false) == Ids{"c"}, "a click selects the Sound");
    require(talas::app::soundFileClick({"a"}, "b", true) == Ids{"a", "b"}, "Shift+Click adds the Sound");
    require(talas::app::soundFileClick({"a", "b"}, "a", true) == Ids{"b"}, "Shift+Click takes a selected Sound out");
    require(talas::app::soundFileClick({"a"}, "", false).empty(), "a click on the background selects nothing");
    require(talas::app::soundFileClick({"a"}, "", true) == Ids{"a"}, "Shift+Click on the background keeps the selection");
    require(talas::app::soundFileBoxSelection({"a"}, {"b", "c"}, false) == Ids{"b", "c"},
        "a box selects all the Sounds within it");
    require(talas::app::soundFileBoxSelection({"a", "b"}, {"b", "c"}, true) == Ids{"a", "c"},
        "with Shift the box adds to the selection or takes from it");
    // Dragged with Control or Option, the Sounds themselves move.
    talas::project::ProjectDocument document;
    for (const auto* id : {"a", "b", "c", "d"})
    {
        document.soundDefinitions.emplace_back();
        document.soundDefinitions.back().definitionId = id;
    }
    const auto order = [](const std::optional<talas::project::ProjectDocument>& moved) {
        Ids ids;
        if (moved.has_value())
            for (const auto& definition : moved->soundDefinitions)
                ids.push_back(definition.definitionId);
        return ids;
    };
    require(order(talas::app::soundFileWindowMoved(document, {"d", "b"}, "a")) == Ids{"b", "d", "a", "c"}
            && order(talas::app::soundFileWindowMoved(document, {"a"}, "")) == Ids{"b", "c", "d", "a"}
            && !talas::app::soundFileWindowMoved(document, {"x"}, "a").has_value(),
        "Sounds moved should go, in their order, before the Sound dropped on, or last");
}

// Format 140: a Sound file holds
// Sounds and Sound collections, a collection Sounds and other collections;
// the window shows a level at a time, its collections then its Sounds.
void testSoundCollections()
{
    talas::project::ProjectDocument document;
    for (const auto* id : {"a", "b", "c"})
    {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = id;
        document.soundDefinitions.push_back(sound);
    }
    document.soundCollections = {{"drums", "Drums", {"b"}, {"loops"}}, {"loops", "Loops", {"c"}, {}}};
    using Icon = talas::app::SoundFileIcon;
    require(talas::app::soundFileIcons(document) == std::vector<Icon>{{"drums", "Drums", true}, {"a", "a", false}},
        "the top level shows its collections and the Sounds in none");
    require(talas::app::soundFileIcons(document, std::string{"drums"})
            == std::vector<Icon>{{"loops", "Loops", true}, {"b", "b", false}},
        "a collection holds Sounds and other collections");
    require(talas::app::soundFileParentCollection(document, "loops") == std::optional<std::string>{"drums"}
            && !talas::app::soundFileParentCollection(document, "drums").has_value(),
        "a collection's level above is the collection holding it");
}

// A Sound file opened as a custom
// prototype strip shows its collections as categories and its other Sounds
// under General.
void testCustomPrototypeStrips()
{
    talas::project::ProjectDocument file;
    for (const auto* id : {"bank", "eq", "voice"})
    {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = std::string{id} + " class";
        file.soundDefinitions.push_back(sound);
    }
    file.soundCollections = {{"filters", "new filters", {"bank", "eq"}, {}}};
    using Category = talas::app::CustomPrototypeCategory;
    require(talas::app::customPrototypeStrip(file)
            == std::vector<Category>{{"new filters", {{"bank", "bank class"}, {"eq", "eq class"}}}, {"General", {{"voice", "voice class"}}}},
        "the collections become categories, the other Sounds General");
}

void testTheIconsFillRows()
{
    require(talas::app::soundFileColumns(500, 120) == 4 && talas::app::soundFileColumns(50, 120) == 1,
        "as many columns as fit, at least one");
    require(talas::app::soundFileCell(5, 4) == talas::app::SoundFileCell{1, 1}, "row after row");
}
}

// A prototype dragged into the Sound file window
// is a new Sound, the prototype alone, numbered as a copy.
void testAPrototypeDroppedIsANewSound()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectDocument document;
    const auto first = talas::app::soundForPrototype(document, registry, "talas.oscillator");
    require(first.has_value() && first->graph.nodes.size() == 1 && first->graph.nodes.front().typeId == "talas.oscillator"
            && !first->outputs.empty() && first->outputs.front().nodeId == first->graph.nodes.front().instanceId
            && first->displayName == "Oscillator1",
        "the prototype should become a Sound of its own, its outputs the Sound's, numbered");
    document.soundDefinitions.push_back(*first);
    const auto second = talas::app::soundForPrototype(document, registry, "talas.oscillator");
    require(second.has_value() && second->definitionId != first->definitionId && second->displayName == "Oscillator2",
        "the number is the times the prototype was dragged in");
    require(!talas::app::soundForPrototype(document, registry, "talas.nothing").has_value(), "an unknown prototype makes no Sound");
    // With the numbers kept in the preferences, a prototype's
    // copies go on numbering from its last, in another project too, past a
    // name the project has already.
    std::map<std::string, unsigned> numbers{{"talas.oscillator", 73}};
    const auto counted = talas::app::soundForPrototype(talas::project::ProjectDocument{}, registry, "talas.oscillator", &numbers);
    auto named = document;
    named.soundDefinitions.back().displayName = "Oscillator75";
    const auto next = talas::app::soundForPrototype(named, registry, "talas.oscillator", &numbers);
    require(counted && counted->displayName == "Oscillator74" && counted->definitionId == "oscillator74" && next
            && next->displayName == "Oscillator76" && numbers.at("talas.oscillator") == 76
            && !talas::app::soundForPrototype(document, registry, "talas.nothing", &numbers) && numbers.size() == 1,
        "the number each prototype last gave should go on from the preferences");
    // A double-click on
    // these prototypes opens an editor on such a copy.
    for (const auto* typeId : {"talas.analogSequencer", "talas.spectrumInRam", "talas.grainCloud", "talas.harmonicResonator", "talas.noise"})
    {
        const auto copy = talas::app::soundForPrototype(document, registry, typeId);
        require(copy.has_value() && copy->displayName.back() == '1'
                && std::ranges::any_of(copy->graph.nodes, [&](const auto& node) { return node.typeId == typeId; }),
            std::string{""} + typeId + " should open as a numbered copy holding the prototype");
    }
    // The tutorials' prototypes join the project as Sounds.
    talas::project::ProjectEditor editor(talas::project::ProjectDocument{}, registry);
    for (const auto* typeId : {"talas.formantBankOscillator", "talas.attenuator", "talas.script", "talas.parameterTransformer",
             "talas.sample", "talas.diskSplicer"})
    {
        auto sound = talas::app::soundForPrototype(editor.document(), registry, typeId);
        const auto added = sound.has_value() ? editor.addSoundDefinition(std::move(*sound)) : talas::project::EditResult{};
        require(sound.has_value() && added.ok(), std::string{typeId} + " should join the project as a Sound: " + added.message);
    }
}

// SoundToGlobalController, double-clicked in the
// Prototypes, opens as a Sound of its own though it has no audio output, so
// that a Val program can be written into its Value field; an EventVariable
// program is taken there and a broken one refused.
void testASoundToGlobalControllerOpensForAProgram()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument start;
    start.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    talas::project::ProjectEditor editor(start, registry);
    auto sound = talas::app::soundForPrototype(editor.document(), registry, "talas.soundToGlobalController");
    require(sound.has_value() && sound->displayName.back() == '1' && sound->graph.nodes.size() == 1
            && sound->graph.nodes.front().typeId == "talas.soundToGlobalController" && sound->outputs.empty(),
        "SoundToGlobalController should open as a numbered Sound holding it alone");
    const auto definitionId = sound->definitionId;
    const auto nodeId = sound->graph.nodes.front().instanceId;
    const auto added = editor.addSoundDefinition(std::move(*sound));
    require(added.ok(), "the controller's Sound should join the project: " + added.message);
    const auto text = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    const auto named = editor.setParameterInSoundDefinition(definitionId, nodeId, text("generatedEvent", "!Result"));
    const auto program = editor.setParameterInSoundDefinition(definitionId, nodeId,
        text("value",
            "| count downCount | count := EventVariable new initialValue: -1. "
            "downCount := EventVariable new initialValue: 1. "
            "(!Sw01 true: ((downCount <+ (downCount - 0.1)), (count <~ (count + 0.1))) false: (downCount))"));
    require(named.ok() && program.ok(),
        "the Value field should take a Val program: " + named.message + program.message);
    const auto& definitions = editor.document().soundDefinitions;
    const auto opened = std::ranges::find(definitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
    require(opened != definitions.end() && !opened->graph.nodes.front().parameters.empty()
            && std::ranges::any_of(opened->graph.nodes.front().parameters,
                [](const talas::project::ProjectParameterExpression& parameter) {
                    return parameter.parameterId == "value" && parameter.text.has_value()
                        && parameter.text->starts_with("| count downCount |");
                }),
        "the program should be kept in the Value field");
    require(!editor.setParameterInSoundDefinition(definitionId, nodeId, text("value", "(!Sw01 true: (count <~ ")).ok(),
        "a broken program should be refused in the Value field");
    // A prototype with no audio output that plays only feeding another
    // Sound still makes none.
    require(!talas::app::soundForPrototype(editor.document(), registry, "talas.threshold").has_value(),
        "a Threshold, a control for other Sounds, should make no Sound of its own");
}

// Several Sounds selected play one after another,
// top to bottom and left to right, a collection's Sounds in its order.
void testSeveralSoundsPlayInTurn()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectDocument document;
    for (const auto* id : {"pad", "bell", "drum", "harp"})
    {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = id;
        sound.graph.nodes.push_back({"tone", "talas.oscillator", {}});
        sound.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
        document.soundDefinitions.push_back(std::move(sound));
    }
    document.soundCollections = {{"beats", "Beats", {"harp", "drum"}, {}}};
    const auto icons = talas::app::soundFileIcons(document);
    const auto ordered = talas::app::soundFileSelectionInOrder(icons, {"pad", "beats"});
    require(ordered == Ids{"beats", "pad"}, "the selection should go in the order its icons lie");
    const auto turn = talas::app::soundsInTurn(document, ordered);
    require(turn.has_value() && turn->graph.nodes.front().typeId == "talas.concatenation" && turn->graph.connections.size() == 3,
        "the Sounds should play one after another through a Concatenation");
    Ids played;
    for (const auto& line : turn->graph.connections)
        played.push_back(*std::ranges::find(turn->graph.nodes, line.fromNode, &talas::project::ProjectNode::instanceId)->soundDefinitionId);
    require(played == Ids{"harp", "drum", "pad"}, "a collection's Sounds should play in its order, then the next Sound");
    talas::project::ProjectEditor editor(document, registry);
    require(editor.addSoundDefinition(*turn).ok(), "the Sounds in turn should be a valid Sound");
    require(!talas::app::soundsInTurn(document, {"pad", "pad"}).has_value(), "one Sound alone should play as itself");
}

// A Sound
// dragged out of a signal flow into a Sound file window leaves a copy
// there, the Sound with every Sound feeding it, laid out as they were.
void testASoundDraggedOutOfTheSignalFlowIsACopy()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"noise", "talas.noise", {}}, {"tone", "talas.oscillator", {}}, {"brittle", "talas.vca", {}},
        {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"noise", "audioOut", "brittle", "audioIn"}, {"brittle", "audioOut", "out", "audioIn"},
        {"tone", "audioOut", "out", "rightIn"}};
    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(document, std::nullopt, "noise")->position = {40, 80};
    const auto description = talas::app::signalFlowSoundDragDescription(std::nullopt, "brittle");
    const auto drag = talas::app::signalFlowSoundDrag(description);
    require(drag == talas::app::SignalFlowSoundDrag{std::nullopt, "brittle"}
            && talas::app::signalFlowSoundDrag(talas::app::signalFlowSoundDragDescription(std::string{"voice"}, "osc"))
                == talas::app::SignalFlowSoundDrag{std::string{"voice"}, "osc"}
            && !talas::app::signalFlowSoundDrag("talas.signalFlowSound:0").has_value(),
        "the drag should carry the signal flow and the Sound");
    const auto copy = talas::app::soundCopiedFromSignalFlow(document, registry, *drag);
    require(copy.has_value() && copy->definition.displayName == "brittle" && copy->definition.graph.nodes.size() == 2
            && copy->definition.graph.connections == std::vector<talas::core::Connection>{{"noise", "audioOut", "brittle", "audioIn"}}
            && copy->definition.outputs.size() == 1 && copy->definition.outputs.front().nodeId == "brittle",
        "the copy should be the Sound with the Sounds feeding it, not the others");
    require(copy->layout.soundDefinitionId == std::optional<std::string>{copy->definition.definitionId}
            && std::ranges::any_of(copy->layout.nodes, [](const talas::project::ProjectNodePresentation& placed) {
                   return placed.nodeId == "noise" && placed.position.x == 40 && placed.position.y == 80;
               }),
        "the copy should be laid out as the signal flow was");
    talas::project::ProjectEditor editor(document, registry);
    require(editor.addSoundDefinition(copy->definition).ok()
            && talas::app::soundCopiedFromSignalFlow(editor.document(), registry, *drag)->definition.displayName == "brittle1",
        "a second copy should be numbered");
    require(!talas::app::soundCopiedFromSignalFlow(document, registry, {std::nullopt, "out"}).has_value()
            && !talas::app::soundCopiedFromSignalFlow(document, registry, {std::string{"missing"}, "brittle"}).has_value(),
        "the output, or a Sound of no signal flow, should make no copy");
}

// A Sound of the signal flow dragged into a
// parameter field gives it the Sound's own value for the parameter of that
// name, between any two Sounds with parameter names in common.
void testASoundDraggedIntoAFieldGivesItsValue()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"cloud1", "talas.oscillator", {{"frequency", "!Pitch * 2"}}}, {"cloud2", "talas.noise", {}},
        {"out", "talas.audioOutput", {}}};
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "voice";
    voice.graph.nodes = {{"osc", "talas.oscillator", {{"frequency", "220 hz"}}}};
    document.soundDefinitions.push_back(voice);
    const auto* oscillator = registry.findNodeType("talas.oscillator");
    const auto amplitude = std::ranges::find(oscillator->parameters, "amplitude", &talas::core::ParameterDescriptor::id);
    require(talas::app::parameterValueOfDraggedSound(document, registry, {std::nullopt, "cloud1"}, "frequency")
                == std::optional<std::string>{"!Pitch * 2"},
        "the field should take the dragged Sound's own value");
    require(talas::app::parameterValueOfDraggedSound(document, registry, {std::nullopt, "cloud1"}, "amplitude")
                == std::optional<std::string>{amplitude->defaultExpression},
        "a parameter the Sound never set should give its prototype's value");
    require(talas::app::parameterValueOfDraggedSound(document, registry, {std::string{"voice"}, "osc"}, "frequency")
                == std::optional<std::string>{"220 hz"},
        "a Sound of a Sound's own signal flow should give its value");
    require(!talas::app::parameterValueOfDraggedSound(document, registry, {std::nullopt, "cloud1"}, "centerValue").has_value()
            && !talas::app::parameterValueOfDraggedSound(document, registry, {std::nullopt, "cloud2"}, "type").has_value()
            && !talas::app::parameterValueOfDraggedSound(document, registry, {std::string{"missing"}, "osc"}, "frequency").has_value(),
        "a Sound without an expression parameter of that name should give nothing");
}

// With Control
// or Option held, a Sound dragged onto another of its signal flow takes its
// place itself, not a copy, so that it is shared; the one replaced goes,
// with the Sounds that fed only it.
void testTheSameSoundNotACopy()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"noise", "talas.noise", {}}, {"left", "talas.vca", {}}, {"tone", "talas.oscillator", {}},
        {"right", "talas.vca", {}}, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"noise", "audioOut", "left", "audioIn"}, {"left", "audioOut", "out", "audioIn"},
        {"tone", "audioOut", "right", "audioIn"}, {"right", "audioOut", "out", "rightIn"}};
    talas::project::reconcileProjectPresentation(document);
    const auto shared = talas::app::soundSharedInPlaceOf(document, registry, {std::nullopt, "tone"}, "left");
    require(shared.has_value(), "the Sound itself should replace the other");
    const auto ids = [](const talas::project::ProjectGraph& graph) {
        Ids result;
        for (const auto& node : graph.nodes)
            result.push_back(node.instanceId);
        return result;
    };
    require(ids(shared->graph) == Ids{"tone", "right", "out"}
            && shared->graph.connections
                == std::vector<talas::core::Connection>{{"tone", "audioOut", "out", "audioIn"},
                    {"tone", "audioOut", "right", "audioIn"}, {"right", "audioOut", "out", "rightIn"}},
        "the same Sound, not a copy, should feed both, the replaced one going with what fed only it");
    require(!talas::app::soundSharedInPlaceOf(document, registry, {std::nullopt, "right"}, "tone").has_value()
            && !talas::app::soundSharedInPlaceOf(document, registry, {std::nullopt, "tone"}, "tone").has_value()
            && !talas::app::soundSharedInPlaceOf(document, registry, {std::nullopt, "out"}, "left").has_value(),
        "a Sound fed by the one it replaces, itself, or one without an output should not be shared");
}

// Sounds copied in a signal flow and pasted in a
// Timeline become a Sound of their own, the last copied with those feeding
// it.
void testCopiedSoundsPasteAsASound()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    talas::project::ProjectDocument document;
    talas::project::ProjectGraphFragment fragment;
    fragment.nodes = {{"noise", "talas.noise", {}}, {"brittle", "talas.vca", {}}};
    fragment.positions = {{0, 0}, {200, 0}};
    fragment.connections = {{"noise", "audioOut", "brittle", "audioIn"}};
    const auto copy = talas::app::soundCopiedFromFragment(document, registry, fragment);
    require(copy.has_value() && copy->definition.displayName == "brittle" && copy->definition.graph.nodes.size() == 2
            && copy->definition.outputs.size() == 1 && copy->definition.outputs.front().nodeId == "brittle",
        "the Sounds copied should paste as the last one with those feeding it");
    require(!talas::app::soundCopiedFromFragment(document, registry, {}).has_value(), "nothing copied, nothing to paste");
    // Copy in the Sound file window and
    // the Prototypes, Paste in a signal flow or a Sound file window.
    talas::project::ProjectSoundDefinition alternate;
    alternate.definitionId = "alternate";
    document.soundDefinitions.push_back(alternate);
    const auto copied = talas::app::soundFileWindowCopied({"alternate"});
    require(copied.nodes.size() == 1 && copied.nodes.front().soundDefinitionId == std::optional<std::string>{"alternate"}
            && talas::project::parseProjectGraphFragment(talas::project::serializeProjectGraphFragment(copied)).fragment == copied,
        "a Sound copied in the Sound file window should go on the clipboard as a Sound to paste");
    require(talas::app::soundsOfCopy(document, copied) == std::vector<std::string>{"alternate"}
            && talas::app::soundsOfCopy(document, fragment).empty(),
        "Sounds copied from a Sound file window should paste into one as copies of themselves");
    const auto lfo = talas::app::prototypeCopied(registry, "talas.oscillator");
    const auto pasted = lfo.has_value() ? talas::app::soundCopiedFromFragment(document, registry, *lfo) : std::nullopt;
    require(lfo.has_value() && lfo->nodes.size() == 1 && lfo->nodes.front().typeId == "talas.oscillator" && pasted.has_value()
            && pasted->definition.graph.nodes.front().typeId == "talas.oscillator"
            && !talas::app::prototypeCopied(registry, "talas.nothing").has_value(),
        "a prototype copied with Ctrl+C should paste as a Sound of that prototype");
}

// Removing Sounds asks about each Sound; Yes
// removes it and asks about the next, Remove all removes all of them at
// once, Cancel keeps the rest.
void testRemovingAsksAboutEachSound()
{
    using talas::app::SoundRemovalAnswer;
    require(talas::app::soundRemovalChoices(3) == Ids{"Yes", "Remove all", "Cancel"}
            && talas::app::soundRemovalChoices(1) == Ids{"Yes", "Cancel"},
        "Remove all should be offered while several Sounds are left");
    require(talas::app::soundRemovalAnswer("Remove all") == SoundRemovalAnswer::RemoveAll
            && !talas::app::soundRemovalAnswer("Maybe").has_value(),
        "each choice should be its answer");
    const talas::app::SoundRemoval asked{{}, {"pad", "bell", "drum"}};
    const auto yes = talas::app::soundRemovalAnswered(asked, SoundRemovalAnswer::Yes);
    require(yes == talas::app::SoundRemoval{{"pad"}, {"bell", "drum"}}, "Yes should remove one and ask about the next");
    require(talas::app::soundRemovalAnswered(yes, SoundRemovalAnswer::RemoveAll)
            == talas::app::SoundRemoval{{"pad", "bell", "drum"}, {}},
        "Remove all should remove all of the selected Sounds");
    require(talas::app::soundRemovalAnswered(yes, SoundRemovalAnswer::Cancel) == talas::app::SoundRemoval{{"pad"}, {}},
        "Cancel should keep the Sounds not yet removed");
    // A Sound with an input is replaced by it, and
    // the question says so.
    require(talas::app::soundRemovalQuestion("Chopper", {"AR Drag&Drop"}) == "Replace Chopper with its input AR Drag&Drop?"
            && talas::app::soundRemovalQuestion("Chopper", {}) == "Remove Chopper?"
            && talas::app::soundRemovalQuestion("Mixer", {"a", "b"}) == "Replace Mixer with one of its inputs?",
        "deleting a Sound with an input should ask about replacing it with its input");
}

// Any number of Sound editors can be open at once,
// but only one on any one Sound. A second open of a Sound brings the editor
// already on it to the front, where it had gone and with the Sound it
// opened on kept for Drop, rather than opening another; the Sound file
// window marks every Sound with an editor open, and a double-click on any
// of them brings its editor forward.
void testSeveralSoundEditorsOneOnEachSound()
{
    talas::project::ProjectDocument document;
    for (const auto* id : {"bucket", "plain", "inner"})
    {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.graph.nodes = {{"tone", "talas.oscillator", {}}};
        document.soundDefinitions.push_back(sound);
    }
    document.soundDefinitions[0].displayName = "Bucket Vocoder";
    talas::app::SoundEditorWindows windows;
    require(talas::app::openSoundEditor(windows, document, "bucket") == talas::app::SoundEditorOpening::Opened
            && talas::app::openSoundEditor(windows, document, "plain") == talas::app::SoundEditorOpening::Opened,
        "an editor should open on each of two Sounds");
    require(windows.editors.size() == 2 && windows.front == std::optional<std::string>{"plain"},
        "any number of editors can be open at once, the one opened last in front");
    // The bucket's editor goes into one of its Sounds, then another editor
    // comes to the front.
    talas::app::followSoundEditorPath(windows, document, {"bucket", "inner"});
    talas::app::followSoundEditorPath(windows, document, {"plain"});
    document.soundDefinitions[0].description = "changed while its editor was open";
    require(talas::app::openSoundEditor(windows, document, "bucket") == talas::app::SoundEditorOpening::BroughtToFront,
        "a second open of a Sound should bring its editor to the front, not open another");
    const auto* bucket = talas::app::frontSoundEditor(windows);
    require(windows.editors.size() == 2 && bucket != nullptr && bucket->definitionId == "bucket"
            && bucket->path == std::vector<std::string>{"bucket", "inner"},
        "only one editor on a Sound, brought forward where it had gone");
    require(bucket->opened.has_value() && bucket->opened->description.empty(),
        "the editor brought forward should keep the Sound as it opened on it, for Drop");
    require(talas::app::soundsBeingEdited(windows) == std::vector<std::string>{"bucket", "plain"},
        "both Sounds have an editor open");
    const auto icons = talas::app::soundFileIcons(document, std::nullopt, talas::app::soundsBeingEdited(windows));
    require(icons[0].beingEdited && icons[1].beingEdited && !icons[2].beingEdited,
        "each Sound with an editor open is marked as being edited");
    require(talas::app::soundFileDoubleClick(icons[1]) == talas::app::SoundFileDoubleClick::BringEditorToFront
            && talas::app::soundFileDoubleClick(icons[2]) == talas::app::SoundFileDoubleClick::EditSound,
        "a double-click on a Sound being edited behind brings its editor forward; another opens its own");
    require(talas::app::soundEditorTitle(document, *bucket) == "Bucket Vocoder", "an editor is titled by its Sound's name");
    // The project's own signal flow in front: the editors stay open behind.
    talas::app::followSoundEditorPath(windows, document, {});
    require(!windows.front.has_value() && windows.editors.size() == 2,
        "the project's signal flow in front should leave the editors open behind it");
    talas::app::closeSoundEditor(windows, "plain");
    require(talas::app::soundsBeingEdited(windows) == std::vector<std::string>{"bucket"}, "a closed editor's Sound is no longer edited");
    require(talas::app::openSoundEditor(windows, document, "missing") == talas::app::SoundEditorOpening::Refused,
        "no editor opens on a Sound the project lacks");
    // A Sound deleted closes its editor; another file opened closes the
    // editors on the old one but the one in front.
    talas::app::followSoundEditorPath(windows, document, {"bucket"});
    (void) talas::app::openSoundEditor(windows, document, "plain");
    (void) talas::app::openSoundEditor(windows, document, "inner");
    document.soundDefinitions.erase(document.soundDefinitions.begin() + 1);
    talas::app::reconcileSoundEditors(windows, document);
    require(talas::app::soundsBeingEdited(windows) == std::vector<std::string>{"bucket", "inner"},
        "the editor on a deleted Sound should close");
    talas::app::soundEditorsOnNewDocument(windows, document);
    require(talas::app::soundsBeingEdited(windows) == std::vector<std::string>{"inner"}
            && talas::app::frontSoundEditor(windows)->opened == document.soundDefinitions[1],
        "another document should close the editors behind, the one in front as if opened now");
    // The Sound editors' window keeps its edits for
    // Undo apart from the Sound file window's and the other windows'.
    require(talas::app::soundEditorHistoryWindow() == talas::app::soundEditorHistoryWindow()
            && talas::app::soundEditorHistoryWindow() != talas::app::namedHistoryWindow("Sound File")
            && talas::app::namedHistoryWindow("Sound File") != talas::app::namedHistoryWindow("Class Editor")
            && talas::app::soundEditorHistoryWindow() != 0 && talas::app::namedHistoryWindow("Sound File") != 0,
        "the Sound editors and the Sound file window should keep their own edits");
}

// A SoundCollectionVariable is renamed by dragging
// it into a Sound file window, pressing Enter and changing the name there,
// and dragging it back into the Inputs field, where it stands for the
// collection under its new name.
void testRenamingASoundCollectionVariableInTheSoundFileWindow()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    talas::project::ProjectParameterExpression variableId;
    variableId.parameterId = "variableId";
    variableId.text = "inputs";
    document.graph.nodes = {{"collection", "talas.soundCollectionVariable", {variableId}}, {"mixer", "talas.mixerV2", {}},
        {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"collection", "audioOut", "mixer", "inputs"}, {"mixer", "leftOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(document);
    talas::project::ProjectEditor editor(std::move(document), registry);
    // Dragged out of the Inputs field into the Sound file window.
    auto copied = talas::app::soundCopiedFromSignalFlow(editor.document(), registry, {std::nullopt, "collection"});
    require(copied.has_value(), "the SoundCollectionVariable should go into the Sound file window");
    const auto soundId = copied->definition.definitionId;
    const auto filed = editor.addSoundDefinition(copied->definition);
    require(filed.ok(), "the Sound file window should take it as a Sound: " + filed.message);
    require(talas::app::soundFileVariable(editor.document(), soundId) != nullptr,
        "the Sound in the Sound file window should be the variable alone");
    // Enter, and a new name.
    require(!talas::app::soundFileVariableRenamed(editor.document(), soundId, "2voices").has_value()
            && !talas::app::soundFileVariableRenamed(editor.document(), soundId, "two voices").has_value(),
        "a variable's name starts with a letter and has no spaces");
    auto renamed = talas::app::soundFileVariableRenamed(editor.document(), soundId, "voices");
    require(renamed.has_value() && editor.replaceSoundDefinition(soundId, std::move(*renamed)).ok(),
        "Enter in the Sound file window should rename the SoundCollectionVariable");
    const auto* variable = talas::app::soundFileVariable(editor.document(), soundId);
    require(variable != nullptr && variable->parameters.front().text == std::optional<std::string>{"voices"},
        "the variable the Sound stands for should take the new name");
    // Dragged back into the Mixer's Inputs, in place of the old one.
    auto dropped = talas::app::soundFileSoundForInput(editor.document(), std::nullopt, soundId);
    require(dropped.has_value() && dropped->typeId == "talas.soundCollectionVariable" && dropped->instanceId == "voices"
            && !dropped->soundDefinitionId.has_value(),
        "dragged back, the variable itself goes into the field under its new name");
    const auto added = editor.addInputSound(std::nullopt, "mixer", "inputs", std::move(*dropped), 0);
    require(added.ok(), "the Inputs field should take the renamed variable: " + added.message);
    const auto inputs = editor.inputsOf(std::nullopt, "mixer", "inputs");
    require(inputs == std::vector<std::string>{"voices"}, "the renamed variable should take the old one's place in Inputs");
    const auto& nodes = editor.document().graph.nodes;
    const auto placed = std::ranges::find(nodes, std::string{"voices"}, &talas::project::ProjectNode::instanceId);
    require(placed != nodes.end() && placed->parameters.front().text == std::optional<std::string>{"voices"},
        "the variable in Inputs should stand for the collection by its new name");
    // Any other Sound of the Sound file window goes in as a Sound of it.
    talas::project::ProjectSoundDefinition tone;
    tone.definitionId = "tone";
    tone.displayName = "Tone";
    tone.graph.nodes = {{"osc", "talas.oscillator", {}}};
    tone.outputs = {{"audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    const auto toneAdded = editor.addSoundDefinition(tone);
    require(toneAdded.ok(), "a plain Sound joins the Sound file window: " + toneAdded.message);
    const auto plain = talas::app::soundFileSoundForInput(editor.document(), std::nullopt, "tone");
    require(plain.has_value() && plain->soundDefinitionId == std::optional<std::string>{"tone"} && plain->instanceId == "tone"
            && !talas::app::soundFileVariableRenamed(editor.document(), "tone", "other").has_value(),
        "a Sound that is not a variable goes into a field as a Sound of it, and renames as a Sound");
}

// A value the dialog asks for may be a number,
// string, symbol or array (or a character), typed
// and taken with the Value button or Enter; the Sound then plays within an
// environment holding that literal, each field reading it as it reads the
// literal typed into it.
void testTheVariableDialogTakesLiterals()
{
    using talas::app::VariableDialogPress;
    using talas::project::VariableLiteralKind;
    const auto taken = [](std::string_view typed, VariableDialogPress press) {
        return talas::app::variableDialogAnswer("value", typed, press);
    };
    for (const auto press : {VariableDialogPress::ValueButton, VariableDialogPress::Enter})
    {
        const auto number = taken("0.25", press);
        const auto quoted = taken("'piano'", press);
        const auto symbol = taken("#Square21", press);
        const auto character = taken("$a", press);
        const auto array = taken("#(1 0.5 -1)", press);
        require(number.value.has_value() && number.value->kind == VariableLiteralKind::Number && number.value->number == 0.25
                && quoted.value.has_value() && quoted.value->kind == VariableLiteralKind::String && quoted.value->text == "piano"
                && symbol.value.has_value() && symbol.value->kind == VariableLiteralKind::Symbol && symbol.value->text == "Square21"
                && character.value.has_value() && character.value->kind == VariableLiteralKind::Character
                && character.value->text == "a" && array.value.has_value() && array.value->kind == VariableLiteralKind::Array
                && array.value->elements == std::vector<double>{1.0, 0.5, -1.0},
            "Value and Enter should take a number, a string, a symbol, a character and an array");
    }
    require(taken("{1. 2. 3}", VariableDialogPress::Enter).value->elements == std::vector<double>{1.0, 2.0, 3.0}
            && taken("'it''s'", VariableDialogPress::Enter).value->text == "it's"
            && taken("#'two words'", VariableDialogPress::Enter).value->text == "two words",
        "a brace array, a quote doubled in a string and a quoted symbol should be taken as Smalltalk writes them");
    for (const auto* refused : {"", "piano", ".5", "'open", "#(1 two)", "$ab", "3 + 4"})
    {
        const auto answer = taken(refused, VariableDialogPress::ValueButton);
        require(!answer.value.has_value() && answer.refusal.starts_with("?value needs"),
            std::string{"text that is no literal should be refused, the dialog kept open: "} + refused);
    }
    const auto cancelled = taken("0.25", VariableDialogPress::Cancel);
    require(!cancelled.value.has_value() && cancelled.refusal.empty(), "Cancel should give no value");

    // Given, the values stay in the environment, and Environment shows each
    // as Smalltalk prints it.
    auto environment = talas::app::withVariableValuesGiven({}, {{"file", *typedLiteral("'piano'")}, {"curve", *typedLiteral("#(1 0 1)")}});
    environment = talas::app::withVariableValuesGiven(std::move(environment), {{"file", *typedLiteral("#Square21")}, {"c", *typedLiteral("$e")}});
    require(talas::app::variableEnvironmentLines(environment)
            == std::vector<std::string>{"?file -> #Square21", "?curve -> #(1 0 1)", "?c -> $e"},
        "the environment should show each variable's literal, a variable given again taking its new value");

    // The fields read the literals: a file field the string, a text field
    // the character, an array field the array, an expression the Array.
    talas::project::ProjectDocument lifted;
    lifted.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    const auto field = [](std::string id, std::string value, bool resource) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        (resource ? parameter.resource : parameter.text) = std::move(value);
        return parameter;
    };
    talas::project::ProjectParameterExpression amplitude;
    amplitude.parameterId = "amplitude";
    amplitude.expression = "2 of: ?curve";
    lifted.graph.nodes = {{"tone", "talas.oscillator", {field("wavetable", "?file", true), amplitude}},
        {"map", "talas.inputOutputCharacteristic", {field("inValues", "-1 0 1", false), field("outValues", "?curve", false)}},
        {"vowel", "talas.cdpHiliteVowels", {field("vowels", "?c", false)}}};
    auto asked = talas::app::variablesToAskBefore(talas::app::TalasMenuOperation::CompileLoadStart, lifted, {});
    std::ranges::sort(asked);
    require(asked == Ids{"c", "curve", "file"}, "the dialog should ask for the file, text and array fields' variables");
    const auto played = talas::project::withVariableLiterals(lifted, environment);
    require(played.graph.nodes[0].parameters[0].resource == "Square21"
            && played.graph.nodes[0].parameters[1].expression == "2 of: #(1 0 1)"
            && played.graph.nodes[1].parameters[1].text == "1 0 1" && played.graph.nodes[2].parameters[0].text == "e",
        "each field should read its variable as it reads the literal typed into it");
}

// Setting an array parameter to a variable asks
// whether the field is an array holding that one variable or the variable
// stands for the whole array; the answer is the field's text, {?name} or
// ?name, and the field reads the variable accordingly.
void testAnArrayParameterSetToAVariableAsksWhatItIs()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto parameterOf = [&](std::string_view type, std::string_view id) {
        const auto* descriptor = registry.findNodeType(std::string{type});
        require(descriptor != nullptr, "the prototype should be in the registry");
        const auto found = std::ranges::find(descriptor->parameters, id, &talas::core::ParameterDescriptor::id);
        require(found != descriptor->parameters.end(), "the parameter should be the prototype's");
        return *found;
    };
    const auto outValues = parameterOf("talas.inputOutputCharacteristic", "outValues");
    require(talas::app::isArrayParameter(outValues) && talas::app::isArrayParameter(parameterOf("talas.scaleVocoder", "intervals")),
        "OutValues and Intervals should be array parameters");
    require(!talas::app::isArrayParameter(parameterOf("talas.oscillator", "frequency"))
            && !talas::app::isArrayParameter(parameterOf("talas.oscillator", "wavetable")),
        "a number or a file field should not be an array parameter");
    require(talas::app::arrayParameterVariableToAsk(outValues, " ?curve ") == std::optional<std::string>{"curve"},
        "a variable alone set into an array parameter should ask what it is");
    require(!talas::app::arrayParameterVariableToAsk(outValues, "-1 ?mid 1").has_value()
            && !talas::app::arrayParameterVariableToAsk(outValues, "{?curve}").has_value()
            && !talas::app::arrayParameterVariableToAsk(parameterOf("talas.oscillator", "frequency"), "?curve").has_value(),
        "an array with other elements, an answered field or another parameter should ask nothing");
    const auto question = talas::app::arrayVariableQuestion("curve");
    require(question.message.find("?curve") != std::string::npos && question.arrayOfTheVariable.find("array") != std::string::npos
            && question.variableIsTheArray.find("whole array") != std::string::npos,
        "the question should offer an array of the one variable or the variable as the whole array");
    using talas::app::ArrayVariableMeaning;
    const auto ofOne = talas::app::arrayParameterVariableText("curve", ArrayVariableMeaning::ArrayOfTheVariable);
    const auto whole = talas::app::arrayParameterVariableText("curve", ArrayVariableMeaning::VariableIsTheArray);
    require(ofOne == "{?curve}" && whole == "?curve", "the answers should write {?curve} and ?curve");

    // The variable standing for the whole array takes all its values; the
    // array of the one variable keeps it as its one element, a number read
    // from the project's variables.
    talas::project::ProjectDocument document;
    talas::project::ProjectParameterExpression field;
    field.parameterId = "outValues";
    field.text = whole;
    document.graph.nodes = {{"map", "talas.inputOutputCharacteristic", {field}}};
    const talas::project::VariableLiteralEnvironment array{{"curve", *talas::project::parseVariableLiteral("#(1 0 1)")}};
    require(talas::project::withVariableLiterals(document, array).graph.nodes[0].parameters[0].text == "1 0 1",
        "a variable standing for the whole array should take all of the array's values");
    document.graph.nodes[0].parameters[0].text = ofOne;
    require(talas::project::withVariableLiterals(document, array).graph.nodes[0].parameters[0].text == "{?curve}"
            && talas::app::variablesToAskBefore(talas::app::TalasMenuOperation::CompileLoadStart, document, {}) == Ids{"curve"},
        "an array of the one variable should keep it as one element, still asked for");
    document.graph.nodes[0].parameters[0].text = "-1 " + whole + " 1";
    require(talas::project::withVariableLiterals(document, array).graph.nodes[0].parameters[0].text == "-1 1 0 1 1",
        "the whole array should take its place among other elements");
}

// Most of the Action and Info menus' operations
// take place within the variables environment: those that compile, play,
// record or make a class ask for the values it lacks and use its values,
// Get Info gives each variable its value from it, Environment shows it and
// Reset environment clears it.
void testActionAndInfoOperationsTakePlaceWithinTheEnvironment()
{
    using talas::app::TalasMenu;
    using talas::app::TalasMenuOperation;
    using talas::app::VariableEnvironmentUse;
    std::size_t operations = 0;
    std::size_t within = 0;
    for (auto value = 0; value <= static_cast<int>(TalasMenuOperation::OnScreenKeyboard); ++value)
    {
        const auto operation = static_cast<TalasMenuOperation>(value);
        const auto menu = talas::app::talasMenuOf(operation);
        if (menu != TalasMenu::Action && menu != TalasMenu::Info)
        {
            require(talas::app::variableEnvironmentUse(operation) == VariableEnvironmentUse::Outside,
                "only the Action and Info menus' operations should take place within the environment");
            continue;
        }
        ++operations;
        if (talas::app::variableEnvironmentUse(operation) != VariableEnvironmentUse::Outside)
            ++within;
    }
    require(operations > 0 && within * 2 > operations,
        "most of the Action and Info operations should take place within the environment: "
            + std::to_string(within) + " of " + std::to_string(operations));
    require(talas::app::variableEnvironmentUse(TalasMenuOperation::Environment) == VariableEnvironmentUse::Shows
            && talas::app::variableEnvironmentUse(TalasMenuOperation::ResetEnvironment) == VariableEnvironmentUse::Resets
            && talas::app::variableEnvironmentUse(TalasMenuOperation::FindPrototype) == VariableEnvironmentUse::Outside,
        "Environment shows it, Reset environment clears it, Find prototype works outside it");

    talas::project::ProjectDocument lifted;
    lifted.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    talas::project::ProjectParameterExpression amplitude;
    amplitude.parameterId = "amplitude";
    amplitude.expression = "?left";
    lifted.graph.nodes = {{"tone", "talas.oscillator", {amplitude}}};
    // Not only those that compile: Full waveform, Compile to disk, Record to
    // disk and New class from example ask too, and the others do not.
    for (const auto operation : {TalasMenuOperation::CompileLoadStart, TalasMenuOperation::FullWaveform,
             TalasMenuOperation::CompileToDisk, TalasMenuOperation::RecordToDisk, TalasMenuOperation::NewClassFromExample,
             TalasMenuOperation::Oscilloscope})
    {
        require(talas::app::variablesToAskBefore(operation, lifted, {}) == Ids{"left"},
            "an operation within the environment should ask for the values it lacks");
    }
    const talas::project::VariableLiteralEnvironment environment{{"left", talas::project::numberLiteral(0.25)}};
    require(talas::app::variablesToAskBefore(TalasMenuOperation::FullWaveform, lifted, environment).empty(),
        "an operation within the environment should take the values it has");
    for (const auto operation : {TalasMenuOperation::FindPrototype, TalasMenuOperation::DescribeSound,
             TalasMenuOperation::SetDefaultSound, TalasMenuOperation::GetInfo})
    {
        require(talas::app::variablesToAskBefore(operation, lifted, {}).empty(),
            "an operation that compiles nothing should not ask for values");
    }
    require(talas::app::variableInformationLines({"left", "right"}, environment)
            == std::vector<std::string>{"?left = 0.25", "?right: asked for when played"},
        "Get Info should give each variable its value from the environment");
    // Compile to disk takes the environment along: nothing is left to ask
    // for, ?left declared at the value given.
    const auto compiled = talas::project::withVariableEnvironment(lifted, environment);
    require(talas::project::undeclaredProjectVariables(compiled).empty() && compiled.variableLibrary.variables.size() == 1
            && compiled.variableLibrary.variables.front().variableId == "left"
            && compiled.variableLibrary.variables.front().initialValue == 0.25,
        "an operation within the environment should work on the Sound with the environment's values");
}

int main()
{
    try
    {
        testTheIconsAreTheSounds();
        testDefaultSoundsForVariables();
        testDoubleClickingAnIcon();
        testDoubleClickingTheSoundBeingEdited();
        testTheSignalFlowOfTheSoundBeingEdited();
        testSelectingSounds();
        testTheSoundFileWindowAsksForVariablesEachTime();
        testTheVariableDialogTakesLiterals();
        testAnArrayParameterSetToAVariableAsksWhatItIs();
        testActionAndInfoOperationsTakePlaceWithinTheEnvironment();
        testSoundCollections();
        testCustomPrototypeStrips();
        testTheIconsFillRows();
        testAPrototypeDroppedIsANewSound();
        testASoundToGlobalControllerOpensForAProgram();
        testRemovingAsksAboutEachSound();
        testASoundDraggedOutOfTheSignalFlowIsACopy();
        testASoundDraggedIntoAFieldGivesItsValue();
        testTheSameSoundNotACopy();
        testCopiedSoundsPasteAsASound();
        testSeveralSoundsPlayInTurn();
        testSeveralSoundEditorsOneOnEachSound();
        testRenamingASoundCollectionVariableInTheSoundFileWindow();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
