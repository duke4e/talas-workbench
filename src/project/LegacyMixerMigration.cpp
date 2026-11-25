#include "LegacyMixerMigration.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace talas::project::detail
{
namespace
{
struct Scope
{
    ProjectGraph& graph;
    ProjectSoundDefinition* definition = nullptr;

    [[nodiscard]] std::optional<std::string> definitionId() const
    {
        return definition == nullptr
            ? std::nullopt
            : std::optional<std::string>{definition->definitionId};
    }
};

[[nodiscard]] const ProjectParameterExpression* findParameter(
    const ProjectNode& node,
    std::string_view parameterId)
{
    const auto found = std::ranges::find(
        node.parameters, parameterId, &ProjectParameterExpression::parameterId);
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] bool isUnitGain(const ProjectParameterExpression* gain)
{
    return gain == nullptr
        || (!gain->choice.has_value() && !gain->boolean.has_value()
            && !gain->resource.has_value() && !gain->text.has_value()
            && (!gain->expression.has_value()
                || *gain->expression == "1.0"
                || *gain->expression == "1"));
}

[[nodiscard]] std::string unusedNodeId(const ProjectGraph& graph)
{
    for (std::size_t ordinal = 1;; ++ordinal)
    {
        auto candidate = "vca" + std::to_string(ordinal);
        if (std::ranges::find(graph.nodes, candidate, &ProjectNode::instanceId)
            == graph.nodes.end())
        {
            return candidate;
        }
    }
}

[[nodiscard]] bool targetsGain(
    const SnapshotTargetIdentity& target,
    const std::optional<std::string>& definitionId,
    std::string_view mixerId,
    std::string_view gainId)
{
    return target.kind == SnapshotTargetKind::NodeParameter
        && target.containingDefinitionId == definitionId
        && target.nodeId == mixerId
        && target.parameterId == gainId;
}

[[nodiscard]] bool isScopedToMixer(
    const ProjectSnapshotBank& bank,
    const std::optional<std::string>& definitionId,
    std::string_view mixerId)
{
    return bank.scope.kind == SnapshotScopeKind::Node
        && bank.scope.containingDefinitionId == definitionId
        && bank.scope.nodeId == mixerId;
}

// Visits every stored reference to a Mixer gain. A bank scoped to the Mixer
// itself keeps its targets, since a VCA lies outside that scope.
template<typename Visit>
void forEachGainReference(
    ProjectDocument& document,
    const Scope& scope,
    std::string_view mixerId,
    std::string_view gainId,
    Visit&& visit)
{
    const auto definitionId = scope.definitionId();
    for (auto& bank : document.snapshotLibrary.banks)
    {
        if (isScopedToMixer(bank, definitionId, mixerId))
            continue;
        for (auto& policy : bank.parameterPolicies)
            if (targetsGain(policy.target, definitionId, mixerId, gainId))
                visit(policy.target.nodeId, policy.target.parameterId);
        for (auto& snapshot : bank.snapshots)
            for (auto& value : snapshot.values)
                if (targetsGain(value.target, definitionId, mixerId, gainId))
                    visit(value.target.nodeId, value.target.parameterId);
    }
    for (auto& control : document.presentation.performanceControls)
    {
        if (targetsGain(control.target, definitionId, mixerId, gainId))
            visit(control.target.nodeId, control.target.parameterId);
        for (auto& slot : control.modulationSlots)
            if (targetsGain(slot.target, definitionId, mixerId, gainId))
                visit(slot.target.nodeId, slot.target.parameterId);
    }
    if (scope.definition == nullptr)
        return;
    for (auto& parameter : scope.definition->exposedParameters)
    {
        for (auto& binding : parameter.bindings)
        {
            if (binding.target == ProjectSoundParameterTarget::NodeParameter
                && binding.nodeId == mixerId
                && binding.parameterId == gainId)
            {
                visit(binding.nodeId, binding.parameterId);
            }
        }
    }
}

void presentNear(
    ProjectDocument& document,
    const Scope& scope,
    std::string_view mixerId,
    std::string nodeId,
    std::int32_t verticalOffset)
{
    const auto definitionId = scope.definitionId();
    const auto layoutScope = definitionId.has_value()
        ? std::optional<std::string_view>{*definitionId}
        : std::nullopt;
    auto* layout = findProjectStructureLayout(document, layoutScope);
    if (layout == nullptr)
        return;
    const auto* mixer = findProjectNodePresentation(
        document, layoutScope, mixerId);
    auto position = mixer == nullptr ? CanvasPosition{} : mixer->position;
    position.x -= 200;
    position.y += verticalOffset;
    layout->nodes.push_back({std::move(nodeId), position});
}

void migrateMixer(
    ProjectDocument& document,
    const Scope& scope,
    std::size_t nodeIndex)
{
    auto& graph = scope.graph;
    const auto mixerId = graph.nodes[nodeIndex].instanceId;
    const auto touchesMixer = [&](const talas::core::Connection& connection,
                                  std::string_view port) {
        return connection.toNode == mixerId && connection.toPort == port;
    };

    constexpr std::array inputs{
        std::array<std::string_view, 2>{"in1", "gain1"},
        std::array<std::string_view, 2>{"in2", "gain2"},
    };
    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        const auto inputPort = inputs[index][0];
        const auto gainPort = inputs[index][1];
        const auto* gain = findParameter(graph.nodes[nodeIndex], gainPort);
        const auto gainParameter = gain == nullptr
            ? std::optional<ProjectParameterExpression>{}
            : std::optional<ProjectParameterExpression>{*gain};
        auto gainReferenced = false;
        forEachGainReference(document, scope, mixerId, gainPort,
            [&](std::string&, std::string&) { gainReferenced = true; });
        const auto source = std::ranges::find_if(
            graph.connections, [&](const auto& connection) {
                return touchesMixer(connection, inputPort);
            });
        // A Sound feeding both inputs needs two distinct connections, and
        // a Sound definition maps at most one input to a port.
        const auto repeatsConnection = source != graph.connections.end()
            && std::ranges::find(graph.connections, talas::core::Connection{
                   source->fromNode, source->fromPort, mixerId, "inputs"})
                != graph.connections.end();
        const auto mapsBoundary = [&](std::string_view port) {
            return scope.definition != nullptr
                && std::ranges::any_of(scope.definition->inputs, [&](const auto& input) {
                       return input.nodeId == mixerId && input.nodePortId == port;
                   });
        };
        const auto repeatsBoundary = mapsBoundary(inputPort) && mapsBoundary("inputs");
        const auto gainConnected = mapsBoundary(gainPort)
            || std::ranges::any_of(graph.connections, [&](const auto& connection) {
                   return touchesMixer(connection, gainPort);
               });

        if (isUnitGain(gain) && !gainConnected && !gainReferenced
            && !repeatsConnection && !repeatsBoundary)
        {
            for (auto& connection : graph.connections)
                if (touchesMixer(connection, inputPort))
                    connection.toPort = "inputs";
            if (scope.definition != nullptr)
                for (auto& port : scope.definition->inputs)
                    if (port.nodeId == mixerId && port.nodePortId == inputPort)
                        port.nodePortId = "inputs";
            continue;
        }

        const auto vcaId = unusedNodeId(graph);
        auto vcaGain = gainParameter.value_or(
            ProjectParameterExpression{"gain", "1.0"});
        vcaGain.parameterId = "gain";
        graph.nodes.push_back({vcaId, "talas.vca", {std::move(vcaGain)}});
        for (auto& connection : graph.connections)
        {
            if (touchesMixer(connection, inputPort))
                connection = {connection.fromNode, connection.fromPort, vcaId, "audioIn"};
            else if (touchesMixer(connection, gainPort))
                connection = {connection.fromNode, connection.fromPort, vcaId, "gain"};
        }
        graph.connections.push_back({vcaId, "audioOut", mixerId, "inputs"});
        if (scope.definition != nullptr)
        {
            for (auto& port : scope.definition->inputs)
            {
                if (port.nodeId != mixerId)
                    continue;
                if (port.nodePortId == inputPort)
                    port = {port.portId, port.type, vcaId, "audioIn"};
                else if (port.nodePortId == gainPort)
                    port = {port.portId, port.type, vcaId, "gain"};
            }
        }
        forEachGainReference(document, scope, mixerId, gainPort,
            [&](std::string& nodeId, std::string& parameterId) {
                nodeId = vcaId;
                parameterId = "gain";
            });
        presentNear(document, scope, mixerId, vcaId, index == 0 ? -60 : 60);
    }

    auto& mixer = graph.nodes[nodeIndex];
    mixer.typeId = "talas.mixerV2";
    mixer.parameters = {{"left", "1.0"}, {"right", "1.0"}};
    for (auto& connection : graph.connections)
        if (connection.fromNode == mixerId && connection.fromPort == "audioOut")
            connection.fromPort = "leftOut";
    if (scope.definition != nullptr)
        for (auto& port : scope.definition->outputs)
            if (port.nodeId == mixerId && port.nodePortId == "audioOut")
                port.nodePortId = "leftOut";
}

void migrateGraph(ProjectDocument& document, const Scope& scope)
{
    // VCAs are appended, so the original nodes keep their indices.
    const auto originalCount = scope.graph.nodes.size();
    for (std::size_t index = 0; index < originalCount; ++index)
    {
        if (scope.graph.nodes[index].typeId == "talas.mixer")
            migrateMixer(document, scope, index);
    }
}
}

void migrateLegacyMixers(ProjectDocument& document)
{
    migrateGraph(document, {document.graph});
    for (auto& definition : document.soundDefinitions)
        migrateGraph(document, {definition.graph, &definition});
}
}
