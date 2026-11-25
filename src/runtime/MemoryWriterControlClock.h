#pragma once

#include <algorithm>

namespace talas::runtime
{
// Bounded Val-rate candidate: the first sample and then at most 1000
// control ticks per second.
inline bool advanceMemoryWriterCyclicControlTick(
    double sampleRate, double& phase) noexcept
{
    const auto tick = phase >= 1.0;
    if (tick)
        phase -= 1.0;
    phase += std::min(1.0, 1000.0 / sampleRate);
    return tick;
}
}
