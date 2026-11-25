#pragma once

#include "talas/project/ProjectDocument.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace talas::project
{
// ADR-0024: the Default global map. Each name follows its controller on the
// default MIDI channel from 0 to 1; !PitchBend follows pitch bend from -1 to
// 1. The kind is how the map displays it in the Virtual Control Surface.
struct DefaultMidiMapController
{
    std::string_view eventName;
    std::uint8_t controller = 0;
    VirtualControlKind kind = VirtualControlKind::Fader;
};

// The map gives the
// controllers memorable names, !Frequency among them, on controller 18.
inline constexpr std::array<DefaultMidiMapController, 11> defaultMidiMapControllers{{
    {"FootController", 4, VirtualControlKind::Fader},
    {"Volume", 7, VirtualControlKind::Fader},
    {"ExpressionController", 11, VirtualControlKind::Fader},
    {"EffectControl1", 12, VirtualControlKind::Fader},
    {"EffectControl2", 13, VirtualControlKind::Fader},
    {"Frequency", 18, VirtualControlKind::Fader},
    {"DamperPedal", 64, VirtualControlKind::Gate},
    {"Hold2", 69, VirtualControlKind::Toggle},
    {"HarmonicContent", 71, VirtualControlKind::Fader},
    {"ExternalEffectsDepth", 91, VirtualControlKind::Fader},
    {"DetuneDepth", 94, VirtualControlKind::Fader},
}};

inline constexpr std::string_view defaultMidiMapPitchBendName = "PitchBend";

[[nodiscard]] constexpr const DefaultMidiMapController* findDefaultMidiMapController(
    std::string_view eventName) noexcept
{
    for (const auto& entry : defaultMidiMapControllers)
    {
        if (entry.eventName == eventName)
            return &entry;
    }
    return nullptr;
}

[[nodiscard]] constexpr bool isDefaultMidiMapName(std::string_view eventName) noexcept
{
    return eventName == defaultMidiMapPitchBendName
        || findDefaultMidiMapController(eventName) != nullptr;
}

// A Virtual Control with its own MIDI or OSC mapping takes its name away
// from the default map.
[[nodiscard]] inline bool projectMapsEventValue(
    const ProjectDocument& document, std::string_view eventName) noexcept
{
    return std::ranges::any_of(document.virtualControls, [&](const VirtualControl& control) {
        return control.eventName == eventName
            && (control.midiMapping.has_value() || control.oscMapping.has_value());
    });
}
}
