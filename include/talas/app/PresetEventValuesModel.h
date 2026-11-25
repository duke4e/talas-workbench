#pragma once

#include "talas/project/ProjectDocument.h"

#include <optional>
#include <string>
#include <string_view>

namespace talas::app
{
// The Preset's Set to Current Event Values action: the
// Virtual Control Surface's current values of the Event Values the
// Preset's Input uses, as the EventValues field holds them, one entry
// `!Name channel: n; initialValue: v.` a line in the surface's
// order; the channel is a control's MIDI mapping's, else 1. Empty when the
// Input uses no Event Value the surface has, or the node is not a Preset
// of that graph (the root graph without a definition id).
[[nodiscard]] std::string presetCurrentEventValues(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& definitionId,
    std::string_view presetNodeId);

// A Preset's EventValues can be read but not typed into; only
// Set to Current Event Values changes them. Every other field takes typing.
[[nodiscard]] bool parameterTakesTyping(std::string_view typeId, std::string_view parameterId) noexcept;
}
