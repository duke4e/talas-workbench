#include "talas/app/SoundLibraryAuditionModel.h"

#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/SoundDefinitionImport.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <unordered_set>

namespace talas::app
{
namespace
{
[[nodiscard]] SoundLibraryAuditionPreparation failure(std::string message)
{
    SoundLibraryAuditionPreparation result;
    result.diagnostic = std::move(message);
    return result;
}

[[nodiscard]] const talas::project::ProjectSoundDefinition* findDefinition(
    const talas::project::ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find(
        document.soundDefinitions,
        definitionId,
        &talas::project::ProjectSoundDefinition::definitionId);
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

[[nodiscard]] talas::project::ProjectParameterExpression expressionParameter(
    std::string id,
    std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    return parameter;
}

[[nodiscard]] talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string resource)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
    return parameter;
}

void appendNode(
    talas::project::ProjectDocument& document,
    talas::project::ProjectNode node,
    talas::project::CanvasPosition position)
{
    const auto nodeId = node.instanceId;
    document.graph.nodes.push_back(std::move(node));
    talas::project::reconcileProjectPresentation(document);
    if (auto* presentation = talas::project::findProjectNodePresentation(
            document, std::nullopt, nodeId))
    {
        presentation->position = position;
    }
}

[[nodiscard]] const talas::project::WorkspaceSoundLibraryInputSubstitution*
findSubstitution(
    const std::vector<talas::project::WorkspaceSoundLibraryInputSubstitution>& values,
    std::string_view portId)
{
    const auto found = std::ranges::find(
        values,
        portId,
        &talas::project::WorkspaceSoundLibraryInputSubstitution::portId);
    return found == values.end() ? nullptr : &*found;
}

[[nodiscard]] std::string numberExpression(double value)
{
    auto text = std::to_string(value);
    while (text.size() > 1 && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

[[nodiscard]] bool validSubstitution(
    const talas::project::WorkspaceSoundLibraryInputSubstitution& substitution)
{
    if (substitution.portId.empty()
        || substitution.portId.size()
            > talas::project::maximumWorkspaceSoundLibraryPortIdBytes
        || !std::isfinite(substitution.value))
    {
        return false;
    }
    const auto hasAsset = substitution.frozenAssetId.has_value();
    if (hasAsset
        && (substitution.frozenAssetId->empty()
            || substitution.frozenAssetId->size()
                > talas::project::maximumWorkspaceSoundLibraryAssetIdBytes))
    {
        return false;
    }
    switch (substitution.source)
    {
    case talas::project::WorkspaceSoundLibraryAuditionSource::Default:
    case talas::project::WorkspaceSoundLibraryAuditionSource::Silence:
        return substitution.value == 0.0 && !hasAsset;
    case talas::project::WorkspaceSoundLibraryAuditionSource::Sine:
        return substitution.value
                >= talas::project::minimumWorkspaceSoundLibrarySineFrequencyHz
            && substitution.value
                <= talas::project::maximumWorkspaceSoundLibrarySineFrequencyHz
            && !hasAsset;
    case talas::project::WorkspaceSoundLibraryAuditionSource::Constant:
        return std::abs(substitution.value)
                <= talas::project::maximumWorkspaceSoundLibraryConstantMagnitude
            && !hasAsset;
    case talas::project::WorkspaceSoundLibraryAuditionSource::FrozenAsset:
        return substitution.value == 0.0 && hasAsset;
    }
    return false;
}

}

SoundLibraryAuditionPreparation prepareSoundLibraryAudition(
    const talas::project::ProjectDocument& source,
    const SoundLibraryEntry& entry,
    const talas::core::NodeRegistry& registry,
    const std::vector<talas::project::WorkspaceSoundLibraryInputSubstitution>&
        substitutions)
{
    if (entry.stableId.empty()
        || entry.compatibility != SoundLibraryCompatibility::Ready
        || !soundLibraryHasAction(entry.actions, SoundLibraryAction::Audition))
    {
        return failure(entry.actionDiagnostic.empty()
                ? "Selected Sound Library entry is not auditionable"
                : entry.actionDiagnostic);
    }
    if (substitutions.size() > maximumSoundLibraryAuditionInputs)
        return failure("Sound Library audition input plan exceeds its bound");
    std::unordered_set<std::string> substitutionIds;
    substitutionIds.reserve(substitutions.size());
    for (const auto& substitution : substitutions)
    {
        if (!validSubstitution(substitution)
            || !substitutionIds.emplace(substitution.portId).second)
        {
            return failure("Sound Library audition substitution is invalid or duplicate");
        }
    }

    if (entry.sourceKind == SoundLibrarySourceKind::Component)
    {
        return failure(
            "Component audition requires a separately authorized private component host");
    }

    if (entry.sourceKind == SoundLibrarySourceKind::OriginalExample
        && !entry.definitionId.has_value())
    {
        if (!substitutions.empty())
            return failure("Whole-example audition has no replaceable Sound inputs");
        const auto structural = talas::project::validateProjectDocument(source, registry);
        const auto available =
            talas::project::validateProjectComponentAvailabilityForExecution(
                source, registry);
        if (!structural.ok() || !available.ok())
            return failure("Original example is not executable in the private runtime");
        const auto outputs = soundLibraryProjectOutputChannelCount(source);
        if ((outputs == 0
                && !soundLibraryProjectHasBoundedTerminalEvents(source))
            || outputs > maximumSoundLibraryAuditionOutputs)
        {
            return failure(
                "Original example has no bounded Audio or terminal-event output profile");
        }
        SoundLibraryAuditionPreparation result;
        result.document = source;
        result.stableEntryId = entry.stableId;
        result.sourceLabel = entry.displayName;
        result.outputChannelCount = outputs;
        return result;
    }

    if (!entry.definitionId.has_value())
        return failure("Sound Library entry has no exact reusable Sound identity");
    const auto* definition = findDefinition(source, *entry.definitionId);
    if (definition == nullptr)
        return failure("Reusable Sound identity is absent from the exact source");
    const auto profile = soundLibraryDefinitionAuditionProfile(*definition);
    if (!profile.ok())
        return failure(profile.diagnostic);

    talas::project::SoundDefinitionImportRequest closureRequest;
    closureRequest.rootDefinitionId = definition->definitionId;
    auto closure = talas::project::planSoundDefinitionImport(
        {}, source, closureRequest, registry);
    if (!closure.ok())
    {
        return failure(closure.diagnostic.empty()
                ? "Reusable Sound audition closure is invalid"
                : closure.diagnostic);
    }
    talas::project::ProjectDocument audition =
        std::move(closure.plan->document);
    audition.metadata.title = entry.displayName + " Private Audition";
    audition.graph = {};
    audition.timeline = {};
    audition.virtualControls.clear();
    appendNode(
        audition,
        {"auditionSound", {}, {}, definition->definitionId},
        {300, 100});

    SoundLibraryAuditionPreparation result;
    result.stableEntryId = entry.stableId;
    result.sourceLabel = entry.displayName;
    result.inputs.reserve(definition->inputs.size());
    std::unordered_set<std::string> consumedSubstitutions;
    consumedSubstitutions.reserve(substitutions.size());
    std::size_t inputOrdinal = 0;
    for (const auto& input : definition->inputs)
    {
        ++inputOrdinal;
        const auto* requested = findSubstitution(substitutions, input.portId);
        const auto sourceKind = requested == nullptr
            ? talas::project::WorkspaceSoundLibraryAuditionSource::Default
            : requested->source;
        const auto value = requested == nullptr ? 0.0 : requested->value;
        const auto frozenAssetId = requested == nullptr
            ? std::optional<std::string>{}
            : requested->frozenAssetId;
        if (requested != nullptr)
            consumedSubstitutions.emplace(input.portId);

        SoundLibraryAuditionInput resolved{
            input.portId, input.type, sourceKind, value, frozenAssetId};
        if (input.replacesSound && requested == nullptr)
        {
            // An unconnected replaceable input
            // plays the Sound it would replace.
            result.inputs.push_back(std::move(resolved));
            continue;
        }
        const auto nodeId = "auditionInput" + std::to_string(inputOrdinal);
        talas::project::ProjectNode sourceNode;
        sourceNode.instanceId = nodeId;
        std::string outputPort;
        if (input.type == talas::core::PortType::Audio)
        {
            if (sourceKind == talas::project::WorkspaceSoundLibraryAuditionSource::Default
                || sourceKind == talas::project::WorkspaceSoundLibraryAuditionSource::Sine
                || sourceKind == talas::project::WorkspaceSoundLibraryAuditionSource::Silence)
            {
                const auto frequency = sourceKind
                        == talas::project::WorkspaceSoundLibraryAuditionSource::Sine
                    ? value
                    : 440.0;
                if (!std::isfinite(frequency)
                    || frequency
                        < talas::project::minimumWorkspaceSoundLibrarySineFrequencyHz
                    || frequency
                        > talas::project::maximumWorkspaceSoundLibrarySineFrequencyHz)
                {
                    return failure("Audio sine substitution is outside its closed range");
                }
                sourceNode.typeId = "talas.oscillator";
                sourceNode.parameters = {
                    expressionParameter("frequency", numberExpression(frequency) + " hz"),
                    expressionParameter(
                        "amplitude",
                        sourceKind
                                == talas::project::WorkspaceSoundLibraryAuditionSource::Silence
                            ? "0"
                            : "0.2"),
                };
                outputPort = "audioOut";
            }
            else if (sourceKind
                == talas::project::WorkspaceSoundLibraryAuditionSource::FrozenAsset)
            {
                if (!frozenAssetId.has_value())
                    return failure("Frozen Audio substitution has no exact asset identity");
                const auto asset = std::ranges::find(
                    source.assets,
                    *frozenAssetId,
                    &talas::project::ProjectAsset::id);
                if (asset == source.assets.end()
                    || asset->kind != talas::project::ProjectAssetKind::Audio
                    || !talas::project::isCanonicalProjectAssetContentHash(
                        asset->contentHash))
                {
                    return failure("Frozen Audio substitution asset is unavailable or unverified");
                }
                const auto retainedAsset = std::ranges::find(
                    audition.assets,
                    *frozenAssetId,
                    &talas::project::ProjectAsset::id);
                if (retainedAsset == audition.assets.end())
                    audition.assets.push_back(*asset);
                else if (*retainedAsset != *asset)
                    return failure("Frozen Audio substitution asset identity conflicts with the selected Sound closure");
                sourceNode.typeId = "talas.diskPlayer";
                sourceNode.parameters = {
                    resourceParameter("fileName", *frozenAssetId),
                    expressionParameter("trigger", "1"),
                };
                outputPort = "audioOut";
            }
            else
            {
                return failure("Audio input substitution source is incompatible");
            }
        }
        else if (input.type == talas::core::PortType::Control)
        {
            const auto constant = sourceKind
                    == talas::project::WorkspaceSoundLibraryAuditionSource::Default
                ? 0.5
                : value;
            if ((sourceKind
                    != talas::project::WorkspaceSoundLibraryAuditionSource::Default
                 && sourceKind
                    != talas::project::WorkspaceSoundLibraryAuditionSource::Constant)
                || !std::isfinite(constant)
                || std::abs(constant)
                    > talas::project::maximumWorkspaceSoundLibraryConstantMagnitude)
            {
                return failure("Control input substitution source is incompatible");
            }
            sourceNode.typeId = "talas.constant";
            sourceNode.parameters = {
                expressionParameter("value", numberExpression(constant)),
            };
            outputPort = "value";
        }
        else
        {
            return failure(
                "Event, NoteStream, resource, and spectral audition inputs are unsupported");
        }
        appendNode(
            audition,
            std::move(sourceNode),
            {40, static_cast<std::int32_t>(60 + inputOrdinal * 100)});
        audition.graph.connections.push_back(
            {nodeId, outputPort, "auditionSound", input.portId});
        result.inputs.push_back(std::move(resolved));
    }
    if (consumedSubstitutions.size() != substitutions.size())
        return failure("Audition substitution targets an absent input port");

    std::size_t outputOrdinal = 0;
    for (const auto& output : definition->outputs)
    {
        if (output.type != talas::core::PortType::Audio)
            continue;
        ++outputOrdinal;
        const auto nodeId = "auditionOutput" + std::to_string(outputOrdinal);
        appendNode(
            audition,
            {nodeId, "talas.audioOutput", {}, {}},
            {560, static_cast<std::int32_t>(60 + outputOrdinal * 100)});
        audition.graph.connections.push_back(
            {"auditionSound", output.portId, nodeId, "audioIn"});
    }
    if (outputOrdinal != profile.outputChannelCount)
        return failure("Reusable Sound audition output profile changed during preparation");

    const auto structural = talas::project::validateProjectDocument(audition, registry);
    const auto available =
        talas::project::validateProjectComponentAvailabilityForExecution(
            audition, registry);
    if (!structural.ok() || !available.ok())
    {
        const auto message = !structural.errors.empty()
            ? structural.errors.front().message
            : !available.errors.empty()
                ? available.errors.front().message
                : std::string{"Private audition document is invalid"};
        return failure(message);
    }
    result.document = std::move(audition);
    result.outputChannelCount = outputOrdinal;
    return result;
}

bool reconcileSoundLibraryAuditionSubstitutions(
    talas::project::WorkspaceSoundLibraryState& state,
    const std::vector<SoundLibraryEntry>& entries)
{
    if (!state.selectedEntryId.has_value())
    {
        const auto changed = !state.inputSubstitutions.empty();
        state.inputSubstitutions.clear();
        return changed;
    }
    const auto entry = std::ranges::find(
        entries,
        *state.selectedEntryId,
        &SoundLibraryEntry::stableId);
    if (entry == entries.end())
    {
        const auto changed = !state.inputSubstitutions.empty();
        state.inputSubstitutions.clear();
        return changed;
    }
    const auto retainedSize = state.inputSubstitutions.size();
    std::erase_if(state.inputSubstitutions, [&](const auto& substitution) {
        const auto port = std::ranges::find(
            entry->auditionInputs,
            substitution.portId,
            &talas::project::ProjectSoundPort::portId);
        if (port == entry->auditionInputs.end())
            return true;
        const auto sourceFits = port->type == talas::core::PortType::Audio
            ? substitution.source
                    == talas::project::WorkspaceSoundLibraryAuditionSource::Default
                || substitution.source
                    == talas::project::WorkspaceSoundLibraryAuditionSource::Silence
                || substitution.source
                    == talas::project::WorkspaceSoundLibraryAuditionSource::Sine
                || substitution.source
                    == talas::project::WorkspaceSoundLibraryAuditionSource::FrozenAsset
            : port->type == talas::core::PortType::Control
                && (substitution.source
                        == talas::project::WorkspaceSoundLibraryAuditionSource::Default
                    || substitution.source
                        == talas::project::WorkspaceSoundLibraryAuditionSource::Constant);
        if (!sourceFits)
            return true;
        return substitution.source
                    == talas::project::WorkspaceSoundLibraryAuditionSource::FrozenAsset
            && (!substitution.frozenAssetId.has_value()
                || std::ranges::find(
                       entry->auditionAudioAssetIds,
                       *substitution.frozenAssetId)
                    == entry->auditionAudioAssetIds.end());
    });
    return retainedSize != state.inputSubstitutions.size();
}
}
