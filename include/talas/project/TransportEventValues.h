#pragma once

#include "talas/project/ProjectDocument.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>

// ADR-0030: the transport's built-in Event Values. A Sound reads the tempo
// and the song position as it reads !KeyPitch: from the project's own
// transport in the Workbench and offline, from the host's in a plugin.
namespace talas::project
{
enum class TransportEventValue : std::size_t
{
    // Quarter notes a minute.
    Tempo,
    // Quarter notes since the start of the song, negative in a host's
    // count-in.
    SongPosition,
    // How far the song position is through its quarter note, 0 to 1.
    BeatPhase,
    // 1 while the transport plays, 0 while it is stopped.
    TransportPlaying,
};

inline constexpr std::size_t transportEventValueCount = 4;

inline constexpr std::array<std::string_view, transportEventValueCount> transportEventValueNames{
    "Tempo",
    "SongPosition",
    "BeatPhase",
    "TransportPlaying",
};

[[nodiscard]] constexpr std::string_view transportEventValueName(TransportEventValue value) noexcept
{
    return transportEventValueNames[static_cast<std::size_t>(value)];
}

[[nodiscard]] constexpr std::optional<TransportEventValue> transportEventValueNamed(std::string_view name) noexcept
{
    for (std::size_t index = 0; index < transportEventValueNames.size(); ++index)
    {
        if (transportEventValueNames[index] == name)
            return static_cast<TransportEventValue>(index);
    }
    return std::nullopt;
}

// Whether the project gives the name a meaning of its own, a Virtual Control
// or a Timeline Event Value, which wins over the built-in one: the arpeggiator
// of an older project may have a Tempo fader.
[[nodiscard]] inline bool projectDeclaresEventValue(const ProjectDocument& document, std::string_view name)
{
    return std::ranges::any_of(document.virtualControls,
               [&](const VirtualControl& control) { return control.eventName == name; })
        || std::ranges::any_of(document.timeline.eventValues,
            [&](const TimelineEventValue& event) { return event.eventName == name; });
}

// Whether `name` is a transport Event Value the project does not declare.
[[nodiscard]] inline bool isBuiltinTransportEventValue(const ProjectDocument& document, std::string_view name)
{
    return transportEventValueNamed(name).has_value() && !projectDeclaresEventValue(document, name);
}

// The four values at a song position, in quarter notes, and tempo.
[[nodiscard]] inline std::array<double, transportEventValueCount> transportEventValuesAt(
    double tempoBpm,
    double songPositionQuarters,
    bool playing) noexcept
{
    const auto phase = songPositionQuarters - std::floor(songPositionQuarters);
    return {
        tempoBpm,
        songPositionQuarters,
        // A position a hair below a whole quarter rounds up to it.
        phase >= 1.0 ? 0.0 : phase,
        playing ? 1.0 : 0.0,
    };
}
}
