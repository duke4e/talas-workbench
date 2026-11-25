#pragma once

#include <cstdint>
#include <string>

namespace talas::core
{
// A named position in a sample file: an AIFF MARK chunk's marker, or a WAVE
// cue point with the label its LIST adtl chunk gives (a SamplesFile's
// markers).
struct SampleMarker
{
    std::string name;
    std::uint64_t frame = 0;

    bool operator==(const SampleMarker&) const = default;
};
}
