#include "talas/app/SoundInformation.h"

#include "talas/app/GraphBranchLayout.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/project/ProjectValidation.h"

#include <algorithm>
#include <cstdio>
#include <regex>
#include <set>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string parameterValue(
    const talas::core::ParameterDescriptor& descriptor, const talas::project::ProjectParameterExpression* value)
{
    using Kind = talas::core::ParameterKind;
    switch (descriptor.kind)
    {
    case Kind::Expression:
        return value != nullptr && value->expression ? *value->expression : descriptor.defaultExpression;
    case Kind::Choice:
        return value != nullptr && value->choice ? *value->choice : descriptor.defaultChoice;
    case Kind::Boolean:
        return (value != nullptr && value->boolean ? *value->boolean : descriptor.defaultBoolean.value_or(false))
            ? "true"
            : "false";
    case Kind::Resource:
        return value != nullptr && value->resource ? *value->resource : descriptor.defaultResource;
    case Kind::Text:
        return value != nullptr && value->text ? *value->text : descriptor.defaultText;
    }
    return {};
}

// The names after ! (Event Values) or ? (variables) in a field's text.
void collectNames(const std::string& text, char marker, std::set<std::string>& names)
{
    static const std::regex name{R"([A-Za-z][A-Za-z0-9_]*)"};
    for (std::size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + 1))
    {
        std::smatch match;
        const auto rest = text.substr(at + 1);
        if (std::regex_search(rest, match, name, std::regex_constants::match_continuous))
            names.insert(match.str());
    }
}

void joinInto(std::string& text, std::string_view heading, const std::vector<std::string>& names)
{
    if (names.empty())
        return;
    text += "\n" + std::string{heading} + ": ";
    for (std::size_t index = 0; index < names.size(); ++index)
        text += (index == 0 ? "" : ", ") + names[index];
}
}

std::optional<SoundInformation> soundInformation(
    const talas::project::ProjectDocument& document,
    const talas::project::ProjectGraph& graph,
    const talas::core::NodeRegistry& registry,
    std::string_view nodeId)
{
    const auto node = std::ranges::find(graph.nodes, nodeId, &talas::project::ProjectNode::instanceId);
    if (node == graph.nodes.end())
        return std::nullopt;
    const auto descriptor = talas::project::describeProjectNode(document, *node, registry);
    if (!descriptor)
        return std::nullopt;
    SoundInformation information;
    information.name = node->instanceId;
    information.soundClass = descriptor->displayName;
    std::set<std::string> eventValues;
    std::set<std::string> variables;
    for (const auto& parameter : descriptor->parameters)
    {
        const auto set = std::ranges::find(node->parameters, parameter.id, &talas::project::ProjectParameterExpression::parameterId);
        const auto value = parameterValue(parameter, set == node->parameters.end() ? nullptr : &*set);
        information.parameters.emplace_back(parameter.id, value);
        if (parameter.kind == talas::core::ParameterKind::Expression || parameter.kind == talas::core::ParameterKind::Text)
        {
            collectNames(value, '!', eventValues);
            collectNames(value, '?', variables);
        }
        if (parameter.kind == talas::core::ParameterKind::Resource && !value.empty())
        {
            const auto asset = std::ranges::find(document.assets, value, &talas::project::ProjectAsset::id);
            information.sampleFiles.push_back(asset == document.assets.end() ? value : asset->path);
        }
    }
    for (const auto& port : descriptor->inputs)
    {
        std::vector<std::string> sounds;
        for (const auto& connection : graph.connections)
            if (connection.toNode == nodeId && connection.toPort == port.id)
                sounds.push_back(connection.fromNode);
        if (!sounds.empty())
            information.inputs.emplace_back(port.id, std::move(sounds));
    }
    information.eventValues.assign(eventValues.begin(), eventValues.end());
    information.variables.assign(variables.begin(), variables.end());
    const auto branch = graphBranchOf(graph, nodeId);
    information.complexity = branch.size() + 1;
    // The longest recording it, or a Sound feeding it, plays.
    for (const auto& played : graph.nodes)
    {
        if (played.instanceId != nodeId && !branch.contains(played.instanceId))
            continue;
        const auto longer = [&](std::optional<double> seconds) {
            if (seconds.has_value())
                information.duration = std::max(information.duration.value_or(0.0), *seconds);
        };
        for (const auto& parameter : played.parameters)
        {
            const auto asset = parameter.resource.has_value()
                ? std::ranges::find(document.assets, *parameter.resource, &talas::project::ProjectAsset::id)
                : document.assets.end();
            if (asset != document.assets.end() && asset->kind == talas::project::ProjectAssetKind::Audio && asset->sampleRate > 0.0
                && asset->frameCount > 0)
                longer(static_cast<double>(asset->frameCount) / asset->sampleRate);
        }
        if (played.soundDefinitionId.has_value())
            longer(talas::project::soundDefinitionSampleDuration(document, *played.soundDefinitionId));
    }
    return information;
}

std::string soundInformationText(const SoundInformation& information)
{
    std::string text = information.name + " (" + information.soundClass + ")";
    text += "\nComplexity: " + std::to_string(information.complexity)
        + (information.complexity == 1 ? " Sound" : " Sounds");
    if (information.duration.has_value())
    {
        char duration[64];
        std::snprintf(duration, sizeof duration, "\nDuration: %g s", *information.duration);
        text += duration;
    }
    if (!information.inputs.empty())
    {
        text += "\n\nInputs";
        for (const auto& [port, sounds] : information.inputs)
        {
            text += "\n" + port + ": ";
            for (std::size_t index = 0; index < sounds.size(); ++index)
                text += (index == 0 ? "" : ", ") + sounds[index];
        }
    }
    if (!information.parameters.empty())
    {
        text += "\n\nParameters";
        for (const auto& [parameter, value] : information.parameters)
            text += "\n" + parameter + ": " + value;
    }
    text += "\n";
    joinInto(text, "Event Values", information.eventValues);
    joinInto(text, "Variables", information.variables);
    joinInto(text, "Sample files", information.sampleFiles);
    while (!text.empty() && text.back() == '\n')
        text.pop_back();
    return text;
}
}
