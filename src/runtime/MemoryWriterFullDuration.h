#pragma once

#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

// A MemoryWriter whose CaptureDuration is 0 s records its Input for its full
// duration: for the Duration of the first
// SetDuration its output reaches, when that Duration is fixed, or else for
// as many frames as a recording holds.
namespace talas::runtime::memory_writer_duration
{
inline constexpr std::size_t maximumRecordedFrames = 1'048'576;

// The fixed Duration, in seconds, of the first SetDuration that `step`'s
// output reaches, nearest first.
[[nodiscard]] inline std::optional<double> enclosingSetDurationSeconds(
    const RuntimePlan& plan,
    const CompileStep& step)
{
    std::vector<const CompileStep*> frontier{&step};
    std::vector<bool> visited(plan.steps.size(), false);
    for (std::size_t cursor = 0; cursor < frontier.size(); ++cursor)
    {
        const auto& producer = *frontier[cursor];
        for (std::size_t index = 0; index < plan.steps.size(); ++index)
        {
            const auto& consumer = plan.steps[index];
            if (visited[index] || &consumer == &step
                || !std::ranges::any_of(consumer.inputs, [&](const InputBinding& binding) {
                       return binding.sourceNodeId == producer.nodeId
                           && binding.sourceScopeId == producer.scopeId;
                   }))
            {
                continue;
            }
            visited[index] = true;
            if (consumer.typeId != "talas.setDuration")
            {
                frontier.push_back(&consumer);
                continue;
            }
            const auto duration = std::ranges::find(
                consumer.parameters, std::string_view{"duration"}, &ParameterValue::id);
            const auto connected = std::ranges::any_of(consumer.inputs, [](const InputBinding& binding) {
                return binding.inputPortId == "duration";
            });
            if (connected || duration == consumer.parameters.end()
                || !duration->expression.has_value()
                || duration->expression->isStateful()
                || !duration->expression->eventReferences().empty()
                || !duration->expression->variableReferences().empty()
                || !duration->expression->identifiers().empty()
                || !std::isfinite(duration->value) || duration->value <= 0.0)
            {
                return std::nullopt;
            }
            return duration->value;
        }
    }
    return std::nullopt;
}

// The frames a full-duration recording holds, from 1 to
// maximumRecordedFrames.
[[nodiscard]] inline std::size_t fullDurationFrames(
    const RuntimePlan& plan,
    const CompileStep& step,
    double sampleRate)
{
    const auto seconds = enclosingSetDurationSeconds(plan, step);
    if (!seconds.has_value() || !std::isfinite(sampleRate) || sampleRate <= 0.0)
        return maximumRecordedFrames;
    const auto frames = std::ceil(*seconds * sampleRate);
    if (!std::isfinite(frames) || frames >= static_cast<double>(maximumRecordedFrames))
        return maximumRecordedFrames;
    return std::max<std::size_t>(1, static_cast<std::size_t>(frames));
}
}
