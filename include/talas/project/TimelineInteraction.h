#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace talas::project
{
struct TimelineLane
{
    std::string eventName;
    std::vector<std::size_t> eventIndices;

    [[nodiscard]] bool operator==(
        const TimelineLane&) const = default;
};

struct TimelineMarkerDragResult
{
    double timeSeconds = 0.0;
    double snapIntervalSeconds = 0.0;
};

enum class TimelineLoopBoundary
{
    Start,
    End,
};

[[nodiscard]] double timelineVisibleDuration(
    double lastEventTimeSeconds,
    double transportTimeSeconds);

[[nodiscard]] double timelineMarkerSnapInterval(
    double visibleDurationSeconds,
    double trackWidthPixels);

[[nodiscard]] std::vector<TimelineLane>
buildTimelineLanes(
    const std::vector<TimelineEventValue>&
        events);

[[nodiscard]] bool
timelineSegmentUsesLinearInterpolation(
    const TimelineEventValue& source,
    const TimelineEventValue& target,
    const TimelineLoopRegion& loop,
    double sampleRate);

[[nodiscard]] std::optional<std::size_t>
timelineLaneAtPosition(
    double positionPixels,
    double laneAreaHeightPixels,
    std::size_t laneCount);

[[nodiscard]] std::optional<double>
timelineLaneCenterPosition(
    std::size_t laneIndex,
    double laneAreaHeightPixels,
    std::size_t laneCount);

[[nodiscard]] std::optional<double>
timelineLaneVisibleCenterPosition(
    std::size_t laneIndex,
    double laneContentHeightPixels,
    std::size_t laneCount,
    double viewportStartPixels,
    double viewportHeightPixels);

[[nodiscard]] double
timelineLaneContentHeight(
    double viewportHeightPixels,
    std::size_t laneCount,
    double minimumLaneHeightPixels);

[[nodiscard]] TimelineMarkerDragResult mapTimelineMarkerDrag(
    double positionPixels,
    double trackWidthPixels,
    double visibleDurationSeconds,
    bool snapEnabled);

[[nodiscard]] TimelineMarkerDragResult mapTimelineLoopBoundaryDrag(
    double positionPixels,
    double trackWidthPixels,
    double visibleDurationSeconds,
    bool snapEnabled,
    TimelineLoopBoundary boundary,
    double otherBoundarySeconds);
}
