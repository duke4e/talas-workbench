#pragma once

#include <cstdint>

namespace talas::core
{
// Sample-frame interval. The end is exclusive, including when a WAV smpl
// chunk supplied an inclusive end frame.
struct SampleLoop
{
    std::uint64_t startFrame = 0;
    std::uint64_t endFrameExclusive = 0;

    bool operator==(const SampleLoop&) const = default;
};
}
