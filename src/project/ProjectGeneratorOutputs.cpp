#include "talas/project/ProjectGeneratorOutputs.h"

#include "JsonValue.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
[[nodiscard]] bool validDomain(talas::expr::ValueDomain domain) noexcept
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
    case talas::expr::ValueDomain::Hertz:
    case talas::expr::ValueDomain::Seconds:
    case talas::expr::ValueDomain::NoteNumber:
    case talas::expr::ValueDomain::Samples:
        return true;
    }
    return false;
}

[[nodiscard]] bool validInterpolation(
    talas::core::AutomationInterpolation interpolation) noexcept
{
    return interpolation == talas::core::AutomationInterpolation::Step
        || interpolation == talas::core::AutomationInterpolation::Linear;
}

[[nodiscard]] bool validEventName(std::string_view value) noexcept
{
    const auto validStart = [](char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || character == '_';
    };
    return !value.empty()
        && value.size() <= maximumScoreIdentifierBytes
        && validStart(value.front())
        && std::ranges::all_of(
            value.substr(1),
            [&](char character) {
                return validStart(character)
                    || (character >= '0' && character <= '9');
            });
}

[[nodiscard]] bool validBoundedText(
    std::string_view value,
    std::size_t maximumBytes) noexcept
{
    if (value.size() > maximumBytes || !json::isValidUtf8(value))
        return false;
    return std::ranges::none_of(
        value,
        [](unsigned char character) {
            return character < 0x20U || character == 0x7fU;
        });
}

[[nodiscard]] bool checkedMultiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept;

[[nodiscard]] bool validScoreClipAction(
    const ProjectGeneratorScoreClipAction& action) noexcept
{
    std::uint64_t totalDuration = 0;
    return isValidScoreIdentifier(action.trackId)
        && isValidScoreIdentifier(action.soundDefinitionId)
        && action.durationTicks != 0
        && action.repeatCount != 0
        && action.repeatCount <= maximumScoreClipRepeats
        && checkedMultiply(
            action.durationTicks,
            action.repeatCount,
            totalDuration)
        && action.fadeInTicks <= totalDuration
        && action.fadeOutTicks <= totalDuration - action.fadeInTicks;
}

[[nodiscard]] bool validActionShape(
    const ProjectGeneratorOutputAction& action) noexcept
{
    return std::visit(
        [](const auto& typed) {
            using Action = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<
                              Action,
                              ProjectGeneratorTimelineEventAction>)
            {
                return validEventName(typed.eventName)
                    && validDomain(typed.domain)
                    && std::isfinite(typed.value)
                    && validInterpolation(typed.interpolation);
            }
            else if constexpr (std::is_same_v<
                                   Action,
                                   ProjectGeneratorScoreClipAction>)
            {
                return validScoreClipAction(typed);
            }
            else if constexpr (std::is_same_v<
                                   Action,
                                   ProjectGeneratorScoreAutomationAction>)
            {
                return isValidScoreIdentifier(typed.laneId)
                    && validDomain(typed.domain)
                    && std::isfinite(typed.value)
                    && validInterpolation(typed.interpolation);
            }
            else if constexpr (std::is_same_v<
                                   Action,
                                   ProjectGeneratorScoreMarkerAction>)
            {
                return !typed.title.empty()
                    && validBoundedText(
                        typed.title,
                        maximumScoreMarkerTitleBytes)
                    && (!typed.durationTicks.has_value()
                        || *typed.durationTicks != 0);
            }
            else
            {
                return !typed.nodeTypeId.empty()
                    && validBoundedText(
                        typed.nodeTypeId,
                        maximumProjectGeneratorNodeTypeIdBytes);
            }
        },
        action);
}

[[nodiscard]] bool validBindingShape(
    const ProjectGeneratorTerminalBinding& binding) noexcept
{
    if (!talas::generative::isValidContextFreeIdentifier(binding.terminal)
        || binding.actions.size()
            > maximumProjectGeneratorActionsPerTerminal)
    {
        return false;
    }
    for (std::size_t actionIndex = 0;
         actionIndex < binding.actions.size();
         ++actionIndex)
    {
        const auto& action = binding.actions[actionIndex];
        if (!validActionShape(action))
            return false;
        for (std::size_t priorIndex = 0;
             priorIndex < actionIndex;
             ++priorIndex)
        {
            const auto& prior = binding.actions[priorIndex];
            const auto* event = std::get_if<
                ProjectGeneratorTimelineEventAction>(&action);
            const auto* priorEvent = std::get_if<
                ProjectGeneratorTimelineEventAction>(&prior);
            if (event != nullptr && priorEvent != nullptr
                && event->eventName == priorEvent->eventName)
            {
                return false;
            }
            const auto* automation = std::get_if<
                ProjectGeneratorScoreAutomationAction>(&action);
            const auto* priorAutomation = std::get_if<
                ProjectGeneratorScoreAutomationAction>(&prior);
            if (automation != nullptr && priorAutomation != nullptr
                && automation->laneId == priorAutomation->laneId)
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool generatorTargetModelWithinBounds(
    const ProjectDocument& document,
    std::size_t additionalRecords = 0) noexcept
{
    if (document.generatorLibrary.programs.size()
            > talas::generative::maximumContextFreePrograms
        || document.virtualControls.size()
            > maximumProjectGeneratorTargetRecords
        || document.timeline.eventValues.size()
            > maximumProjectGeneratorTargetRecords
        || document.soundDefinitions.size()
            > maximumProjectGeneratorTargetRecords
        || document.timeline.score.tracks.size() > maximumScoreTracks
        || document.timeline.score.clips.size() > maximumScoreClips
        || document.timeline.score.automation.size()
            > maximumScoreAutomationLanes
        || document.timeline.score.markers.size() > maximumScoreMarkers)
    {
        return false;
    }
    std::size_t inspectedRecords = 0;
    const auto inspect = [&inspectedRecords](std::size_t count) {
        if (count
            > maximumProjectGeneratorTargetRecords
                - inspectedRecords)
        {
            return false;
        }
        inspectedRecords += count;
        return true;
    };
    if (!inspect(additionalRecords)
        || !inspect(document.generatorLibrary.programs.size())
        || !inspect(document.virtualControls.size())
        || !inspect(document.timeline.eventValues.size())
        || !inspect(document.soundDefinitions.size())
        || !inspect(document.timeline.score.tracks.size())
        || !inspect(document.timeline.score.clips.size())
        || !inspect(document.timeline.score.automation.size())
        || !inspect(document.timeline.score.markers.size()))
    {
        return false;
    }
    std::size_t automationPoints = 0;
    for (const auto& lane : document.timeline.score.automation)
    {
        if (lane.points.size()
                > maximumScoreAutomationPoints - automationPoints
            || !inspect(lane.points.size()))
        {
            return false;
        }
        automationPoints += lane.points.size();
    }
    for (const auto& program : document.generatorLibrary.programs)
    {
        if (program.output.bindings.size()
                > maximumProjectGeneratorTerminalBindings
            || !inspect(program.output.bindings.size()))
        {
            return false;
        }
        for (const auto& binding : program.output.bindings)
        {
            if (binding.actions.size()
                    > maximumProjectGeneratorActionsPerTerminal
                || !inspect(binding.actions.size()))
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] std::optional<ScoreTimeDomain> automationOwnerDomain(
    const ProjectScore& score,
    const ScoreAutomationLane& lane) noexcept
{
    if (lane.scope == ScoreAutomationScope::Clip)
    {
        const auto clip = std::ranges::find(
            score.clips, lane.ownerId, &ScoreClip::clipId);
        if (clip == score.clips.end())
            return std::nullopt;
        const auto track = std::ranges::find(
            score.tracks, clip->trackId, &ScoreTrack::trackId);
        if (track == score.tracks.end()
            || track->kind != ScoreTrackKind::Track)
        {
            return std::nullopt;
        }
        return track->timeDomain;
    }
    const auto track = std::ranges::find(
        score.tracks, lane.ownerId, &ScoreTrack::trackId);
    if (track == score.tracks.end())
        return std::nullopt;
    if ((lane.scope == ScoreAutomationScope::Track
            && track->kind == ScoreTrackKind::Master)
        || (lane.scope == ScoreAutomationScope::Master
            && track->kind != ScoreTrackKind::Master))
    {
        return std::nullopt;
    }
    return track->timeDomain;
}

[[nodiscard]] bool validPreparedOutput(
    const PreparedProjectGeneratorOutputProfile& output,
    const talas::generative::PreparedContextFreeGenerator& generator) noexcept
{
    if (output.timelineStepMicroseconds == 0
        || output.timelineStepMicroseconds
            > maximumProjectGeneratorTimelineMicroseconds
        || output.scoreStepTicks == 0
        || output.bindingBySymbol.size() != generator.symbols.size()
        || generator.terminalFlags.size() != generator.symbols.size()
        || output.bindings.size()
            > maximumProjectGeneratorTerminalBindings)
    {
        return false;
    }
    std::array<std::uint8_t, maximumProjectGeneratorTerminalBindings>
        bindingReferences{};
    for (std::size_t bindingIndex = 0;
         bindingIndex < output.bindings.size();
         ++bindingIndex)
    {
        if (!validBindingShape(output.bindings[bindingIndex]))
            return false;
    }
    for (std::size_t symbolIndex = 0;
         symbolIndex < output.bindingBySymbol.size();
         ++symbolIndex)
    {
        const auto bindingIndex = output.bindingBySymbol[symbolIndex];
        if (bindingIndex == -1)
            continue;
        if (bindingIndex < 0
            || static_cast<std::size_t>(bindingIndex)
                >= output.bindings.size()
            || generator.terminalFlags[symbolIndex] == 0
            || output.bindings[static_cast<std::size_t>(bindingIndex)].terminal
                != generator.symbols[symbolIndex])
        {
            return false;
        }
        ++bindingReferences[static_cast<std::size_t>(bindingIndex)];
    }
    for (std::size_t bindingIndex = 0;
         bindingIndex < output.bindings.size();
         ++bindingIndex)
    {
        if (bindingReferences[bindingIndex] != 1)
            return false;
    }
    return true;
}

[[nodiscard]] bool checkedAdd(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    result = left + right;
    return true;
}

[[nodiscard]] bool checkedMultiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept
{
    if (left != 0
        && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] std::optional<std::uint64_t> positionAt(
    std::uint64_t base,
    std::uint64_t step,
    std::size_t ordinal) noexcept
{
    std::uint64_t offset = 0;
    std::uint64_t position = 0;
    if (!checkedMultiply(
            step,
            static_cast<std::uint64_t>(ordinal),
            offset)
        || !checkedAdd(base, offset, position))
    {
        return std::nullopt;
    }
    return position;
}

[[nodiscard]] ProjectGeneratorOutputValidationError outputError(
    ProjectGeneratorOutputValidationCode code,
    std::size_t bindingIndex,
    std::size_t actionIndex,
    std::string message)
{
    return {
        code,
        bindingIndex,
        actionIndex,
        std::move(message),
    };
}

[[nodiscard]] ProjectGeneratorProjectionStatus generationFailureStatus(
    talas::generative::ContextFreeGenerateStatus status) noexcept
{
    using GenerateStatus = talas::generative::ContextFreeGenerateStatus;
    switch (status)
    {
    case GenerateStatus::Cancelled:
        return ProjectGeneratorProjectionStatus::Cancelled;
    case GenerateStatus::InsufficientScratch:
    case GenerateStatus::AliasedScratch:
        return ProjectGeneratorProjectionStatus::CapacityExceeded;
    case GenerateStatus::Succeeded:
        return ProjectGeneratorProjectionStatus::Succeeded;
    case GenerateStatus::InvalidPreparedProgram:
    case GenerateStatus::RewriteLimitReached:
    case GenerateStatus::OutputLimitExceeded:
    case GenerateStatus::OperationLimitExceeded:
        return ProjectGeneratorProjectionStatus::GenerationFailed;
    }
    return ProjectGeneratorProjectionStatus::GenerationFailed;
}

[[nodiscard]] ProjectGeneratorProjectionStatus scoreFailureStatus(
    ScoreValidationCode code) noexcept
{
    switch (code)
    {
    case ScoreValidationCode::DuplicateIdentifier:
        return ProjectGeneratorProjectionStatus::IdentityCollision;
    case ScoreValidationCode::TimeOverflow:
        return ProjectGeneratorProjectionStatus::TimeOverflow;
    case ScoreValidationCode::TrackLimit:
    case ScoreValidationCode::ClipLimit:
    case ScoreValidationCode::AutomationLaneLimit:
    case ScoreValidationCode::AutomationPointLimit:
    case ScoreValidationCode::MarkerLimit:
    case ScoreValidationCode::EditBatchLimit:
        return ProjectGeneratorProjectionStatus::CapacityExceeded;
    default:
        return ProjectGeneratorProjectionStatus::InvalidTarget;
    }
}

template <typename Result>
void fail(
    Result& result,
    ProjectGeneratorProjectionStatus status,
    std::string message)
{
    result.status = status;
    result.message = std::move(message);
}

[[nodiscard]] const ProjectGeneratorTerminalBinding* bindingForSymbol(
    const PreparedProjectGeneratorOutputProfile& output,
    std::size_t symbolIndex) noexcept
{
    if (symbolIndex >= output.bindingBySymbol.size())
        return nullptr;
    const auto bindingIndex = output.bindingBySymbol[symbolIndex];
    if (bindingIndex < 0
        || static_cast<std::size_t>(bindingIndex) >= output.bindings.size())
    {
        return nullptr;
    }
    return &output.bindings[static_cast<std::size_t>(bindingIndex)];
}

[[nodiscard]] std::string generatedIdentity(
    std::string_view outputNamespace,
    char kind,
    std::size_t ordinal,
    std::size_t actionIndex)
{
    return std::string{outputNamespace} + "_" + kind
        + std::to_string(ordinal) + "_"
        + std::to_string(actionIndex);
}

[[nodiscard]] bool validProjectIdentifier(
    std::string_view value,
    std::size_t maximumBytes) noexcept
{
    const auto validStart = [](char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || character == '_';
    };
    return !value.empty() && value.size() <= maximumBytes
        && validStart(value.front())
        && std::ranges::all_of(
            value.substr(1),
            [&](char character) {
                return validStart(character)
                    || (character >= '0' && character <= '9');
            });
}

[[nodiscard]] std::string generatedNodeIdentity(
    std::string_view outputNamespace,
    std::size_t ordinal)
{
    return std::string{outputNamespace} + "_n" + std::to_string(ordinal);
}

struct CatalogAuthorityFingerprint
{
    static constexpr std::size_t maximumRecords =
        maximumProjectGeneratorTargetRecords;
    static constexpr std::size_t maximumBytes = 1024U * 1024U;

    std::uint64_t value = 1469598103934665603ULL;
    std::size_t records = 0;
    std::size_t bytes = 0;
    bool valid = true;

    void appendByte(std::uint8_t byte) noexcept
    {
        value ^= byte;
        value *= 1099511628211ULL;
    }

    void appendInteger(std::uint64_t integer) noexcept
    {
        for (unsigned shift = 0; shift < 64U; shift += 8U)
            appendByte(static_cast<std::uint8_t>(integer >> shift));
    }

    template <typename Enum>
    void appendEnum(Enum valueToAppend) noexcept
    {
        appendInteger(static_cast<std::uint64_t>(valueToAppend));
    }

    void appendBool(bool valueToAppend) noexcept
    {
        appendByte(valueToAppend ? 1U : 0U);
    }

    void appendString(std::string_view text) noexcept
    {
        if (!valid || text.size() > maximumBytes - bytes)
        {
            valid = false;
            return;
        }
        bytes += text.size();
        appendInteger(text.size());
        for (const auto character : text)
            appendByte(static_cast<std::uint8_t>(character));
    }

    void appendRecord() noexcept
    {
        if (!valid || records == maximumRecords)
        {
            valid = false;
            return;
        }
        ++records;
    }

    [[nodiscard]] bool canAppendRecords(
        std::size_t count) noexcept
    {
        if (!valid || count > maximumRecords - records)
        {
            valid = false;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool appendRecords(std::size_t count) noexcept
    {
        if (!canAppendRecords(count))
            return false;
        records += count;
        return true;
    }
};

void appendCatalogAuthority(
    CatalogAuthorityFingerprint& fingerprint,
    const talas::core::PortSignalContract& signal) noexcept
{
    fingerprint.appendEnum(signal.rate);
    fingerprint.appendEnum(signal.audioLayout);
    fingerprint.appendEnum(signal.edge);
    fingerprint.appendEnum(signal.spectralFormat);
    fingerprint.appendBool(signal.timelineBearing);
}

void appendCatalogAuthority(
    CatalogAuthorityFingerprint& fingerprint,
    const talas::core::PortDescriptor& port) noexcept
{
    fingerprint.appendRecord();
    fingerprint.appendString(port.id);
    fingerprint.appendEnum(port.type);
    fingerprint.appendBool(port.required);
    appendCatalogAuthority(fingerprint, port.signal);
    fingerprint.appendBool(port.compilerPopulated);
}

void appendCatalogAuthority(
    CatalogAuthorityFingerprint& fingerprint,
    const talas::core::ParameterDescriptor& parameter) noexcept
{
    fingerprint.appendRecord();
    fingerprint.appendString(parameter.id);
    fingerprint.appendString(parameter.defaultExpression);
    fingerprint.appendEnum(parameter.domain);
    fingerprint.appendEnum(parameter.kind);
    fingerprint.appendString(parameter.defaultChoice);
    fingerprint.appendInteger(parameter.choices.size());
    if (!fingerprint.appendRecords(parameter.choices.size()))
        return;
    for (const auto& choice : parameter.choices)
        fingerprint.appendString(choice);
    fingerprint.appendBool(parameter.defaultBoolean.has_value());
    if (parameter.defaultBoolean.has_value())
        fingerprint.appendBool(*parameter.defaultBoolean);
    fingerprint.appendString(parameter.defaultResource);
    fingerprint.appendString(parameter.defaultText);
}

void appendCatalogAuthority(
    CatalogAuthorityFingerprint& fingerprint,
    const talas::core::NodeTypeDescriptor& descriptor) noexcept
{
    fingerprint.appendRecord();
    fingerprint.appendString(descriptor.typeId);
    fingerprint.appendString(descriptor.displayName);
    fingerprint.appendString(descriptor.category);
    if (!fingerprint.valid)
        return;
    fingerprint.appendInteger(descriptor.inputs.size());
    if (!fingerprint.canAppendRecords(descriptor.inputs.size()))
        return;
    for (const auto& port : descriptor.inputs)
        appendCatalogAuthority(fingerprint, port);
    fingerprint.appendInteger(descriptor.outputs.size());
    if (!fingerprint.canAppendRecords(descriptor.outputs.size()))
        return;
    for (const auto& port : descriptor.outputs)
        appendCatalogAuthority(fingerprint, port);
    fingerprint.appendInteger(descriptor.parameters.size());
    if (!fingerprint.canAppendRecords(descriptor.parameters.size()))
        return;
    for (const auto& parameter : descriptor.parameters)
        appendCatalogAuthority(fingerprint, parameter);
    fingerprint.appendEnum(descriptor.supportLevel);
    fingerprint.appendString(descriptor.supportSummary);
    const auto& capabilities = descriptor.capabilities;
    fingerprint.appendEnum(capabilities.offline);
    fingerprint.appendEnum(capabilities.preparedRealtime);
    fingerprint.appendEnum(capabilities.offlineBinding);
    fingerprint.appendEnum(capabilities.preparedRealtimeBinding);
    fingerprint.appendEnum(capabilities.nestedSounds);
    fingerprint.appendEnum(capabilities.state);
    fingerprint.appendBool(capabilities.supportsReset);
    fingerprint.appendBool(capabilities.typedEvents);
    fingerprint.appendBool(capabilities.transportAware);
    fingerprint.appendEnum(capabilities.latency.kind);
    fingerprint.appendInteger(capabilities.latency.fixedFrames);
    fingerprint.appendString(capabilities.latency.parameterId);
    fingerprint.appendEnum(capabilities.latency.preparedRule);
    fingerprint.appendEnum(capabilities.resourceAccess);
    fingerprint.appendEnum(capabilities.fanInLatencyPolicy);
    fingerprint.appendInteger(capabilities.outputBusLayouts.size());
    if (!fingerprint.appendRecords(
            capabilities.outputBusLayouts.size()))
    {
        return;
    }
    for (const auto layout : capabilities.outputBusLayouts)
        fingerprint.appendEnum(layout);
    fingerprint.appendInteger(capabilities.limitations.size());
    if (!fingerprint.appendRecords(capabilities.limitations.size()))
        return;
    for (const auto& limitation : capabilities.limitations)
        fingerprint.appendString(limitation);
    fingerprint.appendString(descriptor.typeVersion);
    fingerprint.appendInteger(descriptor.stateSchemaVersion);
    fingerprint.appendString(descriptor.provenance.vendor);
    fingerprint.appendEnum(descriptor.provenance.origin);
}

}

std::optional<std::uint64_t> projectGeneratorSoundCatalogAuthority(
    const ProjectSoundDefinition& definition,
    const talas::core::NodeRegistry& registry)
{
    if (!validProjectIdentifier(
            definition.definitionId,
            maximumProjectGeneratorSoundDefinitionIdBytes)
        || definition.displayName.empty()
        || !validBoundedText(
            definition.displayName,
            maximumProjectGeneratorSoundDisplayNameBytes)
        || definition.interfaceVersion != 1
        || !definition.inputs.empty()
        || !definition.exposedParameters.empty()
        || definition.outputs.size() != 1
        || definition.outputs.front().portId != "audioOut"
        || definition.outputs.front().type != talas::core::PortType::Audio
        || definition.graph.nodes.empty()
        || definition.graph.nodes.size()
            > maximumProjectGeneratorGraphNodes)
    {
        return std::nullopt;
    }
    if (definition.graph.connections.size()
        != definition.graph.nodes.size() - 1U)
    {
        return std::nullopt;
    }
    CatalogAuthorityFingerprint fingerprint;
    fingerprint.appendInteger(definition.graph.nodes.size());
    std::optional<ProjectGeneratorGraphNodeDescriptor> previous;
    for (std::size_t nodeIndex = 0;
         nodeIndex < definition.graph.nodes.size();
         ++nodeIndex)
    {
        const auto& node = definition.graph.nodes[nodeIndex];
        if (!validProjectIdentifier(
                node.instanceId,
                maximumProjectGeneratorSoundDefinitionIdBytes)
            || !node.parameters.empty()
            || node.soundDefinitionId.has_value()
            || node.preservedComponent.has_value())
        {
            return std::nullopt;
        }
        for (std::size_t priorIndex = 0;
             priorIndex < nodeIndex;
             ++priorIndex)
        {
            if (definition.graph.nodes[priorIndex].instanceId
                == node.instanceId)
            {
                return std::nullopt;
            }
        }
        const auto validation = validateProjectGeneratorGraphNodeType(
            node.typeId,
            registry);
        if (!validation.ok()
            || validation.descriptor->isSource() != (nodeIndex == 0U))
        {
            return std::nullopt;
        }
        const auto* descriptor = registry.findNodeType(node.typeId);
        if (descriptor == nullptr)
            return std::nullopt;
        if (previous.has_value())
        {
            const auto& connection =
                definition.graph.connections[nodeIndex - 1U];
            if (connection.fromNode
                    != definition.graph.nodes[nodeIndex - 1U].instanceId
                || connection.fromPort
                    != previous->audioOutputPortId
                || connection.toNode != node.instanceId
                || connection.toPort
                    != *validation.descriptor->audioInputPortId)
            {
                return std::nullopt;
            }
        }
        appendCatalogAuthority(fingerprint, *descriptor);
        if (!fingerprint.valid)
            return std::nullopt;
        previous = *validation.descriptor;
    }
    const auto& output = definition.outputs.front();
    if (output.nodeId != definition.graph.nodes.back().instanceId
        || output.nodePortId != previous->audioOutputPortId)
    {
        return std::nullopt;
    }
    return fingerprint.value;
}

ProjectGeneratorGraphNodeValidationResult
validateProjectGeneratorGraphNodeType(
    std::string_view nodeTypeId,
    const talas::core::NodeRegistry& registry)
{
    const auto reject = [](
                            ProjectGeneratorGraphNodeValidationCode code,
                            std::string message) {
        ProjectGeneratorGraphNodeValidationResult result;
        result.diagnostic = ProjectGeneratorGraphNodeDiagnostic{
            code,
            std::move(message),
        };
        return result;
    };
    if (nodeTypeId.empty()
        || !validBoundedText(
            nodeTypeId,
            maximumProjectGeneratorNodeTypeIdBytes)
        || !std::ranges::all_of(nodeTypeId, [](unsigned char character) {
               return (character >= 'a' && character <= 'z')
                   || (character >= 'A' && character <= 'Z')
                   || (character >= '0' && character <= '9')
                   || character == '.' || character == '_'
                   || character == '-';
           }))
    {
        return reject(
            ProjectGeneratorGraphNodeValidationCode::InvalidTypeId,
            "Generated graph node type ID is invalid or exceeds 128 bytes");
    }
    const auto* descriptor = registry.findNodeType(nodeTypeId);
    if (descriptor == nullptr)
    {
        return reject(
            ProjectGeneratorGraphNodeValidationCode::UnknownType,
            "Generated graph node type is unavailable");
    }
    CatalogAuthorityFingerprint descriptorBounds;
    appendCatalogAuthority(descriptorBounds, *descriptor);
    if (!descriptorBounds.valid)
    {
        return reject(
            ProjectGeneratorGraphNodeValidationCode::DescriptorLimitExceeded,
            "Generated graph node catalog descriptor exceeds fixed inspection limits");
    }
    if (descriptor->provenance.origin != talas::core::NodeOrigin::BuiltIn)
    {
        return reject(
            ProjectGeneratorGraphNodeValidationCode::UnsupportedOrigin,
            "Generated graphs may contain only builtin node types");
    }
    if (descriptor->typeId == "talas.feedbackLoopInput"
        || descriptor->typeId == "talas.feedbackLoopOutput")
    {
        return reject(
            ProjectGeneratorGraphNodeValidationCode::UnsupportedCapability,
            "Generated graphs may not contain structural feedback nodes");
    }
    // Compiling expands a ContextFreeGrammar into copies of its Inputs
    // (ADR-0015 amendment), which a generated chain does not have.
    if (descriptor->typeId == "talas.contextFreeGrammar")
    {
        return reject(
            ProjectGeneratorGraphNodeValidationCode::UnsupportedCapability,
            "Generated graphs may not contain a ContextFreeGrammar");
    }
    if ((descriptor->supportLevel != talas::core::NodeSupportLevel::FullRuntime
            && descriptor->supportLevel
                != talas::core::NodeSupportLevel::FunctionalSubset)
        || descriptor->capabilities.offline
            == talas::core::ExecutionSupport::Unsupported
        || descriptor->capabilities.preparedRealtime
            == talas::core::ExecutionSupport::Unsupported)
    {
        return reject(
            ProjectGeneratorGraphNodeValidationCode::UnsupportedExecution,
            "Generated graph node must have offline and prepared-realtime product support");
    }
    std::optional<std::string> audioInputPortId;
    for (const auto& input : descriptor->inputs)
    {
        if (input.required && input.type != talas::core::PortType::Audio
            && !input.compilerPopulated)
        {
            return reject(
                ProjectGeneratorGraphNodeValidationCode::RequiredSideInput,
                "Generated graph node has a required non-Audio input");
        }
        if (input.type != talas::core::PortType::Audio || !input.required)
            continue;
        if (input.id.empty()
            || !validBoundedText(
                input.id,
                maximumProjectGeneratorNodeTypeIdBytes))
        {
            return reject(
                ProjectGeneratorGraphNodeValidationCode::InvalidAudioPortShape,
                "Generated graph node Audio input identity is invalid or unbounded");
        }
        if (audioInputPortId.has_value())
        {
            return reject(
                ProjectGeneratorGraphNodeValidationCode::InvalidAudioPortShape,
                "Generated graph node must have at most one required Audio input");
        }
        audioInputPortId = input.id;
    }
    std::optional<std::string> audioOutputPortId;
    for (const auto& output : descriptor->outputs)
    {
        if (output.type != talas::core::PortType::Audio)
            continue;
        if (output.id.empty()
            || !validBoundedText(
                output.id,
                maximumProjectGeneratorNodeTypeIdBytes))
        {
            return reject(
                ProjectGeneratorGraphNodeValidationCode::InvalidAudioPortShape,
                "Generated graph node Audio output identity is invalid or unbounded");
        }
        if (audioOutputPortId.has_value())
        {
            return reject(
                ProjectGeneratorGraphNodeValidationCode::InvalidAudioPortShape,
                "Generated graph node must have exactly one Audio output");
        }
        audioOutputPortId = output.id;
    }
    if (!audioOutputPortId.has_value())
    {
        return reject(
            ProjectGeneratorGraphNodeValidationCode::InvalidAudioPortShape,
            "Generated graph node must have exactly one Audio output");
    }
    const auto isSource = !audioInputPortId.has_value();
    const auto validState =
        descriptor->capabilities.state
            == talas::core::NodeStateModel::Stateless
        || (descriptor->capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && descriptor->capabilities.supportsReset);
    const auto validResourceMode =
        descriptor->capabilities.resourceAccess
            == talas::core::ResourceAccessMode::None
        || (isSource
            && (descriptor->capabilities.resourceAccess
                    == talas::core::ResourceAccessMode::Unclassified
                || descriptor->capabilities.resourceAccess
                    == talas::core::ResourceAccessMode::VariableOrUnknown
                || (descriptor->capabilities.resourceAccess
                        == talas::core::ResourceAccessMode::ImmutableRead
                    && nodeTypeId == "talas.oscillator")));
    const auto unsafeParameter = std::ranges::any_of(
        descriptor->parameters,
        [isSource, nodeTypeId](const auto& parameter) {
            // Resource access modes can widen (for example to live
            // MemoryWriter sources), so the parameter kind itself decides:
            // only the approved Oscillator wavetable may appear, empty.
            if (parameter.kind == talas::core::ParameterKind::Resource)
                return !isSource || nodeTypeId != "talas.oscillator"
                    || !parameter.defaultResource.empty();
            if (parameter.kind == talas::core::ParameterKind::Text)
                return !isSource || !parameter.defaultText.empty();
            return false;
        });
    if (!validState
        || descriptor->capabilities.typedEvents
        || descriptor->capabilities.transportAware
        || !validResourceMode
        || unsafeParameter)
    {
        return reject(
            ProjectGeneratorGraphNodeValidationCode::UnsupportedCapability,
            "Generated graph node exposes unsupported state, events, transport, resources, or executable text");
    }
    ProjectGeneratorGraphNodeValidationResult result;
    result.descriptor = ProjectGeneratorGraphNodeDescriptor{
        std::string{nodeTypeId},
        std::move(audioInputPortId),
        std::move(*audioOutputPortId),
    };
    return result;
}

std::optional<ProjectGeneratorOutputValidationError>
projectGeneratorOutputValidationError(
    const ProjectGeneratorOutputProfile& profile,
    std::span<const std::string> declaredTerminals)
{
    if (profile.version != projectGeneratorOutputProfileVersion)
    {
        return outputError(
            ProjectGeneratorOutputValidationCode::InvalidVersion,
            0,
            0,
            "Generator output profile version must be 1");
    }
    if (profile.timelineStepMicroseconds == 0
        || profile.timelineStepMicroseconds
            > maximumProjectGeneratorTimelineMicroseconds)
    {
        return outputError(
            ProjectGeneratorOutputValidationCode::InvalidTimelineStep,
            0,
            0,
            "Generator Timeline step must be within the fixed microsecond horizon");
    }
    if (profile.scoreStepTicks == 0)
    {
        return outputError(
            ProjectGeneratorOutputValidationCode::InvalidScoreStep,
            0,
            0,
            "Generator Score step must be a positive musical-tick count");
    }
    if (profile.bindings.size() > maximumProjectGeneratorTerminalBindings)
    {
        return outputError(
            ProjectGeneratorOutputValidationCode::TooManyBindings,
            0,
            0,
            "Generator output profile exceeds its terminal-binding limit");
    }

    std::unordered_set<std::string_view> boundTerminals;
    boundTerminals.reserve(profile.bindings.size());
    for (std::size_t bindingIndex = 0;
         bindingIndex < profile.bindings.size();
         ++bindingIndex)
    {
        const auto& binding = profile.bindings[bindingIndex];
        if (!talas::generative::isValidContextFreeIdentifier(binding.terminal))
        {
            return outputError(
                ProjectGeneratorOutputValidationCode::InvalidTerminal,
                bindingIndex,
                0,
                "Generator output binding has an invalid terminal identity");
        }
        if (std::ranges::find(declaredTerminals, binding.terminal)
            == declaredTerminals.end())
        {
            return outputError(
                ProjectGeneratorOutputValidationCode::UnknownTerminal,
                bindingIndex,
                0,
                "Generator output binding references an undeclared terminal");
        }
        if (!boundTerminals.insert(binding.terminal).second)
        {
            return outputError(
                ProjectGeneratorOutputValidationCode::DuplicateTerminal,
                bindingIndex,
                0,
                "Generator terminal may have only one output binding");
        }
        if (binding.actions.size()
            > maximumProjectGeneratorActionsPerTerminal)
        {
            return outputError(
                ProjectGeneratorOutputValidationCode::TooManyActions,
                bindingIndex,
                0,
                "Generator terminal binding exceeds its action limit");
        }

        std::unordered_set<std::string_view> timelineTargets;
        std::unordered_set<std::string_view> automationTargets;
        for (std::size_t actionIndex = 0;
             actionIndex < binding.actions.size();
             ++actionIndex)
        {
            const auto& action = binding.actions[actionIndex];
            if (!validActionShape(action))
            {
                return outputError(
                    ProjectGeneratorOutputValidationCode::InvalidAction,
                    bindingIndex,
                    actionIndex,
                    "Generator output action is invalid or outside its fixed limits");
            }
            if (const auto* event = std::get_if<
                    ProjectGeneratorTimelineEventAction>(&action);
                event != nullptr
                && !timelineTargets.insert(event->eventName).second)
            {
                return outputError(
                    ProjectGeneratorOutputValidationCode::DuplicateTarget,
                    bindingIndex,
                    actionIndex,
                    "One terminal occurrence cannot assign one Timeline Event Value twice");
            }
            if (const auto* automation = std::get_if<
                    ProjectGeneratorScoreAutomationAction>(&action);
                automation != nullptr
                && !automationTargets.insert(automation->laneId).second)
            {
                return outputError(
                    ProjectGeneratorOutputValidationCode::DuplicateTarget,
                    bindingIndex,
                    actionIndex,
                    "One terminal occurrence cannot assign one automation lane twice");
            }
        }
    }
    return std::nullopt;
}

PrepareProjectGeneratorOutputResult prepareProjectGeneratorOutputProfile(
    const ProjectGeneratorOutputProfile& profile,
    std::span<const std::string> declaredTerminals,
    const talas::generative::PreparedContextFreeGenerator& generator)
{
    PrepareProjectGeneratorOutputResult result;
    result.error = projectGeneratorOutputValidationError(
        profile,
        declaredTerminals);
    if (result.error.has_value())
        return result;
    if (generator.symbols.size()
            > talas::generative::maximumContextFreeSymbols
        || generator.terminalFlags.size() != generator.symbols.size())
    {
        result.error = outputError(
            ProjectGeneratorOutputValidationCode::InvalidAction,
            0,
            0,
            "Prepared generator symbol authority is invalid");
        return result;
    }
    try
    {
        result.prepared.timelineStepMicroseconds =
            profile.timelineStepMicroseconds;
        result.prepared.scoreStepTicks = profile.scoreStepTicks;
        result.prepared.bindingBySymbol.assign(
            generator.symbols.size(),
            static_cast<std::int16_t>(-1));
        result.prepared.bindings = profile.bindings;
        for (std::size_t bindingIndex = 0;
             bindingIndex < result.prepared.bindings.size();
             ++bindingIndex)
        {
            const auto found = std::ranges::find(
                generator.symbols,
                result.prepared.bindings[bindingIndex].terminal);
            if (found == generator.symbols.end())
            {
                result.error = outputError(
                    ProjectGeneratorOutputValidationCode::UnknownTerminal,
                    bindingIndex,
                    0,
                    "Prepared output terminal does not exist in the generator symbol table");
                result.prepared = {};
                return result;
            }
            const auto symbolIndex = static_cast<std::size_t>(
                std::distance(generator.symbols.begin(), found));
            if (symbolIndex >= generator.terminalFlags.size()
                || generator.terminalFlags[symbolIndex] == 0
                || bindingIndex
                    > static_cast<std::size_t>(
                        std::numeric_limits<std::int16_t>::max()))
            {
                result.error = outputError(
                    ProjectGeneratorOutputValidationCode::UnknownTerminal,
                    bindingIndex,
                    0,
                    "Prepared output binding must target a terminal symbol");
                result.prepared = {};
                return result;
            }
            result.prepared.bindingBySymbol[symbolIndex] =
                static_cast<std::int16_t>(bindingIndex);
        }
    }
    catch (const std::exception&)
    {
        result.error = outputError(
            ProjectGeneratorOutputValidationCode::TooManyActions,
            0,
            0,
            "Generator output preparation exceeded memory capacity");
        result.prepared = {};
    }
    return result;
}

std::optional<ProjectGeneratorProjectionCapacity>
projectGeneratorProjectionCapacity(
    const talas::generative::PreparedContextFreeGenerator& generator,
    const PreparedProjectGeneratorOutputProfile& output) noexcept
{
    if (!validPreparedOutput(output, generator)
        || generator.maximumOutputSymbols == 0
        || generator.maximumOutputSymbols
            > talas::generative::maximumContextFreeOutputSymbols)
    {
        return std::nullopt;
    }
    std::size_t maximumTimelineActionsPerSymbol = 0;
    std::size_t maximumScoreActionsPerSymbol = 0;
    std::size_t maximumGraphActionsPerSymbol = 0;
    for (const auto& binding : output.bindings)
    {
        std::size_t timelineActions = 0;
        std::size_t scoreActions = 0;
        std::size_t graphActions = 0;
        for (const auto& action : binding.actions)
        {
            if (std::holds_alternative<
                    ProjectGeneratorTimelineEventAction>(action))
            {
                ++timelineActions;
            }
            else if (std::holds_alternative<
                         ProjectGeneratorGraphNodeAction>(action))
            {
                ++graphActions;
            }
            else
            {
                ++scoreActions;
            }
        }
        maximumTimelineActionsPerSymbol = std::max(
            maximumTimelineActionsPerSymbol,
            timelineActions);
        maximumScoreActionsPerSymbol = std::max(
            maximumScoreActionsPerSymbol,
            scoreActions);
        maximumGraphActionsPerSymbol = std::max(
            maximumGraphActionsPerSymbol,
            graphActions);
    }
    const auto scratchSymbols = static_cast<std::size_t>(
        generator.maximumOutputSymbols);
    if ((maximumTimelineActionsPerSymbol != 0
            && scratchSymbols
                > std::numeric_limits<std::size_t>::max()
                    / maximumTimelineActionsPerSymbol)
        || (maximumScoreActionsPerSymbol != 0
            && scratchSymbols
                > std::numeric_limits<std::size_t>::max()
                    / maximumScoreActionsPerSymbol)
        || (maximumGraphActionsPerSymbol != 0
            && scratchSymbols
                > std::numeric_limits<std::size_t>::max()
                    / maximumGraphActionsPerSymbol))
    {
        return std::nullopt;
    }
    return ProjectGeneratorProjectionCapacity{
        scratchSymbols,
        scratchSymbols * maximumTimelineActionsPerSymbol,
        scratchSymbols * maximumScoreActionsPerSymbol,
        scratchSymbols * maximumGraphActionsPerSymbol,
    };
}

namespace
{
ProjectGeneratorTargetValidationResult
validateProjectGeneratorOutputTargetsImpl(
    const ProjectDocument& document,
    const talas::core::NodeRegistry* registry)
{
    ProjectGeneratorTargetValidationResult result;
    const auto add = [&result](
                         ProjectGeneratorTargetValidationCode code,
                         std::string path,
                         std::string message) {
        if (result.diagnostics.size()
            >= maximumProjectGeneratorTargetDiagnostics)
        {
            result.truncated = true;
            return;
        }
        result.diagnostics.push_back({
            code,
            std::move(path),
            std::move(message),
        });
    };

    if (!generatorTargetModelWithinBounds(document))
    {
        add(
            ProjectGeneratorTargetValidationCode::TargetModelLimitExceeded,
            "$.generatorLibrary",
            "Generator target model exceeds a fixed collection limit");
        return result;
    }

    struct EventTarget
    {
        talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
        bool ambiguous = false;
    };
    std::unordered_map<std::string_view, EventTarget> eventValues;
    eventValues.reserve(
        document.virtualControls.size()
        + document.timeline.eventValues.size() + 4U);
    const auto addEventTarget = [&eventValues](
                                    std::string_view name,
                                    talas::expr::ValueDomain domain) {
        const auto [found, inserted] = eventValues.emplace(
            name, EventTarget{domain, false});
        if (!inserted && found->second.domain != domain)
            found->second.ambiguous = true;
    };
    for (const auto& control : document.virtualControls)
        addEventTarget(control.eventName, control.domain);
    for (const auto& event : document.timeline.eventValues)
        addEventTarget(event.eventName, event.domain);
    addEventTarget("Pitch", talas::expr::ValueDomain::NoteNumber);
    addEventTarget("KeyVelocity", talas::expr::ValueDomain::Scalar);
    addEventTarget("KeyTimbre", talas::expr::ValueDomain::Scalar);
    addEventTarget("KeyDown", talas::expr::ValueDomain::Scalar);

    std::unordered_map<std::string_view, const ScoreTrack*> tracks;
    tracks.reserve(document.timeline.score.tracks.size());
    for (const auto& track : document.timeline.score.tracks)
        tracks.emplace(track.trackId, &track);
    std::unordered_set<std::string_view> soundDefinitions;
    soundDefinitions.reserve(document.soundDefinitions.size());
    for (const auto& definition : document.soundDefinitions)
        soundDefinitions.insert(definition.definitionId);
    std::unordered_map<std::string_view, const ScoreAutomationLane*> lanes;
    lanes.reserve(document.timeline.score.automation.size());
    for (const auto& lane : document.timeline.score.automation)
        lanes.emplace(lane.laneId, &lane);

    for (std::size_t programIndex = 0;
         programIndex < document.generatorLibrary.programs.size();
         ++programIndex)
    {
        const auto& program =
            document.generatorLibrary.programs[programIndex];
        for (std::size_t bindingIndex = 0;
             bindingIndex < program.output.bindings.size();
             ++bindingIndex)
        {
            const auto& binding = program.output.bindings[bindingIndex];
            for (std::size_t actionIndex = 0;
                 actionIndex < binding.actions.size();
                 ++actionIndex)
            {
                if (result.truncated)
                    return result;
                const auto& action = binding.actions[actionIndex];
                const auto path = "$.generatorLibrary.programs["
                    + std::to_string(programIndex)
                    + "].output.bindings["
                    + std::to_string(bindingIndex)
                    + "].actions["
                    + std::to_string(actionIndex) + "]";
                if (const auto* event = std::get_if<
                        ProjectGeneratorTimelineEventAction>(&action))
                {
                    const auto target = eventValues.find(event->eventName);
                    if (target == eventValues.end())
                    {
                        add(
                            ProjectGeneratorTargetValidationCode::UnknownEventValue,
                            path + ".eventName",
                            "Generator output references an unavailable Event Value");
                    }
                    else if (target->second.ambiguous
                        || target->second.domain != event->domain)
                    {
                        add(
                            ProjectGeneratorTargetValidationCode::EventValueDomainMismatch,
                            path + ".domain",
                            "Generator Event Value output uses a different domain than its target");
                    }
                }
                else if (const auto* clip = std::get_if<
                             ProjectGeneratorScoreClipAction>(&action))
                {
                    const auto target = tracks.find(clip->trackId);
                    if (target == tracks.end())
                    {
                        add(
                            ProjectGeneratorTargetValidationCode::UnknownScoreTrack,
                            path + ".trackId",
                            "Generator clip output references an unavailable Score track");
                    }
                    else if (target->second->timeDomain
                        != ScoreTimeDomain::MusicalTicks)
                    {
                        add(
                            ProjectGeneratorTargetValidationCode::InvalidScoreTrackDomain,
                            path + ".trackId",
                            "Generator clip output requires a musical-tick Score track");
                    }
                    if (!soundDefinitions.contains(clip->soundDefinitionId))
                    {
                        add(
                            ProjectGeneratorTargetValidationCode::UnknownSoundDefinition,
                            path + ".soundDefinitionId",
                            "Generator clip output references an unavailable Sound definition");
                    }
                }
                else if (const auto* automation = std::get_if<
                             ProjectGeneratorScoreAutomationAction>(&action))
                {
                    const auto target = lanes.find(automation->laneId);
                    if (target == lanes.end())
                    {
                        add(
                            ProjectGeneratorTargetValidationCode::UnknownAutomationLane,
                            path + ".laneId",
                            "Generator automation output references an unavailable Score lane");
                    }
                    else if (target->second->domain != automation->domain)
                    {
                        add(
                            ProjectGeneratorTargetValidationCode::AutomationDomainMismatch,
                            path + ".domain",
                            "Generator automation output uses a different domain than its lane");
                    }
                    else if (automationOwnerDomain(
                                 document.timeline.score,
                                 *target->second)
                        != ScoreTimeDomain::MusicalTicks)
                    {
                        add(
                            ProjectGeneratorTargetValidationCode::InvalidAutomationLaneDomain,
                            path + ".laneId",
                            "Generator automation output requires a musical-tick Score lane");
                    }
                }
                else if (const auto* graphNode = std::get_if<
                             ProjectGeneratorGraphNodeAction>(&action);
                         graphNode != nullptr && registry != nullptr)
                {
                    const auto validation =
                        validateProjectGeneratorGraphNodeType(
                            graphNode->nodeTypeId,
                            *registry);
                    if (!validation.ok())
                    {
                        add(
                            ProjectGeneratorTargetValidationCode::InvalidGraphNodeType,
                            path + ".nodeTypeId",
                            validation.diagnostic.has_value()
                                ? validation.diagnostic->message
                                : "Generator graph node validation failed");
                    }
                }
            }
        }
    }
    return result;
}
}

ProjectGeneratorTargetValidationResult
validateProjectGeneratorOutputTargets(const ProjectDocument& document)
{
    return validateProjectGeneratorOutputTargetsImpl(document, nullptr);
}

ProjectGeneratorTargetValidationResult
validateProjectGeneratorOutputTargets(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    return validateProjectGeneratorOutputTargetsImpl(document, &registry);
}

std::optional<double>
projectGeneratorTimelineSeconds(std::uint64_t microseconds) noexcept
{
    if (microseconds > maximumProjectGeneratorTimelineMicroseconds)
        return std::nullopt;
    constexpr std::uint64_t microsecondsPerSecond = 1'000'000U;
    const auto whole = microseconds / microsecondsPerSecond;
    const auto remainder = microseconds % microsecondsPerSecond;
    return static_cast<double>(whole)
        + static_cast<double>(remainder)
            / static_cast<double>(microsecondsPerSecond);
}

ProjectGeneratorTimelineResult generateProjectTimelineEvents(
    const talas::generative::PreparedContextFreeGenerator& generator,
    const PreparedProjectGeneratorOutputProfile& output,
    talas::generative::ContextFreeGeneratorScratch scratch,
    ProjectGeneratorTimelineRequest request,
    std::stop_token stopToken)
{
    ProjectGeneratorTimelineResult result;
    if (request.baseMicroseconds
        > maximumProjectGeneratorTimelineMicroseconds)
    {
        fail(
            result,
            ProjectGeneratorProjectionStatus::InvalidRequest,
            "Generator Timeline request is invalid");
        return result;
    }
    if (!validPreparedOutput(output, generator))
    {
        fail(
            result,
            ProjectGeneratorProjectionStatus::InvalidPreparedAuthority,
            "Generator Timeline prepared authority is invalid");
        return result;
    }
    const auto generated = talas::generative::generateContextFreeSymbols(
        generator,
        scratch,
        stopToken);
    result.generatedSymbols = generated.outputCount;
    result.rewritePasses = generated.rewritePasses;
    result.operations = generated.operations;
    if (!generated.ok())
    {
        fail(
            result,
            generationFailureStatus(generated.status),
            "Context-free generation did not produce a complete Timeline candidate");
        return result;
    }

    try
    {
        result.events.reserve(std::min(
            maximumProjectGeneratorTimelineEvents,
            generated.outputCount
                * maximumProjectGeneratorActionsPerTerminal));
        const auto symbols =
            talas::generative::generatedContextFreeSymbolIndices(
                scratch,
                generated);
        for (std::size_t ordinal = 0; ordinal < symbols.size(); ++ordinal)
        {
            if ((ordinal & 0xffU) == 0U && stopToken.stop_requested())
            {
                result.events.clear();
                fail(
                    result,
                    ProjectGeneratorProjectionStatus::Cancelled,
                    "Generator Timeline projection was cancelled");
                return result;
            }
            const auto position = positionAt(
                request.baseMicroseconds,
                output.timelineStepMicroseconds,
                ordinal);
            if (!position.has_value()
                || *position > maximumProjectGeneratorTimelineMicroseconds)
            {
                result.events.clear();
                fail(
                    result,
                    ProjectGeneratorProjectionStatus::TimeOverflow,
                    "Generated Timeline time overflowed");
                return result;
            }
            const auto* binding = bindingForSymbol(
                output,
                symbols[ordinal]);
            if (binding == nullptr)
                continue;
            for (const auto& action : binding->actions)
            {
                const auto* event = std::get_if<
                    ProjectGeneratorTimelineEventAction>(&action);
                if (event == nullptr)
                    continue;
                if (result.events.size()
                    >= maximumProjectGeneratorTimelineEvents)
                {
                    result.events.clear();
                    fail(
                        result,
                        ProjectGeneratorProjectionStatus::CapacityExceeded,
                        "Generated Timeline event count exceeded its fixed limit");
                    return result;
                }
                result.events.push_back({
                    *position,
                    event->eventName,
                    event->domain,
                    event->value,
                    event->interpolation,
                });
            }
        }
    }
    catch (const std::exception&)
    {
        result.events.clear();
        fail(
            result,
            ProjectGeneratorProjectionStatus::CapacityExceeded,
            "Generator Timeline projection exceeded memory capacity");
        return result;
    }
    result.status = ProjectGeneratorProjectionStatus::Succeeded;
    return result;
}

ProjectGeneratorScoreResult generateProjectScoreEdits(
    const talas::generative::PreparedContextFreeGenerator& generator,
    const PreparedProjectGeneratorOutputProfile& output,
    talas::generative::ContextFreeGeneratorScratch scratch,
    const ProjectDocument& document,
    ProjectGeneratorScoreRequest request,
    std::stop_token stopToken)
{
    ProjectGeneratorScoreResult result;
    if (!isValidScoreIdentifier(request.outputNamespace)
        || request.outputNamespace.size()
            > maximumProjectGeneratorNamespaceBytes)
    {
        fail(
            result,
            ProjectGeneratorProjectionStatus::InvalidRequest,
            "Generator Score request is invalid");
        return result;
    }
    if (!generatorTargetModelWithinBounds(document))
    {
        fail(
            result,
            ProjectGeneratorProjectionStatus::CapacityExceeded,
            "Generator Score target model exceeds a fixed collection limit");
        return result;
    }
    if (!validPreparedOutput(output, generator))
    {
        fail(
            result,
            ProjectGeneratorProjectionStatus::InvalidPreparedAuthority,
            "Generator Score prepared authority is invalid");
        return result;
    }

    const auto generated = talas::generative::generateContextFreeSymbols(
        generator,
        scratch,
        stopToken);
    result.generatedSymbols = generated.outputCount;
    result.rewritePasses = generated.rewritePasses;
    result.operations = generated.operations;
    if (!generated.ok())
    {
        fail(
            result,
            generationFailureStatus(generated.status),
            "Context-free generation did not produce a complete Score candidate");
        return result;
    }

    try
    {
        std::unordered_map<std::string_view, const ScoreTrack*> tracks;
        tracks.reserve(document.timeline.score.tracks.size());
        for (const auto& track : document.timeline.score.tracks)
            tracks.emplace(track.trackId, &track);
        std::unordered_set<std::string_view> definitionIds;
        definitionIds.reserve(document.soundDefinitions.size());
        for (const auto& definition : document.soundDefinitions)
            definitionIds.insert(definition.definitionId);
        std::unordered_map<std::string_view, const ScoreAutomationLane*> lanes;
        lanes.reserve(document.timeline.score.automation.size());
        for (const auto& lane : document.timeline.score.automation)
            lanes.emplace(lane.laneId, &lane);
        std::unordered_map<std::string, std::size_t> updatedLanes;
        std::unordered_set<std::string_view> occupiedScoreIdentities;
        occupiedScoreIdentities.reserve(
            document.timeline.score.tracks.size()
            + document.timeline.score.clips.size()
            + document.timeline.score.automation.size()
            + document.timeline.score.markers.size());
        for (const auto& track : document.timeline.score.tracks)
            occupiedScoreIdentities.insert(track.trackId);
        for (const auto& clip : document.timeline.score.clips)
            occupiedScoreIdentities.insert(clip.clipId);
        for (const auto& lane : document.timeline.score.automation)
            occupiedScoreIdentities.insert(lane.laneId);
        for (const auto& marker : document.timeline.score.markers)
            occupiedScoreIdentities.insert(marker.markerId);
        std::unordered_set<std::string> generatedScoreIdentities;

        const auto acceptGeneratedIdentity = [&](const std::string& identity) {
            return !occupiedScoreIdentities.contains(identity)
                && generatedScoreIdentities.insert(identity).second;
        };

        const auto symbols =
            talas::generative::generatedContextFreeSymbolIndices(
                scratch,
                generated);
        std::size_t projectedActionCount = 0;
        for (std::size_t ordinal = 0; ordinal < symbols.size(); ++ordinal)
        {
            if ((ordinal & 0xffU) == 0U && stopToken.stop_requested())
            {
                result.edits = {};
                fail(
                    result,
                    ProjectGeneratorProjectionStatus::Cancelled,
                    "Generator Score projection was cancelled");
                return result;
            }
            const auto position = positionAt(
                request.baseTick,
                output.scoreStepTicks,
                ordinal);
            if (!position.has_value())
            {
                result.edits = {};
                fail(
                    result,
                    ProjectGeneratorProjectionStatus::TimeOverflow,
                    "Generated Score time overflowed");
                return result;
            }
            const auto* binding = bindingForSymbol(
                output,
                symbols[ordinal]);
            if (binding == nullptr)
                continue;
            for (std::size_t actionIndex = 0;
                 actionIndex < binding->actions.size();
                 ++actionIndex)
            {
                const auto& action = binding->actions[actionIndex];
                if (std::holds_alternative<
                        ProjectGeneratorTimelineEventAction>(action))
                {
                    continue;
                }
                if (projectedActionCount
                    >= maximumProjectGeneratorScoreEdits)
                {
                    result.edits = {};
                    fail(
                        result,
                        ProjectGeneratorProjectionStatus::CapacityExceeded,
                        "Generated Score edit count exceeded its fixed limit");
                    return result;
                }
                ++projectedActionCount;
                if (const auto* clip = std::get_if<
                        ProjectGeneratorScoreClipAction>(&action))
                {
                    const auto track = tracks.find(clip->trackId);
                    if (track == tracks.end()
                        || track->second->timeDomain
                            != ScoreTimeDomain::MusicalTicks
                        || !definitionIds.contains(
                            clip->soundDefinitionId))
                    {
                        result.edits = {};
                        fail(
                            result,
                            ProjectGeneratorProjectionStatus::InvalidTarget,
                            "Generated clip target is unavailable or not musical-time based");
                        return result;
                    }
                    auto clipId = generatedIdentity(
                        request.outputNamespace,
                        'c',
                        ordinal,
                        actionIndex);
                    if (!acceptGeneratedIdentity(clipId))
                    {
                        result.edits = {};
                        fail(
                            result,
                            ProjectGeneratorProjectionStatus::IdentityCollision,
                            "Generated clip identity collides with the current Score");
                        return result;
                    }
                    result.edits.clips.push_back({
                        std::move(clipId),
                        clip->trackId,
                        clip->soundDefinitionId,
                        {ScoreTimeDomain::MusicalTicks, 0.0, *position},
                        {ScoreTimeDomain::MusicalTicks, 0.0, clip->durationTicks},
                        {ScoreTimeDomain::MusicalTicks, 0.0, clip->contentOffsetTicks},
                        {ScoreTimeDomain::MusicalTicks, 0.0, clip->fadeInTicks},
                        {ScoreTimeDomain::MusicalTicks, 0.0, clip->fadeOutTicks},
                        clip->repeatCount,
                    });
                }
                else if (const auto* automation = std::get_if<
                             ProjectGeneratorScoreAutomationAction>(&action))
                {
                    const auto lane = lanes.find(automation->laneId);
                    if (lane == lanes.end()
                        || lane->second->domain != automation->domain
                        || automationOwnerDomain(
                               document.timeline.score,
                               *lane->second)
                            != ScoreTimeDomain::MusicalTicks)
                    {
                        result.edits = {};
                        fail(
                            result,
                            ProjectGeneratorProjectionStatus::InvalidTarget,
                            "Generated automation target is unavailable or has a different domain");
                        return result;
                    }
                    auto update = updatedLanes.find(automation->laneId);
                    if (update == updatedLanes.end())
                    {
                        result.edits.automation.push_back(*lane->second);
                        update = updatedLanes.emplace(
                            automation->laneId,
                            result.edits.automation.size() - 1U).first;
                    }
                    result.edits.automation[update->second].points.push_back({
                        {ScoreTimeDomain::MusicalTicks, 0.0, *position},
                        automation->value,
                        automation->interpolation,
                    });
                }
                else if (const auto* marker = std::get_if<
                             ProjectGeneratorScoreMarkerAction>(&action))
                {
                    ScoreMarker projected;
                    projected.markerId = generatedIdentity(
                        request.outputNamespace,
                        'm',
                        ordinal,
                        actionIndex);
                    if (!acceptGeneratedIdentity(projected.markerId))
                    {
                        result.edits = {};
                        fail(
                            result,
                            ProjectGeneratorProjectionStatus::IdentityCollision,
                            "Generated marker identity collides with the current Score");
                        return result;
                    }
                    projected.title = marker->title;
                    projected.start = {
                        ScoreTimeDomain::MusicalTicks,
                        0.0,
                        *position,
                    };
                    if (marker->durationTicks.has_value())
                    {
                        std::uint64_t end = 0;
                        if (!checkedAdd(
                                *position,
                                *marker->durationTicks,
                                end))
                        {
                            result.edits = {};
                            fail(
                                result,
                                ProjectGeneratorProjectionStatus::TimeOverflow,
                                "Generated marker end overflowed musical time");
                            return result;
                        }
                        projected.end = ScoreTime{
                            ScoreTimeDomain::MusicalTicks,
                            0.0,
                            end,
                        };
                    }
                    result.edits.markers.push_back(std::move(projected));
                }
            }
        }

        for (auto& lane : result.edits.automation)
        {
            std::ranges::stable_sort(
                lane.points,
                {},
                [](const ScoreAutomationPoint& point) {
                    return point.position.tick;
                });
        }
        std::vector<std::string_view> soundDefinitionIds;
        soundDefinitionIds.reserve(document.soundDefinitions.size());
        for (const auto& definition : document.soundDefinitions)
            soundDefinitionIds.push_back(definition.definitionId);
        const auto candidate = projectScoreWithEdits(
            document.timeline.score,
            result.edits,
            soundDefinitionIds);
        if (!candidate.ok())
        {
            result.edits = {};
            fail(
                result,
                candidate.error.has_value()
                    ? scoreFailureStatus(candidate.error->code)
                    : ProjectGeneratorProjectionStatus::InvalidTarget,
                candidate.error.has_value()
                    ? candidate.error->message
                    : "Generated Score candidate is invalid");
            return result;
        }
    }
    catch (const std::exception&)
    {
        result.edits = {};
        fail(
            result,
            ProjectGeneratorProjectionStatus::CapacityExceeded,
            "Generator Score projection exceeded memory capacity");
        return result;
    }
    result.status = ProjectGeneratorProjectionStatus::Succeeded;
    return result;
}

ProjectGeneratorSoundResult generateProjectSoundDefinition(
    const talas::generative::PreparedContextFreeGenerator& generator,
    const PreparedProjectGeneratorOutputProfile& output,
    talas::generative::ContextFreeGeneratorScratch scratch,
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    ProjectGeneratorSoundRequest request,
    std::stop_token stopToken)
{
    ProjectGeneratorSoundResult result;
    if (!validProjectIdentifier(
            request.definitionId,
            maximumProjectGeneratorSoundDefinitionIdBytes)
        || !validProjectIdentifier(
            request.outputNamespace,
            maximumProjectGeneratorNamespaceBytes)
        || request.displayName.empty()
        || !validBoundedText(
            request.displayName,
            maximumProjectGeneratorSoundDisplayNameBytes))
    {
        fail(
            result,
            ProjectGeneratorProjectionStatus::InvalidRequest,
            "Generator Sound request is invalid");
        return result;
    }
    if (!generatorTargetModelWithinBounds(document, 1U))
    {
        fail(
            result,
            ProjectGeneratorProjectionStatus::CapacityExceeded,
            "Generator Sound target model exceeds its aggregate record limit");
        return result;
    }
    if (!validPreparedOutput(output, generator))
    {
        fail(
            result,
            ProjectGeneratorProjectionStatus::InvalidPreparedAuthority,
            "Generator Sound prepared authority is invalid");
        return result;
    }

    const auto generated = talas::generative::generateContextFreeSymbols(
        generator,
        scratch,
        stopToken);
    result.generatedSymbols = generated.outputCount;
    result.rewritePasses = generated.rewritePasses;
    result.operations = generated.operations;
    if (!generated.ok())
    {
        fail(
            result,
            generationFailureStatus(generated.status),
            "Context-free generation did not produce a complete Sound candidate");
        return result;
    }

    try
    {
        for (const auto& existing : document.soundDefinitions)
        {
            if (existing.definitionId == request.definitionId)
            {
                fail(
                    result,
                    ProjectGeneratorProjectionStatus::IdentityCollision,
                    "Generated Sound definition identity already exists");
                return result;
            }
        }

        ProjectSoundDefinition candidate;
        candidate.definitionId = std::move(request.definitionId);
        candidate.displayName = std::move(request.displayName);
        candidate.interfaceVersion = 1;
        candidate.graph.nodes.reserve(maximumProjectGeneratorGraphNodes);
        candidate.graph.connections.reserve(
            maximumProjectGeneratorGraphNodes - 1U);

        std::optional<ProjectGeneratorGraphNodeDescriptor> previous;
        std::string previousNodeId;
        const auto symbols =
            talas::generative::generatedContextFreeSymbolIndices(
                scratch,
                generated);
        for (std::size_t symbolOrdinal = 0;
             symbolOrdinal < symbols.size();
             ++symbolOrdinal)
        {
            if ((symbolOrdinal & 0xffU) == 0U
                && stopToken.stop_requested())
            {
                fail(
                    result,
                    ProjectGeneratorProjectionStatus::Cancelled,
                    "Generator Sound projection was cancelled");
                return result;
            }
            const auto* binding = bindingForSymbol(
                output,
                symbols[symbolOrdinal]);
            if (binding == nullptr)
                continue;
            for (const auto& action : binding->actions)
            {
                const auto* graphAction = std::get_if<
                    ProjectGeneratorGraphNodeAction>(&action);
                if (graphAction == nullptr)
                    continue;
                const auto nodeOrdinal = candidate.graph.nodes.size();
                if (nodeOrdinal >= maximumProjectGeneratorGraphNodes)
                {
                    fail(
                        result,
                        ProjectGeneratorProjectionStatus::CapacityExceeded,
                        "Generated Sound node count exceeded its fixed limit");
                    return result;
                }
                auto validation = validateProjectGeneratorGraphNodeType(
                    graphAction->nodeTypeId,
                    registry);
                if (!validation.ok())
                {
                    fail(
                        result,
                        ProjectGeneratorProjectionStatus::InvalidTarget,
                        validation.diagnostic.has_value()
                            ? validation.diagnostic->message
                            : "Generated Sound node target is invalid");
                    return result;
                }
                auto descriptor = std::move(*validation.descriptor);
                if ((nodeOrdinal == 0U && !descriptor.isSource())
                    || (nodeOrdinal != 0U && descriptor.isSource()))
                {
                    fail(
                        result,
                        ProjectGeneratorProjectionStatus::InvalidGraphSequence,
                        nodeOrdinal == 0U
                            ? "Generated Sound chain must begin with one source"
                            : "Generated Sound chain may contain a source only at its beginning");
                    return result;
                }
                auto nodeId = generatedNodeIdentity(
                    request.outputNamespace,
                    nodeOrdinal);
                if (!validProjectIdentifier(
                        nodeId,
                        maximumProjectGeneratorSoundDefinitionIdBytes))
                {
                    fail(
                        result,
                        ProjectGeneratorProjectionStatus::InvalidRequest,
                        "Generated Sound node identity is invalid");
                    return result;
                }
                candidate.graph.nodes.push_back({
                    nodeId,
                    descriptor.nodeTypeId,
                    {},
                    {},
                    {},
                });
                if (previous.has_value())
                {
                    candidate.graph.connections.push_back({
                        previousNodeId,
                        previous->audioOutputPortId,
                        nodeId,
                        *descriptor.audioInputPortId,
                    });
                }
                previousNodeId = std::move(nodeId);
                previous = std::move(descriptor);
            }
        }
        if (!previous.has_value())
        {
            fail(
                result,
                ProjectGeneratorProjectionStatus::InvalidGraphSequence,
                "Generated Sound chain is empty");
            return result;
        }
        candidate.outputs.push_back({
            "audioOut",
            talas::core::PortType::Audio,
            previousNodeId,
            previous->audioOutputPortId,
        });

        const auto catalogFingerprint =
            projectGeneratorSoundCatalogAuthority(candidate, registry);
        if (!catalogFingerprint.has_value())
        {
            fail(
                result,
                ProjectGeneratorProjectionStatus::CapacityExceeded,
                "Generated Sound catalog authority exceeds its fixed inspection limits");
            return result;
        }

        ProjectDocument validationDocument;
        validationDocument.soundDefinitions.push_back(candidate);
        const auto validation = validateProjectSoundDefinitions(
            validationDocument,
            registry);
        if (!validation.ok())
        {
            fail(
                result,
                ProjectGeneratorProjectionStatus::InvalidTarget,
                validation.errors.empty()
                    ? "Generated Sound candidate is invalid"
                    : validation.errors.front().message);
            return result;
        }
        result.definition =
            std::make_shared<const ProjectSoundDefinition>(
                std::move(candidate));
        result.catalogAuthorityFingerprint = *catalogFingerprint;
    }
    catch (const std::bad_alloc&)
    {
        result.definition.reset();
        result.status = ProjectGeneratorProjectionStatus::AllocationFailed;
        result.message.clear();
        return result;
    }
    catch (const std::exception& error)
    {
        result.definition.reset();
        fail(
            result,
            ProjectGeneratorProjectionStatus::InvalidTarget,
            error.what());
        return result;
    }
    result.status = ProjectGeneratorProjectionStatus::Succeeded;
    return result;
}
}
