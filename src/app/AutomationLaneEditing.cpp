#include "talas/app/AutomationLaneEditing.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <numbers>
#include <utility>

namespace talas::app
{
namespace
{
using talas::project::ScoreAutomationLane;
using talas::project::ScoreAutomationPoint;
using talas::project::ScoreTime;
using talas::project::ScoreTimeDomain;
using Interpolation = talas::core::AutomationInterpolation;

[[nodiscard]] double timeValue(const ScoreTime& time) noexcept
{
    return time.domain == ScoreTimeDomain::Seconds
        ? time.seconds
        : static_cast<double>(time.tick);
}

[[nodiscard]] std::optional<ScoreTime> makeTime(ScoreTimeDomain domain, double value)
{
    if (!std::isfinite(value) || value < 0.0)
        return std::nullopt;
    ScoreTime time;
    time.domain = domain;
    if (domain == ScoreTimeDomain::Seconds)
        time.seconds = value;
    else
    {
        const auto rounded = std::round(value);
        if (rounded > 9.0e18)
            return std::nullopt;
        time.tick = static_cast<std::uint64_t>(rounded);
    }
    return time;
}

// Keeps the points strictly increasing, the first of equal times winning,
// and refuses a lane past the project's point limit.
[[nodiscard]] std::optional<ScoreAutomationLane> finished(ScoreAutomationLane lane)
{
    std::vector<ScoreAutomationPoint> points;
    points.reserve(lane.points.size());
    for (auto& point : lane.points)
    {
        if (!std::isfinite(point.value))
            return std::nullopt;
        if (!points.empty() && timeValue(point.position) <= timeValue(points.back().position))
            continue;
        points.push_back(std::move(point));
    }
    if (points.size() > talas::project::maximumScoreAutomationPoints)
        return std::nullopt;
    lane.points = std::move(points);
    return lane;
}

[[nodiscard]] double curve(AutomationCurveShape shape, double x) noexcept
{
    constexpr double bend = 4.0;
    switch (shape)
    {
    case AutomationCurveShape::Exponential:
        return std::expm1(bend * x) / std::expm1(bend);
    case AutomationCurveShape::Logarithmic:
        return std::log1p(std::expm1(bend) * x) / bend;
    case AutomationCurveShape::SCurve:
        return 0.5 - 0.5 * std::cos(std::numbers::pi * x);
    }
    return x;
}

}

std::vector<std::string_view> automationLiveControls(talas::project::ScoreAutomationScope scope) noexcept
{
    if (scope == talas::project::ScoreAutomationScope::Track)
        return {"Notes"};
    return {};
}

std::vector<AutomationTarget> automationTargets(
    const talas::project::ProjectDocument& document,
    talas::project::ScoreAutomationScope scope,
    std::string_view ownerId)
{
    std::vector<AutomationTarget> targets{{"Gain", talas::expr::ValueDomain::Scalar}};
    // A track's position: Angle 0 hard left to 1 hard right,
    // Radius 0 at the listener to 1 on the circle.
    // A Sound's position relative to its track's.
    if (scope == talas::project::ScoreAutomationScope::Clip)
    {
        // The three controls every Sound in a Timeline has.
        targets.push_back({"FadeInOut", talas::expr::ValueDomain::Scalar});
        targets.push_back({"Panner", talas::expr::ValueDomain::Scalar});
        targets.push_back({"Distance", talas::expr::ValueDomain::Scalar});
    }
    if (scope == talas::project::ScoreAutomationScope::Track)
    {
        // The track's Level, the fader its Sounds pass through.
        targets.push_back({"Level", talas::expr::ValueDomain::Scalar});
        targets.push_back({"Angle", talas::expr::ValueDomain::Scalar});
        targets.push_back({"Radius", talas::expr::ValueDomain::Scalar});
        const auto& tracks = document.timeline.score.tracks;
        const auto track = std::ranges::find(tracks, ownerId, &talas::project::ScoreTrack::trackId);
        if (track != tracks.end() && track->kind == talas::project::ScoreTrackKind::Track)
        {
            auto submixes = track->sendTrackIds;
            submixes.insert(submixes.begin(), track->outputTrackId);
            for (const auto& submixId : submixes)
            {
                const auto submix = std::ranges::find(tracks, submixId, &talas::project::ScoreTrack::trackId);
                if (submix == tracks.end() || submix->kind != talas::project::ScoreTrackKind::Submix)
                    continue;
                const auto name = [](const talas::project::ScoreTrack& named) {
                    return named.displayName.empty() ? named.trackId : named.displayName;
                };
                targets.push_back({talas::project::scoreSendLevelName(submixId), talas::expr::ValueDomain::Scalar,
                    name(*track) + "_To_" + name(*submix)});
            }
        }
    }
    for (const auto& control : document.virtualControls)
    {
        const AutomationTarget target{control.eventName, control.domain};
        if (std::ranges::find(targets, target) == targets.end())
            targets.push_back(target);
    }
    std::erase_if(targets, [&](const AutomationTarget& target) {
        return std::ranges::any_of(document.timeline.score.automation, [&](const auto& lane) {
            return lane.scope == scope && lane.ownerId == ownerId
                && lane.eventName == target.eventName && lane.domain == target.domain;
        });
    });
    return targets;
}

std::optional<ScoreAutomationLane> drawAutomationStroke(
    ScoreAutomationLane lane,
    const std::vector<AutomationStrokePoint>& stroke,
    double minimumSpacing)
{
    if (stroke.empty() || !std::isfinite(minimumSpacing) || minimumSpacing < 0.0)
        return std::nullopt;
    const auto domain = lane.points.empty() ? ScoreTimeDomain::Seconds : lane.points.front().position.domain;
    auto first = stroke.front().time;
    auto last = stroke.front().time;
    for (const auto& point : stroke)
    {
        if (!std::isfinite(point.time) || !std::isfinite(point.value) || point.time < 0.0)
            return std::nullopt;
        first = std::min(first, point.time);
        last = std::max(last, point.time);
    }
    // The drawn points in time order, a later stroke point over an earlier
    // one at the same time.
    std::vector<AutomationStrokePoint> drawn(stroke.begin(), stroke.end());
    std::ranges::stable_sort(drawn, {}, &AutomationStrokePoint::time);
    std::vector<ScoreAutomationPoint> kept;
    for (std::size_t index = 0; index < drawn.size(); ++index)
    {
        const auto isLast = index + 1 == drawn.size();
        if (!kept.empty() && !isLast && drawn[index].time - timeValue(kept.back().position) < minimumSpacing)
            continue;
        const auto time = makeTime(domain, drawn[index].time);
        if (!time.has_value())
            return std::nullopt;
        if (!kept.empty() && timeValue(*time) <= timeValue(kept.back().position))
        {
            kept.back().value = drawn[index].value;
            continue;
        }
        kept.push_back({*time, drawn[index].value, Interpolation::Linear});
    }
    std::vector<ScoreAutomationPoint> points;
    for (const auto& point : lane.points)
    {
        if (timeValue(point.position) < first)
            points.push_back(point);
    }
    if (!points.empty())
        points.back().interpolation = Interpolation::Linear;
    points.insert(points.end(), kept.begin(), kept.end());
    for (const auto& point : lane.points)
    {
        if (timeValue(point.position) > last)
            points.push_back(point);
    }
    lane.points = std::move(points);
    return finished(std::move(lane));
}

std::optional<ScoreAutomationLane> shapeAutomationSegment(
    ScoreAutomationLane lane,
    std::size_t pointIndex,
    AutomationCurveShape shape,
    std::size_t steps)
{
    if (pointIndex + 1 >= lane.points.size() || steps < 2)
        return std::nullopt;
    const auto from = lane.points[pointIndex];
    const auto to = lane.points[pointIndex + 1];
    const auto start = timeValue(from.position);
    const auto span = timeValue(to.position) - start;
    if (!(span > 0.0))
        return std::nullopt;
    // Whole ticks bound how finely a musical-time segment divides.
    if (from.position.domain == ScoreTimeDomain::MusicalTicks)
        steps = std::min<std::size_t>(steps, static_cast<std::size_t>(span));
    if (steps < 2)
        return std::nullopt;
    std::vector<ScoreAutomationPoint> interior;
    interior.reserve(steps - 1);
    for (std::size_t step = 1; step < steps; ++step)
    {
        const auto x = static_cast<double>(step) / static_cast<double>(steps);
        const auto time = makeTime(from.position.domain, start + span * x);
        if (!time.has_value())
            return std::nullopt;
        interior.push_back({
            *time,
            from.value + (to.value - from.value) * curve(shape, x),
            Interpolation::Linear});
    }
    lane.points[pointIndex].interpolation = Interpolation::Linear;
    lane.points.insert(
        lane.points.begin() + static_cast<std::ptrdiff_t>(pointIndex + 1),
        interior.begin(),
        interior.end());
    return finished(std::move(lane));
}

std::optional<ScoreAutomationLane> transformAutomationLane(
    ScoreAutomationLane lane,
    AutomationTransform transform,
    const AutomationTransformParameters& parameters)
{
    auto points = talas::project::transformedScoreAutomationPoints(
        std::move(lane.points), {transform, parameters.value, parameters.secondary, parameters.seed});
    if (!points.has_value())
        return std::nullopt;
    lane.points = std::move(*points);
    return finished(std::move(lane));
}

std::optional<ScoreAutomationLane> automationLaneWithTransformToggled(
    ScoreAutomationLane lane,
    AutomationTransform transform,
    const AutomationTransformParameters& parameters)
{
    const auto pressed = std::ranges::find(lane.transforms, transform, &talas::project::ScoreAutomationTransform::kind);
    if (pressed != lane.transforms.end())
    {
        lane.transforms.erase(pressed);
        return lane;
    }
    const talas::project::ScoreAutomationTransform added{transform, parameters.value, parameters.secondary, parameters.seed};
    if (lane.transforms.size() >= talas::project::maximumScoreAutomationTransforms
        || !talas::project::transformedScoreAutomationPoints(lane.points, added).has_value())
        return std::nullopt;
    lane.transforms.push_back(added);
    return lane;
}

bool automationLaneTransformed(const ScoreAutomationLane& lane, AutomationTransform transform)
{
    return std::ranges::find(lane.transforms, transform, &talas::project::ScoreAutomationTransform::kind) != lane.transforms.end();
}

// The buttons in order: the spray can fourth from the left, Square off
// beside it, Loop second and Time scale third from the right, each with its
// help.
std::span<const AutomationTransformButton> automationTransformButtons() noexcept
{
    static constexpr std::array<AutomationTransformButton, 11> buttons{{
        {AutomationTransform::Invert, "Invert",
            "Invert: flips the selected control's function upside down about the value you give, its axis."},
        {AutomationTransform::Loop, "Loop",
            "Loop: repeats the function's shape, as many whole times as fit, up to the time you give."},
        {AutomationTransform::Reverse, "Reverse",
            "Reverse: plays the function backwards, flipping it about a vertical axis."},
        {AutomationTransform::Spray, "Spray",
            "Spray can: adds random jitter between the points, as large as the amount you give, a new value at "
            "each interval you give."},
        {AutomationTransform::SquareOff, "Square",
            "Square off: makes the function jump to each point's value and hold it, instead of sloping to the "
            "next."},
        {AutomationTransform::Scale, "Scale",
            "Scale: multiplies every value by the factor you give; a negative factor turns it upside down."},
        {AutomationTransform::Offset, "Offset",
            "Offset: adds the value you give to every point, moving the whole function up or down."},
        {AutomationTransform::Threshold, "Thresh",
            "Threshold: 1 wherever the function is above the threshold you give and 0 below it; the hysteresis "
            "is how far past it the function must go to change."},
        {AutomationTransform::TimeScale, "Time",
            "Time scale: stretches or compresses the function in time by the factor you give (0.5 halves it)."},
        {AutomationTransform::ScaleToFit, "Fit",
            "Scale to fit: stretches or compresses the function to fit exactly once within its Sound's duration."},
        {AutomationTransform::QuantizeEndpoints, "Quant",
            "Quantize endpoints: moves each point earlier or later onto the nearest step of the quantization you "
            "give."},
    }};
    return buttons;
}

std::string automationTransformButtonHelp(const ScoreAutomationLane* lane, AutomationTransform transform)
{
    const auto buttons = automationTransformButtons();
    const auto button = std::ranges::find(buttons, transform, &AutomationTransformButton::transform);
    if (button == buttons.end())
        return {};
    std::string help{button->help};
    if (lane == nullptr)
        return help + " Select a control to transform.";
    help += automationLaneTransformed(*lane, transform)
        ? " Pressed on " + lane->eventName + ": click it again to take it away."
        : " The points stay as they are; click it again later to take it away.";
    return help;
}

std::optional<ScoreAutomationLane> automationLaneWithFunctionKept(const talas::project::ProjectScore& score, std::string_view laneId)
{
    const auto lane = std::ranges::find(score.automation, laneId, &ScoreAutomationLane::laneId);
    if (lane == score.automation.end() || (lane->transforms.empty() && lane->masterLaneId.empty()))
        return std::nullopt;
    auto kept = *lane;
    kept.points = automationShownPoints(score, *lane);
    kept.transforms.clear();
    kept.masterLaneId.clear();
    return finished(std::move(kept));
}

namespace
{
[[nodiscard]] bool validSelection(const ScoreAutomationLane& lane, const std::vector<std::size_t>& selected) noexcept
{
    return !selected.empty() && std::ranges::is_sorted(selected) && std::ranges::adjacent_find(selected) == selected.end()
        && selected.back() < lane.points.size();
}
}

double automationValueAtHeight(const ScoreAutomationLane& lane, double height)
{
    if (lane.points.empty())
        return 0.0;
    const auto [lowest, highest] = std::ranges::minmax_element(lane.points, {}, &ScoreAutomationPoint::value);
    return lowest->value + std::clamp(height, 0.0, 1.0) * (highest->value - lowest->value);
}

TimelineControlsSplit timelineControlsSplit(int height, double editorShare, int minimum) noexcept
{
    height = std::max(0, height);
    const auto share = std::isfinite(editorShare) ? std::clamp(editorShare, 0.0, 1.0) : 0.0;
    auto editor = static_cast<int>(std::lround(share * static_cast<double>(height)));
    if (height >= 2 * minimum)
        editor = std::clamp(editor, minimum, height - minimum);
    return {height - editor, editor};
}

double timelineControlsShare(int lineY, int height) noexcept
{
    if (height <= 0)
        return 0.0;
    return std::clamp(static_cast<double>(height - lineY) / static_cast<double>(height), 0.0, 1.0);
}

std::vector<TimelineControlsCurvePoint> timelineControlsCurve(const talas::project::ProjectScore& score,
    const talas::project::ScoreAutomationLane& lane, double from, double to, std::size_t columns)
{
    auto points = automationShownPoints(score, lane);
    std::ranges::stable_sort(points, {}, [](const ScoreAutomationPoint& point) { return automationPointTime(point.position); });
    std::vector<TimelineControlsCurvePoint> curve;
    if (points.empty() || columns == 0 || !(to >= from))
        return curve;
    const auto range = automationValueRange(points, 0);
    const auto valueAt = [&](double time) {
        if (time <= automationPointTime(points.front().position))
            return points.front().value;
        for (std::size_t index = 0; index + 1 < points.size(); ++index)
        {
            const auto start = automationPointTime(points[index].position);
            const auto end = automationPointTime(points[index + 1].position);
            if (time >= end)
                continue;
            if (points[index].interpolation != talas::core::AutomationInterpolation::Linear || !(end > start))
                return points[index].value;
            return points[index].value + (points[index + 1].value - points[index].value) * (time - start) / (end - start);
        }
        return points.back().value;
    };
    for (std::size_t column = 0; column < columns; ++column)
    {
        const auto time = columns == 1 ? from
                                       : from + (to - from) * static_cast<double>(column) / static_cast<double>(columns - 1);
        const auto value = valueAt(time);
        curve.push_back({time, value, automationHeightOfValue(range, value)});
    }
    return curve;
}

std::optional<talas::project::ScoreAutomationLane> timelineControlsPointAdded(
    talas::project::ScoreAutomationLane lane, double from, double to, double across, double up)
{
    if (!std::isfinite(across) || !std::isfinite(up) || !(to >= from))
        return std::nullopt;
    const auto time = from + (to - from) * std::clamp(across, 0.0, 1.0);
    ScoreTime position;
    position.domain = lane.points.empty() ? ScoreTimeDomain::Seconds : lane.points.front().position.domain;
    if (position.domain == ScoreTimeDomain::Seconds)
        position.seconds = time;
    else
        position.tick = static_cast<std::uint64_t>(std::llround(std::max(0.0, time)));
    const auto value = automationValueAtHeight(lane, std::clamp(up, 0.0, 1.0));
    return addAutomationPoint(std::move(lane), position, value);
}

AutomationValueRange automationValueRange(std::span<const ScoreAutomationPoint> points, int zoomOutSteps)
{
    AutomationValueRange range{0.0, 1.0};
    if (!points.empty())
    {
        const auto [lowest, highest] = std::ranges::minmax_element(points, {}, &ScoreAutomationPoint::value);
        range = highest->value > lowest->value ? AutomationValueRange{lowest->value, highest->value}
                                               : AutomationValueRange{lowest->value - 0.5, lowest->value + 0.5};
    }
    const auto middle = 0.5 * (range.low + range.high);
    const auto half = 0.5 * (range.high - range.low) * std::pow(2.0, std::clamp(zoomOutSteps, -16, 16));
    return {middle - half, middle + half};
}

double automationHeightOfValue(const AutomationValueRange& range, double value) noexcept
{
    return range.high > range.low ? (value - range.low) / (range.high - range.low) : 0.5;
}

double automationValueAtHeight(const AutomationValueRange& range, double height) noexcept
{
    return range.low + height * (range.high - range.low);
}

AutomationDragAxis automationDragAxis(bool shift, double horizontal, double vertical) noexcept
{
    if (!shift)
        return AutomationDragAxis::Both;
    return std::abs(horizontal) >= std::abs(vertical) ? AutomationDragAxis::Time : AutomationDragAxis::Value;
}

double automationValueDragged(const ScoreAutomationLane& lane, double startValue, double heightMoved)
{
    auto span = 1.0;
    if (!lane.points.empty())
    {
        const auto [lowest, highest] = std::ranges::minmax_element(lane.points, {}, &ScoreAutomationPoint::value);
        if (highest->value > lowest->value)
            span = highest->value - lowest->value;
    }
    return startValue + heightMoved * span;
}

std::optional<ScoreAutomationLane> addAutomationPoint(ScoreAutomationLane lane, const ScoreTime& position, double value)
{
    if (!std::isfinite(value) || (!lane.points.empty() && lane.points.front().position.domain != position.domain))
        return std::nullopt;
    const auto at = std::ranges::find_if(lane.points, [&](const ScoreAutomationPoint& point) {
        return timeValue(point.position) >= timeValue(position);
    });
    if (at != lane.points.end() && timeValue(at->position) == timeValue(position))
        at->value = value;
    else
        lane.points.insert(at, {position, value, lane.points.empty() ? talas::core::AutomationInterpolation::Step
                                                                     : lane.points.back().interpolation});
    return finished(std::move(lane));
}

double automationPointTime(const ScoreTime& position) noexcept
{
    return timeValue(position);
}

std::vector<std::size_t> automationPointsAt(const ScoreAutomationLane& lane, const std::vector<double>& times)
{
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < lane.points.size(); ++index)
    {
        if (std::ranges::find(times, timeValue(lane.points[index].position)) != times.end())
            indices.push_back(index);
    }
    return indices;
}

std::optional<ScoreAutomationLane> setAutomationPointValues(
    ScoreAutomationLane lane, const std::vector<std::size_t>& selected, double value)
{
    if (!validSelection(lane, selected) || !std::isfinite(value))
        return std::nullopt;
    for (const auto index : selected)
        lane.points[index].value = value;
    return finished(std::move(lane));
}

std::optional<ScoreAutomationLane> moveAutomationPoints(
    ScoreAutomationLane lane, const std::vector<std::size_t>& selected, double firstTime)
{
    if (!validSelection(lane, selected))
        return std::nullopt;
    const auto offset = firstTime - timeValue(lane.points[selected.front()].position);
    std::vector<ScoreAutomationPoint> moved;
    std::vector<ScoreAutomationPoint> kept;
    for (std::size_t index = 0; index < lane.points.size(); ++index)
    {
        auto point = lane.points[index];
        if (std::ranges::binary_search(selected, index))
        {
            const auto time = makeTime(point.position.domain, timeValue(point.position) + offset);
            if (!time.has_value())
                return std::nullopt;
            point.position = *time;
            moved.push_back(point);
        }
        else
            kept.push_back(point);
    }
    // The moved points take their place among the others; where a moved
    // point lands on another, the moved one stays.
    std::vector<ScoreAutomationPoint> merged = moved;
    for (const auto& point : kept)
    {
        if (std::ranges::none_of(moved, [&](const ScoreAutomationPoint& other) {
                return timeValue(other.position) == timeValue(point.position);
            }))
            merged.push_back(point);
    }
    std::ranges::stable_sort(merged, {}, [](const ScoreAutomationPoint& point) { return timeValue(point.position); });
    lane.points = std::move(merged);
    return finished(std::move(lane));
}

std::optional<ScoreAutomationLane> deleteAutomationPoints(
    ScoreAutomationLane lane, const std::vector<std::size_t>& selected)
{
    if (!validSelection(lane, selected) || selected.size() == lane.points.size())
        return std::nullopt;
    for (auto index = selected.rbegin(); index != selected.rend(); ++index)
        lane.points.erase(lane.points.begin() + static_cast<std::ptrdiff_t>(*index));
    return finished(std::move(lane));
}

std::vector<AutomationStrokePoint> copyAutomationPoints(
    const ScoreAutomationLane& lane, const std::vector<std::size_t>& selected)
{
    std::vector<AutomationStrokePoint> copied;
    if (!validSelection(lane, selected))
        return copied;
    const auto first = timeValue(lane.points[selected.front()].position);
    for (const auto index : selected)
        copied.push_back({timeValue(lane.points[index].position) - first, lane.points[index].value});
    return copied;
}

std::optional<ScoreAutomationLane> pasteAutomationPoints(
    ScoreAutomationLane lane, const std::vector<AutomationStrokePoint>& copied, double at)
{
    if (copied.empty() || lane.points.empty())
        return std::nullopt;
    const auto domain = lane.points.front().position.domain;
    const auto end = at + copied.back().time;
    std::vector<ScoreAutomationPoint> points;
    for (const auto& point : lane.points)
    {
        const auto time = timeValue(point.position);
        if (time < at || time > end)
            points.push_back(point);
    }
    for (const auto& point : copied)
    {
        const auto time = makeTime(domain, at + point.time);
        if (!time.has_value())
            return std::nullopt;
        points.push_back({*time, point.value, Interpolation::Linear});
    }
    std::ranges::stable_sort(points, {}, [](const ScoreAutomationPoint& point) { return timeValue(point.position); });
    lane.points = std::move(points);
    return finished(std::move(lane));
}

std::optional<std::vector<ScoreAutomationLane>> liveControlsAsLanes(
    const talas::project::ProjectScore& score, std::string_view clipId, const std::vector<LiveControlValue>& values)
{
    const auto clip = std::ranges::find(score.clips, clipId, &talas::project::ScoreClip::clipId);
    if (clip == score.clips.end())
        return std::nullopt;
    std::vector<ScoreAutomationLane> lanes;
    for (const auto& control : values)
    {
        if (!std::isfinite(control.value))
            return std::nullopt;
        const auto existing = std::ranges::find_if(score.automation, [&](const ScoreAutomationLane& lane) {
            return lane.scope == talas::project::ScoreAutomationScope::Clip && lane.ownerId == clipId
                && lane.eventName == control.eventName;
        });
        ScoreAutomationLane lane;
        lane.laneId = existing != score.automation.end() ? existing->laneId : std::string{clipId} + "-" + control.eventName;
        lane.scope = talas::project::ScoreAutomationScope::Clip;
        lane.ownerId = std::string{clipId};
        lane.eventName = control.eventName;
        lane.domain = control.domain;
        const auto start = makeTime(clip->start.domain, 0.0);
        if (!start.has_value())
            return std::nullopt;
        lane.points = {{*start, control.value, Interpolation::Step}};
        lanes.push_back(std::move(lane));
    }
    return lanes;
}

std::vector<talas::project::ScoreAutomationLane> liveLanesArmedForCache(
    const talas::project::ProjectScore& score, std::string_view clipId)
{
    std::vector<talas::project::ScoreAutomationLane> armed;
    for (const auto& lane : score.automation)
    {
        if (lane.scope != talas::project::ScoreAutomationScope::Clip || lane.ownerId != clipId
            || lane.state != talas::project::ScoreAutomationState::Live)
            continue;
        auto edited = lane;
        edited.state = talas::project::ScoreAutomationState::Record;
        armed.push_back(std::move(edited));
    }
    return armed;
}

std::vector<talas::project::ScoreAutomationLane> lanesInState(
    const talas::project::ProjectScore& score,
    const std::optional<std::vector<std::string>>& clipIds,
    AutomationControlGroup group,
    talas::project::ScoreAutomationState state)
{
    const auto clipGiven = [&clipIds](std::string_view clipId) {
        return !clipIds.has_value() || std::ranges::find(*clipIds, clipId) != clipIds->end();
    };
    std::vector<std::string> tracks;
    for (const auto& clip : score.clips)
    {
        if (clipIds.has_value() && clipGiven(clip.clipId) && std::ranges::find(tracks, clip.trackId) == tracks.end())
            tracks.push_back(clip.trackId);
    }
    std::vector<talas::project::ScoreAutomationLane> changed;
    for (const auto& lane : score.automation)
    {
        const auto inGroup = group == AutomationControlGroup::ParameterControls
            ? lane.scope == talas::project::ScoreAutomationScope::Clip && clipGiven(lane.ownerId)
            : lane.scope == talas::project::ScoreAutomationScope::Track
                && (!clipIds.has_value() || std::ranges::find(tracks, lane.ownerId) != tracks.end())
                && (lane.eventName == "Gain" || lane.eventName == "Level" || lane.eventName == "Angle");
        if (!inGroup || lane.state == state)
            continue;
        auto edited = lane;
        edited.state = state;
        changed.push_back(std::move(edited));
    }
    return changed;
}

std::vector<std::string> automatedEventNames(const talas::project::ProjectScore& score)
{
    std::vector<std::string> names;
    for (const auto& lane : score.automation)
    {
        // A slaved lane is in its master's state.
        const auto& driving = *talas::project::scoreAutomationDrivingLane(score, lane);
        if (driving.state == talas::project::ScoreAutomationState::Automated && !driving.points.empty()
            && std::ranges::find(names, lane.eventName) == names.end())
            names.push_back(lane.eventName);
    }
    return names;
}

std::vector<const talas::project::ScoreAutomationLane*> recordEnabledLanes(const talas::project::ProjectScore& score)
{
    std::vector<const talas::project::ScoreAutomationLane*> lanes;
    for (const auto& lane : score.automation)
    {
        if (lane.state == talas::project::ScoreAutomationState::Record)
            lanes.push_back(&lane);
    }
    return lanes;
}

std::vector<std::string> automationTakeNames(const ScoreAutomationLane& lane)
{
    std::vector<std::string> names{lane.takeName};
    for (const auto& take : lane.takes)
        names.push_back(take.name);
    std::ranges::sort(names, [](const std::string& left, const std::string& right) {
        // take2 before take10.
        return left.size() != right.size() ? left.size() < right.size() : left < right;
    });
    return names;
}

std::string nextAutomationTakeName(const ScoreAutomationLane& lane)
{
    const auto names = automationTakeNames(lane);
    for (std::size_t number = 1;; ++number)
    {
        auto name = "take" + std::to_string(number);
        if (std::ranges::find(names, name) == names.end())
            return name;
    }
}

std::optional<ScoreAutomationLane> automationLaneWithNewTake(
    ScoreAutomationLane lane, std::optional<std::vector<ScoreAutomationPoint>> points)
{
    if (lane.takes.size() >= talas::project::maximumScoreAutomationTakes)
        return std::nullopt;
    const auto name = nextAutomationTakeName(lane);
    lane.takes.push_back({lane.takeName, lane.points});
    lane.takeName = name;
    if (points.has_value())
        lane.points = std::move(*points);
    return lane;
}

std::optional<ScoreAutomationLane> automationLaneWithTake(ScoreAutomationLane lane, std::string_view name)
{
    if (name == lane.takeName)
        return lane;
    const auto take = std::ranges::find(lane.takes, name, &talas::project::ScoreAutomationTake::name);
    if (take == lane.takes.end())
        return std::nullopt;
    std::swap(take->name, lane.takeName);
    std::swap(take->points, lane.points);
    return lane;
}

std::optional<ScoreAutomationLane> automationLaneWithTakeRenamed(
    ScoreAutomationLane lane, std::string_view from, std::string_view to)
{
    const auto names = automationTakeNames(lane);
    if (to.empty() || to.size() > 128 || (from != to && std::ranges::find(names, to) != names.end()))
        return std::nullopt;
    if (lane.takeName == from)
    {
        lane.takeName = std::string{to};
        return lane;
    }
    const auto take = std::ranges::find(lane.takes, from, &talas::project::ScoreAutomationTake::name);
    if (take == lane.takes.end())
        return std::nullopt;
    take->name = std::string{to};
    return lane;
}

std::optional<ScoreAutomationLane> automationLaneSlavedTo(
    const talas::project::ProjectScore& score, std::string_view laneId, std::string_view masterLaneId)
{
    const auto lane = std::ranges::find(score.automation, laneId, &ScoreAutomationLane::laneId);
    if (lane == score.automation.end())
        return std::nullopt;
    if (!masterLaneId.empty())
    {
        const auto candidates = talas::project::scoreAutomationMasterCandidates(score, laneId);
        if (std::ranges::find(candidates, masterLaneId) == candidates.end()
            || std::ranges::any_of(score.automation, [&](const ScoreAutomationLane& other) { return other.masterLaneId == laneId; }))
            return std::nullopt;
    }
    auto slaved = *lane;
    // Unslave leaves a local copy of the master's function to edit.
    if (masterLaneId.empty() && !lane->masterLaneId.empty())
        slaved.points = automationShownPoints(score, *lane);
    slaved.masterLaneId = std::string{masterLaneId};
    return slaved;
}

std::optional<ScoreAutomationLane> newMasterControl(
    const talas::project::ProjectScore& score, std::string_view name, std::string_view laneId)
{
    const auto master = std::ranges::find(score.tracks, talas::project::ScoreTrackKind::Master, &talas::project::ScoreTrack::kind);
    if (master == score.tracks.end() || name.empty() || name.size() > 64
        || !(std::isalpha(static_cast<unsigned char>(name.front())) != 0 || name.front() == '_')
        || !std::ranges::all_of(name, [](char character) {
               return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
           })
        || std::ranges::any_of(score.automation, [&](const ScoreAutomationLane& lane) {
               return lane.scope == talas::project::ScoreAutomationScope::Master && lane.eventName == name;
           }))
        return std::nullopt;
    ScoreAutomationLane lane;
    lane.laneId = std::string{laneId};
    lane.scope = talas::project::ScoreAutomationScope::Master;
    lane.ownerId = master->trackId;
    lane.eventName = std::string{name};
    lane.points = {{{master->timeDomain, 0.0, 0}, 0.0, talas::core::AutomationInterpolation::Linear}};
    return lane;
}

std::optional<std::pair<ScoreAutomationLane, ScoreAutomationLane>> automationLanePromotedToMaster(
    const talas::project::ProjectScore& score, std::string_view laneId, std::string_view masterLaneId)
{
    const auto lane = std::ranges::find(score.automation, laneId, &ScoreAutomationLane::laneId);
    if (lane == score.automation.end() || lane->scope == talas::project::ScoreAutomationScope::Master || !lane->masterLaneId.empty()
        || std::ranges::any_of(score.automation, [&](const ScoreAutomationLane& other) { return other.masterLaneId == laneId; }))
        return std::nullopt;
    auto name = lane->eventName + "Master";
    for (std::size_t suffix = 2; std::ranges::any_of(score.automation, [&](const ScoreAutomationLane& other) {
             return other.scope == talas::project::ScoreAutomationScope::Master && other.eventName == name;
         });
         ++suffix)
        name = lane->eventName + "Master" + std::to_string(suffix);
    auto master = newMasterControl(score, name, masterLaneId);
    if (!master.has_value())
        return std::nullopt;
    // The lane's function at its times in the Timeline.
    std::optional<double> start;
    if (lane->scope == talas::project::ScoreAutomationScope::Clip)
    {
        const auto clip = std::ranges::find(score.clips, lane->ownerId, &talas::project::ScoreClip::clipId);
        if (clip == score.clips.end())
            return std::nullopt;
        start = timeValue(clip->start);
    }
    master->domain = lane->domain;
    master->state = lane->state;
    if (!lane->points.empty())
        master->points.clear();
    for (auto point : lane->points)
    {
        const auto made = makeTime(point.position.domain, timeValue(point.position) + start.value_or(0.0));
        if (!made.has_value())
            return std::nullopt;
        point.position = *made;
        master->points.push_back(point);
    }
    auto slaved = *lane;
    slaved.masterLaneId = master->laneId;
    return std::pair{std::move(*master), std::move(slaved)};
}

std::vector<ScoreAutomationPoint> automationShownPoints(const talas::project::ProjectScore& score, const ScoreAutomationLane& lane)
{
    const auto& driving = *talas::project::scoreAutomationDrivingLane(score, lane);
    // The function through the lane's transformations, on
    // a slaved lane the master's through the master's and then its own.
    if (&driving == &lane)
        return talas::project::scoreAutomationEffectivePoints(lane.points, lane.transforms);
    const auto startOf = [&](const ScoreAutomationLane& owner) -> std::optional<talas::project::ScoreTime> {
        if (owner.scope != talas::project::ScoreAutomationScope::Clip)
            return std::nullopt;
        const auto clip = std::ranges::find(score.clips, owner.ownerId, &talas::project::ScoreClip::clipId);
        return clip == score.clips.end() ? std::nullopt : std::optional<talas::project::ScoreTime>{clip->start};
    };
    // The master's times in the Timeline, then in the slaved lane's own.
    const auto masterStart = startOf(driving);
    const auto ownStart = startOf(lane);
    std::vector<ScoreAutomationPoint> shown;
    for (auto point : talas::project::scoreAutomationEffectivePoints(driving.points, driving.transforms))
    {
        auto time = timeValue(point.position) + (masterStart ? timeValue(*masterStart) : 0.0) - (ownStart ? timeValue(*ownStart) : 0.0);
        if (time < 0.0)
            continue;
        const auto made = makeTime(point.position.domain, time);
        if (!made.has_value())
            continue;
        point.position = *made;
        shown.push_back(point);
    }
    return talas::project::scoreAutomationEffectivePoints(std::move(shown), lane.transforms);
}

std::vector<ScoreAutomationPoint> envelopeAutomationPoints(
    const std::vector<double>& values, double sampleRate, double intervalSeconds, double tolerance)
{
    std::vector<ScoreAutomationPoint> sampled;
    if (values.empty() || !(sampleRate > 0.0) || !(intervalSeconds > 0.0))
        return sampled;
    const auto step = std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(intervalSeconds * sampleRate)));
    for (std::size_t frame = 0; frame < values.size(); frame += step)
    {
        if (std::isfinite(values[frame]))
            sampled.push_back({{talas::project::ScoreTimeDomain::Seconds, static_cast<double>(frame) / sampleRate, 0}, values[frame],
                talas::core::AutomationInterpolation::Linear});
    }
    if (sampled.size() < 3)
        return sampled;
    const auto [lowest, highest] = std::ranges::minmax_element(sampled, {}, &ScoreAutomationPoint::value);
    const auto limit = std::max(0.0, tolerance) * (highest->value - lowest->value);
    // Ramer-Douglas-Peucker: keep the point farthest from the line between
    // the kept ends while it is farther than the limit.
    std::vector<bool> keep(sampled.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<std::size_t, std::size_t>> spans{{0, sampled.size() - 1}};
    while (!spans.empty())
    {
        const auto [first, last] = spans.back();
        spans.pop_back();
        auto farthest = first;
        auto distance = 0.0;
        const auto t0 = sampled[first].position.seconds;
        const auto t1 = sampled[last].position.seconds;
        for (auto index = first + 1; index < last; ++index)
        {
            const auto ratio = (sampled[index].position.seconds - t0) / (t1 - t0);
            const auto line = sampled[first].value + (sampled[last].value - sampled[first].value) * ratio;
            if (const auto off = std::abs(sampled[index].value - line); off > distance)
            {
                distance = off;
                farthest = index;
            }
        }
        if (farthest != first && distance > limit)
        {
            keep[farthest] = true;
            spans.emplace_back(first, farthest);
            spans.emplace_back(farthest, last);
        }
    }
    std::vector<ScoreAutomationPoint> points;
    for (std::size_t index = 0; index < sampled.size(); ++index)
    {
        if (keep[index])
            points.push_back(sampled[index]);
    }
    return points;
}

std::vector<std::string> automationLaneAffectedClips(const talas::project::ProjectScore& score, std::string_view laneId)
{
    std::vector<std::string> clips;
    const auto lane = std::ranges::find(score.automation, laneId, &talas::project::ScoreAutomationLane::laneId);
    if (lane == score.automation.end())
        return clips;
    for (const auto& clip : score.clips)
    {
        const auto affected = lane->scope == talas::project::ScoreAutomationScope::Clip ? clip.clipId == lane->ownerId
            : lane->scope == talas::project::ScoreAutomationScope::Track
            ? talas::project::scoreTrackRoutesThrough(score, clip.trackId, lane->ownerId)
            : true;
        if (affected)
            clips.push_back(clip.clipId);
    }
    return clips;
}
}
