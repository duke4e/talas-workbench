#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectMidiVoiceIslands.h"
#include "talas/project/PreparedProjectSoundVariables.h"
#include "talas/project/PreparedProjectVariables.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/project/ProjectSoundVariables.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using namespace talas::project;

void require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

ProjectNode owner(std::string id, std::string type = "talas.analogSequencer")
{
    ProjectNode node;
    node.instanceId = std::move(id);
    node.typeId = std::move(type);
    if (node.typeId == "talas.analogSequencer")
    {
        ProjectParameterExpression pitches;
        pitches.parameterId = "pitches";
        pitches.text = "#(60 64)";
        node.parameters.push_back(std::move(pitches));
    }
    return node;
}

const ProjectMidiVoiceLocalIsland& region(
    const ProjectMidiVoiceLocalIslandResult& result, std::string_view id)
{
    require(result.ok(), result.error);
    const auto found = std::ranges::find(result.islands, id,
        &ProjectMidiVoiceLocalIsland::ownerNodeId);
    require(found != result.islands.end(), "missing owner region");
    return *found;
}

ProjectNode midiVoiceNode(std::string id)
{
    return {std::move(id), "talas.midiVoice", {}};
}

ProjectNode oscillatorNode(std::string id)
{
    return {std::move(id), "talas.oscillator", {}};
}

ProjectSoundDefinition soundDefinition(
    std::string id, std::vector<ProjectNode> nodes)
{
    ProjectSoundDefinition result;
    result.definitionId = std::move(id);
    result.displayName = "Test Sound";
    result.graph.nodes = std::move(nodes);
    return result;
}

void appendExpansionNode(
    ProjectSoundExpansionManifest& expansion,
    std::string flatNodeId,
    std::vector<std::string> instancePath,
    std::string definitionId,
    std::string localNodeId,
    std::uint32_t scopeOrdinal)
{
    expansion.nodes.push_back({
        std::move(flatNodeId),
        std::move(instancePath),
        std::move(definitionId),
        std::move(localNodeId),
        scopeOrdinal,
        {},
        {},
    });
}

ProjectVariableOwner voiceVariableOwner(
    std::string definitionId, std::string nodeId)
{
    return {
        ProjectVariableOwnerKind::MidiVoice,
        std::move(definitionId),
        std::move(nodeId),
    };
}

ProjectVariableOwner soundVariableOwner(std::string definitionId)
{
    return {
        ProjectVariableOwnerKind::SoundDefinition,
        std::move(definitionId),
        {},
    };
}

ProjectVariableDeclaration scalarVariable(
    std::string id,
    ProjectVariableOwner owner = {},
    double initialValue = 0.0)
{
    ProjectVariableDeclaration result;
    result.variableId = std::move(id);
    result.displayName = "Test Variable";
    result.owner = std::move(owner);
    result.initialValue = initialValue;
    if (result.owner.kind == ProjectVariableOwnerKind::MidiVoice)
        result.snapshotEligible = false;
    return result;
}

ProjectSoundVariableDeclaration soundVariable(
    std::string id, ProjectVariableOwner owner = {})
{
    ProjectSoundVariableDeclaration result;
    result.variableId = std::move(id);
    result.displayName = "Test Sound Variable";
    result.owner = std::move(owner);
    result.candidateDefinitionIds = {"choiceA", "choiceB"};
    if (result.owner.kind == ProjectVariableOwnerKind::MidiVoice)
        result.snapshotEligible = false;
    return result;
}

struct VoicePreparationFixture
{
    ProjectDocument document;
    ProjectDocument expandedDocument;
    ProjectSoundExpansionManifest expansion;
    std::vector<ProjectMidiVoiceOwnerConfiguration> configurations;
    std::vector<std::string> childFlatNodeIds;
    std::vector<std::string> sourceFlatNodeIds;
};

VoicePreparationFixture voicePreparationFixture(
    const std::vector<std::string>& childIds,
    std::size_t parentVoiceCount,
    std::size_t childVoiceCount)
{
    VoicePreparationFixture fixture;
    std::vector<ProjectNode> sharedNodes;
    std::vector<ProjectNode> toneNodes;
    for (std::size_t index = 0; index < childIds.size(); ++index)
    {
        sharedNodes.push_back(midiVoiceNode(childIds[index]));
        const auto sourceId = "source" + std::to_string(index);
        toneNodes.push_back(oscillatorNode(sourceId));
        fixture.childFlatNodeIds.push_back(childIds[index] + "#shared");
        fixture.sourceFlatNodeIds.push_back(sourceId + "#tones");
    }

    fixture.document.soundDefinitions = {
        soundDefinition("container", {midiVoiceNode("outer")}),
        soundDefinition("shared", std::move(sharedNodes)),
        soundDefinition("tones", std::move(toneNodes)),
        soundDefinition("choiceA", {}),
        soundDefinition("choiceB", {}),
    };
    fixture.document.variableLibrary.variables = {
        scalarVariable("projectOnly", {}, 5.0),
        scalarVariable("bias", {}, 1.0),
        scalarVariable(
            "bias", voiceVariableOwner("container", "outer"), 20.0),
        scalarVariable(
            "sharedDefault", soundVariableOwner("shared"), 12.0),
        scalarVariable(
            "toneDefault", soundVariableOwner("tones"), 30.0),
    };
    fixture.document.soundVariableLibrary.variables = {
        soundVariable("projectChoice"),
        soundVariable("parentChoice", voiceVariableOwner("container", "outer")),
        soundVariable("sharedChoice", soundVariableOwner("shared")),
        soundVariable("toneChoice", soundVariableOwner("tones")),
    };
    for (const auto& childId : childIds)
    {
        fixture.document.variableLibrary.variables.push_back(
            scalarVariable(
                "childValue", voiceVariableOwner("shared", childId), 40.0));
        fixture.document.soundVariableLibrary.variables.push_back(
            soundVariable(
                "childChoice", voiceVariableOwner("shared", childId)));
    }

    fixture.expandedDocument = fixture.document;
    fixture.expandedDocument.graph.nodes.push_back(midiVoiceNode("outer#container"));
    fixture.expandedDocument.graph.nodes.push_back({"mix#container", "talas.mixer", {}});
    fixture.expandedDocument.graph.nodes.push_back({"output", "talas.audioOutput", {}});
    fixture.expansion.scopes = {
        {1, 0, {"containerInstance"}, "container"},
        {2, 1, {"containerInstance", "sharedInstance"}, "shared"},
        {3, 2, {"containerInstance", "sharedInstance", "tonesInstance"}, "tones"},
    };
    appendExpansionNode(
        fixture.expansion, "outer#container", {"containerInstance"},
        "container", "outer", 1);
    appendExpansionNode(
        fixture.expansion, "mix#container", {"containerInstance"},
        "container", "mix", 1);
    appendExpansionNode(
        fixture.expansion, "output", {}, {}, "output", 0);

    for (std::size_t index = 0; index < childIds.size(); ++index)
    {
        const auto& childId = childIds[index];
        const auto& childFlatId = fixture.childFlatNodeIds[index];
        const auto& sourceFlatId = fixture.sourceFlatNodeIds[index];
        const auto sourceId = "source" + std::to_string(index);
        fixture.expandedDocument.graph.nodes.push_back(midiVoiceNode(childFlatId));
        fixture.expandedDocument.graph.nodes.push_back(oscillatorNode(sourceFlatId));
        fixture.expandedDocument.graph.connections.push_back(
            {sourceFlatId, "audioOut", childFlatId, "audioIn"});
        fixture.expandedDocument.graph.connections.push_back(
            {childFlatId, "leftOut", "mix#container", "in" + std::to_string(index + 1)});
        appendExpansionNode(
            fixture.expansion, childFlatId,
            {"containerInstance", "sharedInstance"}, "shared", childId, 2);
        appendExpansionNode(
            fixture.expansion, sourceFlatId,
            {"containerInstance", "sharedInstance", "tonesInstance"},
            "tones", sourceId, 3);
        fixture.configurations.push_back({childFlatId, childVoiceCount});
    }
    fixture.expandedDocument.graph.connections.push_back(
        {"mix#container", "audioOut", "outer#container", "audioIn"});
    fixture.expandedDocument.graph.connections.push_back(
        {"outer#container", "leftOut", "output", "audioIn"});
    fixture.configurations.push_back({"outer#container", parentVoiceCount});
    return fixture;
}

std::vector<ProjectMidiVoiceAncestorIdentity> parentVoicePath(
    std::uint16_t ordinal)
{
    return {{"outer", "container", {"containerInstance"}, ordinal}};
}

std::vector<ProjectMidiVoiceAncestorIdentity> fullVoicePath(
    std::uint16_t parentOrdinal,
    std::string childId,
    std::uint16_t childOrdinal)
{
    auto result = parentVoicePath(parentOrdinal);
    result.push_back({
        std::move(childId),
        "shared",
        {"containerInstance", "sharedInstance"},
        childOrdinal,
    });
    return result;
}

const ProjectMidiVoiceIsland& instanceIsland(
    const ProjectMidiVoiceIslandResult& result,
    std::string_view localOwnerNodeId,
    const std::vector<ProjectMidiVoiceAncestorIdentity>& ancestors)
{
    require(result.ok(), result.error);
    const auto found = std::ranges::find_if(result.islands, [&](const auto& island) {
        return island.localOwnerNodeId == localOwnerNodeId
            && island.voiceAncestors == ancestors;
    });
    require(found != result.islands.end(), "missing projected MIDI Voice island");
    return *found;
}

ProjectGraph nested()
{
    ProjectGraph graph;
    graph.nodes = {{"tone", "talas.oscillator", {}}, owner("inner"),
        owner("outer", "talas.midiVoice"), {"out", "talas.audioOutput", {}}};
    graph.connections = {{"tone", "audioOut", "inner", "audioIn"},
        {"inner", "leftOut", "outer", "audioIn"},
        {"inner", "rightOut", "outer", "rightIn"},
        {"outer", "leftOut", "out", "audioIn"}};
    return graph;
}

void testFlatOwnersRetainExactRegions()
{
    for (const auto type : {"talas.midiVoice", "talas.midiMapper", "talas.analogSequencer"})
    {
        ProjectGraph graph;
        graph.nodes = {{"left", "talas.oscillator", {}}, owner("voice", type),
            {"out", "talas.audioOutput", {}}};
        graph.connections = {{"left", "audioOut", "voice", "audioIn"},
            {"voice", "leftOut", "out", "audioIn"}};
        const auto hierarchy = discoverProjectMidiVoiceInputHierarchy(graph);
        const auto flat = discoverProjectMidiVoiceLocalIslands(graph);
        require(hierarchy.ok() && flat.ok() && hierarchy.islands == flat.islands,
            "flat runtime authority must match tree discovery for existing owners");
        const auto& voice = region(hierarchy, "voice");
        require(voice.parentOwnerNodeId.empty() && voice.rootNodeId == "left"
                && voice.nodeIds == std::vector<std::string>{"left"},
            "a root owner retains its exclusive input and no parent");
    }
}

void testNestedBoundariesAndConnectedControls()
{
    auto graph = nested();
    graph.nodes.push_back({"innerStep", "talas.constant", {}});
    graph.nodes.push_back({"outerControl", "talas.constant", {}});
    graph.connections.push_back({"innerStep", "value", "inner", "step"});
    graph.connections.push_back({"outerControl", "value", "outer", "left"});
    const auto hierarchy = discoverProjectMidiVoiceInputHierarchy(graph);
    require(hierarchy.islands.size() == 2, "two structural owners are discovered");
    const auto& inner = region(hierarchy, "inner");
    const auto& outer = region(hierarchy, "outer");
    require(inner.parentOwnerNodeId == "outer"
            && inner.nodeIds == std::vector<std::string>{"tone"},
        "the child's input belongs only to its child region");
    require(outer.parentOwnerNodeId.empty()
            && outer.nodeIds == std::vector<std::string>{"inner", "innerStep"}
            && outer.rootNodeId == "inner" && outer.rootPortId == "leftOut"
            && outer.rightRootNodeId == "inner" && outer.rightRootPortId == "rightOut",
        "parent owns the child node and its connected controls, with both stereo boundaries");
    const auto flat = discoverProjectMidiVoiceLocalIslands(graph);
    require(!flat.ok() && flat.errorNodeId == "inner"
            && flat.error.find("nested MIDI-In") != std::string::npos,
        "structural analysis must not prematurely authorize flat runtime lowering");
    std::ranges::reverse(graph.nodes);
    std::ranges::reverse(graph.connections);
    require(discoverProjectMidiVoiceInputHierarchy(graph).islands == hierarchy.islands,
        "hierarchy and region membership are independent of authored storage order");
}

void testSiblingAndMultiLevelOwners()
{
    auto graph = nested();
    graph.nodes.push_back({"otherTone", "talas.oscillator", {}});
    graph.nodes.push_back(owner("sibling", "talas.midiMapper"));
    graph.nodes.push_back({"mix", "talas.mixer", {}});
    graph.nodes.push_back(owner("deepest"));
    graph.connections[0] = {"tone", "audioOut", "deepest", "audioIn"};
    graph.connections.push_back({"deepest", "leftOut", "inner", "audioIn"});
    std::erase_if(graph.connections, [](const auto& edge) { return edge.toNode == "outer"; });
    graph.connections.insert(graph.connections.end(), {
        {"otherTone", "audioOut", "sibling", "audioIn"},
        {"inner", "leftOut", "mix", "in1"}, {"sibling", "leftOut", "mix", "in2"},
        {"mix", "audioOut", "outer", "audioIn"}});
    const auto hierarchy = discoverProjectMidiVoiceInputHierarchy(graph);
    require(hierarchy.islands.size() == 4, "four hierarchy owners are retained");
    require(region(hierarchy, "deepest").parentOwnerNodeId == "inner"
            && region(hierarchy, "inner").parentOwnerNodeId == "outer"
            && region(hierarchy, "sibling").parentOwnerNodeId == "outer"
            && region(hierarchy, "outer").nodeIds == std::vector<std::string>{"inner", "mix", "sibling"},
        "siblings retain separate child regions under the same parent");
    require(region(hierarchy, "inner").nodeIds == std::vector<std::string>{"deepest"}
            && region(hierarchy, "deepest").nodeIds == std::vector<std::string>{"tone"},
        "grandchild input state is not also claimed by its ancestors");
}

void testHierarchyRejectsCyclesAndLeakedState()
{
    auto self = nested();
    self.connections[0] = {"inner", "leftOut", "inner", "audioIn"};
    require(!discoverProjectMidiVoiceInputHierarchy(self).ok(), "self-owned input rejects");

    auto cycle = nested();
    cycle.connections[0] = {"outer", "leftOut", "inner", "audioIn"};
    std::erase_if(cycle.connections, [](const auto& edge) { return edge.toNode == "out"; });
    const auto cyclic = discoverProjectMidiVoiceInputHierarchy(cycle);
    require(!cyclic.ok() && cyclic.error.find("cycles") != std::string::npos,
        "cycles through owner audio boundaries reject even though traversal stops at each boundary");

    for (const auto source : {"tone", "inner"})
    {
        auto leaked = nested();
        leaked.connections.push_back({source, source == std::string_view{"tone"} ? "audioOut" : "leftOut",
            "out", "rightIn"});
        require(!discoverProjectMidiVoiceInputHierarchy(leaked).ok(),
            "child input and child mixed output cannot escape their parent region");
    }
    auto shared = nested();
    shared.nodes.push_back(owner("other", "talas.midiVoice"));
    shared.connections.push_back({"inner", "leftOut", "other", "audioIn"});
    shared.connections.push_back({"other", "leftOut", "out", "rightIn"});
    require(!discoverProjectMidiVoiceInputHierarchy(shared).ok(), "one child cannot belong to two parent owners");

    auto ambiguous = nested();
    ambiguous.connections.push_back(ambiguous.connections.front());
    require(!discoverProjectMidiVoiceInputHierarchy(ambiguous).ok(), "duplicate audio input authority rejects");
    auto missing = nested();
    missing.connections.front().fromNode = "missing";
    require(!discoverProjectMidiVoiceInputHierarchy(missing).ok(), "missing input ancestor rejects");
    auto terminal = nested();
    terminal.connections.front().fromNode = "out";
    require(!discoverProjectMidiVoiceInputHierarchy(terminal).ok(), "terminal output cannot be a voice input");
}

void testDeepHierarchyAndLegacyAdapter()
{
    ProjectGraph graph;
    graph.nodes.push_back({"tone", "talas.oscillator", {}});
    for (unsigned index = 0; index < 64; ++index)
    {
        const auto id = "voice" + std::to_string(index);
        graph.nodes.push_back(owner(id));
        graph.connections.push_back({index == 0 ? "tone" : "voice" + std::to_string(index - 1),
            index == 0 ? "audioOut" : "leftOut", id, "audioIn"});
    }
    const auto hierarchy = discoverProjectMidiVoiceInputHierarchy(graph);
    require(hierarchy.ok() && hierarchy.islands.size() == 64, "multi-level discovery has no recursive stack dependency");
    require(region(hierarchy, "voice63").parentOwnerNodeId.empty()
            && region(hierarchy, "voice0").parentOwnerNodeId == "voice1",
        "deep parent identity remains exact");

    auto legacy = nested();
    auto& inner = legacy.nodes[1];
    inner.typeId = "talas.midiMapper";
    ProjectParameterExpression model;
    model.parameterId = "voiceModel"; model.choice = "legacyPassthrough";
    inner.parameters = {model};
    const auto legacyTree = discoverProjectMidiVoiceInputHierarchy(legacy);
    require(legacyTree.ok() && legacyTree.islands.size() == 1,
        "a legacy mapper is not a polyphonic hierarchy boundary");
    require(!discoverProjectMidiVoiceLocalIslands(legacy).ok(),
        "existing flat-runtime legacy mapper nesting restriction remains enforced");
}

void testProjectedHierarchyPreparesScopedVariableCells()
{
    const auto fixture = voicePreparationFixture({"child"}, 2, 3);
    const auto hierarchy = discoverProjectMidiVoiceInstanceHierarchy(
        fixture.expandedDocument, fixture.expansion, fixture.configurations);
    require(hierarchy.ok(), hierarchy.error);
    require(hierarchy.islands.size() == 3,
        "one parent owner creates one child instance under each of its two voices");
    const auto& parent = instanceIsland(hierarchy, "outer", {});
    require(parent.sourceScopeOrdinal == 1 && parent.voiceCount == 2
            && parent.voiceAncestors.empty(),
        "the parent keeps its original Sound scope and has no voice ancestors");

    const auto variables = prepareProjectVariables(
        fixture.document, fixture.expansion, hierarchy.islands);
    require(variables.ok(), variables.error);
    const auto soundVariables = prepareProjectSoundVariables(
        fixture.document, fixture.expansion, hierarchy.islands);
    require(soundVariables.ok(), soundVariables.error);

    const auto scalarChildCells = std::ranges::count_if(
        variables.table.cells, [](const auto& cell) {
            return cell.identity.variableId == "childValue"
                && cell.identity.ownerKind == ProjectVariableOwnerKind::MidiVoice;
        });
    const auto soundChildCells = std::ranges::count_if(
        soundVariables.table.cells, [](const auto& cell) {
            return cell.identity.variableId == "childChoice"
                && cell.identity.ownerKind == ProjectVariableOwnerKind::MidiVoice;
        });
    require(scalarChildCells == 6 && soundChildCells == 6,
        "each parent voice gets three independent child scalar and Sound cells");

    std::vector<std::size_t> scalarChildIndices;
    std::vector<std::size_t> soundChildIndices;
    for (std::uint16_t parentOrdinal = 0; parentOrdinal < 2; ++parentOrdinal)
    {
        const auto parentAncestors = parentVoicePath(parentOrdinal);
        const auto& child = instanceIsland(hierarchy, "child", parentAncestors);
        const auto containingScope = std::ranges::find_if(
            parent.scopeProjections, [&](const auto& projection) {
                return projection.sourceScopeOrdinal == 2
                    && projection.voiceAncestors == parentAncestors;
            });
        require(containingScope != parent.scopeProjections.end()
                && child.parentScopeOrdinal == containingScope->scopeOrdinal,
            "the child keeps its containing reusable Sound scope under the matching parent voice");

        for (std::uint16_t childOrdinal = 0; childOrdinal < 3; ++childOrdinal)
        {
            const auto childScope = child.firstVoiceScopeOrdinal + childOrdinal;
            const auto expectedPath = fullVoicePath(
                parentOrdinal, "child", childOrdinal);
            const auto scalarIndex = resolvePreparedProjectVariableCell(
                variables.table, childScope, "childValue");
            require(scalarIndex.has_value(),
                "child scalar lookup resolves inside its own voice scope");
            const auto& scalarCell = variables.table.cells[*scalarIndex];
            require(scalarCell.identity.ownerNodeId == "child"
                    && scalarCell.identity.voiceOrdinal == childOrdinal
                    && scalarCell.identity.voiceAncestors == parentAncestors,
                "child scalar identity includes both its local ordinal and parent voice lineage");
            scalarChildIndices.push_back(*scalarIndex);

            const auto soundIndex = resolvePreparedProjectSoundVariableCell(
                soundVariables.table, childScope, "childChoice");
            require(soundIndex.has_value(),
                "child Sound lookup resolves inside its own voice scope");
            const auto& soundCell = soundVariables.table.cells[*soundIndex];
            require(soundCell.identity.ownerNodeId == "child"
                    && soundCell.identity.voiceOrdinal == childOrdinal
                    && soundCell.identity.voiceAncestors == parentAncestors,
                "child Sound identity includes both its local ordinal and parent voice lineage");
            soundChildIndices.push_back(*soundIndex);

            const auto inheritedBias = resolvePreparedProjectVariableCell(
                variables.table, childScope, "bias");
            require(inheritedBias.has_value(),
                "child scalar scope walks through its parent voice to the project scope");
            const auto& parentBias = variables.table.cells[*inheritedBias];
            require(parentBias.identity.ownerNodeId == "outer"
                    && parentBias.identity.voiceOrdinal == parentOrdinal
                    && parentBias.scopeOrdinal
                        == parent.firstVoiceScopeOrdinal + parentOrdinal,
                "same-named child references resolve to the correct containing parent cell");

            const auto projectCell = resolvePreparedProjectVariableCell(
                variables.table, childScope, "projectOnly");
            require(projectCell.has_value()
                    && variables.table.cells[*projectCell].identity.ownerKind
                        == ProjectVariableOwnerKind::Project,
                "child scope continues past its parent voice to project variables");
            const auto parentSoundCell = resolvePreparedProjectSoundVariableCell(
                soundVariables.table, childScope, "parentChoice");
            require(parentSoundCell.has_value()
                    && soundVariables.table.cells[*parentSoundCell].identity.ownerNodeId
                        == "outer"
                    && soundVariables.table.cells[*parentSoundCell].identity.voiceOrdinal
                        == parentOrdinal,
                "child Sound lookup resolves the matching parent voice selection");
            const auto projectSoundCell = resolvePreparedProjectSoundVariableCell(
                soundVariables.table, childScope, "projectChoice");
            require(projectSoundCell.has_value()
                    && soundVariables.table.cells[*projectSoundCell].identity.ownerKind
                        == ProjectVariableOwnerKind::Project,
                "child Sound scope continues through its parent to project selections");

            const auto& projectedToneScope = child.scopeProjections;
            const auto toneProjection = std::ranges::find_if(
                projectedToneScope, [&](const auto& projection) {
                    return projection.sourceScopeOrdinal == 3
                        && projection.voiceAncestors == expectedPath;
                });
            require(toneProjection != projectedToneScope.end(),
                "a descendant source Sound scope is projected below each child voice");
            const auto toneCell = resolvePreparedProjectVariableCell(
                variables.table, toneProjection->scopeOrdinal, "toneDefault");
            require(toneCell.has_value()
                    && variables.table.cells[*toneCell].identity.voiceAncestors
                        == expectedPath,
                "descendant scalar defaults retain the full parent and child lineage");
            const auto toneSoundCell = resolvePreparedProjectSoundVariableCell(
                soundVariables.table, toneProjection->scopeOrdinal, "toneChoice");
            require(toneSoundCell.has_value()
                    && soundVariables.table.cells[*toneSoundCell].identity.voiceAncestors
                        == expectedPath,
                "descendant Sound defaults retain the full parent and child lineage");
        }
    }
    std::ranges::sort(scalarChildIndices);
    std::ranges::sort(soundChildIndices);
    require(std::ranges::adjacent_find(scalarChildIndices) == scalarChildIndices.end()
            && std::ranges::adjacent_find(soundChildIndices) == soundChildIndices.end(),
        "each child voice has a distinct scalar and Sound-variable cell");

    const auto scalarIdentity = variables.table.cells[scalarChildIndices.front()].identity;
    auto reorderedScalars = variables.table;
    std::ranges::reverse(reorderedScalars.cells);
    std::ranges::reverse(reorderedScalars.scopes);
    const auto reorderedScalar = findPreparedProjectVariableCell(
        reorderedScalars, scalarIdentity);
    require(reorderedScalar.has_value()
            && reorderedScalars.cells[*reorderedScalar].identity == scalarIdentity,
        "exact scalar identity lookup survives prepared cell and scope reordering");

    const auto soundIdentity = soundVariables.table.cells[soundChildIndices.front()].identity;
    auto reorderedSoundVariables = soundVariables.table;
    std::ranges::reverse(reorderedSoundVariables.cells);
    std::ranges::reverse(reorderedSoundVariables.scopes);
    const auto reorderedSound = findPreparedProjectSoundVariableCell(
        reorderedSoundVariables, soundIdentity);
    require(reorderedSound.has_value()
            && reorderedSoundVariables.cells[*reorderedSound].identity
                == soundIdentity,
        "exact Sound-variable identity lookup survives prepared cell and scope reordering");
}

void testSiblingOwnersShareIntermediateSoundProjection()
{
    const auto fixture = voicePreparationFixture(
        {"leftChild", "rightChild"}, 2, 2);
    const auto hierarchy = discoverProjectMidiVoiceInstanceHierarchy(
        fixture.expandedDocument, fixture.expansion, fixture.configurations);
    require(hierarchy.ok(), hierarchy.error);
    const auto& parent = instanceIsland(hierarchy, "outer", {});
    for (std::uint16_t parentOrdinal = 0; parentOrdinal < 2; ++parentOrdinal)
    {
        const auto ancestors = parentVoicePath(parentOrdinal);
        const auto& left = instanceIsland(hierarchy, "leftChild", ancestors);
        const auto& right = instanceIsland(hierarchy, "rightChild", ancestors);
        require(left.parentScopeOrdinal == right.parentScopeOrdinal,
            "sibling owners in one reusable Sound share one projected containing scope");
        require(std::ranges::count_if(
                    parent.scopeProjections, [&](const auto& projection) {
                        return projection.sourceScopeOrdinal == 2
                            && projection.voiceAncestors == ancestors;
                    }) == 1,
            "an intermediate Sound scope is cloned once per parent voice");
    }

    const auto variables = prepareProjectVariables(
        fixture.document, fixture.expansion, hierarchy.islands);
    const auto soundVariables = prepareProjectSoundVariables(
        fixture.document, fixture.expansion, hierarchy.islands);
    require(variables.ok() && soundVariables.ok(),
        "sibling hierarchy prepares both variable tables");
    require(std::ranges::count_if(variables.table.cells, [](const auto& cell) {
                return cell.identity.variableId == "sharedDefault"
                    && cell.identity.voiceAncestors.size() == 1;
            }) == 2
            && std::ranges::count_if(
                soundVariables.table.cells, [](const auto& cell) {
                    return cell.identity.variableId == "sharedChoice"
                        && cell.identity.voiceAncestors.size() == 1;
                }) == 2,
        "each parent voice receives one copy of shared scalar and Sound defaults across siblings");
}

ProjectDocument deepExpandedVoiceGraph(std::size_t ownerCount,
                                       ProjectSoundExpansionManifest& expansion,
                                       std::vector<ProjectMidiVoiceOwnerConfiguration>& configs)
{
    ProjectDocument document;
    document.graph.nodes.push_back(oscillatorNode("tone"));
    appendExpansionNode(expansion, "tone", {}, {}, "tone", 0);
    std::string previous = "tone";
    for (std::size_t index = 0; index < ownerCount; ++index)
    {
        const auto id = "voice" + std::to_string(index);
        document.graph.nodes.push_back(midiVoiceNode(id));
        appendExpansionNode(expansion, id, {}, {}, id, 0);
        document.graph.connections.push_back({
            previous,
            index == 0 ? "audioOut" : "leftOut",
            id,
            "audioIn",
        });
        configs.push_back({id, 1});
        previous = id;
    }
    document.graph.nodes.push_back({"output", "talas.audioOutput", {}});
    document.graph.connections.push_back({previous, "leftOut", "output", "audioIn"});
    appendExpansionNode(expansion, "output", {}, {}, "output", 0);
    return document;
}

void testProjectedHierarchyRejectsUnsupportedSourceScopesAndForgedSources()
{
    const auto fixture = voicePreparationFixture({"child"}, 2, 3);
    auto forgedIdentity = fixture.expansion;
    const auto childIdentity = std::ranges::find(
        forgedIdentity.nodes, fixture.childFlatNodeIds.front(),
        &ExpandedSoundNodeIdentity::flatNodeId);
    require(childIdentity != forgedIdentity.nodes.end(),
        "fixture contains an exact child source identity");
    childIdentity->definitionId = "tones";
    const auto forged = discoverProjectMidiVoiceInstanceHierarchy(
        fixture.expandedDocument, forgedIdentity, fixture.configurations);
    require(!forged.ok() && forged.error.find("source scope") != std::string::npos,
        "projection rejects a forged original owner definition and source scope");

    auto duplicateScope = fixture.expansion;
    auto duplicate = duplicateScope.scopes.front();
    duplicate.scopeOrdinal = 4;
    duplicateScope.scopes.push_back(duplicate);
    require(!discoverProjectMidiVoiceInstanceHierarchy(
                fixture.expandedDocument, duplicateScope, fixture.configurations).ok(),
        "two ordinal scopes cannot claim the same authored Sound instance path");
    auto duplicateNode = fixture.expansion;
    const auto mixer = std::ranges::find(duplicateNode.nodes,
        "mix#container", &ExpandedSoundNodeIdentity::flatNodeId);
    mixer->localNodeId = "outer";
    require(!discoverProjectMidiVoiceInstanceHierarchy(
                fixture.expandedDocument, duplicateNode, fixture.configurations).ok(),
        "distinct flat nodes cannot alias one source-local node identity");

    for (const auto sourceScope : {1U, 4U})
    {
        auto outside = fixture;
        const auto toneIdentity = std::ranges::find(
            outside.expansion.nodes, outside.sourceFlatNodeIds.front(),
            &ExpandedSoundNodeIdentity::flatNodeId);
        require(toneIdentity != outside.expansion.nodes.end(),
            "fixture contains an exact child input source identity");
        if (sourceScope == 1U)
        {
            toneIdentity->scopeOrdinal = 1;
            toneIdentity->definitionId = "container";
            toneIdentity->instancePath = {"containerInstance"};
        }
        else
        {
            outside.expansion.scopes.push_back({
                4, 1, {"containerInstance", "siblingInstance"}, "sibling"});
            outside.expandedDocument.soundDefinitions.push_back(
                soundDefinition("sibling", {oscillatorNode("source0")}));
            toneIdentity->scopeOrdinal = 4;
            toneIdentity->definitionId = "sibling";
            toneIdentity->instancePath = {"containerInstance", "siblingInstance"};
        }
        const auto result = discoverProjectMidiVoiceInstanceHierarchy(
            outside.expandedDocument, outside.expansion, outside.configurations);
        require(!result.ok()
                && result.error.find("outside the owner's lexical subtree")
                    != std::string::npos,
            "ancestor and sibling source scopes remain explicitly unsupported");
    }
}

void testMalformedProjectedAuthorityRejectsBothPreparedTables()
{
    const auto fixture = voicePreparationFixture({"child"}, 2, 3);
    const auto hierarchy = discoverProjectMidiVoiceInstanceHierarchy(
        fixture.expandedDocument, fixture.expansion, fixture.configurations);
    require(hierarchy.ok(), hierarchy.error);

    auto forgedAncestors = hierarchy.islands;
    const auto child = std::ranges::find_if(forgedAncestors, [](const auto& island) {
        return island.localOwnerNodeId == "child";
    });
    require(child != forgedAncestors.end() && !child->voiceAncestors.empty(),
        "hierarchy includes a child ancestor identity to mutate");
    child->voiceAncestors.front().ownerNodeId = "forgedParent";
    const auto badScalarAncestry = prepareProjectVariables(
        fixture.document, fixture.expansion, forgedAncestors);
    const auto badSoundAncestry = prepareProjectSoundVariables(
        fixture.document, fixture.expansion, forgedAncestors);
    require(!badScalarAncestry.ok() && !badSoundAncestry.ok(),
        "both prepared tables reject ancestry that does not match its parent scope");

    auto forgedProjections = hierarchy.islands;
    const auto parent = std::ranges::find_if(
        forgedProjections, [](const auto& island) {
            return island.localOwnerNodeId == "outer";
        });
    require(parent != forgedProjections.end() && !parent->scopeProjections.empty(),
        "parent hierarchy includes an intermediate Sound projection to mutate");
    parent->scopeProjections.front().parentScopeOrdinal =
        parent->scopeProjections.front().scopeOrdinal;
    const auto badScalarProjection = prepareProjectVariables(
        fixture.document, fixture.expansion, forgedProjections);
    const auto badSoundProjection = prepareProjectSoundVariables(
        fixture.document, fixture.expansion, forgedProjections);
    require(!badScalarProjection.ok() && !badSoundProjection.ok(),
        "both prepared tables reject malformed projected scope ancestry");
}

void testProjectedHierarchyConfigurationDepthAndAggregateBounds()
{
    auto fixture = voicePreparationFixture({"child"}, 2, 3);
    const auto missing = discoverProjectMidiVoiceInstanceHierarchy(
        fixture.expandedDocument, fixture.expansion, {});
    require(!missing.ok() && missing.islands.empty(),
        "instance preparation requires explicit counts and publishes no partial result");
    auto invalidConfiguration = fixture.configurations;
    invalidConfiguration.front().voiceCount = 0;
    require(!discoverProjectMidiVoiceInstanceHierarchy(
                 fixture.expandedDocument, fixture.expansion,
                 invalidConfiguration)
                 .ok(),
        "zero voice configuration is rejected");
    invalidConfiguration = fixture.configurations;
    invalidConfiguration.front().voiceCount = maximumPreparedMidiVoiceSlotsPerOwner + 1U;
    require(!discoverProjectMidiVoiceInstanceHierarchy(
                 fixture.expandedDocument, fixture.expansion,
                 invalidConfiguration)
                 .ok(),
        "per-owner voice capacity remains enforced by hierarchy preparation");

    auto overflow = fixture;
    overflow.expandedDocument.soundDefinitions.push_back(
        soundDefinition("overflowScope", {}));
    overflow.expansion.scopes.push_back({
        std::numeric_limits<std::uint32_t>::max(),
        0,
        {"unreachableOverflowScope"},
        "overflowScope",
    });
    const auto overflowResult = discoverProjectMidiVoiceInstanceHierarchy(
        overflow.expandedDocument, overflow.expansion, overflow.configurations);
    require(!overflowResult.ok()
            && overflowResult.error.find("preparation capacity") != std::string::npos,
        "scope ordinal overflow is rejected before projected allocation");

    ProjectSoundExpansionManifest withinDepthManifest;
    std::vector<ProjectMidiVoiceOwnerConfiguration> withinDepthConfigs;
    const auto withinDepthDocument = deepExpandedVoiceGraph(
        maximumPreparedMidiVoiceNestingDepth,
        withinDepthManifest,
        withinDepthConfigs);
    const auto withinDepth = discoverProjectMidiVoiceInstanceHierarchy(
        withinDepthDocument, withinDepthManifest, withinDepthConfigs);
    require(withinDepth.ok()
            && withinDepth.islands.size() == maximumPreparedMidiVoiceNestingDepth,
        "the configured owner depth limit is admitted exactly");

    ProjectSoundExpansionManifest overDepthManifest;
    std::vector<ProjectMidiVoiceOwnerConfiguration> overDepthConfigs;
    const auto overDepthDocument = deepExpandedVoiceGraph(
        maximumPreparedMidiVoiceNestingDepth + 1U,
        overDepthManifest,
        overDepthConfigs);
    const auto overDepth = discoverProjectMidiVoiceInstanceHierarchy(
        overDepthDocument, overDepthManifest, overDepthConfigs);
    require(!overDepth.ok()
            && overDepth.error.find("depth limit") != std::string::npos,
        "one level beyond the configured owner depth limit is rejected");

    const auto aggregateFixture = voicePreparationFixture({"child"}, 128, 128);
    const auto aggregate = discoverProjectMidiVoiceInstanceHierarchy(
        aggregateFixture.expandedDocument,
        aggregateFixture.expansion,
        aggregateFixture.configurations);
    require(!aggregate.ok() && aggregate.islands.empty()
            && aggregate.error.find("preparation capacity") != std::string::npos,
        "parent-by-child voice multiplication stays within the aggregate scope budget");
}

void testProjectedHierarchyIsStorageOrderInvariantAndFlatGateRemainsClosed()
{
    auto fixture = voicePreparationFixture({"leftChild", "rightChild"}, 2, 3);
    const auto hierarchy = discoverProjectMidiVoiceInstanceHierarchy(
        fixture.expandedDocument, fixture.expansion, fixture.configurations);
    require(hierarchy.ok(), hierarchy.error);

    auto reordered = fixture;
    std::ranges::reverse(reordered.expandedDocument.graph.nodes);
    std::ranges::reverse(reordered.expandedDocument.graph.connections);
    std::ranges::reverse(reordered.expansion.nodes);
    std::ranges::reverse(reordered.expansion.scopes);
    std::ranges::reverse(reordered.configurations);
    const auto reorderedHierarchy = discoverProjectMidiVoiceInstanceHierarchy(
        reordered.expandedDocument, reordered.expansion, reordered.configurations);
    require(reorderedHierarchy.ok() && reorderedHierarchy.islands == hierarchy.islands,
        "projected scopes and owner instances are independent of graph and manifest storage order");

    const auto flat = discoverProjectMidiVoiceIslands(
        fixture.expandedDocument, fixture.expansion, fixture.configurations);
    require(!flat.ok() && flat.error.find("nested MIDI-In") != std::string::npos,
        "hierarchy preparation does not admit nested owners through the flat runtime API");
}

void testHierarchyTemplatesRemainUnallocatedAndFilterUnreachableOwners()
{
    auto fixture = voicePreparationFixture({"leftChild", "rightChild"}, 2, 3);
    fixture.expandedDocument.graph.nodes.push_back(oscillatorNode("orphanSource"));
    fixture.expandedDocument.graph.nodes.push_back(midiVoiceNode("orphanVoice"));
    fixture.expandedDocument.graph.connections.push_back(
        {"orphanSource", "audioOut", "orphanVoice", "audioIn"});
    appendExpansionNode(
        fixture.expansion, "orphanSource", {}, {}, "orphanSource", 0);
    appendExpansionNode(
        fixture.expansion, "orphanVoice", {}, {}, "orphanVoice", 0);

    const auto templates = discoverProjectMidiVoiceHierarchyTemplates(
        fixture.expandedDocument, fixture.expansion);
    require(templates.ok(), templates.error);
    require(templates.islands.size() == 3
            && std::ranges::none_of(templates.islands, [](const auto& island) {
                return island.flatOwnerNodeId == "orphanVoice";
            }),
        "templates contain each reachable structural owner and omit an unreachable owner");
    const auto parent = std::ranges::find(
        templates.islands, "outer#container",
        &ProjectMidiVoiceIsland::flatOwnerNodeId);
    const auto child = std::ranges::find(
        templates.islands, "leftChild#shared",
        &ProjectMidiVoiceIsland::flatOwnerNodeId);
    require(parent != templates.islands.end() && child != templates.islands.end()
            && parent->parentOwnerNodeId.empty()
            && child->parentOwnerNodeId == parent->flatOwnerNodeId,
        "templates retain flat structural parent owner identity");
    require(std::ranges::all_of(templates.islands, [](const auto& island) {
                return island.voiceCount == 0
                    && island.firstVoiceScopeOrdinal == 0
                    && island.voiceAncestors.empty()
                    && island.scopeProjections.empty()
                    && island.sourceScopeOrdinal == island.parentScopeOrdinal;
            }),
        "template discovery records authored source scopes without allocating instance state");

    const auto missingConfiguration = discoverProjectMidiVoiceInstanceHierarchy(
        fixture.expandedDocument, fixture.expansion, {});
    require(!missingConfiguration.ok()
            && missingConfiguration.error.find("missing prepared configuration")
                != std::string::npos,
        "the instance projection API continues to require an explicit count for every reachable owner");
    const auto projected = discoverProjectMidiVoiceInstanceHierarchy(
        fixture.expandedDocument, fixture.expansion, fixture.configurations);
    require(projected.ok() && projected.islands.size() == 5
            && std::ranges::none_of(projected.islands, [](const auto& island) {
                return island.flatOwnerNodeId == "orphanVoice";
            }),
        "instance projection still ignores unreachable owners when matching explicit counts");

    auto reordered = fixture;
    std::ranges::reverse(reordered.expandedDocument.graph.nodes);
    std::ranges::reverse(reordered.expandedDocument.graph.connections);
    std::ranges::reverse(reordered.expansion.nodes);
    std::ranges::reverse(reordered.expansion.scopes);
    const auto reorderedTemplates = discoverProjectMidiVoiceHierarchyTemplates(
        reordered.expandedDocument, reordered.expansion);
    require(reorderedTemplates.ok()
            && reorderedTemplates.islands == templates.islands,
        "template parent metadata is independent of graph and manifest storage order");

    auto missingIdentity = fixture;
    std::erase_if(missingIdentity.expansion.nodes, [](const auto& identity) {
        return identity.flatNodeId == "leftChild#shared";
    });
    const auto missing = discoverProjectMidiVoiceHierarchyTemplates(
        missingIdentity.expandedDocument, missingIdentity.expansion);
    require(!missing.ok()
            && missing.error.find("missing exact expansion identity") != std::string::npos,
        "a reachable owner without its exact expansion identity is rejected");

    auto duplicateIdentity = fixture;
    const auto identity = std::ranges::find(
        duplicateIdentity.expansion.nodes, "leftChild#shared",
        &ExpandedSoundNodeIdentity::flatNodeId);
    require(identity != duplicateIdentity.expansion.nodes.end(),
        "fixture contains the child source identity to duplicate");
    const auto duplicateNodeIdentity = *identity;
    duplicateIdentity.expansion.nodes.push_back(duplicateNodeIdentity);
    const auto duplicate = discoverProjectMidiVoiceHierarchyTemplates(
        duplicateIdentity.expandedDocument, duplicateIdentity.expansion);
    require(!duplicate.ok()
            && duplicate.error.find("invalid node source scope") != std::string::npos,
        "duplicate expanded node identities are rejected");
}
}

int main()
{
    try
    {
        testFlatOwnersRetainExactRegions();
        testNestedBoundariesAndConnectedControls();
        testSiblingAndMultiLevelOwners();
        testHierarchyRejectsCyclesAndLeakedState();
        testDeepHierarchyAndLegacyAdapter();
        testProjectedHierarchyPreparesScopedVariableCells();
        testSiblingOwnersShareIntermediateSoundProjection();
        testProjectedHierarchyRejectsUnsupportedSourceScopesAndForgedSources();
        testMalformedProjectedAuthorityRejectsBothPreparedTables();
        testProjectedHierarchyConfigurationDepthAndAggregateBounds();
        testProjectedHierarchyIsStorageOrderInvariantAndFlatGateRemainsClosed();
        testHierarchyTemplatesRemainUnallocatedAndFilterUnreachableOwners();
        std::cout << "PASS MIDI voice input hierarchy (12 groups)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
