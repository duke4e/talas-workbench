#include "talas/project/ProjectValidation.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectExpressionAudit.h"

#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectParameterTransformer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
using DefinitionMap =
    std::unordered_map<std::string, const ProjectSoundDefinition*>;

struct Endpoint
{
    std::string nodeId;
    std::string portId;

    bool operator==(const Endpoint&) const = default;
};

[[nodiscard]] bool isIdentifier(std::string_view value)
{
    const auto isStart = [](char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || character == '_';
    };
    const auto isContinue = [&](char character) {
        return isStart(character)
            || (character >= '0' && character <= '9');
    };
    return !value.empty() && isStart(value.front())
        && std::ranges::all_of(value.substr(1), isContinue);
}

[[nodiscard]] bool isKnownPortType(talas::core::PortType type)
{
    switch (type)
    {
    case talas::core::PortType::Audio:
    case talas::core::PortType::Control:
    case talas::core::PortType::Event:
    case talas::core::PortType::SampleBuffer:
    case talas::core::PortType::Wavetable:
    case talas::core::PortType::Spectrum:
    case talas::core::PortType::ComplexSpectrum:
    case talas::core::PortType::NoteStream:
        return true;
    }
    return false;
}

struct EndpointHash
{
    std::size_t operator()(const Endpoint& value) const noexcept
    {
        const auto nodeHash = std::hash<std::string>{}(value.nodeId);
        const auto portHash = std::hash<std::string>{}(value.portId);
        return nodeHash
            ^ (portHash + 0x9e3779b97f4a7c15ULL
               + (nodeHash << 6U) + (nodeHash >> 2U));
    }
};

[[nodiscard]] const talas::core::PortDescriptor* findPort(
    const std::vector<talas::core::PortDescriptor>& ports,
    std::string_view portId)
{
    const auto found = std::ranges::find_if(
        ports,
        [&](const auto& port) { return port.id == portId; });
    return found == ports.end() ? nullptr : &*found;
}

[[nodiscard]] const ProjectNode* findNode(
    const ProjectGraph& graph,
    std::string_view nodeId)
{
    const auto found = std::ranges::find_if(
        graph.nodes,
        [&](const auto& node) { return node.instanceId == nodeId; });
    return found == graph.nodes.end() ? nullptr : &*found;
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

[[nodiscard]] const ProjectSoundExposedParameter* findExposedParameter(
    const ProjectSoundDefinition& definition,
    std::string_view parameterId)
{
    const auto found = std::ranges::find_if(
        definition.exposedParameters,
        [&](const auto& parameter) {
            return parameter.parameterId == parameterId
                || std::ranges::find(parameter.aliases, parameterId)
                    != parameter.aliases.end();
        });
    return found == definition.exposedParameters.end()
        ? nullptr
        : &*found;
}

[[nodiscard]] const talas::core::ParameterDescriptor* findParameter(
    const talas::core::NodeTypeDescriptor& descriptor,
    std::string_view parameterId)
{
    const auto found = std::ranges::find_if(
        descriptor.parameters,
        [&](const auto& parameter) {
            return parameter.id == parameterId;
        });
    return found == descriptor.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] std::size_t parameterValueCount(
    const ProjectParameterExpression& parameter)
{
    return static_cast<std::size_t>(parameter.expression.has_value())
        + static_cast<std::size_t>(parameter.choice.has_value())
        + static_cast<std::size_t>(parameter.boolean.has_value())
        + static_cast<std::size_t>(parameter.resource.has_value())
        + static_cast<std::size_t>(parameter.text.has_value());
}

[[nodiscard]] bool parameterValueMatches(
    const ProjectParameterExpression& value,
    const ProjectSoundExposedParameter& exposed,
    talas::expr::ExpressionLanguageVersion language,
    const talas::expr::ExpressionDefinitionLibrary& definitions)
{
    if (parameterValueCount(value) != 1)
        return false;
    switch (exposed.kind)
    {
    case talas::core::ParameterKind::Expression:
    {
        if (!value.expression.has_value())
            return false;
        const auto specialized =
            talas::expr::specializeReplicationVariablesForAudit(
                *value.expression, 1, 1);
        if (!specialized.has_value())
            return false;
        if (const auto parsed = talas::expr::parse(
                *specialized,
                language,
                definitions);
            !parsed.ok())
        {
            return false;
        }
        else if (parsed.expression->eventReferences().empty()
                 && parsed.expression->variableReferences().empty()
                 && parsed.expression->identifiers().empty())
        {
            const auto evaluated = talas::expr::evaluate(
                *parsed.expression,
                {},
                {exposed.domain});
            // The Class Editor's range: a constant value
            // outside it is refused.
            return evaluated.ok()
                && std::isfinite(evaluated.value.value)
                && (!exposed.minimum.has_value() || evaluated.value.value >= *exposed.minimum)
                && (!exposed.maximum.has_value() || evaluated.value.value <= *exposed.maximum);
        }
        return true;
    }
    case talas::core::ParameterKind::Choice:
        return value.choice.has_value()
            && std::ranges::find(exposed.choices, *value.choice)
                != exposed.choices.end();
    case talas::core::ParameterKind::Boolean:
        return value.boolean.has_value();
    case talas::core::ParameterKind::Resource:
        return value.resource.has_value();
    case talas::core::ParameterKind::Text:
        return value.text.has_value();
    }
    return false;
}

[[nodiscard]] talas::core::NodeTypeDescriptor makeDefinitionDescriptor(
    const ProjectSoundDefinition& definition,
    const talas::core::NodeRegistry& registry)
{
    talas::core::NodeTypeDescriptor descriptor;
    descriptor.typeId = "talas.project.sound." + definition.definitionId;
    descriptor.displayName = definition.displayName;
    descriptor.category = "Project Sounds";
    descriptor.supportLevel = talas::core::NodeSupportLevel::Utility;
    descriptor.supportSummary =
        "Project-local reusable Sound definition.";
    descriptor.provenance.vendor = "Current project";
    descriptor.provenance.origin = talas::core::NodeOrigin::Project;
    descriptor.capabilities.offline =
        talas::core::ExecutionSupport::Supported;
    descriptor.capabilities.preparedRealtime =
        talas::core::ExecutionSupport::Subset;
    descriptor.capabilities.offlineBinding =
        talas::core::ExecutionBinding::GraphExpansion;
    descriptor.capabilities.preparedRealtimeBinding =
        talas::core::ExecutionBinding::GraphExpansion;
    descriptor.capabilities.nestedSounds =
        talas::core::NestedSoundSupport::Native;
    descriptor.capabilities.limitations = {
        "prepared-realtime-subset",
    };
    // An input that binds a Variable may be left unconnected: the Variable
    // then plays its Default Sound, or silence (ADR-0006 amendment of
    // 2026-10-13). One that binds a SoundCollectionVariable takes any number
    // of Sounds, the collection's own when it is given none,
    // and so does one bound to an input that takes many Sounds. The
    // replaceable input may be left unconnected too: its Sound then plays.
    for (const auto& input : definition.inputs)
    {
        const auto bound = std::ranges::find(
            definition.graph.nodes, input.nodeId, &ProjectNode::instanceId);
        const auto bindsVariable = bound != definition.graph.nodes.end()
            && (bound->typeId == "talas.variable" || bound->typeId == "talas.soundCollectionVariable")
            && !bound->soundDefinitionId.has_value();
        const auto* boundType = bound == definition.graph.nodes.end() || bound->soundDefinitionId.has_value()
            ? nullptr : registry.findNodeType(bound->typeId);
        talas::core::PortDescriptor port{input.portId, input.type, !bindsVariable && !input.replacesSound};
        port.acceptsMany = (bindsVariable && bound->typeId == "talas.soundCollectionVariable")
            || (boundType != nullptr
                && std::ranges::any_of(boundType->inputs, [&](const talas::core::PortDescriptor& candidate) {
                       return candidate.id == input.nodePortId && candidate.acceptsMany;
                   }));
        descriptor.inputs.push_back(std::move(port));
    }
    for (const auto& output : definition.outputs)
        descriptor.outputs.push_back({output.portId, output.type, false});
    // The Class Editor's Field Layout: fields from the top,
    // then from the left; those without a place after, in their order.
    std::vector<const ProjectSoundExposedParameter*> ordered;
    for (const auto& exposed : definition.exposedParameters)
        ordered.push_back(&exposed);
    std::ranges::stable_sort(ordered, [](const ProjectSoundExposedParameter* a, const ProjectSoundExposedParameter* b) {
        if (a->field.has_value() != b->field.has_value())
            return a->field.has_value();
        if (!a->field.has_value())
            return false;
        if (a->field->top != b->field->top)
            return a->field->top < b->field->top;
        return a->field->left < b->field->left;
    });
    for (const auto* placed : ordered)
    {
        const auto& exposed = *placed;
        talas::core::ParameterDescriptor parameter;
        parameter.id = exposed.parameterId;
        parameter.kind = exposed.kind;
        parameter.domain = exposed.domain;
        parameter.choices = exposed.choices;
        parameter.defaultExpression =
            exposed.defaultValue.expression.value_or("");
        parameter.defaultChoice =
            exposed.defaultValue.choice.value_or("");
        parameter.defaultBoolean = exposed.defaultValue.boolean;
        parameter.defaultResource =
            exposed.defaultValue.resource.value_or("");
        parameter.defaultText =
            exposed.defaultValue.text.value_or("");
        descriptor.parameters.push_back(std::move(parameter));
    }
    return descriptor;
}

void addError(
    talas::core::ValidationResult& result,
    talas::core::ValidationCode code,
    std::string message,
    std::string nodeId = {})
{
    result.errors.push_back(
        {code, std::move(message), std::move(nodeId)});
}

[[nodiscard]] bool hasExactlyOneIdentity(const ProjectNode& node)
{
    return !node.typeId.empty()
        != (node.soundDefinitionId.has_value()
            && !node.soundDefinitionId->empty());
}

[[nodiscard]] std::string qualifiedNodeId(
    std::string_view definitionId,
    std::string_view nodeId)
{
    if (definitionId.empty())
        return std::string(nodeId);
    return std::string(definitionId) + "/" + std::string(nodeId);
}

void appendGraphErrors(
    talas::core::ValidationResult& result,
    talas::core::ValidationResult graphResult,
    std::string_view definitionId,
    const std::unordered_set<Endpoint, EndpointHash>& boundaryInputs)
{
    for (auto& error : graphResult.errors)
    {
        if (error.code == talas::core::ValidationCode::MissingRequiredInput
            && !definitionId.empty())
        {
            const auto marker = std::string{"Missing required input: "};
            if (error.message.starts_with(marker)
                && boundaryInputs.contains(
                    {error.nodeId, error.message.substr(marker.size())}))
            {
                continue;
            }
        }
        if (!definitionId.empty())
        {
            error.message =
                "Sound " + std::string(definitionId) + ": "
                + error.message;
            if (!error.nodeId.empty())
                error.nodeId = qualifiedNodeId(definitionId, error.nodeId);
        }
        result.errors.push_back(std::move(error));
    }
}

void validateConnectionDestinations(
    talas::core::ValidationResult& result,
    const ProjectGraph& graph,
    std::string_view definitionId,
    const std::unordered_set<Endpoint, EndpointHash>& boundaryInputs,
    const talas::core::NodeRegistry& registry,
    const std::unordered_map<std::string, std::string>& dynamicTypeIds)
{
    // An input that accepts many connections sums them; each connection may
    // still appear only once. A Sound instance's input accepts many as its
    // definition says.
    const auto acceptsMany = [&](const talas::core::Connection& connection) {
        const auto node = std::ranges::find(
            graph.nodes, connection.toNode, &ProjectNode::instanceId);
        const auto dynamic = node == graph.nodes.end() || !node->soundDefinitionId.has_value()
            ? dynamicTypeIds.end() : dynamicTypeIds.find(*node->soundDefinitionId);
        const auto* type = node == graph.nodes.end() ? nullptr
            : dynamic != dynamicTypeIds.end() ? registry.findNodeType(dynamic->second)
            : registry.findNodeType(node->typeId);
        return type != nullptr
            && std::ranges::any_of(type->inputs, [&](const talas::core::PortDescriptor& port) {
                   return port.id == connection.toPort && port.acceptsMany;
               });
    };
    auto destinations = boundaryInputs;
    std::unordered_set<std::string> seenConnections;
    for (const auto& connection : graph.connections)
    {
        Endpoint endpoint{connection.toNode, connection.toPort};
        auto key = connection.fromNode;
        for (const auto* part : {&connection.fromPort, &connection.toNode, &connection.toPort})
            key.append(1, '\0').append(*part);
        const auto repeated = !seenConnections.insert(std::move(key)).second;
        if ((!destinations.insert(endpoint).second && !acceptsMany(connection))
            || repeated)
        {
            addError(
                result,
                talas::core::ValidationCode::DuplicateInputConnection,
                (definitionId.empty()
                     ? std::string{}
                     : "Sound " + std::string(definitionId) + ": ")
                    + "Input port has multiple bindings: "
                    + connection.toNode + "." + connection.toPort,
                qualifiedNodeId(definitionId, connection.toNode));
        }
    }
}

void validateProjectGraph(
    talas::core::ValidationResult& result,
    const ProjectDocument& document,
    const ProjectGraph& projectGraph,
    const talas::core::NodeRegistry& registry,
    std::string_view definitionId,
    const std::unordered_set<Endpoint, EndpointHash>& boundaryInputs)
{
    talas::core::NodeRegistry dynamicRegistry = registry;
    std::unordered_map<std::string, std::string> dynamicTypeIds;
    for (const auto& definition : document.soundDefinitions)
    {
        auto descriptor = makeDefinitionDescriptor(definition, registry);
        auto typeId = descriptor.typeId;
        std::size_t suffix = 0;
        while (dynamicRegistry.findNodeType(typeId) != nullptr)
            typeId = descriptor.typeId + "." + std::to_string(++suffix);
        descriptor.typeId = typeId;
        try
        {
            dynamicRegistry.addNodeType(std::move(descriptor));
        }
        catch (const std::invalid_argument& error)
        {
            if (result.ok())
            {
                addError(
                    result,
                    talas::core::ValidationCode::InvalidSoundBoundary,
                    "Invalid Sound interface descriptor: "
                        + std::string(error.what()),
                    definition.definitionId);
            }
            continue;
        }
        dynamicTypeIds.emplace(definition.definitionId, typeId);
    }

    talas::core::Graph graph;
    for (const auto& node : projectGraph.nodes)
    {
        if (!hasExactlyOneIdentity(node))
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidNodeIdentity,
                "Node must reference exactly one built-in type or Sound definition",
                qualifiedNodeId(definitionId, node.instanceId));
            graph.addNode({node.instanceId, {}});
            continue;
        }

        if (node.soundDefinitionId.has_value())
        {
            const auto typeId =
                dynamicTypeIds.find(*node.soundDefinitionId);
            if (typeId == dynamicTypeIds.end())
            {
                addError(
                    result,
                    talas::core::ValidationCode::UnknownSoundDefinition,
                    "Unknown Sound definition: "
                        + *node.soundDefinitionId,
                    qualifiedNodeId(definitionId, node.instanceId));
                graph.addNode({node.instanceId, {}});
                continue;
            }
            const auto* definition = findDefinition(
                document,
                *node.soundDefinitionId);
            std::unordered_set<std::string> resolvedParameterIds;
            for (const auto& overrideValue : node.parameters)
            {
                const auto* exposed = definition == nullptr
                    ? nullptr
                    : findExposedParameter(
                        *definition,
                        overrideValue.parameterId);
                if (exposed == nullptr)
                {
                    addError(
                        result,
                        talas::core::ValidationCode::InvalidNodeIdentity,
                        "Unknown Sound instance override: "
                            + overrideValue.parameterId,
                        qualifiedNodeId(
                            definitionId,
                            node.instanceId));
                    continue;
                }
                if (!resolvedParameterIds.insert(
                         exposed->parameterId)
                         .second)
                {
                    addError(
                        result,
                        talas::core::ValidationCode::InvalidNodeIdentity,
                        "Duplicate Sound instance override after alias resolution: "
                            + exposed->parameterId,
                        qualifiedNodeId(
                            definitionId,
                            node.instanceId));
                    continue;
                }
                if (!parameterValueMatches(
                        overrideValue,
                        *exposed,
                        document.expressionLanguage,
                        document.expressionDefinitions))
                {
                    addError(
                        result,
                        talas::core::ValidationCode::InvalidNodeIdentity,
                        "Sound instance override does not match exposed parameter kind: "
                            + exposed->parameterId,
                        qualifiedNodeId(
                            definitionId,
                            node.instanceId));
                }
            }
            graph.addNode({node.instanceId, typeId->second});
            continue;
        }

        graph.addNode({node.instanceId, node.typeId});
    }
    for (const auto& connection : projectGraph.connections)
        graph.connect(connection);

    validateConnectionDestinations(
        result,
        projectGraph,
        definitionId,
        boundaryInputs,
        dynamicRegistry,
        dynamicTypeIds);
    appendGraphErrors(
        result,
        graph.validate(dynamicRegistry),
        definitionId,
        boundaryInputs);
}

void validateBoundary(
    talas::core::ValidationResult& result,
    const ProjectDocument& document,
    const ProjectSoundDefinition& definition,
    const ProjectSoundPort& boundary,
    bool isInput,
    const talas::core::NodeRegistry& registry)
{
    const auto* node = findNode(definition.graph, boundary.nodeId);
    if (node == nullptr)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidSoundBoundary,
            "Sound " + definition.definitionId
                + " boundary " + boundary.portId
                + " references unknown internal node: "
                + boundary.nodeId,
            definition.definitionId);
        return;
    }
    const auto descriptor =
        describeProjectNode(document, *node, registry);
    if (!descriptor.has_value())
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidSoundBoundary,
            "Sound " + definition.definitionId
                + " boundary " + boundary.portId
                + " references a node with an invalid type identity",
            qualifiedNodeId(definition.definitionId, node->instanceId));
        return;
    }
    // An input boundary may bind a Variable itself, named by its output: an
    // instance gives the Variable the Sound connected to that input, as a
    // class made from an example gives its Variable parameter a Sound
    // (ADR-0006 amendment of 2026-10-13), and a
    // SoundCollectionVariable the Sounds connected there.
    const auto bindsVariable = isInput
        && (node->typeId == "talas.variable" || node->typeId == "talas.soundCollectionVariable");
    // The replaceable input
    // names a Sound of the graph, not another Sound class's instance or a
    // Variable, by an Audio output, which is not the Sound's own output.
    if (boundary.replacesSound
        && (!isInput || bindsVariable || node->soundDefinitionId.has_value()
            || boundary.type != talas::core::PortType::Audio
            || std::ranges::any_of(definition.outputs, [&](const ProjectSoundPort& output) {
                   return output.nodeId == boundary.nodeId;
               })
            || std::ranges::count_if(definition.inputs, &ProjectSoundPort::replacesSound) > 1))
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidSoundBoundary,
            "Sound " + definition.definitionId
                + " has one replaceable input, an Audio output of one of its Sounds other than its own output: "
                + boundary.portId,
            qualifiedNodeId(definition.definitionId, node->instanceId));
        return;
    }
    const auto* port = findPort(
        isInput && !bindsVariable && !boundary.replacesSound ? descriptor->inputs : descriptor->outputs,
        boundary.nodePortId);
    if (port == nullptr)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidSoundBoundary,
            "Sound " + definition.definitionId
                + " boundary " + boundary.portId
                + " references unknown internal "
                + (isInput ? "input" : "output") + " port: "
                + boundary.nodePortId,
            qualifiedNodeId(definition.definitionId, node->instanceId));
        return;
    }
    if (port->type != boundary.type)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidSoundBoundary,
            "Sound " + definition.definitionId
                + " boundary " + boundary.portId
                + " type does not match its internal endpoint",
            qualifiedNodeId(definition.definitionId, node->instanceId));
    }
}

// Format 140: Sound collections, each named and holding Sounds of the file
// and other collections, none twice and none within itself.
void validateSoundCollections(
    talas::core::ValidationResult& result,
    const ProjectDocument& document)
{
    std::unordered_set<std::string> ids;
    for (const auto& collection : document.soundCollections)
    {
        if (!isIdentifier(collection.collectionId) || collection.name.empty() || !ids.insert(collection.collectionId).second
            || std::ranges::any_of(document.soundDefinitions, [&](const ProjectSoundDefinition& definition) {
                   return definition.definitionId == collection.collectionId;
               }))
        {
            addError(result, talas::core::ValidationCode::InvalidSoundBoundary,
                "Sound collection ids must be distinct identifiers, unlike any Sound's, and each collection named: "
                    + collection.collectionId);
        }
    }
    std::unordered_set<std::string> members;
    std::unordered_map<std::string, const ProjectSoundCollection*> byId;
    for (const auto& collection : document.soundCollections)
        byId.emplace(collection.collectionId, &collection);
    for (const auto& collection : document.soundCollections)
    {
        for (const auto& sound : collection.soundIds)
        {
            const auto known = std::ranges::any_of(document.soundDefinitions, [&](const ProjectSoundDefinition& definition) {
                return definition.definitionId == sound;
            });
            if (!known || !members.insert(sound).second)
            {
                addError(result, talas::core::ValidationCode::InvalidSoundBoundary,
                    "Sound collection " + collection.collectionId + " holds an unknown Sound or one another holds: " + sound);
            }
        }
        for (const auto& nested : collection.collectionIds)
        {
            if (!byId.contains(nested) || nested == collection.collectionId || !members.insert(nested).second)
            {
                addError(result, talas::core::ValidationCode::InvalidSoundBoundary,
                    "Sound collection " + collection.collectionId + " holds an unknown collection or one another holds: "
                        + nested);
            }
        }
    }
    // No collection holds itself through others.
    for (const auto& collection : document.soundCollections)
    {
        std::unordered_set<std::string> seen{collection.collectionId};
        std::vector<const ProjectSoundCollection*> pending{&collection};
        while (!pending.empty())
        {
            const auto* current = pending.back();
            pending.pop_back();
            for (const auto& nested : current->collectionIds)
            {
                const auto found = byId.find(nested);
                if (found == byId.end())
                    continue;
                if (nested == collection.collectionId)
                {
                    addError(result, talas::core::ValidationCode::InvalidSoundBoundary,
                        "Sound collection " + collection.collectionId + " holds itself");
                    pending.clear();
                    break;
                }
                if (seen.insert(nested).second)
                    pending.push_back(found->second);
            }
        }
    }
}

void validateDefinitionShape(
    talas::core::ValidationResult& result,
    const ProjectSoundDefinition& definition)
{
    if (!isIdentifier(definition.definitionId))
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidSoundBoundary,
            "Sound definition id must be a valid identifier");
    }
    if (definition.displayName.empty())
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidSoundBoundary,
            "Sound definition name cannot be empty",
            definition.definitionId);
    }

    const auto validatePorts = [&](const auto& ports, std::string_view kind) {
        std::unordered_set<std::string> ids;
        for (const auto& port : ports)
        {
            if (!isIdentifier(port.portId) || port.nodeId.empty()
                || port.nodePortId.empty())
            {
                addError(
                    result,
                    talas::core::ValidationCode::InvalidSoundBoundary,
                    "Sound " + definition.definitionId + " "
                        + std::string(kind)
                    + " boundary identities must be valid and non-empty",
                    definition.definitionId);
            }
            if (!isKnownPortType(port.type))
            {
                addError(
                    result,
                    talas::core::ValidationCode::InvalidSoundBoundary,
                    "Sound " + definition.definitionId + " "
                        + std::string(kind)
                        + " boundary has an invalid port type",
                    definition.definitionId);
            }
            if (!ids.insert(port.portId).second)
            {
                addError(
                    result,
                    talas::core::ValidationCode::InvalidSoundBoundary,
                    "Sound " + definition.definitionId + " has duplicate "
                        + std::string(kind) + " boundary: " + port.portId,
                    definition.definitionId);
            }
        }
    };
    validatePorts(definition.inputs, "input");
    validatePorts(definition.outputs, "output");
}

void validateDefinitionInterface(
    talas::core::ValidationResult& result,
    const ProjectDocument& document,
    const ProjectSoundDefinition& definition,
    const talas::core::NodeRegistry& registry)
{
    if (definition.interfaceVersion == 0)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidSoundBoundary,
            "Sound " + definition.definitionId
                + " interface version must be positive",
            definition.definitionId);
    }

    if (definition.description.size() > maximumSoundDescriptionBytes)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidSoundBoundary,
            "Sound " + definition.definitionId + " description exceeds 16384 bytes",
            definition.definitionId);
    }
    // Format 139: the class's icon, another known kind of Sound's or drawn
    // in rows of the icon's height.
    if (definition.icon.has_value())
    {
        const auto& icon = *definition.icon;
        const auto knownSound = icon.sameAs.empty() || registry.findNodeType(icon.sameAs) != nullptr
            || std::ranges::any_of(document.soundDefinitions, [&](const ProjectSoundDefinition& other) {
                   return other.definitionId == icon.sameAs;
               });
        if (!knownSound || (!icon.large.empty() && icon.large.size() != soundIconLargeHeight)
            || (!icon.small.empty() && icon.small.size() != soundIconSmallHeight) || icon == ProjectSoundIcon{}
            || std::ranges::any_of(std::array{icon.inputType, icon.outputType}, [](const std::string& type) {
                   return !type.empty()
                       && std::ranges::none_of(std::array<std::string_view, 8>{"none", "audio", "subaudio", "spectrum",
                                                   "transparent", "constructor", "variable", "environment"},
                           [&](std::string_view name) { return name == type; });
               }))
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidSoundBoundary,
                "Sound " + definition.definitionId
                    + " icon names an unknown Sound or type, or has rows other than the icon's",
                definition.definitionId);
        }
    }

    std::unordered_set<std::string> identities;
    std::unordered_set<std::string> boundTargets;
    for (const auto& exposed : definition.exposedParameters)
    {
        if (!isValidSoundParameterClassFields(exposed))
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidSoundBoundary,
                "Sound " + definition.definitionId
                    + " parameter range, field or comment is invalid: " + exposed.parameterId,
                definition.definitionId);
        }
        if (!isIdentifier(exposed.parameterId)
            || exposed.displayName.empty()
            || exposed.defaultValue.parameterId
                != exposed.parameterId)
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidSoundBoundary,
                "Sound " + definition.definitionId
                    + " exposed parameter identity, name, and default id must be valid",
                definition.definitionId);
        }
        if (!identities.insert(exposed.parameterId).second)
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidSoundBoundary,
                "Sound " + definition.definitionId
                    + " has duplicate exposed parameter identity: "
                    + exposed.parameterId,
                definition.definitionId);
        }
        for (const auto& alias : exposed.aliases)
        {
            if (!isIdentifier(alias)
                || !identities.insert(alias).second)
            {
                addError(
                    result,
                    talas::core::ValidationCode::InvalidSoundBoundary,
                    "Sound " + definition.definitionId
                        + " has ambiguous exposed parameter alias: "
                        + alias,
                    definition.definitionId);
            }
        }
        if (exposed.kind != talas::core::ParameterKind::Choice
            && !exposed.choices.empty())
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidSoundBoundary,
                "Sound " + definition.definitionId
                    + " non-choice parameter cannot declare choices: "
                    + exposed.parameterId,
                definition.definitionId);
        }
        if (exposed.kind == talas::core::ParameterKind::Choice
            && exposed.choices.empty())
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidSoundBoundary,
                "Sound " + definition.definitionId
                    + " choice parameter requires choices: "
                    + exposed.parameterId,
                definition.definitionId);
        }
        if (exposed.kind == talas::core::ParameterKind::Choice)
        {
            std::unordered_set<std::string> choices;
            for (const auto& choice : exposed.choices)
            {
                if (choice.empty()
                    || !choices.insert(choice).second)
                {
                    addError(
                        result,
                        talas::core::ValidationCode::InvalidSoundBoundary,
                        "Sound " + definition.definitionId
                            + " choice parameter values must be non-empty and unique: "
                            + exposed.parameterId,
                        definition.definitionId);
                }
            }
        }
        if (exposed.kind != talas::core::ParameterKind::Expression
            && exposed.domain != talas::expr::ValueDomain::Scalar)
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidSoundBoundary,
                "Sound " + definition.definitionId
                    + " non-expression parameter must use the scalar domain: "
                    + exposed.parameterId,
                definition.definitionId);
        }
        if (!parameterValueMatches(
                exposed.defaultValue,
                exposed,
                document.expressionLanguage,
                document.expressionDefinitions))
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidSoundBoundary,
                "Sound " + definition.definitionId
                    + " exposed parameter default does not match its kind: "
                    + exposed.parameterId,
                definition.definitionId);
        }
        if (exposed.bindings.empty())
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidSoundBoundary,
                "Sound " + definition.definitionId
                    + " exposed parameter requires at least one binding: "
                    + exposed.parameterId,
                definition.definitionId);
        }

        for (const auto& binding : exposed.bindings)
        {
            if (binding.target
                == ProjectSoundParameterTarget::NodeParameter)
            {
                const auto key = "node:" + binding.nodeId + ":"
                    + binding.parameterId;
                if (!boundTargets.insert(key).second)
                {
                    addError(
                        result,
                        talas::core::ValidationCode::InvalidSoundBoundary,
                        "Sound " + definition.definitionId
                            + " has an ambiguous parameter binding: "
                            + binding.nodeId + "."
                            + binding.parameterId,
                        qualifiedNodeId(
                            definition.definitionId,
                            binding.nodeId));
                    continue;
                }
                const auto* node = findNode(
                    definition.graph,
                    binding.nodeId);
                const auto descriptor = node == nullptr
                    ? std::optional<talas::core::NodeTypeDescriptor>{}
                    : describeProjectNode(document, *node, registry);
                const auto* target = descriptor.has_value()
                    ? findParameter(*descriptor, binding.parameterId)
                    : nullptr;
                if (target == nullptr)
                {
                    addError(
                        result,
                        talas::core::ValidationCode::InvalidSoundBoundary,
                        "Sound " + definition.definitionId
                            + " binding references an unknown internal parameter: "
                            + binding.nodeId + "."
                            + binding.parameterId,
                        qualifiedNodeId(
                            definition.definitionId,
                            binding.nodeId));
                    continue;
                }
                if (target->kind != exposed.kind
                    || (exposed.kind
                            == talas::core::ParameterKind::Expression
                        && target->domain != exposed.domain)
                    || (exposed.kind
                            == talas::core::ParameterKind::Choice
                        && std::ranges::any_of(
                            exposed.choices,
                            [&](const auto& choice) {
                                return std::ranges::find(
                                           target->choices,
                                           choice)
                                    == target->choices.end();
                            })))
                {
                    addError(
                        result,
                        talas::core::ValidationCode::InvalidSoundBoundary,
                        "Sound " + definition.definitionId
                            + " binding is incompatible with internal parameter: "
                            + binding.nodeId + "."
                            + binding.parameterId,
                        qualifiedNodeId(
                            definition.definitionId,
                            binding.nodeId));
                }
            }
            else if (binding.target
                     == ProjectSoundParameterTarget::EventValue)
            {
                const auto key = "event:" + binding.eventName;
                if (!boundTargets.insert(key).second
                    || !isIdentifier(binding.eventName)
                    || exposed.kind
                        != talas::core::ParameterKind::Expression
                    || binding.eventDomain != exposed.domain)
                {
                    addError(
                        result,
                        talas::core::ValidationCode::InvalidSoundBoundary,
                        "Sound " + definition.definitionId
                            + " has an invalid or ambiguous local Event Value binding: "
                            + binding.eventName,
                        definition.definitionId);
                }
            }
            else if (binding.target == ProjectSoundParameterTarget::Variable)
            {
                // A ?variable of the class's example.
                const auto key = "variable:" + binding.variableName;
                if (!boundTargets.insert(key).second
                    || !isIdentifier(binding.variableName)
                    || exposed.kind != talas::core::ParameterKind::Expression)
                {
                    addError(
                        result,
                        talas::core::ValidationCode::InvalidSoundBoundary,
                        "Sound " + definition.definitionId
                            + " has an invalid or ambiguous variable binding: "
                            + binding.variableName,
                        definition.definitionId);
                }
            }
            else
            {
                addError(
                    result,
                    talas::core::ValidationCode::InvalidSoundBoundary,
                    "Sound " + definition.definitionId
                        + " has an unknown exposed parameter binding target: "
                        + exposed.parameterId,
                    definition.definitionId);
            }
        }
    }
}

void validateDefinitionDependencies(
    talas::core::ValidationResult& result,
    const ProjectDocument& document,
    const DefinitionMap& definitions)
{
    std::unordered_map<std::string, std::vector<std::string>> dependencies;
    for (const auto& definition : document.soundDefinitions)
    {
        for (const auto& node : definition.graph.nodes)
        {
            if (node.soundDefinitionId.has_value()
                && definitions.contains(*node.soundDefinitionId))
            {
                dependencies[definition.definitionId].push_back(
                    *node.soundDefinitionId);
            }
        }
    }

    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    std::string recursiveDefinition;
    std::function<void(const std::string&)> visit =
        [&](const std::string& definitionId) {
            if (!recursiveDefinition.empty()
                || visited.contains(definitionId))
            {
                return;
            }
            if (!visiting.insert(definitionId).second)
            {
                recursiveDefinition = definitionId;
                return;
            }
            for (const auto& dependency : dependencies[definitionId])
                visit(dependency);
            visiting.erase(definitionId);
            visited.insert(definitionId);
        };
    for (const auto& [definitionId, _] : definitions)
        visit(definitionId);

    if (!recursiveDefinition.empty())
    {
        addError(
            result,
            talas::core::ValidationCode::RecursiveSoundDefinition,
            "Recursive Sound definition dependency: "
                + recursiveDefinition,
            recursiveDefinition);
    }
}

}

std::optional<talas::core::NodeTypeDescriptor> describeProjectNode(
    const ProjectDocument& document,
    const ProjectNode& node,
    const talas::core::NodeRegistry& registry)
{
    if (!hasExactlyOneIdentity(node))
        return std::nullopt;
    if (!node.soundDefinitionId.has_value())
    {
        const auto* descriptor = registry.findNodeType(node.typeId);
        if (descriptor == nullptr)
            return std::nullopt;
        return *descriptor;
    }
    const auto* definition =
        findDefinition(document, *node.soundDefinitionId);
    if (definition == nullptr)
        return std::nullopt;
    return makeDefinitionDescriptor(*definition, registry);
}

std::optional<talas::core::NodeTypeDescriptor>
describeProjectNodeForEditing(
    const ProjectDocument& document,
    const ProjectNode& node,
    const talas::core::NodeRegistry& registry)
{
    if (!node.preservedComponent.has_value()
        || node.soundDefinitionId.has_value())
    {
        return describeProjectNode(document, node, registry);
    }
    if (validateProjectPreservedComponent(
            *node.preservedComponent)
            .has_value())
    {
        return std::nullopt;
    }
    const auto availability =
        classifyProjectComponentAvailability(
            node.typeId,
            &*node.preservedComponent,
            registry);
    if (availability.executable())
        return describeProjectNode(document, node, registry);
    return makeProjectPreservedComponentDescriptor(
        node.typeId,
        *node.preservedComponent);
}

talas::core::ValidationResult validateProjectSoundDefinitions(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    talas::core::ValidationResult result;
    DefinitionMap definitions;
    for (const auto& definition : document.soundDefinitions)
    {
        validateDefinitionShape(result, definition);
        if (!definitions.emplace(
                definition.definitionId,
                &definition)
                 .second)
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidSoundBoundary,
                "Duplicate Sound definition id: "
                    + definition.definitionId,
                definition.definitionId);
        }
    }

    validateDefinitionDependencies(result, document, definitions);
    validateSoundCollections(result, document);
    for (const auto& definition : document.soundDefinitions)
    {
        validateDefinitionInterface(
            result,
            document,
            definition,
            registry);
        std::unordered_set<Endpoint, EndpointHash> boundaryInputs;
        for (const auto& input : definition.inputs)
        {
            validateBoundary(
                result,
                document,
                definition,
                input,
                true,
                registry);
            if (!boundaryInputs.insert(
                    {input.nodeId, input.nodePortId})
                     .second)
            {
                addError(
                    result,
                    talas::core::ValidationCode::DuplicateInputConnection,
                    "Sound " + definition.definitionId
                        + " maps multiple boundaries to "
                        + input.nodeId + "." + input.nodePortId,
                    qualifiedNodeId(
                        definition.definitionId,
                        input.nodeId));
            }
        }
        for (const auto& output : definition.outputs)
        {
            validateBoundary(
                result,
                document,
                definition,
                output,
                false,
                registry);
        }
        validateProjectGraph(
            result,
            document,
            definition.graph,
            registry,
            definition.definitionId,
            boundaryInputs);
    }

    return result;
}

talas::core::ValidationResult
validateProjectComponentAvailabilityForExecution(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    talas::core::ValidationResult result;
    const auto validateAvailability = [&](
                                          const ProjectGraph& graph,
                                          std::string_view definitionId) {
        for (const auto& node : graph.nodes)
        {
            if (node.preservedComponent.has_value()
                && node.soundDefinitionId.has_value())
            {
                addError(
                    result,
                    talas::core::ValidationCode::InvalidNodeIdentity,
                    "Reusable Sound instances cannot carry a preserved component capsule",
                    qualifiedNodeId(definitionId, node.instanceId));
                continue;
            }
            const auto availability =
                classifyProjectComponentAvailability(
                    node.typeId,
                    node.preservedComponent.has_value()
                        ? &*node.preservedComponent
                        : nullptr,
                    registry);
            if (node.preservedComponent.has_value()
                && !availability.executable())
            {
                addError(
                    result,
                    talas::core::ValidationCode::UnknownNodeType,
                    availability.message,
                    qualifiedNodeId(definitionId, node.instanceId));
            }
        }
    };
    for (const auto& definition : document.soundDefinitions)
    {
        validateAvailability(
            definition.graph,
            definition.definitionId);
    }
    validateAvailability(document.graph, {});

    return result;
}

ProjectPersistenceValidationResult
validateProjectDocumentForPersistence(
    const ProjectDocument& document)
{
    ProjectPersistenceValidationResult result;
    const auto validateTransformationSources = [&](
        const ProjectGraph& graph,
        std::string_view graphPath) {
        for (std::size_t nodeIndex = 0;
             nodeIndex < graph.nodes.size();
             ++nodeIndex)
        {
            const auto& node = graph.nodes[nodeIndex];
            if (node.typeId != "talas.parameterTransformer")
                continue;
            const auto parameter = std::ranges::find(
                node.parameters,
                std::string_view{"transformation"},
                &ProjectParameterExpression::parameterId);
            if (parameter == node.parameters.end()
                || !parameter->text.has_value())
            {
                continue;
            }
            const auto parsed = parseProjectParameterTransformation(
                *parameter->text);
            auto path = std::string{graphPath}
                + ".nodes[" + std::to_string(nodeIndex)
                + "].parameters.transformation";
            if (parsed.ok())
            {
                const auto invalidExpression = std::ranges::find_if(
                    parsed.rules,
                    [&](const auto& rule) {
                        return rule.kind
                                == ProjectParameterTransformationRuleKind::Set
                            && !talas::expr::parse(
                                    rule.operand,
                                    document.expressionLanguage,
                                    document.expressionDefinitions).ok();
                    });
                if (invalidExpression == parsed.rules.end())
                    continue;
                result.diagnostics.push_back({
                    ProjectPersistenceValidationCode::InvalidParameterTransformation,
                    std::move(path),
                    "Set rule contains an invalid project-language expression",
                });
                continue;
            }
            if (!parsed.diagnostics.empty())
            {
                result.diagnostics.push_back({
                    ProjectPersistenceValidationCode::InvalidParameterTransformation,
                    std::move(path),
                    parsed.diagnostics.front().message,
                });
            }
            else
            {
                result.diagnostics.push_back({
                    ProjectPersistenceValidationCode::InvalidParameterTransformation,
                    std::move(path),
                    "Parameter transformation validation failed within fixed limits",
                });
            }
        }
    };
    validateTransformationSources(document.graph, "$.graph");
    for (std::size_t definitionIndex = 0;
         definitionIndex < document.soundDefinitions.size();
         ++definitionIndex)
    {
        validateTransformationSources(
            document.soundDefinitions[definitionIndex].graph,
            "$.soundDefinitions["
                + std::to_string(definitionIndex) + "].graph");
    }
    if (std::ranges::none_of(
            result.diagnostics,
            [](const auto& diagnostic) {
                return diagnostic.code
                    == ProjectPersistenceValidationCode::InvalidParameterTransformation;
            }))
    {
        const auto structural = validateProjectDocumentForEditing(
            document,
            talas::core::makeBuiltinNodeRegistry());
        const auto transformation = std::ranges::find(
            structural.errors,
            talas::core::ValidationCode::InvalidParameterTransformation,
            &talas::core::ValidationError::code);
        if (transformation != structural.errors.end())
        {
            result.diagnostics.push_back({
                ProjectPersistenceValidationCode::InvalidParameterTransformation,
                "$.graph",
                transformation->message,
            });
        }
    }
    const auto knownLanguage =
        document.expressionLanguage
            == talas::expr::ExpressionLanguageVersion::LegacyV1
        || talas::expr::isCompiledExpressionLanguage(
            document.expressionLanguage);
    if (!knownLanguage
        || document.formatVersion != currentProjectFormatVersion
        || (document.formatVersion < 23
            && document.expressionLanguage
                != talas::expr::ExpressionLanguageVersion::LegacyV1)
        || (document.formatVersion == 23
            && (document.expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::PureV3
                || document.expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::PureV4))
        || (document.formatVersion == 24
            && document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV4)
        || (!document.expressionDefinitions.definitions.empty()
            && !talas::expr::supportsExpressionDefinitions(
                document.expressionLanguage))
        || (document.formatVersion < 27
            && document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV1)
        || (document.formatVersion < 35
            && document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2)
        || (document.formatVersion < 37
            && document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV3)
        || (document.formatVersion < 40
            && document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV4)
        || (document.formatVersion < 38
            && document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV5))
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidExpressionLanguage,
            "$.expressionLanguage",
            "Project expression language is invalid for its format",
        });
    }
    // Format 156: a beat's tempo for fields from 1 to 1000 quarter notes a
    // minute.
    if (const auto& tempo = document.expressionDefinitions.fieldBeatTempoBpm;
        tempo.has_value()
        && !(std::isfinite(*tempo) && *tempo >= talas::expr::minimumFieldBeatTempoBpm
            && *tempo <= talas::expr::maximumFieldBeatTempoBpm))
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::NotCanonicallyRepresentable,
            "$.fieldBeatTempoBpm",
            "fieldBeatTempoBpm must be from 1 to 1000",
        });
    }
    if (knownLanguage)
    {
        const auto expressionAudit = auditProjectExpressions(
            document,
            document.expressionLanguage);
        for (const auto& diagnostic : expressionAudit.diagnostics)
        {
            result.diagnostics.push_back({
                ProjectPersistenceValidationCode::InvalidExpression,
                diagnostic.path,
                diagnostic.message,
            });
        }
        if (expressionAudit.truncated)
        {
            result.diagnostics.push_back({
                ProjectPersistenceValidationCode::InvalidExpression,
                "$",
                "Project expression diagnostics exceed the fixed limit",
            });
        }
    }
    const auto variableValidation = validateProjectVariableLibrary(document);
    for (const auto& diagnostic : variableValidation.diagnostics)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidVariable,
            diagnostic.path,
            diagnostic.message,
        });
    }
    if (variableValidation.truncated)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidVariable,
            "$.variableLibrary",
            "Variable diagnostics exceed the fixed limit",
        });
    }
    const auto soundVariableValidation =
        validateProjectSoundVariableLibrary(document);
    for (const auto& diagnostic : soundVariableValidation.diagnostics)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidVariable,
            diagnostic.path,
            diagnostic.message,
        });
    }
    if (soundVariableValidation.truncated)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidVariable,
            "$.soundVariableLibrary",
            "Sound-variable diagnostics exceed the fixed limit",
        });
    }
    const auto generatorValidation =
        validateProjectGeneratorLibrary(document.generatorLibrary);
    for (const auto& diagnostic : generatorValidation.diagnostics)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidGenerator,
            diagnostic.path,
            diagnostic.message,
        });
    }
    if (generatorValidation.truncated)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidGenerator,
            "$.generatorLibrary",
            "Generator diagnostics exceed the fixed limit",
        });
    }
    const auto generatorTargets =
        validateProjectGeneratorOutputTargets(
            document,
            talas::core::makeBuiltinNodeRegistry());
    for (const auto& diagnostic : generatorTargets.diagnostics)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidGenerator,
            diagnostic.path,
            diagnostic.message,
        });
    }
    if (generatorTargets.truncated)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidGenerator,
            "$.generatorLibrary",
            "Generator target diagnostics exceed the fixed limit",
        });
    }
    const auto liveSetValidation = validateProjectLiveSetLibrary(document);
    for (const auto& diagnostic : liveSetValidation.diagnostics)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidLiveSet,
            diagnostic.path,
            diagnostic.message,
        });
    }
    if (liveSetValidation.truncated)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidLiveSet,
            "$.liveSetLibrary",
            "Live-set diagnostics exceed the fixed limit",
        });
    }
    std::unordered_set<std::string> assetIds;
    for (std::size_t index = 0;
         index < document.assets.size();
         ++index)
    {
        const auto& asset = document.assets[index];
        const auto path = "$.assets["
            + std::to_string(index) + "]";
        if (const auto invalid = validateProjectAssetManifest(asset);
            invalid.has_value())
        {
            result.diagnostics.push_back({
                ProjectPersistenceValidationCode::InvalidAsset,
                path,
                invalid->message,
            });
        }
        if (asset.kind == ProjectAssetKind::PartialSpectrum
            && document.formatVersion
                < partialTrackProjectFormatVersion)
        {
            result.diagnostics.push_back({
                ProjectPersistenceValidationCode::InvalidAsset,
                path + ".kind",
                "Partial-spectrum assets require project format 22",
            });
        }
        if (!assetIds.insert(asset.id).second)
        {
            result.diagnostics.push_back({
                ProjectPersistenceValidationCode::DuplicateAsset,
                path + ".id",
                "Duplicate project asset id/generation",
            });
        }
    }
    if (const auto artifactError =
            validateProjectScoreRenderArtifacts(document))
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidAsset,
            "$.timeline.score.clips[" + artifactError->path
                + "].renderArtifact",
            artifactError->message,
        });
    }
    std::unordered_set<std::string> partialTrackEditSetIds;
    std::unordered_set<std::string> partialTrackEditAssetIds;
    if (document.partialTrackEdits.size()
        > maximumProjectPartialTrackEditSets)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidAsset,
            "$.partialTrackEdits",
            "Partial-track edit-set count exceeds the fixed limit",
        });
    }
    for (std::size_t index = 0;
         index < document.partialTrackEdits.size();
         ++index)
    {
        const auto& edits = document.partialTrackEdits[index];
        const auto path = "$.partialTrackEdits["
            + std::to_string(index) + "]";
        if (const auto invalid = validateProjectPartialTrackEditSet(edits))
        {
            result.diagnostics.push_back({
                ProjectPersistenceValidationCode::InvalidAsset,
                path,
                invalid->message,
            });
        }
        const auto asset = std::ranges::find(
            document.assets, edits.assetId, &ProjectAsset::id);
        if (asset == document.assets.end()
            || !partialTrackEditSetMatchesAsset(edits, *asset))
        {
            result.diagnostics.push_back({
                ProjectPersistenceValidationCode::InvalidAsset,
                path,
                "Partial-track edit set does not match its exact source asset",
            });
        }
        if (!partialTrackEditSetIds.insert(edits.editSetId).second
            || !partialTrackEditAssetIds.insert(edits.assetId).second)
        {
            result.diagnostics.push_back({
                ProjectPersistenceValidationCode::InvalidAsset,
                path,
                "Partial-track edit identities and target assets must be unique",
            });
        }
    }
    for (const auto& diagnostic :
         projectSampleMapDiagnostics(document))
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidSampleMap,
            diagnostic.path,
            diagnostic.message,
        });
    }
    std::vector<std::string_view> soundDefinitionIds;
    soundDefinitionIds.reserve(document.soundDefinitions.size());
    for (const auto& definition : document.soundDefinitions)
        soundDefinitionIds.push_back(definition.definitionId);
    if (const auto invalidScore = projectScoreValidationError(
            document.timeline.score,
            soundDefinitionIds))
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidScore,
            "$.timeline.score",
            invalidScore->message,
        });
    }
    if (const auto invalidSource =
            validateProjectScoreSourceBindings(document))
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::InvalidScore,
            "$.timeline.score.clips[" + invalidSource->entityId + "]",
            invalidSource->message,
        });
    }
    if (!result.ok())
        return result;

    ProjectLoadResult parsed;
    try
    {
        parsed = parseProjectDocument(
            serializeProjectDocument(document));
    }
    catch (const std::invalid_argument& error)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::NotCanonicallyRepresentable,
            "$",
            error.what(),
        });
        return result;
    }
    result.diagnostics.reserve(parsed.errors.size());
    for (const auto& error : parsed.errors)
    {
        result.diagnostics.push_back({
            ProjectPersistenceValidationCode::NotCanonicallyRepresentable,
            error.path,
            error.message,
        });
    }
    return result;
}

std::optional<ScoreValidationError>
validateProjectScoreSourceBindings(
    const ProjectDocument& document)
{
    for (const auto& clip : document.timeline.score.clips)
    {
        if (!clip.sourceInstanceId.has_value())
            continue;
        const auto instance = std::ranges::find(
            document.graph.nodes,
            *clip.sourceInstanceId,
            &ProjectNode::instanceId);
        if (instance == document.graph.nodes.end()
            || !instance->soundDefinitionId.has_value()
            || *instance->soundDefinitionId != clip.soundDefinitionId)
        {
            return ScoreValidationError{
                ScoreValidationCode::InvalidClip,
                clip.clipId,
                "Clip source instance must be an exact root-graph instance of its Sound definition",
            };
        }
    }
    return std::nullopt;
}

talas::core::ValidationResult validateProjectDocument(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    auto result =
        validateProjectComponentAvailabilityForExecution(
            document,
            registry);

    const auto variableValidation = validateProjectVariableLibrary(document);
    for (const auto& diagnostic : variableValidation.diagnostics)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidProjectVariable,
            diagnostic.message,
            diagnostic.path);
    }
    if (variableValidation.truncated)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidProjectVariable,
            "Variable diagnostics exceed the fixed limit",
            "$.variableLibrary");
    }

    const auto soundVariableValidation =
        validateProjectSoundVariableLibrary(document);
    for (const auto& diagnostic : soundVariableValidation.diagnostics)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidProjectVariable,
            diagnostic.message,
            diagnostic.path);
    }
    if (soundVariableValidation.truncated)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidProjectVariable,
            "Sound-variable diagnostics exceed the fixed limit",
            "$.soundVariableLibrary");
    }

    const auto generatorValidation =
        validateProjectGeneratorLibrary(document.generatorLibrary);
    for (const auto& diagnostic : generatorValidation.diagnostics)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidProjectVariable,
            diagnostic.message,
            diagnostic.path);
    }
    if (generatorValidation.truncated)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidProjectVariable,
            "Generator diagnostics exceed the fixed limit",
            "$.generatorLibrary");
    }

    const auto generatorTargets =
        validateProjectGeneratorOutputTargets(document, registry);
    for (const auto& diagnostic : generatorTargets.diagnostics)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidProjectVariable,
            diagnostic.message,
            diagnostic.path);
    }
    if (generatorTargets.truncated)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidProjectVariable,
            "Generator target diagnostics exceed the fixed limit",
            "$.generatorLibrary");
    }

    const auto liveSetValidation = validateProjectLiveSetLibrary(document);
    for (const auto& diagnostic : liveSetValidation.diagnostics)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidLiveSet,
            diagnostic.message,
            diagnostic.path);
    }
    if (liveSetValidation.truncated)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidLiveSet,
            "Live-set diagnostics exceed the fixed limit",
            "$.liveSetLibrary");
    }

    auto soundValidation =
        validateProjectSoundDefinitions(document, registry);
    result.errors.insert(
        result.errors.end(),
        std::make_move_iterator(soundValidation.errors.begin()),
        std::make_move_iterator(soundValidation.errors.end()));
    if (const auto invalidSource =
            validateProjectScoreSourceBindings(document))
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidNodeIdentity,
            invalidSource->message,
            invalidSource->entityId);
    }
    std::unordered_set<std::string> assetIds;
    for (const auto& asset : document.assets)
    {
        if (const auto invalid = validateProjectAssetManifest(asset);
            invalid.has_value())
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidProjectAsset,
                invalid->message,
                asset.id);
        }
        if (asset.kind == ProjectAssetKind::PartialSpectrum
            && document.formatVersion
                < partialTrackProjectFormatVersion)
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidProjectAsset,
                "Partial-spectrum assets require project format 22",
                asset.id);
        }
        if (!assetIds.insert(asset.id).second)
        {
            addError(
                result,
                talas::core::ValidationCode::DuplicateProjectAsset,
                "Duplicate project asset id/generation: "
                    + asset.id + "/"
                    + std::to_string(asset.generation),
                asset.id);
        }
    }
    if (const auto artifactError =
            validateProjectScoreRenderArtifacts(document))
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidProjectAsset,
            artifactError->message,
            artifactError->path);
    }
    std::unordered_set<std::string> partialTrackEditSetIds;
    std::unordered_set<std::string> partialTrackEditAssetIds;
    if (document.partialTrackEdits.size()
        > maximumProjectPartialTrackEditSets)
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidProjectAsset,
            "Partial-track edit-set count exceeds the fixed limit");
    }
    for (const auto& edits : document.partialTrackEdits)
    {
        if (const auto invalid = validateProjectPartialTrackEditSet(edits))
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidProjectAsset,
                invalid->message,
                edits.editSetId);
        }
        const auto asset = std::ranges::find(
            document.assets, edits.assetId, &ProjectAsset::id);
        if (asset == document.assets.end()
            || !partialTrackEditSetMatchesAsset(edits, *asset))
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidProjectAsset,
                "Partial-track edit set does not match its exact source asset",
                edits.editSetId);
        }
        if (!partialTrackEditSetIds.insert(edits.editSetId).second
            || !partialTrackEditAssetIds.insert(edits.assetId).second)
        {
            addError(
                result,
                talas::core::ValidationCode::InvalidProjectAsset,
                "Partial-track edit identities and target assets must be unique",
                edits.editSetId);
        }
    }
    for (const auto& diagnostic :
         projectSampleMapDiagnostics(document))
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidSampleMap,
            diagnostic.message,
            diagnostic.path);
    }
    validateProjectGraph(
        result,
        document,
        document.graph,
        registry,
        {},
        {});
    return result;
}

talas::core::ValidationResult validateProjectDocumentForEditing(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    auto preservationDocument = document;
    talas::core::NodeRegistry preservationRegistry;
    try
    {
        for (auto descriptor : registry.nodeTypes())
            preservationRegistry.addNodeType(std::move(descriptor));
    }
    catch (const std::invalid_argument& error)
    {
        talas::core::ValidationResult result;
        addError(
            result,
            talas::core::ValidationCode::UnknownNodeType,
            "Unable to clone registry for preservation validation: "
                + std::string(error.what()));
        return result;
    }

    talas::core::ValidationResult preparation;
    std::size_t syntheticOrdinal = 0;
    std::unordered_set<std::string> reservedTypeIds;
    const auto reserveGraphTypeIds = [&](const ProjectGraph& graph) {
        for (const auto& node : graph.nodes)
        {
            if (!node.typeId.empty())
                reservedTypeIds.insert(node.typeId);
        }
    };
    reserveGraphTypeIds(document.graph);
    for (const auto& definition : document.soundDefinitions)
        reserveGraphTypeIds(definition.graph);
    const auto prepareGraph = [&](
                                  ProjectGraph& graph,
                                  std::string_view definitionId) {
        for (auto& node : graph.nodes)
        {
            if (!node.preservedComponent.has_value())
                continue;
            if (node.soundDefinitionId.has_value())
            {
                addError(
                    preparation,
                    talas::core::ValidationCode::InvalidNodeIdentity,
                    "Reusable Sound instances cannot carry a preserved component capsule",
                    qualifiedNodeId(definitionId, node.instanceId));
                continue;
            }
            if (const auto invalid =
                    validateProjectPreservedComponent(
                        *node.preservedComponent);
                invalid.has_value())
            {
                addError(
                    preparation,
                    talas::core::ValidationCode::InvalidNodeIdentity,
                    "Invalid preserved component"
                        + invalid->path + ": " + invalid->message,
                    qualifiedNodeId(definitionId, node.instanceId));
                continue;
            }
            const auto availability =
                classifyProjectComponentAvailability(
                    node.typeId,
                    &*node.preservedComponent,
                    registry);
            if (availability.executable())
                continue;
            std::string syntheticTypeId;
            do
            {
                syntheticTypeId = "talas.preserved."
                    + std::to_string(syntheticOrdinal++);
            } while (
                reservedTypeIds.contains(syntheticTypeId)
                || preservationRegistry.findNodeType(syntheticTypeId)
                    != nullptr);
            reservedTypeIds.insert(syntheticTypeId);
            try
            {
                preservationRegistry.addNodeType(
                    makeProjectPreservedComponentDescriptor(
                        syntheticTypeId,
                        *node.preservedComponent));
                node.typeId = syntheticTypeId;
            }
            catch (const std::invalid_argument& error)
            {
                addError(
                    preparation,
                    talas::core::ValidationCode::InvalidNodeIdentity,
                    "Invalid frozen component interface: "
                        + std::string(error.what()),
                    qualifiedNodeId(definitionId, node.instanceId));
            }
        }
    };
    for (auto& definition : preservationDocument.soundDefinitions)
        prepareGraph(definition.graph, definition.definitionId);
    prepareGraph(preservationDocument.graph, {});
    if (!preparation.ok())
        return preparation;
    auto result = validateProjectDocument(
        preservationDocument,
        preservationRegistry);
    const auto transformation =
        prepareAuthoredProjectParameterTransformers(
            preservationDocument,
            preservationRegistry);
    if (!transformation.ok())
    {
        addError(
            result,
            talas::core::ValidationCode::InvalidParameterTransformation,
            transformation.diagnostics.empty()
                ? "Parameter transformation structural validation exceeded fixed limits"
                : transformation.diagnostics.front().message,
            transformation.diagnostics.empty()
                ? std::string{}
                : transformation.diagnostics.front().transformerNodeId);
    }
    return result;
}

bool isNoAudioTerminalNodeType(std::string_view typeId)
{
    return typeId == "talas.midiFileEcho"
        || typeId == "talas.midiOutputController"
        || typeId == "talas.midiOutputEvent"
        || typeId == "talas.midiOutputEventInBytes"
        || typeId == "talas.midiOutputTimingClock"
        || typeId == "talas.midiOutputStartStopMasterClock"
        || typeId == "talas.feedbackLoopInput"
        || typeId == "talas.soundToGlobalController"
        || typeId == "talas.triggeredSoundToGlobalController";
}

bool isTerminalNodeType(std::string_view typeId)
{
    return typeId == "talas.audioOutput"
        || typeId == "talas.diskRecorder"
        || typeId == "talas.output4"
        || typeId == "talas.output6"
        || typeId == "talas.output8"
        || typeId == "talas.stereoInOutput4"
        || typeId == "talas.stereoInOutput8"
        || isNoAudioTerminalNodeType(typeId);
}
}
