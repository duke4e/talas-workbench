#include "talas/project/TimelineInteraction.h"

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    require(
        std::abs(actual - expected) <= tolerance,
        message + ": expected " + std::to_string(expected)
            + ", got " + std::to_string(actual));
}

void testMapsAndClampsPointerPosition()
{
    const auto middle =
        talas::project::mapTimelineMarkerDrag(25.0, 100.0, 8.0, false);
    requireNear(
        middle.timeSeconds,
        2.0,
        1.0e-12,
        "unsnapped drag should map linearly");
    requireNear(
        talas::project::mapTimelineMarkerDrag(-20.0, 100.0, 8.0, false)
            .timeSeconds,
        0.0,
        1.0e-12,
        "drag should clamp before the track");
    requireNear(
        talas::project::mapTimelineMarkerDrag(140.0, 100.0, 8.0, false)
            .timeSeconds,
        8.0,
        1.0e-12,
        "drag should clamp after the track");
}

void testVisibleDurationLeavesRoomAfterCurrentContent()
{
    requireNear(
        talas::project::timelineVisibleDuration(0.0, 0.0),
        1.0,
        1.0e-12,
        "empty Timeline should expose one second");
    requireNear(
        talas::project::timelineVisibleDuration(1.0, 0.5),
        2.0,
        1.0e-12,
        "short Timeline should leave one second after its last marker");
    requireNear(
        talas::project::timelineVisibleDuration(10.0, 20.0),
        22.0,
        1.0e-12,
        "long Timeline should leave ten percent after the playhead");
}

void testChoosesAdaptiveOneTwoFiveSnapIntervals()
{
    requireNear(
        talas::project::timelineMarkerSnapInterval(1.0, 200.0),
        0.05,
        1.0e-12,
        "one-second view should use a 50 ms grid");
    requireNear(
        talas::project::timelineMarkerSnapInterval(10.0, 200.0),
        0.5,
        1.0e-12,
        "ten-second view should use a 500 ms grid");
    requireNear(
        talas::project::timelineMarkerSnapInterval(100.0, 100.0),
        10.0,
        1.0e-12,
        "long view should use a ten-second grid");
}

void testSnapsByDefaultAndSupportsExactBypass()
{
    const auto snapped =
        talas::project::mapTimelineMarkerDrag(27.0, 100.0, 8.0, true);
    requireNear(
        snapped.snapIntervalSeconds,
        1.0,
        1.0e-12,
        "eight-second view should expose its one-second grid");
    requireNear(
        snapped.timeSeconds,
        2.0,
        1.0e-12,
        "snapped drag should round to the nearest grid line");

    const auto exact =
        talas::project::mapTimelineMarkerDrag(27.0, 100.0, 8.0, false);
    requireNear(
        exact.timeSeconds,
        2.16,
        1.0e-12,
        "snap bypass should preserve exact pointer mapping");
    requireNear(
        exact.snapIntervalSeconds,
        1.0,
        1.0e-12,
        "snap bypass should still report the visible grid interval");
}

void testInvalidGeometryFailsClosed()
{
    const auto zeroWidth =
        talas::project::mapTimelineMarkerDrag(10.0, 0.0, 4.0, true);
    require(zeroWidth.timeSeconds == 0.0,
            "zero-width track should map to zero");
    const auto invalidDuration =
        talas::project::mapTimelineMarkerDrag(10.0, 100.0, -1.0, true);
    require(invalidDuration.timeSeconds == 0.0,
            "negative duration should map to zero");
}

void testLoopBoundaryDragPreservesStrictOrderedRegion()
{
    const auto snappedStart = talas::project::mapTimelineLoopBoundaryDrag(
        90.0,
        100.0,
        8.0,
        true,
        talas::project::TimelineLoopBoundary::Start,
        6.0);
    requireNear(
        snappedStart.timeSeconds,
        5.0,
        1.0e-12,
        "snapped loop start should stop one grid interval before the end");

    const auto snappedEnd = talas::project::mapTimelineLoopBoundaryDrag(
        10.0,
        100.0,
        8.0,
        true,
        talas::project::TimelineLoopBoundary::End,
        2.0);
    requireNear(
        snappedEnd.timeSeconds,
        3.0,
        1.0e-12,
        "snapped loop end should stop one grid interval after the start");

    const auto freeEnd = talas::project::mapTimelineLoopBoundaryDrag(
        25.000001,
        100.0,
        8.0,
        false,
        talas::project::TimelineLoopBoundary::End,
        2.0);
    require(
        freeEnd.timeSeconds > 2.0,
        "free loop boundary drag should retain a strictly non-empty region");
}

void testBuildsDeterministicNameBasedTimelineLanes()
{
    const std::vector<
        talas::project::TimelineEventValue>
        events{
            {0.0, "Gain", talas::expr::ValueDomain::Scalar, 0.1},
            {0.0, "Cutoff", talas::expr::ValueDomain::Hertz, 440.0},
            {1.0, "Cutoff", talas::expr::ValueDomain::Hertz, 880.0},
            {2.0, "Gate", talas::expr::ValueDomain::Scalar, 1.0},
            {3.0, "Gain", talas::expr::ValueDomain::Scalar, 0.9},
        };
    const auto lanes =
        talas::project::buildTimelineLanes(
            events);
    require(
        lanes.size() == 3,
        "distinct Event Value names should create distinct lanes");
    require(
        lanes[0].eventName == "Cutoff"
            && lanes[0].eventIndices
                == std::vector<std::size_t>{1, 2}
            && lanes[1].eventName == "Gain"
            && lanes[1].eventIndices
                == std::vector<std::size_t>{0, 4}
            && lanes[2].eventName == "Gate"
            && lanes[2].eventIndices
                == std::vector<std::size_t>{3},
        "lanes should be lexical and retain exact chronological event indices");

    auto movedEvents = events;
    std::swap(
        movedEvents[0],
        movedEvents[4]);
    const auto movedLanes =
        talas::project::buildTimelineLanes(
            movedEvents);
    require(
        movedLanes.size() == lanes.size()
            && movedLanes[0].eventName
                == lanes[0].eventName
            && movedLanes[1].eventName
                == lanes[1].eventName
            && movedLanes[2].eventName
                == lanes[2].eventName,
        "time reordering should not reorder lane identities");
    require(
        talas::project::buildTimelineLanes({})
            .empty(),
        "an empty Timeline should have no derived lanes");
}

void testLinearSegmentDisplayMatchesLoopPlaybackRegion()
{
    const talas::project::TimelineLoopRegion loop{
        true,
        2.0,
        6.0,
    };
    const auto linearEvent =
        [](double time) {
            return talas::project::
                TimelineEventValue{
                    time,
                    "Gain",
                    talas::expr::ValueDomain::
                        Scalar,
                    time,
                    talas::core::
                        AutomationInterpolation::
                            Linear,
                };
        };
    require(
        talas::project::
            timelineSegmentUsesLinearInterpolation(
                linearEvent(0.0),
                linearEvent(1.0),
                loop,
                4.0),
        "pre-loop linear segment should be visible");
    require(
        talas::project::
            timelineSegmentUsesLinearInterpolation(
                linearEvent(2.0),
                linearEvent(5.0),
                loop,
                4.0),
        "in-loop linear segment should be visible");
    require(
        !talas::project::
             timelineSegmentUsesLinearInterpolation(
                 linearEvent(1.0),
                 linearEvent(2.0),
                 loop,
                 4.0)
             && !talas::project::
                     timelineSegmentUsesLinearInterpolation(
                         linearEvent(5.0),
                         linearEvent(6.0),
                         loop,
                         4.0)
             && !talas::project::
                     timelineSegmentUsesLinearInterpolation(
                         linearEvent(6.0),
                         linearEvent(7.0),
                         loop,
                         4.0),
        "cross-boundary and post-loop linear segments should display as holds");
    auto step = linearEvent(2.0);
    step.interpolation =
        talas::core::AutomationInterpolation::Step;
    require(
        !talas::project::
             timelineSegmentUsesLinearInterpolation(
                 step,
                 linearEvent(5.0),
                 loop,
                 4.0),
        "step source should never display a linear segment");

    auto disabledLoop = loop;
    disabledLoop.enabled = false;
    require(
        talas::project::
            timelineSegmentUsesLinearInterpolation(
                linearEvent(5.0),
                linearEvent(7.0),
                disabledLoop,
                4.0),
        "disabled loop should not suppress a linear segment");
    require(
        talas::project::
            timelineSegmentUsesLinearInterpolation(
                linearEvent(0.9),
                linearEvent(1.5),
                talas::project::
                    TimelineLoopRegion{
                        true,
                        1.0,
                        2.0,
                    },
                4.0),
        "display should use rounded frames when a source rounds onto the loop start");
    require(
        !talas::project::
             timelineSegmentUsesLinearInterpolation(
                 linearEvent(1.5),
                 linearEvent(1.9),
                 talas::project::
                     TimelineLoopRegion{
                         true,
                         1.0,
                         2.0,
                     },
                 4.0),
        "display should suppress a target that rounds onto the excluded loop end");
}

void testMapsTimelineLaneGeometryConsistently()
{
    require(
        talas::project::timelineLaneAtPosition(
            0.0,
            60.0,
            3)
            == std::optional<std::size_t>{0}
            && talas::project::
                   timelineLaneAtPosition(
                       20.0,
                       60.0,
                       3)
                == std::optional<std::size_t>{
                    1}
            && talas::project::
                   timelineLaneAtPosition(
                       59.999,
                       60.0,
                       3)
                == std::optional<std::size_t>{
                    2},
        "bounded lane positions should map to their exact row");
    require(
        !talas::project::timelineLaneAtPosition(
             -0.001,
             60.0,
             3)
             .has_value()
            && !talas::project::
                    timelineLaneAtPosition(
                        60.0,
                        60.0,
                        3)
                    .has_value()
            && !talas::project::
                    timelineLaneAtPosition(
                        10.0,
                        0.0,
                        3)
                    .has_value()
            && !talas::project::
                    timelineLaneAtPosition(
                        10.0,
                        60.0,
                        0)
                    .has_value(),
        "out-of-range or invalid lane geometry should fail closed");

    requireNear(
        talas::project::
            timelineLaneCenterPosition(
                0,
                60.0,
                3)
            .value_or(-1.0),
        10.0,
        1.0e-12,
        "first lane center should share the hit-test partition");
    requireNear(
        talas::project::
            timelineLaneCenterPosition(
                2,
                60.0,
                3)
            .value_or(-1.0),
        50.0,
        1.0e-12,
        "last lane center should share the hit-test partition");
    require(
        !talas::project::
             timelineLaneCenterPosition(
                 3,
                 60.0,
                 3)
             .has_value(),
        "an unknown lane should not produce drawing geometry");

    const auto scrollableHeight =
        talas::project::
            timelineLaneContentHeight(
                64.0,
                65,
                16.0);
    requireNear(
        scrollableHeight,
        1040.0,
        1.0e-12,
        "dense lanes should expand into a scrollable content extent");
    require(
        talas::project::timelineLaneAtPosition(
            scrollableHeight - 0.001,
            scrollableHeight,
            65)
            == std::optional<std::size_t>{
                64},
        "the final dense lane should retain a directly addressable pointer row");
    requireNear(
        talas::project::
            timelineLaneVisibleCenterPosition(
                0,
                80.0,
                5,
                1.0,
                64.0)
            .value_or(-1.0),
        7.5,
        1.0e-12,
        "a partially visible first lane should retain a visible marker center");
    requireNear(
        talas::project::
            timelineLaneVisibleCenterPosition(
                4,
                80.0,
                5,
                1.0,
                64.0)
            .value_or(-1.0),
        63.5,
        1.0e-12,
        "a partially visible final lane should retain a visible marker center");
    require(
        !talas::project::
             timelineLaneVisibleCenterPosition(
                 4,
                 80.0,
                 5,
                 0.0,
                 64.0)
             .has_value(),
        "a lane fully outside the viewport should remain culled");
    requireNear(
        talas::project::
            timelineLaneContentHeight(
                64.0,
                2,
                16.0),
        64.0,
        1.0e-12,
        "sparse lanes should continue filling the visible track");
}
}

int main()
{
    testMapsAndClampsPointerPosition();
    testVisibleDurationLeavesRoomAfterCurrentContent();
    testChoosesAdaptiveOneTwoFiveSnapIntervals();
    testSnapsByDefaultAndSupportsExactBypass();
    testInvalidGeometryFailsClosed();
    testLoopBoundaryDragPreservesStrictOrderedRegion();
    testBuildsDeterministicNameBasedTimelineLanes();
    testLinearSegmentDisplayMatchesLoopPlaybackRegion();
    testMapsTimelineLaneGeometryConsistently();
    return 0;
}
