#include "talas/app/AutomationLaneEditing.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Interpolation = talas::core::AutomationInterpolation;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ScoreTime seconds(double value)
{
    talas::project::ScoreTime time;
    time.seconds = value;
    return time;
}

talas::project::ScoreTime ticks(std::uint64_t value)
{
    talas::project::ScoreTime time;
    time.domain = talas::project::ScoreTimeDomain::MusicalTicks;
    time.tick = value;
    return time;
}

talas::project::ScoreAutomationLane lane(
    std::vector<talas::project::ScoreAutomationPoint> points)
{
    return {"lane", talas::project::ScoreAutomationScope::Track, "track", "Rate",
        talas::expr::ValueDomain::Scalar, std::move(points)};
}

std::vector<double> values(const talas::project::ScoreAutomationLane& value)
{
    std::vector<double> result;
    for (const auto& point : value.points)
        result.push_back(point.value);
    return result;
}

std::vector<double> times(const talas::project::ScoreAutomationLane& value)
{
    std::vector<double> result;
    for (const auto& point : value.points)
        result.push_back(point.position.domain == talas::project::ScoreTimeDomain::Seconds
                ? point.position.seconds
                : static_cast<double>(point.position.tick));
    return result;
}

bool close(const std::vector<double>& left, const std::vector<double>& right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (std::abs(left[index] - right[index]) > 1.0e-9)
            return false;
    return true;
}

const talas::core::NodeRegistry& registry()
{
    static const talas::core::NodeRegistry builtins = [] {
        talas::core::NodeRegistry value;
        talas::core::registerBuiltinNodeTypes(value);
        return value;
    }();
    return builtins;
}

// A track's controls include every controllable
// parameter, not only its level.
void testTargetsAreEveryControlTheOwnerDoesNotAutomate()
{
    talas::project::ProjectDocument document;
    document.virtualControls.push_back({"Rate", talas::expr::ValueDomain::Scalar});
    document.virtualControls.push_back({"Frequency", talas::expr::ValueDomain::Hertz});
    document.timeline.score.automation.push_back(lane({}));
    const auto targets = talas::app::automationTargets(
        document, talas::project::ScoreAutomationScope::Track, "track");
    require(targets == std::vector<talas::app::AutomationTarget>{
                {"Gain", talas::expr::ValueDomain::Scalar},
                {"Level", talas::expr::ValueDomain::Scalar},
                {"Angle", talas::expr::ValueDomain::Scalar},
                {"Radius", talas::expr::ValueDomain::Scalar},
                {"Frequency", talas::expr::ValueDomain::Hertz}},
        "a track should offer Gain, its Level, its Angle and Radius and each control it does not yet automate");
    // A track's Notes control is its discrete note events, always
    // live, listed but never a lane to record or edit.
    require(talas::app::automationLiveControls(talas::project::ScoreAutomationScope::Track) == std::vector<std::string_view>{"Notes"}
            && talas::app::automationLiveControls(talas::project::ScoreAutomationScope::Clip).empty()
            && std::ranges::none_of(targets, [](const auto& target) { return target.eventName == "Notes"; }),
        "Notes should be a track's live control of note events, not a lane");
    require(talas::app::automationTargets(
                document, talas::project::ScoreAutomationScope::Track, "other").size() == 6,
        "another track should offer every control");
    require(std::ranges::none_of(
                talas::app::automationTargets(document, talas::project::ScoreAutomationScope::Clip, "clip"),
                [](const auto& target) { return target.eventName == "Angle"; }),
        "Angle and Radius are track controls, not a clip's");
    const auto clipTargets = talas::app::automationTargets(document, talas::project::ScoreAutomationScope::Clip, "clip");
    require(clipTargets.size() >= 4 && clipTargets[1] == talas::app::AutomationTarget{"FadeInOut", talas::expr::ValueDomain::Scalar}
            && clipTargets[2] == talas::app::AutomationTarget{"Panner", talas::expr::ValueDomain::Scalar}
            && clipTargets[3] == talas::app::AutomationTarget{"Distance", talas::expr::ValueDomain::Scalar},
        "three controls are added to any Sound dragged into the Timeline: FadeInOut, Panner and Distance");
    // The three kinds of controls, a Sound's, a track's and the
    // master's, each name the Event Values the VCS shows.
    const auto master = talas::app::automationTargets(document, talas::project::ScoreAutomationScope::Master, "master");
    const auto clip = talas::app::automationTargets(document, talas::project::ScoreAutomationScope::Clip, "clip");
    for (const auto* targets : {&master, &clip})
    {
        require(std::ranges::find(*targets, talas::app::AutomationTarget{"Rate", talas::expr::ValueDomain::Scalar}) != targets->end()
                && std::ranges::find(*targets, talas::app::AutomationTarget{"Frequency", talas::expr::ValueDomain::Hertz}) != targets->end(),
            "a Sound's and the master's controls should name the VCS's Event Values");
    }
}

// The pencil draws a new function; its points
// replace those within the stroke's span.
void testPencilStrokes()
{
    const auto base = lane({{seconds(0.0), 0.0, Interpolation::Step},
        {seconds(1.0), 1.0, Interpolation::Step},
        {seconds(2.0), 0.5, Interpolation::Step},
        {seconds(3.0), 0.25, Interpolation::Step}});
    const std::vector<talas::app::AutomationStrokePoint> stroke{
        {0.5, 0.2}, {0.6, 0.3}, {0.62, 0.35}, {1.2, 0.9}, {2.5, 0.1}};
    const auto drawn = talas::app::drawAutomationStroke(base, stroke, 0.05);
    require(drawn.has_value() && close(times(*drawn), {0.0, 0.5, 0.6, 1.2, 2.5, 3.0})
            && close(values(*drawn), {0.0, 0.2, 0.3, 0.9, 0.1, 0.25}),
        "a stroke should replace the points within its span, points closer than the spacing left out");
    require(drawn->points[0].interpolation == Interpolation::Linear
            && drawn->points[1].interpolation == Interpolation::Linear,
        "a drawn function should run linearly from point to point");
    const std::vector<talas::app::AutomationStrokePoint> backwards{{2.0, 1.0}, {1.0, 0.0}};
    const auto reversed = talas::app::drawAutomationStroke(base, backwards, 0.0);
    require(reversed.has_value() && close(times(*reversed), {0.0, 1.0, 2.0, 3.0})
            && close(values(*reversed), {0.0, 0.0, 1.0, 0.25}),
        "a stroke drawn right to left should still keep its times in order");
    require(!talas::app::drawAutomationStroke(base, {}, 0.0).has_value()
            && !talas::app::drawAutomationStroke(base, {{-1.0, 0.0}}, 0.0).has_value(),
        "an empty stroke or one before time 0 draws nothing");
    const auto fresh = talas::app::drawAutomationStroke(lane({}), {{0.0, 0.5}, {1.0, 1.0}}, 0.0);
    require(fresh.has_value() && fresh->points.size() == 2, "a stroke should draw into an empty lane");
}

void testCurvesBetweenPoints()
{
    const auto base = lane({{seconds(0.0), 0.0, Interpolation::Step},
        {seconds(1.0), 1.0, Interpolation::Step}});
    const auto shaped = talas::app::shapeAutomationSegment(
        base, 0, talas::app::AutomationCurveShape::SCurve, 4);
    require(shaped.has_value()
            && close(times(*shaped), {0.0, 0.25, 0.5, 0.75, 1.0})
            && close(values(*shaped), {0.0, 0.5 - 0.5 * std::cos(0.25 * std::numbers::pi), 0.5,
                0.5 - 0.5 * std::cos(0.75 * std::numbers::pi), 1.0})
            && shaped->points[0].interpolation == Interpolation::Linear
            && shaped->points[3].interpolation == Interpolation::Linear,
        "an S-curve should ease from one point to the next in linear steps");
    const auto rising = talas::app::shapeAutomationSegment(
        base, 0, talas::app::AutomationCurveShape::Exponential, 4);
    const auto falling = talas::app::shapeAutomationSegment(
        base, 0, talas::app::AutomationCurveShape::Logarithmic, 4);
    require(rising.has_value() && falling.has_value()
            && rising->points[2].value < 0.5 && falling->points[2].value > 0.5,
        "an exponential curve should rise slowly first, a logarithmic one fast");
    require(!talas::app::shapeAutomationSegment(base, 1, talas::app::AutomationCurveShape::SCurve).has_value(),
        "the last point has no segment to shape");
    const auto musical = talas::app::shapeAutomationSegment(
        lane({{ticks(0), 0.0, Interpolation::Step}, {ticks(3), 1.0, Interpolation::Step}}),
        0, talas::app::AutomationCurveShape::SCurve, 16);
    require(musical.has_value() && close(times(*musical), {0.0, 1.0, 2.0, 3.0}),
        "a musical-time segment should divide only into whole ticks");
}

// Invert, Loop, Reverse, Spray can, Square off,
// Scale, Offset, Threshold and Time Scale.
void testTransformations()
{
    const auto m = lane({{seconds(0.0), 0.0, Interpolation::Linear},
        {seconds(1.0), 1.0, Interpolation::Step},
        {seconds(2.0), 0.25, Interpolation::Linear},
        {seconds(4.0), 0.5, Interpolation::Step}});
    using Transform = talas::app::AutomationTransform;
    const auto apply = [&](Transform transform, double value, double secondary = 0.0) {
        return talas::app::transformAutomationLane(m, transform, {value, secondary, 7});
    };
    require(close(values(*apply(Transform::Invert, 0.5)), {1.0, 0.0, 0.75, 0.5}),
        "Invert should mirror the values about the axis");
    require(close(values(*apply(Transform::Scale, 0.5)), {0.0, 0.5, 0.125, 0.25})
            && close(values(*apply(Transform::Offset, 0.5)), {0.5, 1.5, 0.75, 1.0}),
        "Scale and Offset should multiply and shift the values");
    const auto square = apply(Transform::SquareOff, 0.0);
    require(std::ranges::all_of(square->points, [](const auto& point) {
                return point.interpolation == Interpolation::Step;
            }),
        "Square off should hold each value to the next point");
    require(close(values(*apply(Transform::Threshold, 0.4, 0.4)), {0.0, 1.0, 1.0, 1.0})
            && close(values(*apply(Transform::Threshold, 0.4, 0.0)), {0.0, 1.0, 0.0, 1.0}),
        "Threshold should give 1 above and 0 below, the hysteresis holding small dips");
    const auto reversed = apply(Transform::Reverse, 0.0);
    require(close(times(*reversed), {0.0, 2.0, 3.0, 4.0})
            && close(values(*reversed), {0.5, 0.25, 1.0, 0.0})
            && reversed->points[0].interpolation == Interpolation::Linear
            && reversed->points[1].interpolation == Interpolation::Step
            && reversed->points[2].interpolation == Interpolation::Linear,
        "Reverse should run the graph backwards, each curve with it");
    require(close(times(*apply(Transform::TimeScale, talas::app::automationTimeScaleOffered)), {0.0, 0.5, 1.0, 2.0})
            && !apply(Transform::TimeScale, 0.0).has_value(),
        "Time Scale should stretch the times from the first point by a positive factor, the 0.5 offered halving them");
    const auto looped = apply(Transform::Loop, 9.0);
    require(close(times(*looped), {0.0, 1.0, 2.0, 4.0, 5.0, 6.0, 8.0})
            && close(values(*looped), {0.0, 1.0, 0.25, 0.5, 1.0, 0.25, 0.5})
            && looped->points[3].interpolation == Interpolation::Linear,
        "Loop should repeat the shape as many whole times as fit");
    require(!apply(Transform::Loop, 5.0).has_value(), "a loop with room for one shape changes nothing");
    const auto sprayed = apply(Transform::Spray, 0.2, 0.5);
    require(sprayed.has_value() && sprayed->points.size() == 4 + 1 + 1 + 3,
        "Spray should add a point at each interval between the points");
    const auto again = apply(Transform::Spray, 0.2, 0.5);
    require(*sprayed == *again
            && std::ranges::all_of(sprayed->points, [&](const auto& point) {
                   return point.value >= -0.1 && point.value <= 1.1;
               }),
        "the spray should be repeatable and within its amount");
}

void testEditedLanesAreValidProjectEdits()
{
    talas::project::ProjectEditor editor{talas::project::ProjectDocument{}, registry()};
    talas::project::ScoreTrack master;
    master.trackId = "master";
    master.displayName = "Master";
    master.kind = talas::project::ScoreTrackKind::Master;
    require(editor.addScoreTrack(master).ok(), "add the master track");
    talas::project::ScoreTrack track;
    track.trackId = "track";
    track.displayName = "Track";
    track.outputTrackId = "master";
    const auto added = editor.addScoreTrack(track);
    require(added.ok(), "add a Score track: " + added.message);
    auto value = lane({{seconds(0.0), 0.0, Interpolation::Step}, {seconds(2.0), 1.0, Interpolation::Step}});
    require(editor.addScoreAutomationLane(value).ok(), "add a Rate lane");
    auto edited = talas::app::shapeAutomationSegment(value, 0, talas::app::AutomationCurveShape::SCurve);
    edited = talas::app::transformAutomationLane(*edited, talas::app::AutomationTransform::Loop, {6.0});
    edited = talas::app::transformAutomationLane(*edited, talas::app::AutomationTransform::Spray, {0.1, 0.1, 3});
    require(edited.has_value(), "the edits should apply");
    const auto updated = editor.updateScoreAutomationLane(*edited);
    require(updated.ok(), "an edited lane should be a valid project edit: " + updated.message);
}
}

// A Timeline's transformations as the book
// uses them. Reversing a sample's TimeIndex ramp plays it from its end to
// its beginning; a negative Scale inverts the graph and a Scale has no
// limit; Scale 0.5 then Offset 0.5 keeps a 0 to 1 graph half as high with
// its minimum at 0.5; Scale 100 and Offset 50 take an amplitude envelope
// from 0 to 1 into a Frequency from 50 to 150.
void testTransformationsAsTheBookUsesThem()
{
    using Transform = talas::app::AutomationTransform;
    const auto ramp = lane({{seconds(0.0), 0.0, Interpolation::Linear}, {seconds(1.0), 1.0, Interpolation::Linear}});
    const auto reversed = talas::app::transformAutomationLane(ramp, Transform::Reverse, {});
    require(reversed.has_value() && close(values(*reversed), {1.0, 0.0}) && close(times(*reversed), {0.0, 1.0}),
        "a reversed TimeIndex ramp should run from the end to the beginning");
    const auto envelope = lane({{seconds(0.0), 0.0, Interpolation::Linear}, {seconds(0.5), 0.6, Interpolation::Linear},
        {seconds(1.0), 1.0, Interpolation::Linear}});
    const auto scaled = [&](const talas::project::ScoreAutomationLane& from, double factor) {
        return *talas::app::transformAutomationLane(from, Transform::Scale, {factor, 0.0, 1});
    };
    const auto offset = [&](const talas::project::ScoreAutomationLane& from, double amount) {
        return *talas::app::transformAutomationLane(from, Transform::Offset, {amount, 0.0, 1});
    };
    require(close(values(scaled(envelope, -1.0)), {0.0, -0.6, -1.0}), "a negative Scale should invert the graph");
    require(close(values(scaled(envelope, 1000.0)), {0.0, 600.0, 1000.0}), "a Scale should have no limit");
    require(close(values(offset(scaled(envelope, 0.5), 0.5)), {0.5, 0.8, 1.0}),
        "Scale 0.5 then Offset 0.5 should halve the deviation with its minimum at 0.5");
    require(close(values(offset(scaled(envelope, 100.0), 50.0)), {50.0, 110.0, 150.0}),
        "Scale 100 and Offset 50 should take 0 to 50 and 1 to 150");
}

// Scale to fit stretches or squeezes a function to
// fit exactly once within its Sound's duration; Quantize endpoints moves
// each point onto the nearest line of the quantization.
void testScaleToFitAndQuantizeEndpoints()
{
    using Transform = talas::app::AutomationTransform;
    const auto shape = lane({{seconds(0.5), 0.0, Interpolation::Linear}, {seconds(1.0), 1.0, Interpolation::Linear},
        {seconds(2.5), 0.25, Interpolation::Step}});
    const auto fitted = talas::app::transformAutomationLane(shape, Transform::ScaleToFit, {4.0, 0.0, 1});
    require(fitted.has_value() && close(times(*fitted), {0.0, 1.0, 4.0}) && close(values(*fitted), values(shape)),
        "Scale to fit should fit the function once within the 4 s Sound");
    const auto quantized = talas::app::transformAutomationLane(
        lane({{seconds(0.1), 0.0, Interpolation::Linear}, {seconds(0.62), 1.0, Interpolation::Linear},
            {seconds(1.4), 0.5, Interpolation::Linear}}),
        Transform::QuantizeEndpoints, {0.5, 0.0, 1});
    require(quantized.has_value() && close(times(*quantized), {0.0, 0.5, 1.5}),
        "Quantize endpoints should move each point onto the nearest half-second line");
}

// Several selected points at once set to
// one value, moved with the first point's new time while the rest keep
// their offsets, deleted as a group, or copied and pasted elsewhere.
void testSeveralAutomationPointsAtOnce()
{
    const auto m = lane({{seconds(0.0), 0.0, Interpolation::Linear}, {seconds(1.0), 0.2, Interpolation::Linear},
        {seconds(2.0), 0.4, Interpolation::Linear}, {seconds(3.0), 0.6, Interpolation::Linear}});
    const auto same = talas::app::setAutomationPointValues(m, {1, 2}, 1.0);
    require(same.has_value() && close(values(*same), {0.0, 1.0, 1.0, 0.6}),
        "the selected points should all take the value typed");
    const auto moved = talas::app::moveAutomationPoints(m, {1, 2}, 1.5);
    require(moved.has_value() && close(times(*moved), {0.0, 1.5, 2.5, 3.0}) && close(values(*moved), {0.0, 0.2, 0.4, 0.6}),
        "the first selected point should take the time typed and the rest keep their offsets");
    const auto removed = talas::app::deleteAutomationPoints(m, {1, 2});
    require(removed.has_value() && close(times(*removed), {0.0, 3.0}), "a boxed group of points should go together");
    const auto copied = talas::app::copyAutomationPoints(m, {0, 1});
    const auto pasted = talas::app::pasteAutomationPoints(m, copied, 2.0);
    require(pasted.has_value() && close(times(*pasted), {0.0, 1.0, 2.0, 3.0}) && close(values(*pasted), {0.0, 0.2, 0.0, 0.2}),
        "copied points should paste at the time given, in place of the points there");
}

// The parameter control editor's points: a
// Ctrl+Click point at the time and value clicked, a selection kept where
// it lands, all the selected points set to one value, and the earliest's
// time typed moving them together.
void testThePointEditor()
{
    const auto m = lane({{seconds(0.0), 0.2, Interpolation::Linear}, {seconds(1.0), 0.6, Interpolation::Linear},
        {seconds(2.0), 1.0, Interpolation::Linear}});
    require(std::abs(talas::app::automationValueAtHeight(m, 0.5) - 0.6) < 1e-12
            && talas::app::automationValueAtHeight(m, 0.0) == 0.2 && talas::app::automationValueAtHeight(m, 1.5) == 1.0,
        "the height in the lane reads as the lane shows its values, lowest at the foot");
    const auto added = talas::app::addAutomationPoint(m, seconds(1.5), talas::app::automationValueAtHeight(m, 0.25));
    require(added.has_value() && close(times(*added), {0.0, 1.0, 1.5, 2.0}) && close(values(*added), {0.2, 0.6, 0.4, 1.0}),
        "Ctrl+Click adds a point at the time and value where it is clicked");
    const auto replaced = talas::app::addAutomationPoint(m, seconds(1.0), 0.0);
    require(replaced.has_value() && close(values(*replaced), {0.2, 0.0, 1.0}), "a point added on another takes its place");
    require(talas::app::automationPointTime(seconds(2.5)) == 2.5
            && talas::app::automationPointTime({talas::project::ScoreTimeDomain::MusicalTicks, 0.0, 480}) == 480.0,
        "a point's time as a number of its domain");

    const auto same = talas::app::setAutomationPointValues(m, {0, 2}, 0.5);
    require(same.has_value() && close(values(*same), {0.5, 0.6, 0.5}),
        "Enter and a value set all of the selected points to it");
    const auto moved = talas::app::moveAutomationPoints(m, {0, 1}, 1.5);
    require(moved.has_value() && talas::app::automationPointsAt(*moved, {1.5, 2.5}) == std::vector<std::size_t>{0, 2},
        "the earliest selected point's time typed moves the selection, which stays selected");
    const auto deleted = talas::app::deleteAutomationPoints(m, {0, 2});
    require(deleted.has_value() && close(times(*deleted), {1.0}), "Delete takes the selected points out");

    // A dragged point follows the pointer in time and value; Shift
    // held as the drag starts restricts it to the direction it first takes.
    using talas::app::AutomationDragAxis;
    require(talas::app::automationDragAxis(false, 10.0, 3.0) == AutomationDragAxis::Both
            && talas::app::automationDragAxis(true, 10.0, 3.0) == AutomationDragAxis::Time
            && talas::app::automationDragAxis(true, -2.0, 6.0) == AutomationDragAxis::Value,
        "Shift restricts the drag to its initial direction, horizontal or vertical");
    require(std::abs(talas::app::automationValueDragged(m, 0.6, 0.25) - 0.8) < 1e-12
            && std::abs(talas::app::automationValueDragged(lane({{seconds(0.0), 0.5, Interpolation::Step}}), 0.5, -0.25) - 0.25)
                < 1e-12,
        "a point dragged up or down changes its value by the height moved over the lane's span");

    // The vertical zoom buttons show a larger range of
    // values, so a function can be drawn past its highest, or more detail.
    using talas::app::AutomationValueRange;
    const auto near = [](const AutomationValueRange& range, double low, double high) {
        return std::abs(range.low - low) < 1e-12 && std::abs(range.high - high) < 1e-12;
    };
    require(near(talas::app::automationValueRange(m.points, 0), 0.2, 1.0) && near(talas::app::automationValueRange(m.points, 1), -0.2, 1.4)
            && near(talas::app::automationValueRange(m.points, -1), 0.4, 0.8),
        "zooming out should show twice the range of values, zooming in half of it");
    const auto out = talas::app::automationValueRange(m.points, 1);
    require(std::abs(talas::app::automationValueAtHeight(out, 1.0) - 1.4) < 1e-12
            && std::abs(talas::app::automationHeightOfValue(out, 0.6) - 0.5) < 1e-12,
        "zoomed out, the top of the lane should be past its highest value, to draw the function up to it");
    require(near(talas::app::automationValueRange(std::vector<talas::project::ScoreAutomationPoint>{{seconds(0.0), 0.3,
                                                     Interpolation::Step}},
                     0),
                -0.2, 0.8),
        "a flat lane should show its value at the middle");
}

// The Source of Control menu's takes.
void testTakes()
{
    const auto m = lane({{seconds(0.0), 0.2, Interpolation::Linear}, {seconds(1.0), 0.6, Interpolation::Linear}});
    require(m.takeName == "take1" && talas::app::automationTakeNames(m) == std::vector<std::string>{"take1"},
        "the source of control of an automated parameter is take1, the name of its graph");
    const auto copied = talas::app::automationLaneWithNewTake(m);
    require(copied.has_value() && copied->takeName == "take2" && copied->points == m.points && copied->takes.size() == 1
            && copied->takes.front().name == "take1" && copied->takes.front().points == m.points,
        "New Take creates a new take called take2 and preserves take1");
    const auto recorded = talas::app::automationLaneWithNewTake(*copied, std::vector<talas::project::ScoreAutomationPoint>{
        {seconds(0.5), 1.0, Interpolation::Linear}});
    require(recorded.has_value() && recorded->takeName == "take3" && close(values(*recorded), {1.0})
            && talas::app::automationTakeNames(*recorded) == std::vector<std::string>{"take1", "take2", "take3"},
        "a recording becomes the next take, the source of control, the others kept");
    const auto back = talas::app::automationLaneWithTake(*recorded, "take1");
    require(back.has_value() && back->takeName == "take1" && back->points == m.points
            && talas::app::automationTakeNames(*back).size() == 3,
        "the Source menu returns to take1 instead");
    const auto renamed = talas::app::automationLaneWithTakeRenamed(*back, "take3", "slow rise");
    require(renamed.has_value() && talas::app::automationTakeNames(*renamed) == std::vector<std::string>{"take1", "take2", "slow rise"}
            && !talas::app::automationLaneWithTakeRenamed(*back, "take3", "take2").has_value(),
        "Rename take gives a take a more meaningful name");
    require(!talas::app::automationLaneWithTake(m, "take9").has_value(), "only a lane's own takes can be chosen");
    require(talas::app::nextAutomationTakeName(*renamed) == "take3", "the next new take takes the first free number");

    // Format 129: the takes are saved with the lane.
    talas::project::ProjectDocument document;
    talas::project::ScoreTrack master;
    master.trackId = "master";
    master.displayName = "Master";
    master.kind = talas::project::ScoreTrackKind::Master;
    talas::project::ScoreTrack track;
    track.trackId = "trk1";
    track.displayName = "Trk 1";
    track.outputTrackId = "master";
    document.timeline.score.tracks = {master, track};
    auto saved = *renamed;
    saved.laneId = "rate";
    saved.ownerId = "trk1";
    saved.eventName = "Rate";
    document.timeline.score.automation = {saved};
    const auto json = talas::project::serializeProjectDocument(document);
    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok() && loaded.document.timeline.score.automation.front() == saved, "format 129 saves a lane's takes");
    auto older = json;
    const auto version = older.find("\"version\": 156");
    require(version != std::string::npos, "the lane is written at format 129");
    older.replace(version, std::string_view{"\"version\": 156"}.size(), "\"version\": 128");
    require(!talas::project::parseProjectDocument(older).ok(), "format 128 has no takes");
}

// Slave to and New Master.
// The transformation buttons are
// non-destructive: pressed, the function passes through them without its
// points changing; several combine in order; pressed again, one is taken
// away; on a slaved function they transform the slaved function alone;
// and a copy keeps the transformed function as the lane's own points.
void testTransformationsAreNonDestructive()
{
    talas::project::ProjectScore score;
    talas::project::ScoreTrack master;
    master.trackId = "master";
    master.displayName = "Master";
    master.kind = talas::project::ScoreTrackKind::Master;
    score.tracks = {master};
    talas::project::ScoreAutomationLane reverb;
    reverb.laneId = "reverbMaster";
    reverb.scope = talas::project::ScoreAutomationScope::Master;
    reverb.ownerId = "master";
    reverb.eventName = "ReverbMaster";
    reverb.points = {{seconds(0.0), 0.0, talas::core::AutomationInterpolation::Linear},
        {seconds(1.0), 1.0, talas::core::AutomationInterpolation::Linear},
        {seconds(2.0), 0.25, talas::core::AutomationInterpolation::Linear}};
    const auto original = reverb.points;
    auto inverted = talas::app::automationLaneWithTransformToggled(reverb, talas::app::AutomationTransform::Invert, {0.5});
    require(inverted.has_value() && inverted->points == original && inverted->transforms.size() == 1
            && talas::app::automationLaneTransformed(*inverted, talas::app::AutomationTransform::Invert),
        "a non-destructive transformation leaves the points as they were");
    score.automation = {*inverted};
    auto shown = talas::app::automationShownPoints(score, *inverted);
    require(shown.size() == 3 && shown[0].value == 1.0 && shown[1].value == 0.0 && shown[2].value == 0.75,
        "Invert flips the function about its axis");
    const auto reversed = talas::app::automationLaneWithTransformToggled(*inverted, talas::app::AutomationTransform::Reverse);
    score.automation = {*reversed};
    shown = talas::app::automationShownPoints(score, *reversed);
    require(reversed->transforms.size() == 2 && shown[0].value == 0.75 && shown[2].value == 1.0,
        "that's how you can combine transformations");
    const auto undone = talas::app::automationLaneWithTransformToggled(*reversed, talas::app::AutomationTransform::Invert);
    score.automation = {*undone};
    shown = talas::app::automationShownPoints(score, *undone);
    require(undone->transforms.size() == 1 && undone->points == original && shown[0].value == 0.25 && shown[2].value == 0.0,
        "press the Invert button again to undo the transformation, the other staying");
    require(!talas::app::automationLaneWithTransformToggled(reverb, talas::app::AutomationTransform::TimeScale, {0.0}).has_value(),
        "a transformation that cannot apply is not pressed");

    // The transformations affect the slaved function only.
    talas::project::ScoreAutomationLane slave = reverb;
    slave.laneId = "decay";
    slave.eventName = "Decay";
    slave.masterLaneId = "reverbMaster";
    score.automation = {reverb, slave};
    const auto scaled = talas::app::automationLaneWithTransformToggled(slave, talas::app::AutomationTransform::Scale, {2.0});
    score.automation = {reverb, *scaled};
    require(talas::app::automationShownPoints(score, *scaled)[1].value == 2.0
            && talas::app::automationShownPoints(score, score.automation[0])[1].value == 1.0,
        "the original master function is untransformed");

    // Making a copy of a slaved or transformed function.
    const auto kept = talas::app::automationLaneWithFunctionKept(score, "decay");
    require(kept.has_value() && kept->masterLaneId.empty() && kept->transforms.empty() && kept->points.size() == 3
            && kept->points[1].value == 2.0,
        "to edit a transformed function, make a copy of it, the transformations removed");
    require(!talas::app::automationLaneWithFunctionKept(score, "reverbMaster").has_value(), "an own untransformed function needs no copy");

    talas::project::ProjectDocument document;
    document.timeline.score = score;
    const auto loaded = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));
    require(loaded.ok() && loaded.document.timeline.score.automation[1].transforms == scaled->transforms,
        "a lane's transformations are saved: " + (loaded.errors.empty() ? std::string{} : loaded.errors.front().path + " " + loaded.errors.front().message));
}

// Promote to master makes AmpMaster of Amp, at
// Amp's times in the Timeline, and slaves Amp to it.
void testPromoteToMaster()
{
    talas::project::ProjectScore score;
    talas::project::ScoreTrack master;
    master.trackId = "master";
    master.displayName = "Master";
    master.kind = talas::project::ScoreTrackKind::Master;
    talas::project::ScoreTrack track;
    track.trackId = "trk1";
    track.displayName = "Trk 1";
    track.outputTrackId = "master";
    score.tracks = {master, track};
    score.clips = {{"follower", "trk1", "s", seconds(2.0), seconds(4.0), seconds(0), seconds(0), seconds(0), 1}};
    talas::project::ScoreAutomationLane amp;
    amp.laneId = "amp";
    amp.scope = talas::project::ScoreAutomationScope::Clip;
    amp.ownerId = "follower";
    amp.eventName = "Amp";
    amp.state = talas::project::ScoreAutomationState::Live;
    amp.points = {{seconds(0.0), 0.0, talas::core::AutomationInterpolation::Linear},
        {seconds(1.0), 1.0, talas::core::AutomationInterpolation::Linear}};
    score.automation = {amp};
    const auto promoted = talas::app::automationLanePromotedToMaster(score, "amp", "ampMaster");
    require(promoted.has_value() && promoted->first.eventName == "AmpMaster"
            && promoted->first.scope == talas::project::ScoreAutomationScope::Master && promoted->first.laneId == "ampMaster"
            && promoted->first.state == talas::project::ScoreAutomationState::Live,
        "Promote to master creates a new master control with the default name of AmpMaster");
    require(promoted->first.points.size() == 2 && promoted->first.points[0].position == seconds(2.0)
            && promoted->first.points[1].position == seconds(3.0) && promoted->first.points[1].value == 1.0,
        "the master plays Amp's function at its times in the Timeline");
    require(promoted->second.laneId == "amp" && promoted->second.masterLaneId == "ampMaster",
        "and it slaves Amp to AmpMaster");
    score.automation = {promoted->first, promoted->second};
    require(talas::app::automationShownPoints(score, promoted->second) == amp.points,
        "the slaved Amp shows the same function at its own times");
    require(!talas::app::automationLanePromotedToMaster(score, "amp", "again").has_value()
            && !talas::app::automationLanePromotedToMaster(score, "ampMaster", "again").has_value(),
        "a slaved control or a master is not promoted again");
    auto second = amp;
    second.laneId = "amp2";
    score.automation.push_back(second);
    const auto another = talas::app::automationLanePromotedToMaster(score, "amp2", "ampMaster2");
    require(another.has_value() && another->first.eventName == "AmpMaster2", "a second promotion takes a new name");
}

void testSlaving()
{
    talas::project::ProjectScore score;
    talas::project::ScoreTrack master;
    master.trackId = "master";
    master.displayName = "Master";
    master.kind = talas::project::ScoreTrackKind::Master;
    talas::project::ScoreTrack track;
    track.trackId = "trk1";
    track.displayName = "Trk 1";
    track.outputTrackId = "master";
    talas::project::ScoreTrack other = track;
    other.trackId = "trk2";
    score.tracks = {master, track, other};
    score.clips = {{"euverb", "trk1", "s", seconds(2.0), seconds(4.0), seconds(0), seconds(0), seconds(0), 1},
        {"elsewhere", "trk2", "s", seconds(0.0), seconds(4.0), seconds(0), seconds(0), seconds(0), 1}};
    const auto clipLane = [](std::string id, std::string clip, std::string name) {
        talas::project::ScoreAutomationLane result;
        result.laneId = std::move(id);
        result.scope = talas::project::ScoreAutomationScope::Clip;
        result.ownerId = std::move(clip);
        result.eventName = std::move(name);
        return result;
    };
    score.automation = {clipLane("amp", "euverb", "ReverbAmp"), clipLane("decay", "euverb", "Decay"),
        clipLane("far", "elsewhere", "Far")};
    talas::project::ScoreAutomationLane level;
    level.laneId = "level";
    level.ownerId = "trk1";
    level.eventName = "Gain";
    score.automation.push_back(level);
    const auto made = talas::app::newMasterControl(score, "ReverbMaster", "reverbMaster");
    require(made.has_value() && made->scope == talas::project::ScoreAutomationScope::Master && made->ownerId == "master"
            && made->eventName == "ReverbMaster",
        "New Master makes a master control of the name given");
    require(!talas::app::newMasterControl(score, "two words", "x").has_value(), "a master control's name is an Event Value's");
    auto withMaster = *made;
    withMaster.points = {{seconds(0.0), 0.0, Interpolation::Linear}, {seconds(3.0), 1.0, Interpolation::Linear},
        {seconds(6.0), 0.0, Interpolation::Linear}};
    score.automation.push_back(withMaster);
    require(talas::project::scoreAutomationMasterCandidates(score, "amp") == std::vector<std::string>{"decay", "level", "reverbMaster"},
        "the master is another control in the same Sound, a track control of its track, or a master control");
    const auto slaved = talas::app::automationLaneSlavedTo(score, "amp", "reverbMaster");
    require(slaved.has_value() && slaved->masterLaneId == "reverbMaster", "Slave to chooses the master controller");
    score.automation[0] = *slaved;
    require(!talas::project::projectScoreValidationError(score, std::vector<std::string_view>{"s"}).has_value(),
        "the slaved control is a valid lane");
    const auto shown = talas::app::automationShownPoints(score, score.automation[0]);
    require(close(times(talas::project::ScoreAutomationLane{"", {}, {}, {}, {}, shown}), {1.0, 4.0}) && close(
                values(talas::project::ScoreAutomationLane{"", {}, {}, {}, {}, shown}), {1.0, 0.0}),
        "its graph shows the master's, in the Sound's own time");
    const auto automated = talas::app::automatedEventNames(score);
    require(std::ranges::find(automated, "ReverbAmp") != automated.end(),
        "ReverbMaster is automated, so the parameter slaved to it is too");
    const auto decayMasters = talas::project::scoreAutomationMasterCandidates(score, "decay");
    require(std::ranges::find(decayMasters, "amp") == decayMasters.end()
            && !talas::app::automationLaneSlavedTo(score, "reverbMaster", "level").has_value(),
        "a slaved control is no master, nor is a master slaved");
    const auto freed = talas::app::automationLaneSlavedTo(score, "amp", "");
    require(freed.has_value() && freed->masterLaneId.empty() && freed->points == shown,
        "Unslave makes a local copy of the master function, to edit");
}

// An extracted envelope as a controller's
// points, simplified.
void testEnvelopePoints()
{
    std::vector<double> ramp(1000);
    for (std::size_t frame = 0; frame < ramp.size(); ++frame)
        ramp[frame] = static_cast<double>(frame) / 1000.0;
    const auto straight = talas::app::envelopeAutomationPoints(ramp, 1000.0, 0.01, 0.01);
    require(straight.size() == 2 && straight.front().value == 0.0 && std::abs(straight.back().position.seconds - 0.99) < 1e-12,
        "a straight envelope needs only its ends");
    std::vector<double> peak(1000);
    for (std::size_t frame = 0; frame < peak.size(); ++frame)
        peak[frame] = frame < 500 ? static_cast<double>(frame) / 500.0 : static_cast<double>(1000 - frame) / 500.0;
    const auto shaped = talas::app::envelopeAutomationPoints(peak, 1000.0, 0.01, 0.01);
    require(shaped.size() == 3 && std::abs(shaped[1].position.seconds - 0.5) < 1e-12 && shaped[1].value == 1.0,
        "the shape of the controller follows the envelope");
    require(std::ranges::all_of(shaped, [](const auto& point) {
        return point.interpolation == Interpolation::Linear && point.position.domain == talas::project::ScoreTimeDomain::Seconds;
    }), "each point is linear to the next, in seconds");
}

// Set selected Sounds' live controls to
// current values gives each control of the clip a lane holding its VCS
// value throughout, the lane it had for it replaced.
void testLiveControlsBecomeConstantLanes()
{
    talas::project::ProjectScore score;
    score.clips.push_back({"clip", "track", "voice", seconds(2.0), seconds(4.0), seconds(0.0), seconds(0.0), seconds(0.0), 1});
    score.automation.push_back({"old", talas::project::ScoreAutomationScope::Clip, "clip", "Rate", talas::expr::ValueDomain::Scalar,
        {{seconds(0.0), 0.1, Interpolation::Linear}, {seconds(1.0), 0.9, Interpolation::Linear}}});
    const auto lanes = talas::app::liveControlsAsLanes(score, "clip",
        {{"Rate", talas::expr::ValueDomain::Scalar, 0.8}, {"Frequency", talas::expr::ValueDomain::Hertz, 440.0}});
    require(lanes.has_value() && lanes->size() == 2, "each live control of the Sound should become a lane");
    require(lanes->at(0).laneId == "old" && lanes->at(0).points.size() == 1 && lanes->at(0).points.front().value == 0.8
            && lanes->at(0).points.front().interpolation == Interpolation::Step,
        "Rate should hold its captured 0.8, a constant in place of its old lane");
    require(lanes->at(1).scope == talas::project::ScoreAutomationScope::Clip && lanes->at(1).ownerId == "clip"
            && lanes->at(1).points.front().value == 440.0 && lanes->at(1).domain == talas::expr::ValueDomain::Hertz,
        "Frequency should hold its VCS value on the clip");
    require(!talas::app::liveControlsAsLanes(score, "missing", {}).has_value(), "a clip that is not there should be refused");
}

// The Automation menu sets a clip's
// parameter controls, or its track's level and angle, Live, Automated or
// Record-enabled at once; an Automated lane's control leaves the virtual
// control surface, and a recording takes the Record-enabled lanes.
void testLaneStatesAsTheAutomationMenuSetsThem()
{
    using State = talas::project::ScoreAutomationState;
    talas::project::ProjectScore score;
    score.clips.push_back({"clip", "track", "voice", seconds(0.0), seconds(4.0), seconds(0.0), seconds(0.0), seconds(0.0), 1});
    score.clips.push_back({"other", "second", "voice", seconds(0.0), seconds(4.0), seconds(0.0), seconds(0.0), seconds(0.0), 1});
    const std::vector<talas::project::ScoreAutomationPoint> points{{seconds(0.0), 0.5, Interpolation::Linear}};
    score.automation.push_back({"rate", talas::project::ScoreAutomationScope::Clip, "clip", "Rate", talas::expr::ValueDomain::Scalar, points});
    score.automation.push_back({"gain", talas::project::ScoreAutomationScope::Track, "track", "Gain", talas::expr::ValueDomain::Scalar, points});
    score.automation.push_back({"angle", talas::project::ScoreAutomationScope::Track, "track", "Angle", talas::expr::ValueDomain::Scalar, points});
    score.automation.push_back({"cutoff", talas::project::ScoreAutomationScope::Clip, "other", "Cutoff", talas::expr::ValueDomain::Scalar, points});
    const std::optional<std::vector<std::string>> clip{std::vector<std::string>{"clip"}};
    const auto live = talas::app::lanesInState(score, clip, talas::app::AutomationControlGroup::ParameterControls, State::Live);
    require(live.size() == 1 && live.front().laneId == "rate" && live.front().state == State::Live,
        "Live Control sets the selected clip's parameter lanes live, no other clip's");
    const auto record = talas::app::lanesInState(score, clip, talas::app::AutomationControlGroup::TrackLevelAndAngle, State::Record);
    require(record.size() == 2 && record[0].laneId == "gain" && record[1].laneId == "angle"
            && record[0].state == State::Record,
        "Record Enable for track level and angle takes the clip's track's Gain and Angle");
    require(talas::app::lanesInState(score, clip, talas::app::AutomationControlGroup::ParameterControls, State::Automated).empty(),
        "a lane already in the state is not changed");
    // Marking a Sound for caching arms its live parameters, the
    // clip's Live lanes, for recording; Automated ones stay as they are.
    auto marked = score;
    marked.automation[0].state = State::Live;
    marked.automation[3].state = State::Live;
    const auto armed = talas::app::liveLanesArmedForCache(marked, "clip");
    require(armed.size() == 1 && armed.front().laneId == "rate" && armed.front().state == State::Record
            && talas::app::liveLanesArmedForCache(score, "clip").empty(),
        "marking a Sound for caching should arm its live parameters for recording");
    score.automation.push_back({"lonely", talas::project::ScoreAutomationScope::Track, "empty", "Gain", talas::expr::ValueDomain::Scalar, points});
    const auto every = talas::app::lanesInState(score, std::nullopt, talas::app::AutomationControlGroup::TrackLevelAndAngle, State::Live);
    require(every.size() == 3 && every.back().laneId == "lonely",
        "with no Sound selected, every track's level and angle goes live, a track without Sounds too");
    require(talas::app::lanesInState(score, std::nullopt, talas::app::AutomationControlGroup::ParameterControls, State::Live).size() == 2,
        "with no Sound selected, every clip's parameter controls change");
    score.automation.pop_back();

    auto names = talas::app::automatedEventNames(score);
    require(names == std::vector<std::string>{"Rate", "Gain", "Angle", "Cutoff"},
        "every lane with points is Automated, its control off the surface");
    score.automation[0].state = State::Live;
    score.automation[2].state = State::Record;
    names = talas::app::automatedEventNames(score);
    require(names == std::vector<std::string>{"Gain", "Cutoff"},
        "a Live lane's control comes back to the virtual control surface");
    const auto recording = talas::app::recordEnabledLanes(score);
    require(recording.size() == 1 && recording.front()->laneId == "angle",
        "a recording takes the Record-enabled lanes");
}

// The Timeline's lower half is its controls
// editor, sized by the line dragged between it and the score, with the
// controls listed on the left and the selected one's function drawn to
// their right.
void testTheControlsEditorInTheLowerHalf()
{
    const auto split = talas::app::timelineControlsSplit(600, 0.3, 60);
    require(split.scoreHeight == 420 && split.editorHeight == 180,
        "the controls editor should take the lower part of the Timeline");
    require(talas::app::timelineControlsSplit(600, 0.95, 60).scoreHeight == 60
            && talas::app::timelineControlsSplit(600, 0.0, 60).editorHeight == 60
            && std::abs(talas::app::timelineControlsShare(400, 600) - 1.0 / 3.0) < 1e-12
            && talas::app::timelineControlsSplit(600, talas::app::timelineControlsShare(300, 600), 60).editorHeight == 300,
        "dragging the line between the halves should change their sizes, each keeping some height");
    talas::project::ProjectScore score;
    score.automation = {lane({{seconds(0.0), 0.2, Interpolation::Linear}, {seconds(1.0), 0.6, Interpolation::Step},
        {seconds(2.0), 1.0, Interpolation::Step}})};
    const auto curve = talas::app::timelineControlsCurve(score, score.automation.front(), 0.0, 2.0, 5);
    std::vector<double> shown;
    std::vector<double> heights;
    for (const auto& point : curve)
    {
        shown.push_back(point.value);
        heights.push_back(point.height);
    }
    require(close(shown, {0.2, 0.4, 0.6, 0.6, 1.0}) && close(heights, {0.0, 0.25, 0.5, 0.5, 1.0}),
        "the selected control's graph should show how its value varies over time");
    const auto added = talas::app::timelineControlsPointAdded(score.automation.front(), 0.0, 2.0, 0.75, 0.5);
    require(added.has_value() && close(times(*added), {0.0, 1.0, 1.5, 2.0}) && close(values(*added), {0.2, 0.6, 0.6, 1.0}),
        "a Ctrl+Click in the lower half's graph should add a point at the time and value clicked");
}

// The transformation buttons across the
// bottom of the controls editor, in the book's order, each with fly-by help
// that says what it would do before it is clicked.
void testTheTransformationButtonsFlyByHelp()
{
    using Transform = talas::app::AutomationTransform;
    const auto buttons = talas::app::automationTransformButtons();
    std::vector<Transform> order;
    for (const auto& button : buttons)
        order.push_back(button.transform);
    require(order
            == std::vector<Transform>{Transform::Invert, Transform::Loop, Transform::Reverse, Transform::Spray,
                Transform::SquareOff, Transform::Scale, Transform::Offset, Transform::Threshold, Transform::TimeScale,
                Transform::ScaleToFit, Transform::QuantizeEndpoints},
        "a button for each transformation, in the book's order");
    require(order[1] == Transform::Loop && order[3] == Transform::Spray && order[4] == Transform::SquareOff
            && order[order.size() - 3] == Transform::TimeScale,
        "Loop second from the left, the spray can fourth, Square off next to it, Time scale third from the right");
    std::vector<std::string> helps;
    for (const auto& button : buttons)
    {
        require(!button.label.empty() && button.help.size() > button.label.size(),
            "each button has a label and fly-by help saying what it does");
        helps.emplace_back(button.help);
    }
    std::ranges::sort(helps);
    require(std::ranges::adjacent_find(helps) == helps.end(), "each button's help is its own");
    const auto help = [](const talas::project::ScoreAutomationLane* lane, Transform transform) {
        return talas::app::automationTransformButtonHelp(lane, transform);
    };
    require(help(nullptr, Transform::Invert).find("upside down") != std::string::npos
            && help(nullptr, Transform::Spray).find("random") != std::string::npos
            && help(nullptr, Transform::SquareOff).find("jump") != std::string::npos
            && help(nullptr, Transform::TimeScale).find("0.5") != std::string::npos
            && help(nullptr, Transform::Loop).find("repeats") != std::string::npos,
        "the help reads what the effect of the button would be");
    auto rate = lane({{seconds(0.0), 0.0, Interpolation::Linear}, {seconds(1.0), 1.0, Interpolation::Linear}});
    require(help(&rate, Transform::Invert).find("click it again") != std::string::npos
            && help(&rate, Transform::Invert).find("Pressed on") == std::string::npos,
        "before a press the help says the points stay as they are");
    const auto inverted = talas::app::automationLaneWithTransformToggled(rate, Transform::Invert, {0.5});
    require(inverted.has_value() && help(&*inverted, Transform::Invert).find("Pressed on Rate") != std::string::npos
            && help(&*inverted, Transform::Reverse).find("Pressed on") == std::string::npos,
        "on a lane it transforms, the help says the button is pressed and a click takes it away");
}

int main()
{
    try
    {
        testTargetsAreEveryControlTheOwnerDoesNotAutomate();
        testCurvesBetweenPoints();
        testPencilStrokes();
        testTransformations();
        testTransformationsAsTheBookUsesThem();
        testScaleToFitAndQuantizeEndpoints();
        testSeveralAutomationPointsAtOnce();
        testThePointEditor();
        testTakes();
        testSlaving();
        testPromoteToMaster();
        testTransformationsAreNonDestructive();
        testEnvelopePoints();
        testLiveControlsBecomeConstantLanes();
        testLaneStatesAsTheAutomationMenuSetsThem();
        testEditedLanesAreValidProjectEdits();
        testTheControlsEditorInTheLowerHalf();
        testTheTransformationButtonsFlyByHelp();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
