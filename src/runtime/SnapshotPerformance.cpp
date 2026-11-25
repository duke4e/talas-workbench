#include "talas/runtime/SnapshotPerformance.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/Runtime.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>

namespace talas::runtime
{
namespace
{
class FingerprintWriter
{
public:
    void text(std::string_view value)
    {
        number(value.size());
        projection_.push_back(':');
        projection_.append(value);
        projection_.push_back(';');
    }

    template <typename Value>
    void number(Value value)
    {
        static_assert(std::is_integral_v<Value>);
        std::array<char, 32> buffer{};
        const auto normalized = static_cast<std::uint64_t>(value);
        const auto converted = std::to_chars(
            buffer.data(),
            buffer.data() + buffer.size(),
            normalized);
        projection_.append(buffer.data(), converted.ptr);
        projection_.push_back(';');
    }

    template <typename Enum>
    void enumeration(Enum value)
    {
        number(static_cast<std::underlying_type_t<Enum>>(value));
    }

    void floating(double value)
    {
        number(std::bit_cast<std::uint64_t>(value));
    }

    void optionalText(const std::optional<std::string>& value)
    {
        number(value.has_value());
        if (value.has_value())
            text(*value);
    }

    void optionalSize(const std::optional<std::size_t>& value)
    {
        number(value.has_value());
        if (value.has_value())
            number(*value);
    }

    [[nodiscard]] std::string take()
    {
        return std::move(projection_);
    }

private:
    std::string projection_;
};

void appendPreparedSettings(
    FingerprintWriter& writer,
    const RenderSettings& settings)
{
    writer.floating(settings.sampleRate);
    writer.number(settings.maxBlockFrames);
    writer.floating(settings.tempoBpm);
    writer.number(settings.defaultMidiChannel);
    writer.number(settings.globalMap.size());
    for (const auto& entry : settings.globalMap)
    {
        writer.text(entry.eventName);
        writer.number(entry.pitchBend);
        writer.number(entry.controller);
        writer.number(entry.channel.value_or(0));
        writer.floating(entry.minimum);
        writer.floating(entry.maximum);
        writer.floating(entry.grid);
        writer.number(static_cast<int>(entry.taper));
        writer.number(static_cast<int>(entry.displayAs));
    }
    writer.number(settings.maxRealtimeHistoryFrames);
    writer.number(settings.maxRealtimeFeedbackFrames);
    writer.number(settings.maxFeedbackPlanSteps);
    writer.number(settings.maxRealtimeEventTransactions);
    writer.number(settings.enableRealtimeAnalysisCapture);
    writer.number(settings.realtimeAnalysisQueueCapacity);
    writer.number(settings.realtimeAnalysisTargets.size());
    for (const auto& target : settings.realtimeAnalysisTargets)
    {
        writer.number(target.instancePath.size());
        for (const auto& instanceId : target.instancePath)
            writer.text(instanceId);
        writer.number(target.containingDefinitionId.has_value());
        if (target.containingDefinitionId.has_value())
            writer.text(*target.containingDefinitionId);
        writer.text(target.nodeId);
        writer.text(target.portId);
        writer.enumeration(target.kind);
        writer.enumeration(target.payloadKind);
        writer.number(target.channelCount);
        writer.text(target.clientId);
        writer.number(target.scoreClipId.has_value());
        if (target.scoreClipId) writer.text(*target.scoreClipId);
        writer.number(target.scoreIterationOrdinal.has_value());
        if (target.scoreIterationOrdinal) writer.number(*target.scoreIterationOrdinal);
    }
    writer.number(settings.transportLoop.has_value());
    if (settings.transportLoop.has_value())
    {
        writer.number(settings.transportLoop->startFrame);
        writer.number(settings.transportLoop->endFrame);
    }

    writer.number(settings.eventValues.size());
    for (const auto& binding : settings.eventValues)
    {
        writer.text(binding.name);
        writer.enumeration(binding.domain);
        writer.number(binding.scopeId);
        writer.number(binding.sourceScopeId);
    }
    writer.number(settings.eventValueChanges.size());
    for (const auto& change : settings.eventValueChanges)
    {
        writer.number(change.frame);
        writer.text(change.name);
        writer.floating(change.value);
        writer.enumeration(change.domain);
        writer.enumeration(change.interpolation);
    }
    writer.number(settings.resourceSnapshots.size());
    for (const auto& snapshot : settings.resourceSnapshots)
    {
        writer.number(snapshot != nullptr);
        if (snapshot == nullptr)
            continue;
        writer.text(snapshot->id);
        writer.enumeration(snapshot->kind);
        writer.number(snapshot->generation);
        writer.text(snapshot->contentHash);
        writer.text(snapshot->resolvedPath.generic_string());
        writer.number(snapshot->channelCount);
        writer.floating(snapshot->sampleRate);
        writer.number(snapshot->frameCount);
        writer.text(snapshot->encoding);
    }
}

void appendPreparedPlan(
    FingerprintWriter& writer,
    const RuntimePlan& plan)
{
    writer.number(plan.steps.size());
    for (const auto& step : plan.steps)
    {
        writer.text(step.nodeId);
        writer.text(step.typeId);
        writer.number(step.scopeId);
        writer.number(step.eventValueScopeId);
        writer.enumeration(step.offlineBinding);
        writer.enumeration(step.preparedRealtimeBinding);
        writer.number(step.supportsReset);
        writer.number(step.kernelKind.has_value());
        if (step.kernelKind.has_value())
            writer.enumeration(*step.kernelKind);
        writer.text(step.structuralSourcePath);
        writer.optionalSize(step.resolvedLatencyFrames);
        writer.optionalSize(step.cumulativeInputLatencyFrames);
        writer.optionalSize(step.cumulativeOutputLatencyFrames);
        writer.optionalSize(step.preparedSpectralFrameSize);
        writer.optionalSize(step.preparedSpectralHopSize);
        writer.number(step.inputLatencyFrames.size());
        for (const auto& latency : step.inputLatencyFrames)
            writer.optionalSize(latency);

        writer.number(step.inputs.size());
        for (const auto& input : step.inputs)
        {
            writer.text(input.inputPortId);
            writer.text(input.sourceNodeId);
            writer.text(input.sourcePortId);
            writer.number(input.bufferIndex);
            writer.number(input.sourceScopeId);
        }
        writer.number(step.parameters.size());
        for (const auto& parameter : step.parameters)
        {
            writer.text(parameter.id);
            writer.enumeration(parameter.domain);
            writer.optionalText(parameter.resource);
            writer.optionalText(parameter.text);
        }
    }

    writer.number(plan.buffers.size());
    for (const auto& buffer : plan.buffers)
    {
        writer.text(buffer.nodeId);
        writer.text(buffer.portId);
        writer.enumeration(buffer.type);
        writer.number(buffer.frames);
        writer.number(buffer.scopeId);
        writer.number(buffer.complexFrameCapacity);
        writer.number(buffer.complexMaximumBinsPerFrame);
        writer.number(buffer.complexChannelCount);
        writer.number(buffer.partialFrameCapacity);
        writer.number(buffer.partialMaximumPartialsPerFrame);
    }
    writer.number(plan.outputBufferIndex);
    writer.optionalSize(plan.rightOutputBufferIndex);
    writer.number(plan.outputBufferIndices.size());
    for (const auto index : plan.outputBufferIndices)
        writer.number(index);

    writer.number(plan.midiVoices.size());
    for (const auto& midi : plan.midiVoices)
    {
        writer.number(midi.ownerStepIndex);
        writer.number(midi.configuration.channel);
        writer.number(midi.configuration.lowPitch);
        writer.number(midi.configuration.highPitch);
        writer.number(midi.configuration.polyphony);
        writer.number(midi.configuration.retriggerDelayFrames);
        writer.number(midi.voices.size());
        for (const auto& voice : midi.voices)
        {
            writer.number(voice.scopeId);
            writer.number(voice.firstStepIndex);
            writer.number(voice.stepCount);
            writer.number(voice.audioOutputBufferIndex);
            writer.number(voice.rightAudioOutputBufferIndex.has_value() ? 1 : 0);
            if (voice.rightAudioOutputBufferIndex.has_value())
                writer.number(*voice.rightAudioOutputBufferIndex);
        }
    }
    writer.number(plan.feedbackPairs.size());
    for (const auto& feedback : plan.feedbackPairs)
    {
        writer.number(feedback.scopeId);
        writer.text(feedback.connectionId);
        writer.number(feedback.inputStepIndex);
        writer.number(feedback.outputStepIndex);
        writer.number(feedback.inputBufferIndex);
        writer.number(feedback.outputBufferIndex);
        writer.number(feedback.delayFrames);
    }
    writer.number(plan.feedbackComponents.size());
    for (const auto& component : plan.feedbackComponents)
    {
        writer.number(component.stepIndices.size());
        for (const auto index : component.stepIndices)
            writer.number(index);
        writer.number(component.feedbackPairIndices.size());
        for (const auto index : component.feedbackPairIndices)
            writer.number(index);
    }
    writer.number(plan.latencyCompensations.size());
    for (const auto& latency : plan.latencyCompensations)
    {
        writer.number(latency.targetStepIndex);
        writer.number(latency.inputBindingIndex);
        writer.number(latency.sourceBufferIndex);
        writer.number(latency.outputBufferIndex);
        writer.number(latency.delayFrames);
    }
    writer.number(plan.reprepareDiagnostics.size());
    for (const auto& diagnostic : plan.reprepareDiagnostics)
    {
        writer.number(diagnostic.stepIndex);
        writer.text(diagnostic.nodeId);
        writer.text(diagnostic.parameterId);
        writer.text(diagnostic.eventValueName);
        writer.number(diagnostic.eventValueScopeId);
    }
    writer.number(plan.latencyWarnings.size());
    for (const auto& warning : plan.latencyWarnings)
    {
        writer.enumeration(warning.kind);
        writer.number(warning.stepIndex);
        writer.text(warning.nodeId);
    }
    writer.optionalSize(plan.criticalPathLatencyFrames);
    writer.optionalSize(plan.totalOutputLatencyFrames);
}

void appendPreparedSnapshotManifest(
    FingerprintWriter& writer,
    const SnapshotPreparedTargetManifest& manifest)
{
    writer.number(manifest.targets.size());
    for (const auto& target : manifest.targets)
    {
        writer.text(talas::project::snapshotTargetIdentityString(target.target));
        writer.enumeration(target.valueKind);
        writer.enumeration(target.domain);
        writer.number(target.choices.size());
        for (const auto& choice : target.choices)
            writer.text(choice);
        writer.enumeration(target.projection);
        writer.number(target.eventValueTargets.size());
        for (const auto& eventValue : target.eventValueTargets)
        {
            writer.number(eventValue.scopeId);
            writer.number(eventValue.targetIndex);
            writer.enumeration(eventValue.domain);
        }
    }
}
}

SnapshotExecutionTopologyFingerprint
snapshotExecutionTopologyFingerprint(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    auto normalized = document;
    normalized.metadata = {};
    normalized.presentation = {};
    normalized.snapshotLibrary = {};
    normalized.liveSetLibrary = {};
    // What the surface's layouts keep of their own names their preset banks
    // and shows the controls; it plays nothing.
    normalized.virtualControlLayoutSettings.clear();
    const auto normalizeGraph = [](talas::project::ProjectGraph& graph) {
        for (auto& node : graph.nodes)
        {
            for (auto& parameter : node.parameters)
            {
                parameter.expression.reset();
                parameter.choice.reset();
                parameter.boolean.reset();
            }
        }
    };
    normalizeGraph(normalized.graph);
    for (auto& definition : normalized.soundDefinitions)
        normalizeGraph(definition.graph);
    for (auto& control : normalized.virtualControls)
        control.value = 0.0;
    return {
        talas::project::serializeProjectDocument(normalized, registry),
    };
}

SnapshotExecutionTopologyFingerprint
snapshotExecutionTopologyFingerprint(
    const talas::project::ProjectDocument& document)
{
    return snapshotExecutionTopologyFingerprint(
        document,
        talas::core::makeBuiltinNodeRegistry());
}

SnapshotExecutionTopologyFingerprint
snapshotPreparedExecutionTopologyFingerprint(
    const PreparedRuntimeGraph& graph)
{
    FingerprintWriter writer;
    appendPreparedSettings(writer, graph.renderSettings());
    appendPreparedPlan(writer, graph.plan());
    appendPreparedSnapshotManifest(
        writer,
        graph.snapshotPreparedTargets());
    writer.number(graph.preparedAudioStreamCount());
    return {writer.take()};
}

std::uint64_t snapshotSmoothingFrames(
    double smoothingSeconds,
    double sampleRate) noexcept
{
    if (!std::isfinite(smoothingSeconds)
        || !std::isfinite(sampleRate)
        || smoothingSeconds <= 0.0
        || sampleRate <= 0.0)
    {
        return 0;
    }
    const auto frames = static_cast<long double>(smoothingSeconds)
        * static_cast<long double>(sampleRate);
    const auto maximum = static_cast<long double>(
        std::numeric_limits<std::uint64_t>::max());
    if (!std::isfinite(frames) || frames >= maximum)
        return std::numeric_limits<std::uint64_t>::max();
    const auto rounded = std::round(frames);
    return rounded >= maximum
        ? std::numeric_limits<std::uint64_t>::max()
        : static_cast<std::uint64_t>(rounded);
}
}
