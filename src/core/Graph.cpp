#include "talas/core/Graph.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace talas::core
{
namespace
{
const PortDescriptor* findPort(const std::vector<PortDescriptor>& ports, const std::string& portId)
{
    const auto found = std::ranges::find_if(ports, [&](const PortDescriptor& port) {
        return port.id == portId;
    });
    if (found == ports.end())
        return nullptr;
    return &*found;
}

struct InputEndpoint
{
    std::string nodeId;
    std::string portId;

    [[nodiscard]] bool operator==(const InputEndpoint&) const = default;
};

struct InputEndpointHash
{
    [[nodiscard]] std::size_t operator()(const InputEndpoint& endpoint) const
    {
        const auto nodeHash = std::hash<std::string>{}(endpoint.nodeId);
        const auto portHash = std::hash<std::string>{}(endpoint.portId);
        return nodeHash ^ (portHash + 0x9e3779b97f4a7c15ULL + (nodeHash << 6) + (nodeHash >> 2));
    }
};
}

void Graph::addNode(NodeInstance node)
{
    nodes_.push_back(std::move(node));
}

void Graph::connect(Connection connection)
{
    connections_.push_back(std::move(connection));
}

const std::vector<NodeInstance>& Graph::nodes() const
{
    return nodes_;
}

const std::vector<Connection>& Graph::connections() const
{
    return connections_;
}

ValidationResult Graph::validate(const NodeRegistry& registry) const
{
    ValidationResult result;
    std::unordered_map<std::string, const NodeInstance*> nodesById;

    for (const auto& node : nodes_)
    {
        if (!nodesById.emplace(node.instanceId, &node).second)
        {
            result.errors.push_back({
                ValidationCode::DuplicateNodeInstance,
                "Duplicate node instance id",
                node.instanceId,
            });
        }

        if (registry.findNodeType(node.typeId) == nullptr)
        {
            result.errors.push_back({
                ValidationCode::UnknownNodeType,
                "Unknown node type: " + node.typeId,
                node.instanceId,
            });
        }
    }

    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    std::unordered_set<InputEndpoint, InputEndpointHash> validIncomingInputs;

    for (const auto& connection : connections_)
    {
        const auto fromNode = nodesById.find(connection.fromNode);
        const auto toNode = nodesById.find(connection.toNode);
        bool endpointsValid = true;

        if (fromNode == nodesById.end())
        {
            result.errors.push_back({
                ValidationCode::UnknownNodeInstance,
                "Connection references unknown source node: " + connection.fromNode,
                connection.fromNode,
            });
            endpointsValid = false;
        }
        if (toNode == nodesById.end())
        {
            result.errors.push_back({
                ValidationCode::UnknownNodeInstance,
                "Connection references unknown target node: " + connection.toNode,
                connection.toNode,
            });
            endpointsValid = false;
        }
        if (!endpointsValid)
            continue;

        const auto* fromType = registry.findNodeType(fromNode->second->typeId);
        const auto* toType = registry.findNodeType(toNode->second->typeId);
        if (fromType == nullptr || toType == nullptr)
            continue;

        const auto* output = findPort(fromType->outputs, connection.fromPort);
        const auto* input = findPort(toType->inputs, connection.toPort);

        if (output == nullptr)
        {
            result.errors.push_back({
                ValidationCode::UnknownPort,
                "Unknown output port: " + connection.fromPort,
                connection.fromNode,
            });
            continue;
        }
        if (input == nullptr)
        {
            result.errors.push_back({
                ValidationCode::UnknownPort,
                "Unknown input port: " + connection.toPort,
                connection.toNode,
            });
            continue;
        }
        if (!portDescriptorsCompatible(*output, *input))
        {
            result.errors.push_back({
                ValidationCode::PortTypeMismatch,
                output->type == input->type
                    ? "Spectral frame format mismatch between "
                        + connection.fromPort + " ("
                        + std::string(spectralFrameFormatName(
                            output->signal.spectralFormat))
                        + ") and " + connection.toPort + " ("
                        + std::string(spectralFrameFormatName(
                            input->signal.spectralFormat))
                        + ")"
                    : "Port type mismatch between "
                        + connection.fromPort + " and "
                        + connection.toPort,
                connection.toNode,
            });
            continue;
        }

        validIncomingInputs.insert({connection.toNode, connection.toPort});
        adjacency[connection.fromNode].push_back(connection.toNode);
    }

    for (const auto& node : nodes_)
    {
        const auto* descriptor = registry.findNodeType(node.typeId);
        if (descriptor == nullptr)
            continue;

        for (const auto& input : descriptor->inputs)
        {
            if (input.required && !validIncomingInputs.contains({node.instanceId, input.id}))
            {
                result.errors.push_back({
                    ValidationCode::MissingRequiredInput,
                    "Missing required input: " + input.id,
                    node.instanceId,
                });
            }
        }
    }

    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    bool sawCycle = false;

    // Iterative depth-first search, so long chains cannot exhaust a stack.
    std::vector<std::pair<const std::string*, std::size_t>> path;
    for (const auto& node : nodes_)
    {
        if (sawCycle)
            break;
        if (visited.contains(node.instanceId))
            continue;
        visiting.insert(node.instanceId);
        path.push_back({&node.instanceId, 0});
        while (!path.empty() && !sawCycle)
        {
            auto& [nodeId, nextSuccessor] = path.back();
            const auto& successors = adjacency[*nodeId];
            if (nextSuccessor == successors.size())
            {
                visiting.erase(*nodeId);
                visited.insert(*nodeId);
                path.pop_back();
                continue;
            }
            const auto& next = successors[nextSuccessor++];
            if (visited.contains(next))
                continue;
            if (!visiting.insert(next).second)
            {
                sawCycle = true;
                break;
            }
            path.push_back({&next, 0});
        }
    }

    if (sawCycle)
    {
        result.errors.push_back({
            ValidationCode::CycleDetected,
            "Direct graph cycle detected",
            {},
        });
    }

    return result;
}
}
