#include "talas/app/MissingNodeWorkbenchModel.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string portTypeLabel(
    talas::core::PortType type)
{
    switch (type)
    {
    case talas::core::PortType::Audio:
        return "Audio";
    case talas::core::PortType::Control:
        return "Control";
    case talas::core::PortType::Event:
        return "Event";
    case talas::core::PortType::NoteStream:
        return "Note stream";
    case talas::core::PortType::SampleBuffer:
        return "Sample buffer";
    case talas::core::PortType::Wavetable:
        return "Wavetable";
    case talas::core::PortType::Spectrum:
        return "Spectrum";
    case talas::core::PortType::ComplexSpectrum:
        return "Complex spectrum";
    }
    return "Unknown";
}

[[nodiscard]] std::string statusLabel(
    talas::project::ProjectComponentAvailability availability)
{
    switch (availability)
    {
    case talas::project::ProjectComponentAvailability::Native:
        return "Available";
    case talas::project::ProjectComponentAvailability::LegacyUnknown:
    case talas::project::ProjectComponentAvailability::Missing:
        return "Missing";
    case talas::project::ProjectComponentAvailability::IncompatibleVersion:
        return "Incompatible version";
    case talas::project::ProjectComponentAvailability::IncompatibleStateSchema:
        return "Incompatible state";
    case talas::project::ProjectComponentAvailability::IncompatibleInterface:
        return "Incompatible interface";
    case talas::project::ProjectComponentAvailability::Resolved:
        return "Resolved";
    }
    return "Unavailable";
}

struct ScopedNode
{
    const talas::project::ProjectNode* node = nullptr;
    const talas::project::ProjectGraph* graph = nullptr;
};

[[nodiscard]] ScopedNode findScopedNode(
    const talas::project::ProjectDocument& document,
    std::optional<std::string_view> soundDefinitionId,
    std::string_view nodeId)
{
    const talas::project::ProjectGraph* graph =
        &document.graph;
    if (soundDefinitionId.has_value())
    {
        const auto definition = std::ranges::find(
            document.soundDefinitions,
            *soundDefinitionId,
            &talas::project::ProjectSoundDefinition::definitionId);
        if (definition == document.soundDefinitions.end())
            return {};
        graph = &definition->graph;
    }
    const auto node = std::ranges::find(
        graph->nodes,
        nodeId,
        &talas::project::ProjectNode::instanceId);
    return node == graph->nodes.end()
        ? ScopedNode{}
        : ScopedNode{&*node, graph};
}

[[nodiscard]] bool portConnected(
    const talas::project::ProjectGraph& graph,
    std::string_view nodeId,
    std::string_view portId,
    MissingNodePortDirection direction)
{
    return std::ranges::any_of(
        graph.connections,
        [&](const auto& connection) {
            return direction == MissingNodePortDirection::Input
                ? connection.toNode == nodeId
                    && connection.toPort == portId
                : connection.fromNode == nodeId
                    && connection.fromPort == portId;
        });
}
}

std::optional<MissingNodeWorkbenchProjection>
missingNodeWorkbenchProjection(
    const talas::project::ProjectDocument& document,
    std::optional<std::string_view> soundDefinitionId,
    std::string_view nodeId,
    const talas::core::NodeRegistry& registry)
{
    const auto scoped = findScopedNode(
        document,
        soundDefinitionId,
        nodeId);
    if (scoped.node == nullptr
        || scoped.graph == nullptr
        || !scoped.node->preservedComponent.has_value())
    {
        return std::nullopt;
    }

    const auto& node = *scoped.node;
    const auto& component = *node.preservedComponent;
    const auto availability =
        talas::project::classifyProjectComponentAvailability(
            node.typeId,
            &component,
            registry);

    MissingNodeWorkbenchProjection projection;
    projection.availability = availability.availability;
    projection.qualifiedNodeId = soundDefinitionId.has_value()
        ? std::string{*soundDefinitionId} + "/"
            + node.instanceId
        : node.instanceId;
    projection.nodeId = node.instanceId;
    projection.typeId = node.typeId;
    projection.displayName = component.displayName.empty()
        ? node.typeId
        : component.displayName;
    projection.statusLabel = statusLabel(availability.availability);
    projection.typeVersionText = node.typeId + " @ "
        + component.typeVersion;
    projection.stateText = "State schema "
        + std::to_string(component.stateSchemaVersion)
        + " · " + std::to_string(component.state.size())
        + (component.state.size() == 1 ? " byte" : " bytes");
    projection.bundleText = component.bundleId.empty()
        ? "No bundle identifier"
        : component.bundleId;
    projection.recoveryText = component.recoveryHint.empty()
        ? "No recovery hint provided"
        : component.recoveryHint;
    projection.diagnosticText = availability.message;
    projection.canRetry =
        availability.availability
        != talas::project::ProjectComponentAvailability::Resolved;
    projection.parameterEditingEnabled =
        availability.availability
        == talas::project::ProjectComponentAvailability::Resolved;

    projection.ports.reserve(
        component.inputs.size()
        + component.outputs.size());
    const auto appendPorts = [&]<typename Ports>(
                                 const Ports& ports,
                                 MissingNodePortDirection direction) {
        for (const auto& port : ports)
        {
            const auto connected = portConnected(
                *scoped.graph,
                node.instanceId,
                port.portId,
                direction);
            const auto directionLabel =
                direction == MissingNodePortDirection::Input
                ? std::string{"Input"}
                : std::string{"Output"};
            auto accessible = directionLabel + " " + port.portId
                + ", " + portTypeLabel(port.type)
                + (connected ? ", connected" : ", not connected");
            if (direction == MissingNodePortDirection::Input
                && port.required)
            {
                accessible += ", required";
            }
            projection.ports.push_back({
                port.portId,
                direction,
                port.type,
                portTypeLabel(port.type),
                port.required,
                connected,
                std::move(accessible),
            });
            projection.canDisconnect =
                projection.canDisconnect || connected;
        }
    };
    appendPorts(
        component.inputs,
        MissingNodePortDirection::Input);
    appendPorts(
        component.outputs,
        MissingNodePortDirection::Output);

    projection.accessibleName = projection.displayName + ", "
        + projection.statusLabel + ", node "
        + projection.qualifiedNodeId;
    projection.accessibleDescription = projection.typeVersionText
        + ". " + projection.stateText + ". "
        + projection.diagnosticText + ". Recovery: "
        + projection.recoveryText;
    return projection;
}

MissingNodeLayoutMode missingNodeLayoutMode(
    int availableWidth) noexcept
{
    return availableWidth < 280
        ? MissingNodeLayoutMode::Compact
        : MissingNodeLayoutMode::Regular;
}

std::vector<MissingNodeFocusTarget>
missingNodeKeyboardOrder(
    const MissingNodeWorkbenchProjection& projection)
{
    std::vector<MissingNodeFocusTarget> order;
    if (projection.canRetry)
        order.push_back(MissingNodeFocusTarget::Retry);
    if (projection.canDisconnect)
        order.push_back(MissingNodeFocusTarget::Disconnect);
    if (projection.canDelete)
        order.push_back(MissingNodeFocusTarget::Delete);
    order.push_back(MissingNodeFocusTarget::Diagnostic);
    return order;
}
}
