#include "talas/app/PresetEventValuesModel.h"

#include <iostream>
#include <stdexcept>
#include <string>

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

talas::project::VirtualControl control(std::string name, double value)
{
    talas::project::VirtualControl made;
    made.eventName = std::move(name);
    made.value = value;
    return made;
}

talas::project::ProjectDocument presetDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"level", "talas.constant", {expression("value", "!Level * !Bright")}},
        {"osc", "talas.oscillator", {expression("frequency", "!Pitch")}},
        {"held", "talas.preset", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"level", "value", "osc", "amplitude"},
        {"osc", "audioOut", "held", "audioIn"},
        {"held", "audioOut", "out", "audioIn"},
    };
    document.virtualControls = {control("Pitch", 60.0), control("Other", 0.5), control("Level", 0.25),
        control("Bright", 1.0)};
    return document;
}

// Set to Current Event Values writes the surface's current
// settings of the Input's Event Values into the EventValues field, one
// `!Name channel: n; initialValue: v.` entry each.
void testCurrentValuesOfTheInputsEventValues()
{
    auto document = presetDocument();
    require(talas::app::presetCurrentEventValues(document, std::nullopt, "held")
            == "!Pitch channel: 1; initialValue: 60.\n!Level channel: 1; initialValue: 0.25.\n"
               "!Bright channel: 1; initialValue: 1.",
        "the entries should be the Input's Event Values, in the surface's order, at their current values");
    document.virtualControls[0].midiMapping = talas::project::MidiControlMapping{.channel = 3, .controller = 7};
    document.virtualControls[2].value = 0.125;
    require(talas::app::presetCurrentEventValues(document, std::nullopt, "held").starts_with(
                "!Pitch channel: 3; initialValue: 60.\n!Level channel: 1; initialValue: 0.125."),
        "an entry should take its control's MIDI channel and current value");
    require(!talas::app::parameterTakesTyping("talas.preset", "eventValues") && talas::app::parameterTakesTyping("talas.annotation", "text")
            && talas::app::parameterTakesTyping("talas.oscillator", "frequency"),
        "a Preset's EventValues should be read, not typed; other fields take typing");
}

void testOnlyAPresetsOwnInputCounts()
{
    auto document = presetDocument();
    require(talas::app::presetCurrentEventValues(document, std::nullopt, "osc").empty(),
        "a Sound that is not a Preset has no entries");
    require(talas::app::presetCurrentEventValues(document, std::nullopt, "missing").empty(),
        "a missing node has no entries");
    require(talas::app::presetCurrentEventValues(document, std::string{"nowhere"}, "held").empty(),
        "a Preset in another graph has no entries");
    document.graph.connections.erase(document.graph.connections.begin() + 1);
    require(talas::app::presetCurrentEventValues(document, std::nullopt, "held").empty(),
        "a Preset without an Input has no entries");
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "kept";
    definition.graph = presetDocument().graph;
    document.soundDefinitions.push_back(definition);
    require(talas::app::presetCurrentEventValues(document, std::string{"kept"}, "held").find("!Level") != std::string::npos,
        "a Preset inside a Sound definition should take its own Input's Event Values");
}
}

int main()
{
    try
    {
        testCurrentValuesOfTheInputsEventValues();
        testOnlyAPresetsOwnInputCounts();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "preset event values model tests passed\n";
    return 0;
}
