#include "talas/project/ProjectGraphClipboard.h"

#include "JsonValue.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
constexpr std::string_view fragmentMarker = "talasGraphFragment";
constexpr int fragmentVersion = 1;
// A clipboard is text another program may have put there; bound what is read.
constexpr std::size_t maximumFragmentBytes = 16U * 1024U * 1024U;
constexpr std::size_t maximumFragmentNodes = 4096;

[[nodiscard]] ProjectGraphFragmentResult refusal(std::string message)
{
    return {std::nullopt, std::move(message)};
}

[[nodiscard]] const ProjectGraph* graphForScope(
    const ProjectDocument& document,
    std::optional<std::string_view> definitionId)
{
    if (!definitionId.has_value())
        return &document.graph;
    for (const auto& definition : document.soundDefinitions)
    {
        if (definition.definitionId == *definitionId)
            return &definition.graph;
    }
    return nullptr;
}

void appendKey(std::string& output, std::string_view key)
{
    json::appendEscapedString(output, key);
    output.push_back(':');
}

void appendStringMember(
    std::string& output,
    std::string_view key,
    std::string_view value)
{
    appendKey(output, key);
    json::appendEscapedString(output, value);
}

[[nodiscard]] std::optional<std::string> stringMember(
    const json::Value& object,
    std::string_view key)
{
    const auto* member = json::findObjectMember(object, key);
    if (member == nullptr || !member->isString())
        return std::nullopt;
    return member->string();
}

[[nodiscard]] std::optional<std::int32_t> integerMember(
    const json::Value& object,
    std::string_view key)
{
    const auto* member = json::findObjectMember(object, key);
    if (member == nullptr || !member->isNumber())
        return std::nullopt;
    const auto value = member->number();
    if (!std::isfinite(value) || std::floor(value) != value
        || value < static_cast<double>(std::numeric_limits<std::int32_t>::min())
        || value > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
    {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] std::optional<ProjectParameterExpression> parseParameter(
    const json::Value& value)
{
    if (!value.isObject())
        return std::nullopt;
    ProjectParameterExpression parameter;
    const auto id = stringMember(value, "id");
    if (!id.has_value() || id->empty())
        return std::nullopt;
    parameter.parameterId = *id;
    std::size_t values = 0;
    if (const auto expression = stringMember(value, "expression"))
    {
        parameter.expression = *expression;
        ++values;
    }
    if (const auto choice = stringMember(value, "choice"))
    {
        parameter.choice = *choice;
        ++values;
    }
    if (const auto* boolean = json::findObjectMember(value, "boolean"))
    {
        if (!boolean->isBoolean())
            return std::nullopt;
        parameter.boolean = boolean->boolean();
        ++values;
    }
    if (const auto resource = stringMember(value, "resource"))
    {
        parameter.resource = *resource;
        ++values;
    }
    if (const auto text = stringMember(value, "text"))
    {
        parameter.text = *text;
        ++values;
    }
    if (values != 1)
        return std::nullopt;
    return parameter;
}
}

ProjectGraphFragmentResult copyProjectGraphFragment(
    const ProjectDocument& document,
    std::optional<std::string_view> definitionId,
    std::span<const std::string> nodeIds)
{
    const auto* graph = graphForScope(document, definitionId);
    if (graph == nullptr)
        return refusal("Unknown Sound definition: " + std::string{definitionId.value_or("")});
    if (nodeIds.empty())
        return refusal("Nothing is selected to copy");
    std::unordered_set<std::string_view> selected;
    for (const auto& nodeId : nodeIds)
        selected.insert(nodeId);

    ProjectGraphFragment fragment;
    for (const auto& node : graph->nodes)
    {
        if (!selected.contains(node.instanceId))
            continue;
        if (node.preservedComponent.has_value())
        {
            return refusal(
                "A preserved component cannot be copied: " + node.instanceId);
        }
        const auto* presented = findProjectNodePresentation(
            document, definitionId, node.instanceId);
        fragment.nodes.push_back(node);
        fragment.positions.push_back(
            presented == nullptr ? CanvasPosition{} : presented->position);
    }
    if (fragment.nodes.size() != selected.size())
        return refusal("The selection names a node that is not in this graph");
    for (const auto& connection : graph->connections)
    {
        if (selected.contains(connection.fromNode)
            && selected.contains(connection.toNode))
        {
            fragment.connections.push_back(connection);
        }
    }
    return {std::move(fragment), {}};
}

std::string serializeProjectGraphFragment(const ProjectGraphFragment& fragment)
{
    std::string output = "{";
    appendKey(output, fragmentMarker);
    output += std::to_string(fragmentVersion);
    output += ",";
    appendKey(output, "nodes");
    output += "[";
    for (std::size_t index = 0; index < fragment.nodes.size(); ++index)
    {
        const auto& node = fragment.nodes[index];
        const auto position = index < fragment.positions.size()
            ? fragment.positions[index]
            : CanvasPosition{};
        if (index != 0)
            output += ",";
        output += "\n{";
        appendStringMember(output, "id", node.instanceId);
        if (!node.typeId.empty())
        {
            output += ",";
            appendStringMember(output, "typeId", node.typeId);
        }
        if (node.soundDefinitionId.has_value())
        {
            output += ",";
            appendStringMember(
                output, "soundDefinitionId", *node.soundDefinitionId);
        }
        output += ",";
        appendKey(output, "x");
        output += std::to_string(position.x);
        output += ",";
        appendKey(output, "y");
        output += std::to_string(position.y);
        output += ",";
        appendKey(output, "parameters");
        output += "[";
        for (std::size_t parameterIndex = 0;
             parameterIndex < node.parameters.size();
             ++parameterIndex)
        {
            const auto& parameter = node.parameters[parameterIndex];
            if (parameterIndex != 0)
                output += ",";
            output += "{";
            appendStringMember(output, "id", parameter.parameterId);
            if (parameter.expression.has_value())
            {
                output += ",";
                appendStringMember(output, "expression", *parameter.expression);
            }
            if (parameter.choice.has_value())
            {
                output += ",";
                appendStringMember(output, "choice", *parameter.choice);
            }
            if (parameter.boolean.has_value())
            {
                output += ",";
                appendKey(output, "boolean");
                output += *parameter.boolean ? "true" : "false";
            }
            if (parameter.resource.has_value())
            {
                output += ",";
                appendStringMember(output, "resource", *parameter.resource);
            }
            if (parameter.text.has_value())
            {
                output += ",";
                appendStringMember(output, "text", *parameter.text);
            }
            output += "}";
        }
        output += "]}";
    }
    output += "],";
    appendKey(output, "connections");
    output += "[";
    for (std::size_t index = 0; index < fragment.connections.size(); ++index)
    {
        const auto& connection = fragment.connections[index];
        if (index != 0)
            output += ",";
        output += "\n{";
        appendStringMember(output, "fromNode", connection.fromNode);
        output += ",";
        appendStringMember(output, "fromPort", connection.fromPort);
        output += ",";
        appendStringMember(output, "toNode", connection.toNode);
        output += ",";
        appendStringMember(output, "toPort", connection.toPort);
        output += "}";
    }
    output += "]}\n";
    return output;
}

ProjectGraphFragmentResult parseProjectGraphFragment(std::string_view text)
{
    if (text.size() > maximumFragmentBytes)
        return refusal("The clipboard holds too much text to be Sounds");
    json::ParseLimits limits;
    limits.maximumInputBytes = maximumFragmentBytes;
    limits.maximumNestingDepth = 8;
    limits.requireValidUtf8 = true;
    const auto parsed = json::parse(text, limits);
    if (!parsed.ok() || !parsed.value.isObject())
        return refusal("The clipboard does not hold copied Sounds");
    const auto version = integerMember(parsed.value, fragmentMarker);
    if (!version.has_value())
        return refusal("The clipboard does not hold copied Sounds");
    if (*version != fragmentVersion)
        return refusal("The copied Sounds come from a newer version");
    const auto* nodes = json::findObjectMember(parsed.value, "nodes");
    const auto* connections = json::findObjectMember(parsed.value, "connections");
    if (nodes == nullptr || !nodes->isArray()
        || connections == nullptr || !connections->isArray())
    {
        return refusal("The copied Sounds are incomplete");
    }
    if (nodes->array().empty())
        return refusal("The clipboard holds no Sounds");
    if (nodes->array().size() > maximumFragmentNodes)
        return refusal("The clipboard holds too many Sounds");

    ProjectGraphFragment fragment;
    std::unordered_set<std::string> ids;
    for (const auto& value : nodes->array())
    {
        if (!value.isObject())
            return refusal("A copied Sound is malformed");
        ProjectNode node;
        const auto id = stringMember(value, "id");
        if (!id.has_value() || id->empty() || !ids.insert(*id).second)
            return refusal("A copied Sound has a missing or repeated id");
        node.instanceId = *id;
        if (const auto typeId = stringMember(value, "typeId"))
            node.typeId = *typeId;
        if (const auto definition = stringMember(value, "soundDefinitionId"))
            node.soundDefinitionId = *definition;
        if (node.typeId.empty() == !node.soundDefinitionId.has_value())
            return refusal("A copied Sound must name one prototype or Sound: " + node.instanceId);
        const auto x = integerMember(value, "x");
        const auto y = integerMember(value, "y");
        if (!x.has_value() || !y.has_value())
            return refusal("A copied Sound has no position: " + node.instanceId);
        const auto* parameters = json::findObjectMember(value, "parameters");
        if (parameters == nullptr || !parameters->isArray())
            return refusal("A copied Sound has no parameters: " + node.instanceId);
        for (const auto& parameterValue : parameters->array())
        {
            auto parameter = parseParameter(parameterValue);
            if (!parameter.has_value())
                return refusal("A copied parameter is malformed: " + node.instanceId);
            node.parameters.push_back(std::move(*parameter));
        }
        fragment.nodes.push_back(std::move(node));
        fragment.positions.push_back({*x, *y});
    }
    for (const auto& value : connections->array())
    {
        if (!value.isObject())
            return refusal("A copied connection is malformed");
        auto fromNode = stringMember(value, "fromNode");
        auto fromPort = stringMember(value, "fromPort");
        auto toNode = stringMember(value, "toNode");
        auto toPort = stringMember(value, "toPort");
        if (!fromNode.has_value() || !fromPort.has_value()
            || !toNode.has_value() || !toPort.has_value()
            || !ids.contains(*fromNode) || !ids.contains(*toNode))
        {
            return refusal("A copied connection names no copied Sound");
        }
        fragment.connections.push_back({
            std::move(*fromNode),
            std::move(*fromPort),
            std::move(*toNode),
            std::move(*toPort),
        });
    }
    return {std::move(fragment), {}};
}
}
