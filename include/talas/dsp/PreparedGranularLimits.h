#pragma once

#include <cstddef>

namespace talas::dsp
{
inline constexpr std::size_t maximumPreparedGranularActiveGrains = 64;
inline constexpr std::size_t maximumPreparedCloudBankTracks = 64;
inline constexpr std::size_t maximumPreparedCloudBankCandidatesPerFrame = 4096;

[[nodiscard]] constexpr bool isPreparedCloudBankWorkBounded(
    std::size_t trackCount,
    std::size_t activeGrainsPerTrack) noexcept
{
    return trackCount > 0
        && trackCount <= maximumPreparedCloudBankTracks
        && activeGrainsPerTrack > 0
        && activeGrainsPerTrack <= maximumPreparedGranularActiveGrains
        && trackCount
            <= maximumPreparedCloudBankCandidatesPerFrame
                / activeGrainsPerTrack;
}
}
