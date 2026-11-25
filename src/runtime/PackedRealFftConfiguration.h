#pragma once

#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>

namespace talas::runtime::detail
{
[[nodiscard]] inline const ParameterValue* findPackedRealFftParameter(
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

[[nodiscard]] inline std::optional<talas::dsp::PackedRealFftConfiguration>
packedRealFftConfiguration(const CompileStep& step) noexcept
{
    const auto* length = findPackedRealFftParameter(step, "length");
    const auto* inverse = findPackedRealFftParameter(step, "inverse");
    const auto* packing = findPackedRealFftParameter(step, "packing");
    if (step.parameters.size() != 3
        || length == nullptr || inverse == nullptr || packing == nullptr
        || !packing->choice.has_value()
        || (*packing->choice != "talasStereo"
            && *packing->choice != "legacyPacked")
        || packing->boolean.has_value() || packing->resource.has_value()
        || packing->text.has_value() || packing->expression.has_value()
        || length->choice.has_value() || length->boolean.has_value()
        || length->resource.has_value() || length->text.has_value()
        || inverse->choice.has_value() || !inverse->boolean.has_value()
        || inverse->resource.has_value() || inverse->text.has_value()
        || inverse->expression.has_value()
        || !std::isfinite(length->value)
        || length->value != std::round(length->value)
        || length->value < 0.0
        || static_cast<long double>(length->value)
            >= static_cast<long double>(
                std::numeric_limits<std::size_t>::max()))
    {
        return std::nullopt;
    }

    talas::dsp::PackedRealFftConfiguration result{
        static_cast<std::size_t>(length->value),
        *inverse->boolean,
        *packing->choice == "talasStereo",
    };
    return talas::dsp::isValidPackedRealFftConfiguration(result)
        ? std::optional{result}
        : std::nullopt;
}

// The FFT reads its Input and, in the stereo packing, an optional right
// Input. Each binding reads its source's own audio buffer or, when the two
// arrive with different latencies, the prepared compensation that delays it
// to the later one.
[[nodiscard]] inline bool hasAuthoritativePackedRealFftAudioInput(
    const RuntimePlan& plan,
    const CompileStep& step,
    const CompileStep& sourceStep,
    const InputBinding& input,
    std::size_t bindingIndex,
    const talas::core::NodeRegistry& registry) noexcept
{
    const auto configuration = packedRealFftConfiguration(step);
    const auto expectedInputs = std::ranges::count_if(
        step.inputs,
        [](const InputBinding& binding) {
            return binding.inputPortId == "rightIn";
        }) == 1 ? std::size_t{2} : std::size_t{1};
    if (!configuration.has_value()
        || step.inputs.size() != expectedInputs
        || (expectedInputs == 2 && !configuration->stereo)
        || std::ranges::count_if(
               step.inputs,
               [](const InputBinding& binding) {
                   return binding.inputPortId == "audioIn";
               }) != 1
        || step.inputLatencyFrames.size() != step.inputs.size()
        || bindingIndex >= step.inputs.size()
        || &step.inputs[bindingIndex] != &input
        || (input.inputPortId != "audioIn"
            && input.inputPortId != "rightIn")
        || sourceStep.nodeId != input.sourceNodeId
        || sourceStep.scopeId != input.sourceScopeId
        || input.bufferIndex >= plan.buffers.size())
    {
        return false;
    }

    const auto* sourceDescriptor = registry.findNodeType(sourceStep.typeId);
    if (sourceDescriptor == nullptr
        || std::ranges::none_of(
            sourceDescriptor->outputs,
            [&](const talas::core::PortDescriptor& output) {
                return output.id == input.sourcePortId
                    && output.type == talas::core::PortType::Audio;
            }))
    {
        return false;
    }

    const auto isEquivalentAudioBuffer = [](
        const BufferSpec& left,
        const BufferSpec& right) noexcept {
        return left.nodeId == right.nodeId
            && left.portId == right.portId
            && left.type == right.type
            && left.frames == right.frames
            && left.scopeId == right.scopeId
            && left.complexFrameCapacity
                == right.complexFrameCapacity
            && left.complexMaximumBinsPerFrame
                == right.complexMaximumBinsPerFrame
            && left.complexChannelCount
                == right.complexChannelCount
            && left.partialFrameCapacity
                == right.partialFrameCapacity
            && left.partialMaximumPartialsPerFrame
                == right.partialMaximumPartialsPerFrame;
    };
    const auto preparedLatencyCompensation = [
        &plan,
        &isEquivalentAudioBuffer](std::size_t bufferIndex) noexcept
        -> const LatencyCompensationPlan* {
        const LatencyCompensationPlan* matching = nullptr;
        for (const auto& compensation : plan.latencyCompensations)
        {
            if (compensation.outputBufferIndex != bufferIndex)
                continue;
            if (matching != nullptr)
                return nullptr;
            matching = &compensation;
        }
        if (matching == nullptr
            || matching->targetStepIndex >= plan.steps.size()
            || matching->inputBindingIndex
                >= plan.steps[matching->targetStepIndex].inputs.size()
            || matching->sourceBufferIndex >= plan.buffers.size()
            || matching->outputBufferIndex >= plan.buffers.size()
            || matching->sourceBufferIndex
                == matching->outputBufferIndex
            || matching->delayFrames == 0
            || plan.steps[matching->targetStepIndex]
                    .inputs[matching->inputBindingIndex]
                    .bufferIndex
                != matching->outputBufferIndex)
        {
            return nullptr;
        }
        const auto& source = plan.buffers[matching->sourceBufferIndex];
        const auto& output = plan.buffers[matching->outputBufferIndex];
        return source.type == talas::core::PortType::Audio
                && isEquivalentAudioBuffer(source, output)
            ? matching
            : nullptr;
    };

    std::optional<std::size_t> authoritativeBufferIndex;
    for (std::size_t index = 0; index < plan.buffers.size(); ++index)
    {
        const auto& buffer = plan.buffers[index];
        if (buffer.nodeId != input.sourceNodeId
            || buffer.portId != input.sourcePortId
            || buffer.scopeId != input.sourceScopeId)
        {
            continue;
        }
        if (preparedLatencyCompensation(index) != nullptr)
            continue;
        if (authoritativeBufferIndex.has_value())
            return false;
        authoritativeBufferIndex = index;
    }
    if (!authoritativeBufferIndex.has_value()
        || plan.buffers[*authoritativeBufferIndex].type
            != talas::core::PortType::Audio)
    {
        return false;
    }
    if (*authoritativeBufferIndex == input.bufferIndex)
        return true;

    // A compensated channel: only a stereo FFT has two to align.
    const auto* compensation = preparedLatencyCompensation(input.bufferIndex);
    const auto inputLatency = step.inputLatencyFrames[bindingIndex];
    return compensation != nullptr
        && expectedInputs == 2
        && compensation->sourceBufferIndex == *authoritativeBufferIndex
        && &plan.steps[compensation->targetStepIndex] == &step
        && compensation->inputBindingIndex == bindingIndex
        && inputLatency.has_value()
        && step.cumulativeInputLatencyFrames.has_value()
        && *step.cumulativeInputLatencyFrames > *inputLatency
        && compensation->delayFrames
            == *step.cumulativeInputLatencyFrames - *inputLatency;
}

// The latest of the FFT's input latencies, which its frames start from: the
// Input's, or for a stereo FFT with a right Input the later of the two.
[[nodiscard]] inline std::optional<std::size_t>
packedRealFftAlignedInputLatency(
    const CompileStep& step,
    const RuntimePlan& plan) noexcept
{
    std::optional<std::size_t> latest;
    for (std::size_t index = 0; index < step.inputs.size(); ++index)
    {
        const auto& binding = step.inputs[index];
        const auto source = std::ranges::find_if(
            plan.steps,
            [&](const CompileStep& candidate) {
                return candidate.nodeId == binding.sourceNodeId
                    && candidate.scopeId == binding.sourceScopeId;
            });
        if (source == plan.steps.end()
            || index >= step.inputLatencyFrames.size()
            || !source->cumulativeOutputLatencyFrames.has_value()
            || step.inputLatencyFrames[index]
                != source->cumulativeOutputLatencyFrames)
        {
            return std::nullopt;
        }
        latest = std::max(
            latest.value_or(0),
            *source->cumulativeOutputLatencyFrames);
    }
    return latest;
}

// Where a packed FFT's Input's content starts, after the latest point at
// which the latency became unknown: an unknown latency counts as zero and
// the known latencies after it (such as a forward FFT's before an inverse
// one) still count.
[[nodiscard]] inline std::size_t packedRealFftRelativeInputLatency(
    const CompileStep& step,
    const RuntimePlan& plan,
    std::size_t depth = 0) noexcept;

[[nodiscard]] inline std::size_t packedRealFftRelativeOutputLatency(
    const CompileStep& step,
    const RuntimePlan& plan,
    std::size_t depth) noexcept
{
    if (step.cumulativeOutputLatencyFrames.has_value())
        return *step.cumulativeOutputLatencyFrames;
    // A step whose own latency is unknown is where the latency became so.
    if (!step.resolvedLatencyFrames.has_value() || depth > plan.steps.size())
        return 0;
    const auto input = packedRealFftRelativeInputLatency(step, plan, depth + 1);
    return input > std::numeric_limits<std::size_t>::max() - *step.resolvedLatencyFrames
        ? std::numeric_limits<std::size_t>::max()
        : input + *step.resolvedLatencyFrames;
}

[[nodiscard]] inline std::size_t packedRealFftRelativeInputLatency(
    const CompileStep& step,
    const RuntimePlan& plan,
    std::size_t depth) noexcept
{
    if (step.cumulativeInputLatencyFrames.has_value())
        return *step.cumulativeInputLatencyFrames;
    std::size_t latest = 0;
    for (const auto& binding : step.inputs)
    {
        const auto source = std::ranges::find_if(
            plan.steps,
            [&](const CompileStep& candidate) {
                return candidate.nodeId == binding.sourceNodeId
                    && candidate.scopeId == binding.sourceScopeId;
            });
        if (source != plan.steps.end())
            latest = std::max(latest,
                packedRealFftRelativeOutputLatency(*source, plan, depth + 1));
    }
    return latest;
}

// A packed FFT's phase reference: its frames start where its Input's
// content starts, which its input latency gives. After an unknown latency
// they count from the FFT's own start, or, for an FFT behind
// known latencies that follow the unknown one, from where those put the
// content (ADR-0011, 2026-10-10).
[[nodiscard]] inline std::size_t packedRealFftPhaseReferenceFrames(
    const CompileStep& step,
    const RuntimePlan& plan) noexcept
{
    return packedRealFftRelativeInputLatency(step, plan);
}
}
