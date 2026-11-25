#pragma once

#include "talas/core/TerminalAudioLayout.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::runtime::prepared_terminal_audio
{
struct Channel
{
    std::string_view portId; // Static core declaration, never a borrowed plan string.
    std::size_t inputIndex;
    std::size_t bufferIndex;
};

// Resolve logical channels from actual input bindings. Repeated declarations
// retain stereo-pair destinations. Other inputs (e.g. recorder controls) remain
// the adapter's responsibility, as do admission, producer and latency authority.
// A Disk Recorder records one channel, or two when its Channels is stereo.
[[nodiscard]] inline bool isStereoDiskRecorder(const CompileStep& step) noexcept
{
    if (step.typeId != "talas.diskRecorder")
        return false;
    const auto channels = std::ranges::find(step.parameters, "channels", &ParameterValue::id);
    return channels != step.parameters.end() && channels->choice.has_value()
        && *channels->choice == "stereo";
}

[[nodiscard]] inline std::optional<std::vector<Channel>> resolve(const CompileStep& step)
{
    const auto declarations = isStereoDiskRecorder(step)
        ? talas::core::stereoDiskRecorderAudioInputs()
        : talas::core::terminalAudioInputs(step.typeId);
    if (declarations.empty()) return std::nullopt;
    std::vector<Channel> channels;
    channels.reserve(declarations.size());
    const auto bound = [&](std::string_view portId, std::optional<std::size_t>& inputIndex) {
        for (std::size_t index = 0; index < step.inputs.size(); ++index)
        {
            if (step.inputs[index].inputPortId != portId) continue;
            if (inputIndex) return false;
            inputIndex = index;
        }
        return true;
    };
    for (const auto declaration : declarations)
    {
        auto portId = declaration.portId;
        std::optional<std::size_t> inputIndex;
        if (!bound(portId, inputIndex)) return std::nullopt;
        if (!inputIndex && !declaration.fallbackPortId.empty())
        {
            portId = declaration.fallbackPortId;
            if (!bound(portId, inputIndex)) return std::nullopt;
        }
        if (!inputIndex)
        {
            if (declaration.optional) continue;
            return std::nullopt;
        }
        channels.push_back({portId, *inputIndex, step.inputs[*inputIndex].bufferIndex});
    }
    return channels;
}

// Audio Output additionally requires its physical inputs to follow channel
// order. Paired terminals and recorder controls have distinct adapter contracts.
[[nodiscard]] inline bool hasExactInputOrder(const CompileStep& step,
    std::span<const Channel> channels) noexcept
{
    if (channels.empty() || step.inputs.size() != channels.size()) return false;
    for (std::size_t index = 0; index < channels.size(); ++index)
        if (channels[index].inputIndex != index
            || step.inputs[index].inputPortId != channels[index].portId) return false;
    return true;
}

// Check the compiler's existing routing and compatibility aliases. This does
// not establish execution authority or original-producer ownership: an input
// can legitimately refer to a separately produced latency-compensation buffer.
[[nodiscard]] inline bool agreesWithCompiledRoutes(const RuntimePlan& plan,
    std::span<const Channel> channels) noexcept
{
    if (channels.empty() || channels.size() != plan.outputBufferIndices.size()
        || plan.outputBufferIndex != channels.front().bufferIndex
        || plan.rightOutputBufferIndex != (channels.size() > 1
            ? std::optional<std::size_t>{channels[1].bufferIndex} : std::nullopt)) return false;
    for (std::size_t index = 0; index < channels.size(); ++index)
    {
        const auto buffer = channels[index].bufferIndex;
        if (buffer >= plan.buffers.size() || plan.buffers[buffer].type != talas::core::PortType::Audio
            || plan.outputBufferIndices[index] != buffer) return false;
    }
    return true;
}
}
