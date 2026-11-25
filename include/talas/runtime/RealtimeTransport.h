#pragma once

#include <cstdint>
#include <limits>

namespace talas::runtime
{
struct RealtimeTransportSnapshot
{
    std::uint64_t epoch = 0;
    std::uint64_t absoluteFrame = 0;
    std::uint64_t playheadFrame = 0;
    double sampleRate = 0.0;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
    std::uint64_t discontinuitySequence = 0;
    bool playing = false;
    bool loopEnabled = false;
};

[[nodiscard]] constexpr std::uint64_t saturatingTransportFrameAdd(
    std::uint64_t frame,
    std::uint64_t offset) noexcept
{
    return offset > std::numeric_limits<std::uint64_t>::max() - frame
        ? std::numeric_limits<std::uint64_t>::max()
        : frame + offset;
}

[[nodiscard]] constexpr std::uint64_t transportPlayheadFrame(
    std::uint64_t absoluteFrame,
    const RealtimeTransportSnapshot& snapshot) noexcept
{
    if (!snapshot.loopEnabled
        || snapshot.loopEndFrame <= snapshot.loopStartFrame
        || absoluteFrame < snapshot.loopStartFrame)
    {
        return absoluteFrame;
    }
    return snapshot.loopStartFrame
        + ((absoluteFrame - snapshot.loopStartFrame)
           % (snapshot.loopEndFrame - snapshot.loopStartFrame));
}

[[nodiscard]] constexpr RealtimeTransportSnapshot transportSnapshotAtFrameOffset(
    RealtimeTransportSnapshot snapshot,
    std::uint64_t frameOffset) noexcept
{
    if (snapshot.playing)
    {
        snapshot.absoluteFrame = saturatingTransportFrameAdd(
            snapshot.absoluteFrame,
            frameOffset);
    }
    snapshot.playheadFrame = transportPlayheadFrame(
        snapshot.absoluteFrame,
        snapshot);
    return snapshot;
}
}
