#include "talas/app/SelectedSoundDocument.h"

#include "talas/project/ProjectValidation.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace talas::app
{
std::optional<talas::project::ProjectDocument> documentPlayingSound(
    const talas::project::ProjectDocument& document, const talas::core::NodeRegistry& registry, std::string_view nodeId)
{
    auto playing = document;
    auto& graph = playing.graph;
    const auto output = std::ranges::find(graph.nodes, std::string_view{"talas.audioOutput"}, &talas::project::ProjectNode::typeId);
    const auto selected = std::ranges::find(graph.nodes, nodeId, &talas::project::ProjectNode::instanceId);
    if (output == graph.nodes.end() || selected == graph.nodes.end())
        return std::nullopt;
    if (selected == output)
        return playing;
    const auto descriptor = talas::project::describeProjectNode(playing, *selected, registry);
    if (!descriptor.has_value())
        return std::nullopt;
    std::vector<std::string> audio;
    for (const auto& port : descriptor->outputs)
    {
        if (port.type == talas::core::PortType::Audio)
            audio.push_back(port.id);
    }
    if (audio.empty())
        return std::nullopt;
    const auto outputId = output->instanceId;
    std::erase_if(graph.connections, [&](const talas::core::Connection& line) { return line.toNode == outputId; });
    graph.connections.push_back({std::string{nodeId}, audio.front(), outputId, "audioIn"});
    if (audio.size() > 1)
        graph.connections.push_back({std::string{nodeId}, audio[1], outputId, "rightIn"});
    return playing;
}

std::optional<talas::project::ProjectDocument> documentForControlSpace(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, const std::vector<std::string>& selected, bool inSignalFlow)
{
    if (!inSignalFlow || selected.size() != 1)
        return std::nullopt;
    const auto node = std::ranges::find(document.graph.nodes, selected.front(), &talas::project::ProjectNode::instanceId);
    if (node == document.graph.nodes.end() || node->typeId == "talas.audioOutput")
        return std::nullopt;
    return documentPlayingSound(document, registry, selected.front());
}

std::optional<InfoDisplayDocument> documentWithInfoDisplay(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, std::string_view nodeId, InfoDisplay display)
{
    auto playing = documentPlayingSound(document, registry, nodeId);
    if (!playing.has_value())
        return std::nullopt;
    auto& graph = playing->graph;
    const auto output = std::ranges::find(graph.nodes, std::string_view{"talas.audioOutput"}, &talas::project::ProjectNode::typeId);
    const auto outputId = output->instanceId;
    const auto freeId = [&](std::string base) {
        auto id = base;
        for (int suffix = 2; std::ranges::find(graph.nodes, id, &talas::project::ProjectNode::instanceId) != graph.nodes.end();
             ++suffix)
            id = base + std::to_string(suffix);
        return id;
    };
    const auto displayId = freeId("infoDisplay");
    const auto into = [&](std::string_view port) -> std::optional<talas::core::Connection> {
        const auto found = std::ranges::find_if(graph.connections, [&](const talas::core::Connection& line) {
            return line.toNode == outputId && line.toPort == port;
        });
        return found == graph.connections.end() ? std::nullopt : std::optional{*found};
    };
    const auto left = into("audioIn");
    const auto right = into("rightIn");
    if (!left.has_value())
        return std::nullopt;
    std::erase_if(graph.connections, [&](const talas::core::Connection& line) { return line.toNode == outputId; });
    const auto scope = display == InfoDisplay::Oscilloscope;
    std::string typeId = scope ? "talas.oscilloscopeDisplay" : "talas.spectrumAnalyzerDisplay";
    graph.nodes.push_back({displayId, typeId, {}});
    if (!right.has_value())
    {
        graph.connections.push_back({left->fromNode, left->fromPort, displayId, "audioIn"});
        graph.connections.push_back({displayId, "audioOut", outputId, "audioIn"});
    }
    else
    {
        // A stereo Sound's display shows the mix of its
        // left and right outputs, the Sound heard as it is: the display's
        // own output joins the left at no gain, so that it plays.
        const auto mixId = freeId("infoMix");
        const auto half = [](std::string id) {
            talas::project::ProjectParameterExpression gain;
            gain.parameterId = std::move(id);
            gain.expression = "0.5";
            return gain;
        };
        graph.nodes.push_back({mixId, "talas.mixer", {half("gain1"), half("gain2")}});
        graph.connections.push_back({left->fromNode, left->fromPort, mixId, "in1"});
        graph.connections.push_back({right->fromNode, right->fromPort, mixId, "in2"});
        graph.connections.push_back({mixId, "audioOut", displayId, "audioIn"});
        const auto heardId = freeId("infoHeard");
        talas::project::ProjectParameterExpression silent;
        silent.parameterId = "gain2";
        silent.expression = "0";
        graph.nodes.push_back({heardId, "talas.mixer", {silent}});
        graph.connections.push_back({left->fromNode, left->fromPort, heardId, "in1"});
        graph.connections.push_back({displayId, "audioOut", heardId, "in2"});
        graph.connections.push_back({heardId, "audioOut", outputId, "audioIn"});
        graph.connections.push_back(*right);
    }
    talas::project::reconcileProjectPresentation(*playing);
    return InfoDisplayDocument{std::move(*playing), std::move(displayId), std::move(typeId), std::string{nodeId}};
}

std::optional<InfoDisplayDocument> soundWithInfoDisplay(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, std::string_view definitionId, InfoDisplay display)
{
    if (std::ranges::none_of(document.soundDefinitions, [&](const talas::project::ProjectSoundDefinition& sound) {
            return sound.definitionId == definitionId;
        }))
        return std::nullopt;
    auto withSound = document;
    std::string instanceId = "soundFileSound";
    for (int suffix = 2; std::ranges::find(withSound.graph.nodes, instanceId, &talas::project::ProjectNode::instanceId)
             != withSound.graph.nodes.end();
         ++suffix)
        instanceId = "soundFileSound" + std::to_string(suffix);
    withSound.graph.nodes.push_back({instanceId, {}, {}, std::string{definitionId}});
    return documentWithInfoDisplay(withSound, registry, instanceId, display);
}

namespace
{
void longestSample(const talas::project::ProjectDocument& document, const talas::project::ProjectGraph& graph,
    std::vector<std::string> pending, std::vector<std::string>& definitions, std::optional<double>& longest)
{
    std::vector<std::string> seen;
    while (!pending.empty())
    {
        const auto nodeId = std::move(pending.back());
        pending.pop_back();
        if (std::ranges::find(seen, nodeId) != seen.end())
            continue;
        seen.push_back(nodeId);
        const auto node = std::ranges::find(graph.nodes, nodeId, &talas::project::ProjectNode::instanceId);
        if (node == graph.nodes.end())
            continue;
        for (const auto& parameter : node->parameters)
        {
            if (!parameter.resource.has_value())
                continue;
            const auto asset = std::ranges::find(document.assets, *parameter.resource, &talas::project::ProjectAsset::id);
            if (asset != document.assets.end() && asset->kind == talas::project::ProjectAssetKind::Audio && asset->sampleRate > 0.0
                && asset->frameCount > 0)
                longest = std::max(longest.value_or(0.0), static_cast<double>(asset->frameCount) / asset->sampleRate);
        }
        if (node->soundDefinitionId.has_value() && std::ranges::find(definitions, *node->soundDefinitionId) == definitions.end())
        {
            definitions.push_back(*node->soundDefinitionId);
            const auto inner = std::ranges::find(document.soundDefinitions, *node->soundDefinitionId,
                &talas::project::ProjectSoundDefinition::definitionId);
            if (inner != document.soundDefinitions.end())
            {
                std::vector<std::string> all;
                for (const auto& member : inner->graph.nodes)
                    all.push_back(member.instanceId);
                longestSample(document, inner->graph, std::move(all), definitions, longest);
            }
        }
        for (const auto& line : graph.connections)
        {
            if (line.toNode == nodeId)
                pending.push_back(line.fromNode);
        }
    }
}
}

std::optional<double> selectedSoundSampleSeconds(const talas::project::ProjectDocument& document, std::string_view nodeId)
{
    std::vector<std::string> definitions;
    std::optional<double> longest;
    longestSample(document, document.graph, {std::string{nodeId}}, definitions, longest);
    return longest;
}

namespace
{
[[nodiscard]] bool asks(const talas::project::ProjectNode& node)
{
    if (node.typeId != "talas.genericSource")
        return false;
    const auto source = std::ranges::find(node.parameters, std::string_view{"source"},
        &talas::project::ProjectParameterExpression::parameterId);
    if (source == node.parameters.end() || !source->text.has_value())
        return false;
    auto text = *source->text;
    std::erase_if(text, [](char character) { return character == ' '; });
    std::ranges::transform(text, text.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return text == "ask";
}
}

std::vector<std::string> genericSourcesAsking(const talas::project::ProjectDocument& document)
{
    std::vector<std::string> asking;
    for (const auto& node : document.graph.nodes)
        if (asks(node))
            asking.push_back(node.instanceId);
    for (const auto& definition : document.soundDefinitions)
        for (const auto& node : definition.graph.nodes)
            if (asks(node))
                asking.push_back(definition.definitionId + "." + node.instanceId);
    return asking;
}

talas::project::ProjectDocument documentWithGenericSourceAnswer(talas::project::ProjectDocument document, GenericSourceAnswer answer)
{
    const auto answered = [&](talas::project::ProjectGraph& graph) {
        for (auto& node : graph.nodes)
        {
            if (!asks(node))
                continue;
            for (auto& parameter : node.parameters)
                if (parameter.parameterId == "source")
                    parameter.text = std::string{genericSourceAnswerText(answer)};
        }
    };
    answered(document.graph);
    for (auto& definition : document.soundDefinitions)
        answered(definition.graph);
    return document;
}

std::string_view genericSourceAnswerText(GenericSourceAnswer answer) noexcept
{
    switch (answer)
    {
    case GenericSourceAnswer::Live:
        return "Live";
    case GenericSourceAnswer::Ram:
        return "RAM";
    case GenericSourceAnswer::Disk:
        break;
    }
    return "Disk";
}
}
