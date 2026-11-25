#pragma once

#include <cstdint>

namespace talas::core
{
// The MIDI notes and velocities a sample plays, from the INST chunk of an
// AIFF file or the inst chunk of a RIFF/WAVE one.
struct SampleKeyRange
{
    std::uint8_t lowNote = 0;
    std::uint8_t highNote = 127;
    std::uint8_t lowVelocity = 1;
    std::uint8_t highVelocity = 127;

    bool operator==(const SampleKeyRange&) const = default;
};
}
