#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectLocalMapControlDiscovery.h"
#include "talas/project/ProjectMidiMapperMap.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

talas::project::ProjectParameterExpression expression(
    std::string id, std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.expression = std::move(value);
    return result;
}

talas::project::ProjectParameterExpression textParameter(
    std::string id, std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.text = std::move(value);
    return result;
}

talas::project::ProjectParameterExpression choice(
    std::string id, std::string value)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.choice = std::move(value);
    return result;
}

talas::project::ProjectDocument twoRangedMappers()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition input;
    input.definitionId = "tone";
    input.displayName = "Tone";
    input.graph.nodes = {{"osc", "talas.oscillator", {
        expression("frequency", "!Frequency"),
        expression("amplitude", "!KeyDown"),
    }}};
    input.outputs = {{
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(input));
    document.virtualControls.push_back(
        talas::project::makeDefaultVirtualControl(
            "Frequency", talas::expr::ValueDomain::Hertz));
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"mapperA", "talas.midiMapper", {
            textParameter("map",
                "!Frequency is: (`MIDIController18 min: 100 max: 1000)"),
            choice("voiceModel", "polyphonicInput"),
            expression("channel", "1"),
            expression("polyphony", "1"),
        }},
        {"mapperB", "talas.midiMapper", {
            textParameter("map",
                "!Frequency is: (`MIDIController19 min: 200 max: 2000)"),
            choice("voiceModel", "polyphonicInput"),
            expression("channel", "1"),
            expression("polyphony", "1"),
        }},
        {"sourceA", "", {}, "tone"},
        {"sourceB", "", {}, "tone"},
    };
    document.graph.connections = {
        {"sourceA", "audioOut", "mapperA", "audioIn"},
        {"sourceB", "audioOut", "mapperB", "audioIn"},
        {"mapperA", "leftOut", "out", "audioIn"},
        {"mapperB", "leftOut", "out", "rightIn"},
    };
    talas::project::reconcileProjectPresentation(document);
    return document;
}

void testProjectParserKeepsRuntimeMapContract()
{
    const auto parsed = talas::project::parseProjectMidiMapperMap(
        "!Frequency is: (`MIDIController18 min: 100 max: 1000)\n"
        "!Level is: `Level");
    require(parsed.ok() && parsed.localAssociationCount == 2
        && parsed.localAssociations[0].hasRange()
        && parsed.localAssociations[0].localEventValue == "Frequency"
        && parsed.localAssociations[0].minimumText == "100"
        && parsed.localAssociations[0].maximumText == "1000"
        && parsed.localAssociations[1].localEventSource == "Level",
        "project and runtime Map syntax must share one exact parser");
    require(talas::project::parseProjectMidiMapperMap(
        "!Frequency is: (`MIDIController18 min: nan max: 1000)").error
            == talas::project::MidiMapperMapParseError::UnknownMode,
        "project Map parser must reject nonfinite bounds");
}

void testDiscoveryRetainsMapperInstanceIdentity()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = twoRangedMappers();
    const auto discovered =
        talas::project::discoverProjectLocalMapControls(document, registry);
    require(discovered.ok() && discovered.controls.size() == 2,
        "two reachable Mappers must yield separate local controls");
    const auto& first = discovered.controls[0];
    const auto& second = discovered.controls[1];
    require(first.flatMapperNodeId == "mapperA"
        && first.mapperLocalNodeId == "mapperA"
        && first.mapperInstancePath.empty()
        && first.eventName == "Frequency"
        && first.sourceName == "MIDIController18"
        && first.domain == talas::expr::ValueDomain::Hertz
        && first.range == std::optional<std::array<double, 2>>{{100, 1000}},
        "first Mapper must retain its own source, range and domain");
    require(second.flatMapperNodeId == "mapperB"
        && second.eventName == "Frequency"
        && second.sourceName == "MIDIController19"
        && second.range == std::optional<std::array<double, 2>>{{200, 2000}},
        "same-named target must not collapse another Mapper's range");

    document.graph.nodes[1].parameters[0].text =
        "!Frequency is: (`MIDIController18 min: 1000 max: 100)";
    const auto inverted =
        talas::project::discoverProjectLocalMapControls(document, registry);
    require(inverted.ok() && inverted.controls.size() == 2
        && inverted.controls[0].range
            == std::optional<std::array<double, 2>>{{1000, 100}},
        "discovery must preserve authored bound order for later UI policy");

    document.graph.nodes[1].parameters[0].text =
        "!Frequency is: (`MIDIController07 channel: 5; min: 100 max: 1000)";
    const auto tagged =
        talas::project::discoverProjectLocalMapControls(document, registry);
    require(tagged.ok() && tagged.controls.size() == 2
            && tagged.controls[0].channel == 5
            && tagged.controls[0].sourceName == "MIDIController07"
            && tagged.controls[0].range
                == std::optional<std::array<double, 2>>{{100, 1000}}
            && !tagged.controls[1].channel,
        "discovery must retain one Mapper's tagged channel with its own range");

    document.graph.nodes[1].parameters[0].text =
        "!Frequency is: (`MIDIController18 min: 100 max: 1000; displayAs: #smallFader)";
    const auto displayed =
        talas::project::discoverProjectLocalMapControls(document, registry);
    require(displayed.ok() && displayed.controls.size() == 2
            && displayed.controls[0].displayAs
                == talas::project::MidiMapperMapDisplayAs::SmallFader
            && displayed.controls[0].range
                == std::optional<std::array<double, 2>>{{100, 1000}}
            && displayed.controls[1].displayAs
                == talas::project::MidiMapperMapDisplayAs::Default,
        "discovery must preserve one instance's authored VCS type");

    document.graph.nodes[1].parameters[0].text =
        "!Frequency is: (`MIDIController18 min: 100 max: 1000 grid: 50)";
    const auto gridded =
        talas::project::discoverProjectLocalMapControls(document, registry);
    require(gridded.ok() && gridded.controls.size() == 2
            && gridded.controls[0].grid == 50.0
            && gridded.controls[0].range
                == std::optional<std::array<double, 2>>{{100, 1000}}
            && !gridded.controls[1].grid,
        "discovery must retain the grid step for its own Mapper instance");

    document.graph.nodes[1].parameters[0].text =
        "!Frequency is: (`MIDIController07 channel: 5; min: 100 max: 1000 grid: 50; taper: #log; displayAs: #smallFader)";
    const auto combined =
        talas::project::discoverProjectLocalMapControls(document, registry);
    require(combined.ok() && combined.controls.size() == 2
            && combined.controls[0].channel == 5
            && combined.controls[0].range
                == std::optional<std::array<double, 2>>{{100, 1000}}
            && combined.controls[0].grid == 50.0
            && combined.controls[0].taper
                == talas::project::MidiMapperMapTaper::Log
            && combined.controls[0].displayAs
                == talas::project::MidiMapperMapDisplayAs::SmallFader
            && combined.controls[1].taper
                == talas::project::MidiMapperMapTaper::Linear,
        "discovery must retain all source options for only the tagged Mapper");
}

void testDiscoveryRetainsNestedSoundPath()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = twoRangedMappers();
    talas::project::ProjectSoundDefinition wrapper;
    wrapper.definitionId = "wrapped";
    wrapper.displayName = "Wrapped Mapper";
    wrapper.graph.nodes = {
        document.graph.nodes[1], document.graph.nodes[3]};
    wrapper.graph.connections = {
        {"sourceA", "audioOut", "mapperA", "audioIn"}};
    wrapper.outputs = {{
        "audioOut", talas::core::PortType::Audio,
        "mapperA", "leftOut"}};
    document.soundDefinitions.push_back(std::move(wrapper));
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"wrapper", "", {}, "wrapped"},
    };
    document.graph.connections = {
        {"wrapper", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(document);

    const auto discovered =
        talas::project::discoverProjectLocalMapControls(document, registry);
    require(discovered.ok() && discovered.controls.size() == 1,
        "nested Mapper must remain discoverable after Sound expansion");
    const auto& control = discovered.controls.front();
    require(control.flatMapperNodeId != "mapperA"
        && control.mapperInstancePath
            == std::vector<std::string>{"wrapper"}
        && control.mapperDefinitionId == "wrapped"
        && control.mapperLocalNodeId == "mapperA"
        && control.eventName == "Frequency"
        && control.range
            == std::optional<std::array<double, 2>>{{100, 1000}},
        "nested control must retain its exact instance path and local Map");
}
}

int main()
{
    testProjectParserKeepsRuntimeMapContract();
    testDiscoveryRetainsMapperInstanceIdentity();
    testDiscoveryRetainsNestedSoundPath();
}
