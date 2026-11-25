#include "talas/app/SoundBrowserAudition.h"

#include "talas/app/SoundLibraryAuditionModel.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/SoundDefinitionImport.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace talas::app
{
namespace
{
constexpr std::string_view auditionSoundNode = "auditionSound";
constexpr std::string_view inputAssetId = "soundBrowserInput";

[[nodiscard]] SoundBrowserAuditionDocument failure(std::string message)
{
    SoundBrowserAuditionDocument result;
    result.diagnostic = std::move(message);
    return result;
}

[[nodiscard]] const talas::project::ProjectSoundDefinition* definitionIn(
    const talas::project::ProjectDocument& document, std::string_view definitionId)
{
    const auto found = std::ranges::find(
        document.soundDefinitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

[[nodiscard]] const talas::project::ProjectSoundPort* firstAudio(const std::vector<talas::project::ProjectSoundPort>& ports)
{
    const auto found = std::ranges::find(ports, talas::core::PortType::Audio, &talas::project::ProjectSoundPort::type);
    return found == ports.end() ? nullptr : &*found;
}

[[nodiscard]] talas::project::ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

[[nodiscard]] talas::project::ProjectParameterExpression expression(std::string id, std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(text);
    return parameter;
}

[[nodiscard]] std::string seconds(double value)
{
    auto text = std::to_string(value);
    while (text.size() > 1 && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text + " s";
}

[[nodiscard]] std::optional<std::string> invalid(const talas::project::ProjectDocument& document, const talas::core::NodeRegistry& registry)
{
    const auto validation = talas::project::validateProjectDocument(document, registry);
    if (validation.ok())
        return std::nullopt;
    return validation.errors.empty() ? std::string{"The browser's Sound is invalid"} : validation.errors.front().message;
}
}

SoundBrowserAuditionDocument soundBrowserAuditionDocument(
    const talas::project::ProjectDocument& soundFile,
    std::string_view definitionId,
    const SoundBrowserReplacementInput& input,
    const std::optional<talas::project::ProjectDocument>& inputSoundFile,
    const talas::core::NodeRegistry& registry)
{
    const auto* definition = definitionIn(soundFile, definitionId);
    if (definition == nullptr)
        return failure("The Sound is no longer in its Sound file");
    // The replaceable input, else the first Audio input.
    const auto* replaced = talas::project::replaceableSoundInput(*definition);
    const auto replacing = input.connected && replaced != nullptr;

    auto source = soundFile;
    std::vector<talas::project::WorkspaceSoundLibraryInputSubstitution> substitutions;
    double sampleSeconds = 0.0;
    if (replacing && input.kind == SoundBrowserInputKind::Sample)
    {
        talas::project::ProjectAsset manifest;
        manifest.id = std::string{inputAssetId};
        manifest.kind = talas::project::ProjectAssetKind::Audio;
        manifest.path = input.path.string();
        const auto inspected = talas::project::inspectProjectAsset(manifest, input.path);
        if (inspected.error.has_value() || inspected.asset.sampleRate <= 0.0)
            return failure("The replacement sample cannot be read");
        std::erase_if(source.assets, [](const talas::project::ProjectAsset& asset) { return asset.id == inputAssetId; });
        source.assets.push_back(inspected.asset);
        sampleSeconds = static_cast<double>(inspected.asset.frameCount) / inspected.asset.sampleRate;
        substitutions.push_back({replaced->portId, talas::project::WorkspaceSoundLibraryAuditionSource::FrozenAsset, 0.0,
            std::string{inputAssetId}});
    }
    else if (replacing && replaced->replacesSound)
    {
        // A replaceable input is fed only when asked; the node fed is then
        // made the input chosen.
        substitutions.push_back({replaced->portId, talas::project::WorkspaceSoundLibraryAuditionSource::Sine, 440.0, {}});
    }

    SoundLibraryEntry entry;
    entry.stableId = "soundBrowser";
    entry.sourceKind = SoundLibrarySourceKind::ProjectDefinition;
    entry.displayName = definition->displayName.empty() ? definition->definitionId : definition->displayName;
    entry.compatibility = SoundLibraryCompatibility::Ready;
    entry.dependencyHealth = SoundLibraryHealth::Healthy;
    entry.assetHealth = SoundLibraryHealth::Healthy;
    entry.actions = soundLibraryActionMask(SoundLibraryAction::Audition);
    entry.definitionId = definition->definitionId;
    auto prepared = prepareSoundLibraryAudition(source, entry, registry, substitutions);
    if (!prepared.ok())
        return failure(prepared.diagnostic);
    auto audition = std::move(*prepared.document);

    if (replacing)
    {
        auto connection = std::ranges::find_if(audition.graph.connections, [&replaced](const talas::core::Connection& candidate) {
            return candidate.toNode == auditionSoundNode && candidate.toPort == replaced->portId;
        });
        if (connection == audition.graph.connections.end())
            return failure("The Sound's input could not be replaced");
        const auto inputNode = connection->fromNode;
        auto node = std::ranges::find(audition.graph.nodes, inputNode, &talas::project::ProjectNode::instanceId);
        if (node == audition.graph.nodes.end())
            return failure("The Sound's input could not be replaced");
        switch (input.kind)
        {
        case SoundBrowserInputKind::AudioInput:
        {
            // Input N, a live audio input channel.
            const auto channel = std::clamp(input.audioInput, 1U, 8U);
            node->typeId = "talas.audioInput";
            node->soundDefinitionId.reset();
            node->parameters.clear();
            for (unsigned index = 1; index <= 8; ++index)
                node->parameters.push_back(booleanParameter("channel" + std::to_string(index), index == channel));
            connection->fromPort = "audioOut";
            break;
        }
        case SoundBrowserInputKind::Sample:
            // The sample plays through, not for the player's default second.
            node->parameters.push_back(expression("duration", seconds(sampleSeconds)));
            break;
        case SoundBrowserInputKind::Sound:
        {
            if (!inputSoundFile.has_value() || definitionIn(*inputSoundFile, input.definitionId) == nullptr)
                return failure("The replacement Sound is no longer in its Sound file");
            const auto preview = talas::project::suggestSoundDefinitionImportCollisionPlan(
                audition, *inputSoundFile, input.definitionId, registry);
            if (!preview.ok())
                return failure(preview.diagnostic);
            const auto plan = talas::project::planSoundDefinitionImport(audition, *inputSoundFile, *preview.request, registry);
            if (!plan.ok())
                return failure(plan.diagnostic);
            const auto inputNodeId = inputNode;
            audition = plan.plan->document;
            const auto* imported = definitionIn(audition, plan.plan->importedRootDefinitionId);
            const auto* output = imported == nullptr ? nullptr : firstAudio(imported->outputs);
            if (output == nullptr)
                return failure("The replacement Sound has no audio output");
            const auto outputPort = output->portId;
            const auto importedId = plan.plan->importedRootDefinitionId;
            auto replacedNode = std::ranges::find(audition.graph.nodes, inputNodeId, &talas::project::ProjectNode::instanceId);
            auto replacedConnection = std::ranges::find_if(audition.graph.connections, [&](const talas::core::Connection& candidate) {
                return candidate.fromNode == inputNodeId;
            });
            if (replacedNode == audition.graph.nodes.end() || replacedConnection == audition.graph.connections.end())
                return failure("The Sound's input could not be replaced");
            replacedNode->typeId.clear();
            replacedNode->parameters.clear();
            replacedNode->soundDefinitionId = importedId;
            replacedConnection->fromPort = outputPort;
            break;
        }
        }
        talas::project::reconcileProjectPresentation(audition);
    }
    if (const auto error = invalid(audition, registry))
        return failure(*error);
    SoundBrowserAuditionDocument result;
    result.document = std::move(audition);
    result.outputChannelCount = prepared.outputChannelCount;
    return result;
}

SoundBrowserImport soundBrowserImport(
    const talas::project::ProjectDocument& project,
    const talas::project::ProjectDocument& soundFile,
    std::string_view definitionId,
    const SoundBrowserReplacementInput& input,
    const std::optional<talas::project::ProjectDocument>& inputSoundFile,
    const talas::core::NodeRegistry& registry)
{
    SoundBrowserImport result;
    const auto* definition = definitionIn(soundFile, definitionId);
    if (definition == nullptr)
    {
        result.diagnostic = "The Sound is no longer in its Sound file";
        return result;
    }
    auto source = soundFile;
    std::string root{definitionId};
    if (input.connected && talas::project::replaceableSoundInput(*definition) != nullptr)
    {
        // The combination of input and Sound kept as a new Sound.
        const auto audition = soundBrowserAuditionDocument(soundFile, definitionId, input, inputSoundFile, registry);
        if (!audition.ok())
        {
            result.diagnostic = audition.diagnostic;
            return result;
        }
        source = *audition.document;
        talas::project::ProjectSoundDefinition combination;
        combination.definitionId = "browserCombination";
        for (int suffix = 2; definitionIn(source, combination.definitionId) != nullptr; ++suffix)
            combination.definitionId = "browserCombination" + std::to_string(suffix);
        combination.displayName = (definition->displayName.empty() ? definition->definitionId : definition->displayName)
            + " (" + soundBrowserInputText(input) + ")";
        for (const auto& node : source.graph.nodes)
        {
            if (node.typeId != "talas.audioOutput")
                combination.graph.nodes.push_back(node);
        }
        for (const auto& connection : source.graph.connections)
        {
            const auto terminal = std::ranges::find(source.graph.nodes, connection.toNode, &talas::project::ProjectNode::instanceId);
            if (terminal != source.graph.nodes.end() && terminal->typeId == "talas.audioOutput")
            {
                combination.outputs.push_back({"audioOut" + (combination.outputs.empty() ? std::string{} : std::to_string(combination.outputs.size() + 1)),
                    talas::core::PortType::Audio, connection.fromNode, connection.fromPort});
                continue;
            }
            combination.graph.connections.push_back(connection);
        }
        source.graph = {};
        source.soundDefinitions.push_back(std::move(combination));
        root = source.soundDefinitions.back().definitionId;
        talas::project::reconcileProjectPresentation(source);
    }
    const auto preview = talas::project::suggestSoundDefinitionImportCollisionPlan(project, source, root, registry);
    if (!preview.ok())
    {
        result.diagnostic = preview.diagnostic;
        return result;
    }
    const auto plan = talas::project::planSoundDefinitionImport(project, source, *preview.request, registry);
    if (!plan.ok())
    {
        result.diagnostic = plan.diagnostic;
        return result;
    }
    result.document = plan.plan->document;
    result.rootDefinitionId = plan.plan->importedRootDefinitionId;
    return result;
}
}
