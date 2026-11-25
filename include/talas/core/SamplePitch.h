#pragma once

#include <cstdint>

namespace talas::core
{
// The two exact MIDI pitch fields in a RIFF/WAVE smpl chunk. The fractional
// field is an unsigned 32-bit fraction of one semitone.
struct SamplePitch
{
    std::uint32_t midiUnityNote = 0;
    std::uint32_t midiPitchFraction = 0;

    bool operator==(const SamplePitch&) const = default;
};
}
