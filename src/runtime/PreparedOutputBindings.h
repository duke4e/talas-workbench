#pragma once

#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::runtime::prepared_output_bindings
{
struct Port
{
    std::string_view id;
    talas::core::PortType type;
};

template<std::size_t Size>
[[nodiscard]] constexpr auto portsOfType(const std::array<std::string_view, Size>& ids, talas::core::PortType type)
{
    std::array<Port, Size> ports{};
    for (std::size_t index = 0; index < Size; ++index) ports[index] = {ids[index], type};
    return ports;
}

// Classification within an already authenticated plan, not execution
// authority: a latency compensation or a signal adapter writes a copy of
// another buffer.
[[nodiscard]] inline bool isDeclaredCompensationOutput(const RuntimePlan& plan, std::size_t index) noexcept
{
    return std::ranges::any_of(plan.latencyCompensations, [index](const auto& compensation) {
            return compensation.outputBufferIndex == index;
        })
        || std::ranges::any_of(plan.signalAdapters, [index](const auto& adapter) {
            return adapter.outputBufferIndex == index;
        });
}

// `buffer` must be an element of `plan.buffers`.
[[nodiscard]] inline bool isOriginalBuffer(const RuntimePlan& plan, const BufferSpec& buffer) noexcept
{
    return !isDeclaredCompensationOutput(
        plan, static_cast<std::size_t>(&buffer - plan.buffers.data()));
}

[[nodiscard]] inline bool matchesOriginalBuffer(const RuntimePlan& plan, const CompileStep& step,
    const Port& port, std::size_t index, std::size_t minimumFrames) noexcept
{
    if (index >= plan.buffers.size()) return false;
    const auto& buffer = plan.buffers[index];
    return buffer.nodeId == step.nodeId && buffer.scopeId == step.scopeId
        && buffer.portId == port.id && buffer.type == port.type && buffer.frames >= minimumFrames
        && !isDeclaredCompensationOutput(plan, index);
}

// Compare declarations with the buffers the admitted adapter actually writes.
// This is not graph/compiler authority. Keep the adapter's existing authentication.
// Missing metadata preserves legacy hand-built plans; compiler-owned snapshots
// separately reject removal or mutation of records from promoted graphs.
[[nodiscard]] inline bool validate(const RuntimePlan& plan, const CompileStep& step,
    std::span<const Port> ports, std::span<const std::size_t> actual, std::size_t maximumFrames) noexcept
{
    if (ports.size() != actual.size()
        || (!step.outputs.empty() && step.outputs.size() != ports.size())) return false;
    for (std::size_t position = 0; position < ports.size(); ++position)
    {
        const auto index = actual[position];
        if (!matchesOriginalBuffer(plan, step, ports[position], index, maximumFrames)
            || plan.buffers[index].frames != maximumFrames) return false;
        if (!step.outputs.empty()
            && (step.outputs[position].outputPortId != ports[position].id
                || step.outputs[position].bufferIndex != index)) return false;
        for (std::size_t previous = 0; previous < position; ++previous)
            if (actual[previous] == index || ports[previous].id == ports[position].id) return false;
    }
    return true;
}

// Declared ports and currently written ports differ for selectable layouts.
// Authenticate declarations when present, but publish only actual active slots.
// Legacy plans can omit inactive buffers and use nonuniform larger capacities.
[[nodiscard]] inline bool validateActive(const RuntimePlan& plan, const CompileStep& step,
    std::span<const Port> declared, std::span<const Port> active,
    std::span<const std::size_t> actual, std::size_t minimumFrames) noexcept
{
    if (active.size() != actual.size()
        || (!step.outputs.empty() && step.outputs.size() != declared.size())) return false;
    for (std::size_t position = 0; position < declared.size(); ++position)
    {
        for (std::size_t previous = 0; previous < position; ++previous)
            if (declared[previous].id == declared[position].id) return false;
        if (!step.outputs.empty())
        {
            const auto& output = step.outputs[position];
            if (output.outputPortId != declared[position].id
                || !matchesOriginalBuffer(plan, step, declared[position], output.bufferIndex, minimumFrames)) return false;
        }
    }
    for (std::size_t position = 0; position < active.size(); ++position)
    {
        const auto port = std::ranges::find(declared, active[position].id, &Port::id);
        if (port == declared.end() || port->type != active[position].type
            || !matchesOriginalBuffer(plan, step, active[position], actual[position], minimumFrames)) return false;
        if (!step.outputs.empty()
            && step.outputs[static_cast<std::size_t>(port - declared.begin())].bufferIndex != actual[position]) return false;
        for (std::size_t previous = 0; previous < position; ++previous)
            if (actual[previous] == actual[position] || active[previous].id == active[position].id) return false;
    }
    return true;
}

// Structural validation only; adapters retain execution and history authority.
[[nodiscard]] inline bool validateCompensation(const RuntimePlan& plan,
    std::size_t compensationIndex, std::size_t actualOutput, std::size_t maximumFrames) noexcept
{
    if (compensationIndex >= plan.latencyCompensations.size()) return false;
    const auto& compensation = plan.latencyCompensations[compensationIndex];
    if (actualOutput != compensation.outputBufferIndex || actualOutput >= plan.buffers.size()
        || compensation.sourceBufferIndex >= plan.buffers.size()
        || compensation.sourceBufferIndex == actualOutput || compensation.delayFrames == 0
        || compensation.targetStepIndex >= plan.steps.size()) return false;
    const auto& target = plan.steps[compensation.targetStepIndex];
    if (compensation.inputBindingIndex >= target.inputs.size()
        || target.inputs[compensation.inputBindingIndex].bufferIndex != actualOutput) return false;
    const auto& source = plan.buffers[compensation.sourceBufferIndex];
    const auto& output = plan.buffers[actualOutput];
    if (source.type != talas::core::PortType::Audio || output.type != source.type
        || source.frames < maximumFrames || output.frames < maximumFrames) return false;
    if (std::ranges::count(plan.latencyCompensations, actualOutput,
            &LatencyCompensationPlan::outputBufferIndex) != 1) return false;
    for (const auto& step : plan.steps)
        if (std::ranges::any_of(step.outputs, [actualOutput](const auto& original) {
                return original.bufferIndex == actualOutput;
            })) return false;
    return true;
}

// A signal adapter copies a source buffer of the same node, port and scope
// into the other of the Audio and Control types, for the input it feeds
// directly or through that input's latency compensation.
[[nodiscard]] inline bool validateAdapter(const RuntimePlan& plan,
    const SignalAdapterPlan& adapter, std::size_t maximumFrames) noexcept
{
    const auto signal = [](talas::core::PortType type) {
        return type == talas::core::PortType::Audio || type == talas::core::PortType::Control;
    };
    if (adapter.sourceBufferIndex >= plan.buffers.size()
        || adapter.outputBufferIndex >= plan.buffers.size()
        || adapter.sourceBufferIndex == adapter.outputBufferIndex
        || adapter.targetStepIndex >= plan.steps.size()
        || adapter.inputBindingIndex >= plan.steps[adapter.targetStepIndex].inputs.size()) return false;
    const auto& source = plan.buffers[adapter.sourceBufferIndex];
    const auto& output = plan.buffers[adapter.outputBufferIndex];
    const auto bound = plan.steps[adapter.targetStepIndex].inputs[adapter.inputBindingIndex].bufferIndex;
    const auto compensated = std::ranges::any_of(plan.latencyCompensations, [&](const auto& compensation) {
        return compensation.outputBufferIndex == bound
            && compensation.sourceBufferIndex == adapter.outputBufferIndex;
    });
    return signal(source.type) && signal(output.type) && source.type != output.type
        && source.nodeId == output.nodeId && source.portId == output.portId
        && source.scopeId == output.scopeId
        && source.frames >= maximumFrames && output.frames >= maximumFrames
        && (bound == adapter.outputBufferIndex || compensated)
        && std::ranges::count(plan.signalAdapters, adapter.outputBufferIndex,
               &SignalAdapterPlan::outputBufferIndex) == 1;
}

// Count all original slots, including untracked ones. A synthetic declaration
// may exclude a slot only when its compensation structure is consistent.
[[nodiscard]] inline std::optional<std::size_t> countOriginalOutputs(const RuntimePlan& plan,
    const CompileStep& step, std::size_t maximumFrames) noexcept
{
    std::size_t count = 0;
    for (std::size_t index = 0; index < plan.buffers.size(); ++index)
    {
        const auto& buffer = plan.buffers[index];
        if (buffer.nodeId != step.nodeId || buffer.scopeId != step.scopeId) continue;
        const auto adapter = std::ranges::find(plan.signalAdapters, index,
            &SignalAdapterPlan::outputBufferIndex);
        if (adapter != plan.signalAdapters.end())
        {
            if (!validateAdapter(plan, *adapter, maximumFrames)) return std::nullopt;
            continue;
        }
        const auto record = std::ranges::find(plan.latencyCompensations, index,
            &LatencyCompensationPlan::outputBufferIndex);
        if (record == plan.latencyCompensations.end()) ++count;
        else if (!validateCompensation(plan,
                     static_cast<std::size_t>(record - plan.latencyCompensations.begin()),
                     index, maximumFrames)) return std::nullopt;
    }
    return count;
}

class Outputs
{
public:
    [[nodiscard]] static std::optional<Outputs> prepare(const RuntimePlan& plan, const CompileStep& step,
        std::span<const Port> ports, std::span<const std::size_t> actual, std::size_t maximumFrames)
    {
        if (!validate(plan, step, ports, actual, maximumFrames)) return std::nullopt;
        return Outputs{actual};
    }
    [[nodiscard]] static std::optional<Outputs> prepareActive(const RuntimePlan& plan, const CompileStep& step,
        std::span<const Port> declared, std::span<const Port> active,
        std::span<const std::size_t> actual, std::size_t minimumFrames)
    {
        if (!validateActive(plan, step, declared, active, actual, minimumFrames)) return std::nullopt;
        return Outputs{actual};
    }
    // A compensation operation publishes its own synthetic slot, before the
    // target step runs. Existing adapters retain admission and history limits.
    // No source/target BufferSpec identity implies ownership of this output.
    [[nodiscard]] static std::optional<Outputs> prepareCompensation(const RuntimePlan& plan,
        std::size_t compensationIndex, std::size_t actualOutput, std::size_t maximumFrames)
    {
        if (!validateCompensation(plan, compensationIndex, actualOutput, maximumFrames))
            return std::nullopt;
        const std::array<std::size_t, 1> actual{actualOutput};
        return Outputs{actual};
    }

    [[nodiscard]] bool contains(std::size_t index) const noexcept
    {
        return std::ranges::find(indices_, index) != indices_.end();
    }
    [[nodiscard]] std::span<const std::size_t> indices() const noexcept { return indices_; }

private:
    explicit Outputs(std::span<const std::size_t> actual) : indices_(actual.begin(), actual.end()) {}
    std::vector<std::size_t> indices_;
};
}
