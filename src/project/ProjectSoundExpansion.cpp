#include "talas/project/ProjectSoundExpansion.h"

#include "talas/project/ProjectValidation.h"
#include "talas/project/ProjectLiveSets.h"
#include "talas/project/ProjectMidiVoiceIslands.h"
#include "talas/project/ProjectSoundVariables.h"
#include "talas/core/StaticResourceList.h"
#include "talas/core/AnalogSequencerExtraValues.h"
#include "talas/expr/Expression.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace talas::project
{
namespace
{
constexpr std::size_t maxExpansionDepth = 64;
constexpr std::size_t maxExpandedConnectionCount = 32768;

[[nodiscard]] bool isAnalogSequencerEventText(std::string_view parameterId)
{
    return parameterId == "durations" || parameterId == "dutyCycles"
        || parameterId == "pitches" || parameterId == "velocities"
        || parameterId == "keyDowns" || parameterId == "keyPitches"
        || parameterId == "keyVelocities" || parameterId == "keyTimbres"
        || parameterId == "extraValues" || parameterId == "generatedIndex";
}

struct FlatEndpoint
{
    std::string nodeId;
    std::string portId;
    // An instance input bound to a Variable (ADR-0006 amendment of
    // 2026-10-13): the Sound connected there replaces the Variable.
    bool variable = false;
    // The output of the Sound an instance's replaceable input replaces
    //.
    bool replaces = false;
};

struct NodeExpansion
{
    std::optional<std::string> flatNodeId;
    std::unordered_map<std::string, FlatEndpoint> inputs;
    std::unordered_map<std::string, FlatEndpoint> outputs;
};

struct GraphExpansion
{
    std::unordered_map<std::string, NodeExpansion> nodes;
};

struct AppliedParameter
{
    ProjectParameterExpression value;
    std::vector<std::string> qualifiedParameterIds;
    std::uint32_t sourceScopeOrdinal = 0;
};

using AppliedParameterBindings =
    std::unordered_map<std::string, std::vector<AppliedParameter>>;

struct ExpansionContext
{
    const ProjectDocument& source;
    ProjectDocument& destination;
    ProjectSoundExpansionManifest& manifest;
    std::unordered_set<std::string> usedNodeIds;
    std::size_t collisionSuffix = 0;
    std::size_t expansionWorkItemCount = 0;
    std::size_t expandedConnectionCount = 0;
    std::uint32_t nextScopeOrdinal = 0;
    // Variables an instance gave a Sound (ADR-0006 amendment of 2026-10-13),
    // by flat node, and the Sound each is heard as.
    std::unordered_map<std::string, FlatEndpoint> givenVariables;
    // The flat outputs replaced through a replaceable input.
    std::unordered_set<std::string> replacedOutputs;
    talas::core::ValidationResult validation;
};

void failExpansion(
    ExpansionContext& context,
    std::string message,
    std::string nodeId = {})
{
    if (!context.validation.ok())
        return;
    context.validation.errors.push_back({
        talas::core::ValidationCode::SoundExpansionLimitExceeded,
        std::move(message),
        std::move(nodeId),
    });
}

[[nodiscard]] const ProjectSoundDefinition* findDefinition(
    const ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find_if(
        document.soundDefinitions,
        [&](const auto& definition) {
            return definition.definitionId == definitionId;
        });
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

[[nodiscard]] std::string_view soundVariableId(
    const ProjectNode& node) noexcept
{
    const auto found = std::ranges::find(
        node.parameters,
        std::string_view{"variableId"},
        &ProjectParameterExpression::parameterId);
    if (found == node.parameters.end() || !found->text.has_value())
        return {};
    return *found->text;
}

[[nodiscard]] bool containsMidiVoiceInDefinitionClosure(
    const ProjectDocument& document,
    std::string_view definitionId,
    std::unordered_set<std::string>& visited,
    std::size_t depth)
{
    if (depth > maxExpansionDepth)
        return true;
    if (!visited.insert(std::string(definitionId)).second)
        return false;
    const auto* definition = findDefinition(document, definitionId);
    if (definition == nullptr)
        return true;
    for (const auto& node : definition->graph.nodes)
    {
        if (node.typeId == "talas.midiVoice")
            return true;
        if (node.soundDefinitionId.has_value()
            && containsMidiVoiceInDefinitionClosure(
                document,
                *node.soundDefinitionId,
                visited,
                depth + 1U))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] ProjectGraph makeReachableRootGraph(
    const ProjectDocument& document)
{
    std::unordered_map<
        std::string,
        std::vector<const talas::core::Connection*>>
        incomingConnections;
    for (const auto& connection : document.graph.connections)
    {
        incomingConnections[connection.toNode].push_back(
            &connection);
    }

    std::unordered_set<std::string> existingNodeIds;
    existingNodeIds.reserve(document.graph.nodes.size());
    for (const auto& node : document.graph.nodes)
        existingNodeIds.insert(node.instanceId);

    std::unordered_set<std::string> reachableNodeIds;
    std::vector<std::string> pending;
    for (const auto& node : document.graph.nodes)
    {
        if (isTerminalNodeType(node.typeId)
            && reachableNodeIds.insert(node.instanceId).second)
        {
            pending.push_back(node.instanceId);
        }
    }
    for (std::size_t index = 0; index < pending.size(); ++index)
    {
        const auto incoming =
            incomingConnections.find(pending[index]);
        if (incoming == incomingConnections.end())
            continue;
        for (const auto* connection : incoming->second)
        {
            if (existingNodeIds.contains(connection->fromNode)
                && reachableNodeIds.insert(
                       connection->fromNode)
                       .second)
            {
                pending.push_back(connection->fromNode);
            }
        }
    }

    ProjectGraph graph;
    for (const auto& node : document.graph.nodes)
    {
        if (reachableNodeIds.contains(node.instanceId))
            graph.nodes.push_back(node);
    }
    for (const auto& connection : document.graph.connections)
    {
        if (reachableNodeIds.contains(connection.toNode))
            graph.connections.push_back(connection);
    }
    return graph;
}

[[nodiscard]] std::optional<std::string> makeNestedNodeId(
    ExpansionContext& context,
    const std::vector<std::string>& instancePath,
    std::string_view internalNodeId)
{
    std::string base{"__kc_nested"};
    const auto appendSegment = [&](std::string_view segment) {
        const auto length = std::to_string(segment.size());
        const auto addedBytes =
            std::size_t{2} + length.size() + segment.size();
        if (base.size() > maximumGeneratedProjectSoundNodeIdBytes
            || addedBytes
                > maximumGeneratedProjectSoundNodeIdBytes - base.size())
        {
            failExpansion(
                context,
                "Nested Sound generated-id expansion limit exceeded",
                std::string(internalNodeId));
            return false;
        }
        base += "_";
        base += length;
        base += "_";
        base += segment;
        return true;
    };
    for (const auto& segment : instancePath)
    {
        if (!appendSegment(segment))
            return std::nullopt;
    }
    if (!appendSegment(internalNodeId))
        return std::nullopt;

    auto candidate = base;
    while (!context.usedNodeIds.insert(candidate).second)
    {
        candidate =
            base + "_" + std::to_string(++context.collisionSuffix);
        if (candidate.size() > maximumGeneratedProjectSoundNodeIdBytes)
        {
            failExpansion(
                context,
                "Nested Sound generated-id expansion limit exceeded",
                std::string(internalNodeId));
            return std::nullopt;
        }
    }
    return candidate;
}

[[nodiscard]] std::optional<FlatEndpoint> resolveEndpoint(
    const GraphExpansion& graph,
    std::string_view nodeId,
    std::string_view portId,
    bool input)
{
    const auto node = graph.nodes.find(std::string(nodeId));
    if (node == graph.nodes.end())
        return std::nullopt;
    if (node->second.flatNodeId.has_value())
    {
        return FlatEndpoint{
            *node->second.flatNodeId,
            std::string(portId),
        };
    }
    const auto& ports =
        input ? node->second.inputs : node->second.outputs;
    const auto port = ports.find(std::string(portId));
    if (port == ports.end())
        return std::nullopt;
    return port->second;
}

[[nodiscard]] std::string makeSourcePath(
    const std::vector<std::string>& instancePath,
    std::string_view definitionId,
    std::string_view localNodeId)
{
    std::string result;
    for (const auto& instanceId : instancePath)
    {
        if (!result.empty())
            result += "/";
        result += instanceId;
    }
    if (!definitionId.empty())
    {
        if (!result.empty())
            result += "/";
        result += std::string(definitionId);
        result += "::";
    }
    result += std::string(localNodeId);
    return result;
}

void applyParameterBindings(
    ProjectNode& node,
    const std::vector<AppliedParameter>& bindings)
{
    for (const auto& binding : bindings)
    {
        const auto found = std::ranges::find_if(
            node.parameters,
            [&](const auto& parameter) {
                return parameter.parameterId
                    == binding.value.parameterId;
            });
        if (found == node.parameters.end())
            node.parameters.push_back(binding.value);
        else
            *found = binding.value;
    }
}

// A class's variables given by an instance: their names and values, and
// where the values come from.
struct ClassVariableValue
{
    std::string name;
    std::string value;
    std::vector<std::string> qualifiedParameterIds;
    std::uint32_t sourceScopeOrdinal = 0;
};

[[nodiscard]] AppliedParameterBindings
resolveDefinitionParameterBindings(
    ExpansionContext& context,
    const ProjectSoundDefinition& definition,
    const ProjectNode& instance,
    const std::vector<AppliedParameter>* inheritedBindings,
    const std::vector<std::string>& instancePath,
    std::uint32_t parentScopeOrdinal,
    std::uint32_t instanceScopeOrdinal)
{
    AppliedParameterBindings result;
    std::vector<ClassVariableValue> variables;
    for (const auto& exposed : definition.exposedParameters)
    {
        const auto matchesExposed = [&](std::string_view parameterId) {
            return parameterId == exposed.parameterId
                || std::ranges::find(exposed.aliases, parameterId)
                    != exposed.aliases.end();
        };

        const ProjectParameterExpression* resolvedValue =
            &exposed.defaultValue;
        const AppliedParameter* inheritedSource = nullptr;
        // Definition-authored defaults are evaluated inside each exact child
        // instance. Only a caller override belongs to the parent scope.
        auto sourceScopeOrdinal = instanceScopeOrdinal;
        if (inheritedBindings != nullptr)
        {
            const auto inherited = std::ranges::find_if(
                *inheritedBindings,
                [&](const auto& candidate) {
                    return matchesExposed(
                        candidate.value.parameterId);
                });
            if (inherited != inheritedBindings->end())
            {
                resolvedValue = &inherited->value;
                inheritedSource = &*inherited;
                sourceScopeOrdinal = inherited->sourceScopeOrdinal;
            }
        }
        if (inheritedSource == nullptr)
        {
            const auto overrideValue = std::ranges::find_if(
                instance.parameters,
                [&](const auto& candidate) {
                    return matchesExposed(candidate.parameterId);
                });
            if (overrideValue != instance.parameters.end())
            {
                resolvedValue = &*overrideValue;
                sourceScopeOrdinal = parentScopeOrdinal;
            }
        }

        std::vector<std::string> qualifiedIds;
        if (inheritedSource != nullptr)
        {
            qualifiedIds =
                inheritedSource->qualifiedParameterIds;
        }
        const auto qualifiedId =
            qualifiedSnapshotSoundParameterIdentity(
                instancePath,
                definition.definitionId,
                exposed.parameterId);
        if (std::ranges::find(qualifiedIds, qualifiedId)
            == qualifiedIds.end())
        {
            qualifiedIds.push_back(qualifiedId);
        }

        for (const auto& binding : exposed.bindings)
        {
            if (binding.target == ProjectSoundParameterTarget::Variable)
            {
                if (resolvedValue->expression.has_value())
                {
                    variables.push_back({binding.variableName, *resolvedValue->expression,
                        qualifiedIds, sourceScopeOrdinal});
                }
                continue;
            }
            if (binding.target
                == ProjectSoundParameterTarget::EventValue)
            {
                auto value = *resolvedValue;
                value.parameterId = exposed.parameterId;
                context.manifest.eventValues.push_back({
                    instanceScopeOrdinal,
                    sourceScopeOrdinal,
                    binding.eventName,
                    binding.eventDomain,
                    std::move(value),
                    qualifiedIds,
                    makeSourcePath(
                        instancePath,
                        definition.definitionId,
                        "!" + binding.eventName),
                });
                continue;
            }
            auto appliedValue = *resolvedValue;
            appliedValue.parameterId = binding.parameterId;
            result[binding.nodeId].push_back({
                std::move(appliedValue),
                qualifiedIds,
                sourceScopeOrdinal,
            });
        }
    }
    // Each field of the class holding its variables takes the instance's
    // values in their place, unless a parameter sets the whole field.
    if (!variables.empty())
    {
        std::vector<std::pair<std::string, std::string>> values;
        for (const auto& variable : variables)
            values.emplace_back(variable.name, variable.value);
        for (const auto& node : definition.graph.nodes)
        {
            for (const auto& parameter : node.parameters)
            {
                if (!parameter.expression.has_value())
                    continue;
                auto substituted = withClassVariables(*parameter.expression, values);
                if (substituted == *parameter.expression)
                    continue;
                auto& applied = result[node.instanceId];
                if (std::ranges::any_of(applied, [&](const AppliedParameter& candidate) {
                        return candidate.value.parameterId == parameter.parameterId;
                    }))
                {
                    continue;
                }
                AppliedParameter variableValue{parameter, {}, instanceScopeOrdinal};
                variableValue.value.expression = std::move(substituted);
                for (const auto& variable : variables)
                {
                    if (withClassVariables(*parameter.expression, std::array{std::pair{variable.name, variable.value}})
                        == *parameter.expression)
                        continue;
                    // The instance's value is read where it was given.
                    variableValue.sourceScopeOrdinal = variable.sourceScopeOrdinal;
                    for (const auto& qualified : variable.qualifiedParameterIds)
                    {
                        if (std::ranges::find(variableValue.qualifiedParameterIds, qualified)
                            == variableValue.qualifiedParameterIds.end())
                            variableValue.qualifiedParameterIds.push_back(qualified);
                    }
                }
                applied.push_back(std::move(variableValue));
            }
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::string> collectQualifiedParameterIds(
    const std::vector<AppliedParameter>* bindings)
{
    std::vector<std::string> result;
    if (bindings == nullptr)
        return result;
    for (const auto& binding : *bindings)
    {
        for (const auto& qualifiedId : binding.qualifiedParameterIds)
        {
            if (std::ranges::find(result, qualifiedId)
                == result.end())
            {
                result.push_back(qualifiedId);
            }
        }
    }
    return result;
}

[[nodiscard]] GraphExpansion expandGraph(
    ExpansionContext& context,
    const ProjectGraph& graph,
    const std::vector<std::string>& instancePath,
    std::uint32_t scopeOrdinal,
    std::string_view definitionId,
    const AppliedParameterBindings& parameterBindings)
{
    GraphExpansion expanded;
    if (instancePath.size() > maxExpansionDepth)
    {
        failExpansion(
            context,
            "Nested Sound depth expansion limit exceeded");
        return expanded;
    }
    std::vector<const ProjectNode*> orderedNodes;
    orderedNodes.reserve(graph.nodes.size());
    for (const auto& node : graph.nodes)
        orderedNodes.push_back(&node);
    std::ranges::sort(
        orderedNodes,
        {},
        [](const ProjectNode* node) {
            return node->instanceId;
        });

    for (const auto* nodePointer : orderedNodes)
    {
        const auto& node = *nodePointer;
        if (!context.validation.ok())
            return {};
        if (!instancePath.empty())
        {
            if (context.expansionWorkItemCount
                >= maximumProjectSoundExpansionWorkItems)
            {
                failExpansion(
                    context,
                    "Nested Sound node expansion limit exceeded",
                    node.instanceId);
                return {};
            }
            ++context.expansionWorkItemCount;
        }
        const auto applied = parameterBindings.find(
            node.instanceId);
        const auto* nodeBindings = applied == parameterBindings.end()
            ? nullptr
            : &applied->second;
        auto preparedNode = node;
        if (nodeBindings != nullptr)
            applyParameterBindings(preparedNode, *nodeBindings);

        NodeExpansion nodeExpansion;
        if (!preparedNode.soundDefinitionId.has_value())
        {
            auto flatNode = preparedNode;
            if (!instancePath.empty())
            {
                const auto nestedNodeId = makeNestedNodeId(
                    context,
                    instancePath,
                    node.instanceId);
                if (!nestedNodeId.has_value())
                    return {};
                flatNode.instanceId = *nestedNodeId;
            }
            flatNode.soundDefinitionId.reset();
            nodeExpansion.flatNodeId = flatNode.instanceId;
            context.manifest.nodes.push_back({
                flatNode.instanceId,
                instancePath,
                std::string(definitionId),
                node.instanceId,
                scopeOrdinal,
                makeSourcePath(
                    instancePath,
                    definitionId,
                    node.instanceId),
                collectQualifiedParameterIds(nodeBindings),
            });
            if (nodeBindings != nullptr)
            {
                for (const auto& binding : *nodeBindings)
                {
                    context.manifest.nodeParameters.push_back({
                        flatNode.instanceId,
                        binding.value.parameterId,
                        binding.sourceScopeOrdinal,
                        binding.qualifiedParameterIds,
                    });
                }
            }
            context.destination.graph.nodes.push_back(
                std::move(flatNode));
        }
        else
        {
            const auto* definition = findDefinition(
                context.source,
                *preparedNode.soundDefinitionId);
            if (definition != nullptr)
            {
                if (context.nextScopeOrdinal
                    == std::numeric_limits<std::uint32_t>::max())
                {
                    failExpansion(
                        context,
                        "Nested Sound scope expansion limit exceeded",
                        node.instanceId);
                    return {};
                }
                auto childPath = instancePath;
                childPath.push_back(preparedNode.instanceId);
                const auto childScopeOrdinal =
                    ++context.nextScopeOrdinal;
                context.manifest.scopes.push_back({
                    childScopeOrdinal,
                    scopeOrdinal,
                    childPath,
                    definition->definitionId,
                });
                const auto childParameterBindings =
                    resolveDefinitionParameterBindings(
                        context,
                        *definition,
                        preparedNode,
                        nodeBindings,
                        childPath,
                        scopeOrdinal,
                        childScopeOrdinal);
                const auto child = expandGraph(
                    context,
                    definition->graph,
                    childPath,
                    childScopeOrdinal,
                    definition->definitionId,
                    childParameterBindings);
                if (!context.validation.ok())
                    return {};
                for (const auto& input : definition->inputs)
                {
                    // A boundary that names a Variable binds the Variable
                    // itself: the Sound an instance connects there takes
                    // its place.
                    const auto bound = std::ranges::find(
                        definition->graph.nodes, input.nodeId, &ProjectNode::instanceId);
                    const auto bindsVariable = bound != definition->graph.nodes.end()
                        && bound->typeId == "talas.variable"
                        && !bound->soundDefinitionId.has_value();
                    auto endpoint = resolveEndpoint(
                        child,
                        input.nodeId,
                        bindsVariable ? std::string_view{"audioIn"} : std::string_view{input.nodePortId},
                        !input.replacesSound);
                    if (endpoint.has_value() && bindsVariable)
                        endpoint->variable = true;
                    if (endpoint.has_value() && input.replacesSound)
                        endpoint->replaces = true;
                    if (endpoint.has_value())
                    {
                        nodeExpansion.inputs.emplace(
                            input.portId,
                            *endpoint);
                    }
                }
                for (const auto& output : definition->outputs)
                {
                    const auto endpoint = resolveEndpoint(
                        child,
                        output.nodeId,
                        output.nodePortId,
                        false);
                    if (endpoint.has_value())
                    {
                        nodeExpansion.outputs.emplace(
                            output.portId,
                            *endpoint);
                    }
                }
            }
        }
        expanded.nodes.emplace(
            node.instanceId,
            std::move(nodeExpansion));
    }

    for (const auto& connection : graph.connections)
    {
        if (!context.validation.ok())
            return {};
        auto source = resolveEndpoint(
            expanded,
            connection.fromNode,
            connection.fromPort,
            false);
        // A Variable given a Sound is heard as that Sound.
        if (source.has_value())
        {
            if (const auto given = context.givenVariables.find(source->nodeId);
                given != context.givenVariables.end())
            {
                source = given->second;
            }
        }
        const auto target = resolveEndpoint(
            expanded,
            connection.toNode,
            connection.toPort,
            true);
        if (!source.has_value() || !target.has_value())
            continue;
        if (target->replaces)
        {
            // The Sound connected to the replaceable input plays wherever
            // the Sound it replaces fed; that one is left unheard.
            if (!context.replacedOutputs.emplace(target->nodeId + '\n' + target->portId).second)
            {
                failExpansion(
                    context,
                    "A Sound instance's replaceable input takes exactly one Sound",
                    connection.toNode);
                return {};
            }
            for (auto& heard : context.destination.graph.connections)
            {
                if (heard.fromNode == target->nodeId && heard.fromPort == target->portId)
                {
                    heard.fromNode = source->nodeId;
                    heard.fromPort = source->portId;
                }
            }
            continue;
        }
        if (target->variable)
        {
            // The Sound given the Variable plays where the Variable was, in
            // place of its declaration or silence; the Variable is left
            // silent and unheard.
            const auto variable = std::ranges::find(
                context.destination.graph.nodes, target->nodeId, &ProjectNode::instanceId);
            if (variable == context.destination.graph.nodes.end()
                || variable->typeId != "talas.variable"
                || !context.givenVariables.emplace(target->nodeId, *source).second)
            {
                failExpansion(
                    context,
                    "A Sound instance's input bound to a Variable must give it exactly one Sound",
                    connection.toNode);
                return {};
            }
            variable->typeId = "talas.valToSound";
            variable->parameters = {{"value", "0"}};
            for (auto& heard : context.destination.graph.connections)
            {
                if (heard.fromNode == target->nodeId)
                {
                    heard.fromNode = source->nodeId;
                    heard.fromPort = source->portId;
                }
            }
            continue;
        }
        if (!instancePath.empty())
        {
            if (context.expandedConnectionCount
                >= maxExpandedConnectionCount)
            {
                failExpansion(
                    context,
                    "Nested Sound connection expansion limit exceeded",
                    connection.toNode);
                return {};
            }
            ++context.expandedConnectionCount;
        }
        context.destination.graph.connections.push_back({
            source->nodeId,
            source->portId,
            target->nodeId,
            target->portId,
        });
    }
    return expanded;
}

[[nodiscard]] bool validGreenVariableName(std::string_view name) noexcept
{
    if (name.size() < 2 || name.front() != '?')
        return false;
    const auto letter = [](char value) {
        return (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z');
    };
    if (!letter(name[1]))
        return false;
    for (const auto value : name.substr(2))
    {
        if (!letter(value) && (value < '0' || value > '9')
            && value != '_')
        {
            return false;
        }
    }
    return name != "?VoiceNumber";
}

[[nodiscard]] bool lowerSampleWithTimeIndex(
    ProjectSoundExpansionResult& result)
{
    for (auto& node : result.document.graph.nodes)
    {
        if (node.typeId != "talas.sampleWithTimeIndex")
            continue;
        const auto sample = std::ranges::find(
            node.parameters, std::string_view{"sample"},
            &ProjectParameterExpression::parameterId);
        if (sample == node.parameters.end()
            || !sample->resource.has_value()
            || sample->resource->empty())
        {
            result.validation.errors.push_back({
                talas::core::ValidationCode::InvalidParameterTransformation,
                "SampleWithTimeIndex requires one static sample resource",
                node.instanceId,
            });
            return false;
        }
        node.typeId = "talas.waveshaper";
        sample->parameterId = "shapingFunction";
        for (auto& edge : result.document.graph.connections)
        {
            if (edge.toNode == node.instanceId
                && edge.toPort == "timeIndex")
            {
                edge.toPort = "audioIn";
            }
        }
        for (auto& binding : result.manifest.nodeParameters)
        {
            if (binding.flatNodeId == node.instanceId
                && binding.parameterId == "sample")
            {
                binding.parameterId = "shapingFunction";
            }
        }
    }
    return true;
}

[[nodiscard]] bool lowerMultiSampleWithTimeIndex(
    ProjectSoundExpansionResult& result)
{
    auto& graph = result.document.graph;
    std::unordered_set<std::string> usedIds;
    for (const auto& node : graph.nodes)
        usedIds.insert(node.instanceId);
    std::size_t generatedOrdinal = 0;
    const auto generatedId = [&] {
        std::string id;
        do
        {
            id = "__kc_multi_sample_" + std::to_string(generatedOrdinal++);
        } while (!usedIds.insert(id).second);
        return id;
    };
    const auto fail = [&](const ProjectNode& node, std::string message) {
        result.validation.errors.push_back({
            talas::core::ValidationCode::InvalidParameterTransformation,
            std::move(message), node.instanceId});
        return false;
    };
    std::vector<std::string> loweredIds;
    for (const auto& node : graph.nodes)
    {
        if (node.typeId == "talas.multiSampleWithTimeIndex")
            loweredIds.push_back(node.instanceId);
    }
    for (const auto& loweredId : loweredIds)
    {
        const auto original = std::ranges::find(
            graph.nodes, loweredId, &ProjectNode::instanceId);
        if (original == graph.nodes.end())
            return false;
        const auto node = *original;
        const auto fileNames = std::ranges::find(
            node.parameters, std::string_view{"fileNames"},
            &ProjectParameterExpression::parameterId);
        const auto resources = fileNames != node.parameters.end()
            && fileNames->text.has_value()
            ? talas::core::parseBoundedStaticResourceList(*fileNames->text)
            : std::nullopt;
        if (!resources.has_value()
            || resources->size() < 2 || resources->size() > 16)
        {
            return fail(node,
                "MultiSampleWithTimeIndex requires two through sixteen static file names");
        }
        const auto timeIndexEdge = std::ranges::find_if(
            graph.connections, [&](const auto& edge) {
                return edge.toNode == loweredId
                    && edge.toPort == "timeIndex";
            });
        if (timeIndexEdge == graph.connections.end())
            return fail(node, "MultiSampleWithTimeIndex requires a time index input");
        const auto sourceTimeIndex = *timeIndexEdge;
        const auto selectionEdge = std::ranges::find_if(
            graph.connections, [&](const auto& edge) {
                return edge.toNode == loweredId
                    && edge.toPort == "index";
            });
        const bool connectedSelection =
            selectionEdge != graph.connections.end();
        const auto indexParameter = std::ranges::find(
            node.parameters, std::string_view{"index"},
            &ProjectParameterExpression::parameterId);
        const auto interpolationParameter = std::ranges::find(
            node.parameters, std::string_view{"interpolation"},
            &ProjectParameterExpression::parameterId);

        const auto identity = std::ranges::find(
            result.manifest.nodes, loweredId,
            &ExpandedSoundNodeIdentity::flatNodeId);
        if (identity == result.manifest.nodes.end())
            return fail(node, "MultiSampleWithTimeIndex has no expanded identity");
        const auto sourceIdentity = *identity;
        const auto addNode = [&](ProjectNode child) {
            auto childIdentity = sourceIdentity;
            childIdentity.flatNodeId = child.instanceId;
            result.manifest.nodes.push_back(std::move(childIdentity));
            graph.nodes.push_back(std::move(child));
        };
        const auto connect = [&](std::string fromNode,
                                 std::string fromPort,
                                 std::string toNode,
                                 std::string toPort) {
            graph.connections.push_back({
                std::move(fromNode), std::move(fromPort),
                std::move(toNode), std::move(toPort)});
        };

        std::string selectorNode;
        std::string selectorPort;
        if (connectedSelection)
        {
            selectorNode = selectionEdge->fromNode;
            selectorPort = selectionEdge->fromPort;
        }
        else
        {
            selectorNode = generatedId();
            selectorPort = "value";
            ProjectParameterExpression value{
                "value", std::string{"0"}};
            if (indexParameter != node.parameters.end())
            {
                value = *indexParameter;
                value.parameterId = "value";
            }
            addNode({selectorNode, "talas.constant", {value}});
        }

        std::vector<std::string> readers;
        std::vector<std::string> masks;
        readers.reserve(resources->size());
        masks.reserve(resources->size());
        for (std::size_t index = 0; index < resources->size(); ++index)
        {
            const auto reader = generatedId();
            std::vector<ProjectParameterExpression> parameters{
                {"sample", {}, {}, {}, (*resources)[index]}};
            if (interpolationParameter != node.parameters.end())
                parameters.push_back(*interpolationParameter);
            addNode({reader, "talas.sampleWithTimeIndex",
                std::move(parameters)});
            connect(sourceTimeIndex.fromNode, sourceTimeIndex.fromPort,
                    reader, "timeIndex");
            readers.push_back(reader);

            const auto ordinal = generatedId();
            addNode({ordinal, "talas.constant", {
                {"value", std::to_string(index)}}});
            const auto mask = generatedId();
            addNode({mask, "talas.equality", {}});
            connect(selectorNode, selectorPort, mask, "left");
            connect(ordinal, "value", mask, "right");
            masks.push_back(mask);
        }

        std::string mixed;
        for (std::size_t index = 1; index < readers.size(); ++index)
        {
            const auto mixer = generatedId();
            addNode({mixer, "talas.mixer", {}});
            connect(index == 1 ? readers[0] : mixed,
                    "audioOut", mixer, "in1");
            connect(readers[index], "audioOut", mixer, "in2");
            if (index == 1)
                connect(masks[0], "value", mixer, "gain1");
            connect(masks[index], "value", mixer, "gain2");
            mixed = mixer;
        }
        for (auto& edge : graph.connections)
        {
            if (edge.fromNode == loweredId)
            {
                edge.fromNode = mixed;
                edge.fromPort = "audioOut";
            }
        }
        std::erase_if(graph.connections, [&](const auto& edge) {
            return edge.toNode == loweredId;
        });
        if (!connectedSelection)
        {
            for (auto& binding : result.manifest.nodeParameters)
            {
                if (binding.flatNodeId == loweredId
                    && binding.parameterId == "index")
                {
                    binding.flatNodeId = selectorNode;
                    binding.parameterId = "value";
                }
            }
        }
        std::erase_if(result.manifest.nodeParameters,
            [&](const auto& binding) {
                return binding.flatNodeId == loweredId;
            });
        std::erase_if(result.manifest.nodes, [&](const auto& entry) {
            return entry.flatNodeId == loweredId;
        });
        std::erase_if(graph.nodes, [&](const auto& entry) {
            return entry.instanceId == loweredId;
        });
    }
    return true;
}

[[nodiscard]] bool lowerSplitSurroundSampleWithTimeIndex(
    ProjectSoundExpansionResult& result)
{
    auto& graph = result.document.graph;
    std::unordered_set<std::string> usedIds;
    for (const auto& node : graph.nodes)
        usedIds.insert(node.instanceId);
    std::size_t generatedOrdinal = 0;
    const auto generatedId = [&] {
        std::string id;
        do
        {
            id = "__kc_split_surround_"
                + std::to_string(generatedOrdinal++);
        } while (!usedIds.insert(id).second);
        return id;
    };
    const auto fail = [&](const ProjectNode& node,
                          std::string message) {
        result.validation.errors.push_back({
            talas::core::ValidationCode::InvalidParameterTransformation,
            std::move(message), node.instanceId});
        return false;
    };
    std::vector<std::string> loweredIds;
    for (const auto& node : graph.nodes)
    {
        if (node.typeId == "talas.splitSurroundSampleWithTimeIndex")
            loweredIds.push_back(node.instanceId);
    }
    for (const auto& loweredId : loweredIds)
    {
        const auto original = std::ranges::find(
            graph.nodes, loweredId, &ProjectNode::instanceId);
        if (original == graph.nodes.end())
            return false;
        const auto node = *original;
        const auto fileNames = std::ranges::find(
            node.parameters, std::string_view{"fileNames"},
            &ProjectParameterExpression::parameterId);
        const auto resources = fileNames != node.parameters.end()
            && fileNames->text.has_value()
            ? talas::core::parseBoundedStaticResourceList(
                *fileNames->text)
            : std::nullopt;
        if (!resources.has_value()
            || resources->size() < 2 || resources->size() > 8)
            return fail(node,
                "SplitSurroundSampleWithTimeIndex requires two through eight ordered static file names");
        const auto timeIndexEdge = std::ranges::find_if(
            graph.connections, [&](const auto& edge) {
                return edge.toNode == loweredId
                    && edge.toPort == "timeIndex";
            });
        if (timeIndexEdge == graph.connections.end())
            return fail(node,
                "SplitSurroundSampleWithTimeIndex requires a time index input");
        const auto sourceTimeIndex = *timeIndexEdge;
        const auto interpolationParameter = std::ranges::find(
            node.parameters, std::string_view{"interpolation"},
            &ProjectParameterExpression::parameterId);
        const auto identity = std::ranges::find(
            result.manifest.nodes, loweredId,
            &ExpandedSoundNodeIdentity::flatNodeId);
        if (identity == result.manifest.nodes.end())
            return fail(node,
                "SplitSurroundSampleWithTimeIndex has no expanded identity");
        const auto sourceIdentity = *identity;
        result.manifest.splitSurroundResourceGroups.push_back({
            loweredId, *resources});
        std::vector<std::string> readers;
        readers.reserve(resources->size());
        for (const auto& resource : *resources)
        {
            const auto reader = generatedId();
            std::vector<ProjectParameterExpression> parameters{
                {"sample", {}, {}, {}, resource}};
            if (interpolationParameter != node.parameters.end())
                parameters.push_back(*interpolationParameter);
            auto childIdentity = sourceIdentity;
            childIdentity.flatNodeId = reader;
            result.manifest.nodes.push_back(std::move(childIdentity));
            graph.nodes.push_back({reader, "talas.sampleWithTimeIndex",
                std::move(parameters)});
            graph.connections.push_back({
                sourceTimeIndex.fromNode, sourceTimeIndex.fromPort,
                reader, "timeIndex"});
            readers.push_back(reader);
        }
        for (auto& edge : graph.connections)
        {
            if (edge.fromNode != loweredId)
                continue;
            if (edge.fromPort.size() != 4
                || edge.fromPort.substr(0, 3) != "out"
                || edge.fromPort[3] < '1'
                || edge.fromPort[3] > '8')
                return fail(node,
                    "SplitSurroundSampleWithTimeIndex has an invalid channel output");
            const auto channel = static_cast<std::size_t>(
                edge.fromPort[3] - '1');
            if (channel >= readers.size())
                return fail(node,
                    "SplitSurroundSampleWithTimeIndex output exceeds its file count");
            edge.fromNode = readers[channel];
            edge.fromPort = "audioOut";
        }
        std::erase_if(graph.connections, [&](const auto& edge) {
            return edge.toNode == loweredId;
        });
        std::erase_if(result.manifest.nodeParameters,
            [&](const auto& binding) {
                return binding.flatNodeId == loweredId;
            });
        std::erase_if(result.manifest.nodes,
            [&](const auto& entry) {
                return entry.flatNodeId == loweredId;
            });
        std::erase_if(graph.nodes, [&](const auto& entry) {
            return entry.instanceId == loweredId;
        });
    }
    return true;
}

[[nodiscard]] bool lowerSplitSurroundOscillator(
    ProjectSoundExpansionResult& result)
{
    auto& graph = result.document.graph;
    std::unordered_set<std::string> usedIds;
    for (const auto& node : graph.nodes)
        usedIds.insert(node.instanceId);
    std::size_t generatedOrdinal = 0;
    const auto generatedId = [&] {
        std::string id;
        do
        {
            id = "__kc_split_osc_"
                + std::to_string(generatedOrdinal++);
        } while (!usedIds.insert(id).second);
        return id;
    };
    const auto fail = [&](std::string_view nodeId, std::string message) {
        result.validation.errors.push_back({
            talas::core::ValidationCode::InvalidParameterTransformation,
            std::move(message), std::string{nodeId}});
        return false;
    };
    std::vector<std::string> loweredIds;
    for (const auto& node : graph.nodes)
        if (node.typeId == "talas.splitSurroundOscillator")
            loweredIds.push_back(node.instanceId);
    for (const auto& loweredId : loweredIds)
    {
        const auto original = std::ranges::find(
            graph.nodes, loweredId, &ProjectNode::instanceId);
        if (original == graph.nodes.end())
            return fail(loweredId,
                "SplitSurroundOscillator disappeared during expansion");
        const auto node = *original;
        const auto parameter = [&](std::string_view id) {
            return std::ranges::find(
                node.parameters, id,
                &ProjectParameterExpression::parameterId);
        };
        // Left out, FileNames is the catalog's Sine for each of two
        // channels.
        const auto files = parameter("fileNames");
        const auto resources = talas::core::parseBoundedStaticResourceList(
            files != node.parameters.end() && files->text.has_value()
                ? std::string_view{*files->text}
                : std::string_view{"Sine Sine"});
        const auto channelsParameter = parameter("channels");
        const auto channelsText = channelsParameter != node.parameters.end()
            ? channelsParameter->choice.value_or("2")
            : std::string{"2"};
        if (channelsText.size() != 1
            || channelsText.front() < '1'
            || channelsText.front() > '8')
            return fail(loweredId,
                "SplitSurroundOscillator channels must be a static choice from 1 through 8");
        const auto channels = static_cast<std::size_t>(
            channelsText.front() - '0');
        if (!resources.has_value() || resources->empty()
            || resources->size() > 8
            || (resources->size() == 1
                ? channels > 2
                : resources->size() != channels))
            return fail(loweredId,
                "SplitSurroundOscillator requires one mono/stereo cycle file or one split file per output channel");
        const auto frequency = parameter("frequency");
        const auto interpolation = parameter("interpolation");
        const auto identity = std::ranges::find(
            result.manifest.nodes, loweredId,
            &ExpandedSoundNodeIdentity::flatNodeId);
        if (identity == result.manifest.nodes.end())
            return fail(loweredId,
                "SplitSurroundOscillator has no expanded identity");
        const auto sourceIdentity = *identity;
        const auto addNode = [&](ProjectNode child) {
            auto childIdentity = sourceIdentity;
            childIdentity.flatNodeId = child.instanceId;
            result.manifest.nodes.push_back(std::move(childIdentity));
            graph.nodes.push_back(std::move(child));
        };
        const auto phaseId = generatedId();
        ProjectParameterExpression phaseFrequency{
            "frequency", std::string{"440 hz"}};
        if (frequency != node.parameters.end())
            phaseFrequency = *frequency;
        addNode({phaseId, "talas.oscillatorTimeIndex", {phaseFrequency}});
        std::vector<std::string> readers;
        readers.reserve(resources->size());
        for (const auto& resource : *resources)
        {
            const auto readerId = generatedId();
            std::vector<ProjectParameterExpression> parameters{
                {"shapingFunction", {}, {}, {}, resource}};
            if (interpolation != node.parameters.end())
                parameters.push_back(*interpolation);
            addNode({readerId, "talas.waveshaper", std::move(parameters)});
            graph.connections.push_back({
                phaseId, "audioOut", readerId, "audioIn"});
            readers.push_back(readerId);
        }
        for (auto& edge : graph.connections)
        {
            if (edge.toNode == loweredId)
            {
                if (edge.toPort != "frequency")
                    return fail(loweredId,
                        "SplitSurroundOscillator received an unsupported input");
                edge.toNode = phaseId;
            }
            if (edge.fromNode != loweredId)
                continue;
            if (edge.fromPort.size() != 4
                || edge.fromPort.substr(0, 3) != "out"
                || edge.fromPort[3] < '1'
                || edge.fromPort[3] > '8')
                return fail(loweredId,
                    "SplitSurroundOscillator has an invalid output channel");
            const auto channel = static_cast<std::size_t>(
                edge.fromPort[3] - '1');
            if (channel >= channels)
                return fail(loweredId,
                    "SplitSurroundOscillator output exceeds its channel count");
            edge.fromNode = readers[resources->size() == 1 ? 0 : channel];
            edge.fromPort = resources->size() == 1 && channel == 1
                ? "rightOut" : "audioOut";
        }
        for (auto& binding : result.manifest.nodeParameters)
            if (binding.flatNodeId == loweredId
                && binding.parameterId == "frequency")
                binding.flatNodeId = phaseId;
        std::erase_if(result.manifest.nodeParameters,
            [&](const auto& binding) {
                return binding.flatNodeId == loweredId;
            });
        std::erase_if(result.manifest.nodes,
            [&](const auto& entry) {
                return entry.flatNodeId == loweredId;
            });
        std::erase_if(graph.nodes,
            [&](const auto& entry) {
                return entry.instanceId == loweredId;
            });
        result.manifest.splitSurroundResourceGroups.push_back({
            loweredId, *resources, channels});
    }
    return true;
}

[[nodiscard]] bool lowerSplitSurroundAntialiasedOscillator(
    ProjectSoundExpansionResult& result)
{
    auto& graph = result.document.graph;
    std::unordered_set<std::string> usedIds;
    for (const auto& node : graph.nodes)
        usedIds.insert(node.instanceId);
    std::size_t generatedOrdinal = 0;
    const auto generatedId = [&] {
        std::string id;
        do
        {
            id = "__kc_split_antialiased_"
                + std::to_string(generatedOrdinal++);
        } while (!usedIds.insert(id).second);
        return id;
    };
    const auto fail = [&](std::string_view nodeId, std::string message) {
        result.validation.errors.push_back({
            talas::core::ValidationCode::InvalidParameterTransformation,
            std::move(message), std::string{nodeId}});
        return false;
    };
    std::vector<std::string> loweredIds;
    for (const auto& node : graph.nodes)
        if (node.typeId == "talas.splitSurroundAntialiasedOscillator")
            loweredIds.push_back(node.instanceId);
    for (const auto& loweredId : loweredIds)
    {
        const auto original = std::ranges::find(
            graph.nodes, loweredId, &ProjectNode::instanceId);
        if (original == graph.nodes.end())
            return fail(loweredId,
                "SplitSurroundAntialiasedOscillator disappeared during expansion");
        const auto node = *original;
        const auto parameter = [&](std::string_view id) {
            return std::ranges::find(
                node.parameters, id,
                &ProjectParameterExpression::parameterId);
        };
        // Left out, FileNames is the catalog's Sine for each of two
        // channels.
        const auto files = parameter("fileNames");
        const auto resources = talas::core::parseBoundedStaticResourceList(
            files != node.parameters.end() && files->text.has_value()
                ? std::string_view{*files->text}
                : std::string_view{"Sine Sine"});
        const auto channelsParameter = parameter("channels");
        const auto channelsText = channelsParameter != node.parameters.end()
            ? channelsParameter->choice.value_or("2")
            : std::string{"2"};
        if (channelsText.size() != 1
            || channelsText.front() < '1'
            || channelsText.front() > '8')
            return fail(loweredId,
                "SplitSurroundAntialiasedOscillator channels must be a static choice from 1 through 8");
        const auto channels = static_cast<std::size_t>(
            channelsText.front() - '0');
        if (!resources.has_value() || resources->empty()
            || resources->size() > 8
            || (resources->size() == 1
                ? channels > 2
                : resources->size() != channels))
            return fail(loweredId,
                "SplitSurroundAntialiasedOscillator requires one mono/stereo cycle file or one split file per output channel");
        const auto identity = std::ranges::find(
            result.manifest.nodes, loweredId,
            &ExpandedSoundNodeIdentity::flatNodeId);
        if (identity == result.manifest.nodes.end())
            return fail(loweredId,
                "SplitSurroundAntialiasedOscillator has no expanded identity");
        const auto sourceIdentity = *identity;
        std::vector<std::string> readers;
        readers.reserve(channels);
        for (std::size_t channel = 0; channel < channels; ++channel)
        {
            const auto readerId = generatedId();
            const auto& resource = (*resources)[resources->size() == 1
                ? 0 : channel];
            const auto channelChoice = resources->size() == 1
                && channels == 2
                ? (channel == 0 ? "left" : "right")
                : "mono";
            std::vector<ProjectParameterExpression> parameters{
                {"fileName", {}, {}, {}, resource},
                {"channel", {}, std::string{channelChoice}},
            };
            for (const auto id : {"frequency", "brightness", "dutyCycle"})
            {
                const auto source = parameter(id);
                if (source != node.parameters.end())
                    parameters.push_back(*source);
            }
            auto childIdentity = sourceIdentity;
            childIdentity.flatNodeId = readerId;
            result.manifest.nodes.push_back(std::move(childIdentity));
            graph.nodes.push_back({
                readerId, "talas.antialiasedOscillator",
                std::move(parameters)});
            readers.push_back(readerId);
        }
        auto fanout = graph.connections;
        fanout.clear();
        for (auto& edge : graph.connections)
        {
            if (edge.toNode == loweredId)
            {
                if (edge.toPort != "frequency"
                    && edge.toPort != "brightness")
                    return fail(loweredId,
                        "SplitSurroundAntialiasedOscillator received an unsupported input");
                for (const auto& reader : readers)
                    fanout.push_back({
                        edge.fromNode, edge.fromPort,
                        reader, edge.toPort});
            }
            if (edge.fromNode != loweredId)
                continue;
            if (edge.fromPort.size() != 4
                || edge.fromPort.substr(0, 3) != "out"
                || edge.fromPort[3] < '1'
                || edge.fromPort[3] > '8')
                return fail(loweredId,
                    "SplitSurroundAntialiasedOscillator has an invalid output channel");
            const auto channel = static_cast<std::size_t>(
                edge.fromPort[3] - '1');
            if (channel >= channels)
                return fail(loweredId,
                    "SplitSurroundAntialiasedOscillator output exceeds its channel count");
            edge.fromNode = readers[channel];
            edge.fromPort = "audioOut";
        }
        std::erase_if(graph.connections, [&](const auto& edge) {
            return edge.toNode == loweredId;
        });
        graph.connections.insert(
            graph.connections.end(), fanout.begin(), fanout.end());

        std::vector<ExpandedSoundNodeParameterBinding> clonedBindings;
        for (const auto& binding : result.manifest.nodeParameters)
        {
            if (binding.flatNodeId != loweredId
                || (binding.parameterId != "frequency"
                    && binding.parameterId != "brightness"
                    && binding.parameterId != "dutyCycle"))
                continue;
            for (const auto& reader : readers)
            {
                auto clone = binding;
                clone.flatNodeId = reader;
                clonedBindings.push_back(std::move(clone));
            }
        }
        std::erase_if(result.manifest.nodeParameters,
            [&](const auto& binding) {
                return binding.flatNodeId == loweredId;
            });
        result.manifest.nodeParameters.insert(
            result.manifest.nodeParameters.end(),
            clonedBindings.begin(), clonedBindings.end());
        std::erase_if(result.manifest.nodes,
            [&](const auto& entry) {
                return entry.flatNodeId == loweredId;
            });
        std::erase_if(graph.nodes,
            [&](const auto& entry) {
                return entry.instanceId == loweredId;
            });
        result.manifest.splitSurroundResourceGroups.push_back({
            loweredId, *resources, channels, true});
    }
    return true;
}

[[nodiscard]] bool applyPrefixerEventNames(
    ProjectSoundExpansionResult& result)
{
    auto& graph = result.document.graph;
    std::vector<std::string> wrapperIds;
    for (const auto& node : graph.nodes)
        if (node.typeId == "talas.prefixer")
            wrapperIds.push_back(node.instanceId);
    if (wrapperIds.empty())
        return true;
    const auto fail = [&](std::string_view nodeId, std::string message) {
        result.validation.errors.push_back({
            talas::core::ValidationCode::InvalidParameterTransformation,
            std::move(message), std::string{nodeId}});
        return false;
    };
    const auto validName = [](std::string_view name) {
        if (name.empty()
            || name.size() >= talas::expr::ExpressionLimits::maximumSourceBytes)
            return false;
        const auto start = [](char ch) {
            return (ch >= 'a' && ch <= 'z')
                || (ch >= 'A' && ch <= 'Z') || ch == '_';
        };
        return start(name.front())
            && std::ranges::all_of(name.substr(1), [&](char ch) {
                return start(ch) || (ch >= '0' && ch <= '9');
            });
    };
    const auto nodeFor = [&](std::string_view id) {
        return std::ranges::find(
            graph.nodes, id, &ProjectNode::instanceId);
    };
    const auto inputFor = [&](std::string_view id) {
        return std::ranges::find_if(
            graph.connections, [&](const auto& edge) {
                return edge.toNode == id && edge.toPort == "audioIn";
            });
    };
    const auto upstreamSize = [&](const std::string& id) {
        std::unordered_set<std::string> seen;
        std::vector<std::string> pending{id};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            if (!seen.insert(pending[index]).second)
                continue;
            for (const auto& edge : graph.connections)
                if (edge.toNode == pending[index])
                    pending.push_back(edge.fromNode);
        }
        return seen.size();
    };
    std::ranges::stable_sort(wrapperIds,
        [&](const auto& left, const auto& right) {
            return upstreamSize(left) < upstreamSize(right);
        });

    for (const auto& wrapperId : wrapperIds)
    {
        const auto wrapper = nodeFor(wrapperId);
        const auto input = inputFor(wrapperId);
        if (wrapper == graph.nodes.end()
            || input == graph.connections.end()
            || std::ranges::count_if(
                   graph.connections, [&](const auto& edge) {
                       return edge.toNode == wrapperId;
                   }) != 1)
            return fail(wrapperId,
                "Prefixer requires one isolated upstream mono Sound");
        const auto parameter = [&](std::string_view id)
            -> const ProjectParameterExpression* {
            const auto found = std::ranges::find(
                wrapper->parameters, id,
                &ProjectParameterExpression::parameterId);
            return found == wrapper->parameters.end() ? nullptr : &*found;
        };
        const auto* idParameter = parameter("id");
        const auto* specialParameter = parameter("specialEvents");
        const auto* modeParameter = parameter("renameSpecialEvents");
        const auto sourceNodeId = input->fromNode;
        const auto sourcePortId = input->fromPort;
        if (idParameter != nullptr && !idParameter->text.has_value())
            return fail(wrapperId,
                "Prefixer Id must be static text");
        std::string prefix = idParameter != nullptr
            ? *idParameter->text : std::string{};
        if (prefix.empty())
        {
            // An unfilled Id uses the input Sound's name. Reusable Sounds
            // carry an authored display name across an instance boundary;
            // a node in the same scope has only its local ID.
            const auto wrapperIdentity = std::ranges::find(
                result.manifest.nodes, wrapperId,
                &ExpandedSoundNodeIdentity::flatNodeId);
            const auto identity = std::ranges::find(
                result.manifest.nodes, sourceNodeId,
                &ExpandedSoundNodeIdentity::flatNodeId);
            if (identity != result.manifest.nodes.end())
            {
                prefix = identity->localNodeId;
                if (!identity->definitionId.empty()
                    && (wrapperIdentity == result.manifest.nodes.end()
                        || identity->scopeOrdinal
                            != wrapperIdentity->scopeOrdinal))
                {
                    const auto* definition = findDefinition(
                        result.document, identity->definitionId);
                    if (definition != nullptr)
                        prefix = definition->displayName;
                }
            }
            if (prefix.empty())
                prefix = sourceNodeId;
        }
        const auto specialText = specialParameter != nullptr
            ? specialParameter->text.value_or("") : std::string{};
        if (!validName(prefix)
            || (modeParameter != nullptr
                && !modeParameter->boolean.has_value()))
            return fail(wrapperId,
                "Prefixer requires a valid explicit or input-Sound Id and optional RenameSpecialEvents boolean");
        const bool renameSelected = modeParameter != nullptr
            && *modeParameter->boolean;
        std::unordered_set<std::string> special;
        if (!specialText.empty())
        {
            const auto members =
                talas::core::parseBoundedStaticResourceList(specialText);
            if (!members.has_value())
                return fail(wrapperId,
                    "Prefixer SpecialEvents requires a bounded static !Name list");
            for (const auto& member : *members)
            {
                if (!member.starts_with('!')
                    || !validName(std::string_view{member}.substr(1))
                    || !special.insert(member.substr(1)).second)
                    return fail(wrapperId,
                        "Prefixer SpecialEvents contains an invalid or duplicate !Name");
            }
        }

        std::unordered_set<std::string> upstream;
        std::vector<std::string> pending{sourceNodeId};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            const auto id = pending[index];
            if (!upstream.insert(id).second)
                continue;
            if (nodeFor(id) == graph.nodes.end())
                return fail(wrapperId, "Prefixer lost an upstream Sound");
            for (const auto& edge : graph.connections)
                if (edge.toNode == id)
                    pending.push_back(edge.fromNode);
        }
        for (const auto& edge : graph.connections)
            if (upstream.contains(edge.fromNode)
                && edge.toNode != wrapperId
                && !upstream.contains(edge.toNode))
                return fail(wrapperId,
                    "Prefixer upstream Sound is also routed outside its rename scope");

        std::unordered_set<std::uint32_t> upstreamScopes;
        for (const auto& identity : result.manifest.nodes)
        {
            if (!upstream.contains(identity.flatNodeId))
                continue;
            auto scopeOrdinal = identity.scopeOrdinal;
            while (scopeOrdinal != 0
                && upstreamScopes.insert(scopeOrdinal).second)
            {
                const auto scope = std::ranges::find(
                    result.manifest.scopes, scopeOrdinal,
                    &ExpandedSoundScopeIdentity::scopeOrdinal);
                if (scope == result.manifest.scopes.end())
                    break;
                scopeOrdinal = scope->parentScopeOrdinal;
            }
        }
        if (std::ranges::any_of(
                result.manifest.eventValues,
                [&](const auto& binding) {
                    return upstreamScopes.contains(binding.scopeOrdinal);
                }))
            return fail(wrapperId,
                "Prefixer local Sound Event Value bindings are not yet supported");

        std::unordered_set<std::string> generatedIndexNames;
        const auto scans = [&](const ProjectNode& node,
                               std::unordered_set<std::string>& names) {
            for (const auto& parameterValue : node.parameters)
            {
                const auto add = [&](std::string_view source) {
                    const auto scanned = talas::expr::scanEventReferenceNames(
                        source, result.document.expressionLanguage);
                    if (!scanned.has_value())
                        return false;
                    names.insert(scanned->begin(), scanned->end());
                    return true;
                };
                if (parameterValue.expression.has_value()
                    && !add(*parameterValue.expression))
                    return false;
                if (node.typeId == "talas.analogSequencer"
                    && parameterValue.parameterId == "generatedIndex"
                    && parameterValue.text.has_value())
                {
                    const auto target =
                        talas::core::parseAnalogSequencerGeneratedIndex(
                            *parameterValue.text);
                    if (!target.has_value())
                        return false;
                    if (!target->empty())
                    {
                        names.insert(*target);
                        if (upstream.contains(node.instanceId))
                            generatedIndexNames.insert(*target);
                    }
                }
                if (node.typeId == "talas.analogSequencer"
                    && parameterValue.parameterId != "generatedIndex"
                    && isAnalogSequencerEventText(parameterValue.parameterId)
                    && parameterValue.text.has_value()
                    && parameterValue.text->find('!') != std::string::npos
                    && !add(*parameterValue.text))
                    return false;
                if ((node.typeId == "talas.soundToGlobalController"
                        || node.typeId
                            == "talas.triggeredSoundToGlobalController")
                    && (parameterValue.parameterId == "generatedEvent"
                        || parameterValue.parameterId == "value")
                    && parameterValue.text.has_value()
                    && !add(*parameterValue.text))
                    return false;
            }
            return true;
        };
        std::unordered_set<std::string> allNames;
        std::unordered_set<std::string> scopeNames;
        for (const auto& node : graph.nodes)
        {
            if (node.instanceId == wrapperId)
                continue;
            if (!scans(node, allNames))
                return fail(wrapperId,
                    "Prefixer encountered an oversized Event Value field");
            if (upstream.contains(node.instanceId)
                && !scans(node, scopeNames))
                return fail(wrapperId,
                    "Prefixer encountered an oversized upstream Event Value field");
        }
        std::vector<std::pair<std::string, std::string>> mapped;
        for (const auto& name : scopeNames)
        {
            const bool listed = special.contains(name);
            if (listed != renameSelected)
                continue;
            auto next = prefix + name;
            if (!validName(next)
                || (generatedIndexNames.contains(name)
                    && next.size() > 128)
                || allNames.contains(next))
                return fail(wrapperId,
                    "Prefixer generated an invalid or colliding Event Value name");
            mapped.emplace_back(name, std::move(next));
        }
        std::vector<talas::expr::EventReferenceRename> renames;
        renames.reserve(mapped.size());
        for (const auto& [from, to] : mapped)
            renames.push_back({from, to});
        for (auto& node : graph.nodes)
        {
            if (!upstream.contains(node.instanceId))
                continue;
            for (auto& value : node.parameters)
            {
                const auto rename = [&](std::string& source) {
                    const auto rewritten = talas::expr::renameEventReferences(
                        source, result.document.expressionLanguage, renames);
                    if (!rewritten.has_value())
                        return false;
                    source = *rewritten;
                    return true;
                };
                if (value.expression.has_value()
                    && !rename(*value.expression))
                    return fail(wrapperId,
                        "Prefixer expression exceeds its renamed size limit");
                if ((node.typeId == "talas.soundToGlobalController"
                        || node.typeId
                            == "talas.triggeredSoundToGlobalController")
                    && (value.parameterId == "generatedEvent"
                        || value.parameterId == "value")
                    && value.text.has_value()
                    && !rename(*value.text))
                    return fail(wrapperId,
                        "Prefixer terminal field exceeds its renamed size limit");
                if (node.typeId == "talas.analogSequencer"
                    && isAnalogSequencerEventText(value.parameterId)
                    && value.text.has_value()
                    && value.text->find('!') != std::string::npos
                    && !rename(*value.text))
                    return fail(wrapperId,
                        "Prefixer Analog Sequencer field exceeds its renamed size limit");
            }
        }
        for (auto& edge : graph.connections)
            if (edge.fromNode == wrapperId)
            {
                edge.fromNode = sourceNodeId;
                edge.fromPort = sourcePortId;
            }
        std::erase_if(graph.connections, [&](const auto& edge) {
            return edge.toNode == wrapperId;
        });
        std::erase_if(graph.nodes, [&](const auto& node) {
            return node.instanceId == wrapperId;
        });
        std::erase_if(result.manifest.nodes, [&](const auto& node) {
            return node.flatNodeId == wrapperId;
        });
        std::erase_if(result.manifest.nodeParameters, [&](const auto& binding) {
            return binding.flatNodeId == wrapperId;
        });
    }
    return true;
}

[[nodiscard]] std::optional<std::vector<std::string>>
parseReplicatorLabels(std::string_view text)
{
    if (text.size() > talas::core::maximumStaticResourceListTextBytes)
        return std::nullopt;
    if (text.empty())
        return std::vector<std::string>{};
    const auto separator = [](char ch) {
        return ch == ',' || ch == ' ' || ch == '\t' || ch == '\n'
            || ch == '\r';
    };
    const auto labelCharacter = [](char ch) {
        return (ch >= 'a' && ch <= 'z')
            || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9') || ch == '_';
    };
    std::vector<std::string> labels;
    for (std::size_t index = 0; index < text.size();)
    {
        while (index < text.size() && separator(text[index]))
            ++index;
        if (index == text.size())
            break;
        const bool quoted = text[index] == '\'';
        if (quoted)
            ++index;
        const auto start = index;
        while (index < text.size()
            && (quoted ? text[index] != '\'' : !separator(text[index])))
            ++index;
        if (quoted && index == text.size())
            return std::nullopt;
        const auto label = text.substr(start, index - start);
        if (label.empty()
            || !std::ranges::all_of(label, labelCharacter)
            || labels.size() >= 64)
            return std::nullopt;
        labels.emplace_back(label);
        if (quoted)
        {
            ++index;
            if (index < text.size() && !separator(text[index]))
                return std::nullopt;
        }
    }
    if (labels.empty())
        return std::nullopt;
    return labels;
}

[[nodiscard]] std::optional<std::string> replicatorLabelForVoice(
    const std::vector<std::string>& labels, std::size_t voice,
    bool increment)
{
    if (labels.empty())
        return std::string{};
    const auto& label = labels[std::min(voice - 1, labels.size() - 1)];
    if (!increment)
        return label;
    std::string result;
    for (std::size_t index = 0; index < label.size();)
    {
        if (label[index] < '0' || label[index] > '9')
        {
            result.push_back(label[index++]);
            continue;
        }
        const auto start = index;
        while (index < label.size()
            && label[index] >= '0' && label[index] <= '9')
            ++index;
        std::uint64_t number = 0;
        const auto parsed = std::from_chars(
            label.data() + start, label.data() + index, number);
        if (parsed.ec != std::errc{}
            || number > std::numeric_limits<std::uint64_t>::max()
                - (voice - 1))
            return std::nullopt;
        const auto next = std::to_string(number + voice - 1);
        result.append(std::max(next.size(), index - start) - next.size(), '0');
        result += next;
    }
    return result;
}

[[nodiscard]] bool applyReplicatorCopies(
    ProjectSoundExpansionResult& result)
{
    auto& graph = result.document.graph;
    std::vector<std::string> wrapperIds;
    for (const auto& node : graph.nodes)
        if (node.typeId == "talas.replicator"
            || node.typeId == "talas.stereoizer"
            || node.typeId == "talas.surroundifer")
            wrapperIds.push_back(node.instanceId);
    if (wrapperIds.empty())
        return true;
    const auto nodeFor = [&](std::string_view id) {
        return std::ranges::find(graph.nodes, id, &ProjectNode::instanceId);
    };
    const auto fail = [&](std::string_view id, std::string message,
                          talas::core::ValidationCode code =
                              talas::core::ValidationCode::InvalidParameterTransformation) {
        const auto wrapper = nodeFor(id);
        if (wrapper != graph.nodes.end()
            && wrapper->typeId == "talas.stereoizer"
            && message.starts_with("Replicator"))
            message.replace(0, std::string_view{"Replicator"}.size(),
                "Stereoizer");
        if (wrapper != graph.nodes.end()
            && wrapper->typeId == "talas.surroundifer"
            && message.starts_with("Replicator"))
            message.replace(0, std::string_view{"Replicator"}.size(),
                "Surroundifer");
        result.validation.errors.push_back({
            code, std::move(message), std::string{id}});
        return false;
    };
    const auto upstreamSize = [&](const std::string& id) {
        std::unordered_set<std::string> seen;
        std::vector<std::string> pending{id};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            if (!seen.insert(pending[index]).second)
                continue;
            for (const auto& edge : graph.connections)
                if (edge.toNode == pending[index])
                    pending.push_back(edge.fromNode);
        }
        return seen.size();
    };
    std::ranges::stable_sort(wrapperIds,
        [&](const auto& left, const auto& right) {
            return upstreamSize(left) < upstreamSize(right);
        });

    std::unordered_set<std::string> usedIds;
    for (const auto& node : graph.nodes)
        usedIds.insert(node.instanceId);
    const auto generatedId = [&](std::string_view wrapperId,
                                 std::string_view originalId,
                                 std::size_t voice,
                                 bool mixer = false) -> std::optional<std::string> {
        auto candidate = "__kc_rep_" + std::to_string(wrapperId.size())
            + "_" + std::string{wrapperId} + "_v" + std::to_string(voice)
            + (mixer ? "_m_" : "_n_")
            + std::to_string(originalId.size()) + "_"
            + std::string{originalId};
        if (candidate.size() > maximumGeneratedProjectSoundNodeIdBytes
            || !usedIds.insert(candidate).second)
            return std::nullopt;
        return candidate;
    };
    constexpr std::size_t maxCopies = 64;
    for (const auto& wrapperId : wrapperIds)
    {
        const auto wrapper = nodeFor(wrapperId);
        const bool stereoizer = wrapper != graph.nodes.end()
            && wrapper->typeId == "talas.stereoizer";
        const bool surroundifer = wrapper != graph.nodes.end()
            && wrapper->typeId == "talas.surroundifer";
        const bool spatializer = stereoizer || surroundifer;
        const auto input = std::ranges::find_if(
            graph.connections, [&](const auto& edge) {
                return edge.toNode == wrapperId && edge.toPort == "audioIn";
            });
        const auto rightInput = std::ranges::find_if(
            graph.connections, [&](const auto& edge) {
                return edge.toNode == wrapperId && edge.toPort == "rightIn";
            });
        const bool hasRightInput = spatializer
            && rightInput != graph.connections.end();
        if (wrapper == graph.nodes.end()
            || input == graph.connections.end()
            || std::ranges::count_if(graph.connections,
                   [&](const auto& edge) { return edge.toNode == wrapperId; })
                != (hasRightInput ? 2 : 1)
            || (surroundifer && !hasRightInput)
            || (!spatializer && rightInput != graph.connections.end())
            || std::ranges::count_if(graph.connections,
                   [&](const auto& edge) {
                       return edge.toNode == wrapperId
                           && edge.toPort == "audioIn";
                   }) != 1)
            return fail(wrapperId,
                spatializer
                    ? "Surround processing requires one processing input and a matching right shared input for Surroundifer"
                    : "Replicator requires exactly one upstream mono Sound");
        const auto rightSourceId = hasRightInput
            ? rightInput->fromNode : std::string{};
        const auto rightSourcePort = hasRightInput
            ? rightInput->fromPort : std::string{};
        const auto countParameter = std::ranges::find(
            wrapper->parameters, std::string_view{"number"},
            &ProjectParameterExpression::parameterId);
        const auto countSource = spatializer
            ? std::string{surroundifer ? "6" : "2"}
            : countParameter == wrapper->parameters.end()
                ? std::string{"1"}
                : countParameter->expression.value_or("");
        const auto countExpression = talas::expr::parse(
            countSource, result.document.expressionLanguage,
            result.document.expressionDefinitions);
        if (!countExpression.ok()
            || !countExpression.expression->eventReferences().empty()
            || !countExpression.expression->variableReferences().empty()
            || !countExpression.expression->identifiers().empty()
            || countExpression.expression->usesTime()
            || countExpression.expression->isStateful())
            return fail(wrapperId,
                "Replicator Number must be a static scalar expression");
        const auto countValue = talas::expr::evaluate(
            *countExpression.expression, {},
            {talas::expr::ValueDomain::Scalar});
        if (!countValue.ok()
            || !std::isfinite(countValue.value.value)
            || countValue.value.value < 1.0
            || countValue.value.value > static_cast<double>(maxCopies)
            || std::floor(countValue.value.value) != countValue.value.value)
            return fail(wrapperId,
                "Replicator Number must be an integer from 1 through 64");
        const auto copies = static_cast<std::size_t>(countValue.value.value);
        const auto parameter = [&](std::string_view id)
            -> const ProjectParameterExpression* {
            const auto found = std::ranges::find(
                wrapper->parameters, id,
                &ProjectParameterExpression::parameterId);
            return found == wrapper->parameters.end() ? nullptr : &*found;
        };
        const auto* specialParameter = parameter("specialEvents");
        const auto* modeParameter = parameter("renameSpecialEvents");
        const auto* prefixParameter = parameter("prefix");
        const auto* suffixParameter = parameter("suffix");
        const auto* incrementPrefixParameter = parameter("incrementPrefix");
        const auto* incrementSuffixParameter = parameter("incrementSuffix");
        const auto* leftBypassParameter = stereoizer
            ? parameter("leftBypass") : nullptr;
        const auto* rightBypassParameter = stereoizer
            ? parameter("rightBypass") : nullptr;
        if (modeParameter != nullptr
            && !modeParameter->boolean.has_value())
            return fail(wrapperId,
                "Replicator RenameSpecialEvents must be a static boolean");
        if ((prefixParameter != nullptr
                && !prefixParameter->text.has_value())
            || (suffixParameter != nullptr
                && !suffixParameter->text.has_value())
            || (incrementPrefixParameter != nullptr
                && !incrementPrefixParameter->boolean.has_value())
            || (incrementSuffixParameter != nullptr
                && !incrementSuffixParameter->boolean.has_value()))
            return fail(wrapperId,
                "Replicator Prefix and Suffix require static labels and increment booleans");
        if ((leftBypassParameter != nullptr
                && !leftBypassParameter->boolean.has_value())
            || (rightBypassParameter != nullptr
                && !rightBypassParameter->boolean.has_value()))
            return fail(wrapperId,
                "Stereoizer bypass controls must be static booleans");
        const bool leftBypass = leftBypassParameter != nullptr
            && *leftBypassParameter->boolean;
        const bool rightBypass = rightBypassParameter != nullptr
            && *rightBypassParameter->boolean;
        std::array<std::string, 6> surroundLevels{};
        std::array<bool, 7> surroundBypass{};
        if (surroundifer)
        {
            constexpr std::array<std::string_view, 6> levelNames{
                "levelL", "levelR", "levelLS", "levelRS",
                "levelC", "levelLFE"};
            constexpr std::array<std::string_view, 6> bypassNames{
                "bypassL", "bypassR", "bypassLS", "bypassRS",
                "bypassC", "bypassLFE"};
            for (std::size_t channel = 0; channel < levelNames.size(); ++channel)
            {
                const auto* field = parameter(levelNames[channel]);
                if (field != nullptr && !field->expression.has_value())
                    return fail(wrapperId,
                        "Surroundifer levels require scalar expressions");
                surroundLevels[channel] = field == nullptr
                    ? "1.0" : *field->expression;
                const auto* bypass = parameter(bypassNames[channel]);
                if (bypass != nullptr && !bypass->boolean.has_value())
                    return fail(wrapperId,
                        "Surroundifer bypass controls must be static booleans");
                surroundBypass[channel + 1] = bypass != nullptr
                    && *bypass->boolean;
            }
        }
        const bool anyBypass = leftBypass || rightBypass
            || std::ranges::any_of(surroundBypass,
                [](bool bypass) { return bypass; });
        const bool renameSelected = modeParameter != nullptr
            && *modeParameter->boolean;
        const auto prefixLabels = parseReplicatorLabels(
            prefixParameter != nullptr ? *prefixParameter->text : "");
        const auto suffixLabels = parseReplicatorLabels(
            suffixParameter != nullptr ? *suffixParameter->text
                : surroundifer ? "'L' 'R' 'LS' 'RS' 'C' 'LFE'"
                : stereoizer ? "'L' 'R'" : "01");
        if (!prefixLabels.has_value() || !suffixLabels.has_value())
            return fail(wrapperId,
                "Replicator Prefix or Suffix requires bounded ASCII labels");
        const bool incrementPrefix = incrementPrefixParameter == nullptr
            ? !spatializer : *incrementPrefixParameter->boolean;
        const bool incrementSuffix = incrementSuffixParameter == nullptr
            ? !spatializer : *incrementSuffixParameter->boolean;
        const auto validName = [](std::string_view name) {
            if (name.empty()
                || name.size()
                    >= talas::expr::ExpressionLimits::maximumSourceBytes)
                return false;
            const auto letter = [](char value) {
                return (value >= 'a' && value <= 'z')
                    || (value >= 'A' && value <= 'Z')
                    || value == '_';
            };
            return letter(name.front())
                && std::ranges::all_of(name.substr(1), [&](char value) {
                    return letter(value)
                        || (value >= '0' && value <= '9');
                });
        };
        std::unordered_set<std::string> special;
        if (specialParameter != nullptr
            && !specialParameter->text.value_or("").empty())
        {
            const auto members = talas::core::parseBoundedStaticResourceList(
                *specialParameter->text);
            if (!members.has_value())
                return fail(wrapperId,
                    "Replicator SpecialEvents requires a bounded static !Name list");
            for (const auto& member : *members)
            {
                if (!member.starts_with('!')
                    || !validName(std::string_view{member}.substr(1))
                    || !special.insert(member.substr(1)).second)
                    return fail(wrapperId,
                        "Replicator SpecialEvents contains an invalid or duplicate !Name");
            }
        }
        const auto sourceId = input->fromNode;
        const auto sourcePort = input->fromPort;
        std::unordered_set<std::string> upstream;
        std::vector<std::string> pending{sourceId};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            const auto id = pending[index];
            if (!upstream.insert(id).second)
                continue;
            if (nodeFor(id) == graph.nodes.end())
                return fail(wrapperId, "Replicator lost an upstream Sound");
            for (const auto& edge : graph.connections)
                if (edge.toNode == id)
                    pending.push_back(edge.fromNode);
        }
        std::unordered_set<std::string> shared;
        std::string bypassSourceId;
        std::string bypassLeftPort;
        if (spatializer)
        {
            const auto* sharedParameter = parameter("sharedSounds");
            if (sharedParameter != nullptr
                && !sharedParameter->text.has_value())
                return fail(wrapperId,
                    "Stereoizer SharedSounds requires static node identifiers");
            std::vector<std::string> sharedRoots;
            if (sharedParameter != nullptr
                && !sharedParameter->text->empty())
            {
                const auto listed = talas::core::parseBoundedStaticResourceList(
                    *sharedParameter->text);
                if (!listed.has_value())
                    return fail(wrapperId,
                        "Stereoizer SharedSounds requires a bounded static node list");
                sharedRoots = *listed;
            }
            if (hasRightInput)
                sharedRoots.push_back(rightSourceId);
            for (const auto& root : sharedRoots)
            {
                if (!upstream.contains(root))
                    return fail(wrapperId,
                        "Stereoizer SharedSounds must name an upstream Sound");
                std::vector<std::string> ancestors{root};
                for (std::size_t index = 0; index < ancestors.size(); ++index)
                {
                    const auto id = ancestors[index];
                    if (!shared.insert(id).second)
                        continue;
                    for (const auto& edge : graph.connections)
                        if (edge.toNode == id)
                            ancestors.push_back(edge.fromNode);
                }
            }
            if (shared.contains(sourceId))
                return fail(wrapperId,
                    "Stereoizer needs processing after its shared source");
            for (const auto& identity : result.manifest.nodes)
                if (shared.contains(identity.flatNodeId)
                    && identity.scopeOrdinal != 0)
                    return fail(wrapperId,
                        "Stereoizer shared reusable Sound scopes are not yet supported");
            for (const auto& id : shared)
                upstream.erase(id);
            if (hasRightInput || leftBypass || rightBypass)
            {
                std::size_t boundaryCount = 0;
                for (const auto& edge : graph.connections)
                    if (shared.contains(edge.fromNode)
                        && upstream.contains(edge.toNode))
                    {
                        if (hasRightInput && edge.fromNode != rightSourceId)
                            return fail(wrapperId,
                                "Stereoizer right input must match its shared source boundary");
                        bypassSourceId = edge.fromNode;
                        bypassLeftPort = edge.fromPort;
                        ++boundaryCount;
                    }
                if (boundaryCount != 1)
                    return fail(wrapperId,
                        "Stereoizer bypass or right input requires one shared source boundary");
            }
        }
        for (const auto& edge : graph.connections)
            if (upstream.contains(edge.fromNode)
                && !upstream.contains(edge.toNode)
                && edge.toNode != wrapperId)
                return fail(wrapperId,
                    "Replicator upstream Sound is routed outside its copy scope");
        const auto wrapperIdentity = std::ranges::find(
            result.manifest.nodes, wrapperId,
            &ExpandedSoundNodeIdentity::flatNodeId);
        if (wrapperIdentity == result.manifest.nodes.end())
            return fail(wrapperId,
                "Replicator lost its authored Sound identity");
        const auto wrapperScopeOrdinal = wrapperIdentity->scopeOrdinal;
        const auto wrapperPathDepth = wrapperIdentity->instancePath.size();
        if (wrapperScopeOrdinal != 0)
        {
            for (const auto& identity : result.manifest.nodes)
            {
                if (!upstream.contains(identity.flatNodeId))
                    continue;
                auto ordinal = identity.scopeOrdinal;
                while (ordinal != 0 && ordinal != wrapperScopeOrdinal)
                {
                    const auto scope = std::ranges::find(
                        result.manifest.scopes, ordinal,
                        &ExpandedSoundScopeIdentity::scopeOrdinal);
                    if (scope == result.manifest.scopes.end())
                        break;
                    ordinal = scope->parentScopeOrdinal;
                }
                if (ordinal != wrapperScopeOrdinal)
                    return fail(wrapperId,
                        "Replicator inside a reusable Sound requires an upstream tree in its own or child scopes");
            }
        }
        std::unordered_set<std::uint32_t> upstreamScopeOrdinals;
        for (const auto& identity : result.manifest.nodes)
        {
            if (!upstream.contains(identity.flatNodeId))
                continue;
            auto ordinal = identity.scopeOrdinal;
            while (ordinal != 0
                && ordinal != wrapperScopeOrdinal
                && upstreamScopeOrdinals.insert(ordinal).second)
            {
                const auto scope = std::ranges::find(
                    result.manifest.scopes, ordinal,
                    &ExpandedSoundScopeIdentity::scopeOrdinal);
                if (scope == result.manifest.scopes.end())
                    return fail(wrapperId,
                        "Replicator lost a reusable Sound scope");
                ordinal = scope->parentScopeOrdinal;
            }
        }
        if (anyBypass
            && !upstreamScopeOrdinals.empty())
            return fail(wrapperId,
                "Spatializer bypass of reusable Sound scopes is not yet supported");
        for (const auto& identity : result.manifest.nodes)
            if (upstreamScopeOrdinals.contains(identity.scopeOrdinal)
                && !upstream.contains(identity.flatNodeId))
                return fail(wrapperId,
                    "Replicator reusable Sound scope is shared outside its copy tree");
        std::vector<std::size_t> sourceEventBindingIndices;
        std::vector<ExpandedSoundEventValueBinding> sourceEventBindings;
        std::unordered_set<std::string> localEventNames;
        std::unordered_set<std::uint32_t> enclosingScopeOrdinals;
        for (auto ordinal = wrapperScopeOrdinal; ordinal != 0;)
        {
            if (!enclosingScopeOrdinals.insert(ordinal).second)
                return fail(wrapperId,
                    "Replicator has a cyclic enclosing Sound scope");
            const auto scope = std::ranges::find(
                result.manifest.scopes, ordinal,
                &ExpandedSoundScopeIdentity::scopeOrdinal);
            if (scope == result.manifest.scopes.end())
                return fail(wrapperId,
                    "Replicator lost its enclosing Sound scope");
            ordinal = scope->parentScopeOrdinal;
        }
        for (std::size_t index = 0;
             index < result.manifest.eventValues.size(); ++index)
        {
            const auto& binding = result.manifest.eventValues[index];
            if (upstreamScopeOrdinals.contains(binding.scopeOrdinal))
            {
                sourceEventBindingIndices.push_back(index);
                sourceEventBindings.push_back(binding);
                localEventNames.insert(binding.eventName);
            }
            else if (enclosingScopeOrdinals.contains(
                         binding.scopeOrdinal))
                localEventNames.insert(binding.eventName);
            else if (upstreamScopeOrdinals.contains(
                         binding.sourceScopeOrdinal))
                return fail(wrapperId,
                    "Replicator local Sound binding is shared outside its copy scope");
        }
        std::vector<ExpandedSoundNodeParameterBinding>
            sourceNodeParameterBindings;
        for (const auto& binding : result.manifest.nodeParameters)
            if (upstream.contains(binding.flatNodeId))
                sourceNodeParameterBindings.push_back(binding);
        for (const auto& group : result.manifest.splitSurroundResourceGroups)
            if (upstream.contains(group.flatNodeId))
                return fail(wrapperId,
                    "Replicator copied resource groups are not yet supported");
        for (const auto& selector : result.manifest.soundVariableSelectors)
            if (upstream.contains(selector.flatNodeId))
                return fail(wrapperId,
                    "Replicator copied Sound variable selectors are not yet supported");
        std::vector<ExpandedSoundScopeIdentity> sourceScopes;
        for (const auto& scope : result.manifest.scopes)
            if (upstreamScopeOrdinals.contains(scope.scopeOrdinal))
                sourceScopes.push_back(scope);
        std::ranges::sort(sourceScopes, {},
            [](const auto& scope) {
                return scope.instancePath.size();
            });
        auto nextScopeOrdinal = std::uint32_t{0};
        for (const auto& scope : result.manifest.scopes)
            nextScopeOrdinal = std::max(
                nextScopeOrdinal, scope.scopeOrdinal);
        if ((copies > 1 && sourceScopes.size()
                > (std::numeric_limits<std::uint32_t>::max()
                    - nextScopeOrdinal) / (copies - 1))
            || result.manifest.scopes.size()
                    + (copies - 1) * sourceScopes.size()
                > maximumProjectSoundExpansionWorkItems
            || result.manifest.eventValues.size()
                    + (copies - 1) * sourceEventBindings.size()
                > maximumProjectSoundExpansionWorkItems
            || result.manifest.nodeParameters.size()
                    + (copies - 1)
                        * sourceNodeParameterBindings.size()
                > maximumProjectSoundExpansionWorkItems)
            return fail(wrapperId,
                "Replicator reusable Sound scope capacity exceeded",
                talas::core::ValidationCode::SoundExpansionLimitExceeded);

        const auto nearestLocalBindingScope = [&](
            std::uint32_t ordinal,
            std::string_view name) -> std::optional<std::uint32_t> {
            while (ordinal != 0)
            {
                if (std::ranges::any_of(
                        result.manifest.eventValues,
                        [&](const auto& binding) {
                            return binding.scopeOrdinal == ordinal
                                && binding.eventName == name;
                        }))
                    return ordinal;
                const auto scope = std::ranges::find(
                    result.manifest.scopes, ordinal,
                    &ExpandedSoundScopeIdentity::scopeOrdinal);
                if (scope == result.manifest.scopes.end())
                    break;
                ordinal = scope->parentScopeOrdinal;
            }
            return std::nullopt;
        };

        std::unordered_set<std::string> allNames;
        allNames.insert(
            localEventNames.begin(), localEventNames.end());
        for (const auto& node : graph.nodes)
        {
            if (node.instanceId == wrapperId)
                continue;
            for (const auto& parameterValue : node.parameters)
                for (const auto* field : {
                         parameterValue.expression
                             ? &*parameterValue.expression : nullptr,
                         parameterValue.text
                             ? &*parameterValue.text : nullptr})
                {
                    if (field == nullptr)
                        continue;
                    const auto names = talas::expr::scanEventReferenceNames(
                        *field, result.document.expressionLanguage);
                    if (!names.has_value())
                        return fail(wrapperId,
                            "Replicator encountered an oversized Event Value field");
                    allNames.insert(names->begin(), names->end());
                }
        }
        for (const auto& definition
             : result.document.expressionDefinitions.definitions)
        {
            const auto names = talas::expr::scanEventReferenceNames(
                definition.source, result.document.expressionLanguage);
            if (!names.has_value() || !names->empty())
                return fail(wrapperId,
                    "Replicator Event Values inside expression definitions are not yet supported");
        }

        for (const auto& binding : sourceEventBindings)
        {
            allNames.insert(binding.eventName);
            if (!binding.value.expression.has_value())
                continue;
            const auto directNames = talas::expr::scanEventReferenceNames(
                *binding.value.expression,
                result.document.expressionLanguage);
            const auto specialized = talas::expr::specializeReplicationVariables(
                *binding.value.expression, 1,
                static_cast<std::uint16_t>(copies));
            if (!directNames.has_value() || !specialized.has_value())
                return fail(wrapperId,
                    "Replicator local Sound binding exceeds its expression limit");
            const auto parsed = talas::expr::parse(
                *specialized,
                result.document.expressionLanguage,
                result.document.expressionDefinitions);
            if (!parsed.ok())
                return fail(wrapperId,
                    "Replicator local Sound binding cannot be specialized");
            for (const auto& name : parsed.expression->eventReferences())
                if (std::ranges::find(*directNames, name)
                    == directNames->end())
                    return fail(wrapperId,
                        "Replicator cannot rename an Event Value hidden in a definition");
            allNames.insert(directNames->begin(), directNames->end());
        }

        std::unordered_set<std::string> scopeNames;
        std::unordered_set<std::string> generatedIndexNames;
        for (const auto& binding : sourceEventBindings)
            if (binding.value.expression.has_value())
            {
                const auto names = talas::expr::scanEventReferenceNames(
                    *binding.value.expression,
                    result.document.expressionLanguage);
                for (const auto& name : *names)
                {
                    if (localEventNames.contains(name))
                    {
                        const auto localScope = nearestLocalBindingScope(
                            binding.sourceScopeOrdinal, name);
                        if (!localScope.has_value())
                            return fail(wrapperId,
                                "Replicator local and global Event Value names overlap");
                    }
                    else
                        scopeNames.insert(name);
                }
            }
        std::vector<ProjectNode> sourceNodes;
        for (const auto& node : graph.nodes)
            if (upstream.contains(node.instanceId))
            {
                if (node.typeId == "talas.audioInput")
                    return fail(wrapperId,
                        "Replicator open Audio inputs are not yet supported");
                for (const auto& parameter : node.parameters)
                {
                    if (parameter.expression.has_value())
                    {
                        const auto directNames =
                            talas::expr::scanEventReferenceNames(
                                *parameter.expression,
                                result.document.expressionLanguage);
                        if (!directNames.has_value())
                            return fail(wrapperId,
                                "Replicator encountered an oversized Event Value expression");
                        const auto identity = std::ranges::find(
                            result.manifest.nodes, node.instanceId,
                            &ExpandedSoundNodeIdentity::flatNodeId);
                        for (const auto& name : *directNames)
                        {
                            if (localEventNames.contains(name))
                            {
                                const auto localScope = identity
                                        == result.manifest.nodes.end()
                                    ? std::optional<std::uint32_t>{}
                                    : nearestLocalBindingScope(
                                        identity->scopeOrdinal, name);
                                if (!localScope.has_value())
                                    return fail(wrapperId,
                                        "Replicator local and global Event Value names overlap");
                            }
                            else
                                scopeNames.insert(name);
                        }
                        const auto specialized =
                            talas::expr::specializeReplicationVariables(
                                *parameter.expression, 1,
                                static_cast<std::uint16_t>(copies));
                        if (!specialized.has_value())
                            return fail(wrapperId,
                                "Replicator expression exceeds its size limit");
                        const auto parsed = talas::expr::parse(
                            *specialized,
                            result.document.expressionLanguage,
                            result.document.expressionDefinitions);
                        if (!parsed.ok())
                            return fail(wrapperId,
                                "Replicator upstream expression cannot be specialized");
                        for (const auto& name
                             : parsed.expression->eventReferences())
                            if (std::ranges::find(*directNames, name)
                                == directNames->end())
                                return fail(wrapperId,
                                    "Replicator cannot rename an Event Value hidden in a definition");
                    }
                    if (parameter.text.has_value())
                    {
                        const auto names = talas::expr::scanEventReferenceNames(
                            *parameter.text,
                            result.document.expressionLanguage);
                        if (!names.has_value())
                            return fail(wrapperId,
                                "Replicator encountered an oversized Event Value text field");
                        const bool terminalEventText =
                            (node.typeId == "talas.soundToGlobalController"
                                || node.typeId
                                    == "talas.triggeredSoundToGlobalController")
                            && (parameter.parameterId == "generatedEvent"
                                || parameter.parameterId == "value");
                        const bool generatedIndexText =
                            node.typeId == "talas.analogSequencer"
                            && parameter.parameterId == "generatedIndex";
                        const bool analogArrayText =
                            node.typeId == "talas.analogSequencer"
                            && isAnalogSequencerEventText(parameter.parameterId);
                        if (generatedIndexText)
                        {
                            const auto target =
                                talas::core::parseAnalogSequencerGeneratedIndex(
                                    *parameter.text);
                            if (!target.has_value())
                                return fail(wrapperId,
                                    "Replicator encountered an invalid GeneratedIndex target");
                            if (!target->empty())
                                generatedIndexNames.insert(*target);
                        }
                        if (!names->empty() && !terminalEventText
                            && !analogArrayText)
                            return fail(wrapperId,
                                "Replicator Event Values in other text fields are not yet supported");
                        for (const auto& name : *names)
                        {
                            if (localEventNames.contains(name))
                                return fail(wrapperId,
                                    "Replicator local Event Value in terminal text is not supported");
                            scopeNames.insert(name);
                        }
                    }
                }
                sourceNodes.push_back(node);
            }
        std::vector<std::string> selectedNames;
        for (const auto& name : scopeNames)
            if (special.contains(name) == renameSelected)
                selectedNames.push_back(name);
        std::ranges::sort(selectedNames);
        std::vector<std::vector<std::pair<std::string, std::string>>>
            renamedByVoice(copies + 1);
        std::unordered_map<std::string, std::string> generatedNames;
        for (std::size_t voice = 1; voice <= copies; ++voice)
        {
            const auto prefix = replicatorLabelForVoice(
                *prefixLabels, voice, incrementPrefix);
            const auto suffix = replicatorLabelForVoice(
                *suffixLabels, voice, incrementSuffix);
            if (!prefix.has_value() || !suffix.has_value())
                return fail(wrapperId,
                    "Replicator Prefix or Suffix number cannot be incremented");
            for (const auto& name : selectedNames)
            {
                auto generated = *prefix + name + *suffix;
                const auto prior = generatedNames.find(generated);
                if (!validName(generated)
                    || (generatedIndexNames.contains(name)
                        && generated.size() > 128)
                    || (generated != name && allNames.contains(generated))
                    || (prior != generatedNames.end()
                        && prior->second != name))
                    return fail(wrapperId,
                        "Replicator generated an invalid or colliding Event Value name");
                generatedNames.emplace(generated, name);
                renamedByVoice[voice].emplace_back(
                    name, std::move(generated));
            }
        }
        std::vector<talas::core::Connection> sourceEdges;
        for (const auto& edge : graph.connections)
            if ((upstream.contains(edge.fromNode)
                    || shared.contains(edge.fromNode))
                && upstream.contains(edge.toNode))
                sourceEdges.push_back(edge);
        const auto addedNodes = (copies - 1) * (sourceNodes.size() + 1)
            + (surroundifer ? 9 : 0);
        const auto addedEdges = (copies - 1) * (sourceEdges.size() + 2)
            + (surroundifer ? 12 : 0);
        if (graph.nodes.size() + addedNodes
                > maximumProjectSoundExpansionWorkItems
            || graph.connections.size() + addedEdges
                > maxExpandedConnectionCount)
            return fail(wrapperId,
                "Replicator exceeds the bounded graph expansion capacity",
                talas::core::ValidationCode::SoundExpansionLimitExceeded);

        const auto specialize = [&](ProjectNode& node, std::size_t voice) {
            std::vector<talas::expr::EventReferenceRename> renames;
            renames.reserve(renamedByVoice[voice].size());
            for (const auto& [from, to] : renamedByVoice[voice])
                renames.push_back({from, to});
            for (auto& parameter : node.parameters)
            {
                if (parameter.expression.has_value())
                {
                    auto bound = talas::expr::specializeReplicationVariables(
                        *parameter.expression,
                        static_cast<std::uint16_t>(voice),
                        static_cast<std::uint16_t>(copies));
                    if (!bound.has_value())
                        return false;
                    auto rewritten = talas::expr::renameEventReferences(
                        *bound, result.document.expressionLanguage,
                        renames);
                    if (!rewritten.has_value())
                        return false;
                    parameter.expression = std::move(*rewritten);
                }
                const bool terminalEventText =
                    (node.typeId == "talas.soundToGlobalController"
                        || node.typeId
                            == "talas.triggeredSoundToGlobalController")
                    && (parameter.parameterId == "generatedEvent"
                        || parameter.parameterId == "value");
                const bool analogEventText =
                    node.typeId == "talas.analogSequencer"
                    && isAnalogSequencerEventText(parameter.parameterId);
                if ((terminalEventText || analogEventText)
                    && parameter.text.has_value())
                {
                    auto rewritten = talas::expr::renameEventReferences(
                        *parameter.text,
                        result.document.expressionLanguage,
                        renames);
                    if (!rewritten.has_value())
                        return false;
                    parameter.text = std::move(*rewritten);
                }
            }
            return true;
        };
        const auto specializeEventBinding = [&](
            ExpandedSoundEventValueBinding& binding,
            std::size_t voice) {
            if (!binding.value.expression.has_value())
                return true;
            std::vector<talas::expr::EventReferenceRename> renames;
            renames.reserve(renamedByVoice[voice].size());
            for (const auto& [from, to] : renamedByVoice[voice])
                renames.push_back({from, to});
            auto bound = talas::expr::specializeReplicationVariables(
                *binding.value.expression,
                static_cast<std::uint16_t>(voice),
                static_cast<std::uint16_t>(copies));
            if (!bound.has_value())
                return false;
            auto rewritten = talas::expr::renameEventReferences(
                *bound, result.document.expressionLanguage, renames);
            if (!rewritten.has_value())
                return false;
            binding.value.expression = std::move(*rewritten);
            return true;
        };
        for (auto& node : graph.nodes)
            if (upstream.contains(node.instanceId)
                && !specialize(node, 1))
                return fail(wrapperId,
                    "Replicator source expression exceeds its size limit");
        for (const auto index : sourceEventBindingIndices)
            if (!specializeEventBinding(
                    result.manifest.eventValues[index], 1))
                return fail(wrapperId,
                    "Replicator source local binding exceeds its size limit");

        std::array<FlatEndpoint, 7> surroundInputs{};
        if (surroundifer)
        {
            surroundInputs[1] = {bypassSourceId, bypassLeftPort};
            surroundInputs[2] = {rightSourceId, rightSourcePort};
            // The six channels are named, but no numeric pseudo-surround
            // matrix is specified. These finite mid/side seeds are a bounded
            // local candidate.
            struct MatrixSeed
            {
                std::size_t voice;
                std::string_view leftGain;
                std::string_view rightGain;
            };
            constexpr std::array matrixSeeds{
                MatrixSeed{3, "0.5", "-0.5"},
                MatrixSeed{4, "-0.5", "0.5"},
                MatrixSeed{5, "0.5", "0.5"},
            };
            for (const auto& seed : matrixSeeds)
            {
                auto id = generatedId(
                    wrapperId, "surroundMix", seed.voice, true);
                if (!id.has_value())
                    return fail(wrapperId,
                        "Surroundifer generated an oversized or colliding mixer id");
                graph.nodes.push_back({
                    *id, "talas.mixer",
                    {{"gain1", std::string{seed.leftGain}},
                     {"gain2", std::string{seed.rightGain}}}});
                result.manifest.nodes.push_back({
                    *id, {}, {}, *id, 0, wrapperId, {}});
                graph.connections.push_back({
                    bypassSourceId, bypassLeftPort, *id, "in1"});
                graph.connections.push_back({
                    rightSourceId, rightSourcePort, *id, "in2"});
                surroundInputs[seed.voice] = {*id, "audioOut"};
            }
            surroundInputs[6] = surroundInputs[5];
        }

        std::string mixedSource = sourceId;
        std::string mixedPort = sourcePort;
        std::string rightSource;
        std::array<std::string, 7> surroundBranchSources{};
        std::array<std::unordered_set<std::string>, 7>
            surroundBranchNodeIds{};
        if (surroundifer)
        {
            surroundBranchSources[1] = sourceId;
            surroundBranchNodeIds[1].insert(
                upstream.begin(), upstream.end());
        }
        std::unordered_set<std::string> rightBranchIds;
        for (std::size_t voice = 2; voice <= copies; ++voice)
        {
            const auto replicaMarker = "#replica" + std::to_string(voice);
            std::unordered_map<std::uint32_t, std::uint32_t> clonedScopes;
            for (const auto& original : sourceScopes)
            {
                if (original.instancePath.empty())
                    return fail(wrapperId,
                        "Replicator reusable Sound scope has no instance path");
                auto clone = original;
                clone.scopeOrdinal = ++nextScopeOrdinal;
                if (clone.parentScopeOrdinal != 0
                    && clone.parentScopeOrdinal != wrapperScopeOrdinal)
                {
                    const auto parent = clonedScopes.find(
                        clone.parentScopeOrdinal);
                    if (parent == clonedScopes.end())
                        return fail(wrapperId,
                            "Replicator reusable Sound scope parent is missing");
                    clone.parentScopeOrdinal = parent->second;
                }
                if (clone.instancePath.size() <= wrapperPathDepth)
                    return fail(wrapperId,
                        "Replicator child Sound scope has no nested instance path");
                clone.instancePath[wrapperPathDepth] += replicaMarker;
                if (std::ranges::any_of(
                        result.manifest.scopes,
                        [&](const auto& existing) {
                            return existing.instancePath
                                == clone.instancePath;
                        }))
                    return fail(wrapperId,
                        "Replicator generated a colliding reusable Sound path");
                clonedScopes.emplace(original.scopeOrdinal,
                    clone.scopeOrdinal);
                result.manifest.scopes.push_back(std::move(clone));
            }
            std::unordered_map<std::string, std::string> clonedIds;
            for (const auto& sourceNode : sourceNodes)
            {
                auto id = generatedId(
                    wrapperId, sourceNode.instanceId, voice);
                if (!id.has_value())
                    return fail(wrapperId,
                        "Replicator generated an oversized or colliding node id");
                clonedIds.emplace(sourceNode.instanceId, *id);
                if (surroundifer)
                    surroundBranchNodeIds[voice].insert(*id);
                auto clone = sourceNode;
                clone.instanceId = *id;
                if (!specialize(clone, voice))
                    return fail(wrapperId,
                        "Replicator copy expression exceeds its size limit");
                graph.nodes.push_back(std::move(clone));
                const auto originalIdentity = std::ranges::find(
                    result.manifest.nodes, sourceNode.instanceId,
                    &ExpandedSoundNodeIdentity::flatNodeId);
                if (originalIdentity != result.manifest.nodes.end())
                {
                    auto identity = *originalIdentity;
                    identity.flatNodeId = *id;
                    identity.localNodeId += replicaMarker;
                    identity.sourcePath += replicaMarker;
                    if (identity.scopeOrdinal != 0)
                    {
                        if (const auto scope = clonedScopes.find(
                                identity.scopeOrdinal);
                            scope != clonedScopes.end())
                        {
                            identity.scopeOrdinal = scope->second;
                            identity.instancePath[wrapperPathDepth] += replicaMarker;
                        }
                    }
                    result.manifest.nodes.push_back(std::move(identity));
                }
            }
            for (const auto& original : sourceNodeParameterBindings)
            {
                auto clone = original;
                clone.flatNodeId = clonedIds.at(original.flatNodeId);
                if (const auto source = clonedScopes.find(
                        clone.sourceScopeOrdinal);
                    source != clonedScopes.end())
                    clone.sourceScopeOrdinal = source->second;
                result.manifest.nodeParameters.push_back(
                    std::move(clone));
            }
            for (const auto& original : sourceEventBindings)
            {
                auto clone = original;
                clone.scopeOrdinal = clonedScopes.at(
                    clone.scopeOrdinal);
                if (const auto source = clonedScopes.find(
                        clone.sourceScopeOrdinal);
                    source != clonedScopes.end())
                    clone.sourceScopeOrdinal = source->second;
                if (!specializeEventBinding(clone, voice))
                    return fail(wrapperId,
                        "Replicator copied local binding exceeds its size limit");
                const auto scope = std::ranges::find(
                    result.manifest.scopes, clone.scopeOrdinal,
                    &ExpandedSoundScopeIdentity::scopeOrdinal);
                clone.sourcePath = makeSourcePath(
                    scope->instancePath, scope->definitionId,
                    "!" + clone.eventName);
                result.manifest.eventValues.push_back(std::move(clone));
            }
            for (const auto& edge : sourceEdges)
                graph.connections.push_back({
                    shared.contains(edge.fromNode)
                        ? (surroundifer
                            ? surroundInputs[voice].nodeId
                            : hasRightInput ? rightSourceId : edge.fromNode)
                        : clonedIds.at(edge.fromNode),
                    shared.contains(edge.fromNode)
                        ? (surroundifer
                            ? surroundInputs[voice].portId
                            : hasRightInput ? rightSourcePort : edge.fromPort)
                        : edge.fromPort,
                    clonedIds.at(edge.toNode), edge.toPort});
            if (spatializer)
            {
                if (surroundifer)
                    surroundBranchSources[voice] = clonedIds.at(sourceId);
                else
                    rightSource = clonedIds.at(sourceId);
                if (stereoizer && rightBypass)
                    for (const auto& entry : clonedIds)
                        rightBranchIds.insert(entry.second);
                continue;
            }
            auto mixerId = generatedId(wrapperId, "mix", voice, true);
            if (!mixerId.has_value())
                return fail(wrapperId,
                    "Replicator generated an oversized or colliding mixer id");
            graph.nodes.push_back({*mixerId, "talas.mixer", {}});
            result.manifest.nodes.push_back({
                *mixerId, {}, {}, *mixerId, 0, wrapperId, {}});
            graph.connections.push_back({
                mixedSource, mixedPort, *mixerId, "in1"});
            graph.connections.push_back({
                clonedIds.at(sourceId), sourcePort, *mixerId, "in2"});
            mixedSource = *mixerId;
            mixedPort = "audioOut";
        }
        std::array<std::string, 7> surroundLevelIds{};
        if (surroundifer)
        {
            for (std::size_t voice = 1; voice <= 6; ++voice)
            {
                auto id = generatedId(wrapperId, "surroundLevel", voice, true);
                if (!id.has_value())
                    return fail(wrapperId,
                        "Surroundifer generated an oversized or colliding level id");
                graph.nodes.push_back({
                    *id, "talas.vca",
                    {{"gain", surroundLevels[voice - 1]}}});
                result.manifest.nodes.push_back({
                    *id, {}, {}, *id, 0, wrapperId, {}});
                graph.connections.push_back({
                    surroundBypass[voice]
                        ? surroundInputs[voice].nodeId
                        : surroundBranchSources[voice],
                    surroundBypass[voice]
                        ? surroundInputs[voice].portId : sourcePort,
                    *id, "audioIn"});
                surroundLevelIds[voice] = *id;
            }
        }
        for (auto& edge : graph.connections)
            if (edge.fromNode == wrapperId)
            {
                if (surroundifer)
                {
                    constexpr std::array<std::string_view, 6> outputs{
                        "leftOut", "rightOut", "leftSurroundOut",
                        "rightSurroundOut", "centerOut", "lfeOut"};
                    const auto port = std::ranges::find(
                        outputs, std::string_view{edge.fromPort});
                    if (port == outputs.end())
                        return fail(wrapperId,
                            "Surroundifer requires one of its six ordered outputs");
                    const auto voice = static_cast<std::size_t>(
                        std::distance(outputs.begin(), port)) + 1;
                    edge.fromNode = surroundLevelIds[voice];
                    edge.fromPort = "audioOut";
                }
                else if (stereoizer)
                {
                    if (edge.fromPort != "leftOut"
                        && edge.fromPort != "rightOut")
                        return fail(wrapperId,
                            "Stereoizer requires leftOut or rightOut routing");
                    const bool left = edge.fromPort == "leftOut";
                    const bool bypass = left ? leftBypass : rightBypass;
                    edge.fromNode = bypass
                        ? bypassSourceId
                        : left ? sourceId : rightSource;
                    edge.fromPort = bypass
                        ? (!left && hasRightInput
                            ? rightSourcePort : bypassLeftPort)
                        : sourcePort;
                }
                else
                {
                    edge.fromNode = mixedSource;
                    edge.fromPort = mixedPort;
                }
            }
        std::erase_if(graph.connections, [&](const auto& edge) {
            return edge.toNode == wrapperId;
        });
        std::erase_if(graph.nodes, [&](const auto& node) {
            return node.instanceId == wrapperId;
        });
        std::erase_if(result.manifest.nodes, [&](const auto& identity) {
            return identity.flatNodeId == wrapperId;
        });
        std::erase_if(result.manifest.nodeParameters, [&](const auto& binding) {
            return binding.flatNodeId == wrapperId;
        });
        if (anyBypass)
        {
            std::unordered_set<std::string> bypassedIds;
            if (leftBypass)
                bypassedIds.insert(upstream.begin(), upstream.end());
            if (rightBypass)
                bypassedIds.insert(rightBranchIds.begin(), rightBranchIds.end());
            for (std::size_t voice = 1; voice <= 6; ++voice)
                if (surroundBypass[voice])
                    bypassedIds.insert(
                        surroundBranchNodeIds[voice].begin(),
                        surroundBranchNodeIds[voice].end());
            std::erase_if(graph.connections, [&](const auto& edge) {
                return bypassedIds.contains(edge.fromNode)
                    || bypassedIds.contains(edge.toNode);
            });
            std::erase_if(graph.nodes, [&](const auto& node) {
                return bypassedIds.contains(node.instanceId);
            });
            std::erase_if(result.manifest.nodes, [&](const auto& identity) {
                return bypassedIds.contains(identity.flatNodeId);
            });
            std::erase_if(result.manifest.nodeParameters,
                [&](const auto& binding) {
                    return bypassedIds.contains(binding.flatNodeId);
                });
        }
    }
    return true;
}

[[nodiscard]] bool applyMapEventValueBindings(
    ProjectSoundExpansionResult& result)
{
    auto& graph = result.document.graph;
    std::vector<std::string> mapIds;
    for (const auto& node : graph.nodes)
    {
        if (node.typeId == "talas.mapEventValues")
            mapIds.push_back(node.instanceId);
    }
    const auto fail = [&](std::string_view nodeId, std::string message) {
        result.validation.errors.push_back({
            talas::core::ValidationCode::InvalidParameterTransformation,
            std::move(message), std::string{nodeId}});
        return false;
    };
    std::unordered_set<std::string> claimedBindings;
    for (const auto& mapId : mapIds)
    {
        const auto mapNode = std::ranges::find(
            graph.nodes, mapId, &ProjectNode::instanceId);
        if (mapNode == graph.nodes.end())
            return fail(mapId, "MapEventValues node disappeared during expansion");
        const auto parameter = [&](std::string_view id)
            -> const ProjectParameterExpression* {
            const auto found = std::ranges::find(
                mapNode->parameters, id,
                &ProjectParameterExpression::parameterId);
            return found == mapNode->parameters.end() ? nullptr : &*found;
        };
        const auto* nameParameter = parameter("greenVariable");
        const auto* kindParameter = parameter("valueKind");
        const auto* stringParameter = parameter("stringValue");
        const auto* collectionParameter = parameter("collectionValues");
        const auto name = nameParameter != nullptr
            ? nameParameter->text.value_or("") : std::string{};
        const auto kind = kindParameter != nullptr
            ? kindParameter->choice.value_or("string") : std::string{"string"};
        const auto stringValue = stringParameter != nullptr
            ? stringParameter->text.value_or("") : std::string{};
        const auto collectionText = collectionParameter != nullptr
            ? collectionParameter->text.value_or("") : std::string{};
        if (!validGreenVariableName(name))
            return fail(mapId, "MapEventValues requires one valid ?greenVariable name");
        if (kind != "string" && kind != "collection")
            return fail(mapId, "MapEventValues valueKind must be string or collection");
        if ((kind == "string" && (stringValue.empty()
                || stringValue.size()
                    > talas::core::maximumStaticResourceListTextBytes
                || !collectionText.empty()))
            || (kind == "collection" && (!stringValue.empty()
                || collectionText.empty())))
        {
            return fail(mapId, "MapEventValues requires exactly one typed String or Collection value");
        }
        const auto collection = kind == "collection"
            ? talas::core::parseBoundedStaticResourceList(collectionText)
            : std::optional<std::vector<std::string>>{};
        if (kind == "collection" && !collection.has_value())
            return fail(mapId, "MapEventValues Collection must be a bounded nonempty String list");

        const auto input = std::ranges::find_if(
            graph.connections,
            [&](const auto& edge) {
                return edge.toNode == mapId && edge.toPort == "audioIn";
            });
        if (input == graph.connections.end())
            return fail(mapId, "MapEventValues requires one upstream Sound");
        const auto sourceNodeId = input->fromNode;
        const auto sourcePortId = input->fromPort;
        std::unordered_set<std::string> upstream;
        std::vector<std::string> pending{sourceNodeId};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            if (!upstream.insert(pending[index]).second)
                continue;
            for (const auto& edge : graph.connections)
            {
                if (edge.toNode == pending[index])
                    pending.push_back(edge.fromNode);
            }
        }
        for (const auto& edge : graph.connections)
        {
            if (upstream.contains(edge.fromNode)
                && edge.toNode != mapId
                && !upstream.contains(edge.toNode))
            {
                return fail(mapId,
                    "MapEventValues upstream Sound is also routed outside this binding; shared-branch isolation is not yet supported");
            }
        }
        for (const auto& upstreamNodeId : upstream)
        {
            auto claim = upstreamNodeId;
            claim.push_back('\0');
            claim += name;
            if (!claimedBindings.insert(std::move(claim)).second)
            {
                return fail(mapId,
                    "MapEventValues repeated binding of one green variable in an overlapping Sound closure is not yet supported");
            }
        }
        std::string serializedCollection;
        if (collection.has_value())
        {
            for (const auto& member : *collection)
            {
                if (!serializedCollection.empty())
                    serializedCollection += ',';
                serializedCollection += member;
            }
        }
        for (auto& node : graph.nodes)
        {
            if (!upstream.contains(node.instanceId)
                || node.typeId == "talas.mapEventValues")
                continue;
            for (auto& value : node.parameters)
            {
                if (kind == "string" && value.resource == name)
                    value.resource = stringValue;
                else if (kind == "collection" && value.text == name)
                    value.text = serializedCollection;
            }
        }
        for (auto& edge : graph.connections)
        {
            if (edge.fromNode == mapId)
            {
                edge.fromNode = sourceNodeId;
                edge.fromPort = sourcePortId;
            }
        }
        std::erase_if(graph.connections, [&](const auto& edge) {
            return edge.toNode == mapId;
        });
        std::erase_if(graph.nodes, [&](const auto& node) {
            return node.instanceId == mapId;
        });
        std::erase_if(result.manifest.nodes, [&](const auto& node) {
            return node.flatNodeId == mapId;
        });
        std::erase_if(result.manifest.nodeParameters, [&](const auto& binding) {
            return binding.flatNodeId == mapId;
        });
    }
    return true;
}

[[nodiscard]] bool applySharedFileBindings(
    ProjectSoundExpansionResult& result)
{
    struct FileBinding
    {
        std::string variable;
        std::string file;
        std::size_t replacementCount = 0;
    };

    auto& graph = result.document.graph;
    std::vector<std::string> wrapperIds;
    for (const auto& node : graph.nodes)
    {
        if (node.typeId == "talas.sharedFileName"
            || node.typeId == "talas.sharedFileNames")
        {
            wrapperIds.push_back(node.instanceId);
        }
    }
    const auto fail = [&](std::string_view nodeId, std::string message) {
        result.validation.errors.push_back({
            talas::core::ValidationCode::InvalidParameterTransformation,
            std::move(message), std::string{nodeId}});
        return false;
    };
    // Inner wrappers must bind before outer wrappers consume their audio output.
    const auto upstreamSize = [&](const std::string& id) {
        std::unordered_set<std::string> seen;
        std::vector<std::string> pending{id};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            if (!seen.insert(pending[index]).second)
                continue;
            for (const auto& edge : graph.connections)
            {
                if (edge.toNode == pending[index])
                    pending.push_back(edge.fromNode);
            }
        }
        return seen.size();
    };
    std::ranges::stable_sort(wrapperIds, [&](const auto& left, const auto& right) {
        return upstreamSize(left) < upstreamSize(right);
    });

    std::unordered_set<std::string> claimedBindings;
    for (const auto& wrapperId : wrapperIds)
    {
        const auto wrapper = std::ranges::find(
            graph.nodes, wrapperId, &ProjectNode::instanceId);
        if (wrapper == graph.nodes.end())
            return fail(wrapperId, "SharedFile wrapper disappeared during expansion");
        const auto multiple = wrapper->typeId == "talas.sharedFileNames";
        const auto parameter = [&](std::string_view id)
            -> const ProjectParameterExpression* {
            const auto found = std::ranges::find(
                wrapper->parameters, id,
                &ProjectParameterExpression::parameterId);
            return found == wrapper->parameters.end() ? nullptr : &*found;
        };
        std::vector<FileBinding> bindings;
        if (multiple)
        {
            const auto* names = parameter("fileVariables");
            const auto* files = parameter("selectedFiles");
            const auto parsedNames = names != nullptr && names->text.has_value()
                ? talas::core::parseBoundedStaticResourceList(*names->text)
                : std::nullopt;
            const auto parsedFiles = files != nullptr && files->text.has_value()
                ? talas::core::parseBoundedStaticResourceList(*files->text)
                : std::nullopt;
            if (!parsedNames.has_value() || !parsedFiles.has_value()
                || parsedNames->size() != parsedFiles->size())
            {
                return fail(wrapperId,
                    "SharedFileNames requires equal nonempty bounded lists of file variables and selected files");
            }
            for (std::size_t index = 0; index < parsedNames->size(); ++index)
                bindings.push_back({(*parsedNames)[index], (*parsedFiles)[index]});
        }
        else
        {
            const auto* name = parameter("fileVariable");
            const auto* file = parameter("selectedFile");
            bindings.push_back({
                name != nullptr ? name->text.value_or("") : std::string{},
                file != nullptr ? file->resource.value_or("") : std::string{},
            });
        }
        std::unordered_set<std::string> uniqueNames;
        for (const auto& binding : bindings)
        {
            if (!validGreenVariableName(binding.variable)
                || !uniqueNames.insert(binding.variable).second
                || binding.file.empty()
                || binding.file.front() == '?'
                || binding.file.size()
                    > talas::core::maximumStaticResourceListTextBytes)
            {
                return fail(wrapperId,
                    "SharedFile binding requires distinct valid ?file variables and static selected files");
            }
        }

        const auto input = std::ranges::find_if(
            graph.connections,
            [&](const auto& edge) {
                return edge.toNode == wrapperId && edge.toPort == "audioIn";
            });
        if (input == graph.connections.end())
            return fail(wrapperId, "SharedFile wrapper requires one upstream Sound");
        const auto sourceNodeId = input->fromNode;
        const auto sourcePortId = input->fromPort;
        std::unordered_set<std::string> upstream;
        std::vector<std::string> pending{sourceNodeId};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            if (!upstream.insert(pending[index]).second)
                continue;
            for (const auto& edge : graph.connections)
            {
                if (edge.toNode == pending[index])
                    pending.push_back(edge.fromNode);
            }
        }
        for (const auto& edge : graph.connections)
        {
            if (upstream.contains(edge.fromNode)
                && edge.toNode != wrapperId
                && !upstream.contains(edge.toNode))
            {
                return fail(wrapperId,
                    "SharedFile upstream Sound is also routed outside this binding; shared-branch isolation is not yet supported");
            }
        }
        for (const auto& upstreamNodeId : upstream)
        {
            for (const auto& binding : bindings)
            {
                auto claim = upstreamNodeId;
                claim.push_back('\0');
                claim += binding.variable;
                if (!claimedBindings.insert(std::move(claim)).second)
                {
                    return fail(wrapperId,
                        "SharedFile repeated binding of one file variable in overlapping Sound closures is not yet supported");
                }
            }
        }
        for (auto& node : graph.nodes)
        {
            if (!upstream.contains(node.instanceId)
                || node.typeId == "talas.sharedFileName"
                || node.typeId == "talas.sharedFileNames")
                continue;
            for (auto& value : node.parameters)
            {
                for (auto& binding : bindings)
                {
                    if (value.resource == binding.variable)
                    {
                        value.resource = binding.file;
                        ++binding.replacementCount;
                    }
                }
                if ((value.parameterId == "fileNames"
                        || value.parameterId == "samples")
                    && value.text.has_value())
                {
                    auto members = talas::core::parseBoundedStaticResourceList(
                        *value.text);
                    if (!members.has_value())
                        continue;
                    bool changed = false;
                    for (auto& member : *members)
                    {
                        for (auto& binding : bindings)
                        {
                            if (member == binding.variable)
                            {
                                if (binding.file.find_first_of(
                                        ", \t\r\n\f\v")
                                    != std::string::npos)
                                {
                                    return fail(wrapperId,
                                        "SharedFile static file-list selections cannot contain separators");
                                }
                                member = binding.file;
                                ++binding.replacementCount;
                                changed = true;
                            }
                        }
                    }
                    if (changed)
                    {
                        std::string serialized;
                        for (const auto& member : *members)
                        {
                            if (!serialized.empty())
                                serialized.push_back(',');
                            serialized += member;
                        }
                        if (serialized.size()
                            > talas::core::maximumStaticResourceListTextBytes)
                        {
                            return fail(wrapperId,
                                "SharedFile replacement exceeds the static file-list size limit");
                        }
                        value.text = std::move(serialized);
                    }
                }
            }
        }
        if (std::ranges::any_of(bindings, [](const auto& binding) {
                return binding.replacementCount == 0;
            }))
        {
            return fail(wrapperId,
                "SharedFile wrapper has a file variable with no occurrence in its upstream Sound");
        }
        for (auto& edge : graph.connections)
        {
            if (edge.fromNode == wrapperId)
            {
                edge.fromNode = sourceNodeId;
                edge.fromPort = sourcePortId;
            }
        }
        std::erase_if(graph.connections, [&](const auto& edge) {
            return edge.toNode == wrapperId;
        });
        std::erase_if(graph.nodes, [&](const auto& node) {
            return node.instanceId == wrapperId;
        });
        std::erase_if(result.manifest.nodes, [&](const auto& node) {
            return node.flatNodeId == wrapperId;
        });
        std::erase_if(result.manifest.nodeParameters, [&](const auto& binding) {
            return binding.flatNodeId == wrapperId;
        });
    }
    return true;
}

[[nodiscard]] bool applyMemoryWriterNameScopes(
    ProjectSoundExpansionResult& result)
{
    auto& graph = result.document.graph;
    std::vector<std::string> wrapperIds;
    std::vector<std::string> uniqueIds;
    for (const auto& node : graph.nodes)
    {
        if (node.typeId == "talas.uniqueMemoryWriterRecordingNames")
        {
            wrapperIds.push_back(node.instanceId);
            uniqueIds.push_back(node.instanceId);
        }
        else if (node.typeId == "talas.cancelUniqueMemoryWriterRecordingNames")
        {
            wrapperIds.push_back(node.instanceId);
        }
    }
    if (wrapperIds.empty())
        return true;
    const auto fail = [&](std::string_view nodeId, std::string message) {
        result.validation.errors.push_back({
            talas::core::ValidationCode::InvalidParameterTransformation,
            std::move(message), std::string{nodeId}});
        return false;
    };
    const auto nodeFor = [&](std::string_view id) {
        return std::ranges::find(
            graph.nodes, id, &ProjectNode::instanceId);
    };
    const auto inputFor = [&](std::string_view id) {
        return std::ranges::find_if(
            graph.connections, [&](const auto& edge) {
                return edge.toNode == id && edge.toPort == "audioIn";
            });
    };
    std::unordered_set<std::string> occupiedNames;
    for (const auto& node : graph.nodes)
        for (const auto& parameter : node.parameters)
            if (parameter.resource.has_value())
                occupiedNames.insert(*parameter.resource);

    for (const auto& wrapperId : uniqueIds)
    {
        const auto input = inputFor(wrapperId);
        if (input == graph.connections.end()
            || std::ranges::count_if(
                   graph.connections, [&](const auto& edge) {
                       return edge.toNode == wrapperId;
                   }) != 1)
        {
            return fail(wrapperId,
                "UniqueMemoryWriterRecordingNames requires one upstream mono Sound");
        }
        std::unordered_set<std::string> scope;
        std::vector<std::string> pending{input->fromNode};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            const auto id = pending[index];
            if (!scope.insert(id).second)
                continue;
            const auto node = nodeFor(id);
            if (node == graph.nodes.end())
                return fail(wrapperId, "MemoryWriter scope lost an upstream Sound");
            if (node->typeId == "talas.cancelUniqueMemoryWriterRecordingNames"
                || node->typeId == "talas.uniqueMemoryWriterRecordingNames")
                continue;
            for (const auto& edge : graph.connections)
                if (edge.toNode == id)
                    pending.push_back(edge.fromNode);
        }
        for (const auto& id : scope)
        {
            const auto node = nodeFor(id);
            if (node->typeId == "talas.cancelUniqueMemoryWriterRecordingNames"
                || node->typeId == "talas.uniqueMemoryWriterRecordingNames")
                continue;
            for (const auto& edge : graph.connections)
                if (edge.fromNode == id && edge.toNode != wrapperId
                    && !scope.contains(edge.toNode))
                    return fail(wrapperId,
                        "UniqueMemoryWriterRecordingNames upstream Sound is also routed outside its scope");
        }
        std::unordered_map<std::string, std::string> renamed;
        for (auto& node : graph.nodes)
        {
            if (!scope.contains(node.instanceId)
                || node.typeId != "talas.memoryWriter")
                continue;
            const auto name = std::ranges::find(
                node.parameters, std::string_view{"recordingName"},
                &ProjectParameterExpression::parameterId);
            if (name == node.parameters.end()
                || !name->resource.has_value()
                || name->resource->empty())
                return fail(wrapperId,
                    "UniqueMemoryWriterRecordingNames requires static nonempty recording names");
            const auto original = *name->resource;
            if (!renamed.contains(original))
            {
                auto generated = "talas.umwrn."
                    + std::to_string(wrapperId.size()) + "."
                    + wrapperId + "." + original;
                if (occupiedNames.contains(generated))
                    return fail(wrapperId,
                        "UniqueMemoryWriterRecordingNames generated a colliding recording name");
                occupiedNames.insert(generated);
                renamed.emplace(original, std::move(generated));
            }
            name->resource = renamed.at(original);
        }
        const auto readerResourceId = [](std::string_view typeId)
            -> std::string_view {
            if (typeId == "talas.sample" || typeId == "talas.sampleCloud"
                || typeId == "talas.timeFrequencyScale")
                return "sample";
            if (typeId == "talas.functionGenerator"
                || typeId == "talas.triggeredTableRead")
                return "wavetable";
            if (typeId == "talas.waveshaper")
                return "shapingFunction";
            return {};
        };
        for (auto& node : graph.nodes)
        {
            if (!scope.contains(node.instanceId))
                continue;
            const auto resourceId = readerResourceId(node.typeId);
            if (resourceId.empty())
                continue;
            const auto fromWriter = std::ranges::find(
                node.parameters, std::string_view{"fromMemoryWriter"},
                &ProjectParameterExpression::parameterId);
            if (fromWriter == node.parameters.end()
                || fromWriter->boolean != true)
                continue;
            const auto resource = std::ranges::find(
                node.parameters, resourceId,
                &ProjectParameterExpression::parameterId);
            if (resource == node.parameters.end()
                || !resource->resource.has_value()
                || !renamed.contains(*resource->resource))
                return fail(wrapperId,
                    "UniqueMemoryWriterRecordingNames requires each enclosed reader to match an enclosed writer");
            resource->resource = renamed.at(*resource->resource);
        }
    }

    const auto upstreamSize = [&](const std::string& id) {
        std::unordered_set<std::string> seen;
        std::vector<std::string> pending{id};
        for (std::size_t index = 0; index < pending.size(); ++index)
        {
            if (!seen.insert(pending[index]).second)
                continue;
            for (const auto& edge : graph.connections)
                if (edge.toNode == pending[index])
                    pending.push_back(edge.fromNode);
        }
        return seen.size();
    };
    std::ranges::stable_sort(wrapperIds,
        [&](const auto& left, const auto& right) {
            return upstreamSize(left) < upstreamSize(right);
        });
    for (const auto& wrapperId : wrapperIds)
    {
        const auto input = inputFor(wrapperId);
        if (input == graph.connections.end()
            || std::ranges::count_if(
                   graph.connections, [&](const auto& edge) {
                       return edge.toNode == wrapperId;
                   }) != 1)
            return fail(wrapperId,
                "MemoryWriter recording-name wrapper requires one upstream mono Sound");
        const auto sourceNodeId = input->fromNode;
        const auto sourcePortId = input->fromPort;
        for (auto& edge : graph.connections)
            if (edge.fromNode == wrapperId)
            {
                edge.fromNode = sourceNodeId;
                edge.fromPort = sourcePortId;
            }
        std::erase_if(graph.connections, [&](const auto& edge) {
            return edge.toNode == wrapperId;
        });
        std::erase_if(graph.nodes, [&](const auto& node) {
            return node.instanceId == wrapperId;
        });
        std::erase_if(result.manifest.nodes, [&](const auto& node) {
            return node.flatNodeId == wrapperId;
        });
        std::erase_if(result.manifest.nodeParameters, [&](const auto& binding) {
            return binding.flatNodeId == wrapperId;
        });
    }
    return true;
}
}

std::optional<ProjectTrackInputSubstitution> projectWithTrackInputSubstituted(
    const ProjectDocument& source,
    std::string_view soundDefinitionId,
    std::optional<std::string_view> sourceInstanceId,
    std::size_t channelCount,
    std::string_view syntheticSourceId)
{
    const auto definition = std::ranges::find(
        source.soundDefinitions, soundDefinitionId, &ProjectSoundDefinition::definitionId);
    if (definition == source.soundDefinitions.end())
        return std::nullopt;
    std::vector<std::string_view> replaced;
    if (const auto* replaceable = replaceableSoundInput(*definition);
        replaceable != nullptr && replaceable->replacesSound)
    {
        // The replaceable input's Sound gives way to the track's input.
        replaced.push_back(replaceable->portId);
    }
    else
    {
        for (const auto& input : definition->inputs)
        {
            if (input.type == talas::core::PortType::Audio && replaced.size() < (channelCount >= 2 ? 2U : 1U))
                replaced.push_back(input.portId);
        }
    }
    if (replaced.empty())
        return std::nullopt;
    ProjectTrackInputSubstitution result{source, std::string{sourceInstanceId.value_or(syntheticSourceId)}};
    auto& graph = result.document.graph;
    if (!sourceInstanceId.has_value()
        || std::ranges::find(graph.nodes, *sourceInstanceId, &ProjectNode::instanceId) == graph.nodes.end())
    {
        graph = {};
        ProjectNode instance;
        instance.instanceId = result.sourceInstanceId;
        instance.soundDefinitionId = std::string{soundDefinitionId};
        graph.nodes.push_back(std::move(instance));
    }
    std::erase_if(graph.connections, [&](const talas::core::Connection& connection) {
        return connection.toNode == result.sourceInstanceId
            && std::ranges::find(replaced, connection.toPort) != replaced.end();
    });
    std::string inputId = "scoreTrackInput";
    while (std::ranges::find(graph.nodes, inputId, &ProjectNode::instanceId) != graph.nodes.end())
        inputId.push_back('_');
    ProjectNode input{inputId, "talas.audioInput", {}};
    if (replaced.size() == 2)
    {
        ProjectParameterExpression routing;
        routing.parameterId = "routing";
        routing.choice = "stereoPair";
        ProjectParameterExpression right;
        right.parameterId = "channel2";
        right.boolean = true;
        input.parameters = {std::move(routing), std::move(right)};
    }
    graph.nodes.push_back(std::move(input));
    graph.connections.push_back({inputId, "audioOut", result.sourceInstanceId, std::string{replaced[0]}});
    if (replaced.size() == 2)
        graph.connections.push_back({inputId, "rightOut", result.sourceInstanceId, std::string{replaced[1]}});
    return result;
}

ProjectSoundPreparationDocumentResult projectSoundPreparationDocument(
    const ProjectDocument& source,
    std::string_view soundDefinitionId,
    std::optional<std::string_view> sourceInstanceId,
    std::size_t requestedOutputChannelCount,
    std::string_view syntheticSourceId)
{
    ProjectSoundPreparationDocumentResult result;
    if (!isValidScoreIdentifier(syntheticSourceId))
    {
        result.error = "Sound preparation received an invalid synthetic source identity";
        return result;
    }
    result.document = source;
    result.document.timeline.score = {};
    result.document.timeline.loop = {};
    // The isolated program is an execution projection, not a second project
    // authority. Snapshot scopes and C9 targets may name root entities that
    // are intentionally absent from the retained upstream closure.
    result.document.snapshotLibrary = {};
    result.document.liveSetLibrary = {};
    const auto definition = std::ranges::find(
        source.soundDefinitions,
        soundDefinitionId,
        &ProjectSoundDefinition::definitionId);
    if (definition == source.soundDefinitions.end())
    {
        result.error = "Sound preparation lost definition '"
            + std::string{soundDefinitionId} + "'";
        return result;
    }

    std::string sourceNodeId;
    if (sourceInstanceId.has_value())
    {
        const auto instance = std::ranges::find(
            source.graph.nodes,
            *sourceInstanceId,
            &ProjectNode::instanceId);
        if (instance == source.graph.nodes.end()
            || !instance->soundDefinitionId.has_value()
            || *instance->soundDefinitionId != soundDefinitionId)
        {
            result.error = "Sound preparation lost exact source instance '"
                + std::string{*sourceInstanceId} + "'";
            return result;
        }
        std::unordered_multimap<std::string_view, std::string_view>
            incomingConnections;
        incomingConnections.reserve(source.graph.connections.size());
        for (const auto& connection : source.graph.connections)
        {
            incomingConnections.emplace(
                connection.toNode,
                connection.fromNode);
        }
        std::unordered_set<std::string_view> upstreamIds;
        upstreamIds.reserve(source.graph.nodes.size());
        std::vector<std::string_view> pending{*sourceInstanceId};
        upstreamIds.insert(*sourceInstanceId);
        for (std::size_t cursor = 0; cursor < pending.size(); ++cursor)
        {
            const auto [first, last] = incomingConnections.equal_range(
                pending[cursor]);
            for (auto connection = first; connection != last; ++connection)
            {
                if (upstreamIds.insert(connection->second).second)
                    pending.push_back(connection->second);
            }
        }
        result.document.graph = source.graph;
        std::erase_if(
            result.document.graph.nodes,
            [&](const auto& candidate) {
                return !upstreamIds.contains(candidate.instanceId);
            });
        std::erase_if(
            result.document.graph.connections,
            [&](const auto& connection) {
                return !upstreamIds.contains(connection.fromNode)
                    || !upstreamIds.contains(connection.toNode);
            });
        sourceNodeId = std::string{*sourceInstanceId};
    }
    else
    {
        // A replaceable input left unconnected plays its own Sound.
        if (std::ranges::any_of(definition->inputs, [](const ProjectSoundPort& input) { return !input.replacesSound; }))
        {
            result.error = "Sound '" + std::string{soundDefinitionId}
                + "' requires an exact root-graph source instance";
            return result;
        }
        result.document.graph = {};
        ProjectNode instance;
        instance.instanceId = std::string{syntheticSourceId};
        instance.soundDefinitionId = std::string{soundDefinitionId};
        result.document.graph.nodes.push_back(std::move(instance));
        sourceNodeId = std::string{syntheticSourceId};
    }

    std::vector<std::string_view> audioPorts;
    for (const auto& output : definition->outputs)
    {
        if (output.type == talas::core::PortType::Audio)
            audioPorts.push_back(output.portId);
    }
    const auto channelCount = requestedOutputChannelCount == 0
        ? audioPorts.size()
        : requestedOutputChannelCount;
    if ((channelCount != 1 && channelCount != 2
         && channelCount != 4 && channelCount != 6
         && channelCount != 8)
        || (audioPorts.size() != 1
            && audioPorts.size() != channelCount))
    {
        result.error = "Sound preparation requires one, two, four, six, or eight exact Audio outputs";
        return result;
    }

    std::string terminalType;
    std::vector<std::string_view> terminalPorts;
    switch (channelCount)
    {
    case 1:
        terminalType = "talas.audioOutput";
        terminalPorts = {"audioIn"};
        break;
    case 2:
        terminalType = "talas.audioOutput";
        terminalPorts = {"audioIn", "rightIn"};
        break;
    case 4:
        terminalType = "talas.output4";
        terminalPorts = {"out1", "out2", "out3", "out4"};
        break;
    case 6:
        terminalType = "talas.output6";
        terminalPorts = {"out1", "out2", "out3", "out4", "out5", "out6"};
        break;
    case 8:
        terminalType = "talas.output8";
        terminalPorts = {
            "out1", "out2", "out3", "out4",
            "out5", "out6", "out7", "out8"};
        break;
    default:
        result.error = "Sound preparation received an unsupported channel count";
        return result;
    }
    auto terminalId = std::string{"liveSetPreparationOutput"};
    for (std::size_t suffix = 1;
         std::ranges::any_of(
             result.document.graph.nodes,
             [&](const auto& candidate) {
                 return candidate.instanceId == terminalId;
             });
         ++suffix)
    {
        terminalId = "liveSetPreparationOutput"
            + std::to_string(suffix);
    }
    ProjectNode terminal;
    terminal.instanceId = terminalId;
    terminal.typeId = std::move(terminalType);
    result.document.graph.nodes.push_back(std::move(terminal));
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        result.document.graph.connections.push_back({
            sourceNodeId,
            std::string{audioPorts.size() == 1
                ? audioPorts.front()
                : audioPorts[channel]},
            terminalId,
            std::string{terminalPorts[channel]},
        });
    }
    result.outputChannelCount = channelCount;
    return result;
}

ProjectGraph reachableProjectRootGraph(const ProjectDocument& document)
{
    return makeReachableRootGraph(document);
}

ProjectSoundExpansionResult expandProjectSounds(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    ProjectSoundExpansionResult result;
    const auto hasRootSoundInstance = std::ranges::any_of(
        document.graph.nodes,
        [](const auto& node) {
            return node.soundDefinitionId.has_value();
        });
    if (document.soundDefinitions.empty()
        && !hasRootSoundInstance)
    {
        if (std::ranges::any_of(
                document.graph.nodes,
                [](const auto& node) {
                    return node.typeId == "talas.mapEventValues"
                        || node.typeId == "talas.prefixer"
                        || node.typeId == "talas.replicator"
                        || node.typeId == "talas.stereoizer"
                        || node.typeId == "talas.surroundifer"
                        || node.typeId == "talas.sampleWithTimeIndex"
                        || node.typeId == "talas.multiSampleWithTimeIndex"
                        || node.typeId == "talas.splitSurroundSampleWithTimeIndex"
                        || node.typeId == "talas.splitSurroundOscillator"
                        || node.typeId == "talas.splitSurroundAntialiasedOscillator"
                        || node.typeId == "talas.sharedFileName"
                        || node.typeId == "talas.sharedFileNames"
                        || node.typeId == "talas.uniqueMemoryWriterRecordingNames"
                        || node.typeId == "talas.cancelUniqueMemoryWriterRecordingNames";
                }))
        {
            result.validation = validateProjectDocument(document, registry);
            if (!result.ok())
                return result;
        }
        result.document = document;
        for (const auto& node : document.graph.nodes)
        {
            result.manifest.nodes.push_back({
                node.instanceId,
                {},
                {},
                node.instanceId,
                0,
                node.instanceId,
            });
        }
        if (lowerSampleWithTimeIndex(result)
            && applyReplicatorCopies(result)
            && applyPrefixerEventNames(result)
            && applyMapEventValueBindings(result)
            && applySharedFileBindings(result)
            && applyMemoryWriterNameScopes(result)
            && lowerMultiSampleWithTimeIndex(result)
            && lowerSplitSurroundSampleWithTimeIndex(result)
            && lowerSplitSurroundOscillator(result)
            && lowerSplitSurroundAntialiasedOscillator(result))
            static_cast<void>(lowerSampleWithTimeIndex(result));
        return result;
    }

    const auto liveSetValidation = validateProjectLiveSetLibrary(document);
    for (const auto& diagnostic : liveSetValidation.diagnostics)
        result.validation.errors.push_back({
            talas::core::ValidationCode::InvalidLiveSet,
            diagnostic.message,
            diagnostic.path});
    if (liveSetValidation.truncated)
        result.validation.errors.push_back({
            talas::core::ValidationCode::InvalidLiveSet,
            "Live-set diagnostics exceed the fixed limit",
            "$.liveSetLibrary"});
    if (!result.validation.ok())
        return result;

    auto validationDocument = document;
    validationDocument.graph =
        makeReachableRootGraph(document);
    // Live Set Sound instances are prepared independently and may be outside
    // the root output's reachable graph.
    validationDocument.liveSetLibrary = {};
    result.validation =
        validateProjectDocument(validationDocument, registry);
    if (!result.validation.ok())
        return result;

    result.document = document;
    result.document.graph = {};
    ExpansionContext context{
        document,
        result.document,
        result.manifest};
    for (const auto& node : validationDocument.graph.nodes)
    {
        if (!node.soundDefinitionId.has_value())
            context.usedNodeIds.insert(node.instanceId);
    }
    static_cast<void>(
        expandGraph(
            context,
            validationDocument.graph,
            {},
            0,
            {},
            {}));
    if (!context.validation.ok())
    {
        result.document = {};
        result.manifest = {};
        result.validation = std::move(context.validation);
    }
    if (result.ok() && lowerSampleWithTimeIndex(result)
        && applyReplicatorCopies(result)
        && applyPrefixerEventNames(result)
        && applyMapEventValueBindings(result)
        && applySharedFileBindings(result)
        && applyMemoryWriterNameScopes(result)
        && lowerMultiSampleWithTimeIndex(result)
        && lowerSplitSurroundSampleWithTimeIndex(result)
        && lowerSplitSurroundOscillator(result)
        && lowerSplitSurroundAntialiasedOscillator(result))
        static_cast<void>(lowerSampleWithTimeIndex(result));
    return result;
}

ProjectSoundExpansionResult
expandProjectSoundDefinitionForPreparation(
    const ProjectDocument& document,
    std::string_view definitionId,
    const talas::core::NodeRegistry& registry)
{
    ProjectSoundExpansionResult result;
    result.validation = validateProjectDocument(document, registry);
    if (!result.validation.ok())
        return result;
    if (findDefinition(document, definitionId) == nullptr)
    {
        result.validation.errors.push_back({
            talas::core::ValidationCode::UnknownSoundDefinition,
            "Unknown reusable Sound definition",
            std::string{definitionId},
        });
        return result;
    }

    result.document = document;
    result.document.graph = {};
    ExpansionContext context{
        document,
        result.document,
        result.manifest,
    };
    ProjectGraph root;
    ProjectNode instance;
    instance.instanceId = "__kc_transformer_validation_root";
    instance.soundDefinitionId = std::string{definitionId};
    const auto* definition = findDefinition(document, definitionId);
    for (std::size_t inputIndex = 0;
         inputIndex < definition->inputs.size();
         ++inputIndex)
    {
        const auto& input = definition->inputs[inputIndex];
        if (input.type != talas::core::PortType::Audio)
            continue;
        auto boundaryId = "__kc_transformer_boundary_"
            + std::to_string(inputIndex);
        root.nodes.push_back({
            boundaryId,
            "talas.audioInput",
            {},
        });
        root.connections.push_back({
            boundaryId,
            "audioOut",
            instance.instanceId,
            input.portId,
        });
        result.preparationOpenBoundaryNodeIds.push_back(
            std::move(boundaryId));
    }
    root.nodes.push_back(std::move(instance));
    static_cast<void>(expandGraph(
        context,
        root,
        {},
        0,
        {},
        {}));
    if (!context.validation.ok())
    {
        result.document = {};
        result.manifest = {};
        result.validation = std::move(context.validation);
    }
    if (result.ok() && lowerSampleWithTimeIndex(result)
        && applyReplicatorCopies(result)
        && applyPrefixerEventNames(result)
        && applyMapEventValueBindings(result)
        && applySharedFileBindings(result)
        && applyMemoryWriterNameScopes(result)
        && lowerMultiSampleWithTimeIndex(result)
        && lowerSplitSurroundSampleWithTimeIndex(result)
        && lowerSplitSurroundOscillator(result)
        && lowerSplitSurroundAntialiasedOscillator(result))
        static_cast<void>(lowerSampleWithTimeIndex(result));
    return result;
}

talas::core::ValidationResult materializeProjectSoundVariableBranches(
    const ProjectDocument& source,
    ProjectSoundExpansionResult& expanded,
    const std::vector<ProjectMidiVoiceIsland>& midiVoiceIslands)
{
    ExpansionContext context{
        source,
        expanded.document,
        expanded.manifest,
    };
    context.expansionWorkItemCount = static_cast<std::size_t>(
        std::ranges::count_if(
            expanded.manifest.nodes,
            [](const auto& node) { return !node.instancePath.empty(); }));
    context.expandedConnectionCount = expanded.document.graph.connections.size();
    for (const auto& node : expanded.document.graph.nodes)
        context.usedNodeIds.insert(node.instanceId);
    for (const auto& scope : expanded.manifest.scopes)
        context.nextScopeOrdinal = std::max(
            context.nextScopeOrdinal,
            scope.scopeOrdinal);

    std::unordered_map<std::string, ProjectVariableOwner> voiceOwners;
    std::unordered_map<std::string, const ProjectMidiVoiceIsland*> voiceHierarchyByNode;
    const auto hierarchical = std::ranges::any_of(midiVoiceIslands,
        [](const auto& island) { return !island.parentOwnerNodeId.empty(); });
    for (const auto& island : midiVoiceIslands)
    {
        ProjectVariableOwner owner;
        owner.kind = ProjectVariableOwnerKind::MidiVoice;
        owner.definitionId = island.definitionId;
        owner.nodeId = island.localOwnerNodeId;
        for (const auto& nodeId : island.nodeIds)
        {
            if (!voiceOwners.emplace(nodeId, owner).second)
            {
                failExpansion(
                    context,
                    "Sound-variable selector belongs to multiple MIDI Voice owners",
                    nodeId);
                return std::move(context.validation);
            }
            voiceHierarchyByNode.emplace(nodeId, &island);
        }
    }

    std::unordered_set<std::string> processedSelectors;
    for (std::size_t manifestIndex = 0;
         manifestIndex < expanded.manifest.nodes.size();
         ++manifestIndex)
    {
        const auto identity = expanded.manifest.nodes[manifestIndex];
        const auto node = std::ranges::find(
            expanded.document.graph.nodes,
            identity.flatNodeId,
            &ProjectNode::instanceId);
        if (node == expanded.document.graph.nodes.end()
            || (node->typeId != "talas.variable"
                && node->typeId != "talas.soundCollectionVariable"))
        {
            continue;
        }
        if (!processedSelectors.insert(node->instanceId).second)
            continue;

        const auto selectorNodeId = node->instanceId;
        const auto selectorTypeId = node->typeId;
        const auto variableId = soundVariableId(*node);
        std::vector<std::string_view> definitionScope;
        auto currentScope = identity.scopeOrdinal;
        for (std::size_t depth = 0;
             currentScope != 0 && depth <= maxExpansionDepth;
             ++depth)
        {
            const auto scope = std::ranges::find(
                expanded.manifest.scopes,
                currentScope,
                &ExpandedSoundScopeIdentity::scopeOrdinal);
            if (scope == expanded.manifest.scopes.end())
                break;
            definitionScope.push_back(scope->definitionId);
            currentScope = scope->parentScopeOrdinal;
        }
        std::ranges::reverse(definitionScope);
        const auto inheritedVoice = voiceOwners.find(selectorNodeId);
        const auto inheritedVoiceOwner = inheritedVoice == voiceOwners.end()
            ? std::optional<ProjectVariableOwner>{}
            : std::optional<ProjectVariableOwner>{inheritedVoice->second};
        const auto* declaration = findVisibleProjectSoundVariableDeclaration(
            source,
            variableId,
            definitionScope,
            inheritedVoiceOwner.has_value()
                ? &*inheritedVoiceOwner
                : nullptr);
        if (hierarchical)
        {
            // Walk the same interleaved Sound/voice lexical chain that the
            // prepared variable tables will project for every parent voice.
            std::vector<ProjectVariableOwner> lexicalOwners;
            auto sourceScope = identity.scopeOrdinal;
            const auto containing = voiceHierarchyByNode.find(selectorNodeId);
            const auto* voice = containing == voiceHierarchyByNode.end()
                ? nullptr : containing->second;
            const auto appendSoundsUntil = [&](std::uint32_t boundary) {
                std::size_t depth = 0;
                while (sourceScope != boundary)
                {
                    if (++depth > expanded.manifest.scopes.size()) return false;
                    const auto scope = std::ranges::find(expanded.manifest.scopes,
                        sourceScope, &ExpandedSoundScopeIdentity::scopeOrdinal);
                    if (scope == expanded.manifest.scopes.end()) return false;
                    lexicalOwners.push_back({ProjectVariableOwnerKind::SoundDefinition,
                        scope->definitionId, {}});
                    sourceScope = scope->parentScopeOrdinal;
                }
                return true;
            };
            bool validChain = true;
            std::size_t voiceDepth = 0;
            while (voice != nullptr && validChain)
            {
                if (++voiceDepth > midiVoiceIslands.size())
                {
                    validChain = false;
                    break;
                }
                validChain = appendSoundsUntil(voice->sourceScopeOrdinal);
                lexicalOwners.push_back({ProjectVariableOwnerKind::MidiVoice,
                    voice->definitionId, voice->localOwnerNodeId});
                const auto parent = std::ranges::find(midiVoiceIslands,
                    voice->parentOwnerNodeId, &ProjectMidiVoiceIsland::flatOwnerNodeId);
                if (!voice->parentOwnerNodeId.empty() && parent == midiVoiceIslands.end())
                    validChain = false;
                voice = parent == midiVoiceIslands.end() ? nullptr : &*parent;
            }
            if (!validChain || !appendSoundsUntil(0))
            {
                failExpansion(context,
                    "Nested Sound-variable selector has no exact lexical source chain",
                    selectorNodeId);
                return std::move(context.validation);
            }
            declaration = nullptr;
            for (const auto& owner : lexicalOwners)
            {
                const auto found = std::ranges::find_if(source.soundVariableLibrary.variables,
                    [&](const auto& candidate) {
                        return candidate.variableId == variableId && candidate.owner == owner;
                    });
                if (found != source.soundVariableLibrary.variables.end())
                {
                    declaration = &*found;
                    break;
                }
            }
            if (declaration == nullptr)
                declaration = findVisibleProjectSoundVariableDeclaration(source, variableId);
        }
        // From format 90 compiling replaces a SoundCollectionVariable by
        // the Sounds of its collection, where a field takes more than one
        // Sound.
        if (selectorTypeId == "talas.soundCollectionVariable"
            && std::ranges::none_of(node->parameters, [](const auto& parameter) {
                   return parameter.parameterId == "members"
                       && parameter.choice == std::optional<std::string>{"legacySelected"};
               }))
        {
            failExpansion(
                context,
                "A SoundCollectionVariable stands for all its Sounds and can be placed only in a field that takes more than one Sound, such as a Mixer's Inputs",
                selectorNodeId);
            return std::move(context.validation);
        }
        // A Variable is a placeholder for a Sound, whose value is asked for
        // when it plays; with no declaration to give its Default Sound, and nothing a
        // Script assigned, it is silent.
        if (selectorTypeId == "talas.variable" && declaration == nullptr)
        {
            node->typeId = "talas.valToSound";
            node->parameters = {{"value", "0"}};
            continue;
        }
        const auto expectedKind = selectorTypeId == "talas.variable"
            ? ProjectSoundVariableKind::Sound
            : ProjectSoundVariableKind::SoundCollection;
        if (variableId.empty() || declaration == nullptr
            || declaration->kind != expectedKind)
        {
            failExpansion(
                context,
                "Sound-variable node does not resolve one exact visible declaration of the required kind",
                selectorNodeId);
            return std::move(context.validation);
        }

        expanded.manifest.soundVariableSelectors.push_back({
            selectorNodeId,
            identity.scopeOrdinal,
            declaration->variableId,
            declaration->kind,
            declaration->candidateDefinitionIds,
        });
        for (std::size_t candidateIndex = 0;
             candidateIndex < declaration->candidateDefinitionIds.size();
             ++candidateIndex)
        {
            const auto& candidateId =
                declaration->candidateDefinitionIds[candidateIndex];
            const auto* definition = findDefinition(source, candidateId);
            std::unordered_set<std::string> visitedDefinitions;
            if (definition == nullptr || !definition->inputs.empty()
                || definition->outputs.size() != 1
                || definition->outputs.front().type
                    != talas::core::PortType::Audio
                || containsMidiVoiceInDefinitionClosure(
                    source,
                    candidateId,
                    visitedDefinitions,
                    0))
            {
                failExpansion(
                    context,
                    "Sound-variable functional subset requires each candidate to be a source Sound with one mono Audio output and no MIDI Voice owner",
                    selectorNodeId);
                return std::move(context.validation);
            }

            ProjectGraph candidateGraph;
            ProjectNode candidateNode;
            candidateNode.instanceId = "$sound-variable-candidate:"
                + std::to_string(candidateIndex);
            candidateNode.soundDefinitionId = candidateId;
            candidateGraph.nodes.push_back(std::move(candidateNode));
            const auto firstAddedManifestNode =
                expanded.manifest.nodes.size();
            const auto branch = expandGraph(
                context,
                candidateGraph,
                identity.instancePath,
                identity.scopeOrdinal,
                identity.definitionId,
                {});
            if (!context.validation.ok())
                return std::move(context.validation);
            const auto endpoint = resolveEndpoint(
                branch,
                candidateGraph.nodes.front().instanceId,
                definition->outputs.front().portId,
                false);
            if (!endpoint.has_value())
            {
                failExpansion(
                    context,
                    "Sound-variable candidate output did not resolve to one prepared Audio branch",
                    selectorNodeId);
                return std::move(context.validation);
            }
            expanded.document.graph.connections.push_back({
                endpoint->nodeId,
                endpoint->portId,
                selectorNodeId,
                "candidate" + std::to_string(candidateIndex),
            });
            if (inheritedVoiceOwner.has_value())
            {
                for (std::size_t added = firstAddedManifestNode;
                     added < expanded.manifest.nodes.size();
                     ++added)
                {
                    voiceOwners.emplace(
                        expanded.manifest.nodes[added].flatNodeId,
                        *inheritedVoiceOwner);
                    const auto inheritedHierarchy = voiceHierarchyByNode.find(selectorNodeId);
                    if (inheritedHierarchy != voiceHierarchyByNode.end())
                        voiceHierarchyByNode.emplace(expanded.manifest.nodes[added].flatNodeId,
                            inheritedHierarchy->second);
                }
            }
        }
    }
    if (lowerSampleWithTimeIndex(expanded)
        && applyReplicatorCopies(expanded)
        && applyPrefixerEventNames(expanded)
        && applyMapEventValueBindings(expanded)
        && applySharedFileBindings(expanded)
        && applyMemoryWriterNameScopes(expanded)
        && lowerMultiSampleWithTimeIndex(expanded)
        && lowerSplitSurroundSampleWithTimeIndex(expanded)
        && lowerSplitSurroundOscillator(expanded)
        && lowerSplitSurroundAntialiasedOscillator(expanded))
        static_cast<void>(lowerSampleWithTimeIndex(expanded));
    return std::move(expanded.validation);
}

std::string withClassVariables(
    std::string_view expression,
    std::span<const std::pair<std::string, std::string>> values)
{
    std::string text{expression};
    for (const auto& [name, value] : values)
    {
        for (const auto mark : {'?', '@'})
        {
            const auto reference = mark + name;
            for (auto at = text.find(reference); at != std::string::npos; at = text.find(reference, at))
            {
                const auto end = at + reference.size();
                const auto continues = end < text.size()
                    && (std::isalnum(static_cast<unsigned char>(text[end])) != 0 || text[end] == '_');
                const auto follows = at > 0
                    && (std::isalnum(static_cast<unsigned char>(text[at - 1])) != 0 || text[at - 1] == '_');
                if (continues || follows)
                {
                    at = end;
                    continue;
                }
                const auto replacement = "(" + value + ")";
                text.replace(at, reference.size(), replacement);
                at += replacement.size();
            }
        }
    }
    return text;
}

namespace
{
void collectSampleDuration(const ProjectDocument& document, std::string_view definitionId,
    std::unordered_set<std::string>& visited, std::optional<double>& longest)
{
    if (!visited.insert(std::string{definitionId}).second)
        return;
    const auto definition = std::ranges::find(document.soundDefinitions, definitionId, &ProjectSoundDefinition::definitionId);
    if (definition == document.soundDefinitions.end())
        return;
    for (const auto& node : definition->graph.nodes)
    {
        for (const auto& parameter : node.parameters)
        {
            if (!parameter.resource.has_value())
                continue;
            const auto asset = std::ranges::find(document.assets, *parameter.resource, &ProjectAsset::id);
            if (asset != document.assets.end() && asset->kind == ProjectAssetKind::Audio && asset->sampleRate > 0.0
                && asset->frameCount > 0)
                longest = std::max(longest.value_or(0.0), static_cast<double>(asset->frameCount) / asset->sampleRate);
        }
        if (node.soundDefinitionId.has_value())
            collectSampleDuration(document, *node.soundDefinitionId, visited, longest);
    }
}
}

std::optional<double> soundDefinitionSampleDuration(const ProjectDocument& document, std::string_view definitionId)
{
    std::unordered_set<std::string> visited;
    std::optional<double> longest;
    collectSampleDuration(document, definitionId, visited, longest);
    return longest;
}
}
