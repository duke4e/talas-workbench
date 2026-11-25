#include "talas/project/ProjectStructureText.h"

#include <algorithm>
#include <string>
#include <vector>

namespace talas::project
{
namespace
{
// The Sounds feeding a node, in the order of its connections.
[[nodiscard]] std::vector<std::string> inputsOf(const ProjectGraph& graph, std::string_view nodeId)
{
    std::vector<std::string> inputs;
    for (const auto& connection : graph.connections)
    {
        if (connection.toNode == nodeId)
            inputs.push_back(connection.fromNode);
    }
    return inputs;
}

void appendStructure(
    const ProjectGraph& graph,
    const std::string& nodeId,
    std::size_t level,
    std::vector<std::string>& path,
    std::string& text)
{
    // An input's line is indented and starts with
    // a dot for each level down.
    text.append(level, '\t');
    text.append(level, '.');
    text += nodeId;
    const auto inputs = inputsOf(graph, nodeId);
    if (inputs.empty() || std::ranges::find(path, nodeId) != path.end())
    {
        text += '\n';
        return;
    }
    text += " (";
    for (std::size_t index = 0; index < inputs.size(); ++index)
    {
        if (index > 0)
            text += ", ";
        text += inputs[index];
    }
    text += ")\n";
    path.push_back(nodeId);
    for (const auto& input : inputs)
        appendStructure(graph, input, level + 1, path, text);
    path.pop_back();
}
}

std::string projectStructureAsText(const ProjectGraph& graph, std::string_view soundId)
{
    std::string root{soundId};
    if (root.empty())
    {
        const auto output = std::ranges::find(graph.nodes, std::string_view{"talas.audioOutput"}, &ProjectNode::typeId);
        if (output == graph.nodes.end())
            return {};
        const auto heard = inputsOf(graph, output->instanceId);
        if (heard.empty())
            return {};
        root = heard.front();
    }
    std::vector<std::string> path;
    std::string text;
    appendStructure(graph, root, 0, path, text);
    return text;
}

std::string soundStructureAsText(const ProjectSoundDefinition& sound)
{
    if (sound.outputs.empty())
        return {};
    return projectStructureAsText(sound.graph, sound.outputs.front().nodeId);
}
}
