#include "talas/project/TimelineInteraction.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace talas::project
{
double timelineVisibleDuration(
    double lastEventTimeSeconds,
    double transportTimeSeconds)
{
    const auto sanitizedLast =
        std::isfinite(lastEventTimeSeconds)
            && lastEventTimeSeconds >= 0.0
        ? lastEventTimeSeconds
        : 0.0;
    const auto sanitizedTransport =
        std::isfinite(transportTimeSeconds)
            && transportTimeSeconds >= 0.0
        ? transportTimeSeconds
        : 0.0;
    const auto contentDuration =
        std::max(sanitizedLast, sanitizedTransport);
    const auto headroom = std::max(1.0, contentDuration * 0.1);
    if (!std::isfinite(headroom)
        || contentDuration
            > std::numeric_limits<double>::max() - headroom)
    {
        return contentDuration;
    }
    return std::max(1.0, contentDuration + headroom);
}

double timelineMarkerSnapInterval(
    double visibleDurationSeconds,
    double trackWidthPixels)
{
    if (!std::isfinite(visibleDurationSeconds)
        || visibleDurationSeconds <= 0.0
        || !std::isfinite(trackWidthPixels)
        || trackWidthPixels <= 0.0)
    {
        return 0.0;
    }

    constexpr double targetSpacingPixels = 12.0;
    const auto rawInterval =
        visibleDurationSeconds * targetSpacingPixels / trackWidthPixels;
    if (!std::isfinite(rawInterval) || rawInterval <= 0.0)
        return 0.0;

    const auto exponent = std::floor(std::log10(rawInterval));
    const auto scale = std::pow(10.0, exponent);
    if (!std::isfinite(scale) || scale <= 0.0)
        return rawInterval;

    const std::array candidates{
        scale,
        2.0 * scale,
        5.0 * scale,
        10.0 * scale,
    };
    auto best = candidates.front();
    auto bestDistance = std::abs(std::log(best / rawInterval));
    for (const auto candidate : candidates)
    {
        const auto distance = std::abs(std::log(candidate / rawInterval));
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }
    return best;
}

std::vector<TimelineLane>
buildTimelineLanes(
    const std::vector<TimelineEventValue>&
        events)
{
    std::map<
        std::string,
        std::vector<std::size_t>>
        indicesByName;
    for (std::size_t index = 0;
         index < events.size();
         ++index)
    {
        indicesByName[events[index].eventName]
            .push_back(index);
    }

    std::vector<TimelineLane> lanes;
    lanes.reserve(indicesByName.size());
    for (auto& [eventName, eventIndices] :
         indicesByName)
    {
        lanes.push_back({
            std::move(eventName),
            std::move(eventIndices),
        });
    }
    return lanes;
}

bool timelineSegmentUsesLinearInterpolation(
    const TimelineEventValue& source,
    const TimelineEventValue& target,
    const TimelineLoopRegion& loop,
    double sampleRate)
{
    if (source.interpolation
            != talas::core::
                AutomationInterpolation::Linear
        || !std::isfinite(source.timeSeconds)
        || !std::isfinite(target.timeSeconds)
        || target.timeSeconds
            <= source.timeSeconds)
    {
        return false;
    }
    if (!loop.enabled)
        return true;
    if (!std::isfinite(loop.startSeconds)
        || !std::isfinite(loop.endSeconds)
        || loop.startSeconds < 0.0
        || loop.endSeconds
            <= loop.startSeconds
        || !std::isfinite(sampleRate)
        || sampleRate <= 0.0)
    {
        return false;
    }
    const auto roundedFrame =
        [sampleRate](
            double timeSeconds)
            -> std::optional<
                std::uint64_t> {
        const auto exact =
            static_cast<long double>(
                timeSeconds)
            * static_cast<long double>(
                sampleRate);
        const auto rounded =
            std::floor(exact + 0.5L);
        if (rounded < 0.0L
            || rounded
                > static_cast<long double>(
                    std::numeric_limits<
                        std::uint64_t>::max()))
        {
            return {};
        }
        return static_cast<std::uint64_t>(
            rounded);
    };
    const auto sourceFrame =
        roundedFrame(source.timeSeconds);
    const auto targetFrame =
        roundedFrame(target.timeSeconds);
    const auto loopStartFrame =
        roundedFrame(loop.startSeconds);
    const auto loopEndFrame =
        roundedFrame(loop.endSeconds);
    if (!sourceFrame.has_value()
        || !targetFrame.has_value()
        || !loopStartFrame.has_value()
        || !loopEndFrame.has_value()
        || *loopEndFrame
            <= *loopStartFrame
        || *targetFrame
            <= *sourceFrame)
    {
        return false;
    }
    const auto bothBefore =
        *sourceFrame < *loopStartFrame
        && *targetFrame
            < *loopStartFrame;
    const auto bothInside =
        *sourceFrame >= *loopStartFrame
        && *sourceFrame < *loopEndFrame
        && *targetFrame >= *loopStartFrame
        && *targetFrame < *loopEndFrame;
    return bothBefore || bothInside;
}

std::optional<std::size_t>
timelineLaneAtPosition(
    double positionPixels,
    double laneAreaHeightPixels,
    std::size_t laneCount)
{
    if (!std::isfinite(positionPixels)
        || !std::isfinite(
            laneAreaHeightPixels)
        || laneAreaHeightPixels <= 0.0
        || laneCount == 0
        || positionPixels < 0.0
        || positionPixels
            >= laneAreaHeightPixels)
    {
        return {};
    }
    const auto lane =
        static_cast<std::size_t>(
            std::floor(
                positionPixels
                / laneAreaHeightPixels
                * static_cast<double>(
                    laneCount)));
    return std::min(lane, laneCount - 1);
}

std::optional<double>
timelineLaneCenterPosition(
    std::size_t laneIndex,
    double laneAreaHeightPixels,
    std::size_t laneCount)
{
    if (!std::isfinite(
            laneAreaHeightPixels)
        || laneAreaHeightPixels <= 0.0
        || laneCount == 0
        || laneIndex >= laneCount)
    {
        return {};
    }
    const auto center =
        (static_cast<double>(laneIndex)
             + 0.5)
        / static_cast<double>(laneCount)
        * laneAreaHeightPixels;
    if (!std::isfinite(center))
        return {};
    return center;
}

std::optional<double>
timelineLaneVisibleCenterPosition(
    std::size_t laneIndex,
    double laneContentHeightPixels,
    std::size_t laneCount,
    double viewportStartPixels,
    double viewportHeightPixels)
{
    const auto laneCenter =
        timelineLaneCenterPosition(
            laneIndex,
            laneContentHeightPixels,
            laneCount);
    if (!laneCenter.has_value()
        || !std::isfinite(
            viewportStartPixels)
        || viewportStartPixels < 0.0
        || !std::isfinite(
            viewportHeightPixels)
        || viewportHeightPixels <= 0.0)
    {
        return {};
    }

    const auto viewportEnd =
        viewportStartPixels
        + viewportHeightPixels;
    if (!std::isfinite(viewportEnd))
        return {};
    const auto laneHeight =
        laneContentHeightPixels
        / static_cast<double>(laneCount);
    const auto visibleStart = std::max(
        *laneCenter
            - laneHeight * 0.5,
        viewportStartPixels);
    const auto visibleEnd = std::min(
        *laneCenter
            + laneHeight * 0.5,
        viewportEnd);
    if (visibleEnd <= visibleStart)
        return {};
    return (visibleStart + visibleEnd)
            * 0.5
        - viewportStartPixels;
}

double timelineLaneContentHeight(
    double viewportHeightPixels,
    std::size_t laneCount,
    double minimumLaneHeightPixels)
{
    if (!std::isfinite(
            viewportHeightPixels)
        || viewportHeightPixels <= 0.0
        || !std::isfinite(
            minimumLaneHeightPixels)
        || minimumLaneHeightPixels <= 0.0)
    {
        return 0.0;
    }
    if (laneCount == 0)
        return viewportHeightPixels;

    const auto required =
        static_cast<long double>(
            laneCount)
        * static_cast<long double>(
            minimumLaneHeightPixels);
    const auto maximum =
        static_cast<long double>(
            std::numeric_limits<double>::
                max());
    return std::max(
        viewportHeightPixels,
        required >= maximum
            ? std::numeric_limits<double>::
                max()
            : static_cast<double>(
                required));
}

TimelineMarkerDragResult mapTimelineMarkerDrag(
    double positionPixels,
    double trackWidthPixels,
    double visibleDurationSeconds,
    bool snapEnabled)
{
    TimelineMarkerDragResult result;
    result.snapIntervalSeconds = timelineMarkerSnapInterval(
        visibleDurationSeconds,
        trackWidthPixels);
    if (!std::isfinite(positionPixels)
        || !std::isfinite(trackWidthPixels)
        || trackWidthPixels <= 0.0
        || !std::isfinite(visibleDurationSeconds)
        || visibleDurationSeconds <= 0.0)
    {
        return result;
    }

    const auto clampedPosition =
        std::clamp(positionPixels, 0.0, trackWidthPixels);
    auto time =
        clampedPosition / trackWidthPixels * visibleDurationSeconds;
    if (snapEnabled && result.snapIntervalSeconds > 0.0)
    {
        time = std::round(time / result.snapIntervalSeconds)
            * result.snapIntervalSeconds;
    }
    result.timeSeconds =
        std::clamp(time, 0.0, visibleDurationSeconds);
    return result;
}

TimelineMarkerDragResult mapTimelineLoopBoundaryDrag(
    double positionPixels,
    double trackWidthPixels,
    double visibleDurationSeconds,
    bool snapEnabled,
    TimelineLoopBoundary boundary,
    double otherBoundarySeconds)
{
    auto result = mapTimelineMarkerDrag(
        positionPixels,
        trackWidthPixels,
        visibleDurationSeconds,
        snapEnabled);
    if (!std::isfinite(otherBoundarySeconds)
        || otherBoundarySeconds < 0.0
        || !std::isfinite(visibleDurationSeconds)
        || visibleDurationSeconds <= 0.0)
    {
        result.timeSeconds = 0.0;
        return result;
    }

    const auto minimumSeparation =
        snapEnabled && result.snapIntervalSeconds > 0.0
        ? result.snapIntervalSeconds
        : 1.0e-6;
    if (boundary == TimelineLoopBoundary::Start)
    {
        result.timeSeconds = std::clamp(
            result.timeSeconds,
            0.0,
            std::max(0.0, otherBoundarySeconds - minimumSeparation));
    }
    else
    {
        result.timeSeconds = std::clamp(
            result.timeSeconds,
            std::min(
                visibleDurationSeconds,
                otherBoundarySeconds + minimumSeparation),
            visibleDurationSeconds);
    }
    return result;
}
}
