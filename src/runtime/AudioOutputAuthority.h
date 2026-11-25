#pragma once

#include "talas/runtime/Runtime.h"
#include "PreparedTerminalAudioBindings.h"
#include "talas/project/ProjectValidation.h"

#include <algorithm>
#include <optional>

namespace talas::runtime::detail
{

[[nodiscard]] inline std::optional<std::size_t>
findUniqueAudioOutputTerminalStepIndex(const RuntimePlan& plan)
{
    std::optional<std::size_t> terminalIndex;
    for (std::size_t index = 0; index < plan.steps.size(); ++index)
    {
        const auto& step = plan.steps[index];
        if (!talas::project::isTerminalNodeType(step.typeId))
            continue;
        if (step.typeId == "talas.feedbackLoopInput"
            && std::ranges::any_of(
                plan.feedbackPairs,
                [&](const FeedbackPairPlan& pair) {
                    return pair.inputStepIndex == index;
                }))
        {
            continue;
        }
        if (terminalIndex.has_value())
            return std::nullopt;
        terminalIndex = index;
    }
    if (!terminalIndex.has_value()
        || plan.steps[*terminalIndex].typeId != "talas.audioOutput")
    {
        return std::nullopt;
    }
    return terminalIndex;
}

[[nodiscard]] inline std::optional<std::size_t>
findUniqueProducerStepIndex(
    const RuntimePlan& plan,
    const InputBinding& binding)
{
    std::optional<std::size_t> result;
    for (std::size_t index = 0; index < plan.steps.size(); ++index)
    {
        const auto& candidate = plan.steps[index];
        if (candidate.nodeId != binding.sourceNodeId
            || candidate.scopeId != binding.sourceScopeId)
        {
            continue;
        }
        if (result.has_value())
            return std::nullopt;
        result = index;
    }
    return result;
}

[[nodiscard]] inline bool hasExactAudioOutputTerminalStructure(
    const RuntimePlan& plan,
    bool allowLegacySubset)
{
    const auto outputIndex =
        findUniqueAudioOutputTerminalStepIndex(plan);
    if (!outputIndex.has_value())
        return false;

    const auto& step = plan.steps[*outputIndex];
    const auto channels = prepared_terminal_audio::resolve(step);
    const auto exactInputOrder = channels
        && prepared_terminal_audio::hasExactInputOrder(step, *channels);
    if (!exactInputOrder
        || !step.parameters.empty()
        || step.offlineSupport
            != talas::core::ExecutionSupport::Supported
        || step.offlineBinding
            != talas::core::ExecutionBinding::LegacyAdapter
        || (step.preparedRealtimeSupport
                != talas::core::ExecutionSupport::Supported
            && (!allowLegacySubset
                || step.preparedRealtimeSupport
                    != talas::core::ExecutionSupport::Subset))
        || step.preparedRealtimeBinding
            != talas::core::ExecutionBinding::LegacyAdapter
        || step.supportsReset
        || step.kernelKind.has_value()
        || step.preparedIsolatedComponent
        || step.scopeId != globalRuntimeScope
        || step.structuralScopeId != globalRuntimeScope
        || step.eventValueScopeId != globalRuntimeScope
        || step.resolvedLatencyFrames != 0
        || step.inputLatencyFrames.size() != step.inputs.size()
        || step.preparedSpectralFrameSize.has_value()
        || step.preparedSpectralHopSize.has_value()
        || !step.spectrumModifierProbabilityProfile.empty()
        || step.sampleMapPlayback.has_value()
        || step.diskCachePlayback.has_value()
        || step.soundVariableCellIndex.has_value()
        || !step.soundVariableId.empty()
        || !step.soundVariableCandidateDefinitionIds.empty())
    {
        return false;
    }

    const auto ownedBufferCount = std::ranges::count_if(
        plan.buffers,
        [&](const BufferSpec& buffer) {
            return buffer.nodeId == step.nodeId
                && buffer.scopeId == step.scopeId;
        });
    if (ownedBufferCount != 0)
        return false;

    bool inputsResolved = true;
    std::size_t maximumInputLatency = 0;
    for (std::size_t inputIndex = 0;
         inputIndex < step.inputs.size(); ++inputIndex)
    {
        const auto& binding = step.inputs[inputIndex];
        if (binding.bufferIndex >= plan.buffers.size())
            return false;
        const auto& buffer = plan.buffers[binding.bufferIndex];
        const auto producerIndex =
            findUniqueProducerStepIndex(plan, binding);
        if (!producerIndex.has_value()
            || *producerIndex >= *outputIndex
            || buffer.nodeId != binding.sourceNodeId
            || buffer.portId != binding.sourcePortId
            || buffer.scopeId != binding.sourceScopeId
            || buffer.type != talas::core::PortType::Audio
            || buffer.complexFrameCapacity != 0
            || buffer.complexMaximumBinsPerFrame != 0
            || buffer.complexChannelCount != 0
            || buffer.partialFrameCapacity != 0
            || buffer.partialMaximumPartialsPerFrame != 0)
        {
            return false;
        }
        const auto producerLatency =
            plan.steps[*producerIndex].cumulativeOutputLatencyFrames;
        if (step.inputLatencyFrames[inputIndex] != producerLatency)
        {
            return false;
        }
        if (!producerLatency.has_value())
        {
            inputsResolved = false;
            continue;
        }
        maximumInputLatency = std::max(
            maximumInputLatency, *producerLatency);
    }
    const auto expectedLatency = inputsResolved
        ? std::optional<std::size_t>{maximumInputLatency}
        : std::optional<std::size_t>{};
    bool allTimelineLatencyResolved = true;
    std::size_t criticalPathLatency = 0;
    const auto isTimelineBearing = [](talas::core::PortType type) {
        return type == talas::core::PortType::Audio
            || type == talas::core::PortType::Spectrum
            || type == talas::core::PortType::ComplexSpectrum;
    };
    for (const auto& candidate : plan.steps)
    {
        const auto hasTimelineOutput = std::ranges::any_of(
            plan.buffers,
            [&](const BufferSpec& buffer) {
                return buffer.nodeId == candidate.nodeId
                    && buffer.scopeId == candidate.scopeId
                    && isTimelineBearing(buffer.type);
            });
        const auto hasTimelineInput = std::ranges::any_of(
            candidate.inputs,
            [&](const InputBinding& input) {
                return input.bufferIndex < plan.buffers.size()
                    && isTimelineBearing(
                        plan.buffers[input.bufferIndex].type);
            });
        if (!hasTimelineOutput && !hasTimelineInput)
            continue;
        if (!candidate.cumulativeOutputLatencyFrames.has_value())
        {
            allTimelineLatencyResolved = false;
            continue;
        }
        criticalPathLatency = std::max(
            criticalPathLatency,
            *candidate.cumulativeOutputLatencyFrames);
    }
    const auto expectedCriticalPathLatency =
        allTimelineLatencyResolved
        ? std::optional<std::size_t>{criticalPathLatency}
        : std::optional<std::size_t>{};
    if (step.cumulativeInputLatencyFrames != expectedLatency
        || step.cumulativeOutputLatencyFrames != expectedLatency
        || plan.totalOutputLatencyFrames != expectedLatency
        || plan.criticalPathLatencyFrames
            != expectedCriticalPathLatency)
    {
        return false;
    }

    return prepared_terminal_audio::agreesWithCompiledRoutes(plan, *channels);
}

[[nodiscard]] inline bool hasExactAudioOutputProducerStepAuthority(
    const RuntimePlan& plan,
    const RuntimePlan& authorityPlan,
    const InputBinding& binding,
    const InputBinding& authorityBinding)
{
    const auto producerIndex =
        findUniqueProducerStepIndex(plan, binding);
    const auto authorityProducerIndex =
        findUniqueProducerStepIndex(authorityPlan, authorityBinding);
    if (!producerIndex.has_value()
        || producerIndex != authorityProducerIndex)
    {
        return false;
    }

    const auto& producer = plan.steps[*producerIndex];
    const auto& authorityProducer =
        authorityPlan.steps[*authorityProducerIndex];
    return producer.nodeId == authorityProducer.nodeId
        && producer.typeId == authorityProducer.typeId
        && producer.scopeId == authorityProducer.scopeId
        && producer.structuralScopeId
            == authorityProducer.structuralScopeId
        && producer.eventValueScopeId
            == authorityProducer.eventValueScopeId
        && producer.structuralSourcePath
            == authorityProducer.structuralSourcePath;
}

}
