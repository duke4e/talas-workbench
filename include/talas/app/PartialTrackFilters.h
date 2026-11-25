#pragma once

#include "talas/project/ProjectPartialTrackEdits.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace talas::app
{
// The spectrum editor's track filters: algorithms,
// not audio filters, that reshape the amplitude or frequency envelope of each
// selected track within the selected region.
enum class PartialTrackFilter : std::uint8_t
{
    ScaleAmplitudes,
    AverageAmplitude,
    SmoothAmplitude,
    RemoveAmplitudeOutliers,
    ForceHarmonicFrequency,
    AverageFrequency,
    SmoothFrequency,
    RemoveFrequencyOutliers,
    AddFrequencyNoise,
    Arpeggiate,
};

// The tracks a filter reshapes, and the source frames it reads between.
struct PartialTrackFilterRegion
{
    std::vector<std::uint64_t> trackIds;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = std::numeric_limits<std::uint64_t>::max();
};

// What each filter prompts for: the scale, the neighborhood a smooth or a
// median takes, the harmonic guide track, the noise's deviation and how often
// it changes, and the delay between arpeggiated tracks.
struct PartialTrackFilterSettings
{
    double scale = 1.0;
    std::size_t neighborhood = 3;
    std::uint64_t guideTrack = 1;
    double deviationHz = 0.0;
    double changesPerSecond = 10.0;
    std::uint64_t seed = 1;
    double delaySeconds = 0.0;
};

struct PartialTrackFilterResult
{
    std::vector<talas::project::ProjectPartialTrackPointMutation> mutations;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

// The point updates a filter makes; a track number is its trackId, so
// Force to harmonic frequency sets track 4 to twice guide track 2.
[[nodiscard]] PartialTrackFilterResult filterPartialTracks(
    const talas::project::MaterializedPartialTrackEdits& materialized,
    const PartialTrackFilterRegion& region,
    PartialTrackFilter filter,
    const PartialTrackFilterSettings& settings);

// Clear zeroes the selected tracks and Trim everything but them; zeroed
// tracks stay, muted, until they are restored or the result is rendered
//.
[[nodiscard]] std::vector<talas::project::ProjectPartialTrackStateMutation>
clearPartialTracks(
    const talas::project::MaterializedPartialTrackEdits& materialized,
    std::span<const std::uint64_t> trackIds);

// Clear of a region of selected points zeroes
// their amplitude alone, part of a track's duration, the rest of each track
// sounding on; given as (track, frame) pairs.
[[nodiscard]] std::vector<talas::project::ProjectPartialTrackPointMutation>
clearPartialTrackPoints(
    const talas::project::MaterializedPartialTrackEdits& materialized,
    const std::set<std::pair<std::uint64_t, std::uint64_t>>& points);

[[nodiscard]] std::vector<talas::project::ProjectPartialTrackStateMutation>
trimPartialTracks(
    const talas::project::MaterializedPartialTrackEdits& materialized,
    std::span<const std::uint64_t> trackIds);
}
