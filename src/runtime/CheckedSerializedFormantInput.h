#pragma once

#include "talas/runtime/Runtime.h"
#include "CheckedSerializedSpectrumSource.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>

namespace talas::runtime::checked_serialized_formant
{
struct Source
{
    std::size_t partialCount = 0;
    std::size_t amplitudeBuffer = 0;
    std::size_t frequencyBuffer = 0;
    std::optional<std::size_t> countBuffer;
};

[[nodiscard]] inline bool hasExactInputs(const CompileStep& step, bool serialized)
{
    std::array<std::string_view, 5> expected{};
    std::size_t count = 0;
    if (!serialized)
        expected[count++] = "spectrum";
    if (step.typeId == "talas.formantBankOscillator")
        expected[count++] = "cascadeInput";
    if (std::ranges::any_of(step.inputs, [](const InputBinding& binding) {
            return binding.inputPortId == "frequency";
        }))
        expected[count++] = "frequency";
    if (serialized)
    {
        expected[count++] = "amplitudeIn";
        expected[count++] = "frequencyIn";
    }
    return step.inputs.size() == count
        && std::equal(step.inputs.begin(), step.inputs.end(), expected.begin(),
            [](const InputBinding& binding, std::string_view id) {
                return binding.inputPortId == id;
            });
}

// The stream has no embedded count or encoding tag. Admit a direct pair from
// one scoped source so selection cannot accidentally change its frame period.
// The bandwidth-enabled lane alignment is not established by the reference.
[[nodiscard]] inline std::optional<Source> source(
    const RuntimePlan& plan, const CompileStep& consumer)
{
    const auto input = [&](std::string_view id) -> const InputBinding* {
        if (std::ranges::count(consumer.inputs, id, &InputBinding::inputPortId) != 1)
            return nullptr;
        return &*std::ranges::find(consumer.inputs, id, &InputBinding::inputPortId);
    };
    const auto* amplitude = input("amplitudeIn");
    const auto* frequency = input("frequencyIn");
    if (amplitude == nullptr || frequency == nullptr
        || amplitude->sourceNodeId != frequency->sourceNodeId
        || amplitude->sourceScopeId != frequency->sourceScopeId
        || amplitude->sourcePortId != "amplitudeOut"
        || frequency->sourcePortId != "frequencyOut")
        return std::nullopt;

    if (consumer.typeId == "talas.formantBank")
    {
        const auto metadata = checked_serialized_spectrum_source::resolve(
            plan, *amplitude, *frequency);
        if (!metadata.has_value() || !metadata->countBuffer.has_value())
            return std::nullopt;
        return Source{0, amplitude->bufferIndex, frequency->bufferIndex,
            metadata->countBuffer};
    }

    for (const auto* binding : {amplitude, frequency})
    {
        if (binding->bufferIndex >= plan.buffers.size())
            return std::nullopt;
        const auto& buffer = plan.buffers[binding->bufferIndex];
        if (buffer.nodeId != binding->sourceNodeId
            || buffer.scopeId != binding->sourceScopeId
            || buffer.portId != binding->sourcePortId
            || buffer.type != talas::core::PortType::Audio)
            return std::nullopt;
    }

    const CompileStep* producer = nullptr;
    for (const auto& step : plan.steps)
    {
        if (step.nodeId == amplitude->sourceNodeId
            && step.scopeId == amplitude->sourceScopeId)
        {
            if (producer != nullptr)
                return std::nullopt;
            producer = &step;
        }
    }
    if (producer == nullptr
        || (producer->typeId != "talas.syntheticSpectrumFromArray"
            && producer->typeId != "talas.syntheticSpectrumFromSounds")
        || std::ranges::any_of(producer->inputs, [](const InputBinding& binding) {
            return binding.inputPortId == "nbrPartials";
        }))
        return std::nullopt;

    const auto parameter = [&](std::string_view id) -> const ParameterValue* {
        if (std::ranges::count(producer->parameters, id, &ParameterValue::id) != 1)
            return nullptr;
        return &*std::ranges::find(producer->parameters, id, &ParameterValue::id);
    };
    const auto* count = parameter("nbrPartials");
    const auto* logarithmic = parameter("logScale");
    const auto* bandwidths = parameter(producer->typeId == "talas.syntheticSpectrumFromArray"
        ? "sendBandwidths" : "includesBandwidths");
    if (count == nullptr || !std::isfinite(count->value)
        || count->value < 1.0 || count->value > 4096.0
        || std::floor(count->value) != count->value
        || !count->expression.has_value()
        || count->expression->isStateful() || count->expression->usesTime()
        || !count->expression->eventReferences().empty()
        || !count->expression->variableReferences().empty()
        || !count->variableCellIndices.empty()
        || logarithmic == nullptr || !logarithmic->boolean.has_value()
        || *logarithmic->boolean
        || bandwidths == nullptr || !bandwidths->boolean.has_value()
        || *bandwidths->boolean)
        return std::nullopt;

    return Source{static_cast<std::size_t>(count->value),
        amplitude->bufferIndex, frequency->bufferIndex, std::nullopt};
}

// Interleaved retained amplitude/frequency pairs, initially zero. Updating on
// the arriving sample is an explicit native convention; widths remain zero.
[[nodiscard]] inline bool accept(
    double sampleRate, double amplitude, double frequency,
    std::span<double> pairs, std::size_t& phase, std::size_t sourceCount) noexcept
{
    if (pairs.empty() || pairs.size() % 2 != 0 || pairs.size() > 8192
        || sourceCount == 0 || sourceCount > 4096
        || pairs.size() / 2 > sourceCount || phase >= sourceCount
        || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(amplitude) || !std::isfinite(frequency)
        || frequency < 0.0 || frequency >= sampleRate * 0.5)
        return false;
    if (phase < pairs.size() / 2)
    {
        pairs[phase * 2] = amplitude;
        pairs[phase * 2 + 1] = frequency;
    }
    if (++phase == sourceCount)
        phase = 0;
    return true;
}
}
