#include "talas/runtime/PreparedGraphSummary.h"

#include "LiveSpectralAnalysisConfiguration.h"
#include "NativeStftConfiguration.h"
#include "PackedRealFftConfiguration.h"
#include "talas/project/MidiPitchBounds.h"
#include "talas/project/ProjectDiskCache.h"
#include "talas/project/ProjectAssetInspection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

namespace talas::runtime
{
namespace
{
[[nodiscard]] bool isCanonicalSha256(std::string_view value) noexcept
{
    return value.size() == 64
        && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9')
                   || (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] const CompileStep* findUniqueStep(
    const RuntimePlan& plan,
    std::string_view nodeId,
    RuntimeScopeId scopeId) noexcept
{
    const CompileStep* result = nullptr;
    for (const auto& step : plan.steps)
    {
        if (step.nodeId != nodeId || step.scopeId != scopeId)
            continue;
        if (result != nullptr)
            return nullptr;
        result = &step;
    }
    return result;
}

[[nodiscard]] const ParameterValue* findParameter(
    const CompileStep& step,
    std::string_view parameterId) noexcept
{
    const auto found = std::ranges::find_if(
        step.parameters,
        [&](const ParameterValue& parameter) {
            return parameter.id == parameterId;
        });
    return found == step.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<PreparedMidiMapperConfiguration>
preparedMidiMapperConfigurationForStep(const CompileStep& step) noexcept
{
    const auto* map = findParameter(step, "map");
    const auto* channel = findParameter(step, "channel");
    const auto* lowPitch = findParameter(step, "lowPitch");
    const auto* highPitch = findParameter(step, "highPitch");
    const auto* polyphony = findParameter(step, "polyphony");
    if (map == nullptr || !map->text.has_value()
        || channel == nullptr || lowPitch == nullptr
        || highPitch == nullptr || polyphony == nullptr)
    {
        return std::nullopt;
    }
    const auto parsed = parsePreparedMidiMapperMode(*map->text);
    const auto exactIntegerInRange = [](double value,
                                         double minimum,
                                         double maximum) noexcept {
        return std::isfinite(value) && std::floor(value) == value
            && value >= minimum && value <= maximum;
    };
    // A bound given as a frequency is the nearest note, as the
    // renderers take it.
    const auto pitchBounds = talas::project::roundedMidiPitchBounds(
        lowPitch->value, highPitch->value);
    if (!parsed.ok()
        || !pitchBounds.has_value()
        || !exactIntegerInRange(channel->value, 0.0, 16.0)
        || !exactIntegerInRange(
            polyphony->value,
            1.0,
            static_cast<double>(maximumPreparedMidiMapperPolyphony))
        || lowPitch->value > highPitch->value)
    {
        return std::nullopt;
    }
    PreparedMidiMapperConfiguration configuration;
    configuration.mode = parsed.mode;
    configuration.channel = static_cast<std::uint8_t>(channel->value);
    configuration.lowPitch = (*pitchBounds)[0];
    configuration.highPitch = (*pitchBounds)[1];
    configuration.polyphony = static_cast<std::uint8_t>(polyphony->value);
    if (!PreparedMidiMapperState{configuration}.ready())
        return std::nullopt;
    return configuration;
}

[[nodiscard]] bool samePreparedMidiMapperConfiguration(
    const PreparedMidiMapperConfiguration& left,
    const PreparedMidiMapperConfiguration& right) noexcept
{
    return left.mode == right.mode
        && left.channel == right.channel
        && left.lowPitch == right.lowPitch
        && left.highPitch == right.highPitch
        && left.polyphony == right.polyphony;
}

[[nodiscard]] bool appendLimitation(
    PreparedSpectralNodeSummary& summary,
    std::string id,
    std::string description)
{
    try
    {
        summary.limitationIds.push_back(std::move(id));
        summary.limitationSummaries.push_back(std::move(description));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

[[nodiscard]] bool appendLimitation(
    PreparedStreamContractSummary& summary,
    std::string id,
    std::string description)
{
    try
    {
        summary.limitationIds.push_back(std::move(id));
        summary.limitationSummaries.push_back(std::move(description));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

[[nodiscard]] bool addCadence(
    PreparedSpectralNodeSummary& summary,
    double sampleRate,
    std::size_t hopSize) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || hopSize == 0)
    {
        return false;
    }
    summary.cadenceHz = sampleRate
        / static_cast<double>(hopSize);
    return std::isfinite(summary.cadenceHz)
        && summary.cadenceHz > 0.0;
}

struct SpectralInputAuthority
{
    const CompileStep* source = nullptr;
    std::size_t bindingIndex = 0;
};

[[nodiscard]] std::optional<SpectralInputAuthority>
spectralInputAuthority(
    const RuntimePlan& plan,
    const CompileStep& step,
    std::string_view inputPortId) noexcept
{
    const InputBinding* binding = nullptr;
    std::size_t bindingIndex = 0;
    for (std::size_t index = 0;
         index < step.inputs.size();
         ++index)
    {
        if (step.inputs[index].inputPortId != inputPortId)
            continue;
        if (binding != nullptr)
            return std::nullopt;
        binding = &step.inputs[index];
        bindingIndex = index;
    }
    if (binding == nullptr)
        return std::nullopt;
    const auto* source = findUniqueStep(
        plan,
        binding->sourceNodeId,
        binding->sourceScopeId);
    if (source == nullptr)
        return std::nullopt;
    return SpectralInputAuthority{source, bindingIndex};
}

[[nodiscard]] bool hasExactSpectralLatencyAuthority(
    const CompileStep& step,
    const SpectralInputAuthority& input,
    std::size_t intrinsicFrames) noexcept
{
    if (step.resolvedLatencyFrames != intrinsicFrames
        || input.bindingIndex >= step.inputLatencyFrames.size()
        || !step.inputLatencyFrames[input.bindingIndex].has_value()
        || !input.source->cumulativeOutputLatencyFrames.has_value()
        || step.inputLatencyFrames[input.bindingIndex]
            != input.source->cumulativeOutputLatencyFrames
        || step.cumulativeInputLatencyFrames
            != input.source->cumulativeOutputLatencyFrames
        || !step.cumulativeInputLatencyFrames.has_value()
        || !step.cumulativeOutputLatencyFrames.has_value()
        || intrinsicFrames
            > std::numeric_limits<std::size_t>::max()
                - *step.cumulativeInputLatencyFrames)
    {
        return false;
    }
    return *step.cumulativeOutputLatencyFrames
        == *step.cumulativeInputLatencyFrames + intrinsicFrames;
}

[[nodiscard]] PreparedGraphSummaryResult fail(std::string message)
{
    return {std::nullopt, std::move(message)};
}

[[nodiscard]] PreparedGraphSummaryResult summarize(
    const PreparedRuntimeGraph& graph)
{
    const auto& plan = graph.plan();
    const auto& settings = graph.renderSettings();
    if (!std::isfinite(settings.sampleRate)
        || settings.sampleRate <= 0.0
        || settings.maxBlockFrames == 0)
    {
        return fail(
            "Prepared graph summary requires a positive finite sample rate and block size");
    }

    PreparedGraphSummary summary;
    summary.sampleRate = settings.sampleRate;
    summary.maxBlockFrames = settings.maxBlockFrames;
    summary.latency.criticalPathFrames =
        plan.criticalPathLatencyFrames;
    summary.latency.terminalOutputFrames =
        plan.totalOutputLatencyFrames;

    const auto placementValidation =
        validateProcessorPlacementPlan(plan);
    if (!placementValidation.ok())
    {
        return fail(
            "Prepared graph summary rejected processor placement: "
            + placementValidation.error);
    }
    const auto placementIdentity = [&](std::size_t stepIndex)
        -> std::optional<PreparedProcessorPlacementIdentity> {
        if (stepIndex >= plan.steps.size())
            return std::nullopt;
        const auto& step = plan.steps[stepIndex];
        return PreparedProcessorPlacementIdentity{
            step.nodeId,
            step.typeId,
            step.scopeId,
            step.structuralSourcePath,
        };
    };
    summary.processorPlacements.reserve(
        plan.processorPlacement.domains.size());
    for (const auto& domain : plan.processorPlacement.domains)
    {
        PreparedProcessorPlacementDomainSummary placement;
        placement.processorId = domain.processorId;
        placement.steps.reserve(domain.stepIndices.size());
        placement.boundaries.reserve(
            domain.boundaryStepIndices.size());
        for (const auto stepIndex : domain.stepIndices)
        {
            auto identity = placementIdentity(stepIndex);
            if (!identity.has_value())
            {
                return fail(
                    "Prepared graph summary received invalid processor placement step authority");
            }
            placement.steps.push_back(std::move(*identity));
        }
        for (const auto stepIndex : domain.boundaryStepIndices)
        {
            auto identity = placementIdentity(stepIndex);
            if (!identity.has_value())
            {
                return fail(
                    "Prepared graph summary received invalid processor placement boundary authority");
            }
            placement.boundaries.push_back(std::move(*identity));
        }
        summary.processorPlacements.push_back(
            std::move(placement));
    }
    summary.processorTransfers.reserve(
        plan.processorPlacement.transfers.size());
    for (const auto& transfer : plan.processorPlacement.transfers)
    {
        auto source = placementIdentity(transfer.sourceStepIndex);
        auto target = placementIdentity(transfer.targetStepIndex);
        if (!source.has_value() || !target.has_value()
            || transfer.inputBindingIndex
                >= plan.steps[transfer.targetStepIndex].inputs.size())
        {
            return fail(
                "Prepared graph summary received invalid processor transfer authority");
        }
        const auto& binding = plan.steps[transfer.targetStepIndex]
                                  .inputs[transfer.inputBindingIndex];
        summary.processorTransfers.push_back({
            std::move(*source),
            binding.sourcePortId,
            std::move(*target),
            binding.inputPortId,
            transfer.sourceProcessorId,
            transfer.targetProcessorId,
        });
    }

    const auto recorderValidation =
        validateDiskRecorderCapturePlan(plan);
    if (!recorderValidation.ok())
    {
        return fail(
            "Prepared graph summary rejected Disk Recorder capture: "
            + recorderValidation.error);
    }
    if (plan.diskRecorderCapture.has_value())
    {
        const auto& capture = *plan.diskRecorderCapture;
        const auto& recorder = plan.steps[capture.stepIndex];
        summary.diskRecorderCapture = PreparedDiskRecorderCaptureSummary{
            recorder.nodeId,
            recorder.scopeId,
            recorder.structuralSourcePath,
            capture.captureBufferIndex,
            capture.triggerBufferIndex,
            capture.captureDurationBufferIndex,
            capture.destinationNameHint,
            capture.format,
            capture.wordSize,
            capture.channels,
            capture.trigger,
            capture.captureDurationSeconds,
            capture.gated,
            capture.writeSupport,
        };
    }

    for (const auto& tap : plan.diskRecorderTaps)
    {
        const auto& capture = tap.capture;
        if (capture.stepIndex >= plan.steps.size())
            return fail("Prepared graph summary rejected a Disk Recorder within the Sound");
        const auto& recorder = plan.steps[capture.stepIndex];
        summary.diskRecorderTaps.push_back({
            recorder.nodeId,
            recorder.scopeId,
            recorder.structuralSourcePath,
            capture.captureBufferIndex,
            capture.triggerBufferIndex,
            capture.captureDurationBufferIndex,
            capture.destinationNameHint,
            capture.format,
            capture.wordSize,
            capture.channels,
            capture.trigger,
            capture.captureDurationSeconds,
            capture.gated,
            capture.writeSupport,
        });
    }

    const auto& outputIndices = plan.outputBufferIndices;
    std::vector<std::size_t> legacyOutputIndices;
    const std::vector<std::size_t>* selectedOutputIndices =
        &outputIndices;
    if (selectedOutputIndices->empty() && !plan.buffers.empty())
    {
        legacyOutputIndices.push_back(plan.outputBufferIndex);
        if (plan.rightOutputBufferIndex.has_value())
            legacyOutputIndices.push_back(*plan.rightOutputBufferIndex);
        selectedOutputIndices = &legacyOutputIndices;
    }
    summary.outputs.reserve(selectedOutputIndices->size());
    for (std::size_t ordinal = 0;
         ordinal < selectedOutputIndices->size();
         ++ordinal)
    {
        const auto bufferIndex = (*selectedOutputIndices)[ordinal];
        if (bufferIndex >= plan.buffers.size())
        {
            return fail(
                "Prepared graph summary received an invalid output buffer index at ordinal "
                + std::to_string(ordinal + 1));
        }
        const auto& buffer = plan.buffers[bufferIndex];
        if (buffer.type != talas::core::PortType::Audio)
        {
            return fail(
                "Prepared graph summary output '" + buffer.nodeId
                + "' is not an Audio buffer");
        }
        const auto* source = findUniqueStep(
            plan, buffer.nodeId, buffer.scopeId);
        if (source == nullptr)
        {
            return fail(
                "Prepared graph summary cannot resolve output source '"
                + buffer.nodeId + "'");
        }
        summary.outputs.push_back({
            ordinal + 1,
            buffer.nodeId,
            buffer.portId,
            buffer.scopeId,
            source->structuralSourcePath,
            buffer.type,
        });
    }
    if (summary.outputs.empty())
        summary.outputLayout = PreparedOutputLayout::None;
    else if (summary.outputs.size() == 1)
        summary.outputLayout = PreparedOutputLayout::Mono;
    else if (summary.outputs.size() == 2)
        summary.outputLayout = PreparedOutputLayout::Stereo;
    else
        summary.outputLayout = PreparedOutputLayout::Discrete;

    summary.latency.compensations.reserve(
        plan.latencyCompensations.size());
    for (const auto& compensation : plan.latencyCompensations)
    {
        if (compensation.targetStepIndex >= plan.steps.size())
            return fail("Prepared graph summary received an invalid latency target");
        const auto& target = plan.steps[compensation.targetStepIndex];
        if (compensation.inputBindingIndex >= target.inputs.size()
            || compensation.inputBindingIndex
                >= target.inputLatencyFrames.size()
            || !target.inputLatencyFrames[
                    compensation.inputBindingIndex]
                    .has_value()
            || compensation.sourceBufferIndex >= plan.buffers.size()
            || compensation.outputBufferIndex >= plan.buffers.size()
            || compensation.delayFrames == 0
            || target.inputs[compensation.inputBindingIndex]
                    .bufferIndex
                != compensation.outputBufferIndex)
        {
            return fail(
                "Prepared graph summary received invalid latency compensation for '"
                + target.nodeId + "'");
        }
        const auto before = *target.inputLatencyFrames[
            compensation.inputBindingIndex];
        if (before > std::numeric_limits<std::size_t>::max()
                - compensation.delayFrames)
        {
            return fail(
                "Prepared graph summary latency compensation overflow for '"
                + target.nodeId + "'");
        }
        summary.latency.compensations.push_back({
            target.nodeId,
            target.inputs[compensation.inputBindingIndex]
                .inputPortId,
            target.scopeId,
            target.structuralSourcePath,
            before,
            compensation.delayFrames,
            before + compensation.delayFrames,
        });
    }

    summary.latency.warnings.reserve(plan.latencyWarnings.size());
    for (const auto& warning : plan.latencyWarnings)
    {
        if (warning.stepIndex >= plan.steps.size()
            || plan.steps[warning.stepIndex].nodeId
                != warning.nodeId)
        {
            return fail(
                "Prepared graph summary received invalid latency warning authority");
        }
        const auto& target = plan.steps[warning.stepIndex];
        summary.latency.warnings.push_back({
            warning.kind,
            warning.nodeId,
            target.scopeId,
            target.structuralSourcePath,
            warning.message,
        });
    }

    summary.resources.reserve(settings.resourceSnapshots.size());
    for (const auto& snapshot : settings.resourceSnapshots)
    {
        if (snapshot == nullptr)
            return fail("Prepared graph summary received a null resource snapshot");
        const auto verified = snapshot->generation > 0
            && isCanonicalSha256(snapshot->contentHash);
        const auto compatibility = snapshot->generation == 0
            && snapshot->contentHash.empty()
            && ((snapshot->kind
                        == talas::core::ResourceSnapshotKind::Table
                    && !snapshot->tableSamples.empty())
                || (snapshot->kind
                        == talas::core::ResourceSnapshotKind::Binary
                    && !snapshot->binaryBytes.empty()));
        if (snapshot->id.empty()
            || (!verified && !compatibility))
        {
            return fail(
                "Prepared graph summary received invalid resource identity for '"
                + snapshot->id + "'");
        }
        if (std::ranges::any_of(
                summary.resources,
                [&](const PreparedResourceSummary& candidate) {
                    return candidate.id == snapshot->id;
                }))
        {
            return fail(
                "Prepared graph summary received duplicate resource ID '"
                + snapshot->id + "'");
        }
        summary.resources.push_back({
            snapshot->id,
            snapshot->kind,
            snapshot->generation,
            snapshot->contentHash,
            verified
                ? PreparedResourceHealth::Verified
                : PreparedResourceHealth::Compatibility,
            snapshot->channelCount,
            snapshot->sampleRate,
            snapshot->frameCount,
            snapshot->encoding,
        });
    }

    summary.midiMappers.reserve(plan.midiMappers.size());
    std::size_t consumedMappedPlans = 0;
    for (std::size_t stepIndex = 0;
         stepIndex < plan.steps.size();
         ++stepIndex)
    {
        const auto& step = plan.steps[stepIndex];
        if (step.typeId != "talas.midiMapper")
            continue;
        const auto* map = findParameter(step, "map");
        if (map == nullptr || !map->text.has_value())
        {
            return fail(
                "Prepared graph summary cannot resolve MIDI Mapper mode for '"
                + step.nodeId + "'");
        }
        const auto parsed = parsePreparedMidiMapperMode(*map->text);
        if (!parsed.ok())
        {
            return fail(
                "Prepared graph summary received invalid MIDI Mapper mode for '"
                + step.nodeId + "'");
        }
        const auto* voiceModel = findParameter(step, "voiceModel");
        const auto polyphonicInput = voiceModel != nullptr
            && voiceModel->choice.has_value()
            && *voiceModel->choice == "polyphonicInput";
        const auto voicePlan = std::ranges::find_if(
            plan.midiVoices,
            [&](const MidiVoicePlan& candidate) {
                return candidate.ownerStepIndex == stepIndex;
            });
        if (parsed.localAssociationCount != 0)
        {
            const auto input = std::ranges::find(
                step.inputs,
                std::string_view{"audioIn"},
                &InputBinding::inputPortId);
            const auto rightInput = std::ranges::find(
                step.inputs,
                std::string_view{"rightIn"},
                &InputBinding::inputPortId);
            const auto findEvent = [&](RuntimeScopeId scope,
                                       std::string_view name) {
                return std::ranges::find_if(
                    settings.eventValues,
                    [&](const EventValueBinding& binding) {
                        return binding.scopeId == scope
                            && binding.name == name;
                    });
            };
            if ((!polyphonicInput
                    && (input == step.inputs.end()
                        || input->sourceScopeId == globalRuntimeScope
                        || input->sourceScopeId == step.scopeId
                        || (rightInput != step.inputs.end()
                            && rightInput->sourceScopeId
                                != input->sourceScopeId)))
                || (polyphonicInput
                    && (voicePlan == plan.midiVoices.end()
                        || input != step.inputs.end()
                        || rightInput != step.inputs.end())))
            {
                return fail(
                    "Prepared graph summary received an unscoped MIDI Mapper local Map for '"
                    + step.nodeId + "'");
            }
            for (std::size_t index = 0;
                 index < parsed.localAssociationCount; ++index)
            {
                const auto& association = parsed.localAssociations[index];
                const auto sourceName = resolvePreparedMidiMapperLocalSource(
                    association.localEventSource, association.channel);
                if (!sourceName)
                {
                    return fail(
                        "Prepared graph summary received an invalid MIDI Mapper local Map source for '"
                        + step.nodeId + "'");
                }
                const auto voiceSource =
                    association.localEventSource == "MIDIKeyDown"
                    || association.localEventSource == "MIDIKeyNumber"
                    || association.localEventSource == "MIDIKeyVelocity";
                const auto noteNumberSource =
                    association.localEventSource == "MIDIKeyNumber";
                const auto target = findEvent(
                    globalRuntimeScope, association.localEventValue);
                const auto source = voiceSource
                    ? settings.eventValues.end()
                    : findEvent(globalRuntimeScope, *sourceName);
                const auto bend = association.pitchBendSemitones
                    ? findEvent(globalRuntimeScope, "MIDIPitchBend")
                    : settings.eventValues.end();
                if (target == settings.eventValues.end()
                    || (voiceSource
                        ? (!polyphonicInput || association.hasRange()
                            || target->domain
                                != (noteNumberSource
                                    ? talas::expr::ValueDomain::NoteNumber
                                    : talas::expr::ValueDomain::Scalar))
                        : (source == settings.eventValues.end()
                            || (!association.hasRange()
                                && target->domain != source->domain)
                            || (association.hasRange()
                                && source->domain
                                    != talas::expr::ValueDomain::Scalar)))
                    || (association.pitchBendSemitones
                        && (bend == settings.eventValues.end()
                            || bend->domain != talas::expr::ValueDomain::Scalar
                            || bend->projectionExpression.has_value())))
                {
                    return fail(
                        "Prepared graph summary received a MIDI Mapper local Map without its exact scoped Event Value projection for '"
                        + step.nodeId + "'");
                }
                const auto projectionText =
                    preparedMidiMapperProjectionExpression(
                        association, *sourceName, target->domain);
                if (!projectionText)
                    return fail("Prepared graph summary received an invalid MIDI Mapper local Map range for '" + step.nodeId + "'");
                const auto expression = talas::expr::parse(
                    *projectionText,
                    settings.expressionLanguage,
                    settings.expressionDefinitions);
                if (!expression.ok() || !expression.expression.has_value())
                    return fail("Prepared graph summary received an invalid MIDI Mapper local Map projection for '" + step.nodeId + "'");
                const auto checkProjection = [&](RuntimeScopeId scope) {
                    const auto local = findEvent(
                        scope, association.localEventValue);
                    const auto scopedSource = findEvent(
                        voiceSource ? scope : globalRuntimeScope,
                        *sourceName);
                    const auto localCount = std::ranges::count_if(
                        settings.eventValues,
                        [&](const EventValueBinding& binding) {
                            return binding.scopeId == scope
                                && binding.name
                                    == association.localEventValue;
                        });
                    return local != settings.eventValues.end()
                        && scopedSource != settings.eventValues.end()
                        && (!voiceSource
                            || (scopedSource->domain
                                    == (noteNumberSource
                                        ? talas::expr::ValueDomain::NoteNumber
                                        : talas::expr::ValueDomain::Scalar)
                                && scopedSource->value
                                    == (noteNumberSource ? 60.0 : 0.0)
                                && !scopedSource->projectionExpression))
                        && localCount == 1
                        && local->domain == target->domain
                        && local->sourceScopeId
                            == (voiceSource ? scope : globalRuntimeScope)
                        && local->projectionExpression.has_value()
                        && local->projectionExpression->structurallyEquivalent(
                            *expression.expression);
                };
                if (polyphonicInput)
                {
                    for (const auto& voice : voicePlan->voices)
                    {
                        if (!checkProjection(voice.scopeId))
                        {
                            return fail(
                                "Prepared graph summary received a MIDI Mapper voice without its exact local Map projection for '"
                                + step.nodeId + "'");
                        }
                    }
                }
                else if (!checkProjection(input->sourceScopeId))
                {
                    return fail(
                        "Prepared graph summary received a MIDI Mapper local Map without its exact scoped Event Value projection for '"
                        + step.nodeId + "'");
                }
            }
        }

        const MidiMapperPlan* mapped = nullptr;
        std::size_t matchedPlans = 0;
        for (const auto& candidate : plan.midiMappers)
        {
            if (candidate.ownerStepIndex != stepIndex)
                continue;
            mapped = &candidate;
            ++matchedPlans;
        }
        if (matchedPlans > 1)
        {
            return fail(
                "Prepared graph summary received duplicate MIDI Mapper authority for '"
                + step.nodeId + "'");
        }

        const auto configured =
            preparedMidiMapperConfigurationForStep(step);
        if (!configured.has_value())
        {
            return fail(
                "Prepared graph summary received invalid MIDI Mapper configuration for '"
                + step.nodeId + "'");
        }
        auto configuration = *configured;
        if (parsed.mode == PreparedMidiMapperMode::Passthrough)
        {
            if (mapped != nullptr
                || (polyphonicInput
                    && (voicePlan == plan.midiVoices.end()
                        || voicePlan->voices.size()
                            != configuration.polyphony))
                || (!polyphonicInput
                    && voicePlan != plan.midiVoices.end()))
            {
                return fail(
                    "Prepared graph summary received mapped authority for blank MIDI Mapper '"
                    + step.nodeId + "'");
            }
        }
        else
        {
            if (polyphonicInput || mapped == nullptr
                || mapped->nodeId != step.nodeId
                || mapped->scopeId != step.scopeId
                || !samePreparedMidiMapperConfiguration(
                    mapped->configuration,
                    configuration))
            {
                return fail(
                    "Prepared graph summary received inconsistent MIDI Mapper authority for '"
                    + step.nodeId + "'");
            }
            ++consumedMappedPlans;
        }
        summary.midiMappers.push_back({
            step.nodeId,
            step.scopeId,
            step.structuralSourcePath,
            configuration,
            polyphonicInput,
        });
    }
    if (consumedMappedPlans != plan.midiMappers.size())
    {
        return fail(
            "Prepared graph summary received orphaned MIDI Mapper authority");
    }

    summary.spectralNodes.reserve(plan.steps.size());
    summary.streams.reserve(plan.steps.size());
    for (const auto& step : plan.steps)
    {
        PreparedSpectralNodeSummary spectral;
        spectral.nodeId = step.nodeId;
        spectral.typeId = step.typeId;
        spectral.scopeId = step.scopeId;
        spectral.structuralSourcePath =
            step.structuralSourcePath;
        spectral.intrinsicLatencyFrames =
            step.resolvedLatencyFrames;
        spectral.cumulativeInputLatencyFrames =
            step.cumulativeInputLatencyFrames;
        spectral.cumulativeOutputLatencyFrames =
            step.cumulativeOutputLatencyFrames;
        bool isSpectral = true;

        if (step.typeId == "talas.forwardFftV1"
            || step.typeId == "talas.inverseFftV1")
        {
            const auto configuration =
                detail::nativeStftConfiguration(step);
            const auto input = spectralInputAuthority(
                plan,
                step,
                step.typeId == "talas.forwardFftV1"
                    ? std::string_view{"audioIn"}
                    : std::string_view{"spectrumIn"});
            const auto sourceConfiguration = input.has_value()
                    && step.typeId == "talas.inverseFftV1"
                    && input->source->typeId == "talas.forwardFftV1"
                ? detail::nativeStftConfiguration(*input->source)
                : std::optional<talas::dsp::NativeStftConfiguration>{};
            if (!configuration.has_value()
                || !input.has_value()
                || step.inputs.size() != 1
                || !talas::dsp::isValidNativeStftConfiguration(
                    *configuration)
                || (step.typeId == "talas.inverseFftV1"
                    && (!sourceConfiguration.has_value()
                        || *sourceConfiguration != *configuration))
                || step.preparedSpectralFrameSize
                    != configuration->frameSize
                || step.preparedSpectralHopSize
                    != configuration->hopSize
                || !hasExactSpectralLatencyAuthority(
                    step,
                    *input,
                    step.typeId == "talas.forwardFftV1"
                        ? configuration->frameSize - 1
                        : std::size_t{0}))
            {
                return fail(
                    "Prepared graph summary received inconsistent native STFT authority for '"
                    + step.nodeId + "'");
            }
            spectral.format =
                PreparedSpectralFormat::ComplexSpectrumV1;
            spectral.role = step.typeId == "talas.forwardFftV1"
                ? PreparedSpectralRole::Forward
                : PreparedSpectralRole::Inverse;
            spectral.frameSize = configuration->frameSize;
            spectral.hopSize = configuration->hopSize;
            spectral.window = configuration->window;
            if (!addCadence(
                    spectral,
                    settings.sampleRate,
                    configuration->hopSize)
                || !appendLimitation(
                    spectral,
                    "native-stft-radix2-16-4096",
                    "Native complex STFT supports reviewed radix-2 frame sizes from 16 through 4096."))
            {
                return fail(
                    "Prepared graph summary could not publish native STFT contract for '"
                    + step.nodeId + "'");
            }
            if (configuration->window
                    == talas::dsp::SpectralWindowKind::Hann
                && !appendLimitation(
                    spectral,
                    "native-stft-hann-cola-hops",
                    "Periodic Hann synthesis is admitted only for the reviewed N/2 and N/4 COLA hops."))
            {
                return fail(
                    "Prepared graph summary could not publish native STFT limitations for '"
                    + step.nodeId + "'");
            }
        }
        else if (step.typeId == "talas.fft")
        {
            const auto configuration =
                detail::packedRealFftConfiguration(step);
            const auto input = spectralInputAuthority(
                plan, step, "audioIn");
            const auto rightInput = spectralInputAuthority(
                plan, step, "rightIn");
            // A stereo FFT's frames start from the later of its two
            // channels' latencies.
            const auto alignedInputLatency =
                detail::packedRealFftAlignedInputLatency(step, plan);
            const auto intrinsicFrames = configuration.has_value()
                ? configuration->frameSize - 1
                : std::size_t{0};
            // After an unknown input latency the frames count from the
            // FFT's own start and the output's latency is unknown too
            // (ADR-0011, 2026-10-10).
            const auto inputsFollowTheirSources = [&] {
                for (const auto* authority : {&input, &rightInput})
                {
                    if (!authority->has_value())
                        continue;
                    const auto bindingIndex = (*authority)->bindingIndex;
                    if (bindingIndex >= step.inputLatencyFrames.size()
                        || step.inputLatencyFrames[bindingIndex]
                            != (*authority)->source->cumulativeOutputLatencyFrames)
                    {
                        return false;
                    }
                }
                return true;
            };
            if (!configuration.has_value()
                || !input.has_value()
                || (rightInput.has_value()
                        ? !configuration->stereo || step.inputs.size() != 2
                        : step.inputs.size() != 1)
                || (!step.cumulativeInputLatencyFrames.has_value()
                        ? alignedInputLatency.has_value()
                            || step.resolvedLatencyFrames != intrinsicFrames
                            || step.cumulativeOutputLatencyFrames.has_value()
                            || !inputsFollowTheirSources()
                    : rightInput.has_value()
                        ? !alignedInputLatency.has_value()
                            || step.cumulativeInputLatencyFrames
                                != alignedInputLatency
                            || step.resolvedLatencyFrames != intrinsicFrames
                            || !step.cumulativeOutputLatencyFrames.has_value()
                            || intrinsicFrames
                                > std::numeric_limits<std::size_t>::max()
                                    - *alignedInputLatency
                            || *step.cumulativeOutputLatencyFrames
                                != *alignedInputLatency + intrinsicFrames
                        : !hasExactSpectralLatencyAuthority(
                            step,
                            *input,
                            intrinsicFrames)))
            {
                return fail(
                    "Prepared graph summary received inconsistent packed FFT authority for '"
                    + step.nodeId + "'");
            }
            spectral.format = configuration->stereo
                ? PreparedSpectralFormat::TalasStereoAudioV1
                : PreparedSpectralFormat::PackedRealAudioV1;
            spectral.role = configuration->inverse
                ? PreparedSpectralRole::Inverse
                : PreparedSpectralRole::Forward;
            spectral.frameSize = configuration->frameSize;
            spectral.hopSize = configuration->frameSize;
            spectral.window =
                talas::dsp::SpectralWindowKind::Rectangular;
            if (!addCadence(
                    spectral,
                    settings.sampleRate,
                    configuration->frameSize)
                || !(configuration->stereo
                        ? appendLimitation(
                            spectral,
                            "packed-stereo-audio-layout",
                            "talas.fft keeps Audio ports in the packed frame layout: the left and then the right channel's Length / 2 bins, real parts left and imaginary right, each Nyquist bin in its DC bin's imaginary place.")
                        : appendLimitation(
                            spectral,
                            "packed-real-audio-compatibility-layout",
                            "Historical talas.fft keeps Audio ports and uses the frozen packed-real frame layout, not ComplexSpectrumV1."))
                || !appendLimitation(
                    spectral,
                    "packed-real-radix2-or-identity",
                    "Packed FFT supports length one identity or reviewed radix-2 lengths from 16 through 4096."))
            {
                return fail(
                    "Prepared graph summary could not publish packed FFT contract for '"
                    + step.nodeId + "'");
            }
        }
        else if (step.typeId == "talas.liveSpectralAnalysis")
        {
            const auto configuration =
                detail::liveSpectralAnalysisConfiguration(
                    step, settings);
            const auto input = spectralInputAuthority(
                plan, step, "audioIn");
            if (!configuration.has_value()
                || !input.has_value()
                || step.preparedSpectralFrameSize
                    != configuration->stft.frameSize
                || step.preparedSpectralHopSize
                    != configuration->stft.hopSize
                || !hasExactSpectralLatencyAuthority(
                    step,
                    *input,
                    configuration->stft.frameSize - 1))
            {
                return fail(
                    "Prepared graph summary received inconsistent LiveSpectralAnalysis authority for '"
                    + step.nodeId + "'");
            }
            spectral.format =
                PreparedSpectralFormat::PartialSpectrumV1;
            spectral.role = PreparedSpectralRole::Analysis;
            spectral.frameSize = configuration->stft.frameSize;
            spectral.hopSize = configuration->stft.hopSize;
            spectral.window = configuration->stft.window;
            if (!addCadence(
                    spectral,
                    settings.sampleRate,
                    configuration->stft.hopSize)
                || !appendLimitation(
                    spectral,
                    "live-spectral-defined-partial-subset",
                    "LiveSpectralAnalysis emits bounded PartialSpectrumV1 peaks without response scripts, harmonic/unpitched classification, or stable track identity."))
            {
                return fail(
                    "Prepared graph summary could not publish LiveSpectralAnalysis contract for '"
                    + step.nodeId + "'");
            }
        }
        else if (step.typeId
                 == "talas.partialSpectrumLegacyHoldV1")
        {
            if (step.inputs.size() != 1
                || step.inputs.front().inputPortId
                    != "spectrumIn")
            {
                return fail(
                    "Prepared graph summary received invalid legacy partial hold authority for '"
                    + step.nodeId + "'");
            }
            const auto& input = step.inputs.front();
            const auto* source = findUniqueStep(
                plan, input.sourceNodeId, input.sourceScopeId);
            const auto inputAuthority = spectralInputAuthority(
                plan, step, "spectrumIn");
            const auto sourceConfiguration = source != nullptr
                    && source->typeId
                        == "talas.liveSpectralAnalysis"
                ? detail::liveSpectralAnalysisConfiguration(
                      *source, settings)
                : std::optional<
                      talas::dsp::LiveSpectralAnalysisConfiguration>{};
            if (!sourceConfiguration.has_value()
                || source->preparedSpectralFrameSize
                    != sourceConfiguration->stft.frameSize
                || source->preparedSpectralHopSize
                    != sourceConfiguration->stft.hopSize
                || !inputAuthority.has_value()
                || !hasExactSpectralLatencyAuthority(
                    step, *inputAuthority, 0))
            {
                return fail(
                    "Prepared graph summary cannot resolve legacy partial hold source for '"
                    + step.nodeId + "'");
            }
            spectral.format =
                PreparedSpectralFormat::LegacyPartialTriples;
            spectral.role =
                PreparedSpectralRole::CompatibilityHold;
            spectral.frameSize =
                sourceConfiguration->stft.frameSize;
            spectral.hopSize =
                sourceConfiguration->stft.hopSize;
            spectral.window = sourceConfiguration->stft.window;
            // Renderers split their blocks at each frame the analysis
            // completes, so the Hold presents every frame from its sample
            // on (ADR-0016, 2026-10-10).
            if (!addCadence(
                    spectral,
                    settings.sampleRate,
                    sourceConfiguration->stft.hopSize))
            {
                return fail(
                    "Prepared graph summary could not publish legacy partial hold contract for '"
                    + step.nodeId + "'");
            }
        }
        else
        {
            isSpectral = false;
        }
        if (isSpectral)
            summary.spectralNodes.push_back(std::move(spectral));

        if (step.typeId == "talas.sampleMapPlayer")
        {
            if (!step.sampleMapPlayback.has_value())
            {
                return fail(
                    "Prepared graph summary cannot resolve sample-map projection for '"
                    + step.nodeId + "'");
            }
            for (const auto& region :
                 step.sampleMapPlayback->regions)
            {
                if (std::ranges::none_of(
                        summary.resources,
                        [&](const PreparedResourceSummary& candidate) {
                            return candidate.id == region.resourceId
                                && candidate.kind
                                    == talas::core::ResourceSnapshotKind::Audio
                                && candidate.health
                                    == PreparedResourceHealth::Verified;
                        }))
                {
                    return fail(
                        "Prepared graph summary sample-map region '"
                        + region.regionId
                        + "' references an unverified Audio resource");
                }
                PreparedStreamContractSummary stream;
                stream.nodeId = step.nodeId;
                stream.typeId = step.typeId;
                stream.scopeId = step.scopeId;
                stream.structuralSourcePath =
                    region.structuralSourcePath;
                stream.resourceId = region.resourceId;
                stream.role = PreparedStreamRole::Playback;
                if (!appendLimitation(
                        stream,
                        "stream-mono-or-stereo-wav-or-aiff-only",
                        "Prepared streaming plays mono or stereo WAV and AIFF Audio snapshots.")
                    || !appendLimitation(
                        stream,
                        "sample-map-ordered-region-layer",
                        "Sample Map Player retains this ordered map region as one prepared layer source."))
                {
                    return fail(
                        "Prepared graph summary could not publish sample-map stream contract for '"
                        + step.nodeId + "'");
                }
                summary.streams.push_back(std::move(stream));
            }
            continue;
        }

        if (step.typeId != "talas.diskPlayer"
            && step.typeId != "talas.diskCache")
        {
            continue;
        }
        const auto* fileName = findParameter(step, "fileName");
        if (fileName == nullptr
            || !fileName->resource.has_value()
            || fileName->resource->empty())
        {
            return fail(
                "Prepared graph summary cannot resolve stream resource for '"
                + step.nodeId + "'");
        }
        const auto resource = std::ranges::find_if(
            summary.resources,
            [&](const PreparedResourceSummary& candidate) {
                return candidate.id == *fileName->resource
                    && candidate.kind
                        == talas::core::ResourceSnapshotKind::Audio
                    && candidate.health
                        == PreparedResourceHealth::Verified;
            });
        if (resource == summary.resources.end())
        {
            return fail(
                "Prepared graph summary stream '" + step.nodeId
                + "' references an unverified Audio resource");
        }
        const auto diskCacheSnapshot = std::ranges::find_if(
            settings.resourceSnapshots,
            [&](const auto& candidate) {
                return candidate != nullptr
                    && candidate->id == *fileName->resource;
            });
        const auto capturedSnapshot = step.typeId == "talas.diskCache"
            && diskCacheSnapshot != settings.resourceSnapshots.end()
            && (*diskCacheSnapshot)->provenance.starts_with(
                talas::project::diskCacheCaptureProvenanceNamespace);
        if (step.typeId == "talas.diskCache"
            && capturedSnapshot != step.diskCachePlayback.has_value())
        {
            return fail(
                "Prepared graph summary received a forged or stale DiskCache captured-playback plan for '"
                + step.nodeId + "'");
        }
        if (step.diskCachePlayback.has_value())
        {
            const auto& playback = *step.diskCachePlayback;
            const auto snapshot = std::ranges::find_if(
                settings.resourceSnapshots,
                [&](const auto& candidate) {
                    return candidate != nullptr
                        && candidate->id == playback.assetId;
                });
            if (step.typeId != "talas.diskCache"
                || playback.assetId != *fileName->resource
                || playback.generation == 0
                || playback.generation
                    > talas::project::maximumProjectAssetJsonInteger
                || playback.generation != resource->generation
                || snapshot == settings.resourceSnapshots.end()
                || !talas::project::isValidDiskCacheAssetPath(
                    playback.assetPath)
                || playback.assetPath
                    != (*snapshot)->projectAssetPath
                || !isCanonicalSha256(playback.contentHash)
                || playback.contentHash != resource->contentHash
                || playback.channelCount != 1
                || playback.channelCount != resource->channelCount
                || playback.sampleRate != summary.sampleRate
                || playback.sampleRate != resource->sampleRate
                || playback.frameCount == 0
                || playback.frameCount != resource->frameCount
                || playback.encoding != "pcm16-wav"
                || playback.encoding != resource->encoding
                || !talas::project::isCanonicalDiskCacheCaptureFingerprint(
                    playback.sourceFingerprint)
                || playback.provenance
                    != talas::project::makeDiskCacheCaptureProvenance(
                        playback.sourceFingerprint)
                || playback.provenance
                    != (*snapshot)->provenance)
            {
                return fail(
                    "Prepared graph summary received a forged or stale DiskCache captured-playback plan for '"
                    + step.nodeId + "'");
            }
        }
        PreparedStreamContractSummary stream;
        stream.nodeId = step.nodeId;
        stream.typeId = step.typeId;
        stream.scopeId = step.scopeId;
        stream.structuralSourcePath =
            step.structuralSourcePath;
        stream.resourceId = *fileName->resource;
        stream.role = step.typeId == "talas.diskPlayer"
            || step.diskCachePlayback.has_value()
            ? PreparedStreamRole::Playback
            : PreparedStreamRole::CachePrewarm;
        if (!appendLimitation(
                stream,
                "stream-mono-or-stereo-wav-or-aiff-only",
                "Prepared streaming plays mono or stereo WAV and AIFF Audio snapshots."))
        {
            return fail(
                "Prepared graph summary could not publish stream limitation for '"
                + step.nodeId + "'");
        }
        if (step.typeId == "talas.diskPlayer")
        {
            if (!appendLimitation(
                    stream,
                    "stream-positive-rate-linear-interpolation",
                    "DiskPlayer streaming uses positive-rate linear interpolation and deterministic mono reduction."))
            {
                return fail(
                    "Prepared graph summary could not publish DiskPlayer limitations for '"
                    + step.nodeId + "'");
            }
        }
        else if (step.diskCachePlayback.has_value())
        {
            if (!appendLimitation(
                    stream,
                    "disk-cache-captured-pcm16-sequential-playback",
                    "Captured DiskCache playback ignores live input, starts at frame zero, and zero-fills after immutable PCM16-WAV EOF.")
                || !appendLimitation(
                    stream,
                    "disk-cache-record-explicit-operation-only",
                    "DiskCache record=true is available only through the explicit bounded capture operation."))
            {
                return fail(
                    "Prepared graph summary could not publish captured DiskCache limitations for '"
                    + step.nodeId + "'");
            }
        }
        else if (!appendLimitation(
                     stream,
                     "disk-cache-read-prewarm-only",
                     "Legacy DiskCache supports record=false read-cache prewarm passthrough only."))
        {
            return fail(
                "Prepared graph summary could not publish DiskCache limitations for '"
                + step.nodeId + "'");
        }
        summary.streams.push_back(std::move(stream));
    }

    return {std::move(summary), {}};
}
}

PreparedGraphSummaryResult
makePreparedGraphSummary(const PreparedRuntimeGraph& graph)
{
    try
    {
        return summarize(graph);
    }
    catch (const std::bad_alloc&)
    {
        return fail("Prepared graph summary allocation failed");
    }
    catch (const std::length_error&)
    {
        return fail("Prepared graph summary exceeded supported limits");
    }
}

std::string_view
preparedOutputLayoutName(PreparedOutputLayout layout) noexcept
{
    switch (layout)
    {
    case PreparedOutputLayout::None:
        return "none";
    case PreparedOutputLayout::Mono:
        return "mono";
    case PreparedOutputLayout::Stereo:
        return "stereo";
    case PreparedOutputLayout::Discrete:
        return "discrete";
    }
    return "unknown";
}

std::string_view
preparedSpectralFormatName(PreparedSpectralFormat format) noexcept
{
    switch (format)
    {
    case PreparedSpectralFormat::ComplexSpectrumV1:
        return "ComplexSpectrumV1";
    case PreparedSpectralFormat::PackedRealAudioV1:
        return "PackedRealAudioV1";
    case PreparedSpectralFormat::PartialSpectrumV1:
        return "PartialSpectrumV1";
    case PreparedSpectralFormat::LegacyPartialTriples:
        return "LegacyPartialTriples";
    case PreparedSpectralFormat::TalasStereoAudioV1:
        return "TalasStereoAudioV1";
    }
    return "unknown";
}

std::string_view
preparedSpectralRoleName(PreparedSpectralRole role) noexcept
{
    switch (role)
    {
    case PreparedSpectralRole::Forward:
        return "forward";
    case PreparedSpectralRole::Inverse:
        return "inverse";
    case PreparedSpectralRole::Analysis:
        return "analysis";
    case PreparedSpectralRole::CompatibilityHold:
        return "compatibility-hold";
    }
    return "unknown";
}

std::string_view
preparedResourceKindName(
    talas::core::ResourceSnapshotKind kind) noexcept
{
    switch (kind)
    {
    case talas::core::ResourceSnapshotKind::Table:
        return "table";
    case talas::core::ResourceSnapshotKind::Binary:
        return "binary";
    case talas::core::ResourceSnapshotKind::Audio:
        return "audio";
    case talas::core::ResourceSnapshotKind::PartialSpectrum:
        return "partial-spectrum";
    }
    return "unknown";
}

std::string_view
preparedResourceHealthName(
    PreparedResourceHealth health) noexcept
{
    switch (health)
    {
    case PreparedResourceHealth::Verified:
        return "verified";
    case PreparedResourceHealth::Compatibility:
        return "compatibility";
    case PreparedResourceHealth::Invalid:
        return "invalid";
    }
    return "unknown";
}

std::string_view
preparedStreamRoleName(PreparedStreamRole role) noexcept
{
    switch (role)
    {
    case PreparedStreamRole::Playback:
        return "playback";
    case PreparedStreamRole::CachePrewarm:
        return "cache-prewarm";
    }
    return "unknown";
}

std::string_view
preparedMidiMapperModeName(PreparedMidiMapperMode mode) noexcept
{
    switch (mode)
    {
    case PreparedMidiMapperMode::Passthrough:
        return "blank passthrough";
    case PreparedMidiMapperMode::Gate:
        return "gate";
    case PreparedMidiMapperMode::Velocity:
        return "velocity";
    }
    return "unknown";
}
}
