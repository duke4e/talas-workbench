#include "app/workbench/WorkbenchComponentInternal.h"

#include <cmath>
#include <numbers>

namespace talas::app::workbench
{
talas::core::Connection connection(
    std::string sourceNode,
    std::string sourcePort,
    std::string targetNode,
    std::string targetPort);

std::optional<std::string> WorkbenchComponent::runParameterTransformerSmoke(
    const std::string& oscillatorId,
    const std::string& vcaId)
{
    constexpr std::string_view transformerId =
        "smoke-parameter-transformer";
    auto edit = [](const talas::project::EditResult& result, std::string action)
        -> std::optional<std::string> {
        if (result.ok())
            return {};
        return action + ": " + result.message;
    };

    talas::project::ProjectNode transformer;
    transformer.instanceId = transformerId;
    transformer.typeId = "talas.parameterTransformer";
    talas::project::ProjectParameterExpression source;
    source.parameterId = "transformation";
    source.text = std::string{};
    transformer.parameters.push_back(std::move(source));
    if (auto error = edit(
            projectBridge_.editor().disconnect(vcaId, "audioIn"),
            "ParameterTransformer smoke input reroute failed"))
    {
        return error;
    }
    if (auto error = edit(
            projectBridge_.editor().addNode(
                std::move(transformer),
                {250, 170}),
            "ParameterTransformer smoke node creation failed"))
    {
        return error;
    }
    if (auto error = edit(
            projectBridge_.editor().connect(connection(
                oscillatorId,
                "audioOut",
                std::string{transformerId},
                "audioIn")),
            "ParameterTransformer smoke source connection failed"))
    {
        return error;
    }
    if (auto error = edit(
            projectBridge_.editor().connect(connection(
                std::string{transformerId},
                "audioOut",
                vcaId,
                "audioIn")),
            "ParameterTransformer smoke output connection failed"))
    {
        return error;
    }
    documentChanged();
    selectNode(std::string{transformerId});
    const auto* transformationEditor = dynamic_cast<juce::TextEditor*>(
        inspectorPanel_->expressionValueComponentForSmoke());
    if (transformationEditor == nullptr
        || !transformationEditor->isMultiLine()
        || !transformationEditor->getReturnKeyStartsNewLine()
        || transformationEditor->getBounds().getHeight() < 72)
    {
        return "ParameterTransformer Inspector must allow one rule per line with Return";
    }
    inspectorPanel_->commitExpressionForSmoke(
        std::string{transformerId},
        "transformation",
        "set frequency = 400 hz\nscale frequency by 2");
    const auto authoredSource = [&]() -> std::optional<std::string> {
        const auto& document = projectBridge_.editor().document();
        const auto found = std::ranges::find(
            document.graph.nodes,
            transformerId,
            &talas::project::ProjectNode::instanceId);
        if (found == document.graph.nodes.end())
            return {};
        const auto parameter = std::ranges::find(
            found->parameters,
            std::string_view{"transformation"},
            &talas::project::ProjectParameterExpression::parameterId);
        return parameter != found->parameters.end()
                && parameter->text.has_value()
            ? parameter->text
            : std::optional<std::string>{};
    };
    const auto acceptedSource = authoredSource();
    const auto canonical = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    const auto reopened = talas::project::parseProjectDocument(canonical);
    const auto reopenedSource = [&]() -> std::optional<std::string> {
        if (!reopened.ok())
            return {};
        const auto node = std::ranges::find(
            reopened.document.graph.nodes,
            transformerId,
            &talas::project::ProjectNode::instanceId);
        if (node == reopened.document.graph.nodes.end())
            return {};
        const auto parameter = std::ranges::find(
            node->parameters,
            std::string_view{"transformation"},
            &talas::project::ProjectParameterExpression::parameterId);
        return parameter != node->parameters.end()
                && parameter->text.has_value()
            ? parameter->text
            : std::optional<std::string>{};
    }();
    if (acceptedSource
            != std::optional<std::string>{
                "set frequency = 400 hz\nscale frequency by 2"}
        || transformationEditor->getText().toStdString() != *acceptedSource
        || !reopened.ok()
        || reopened.document.formatVersion != talas::project::currentProjectFormatVersion
        || reopenedSource != acceptedSource
        || !inspectorPanel_->parameterHintContainsForSmoke(
            "Closed compile-time syntax"))
    {
        return "ParameterTransformer Inspector authoring/help or format-32 source round-trip failed";
    }

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8000.0;
    settings.maxBlockFrames = 16;
    if (!stageProject(settings, "ParameterTransformer smoke"))
    {
        return "ParameterTransformer Inspector-authored graph did not stage: "
            + statusLabel_.getText().toStdString();
    }
    const auto acceptedStage = projectBridge_.latestStageGeneration();
    inspectorPanel_->commitExpressionForSmoke(
        std::string{transformerId},
        "transformation",
        "snd frequency: (400 hz");
    if (authoredSource() != acceptedSource
        || projectBridge_.latestStageGeneration() != acceptedStage)
    {
        return "rejected ParameterTransformer Inspector edit changed authored or active-stage authority";
    }

    std::array<float, 16> output{};
    std::array<float*, 1> channels{output.data()};
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            channels.data(),
            channels.size(),
            output.size()))
    {
        return "ParameterTransformer Inspector-authored stage did not render";
    }
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto expected = 0.5 * std::sin(
            2.0 * std::numbers::pi * 800.0
            * static_cast<double>(frame) / settings.sampleRate);
        if (std::abs(
                static_cast<double>(output[frame]) - expected)
            > 1.0e-5)
        {
            return "ParameterTransformer Workbench render did not use the exact transformed 800 Hz symbolic frequency";
        }
    }
    if (auto error = edit(
            projectBridge_.editor().disconnect(vcaId, "audioIn"),
            "ParameterTransformer smoke cleanup disconnect failed"))
    {
        return error;
    }
    if (auto error = edit(
            projectBridge_.editor().removeNode(transformerId),
            "ParameterTransformer smoke cleanup remove failed"))
    {
        return error;
    }
    if (auto error = edit(
            projectBridge_.editor().connect(connection(
                oscillatorId,
                "audioOut",
                vcaId,
                "audioIn")),
            "ParameterTransformer smoke cleanup reconnect failed"))
    {
        return error;
    }
    documentChanged();
    if (!stageProject(
            std::move(settings),
            "ParameterTransformer smoke cleanup")
        || !projectBridge_.render(
            channels.data(),
            channels.size(),
            output.size()))
    {
        return "ParameterTransformer smoke cleanup graph did not stage and render";
    }
    projectBridge_.setPlaying(false);
    static_cast<void>(projectBridge_.collectRetired());
    return {};
}
}
