#include "talas/app/PresetEventValuesModel.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <set>
#include <vector>

namespace talas::app
{
namespace
{
// The Event Value names, !Name, a field's text holds.
void collectEventNames(std::string_view text, std::set<std::string, std::less<>>& names)
{
    for (std::size_t at = text.find('!'); at != std::string_view::npos; at = text.find('!', at + 1))
    {
        auto end = at + 1;
        while (end < text.size()
            && (std::isalnum(static_cast<unsigned char>(text[end])) != 0 || text[end] == '_'))
            ++end;
        if (end > at + 1)
            names.emplace(text.substr(at + 1, end - at - 1));
    }
}

[[nodiscard]] std::string number(double value)
{
    std::array<char, 64> buffer{};
    const auto [end, failed] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return failed == std::errc{} ? std::string(buffer.data(), end) : std::string{"0"};
}
}

std::string presetCurrentEventValues(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::string_view presetNodeId)
{
    const talas::project::ProjectGraph* graph = &document.graph;
    if (definitionId.has_value())
    {
        const auto definition = std::ranges::find(
            document.soundDefinitions, *definitionId, &talas::project::ProjectSoundDefinition::definitionId);
        if (definition == document.soundDefinitions.end())
            return {};
        graph = &definition->graph;
    }
    const auto preset = std::ranges::find(graph->nodes, presetNodeId, &talas::project::ProjectNode::instanceId);
    if (preset == graph->nodes.end() || preset->typeId != "talas.preset")
        return {};
    // The Sounds the Preset hears, however far up.
    std::set<std::string, std::less<>> upstream;
    std::vector<std::string> pending{std::string{presetNodeId}};
    while (!pending.empty())
    {
        const auto current = std::move(pending.back());
        pending.pop_back();
        for (const auto& connection : graph->connections)
        {
            if (connection.toNode == current && upstream.insert(connection.fromNode).second)
                pending.push_back(connection.fromNode);
        }
    }
    std::set<std::string, std::less<>> names;
    for (const auto& node : graph->nodes)
    {
        if (!upstream.contains(node.instanceId))
            continue;
        for (const auto& parameter : node.parameters)
        {
            if (parameter.expression.has_value())
                collectEventNames(*parameter.expression, names);
            if (parameter.text.has_value())
                collectEventNames(*parameter.text, names);
        }
    }
    std::string entries;
    for (const auto& control : document.virtualControls)
    {
        if (!names.contains(control.eventName))
            continue;
        const auto channel = control.midiMapping.has_value() ? control.midiMapping->channel : 1;
        if (!entries.empty())
            entries += '\n';
        entries += "!" + control.eventName + " channel: " + std::to_string(channel)
            + "; initialValue: " + number(control.value) + ".";
    }
    return entries;
}

bool parameterTakesTyping(std::string_view typeId, std::string_view parameterId) noexcept
{
    return !(typeId == "talas.preset" && parameterId == "eventValues");
}
}
