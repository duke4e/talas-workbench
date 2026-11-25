#pragma once

#include "talas/runtime/Runtime.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace talas::runtime
{
[[nodiscard]] std::optional<std::vector<MidiEvent>>
parseStandardMidiFileChannelEvents(
    const std::vector<std::uint8_t>& bytes,
    double sampleRate,
    std::size_t frameCount,
    std::uint8_t lowChannel,
    std::uint8_t highChannel,
    std::size_t maxEventCount,
    std::string& error);

// The frames of a file's MIDI timing clock
// messages (F8), which a Standard MIDI File keeps as escaped events (F7,
// then the length and the byte), each while the clock runs: a Stop (FC)
// holds the clock until a Start (FA) or a Continue (FB). At most
// maxEventCount of them, else an error.
[[nodiscard]] std::optional<std::vector<std::size_t>>
parseStandardMidiFileTimingClockFrames(
    const std::vector<std::uint8_t>& bytes,
    double sampleRate,
    std::size_t maxEventCount,
    std::string& error);

// The channel events with frames counting beats, framesPerBeat a beat: a
// file's quarter notes (its ticks over its division, whatever its tempo),
// or its seconds where it counts SMPTE frames.
[[nodiscard]] std::optional<std::vector<MidiEvent>>
parseStandardMidiFileChannelEventsByBeat(
    const std::vector<std::uint8_t>& bytes,
    double framesPerBeat,
    std::uint8_t lowChannel,
    std::uint8_t highChannel,
    std::size_t maxEventCount,
    std::string& error);
}
