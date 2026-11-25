#include "talas/app/DefaultSounds.h"

#include "talas/app/GraphBranchLayout.h"
#include "talas/project/ProjectSoundVariables.h"

#include <algorithm>

namespace talas::app
{
namespace
{
[[nodiscard]] std::optional<std::string> variableIdOf(const talas::project::ProjectNode& node)
{
    if (node.typeId != "talas.variable" && node.typeId != "talas.soundCollectionVariable")
        return std::nullopt;
    const auto named = std::ranges::find(node.parameters, std::string_view{"variableId"},
        &talas::project::ProjectParameterExpression::parameterId);
    if (named == node.parameters.end() || !named->text.has_value() || named->text->empty())
        return std::nullopt;
    return *named->text;
}
}

std::vector<UndeclaredSoundVariable> undeclaredSoundVariables(const talas::project::ProjectDocument& document)
{
    std::vector<UndeclaredSoundVariable> undeclared;
    const auto visit = [&](const talas::project::ProjectGraph& graph) {
        for (const auto& node : graph.nodes)
        {
            const auto variableId = variableIdOf(node);
            if (!variableId.has_value()
                || std::ranges::any_of(document.soundVariableLibrary.variables,
                    [&](const auto& declaration) { return declaration.variableId == *variableId; })
                || std::ranges::any_of(undeclared, [&](const auto& asked) { return asked.variableId == *variableId; }))
                continue;
            undeclared.push_back({*variableId, node.typeId == "talas.soundCollectionVariable"});
        }
    };
    visit(document.graph);
    for (const auto& definition : document.soundDefinitions)
        visit(definition.graph);
    return undeclared;
}

talas::project::ProjectSoundDefinition defaultSoundOfNode(const talas::project::ProjectDocument& document,
    const talas::project::ProjectGraph& graph, std::string_view nodeId, const talas::core::NodeRegistry& registry)
{
    talas::project::ProjectSoundDefinition sound;
    const auto node = std::ranges::find(graph.nodes, nodeId, &talas::project::ProjectNode::instanceId);
    if (node == graph.nodes.end())
        return sound;
    if (node->soundDefinitionId.has_value())
    {
        const auto definition = std::ranges::find(
            document.soundDefinitions, *node->soundDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
        if (definition != document.soundDefinitions.end())
            return *definition;
    }
    auto kept = graphBranchOf(graph, nodeId);
    kept.insert(std::string{nodeId});
    sound.definitionId = "default_" + std::string{nodeId};
    sound.displayName = std::string{nodeId};
    for (const auto& member : graph.nodes)
        if (kept.contains(member.instanceId))
            sound.graph.nodes.push_back(member);
    for (const auto& connection : graph.connections)
        if (kept.contains(connection.fromNode) && kept.contains(connection.toNode))
            sound.graph.connections.push_back(connection);
    if (const auto* type = registry.findNodeType(node->typeId); type != nullptr)
    {
        const auto audio = std::ranges::find(type->outputs, talas::core::PortType::Audio, &talas::core::PortDescriptor::type);
        if (audio != type->outputs.end())
            sound.outputs.push_back({"audioOut", talas::core::PortType::Audio, node->instanceId, audio->id});
    }
    return sound;
}

talas::project::ProjectSoundDefinition factoryDefaultSound(const talas::core::NodeRegistry& registry)
{
    talas::project::ProjectGraph graph;
    graph.nodes.push_back({"sine", "talas.oscillator", {}});
    return defaultSoundOfNode({}, graph, "sine", registry);
}

std::vector<std::pair<std::string, std::vector<talas::project::ProjectSoundDefinition>>> defaultSoundEnvironment(
    const std::vector<UndeclaredSoundVariable>& asked, const talas::project::ProjectSoundDefinition& defaultSound,
    const std::vector<talas::project::ProjectSoundDefinition>& defaultCollection)
{
    std::vector<std::pair<std::string, std::vector<talas::project::ProjectSoundDefinition>>> environment;
    for (const auto& variable : asked)
    {
        environment.emplace_back(variable.variableId,
            variable.collection && !defaultCollection.empty()
                ? defaultCollection
                : std::vector<talas::project::ProjectSoundDefinition>{defaultSound});
    }
    return environment;
}
}
