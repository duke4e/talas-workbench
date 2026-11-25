#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
// Time scale asks for a factor, offering 0.5, which
// compresses the control function to half its duration.
inline constexpr double automationTimeScaleOffered = 0.5;

// Editing a Timeline automation lane. Every
// function returns the edited lane for ProjectEditor's
// updateScoreAutomationLane, or nothing when the edit does not apply; the
// points stay in the owner's time domain and strictly increasing.

// What a track's (or the master's, or a clip's) lane can automate: Gain,
// a track's Angle and Radius and its Track_To_Submix level
// for each submix it is routed to, and each Event Value the
// project's controls give, less those the owner already automates (pp.
// 71-72). A send level is labelled with the track's name, To and the
// submix's.
struct AutomationTarget
{
    std::string eventName;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::string label;

    bool operator==(const AutomationTarget&) const = default;
};

[[nodiscard]] std::vector<AutomationTarget> automationTargets(
    const talas::project::ProjectDocument& document,
    talas::project::ScoreAutomationScope scope,
    std::string_view ownerId);

// A track's Notes control is unlike the others: it
// stands for the discrete note events the track's Sounds hear, always live
// from a MIDI keyboard or sequencer, not a continuous controller, so it is
// listed but never recorded or edited as a lane. The live controls of an
// owner of the scope given.
[[nodiscard]] std::vector<std::string_view> automationLiveControls(talas::project::ScoreAutomationScope scope) noexcept;

// The pencil: the points of a stroke, in the
// order drawn, replace the lane's points within the stroke's span, each
// linear to the next; points nearer than `minimumSpacing` (seconds or
// ticks, as the lane's times are) to the last one kept are left out, the
// stroke's last point always kept.
struct AutomationStrokePoint
{
    double time = 0.0;
    double value = 0.0;
};

[[nodiscard]] std::optional<talas::project::ScoreAutomationLane>
drawAutomationStroke(
    talas::project::ScoreAutomationLane lane,
    const std::vector<AutomationStrokePoint>& stroke,
    double minimumSpacing);

// A curve from a point to the next, drawn as linear steps: Exponential
// rises slowly then fast, Logarithmic fast then slowly, S-Curve eases in
// and out.
enum class AutomationCurveShape
{
    Exponential,
    Logarithmic,
    SCurve,
};

inline constexpr std::size_t automationCurveSteps = 16;

[[nodiscard]] std::optional<talas::project::ScoreAutomationLane>
shapeAutomationSegment(
    talas::project::ScoreAutomationLane lane,
    std::size_t pointIndex,
    AutomationCurveShape shape,
    std::size_t steps = automationCurveSteps);

// The controller transformations, applied to the lane's
// points themselves.
using AutomationTransform = talas::project::ScoreAutomationTransformKind;

struct AutomationTransformParameters
{
    double value = 0.0;
    double secondary = 0.0;
    std::uint64_t seed = 1;
};

[[nodiscard]] std::optional<talas::project::ScoreAutomationLane>
transformAutomationLane(
    talas::project::ScoreAutomationLane lane,
    AutomationTransform transform,
    const AutomationTransformParameters& parameters = {});
// A transformation button pressed on a
// lane, its function passed through it without changing its points, or
// pressed again, taken away; whether it is pressed; and a copy of
// the function it plays, transformed or slaved, as its own points, the
// transformations and the master let go.
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> automationLaneWithTransformToggled(
    talas::project::ScoreAutomationLane lane,
    AutomationTransform transform,
    const AutomationTransformParameters& parameters = {});
[[nodiscard]] bool automationLaneTransformed(const talas::project::ScoreAutomationLane& lane, AutomationTransform transform);
// The transformation buttons arrayed across the bottom of the controls
// editor, left to right: Invert, Loop (second from the left), Reverse, the
// spray can (fourth), Square off (next to it), Scale, Offset, Threshold, Time
// scale (third from the right), Scale to fit and Quantize endpoints. Each has
// the fly-by help the pointer over it shows without a click: what pressing it
// does to the selected control's function and what it asks for; the help for a
// lane says too whether it is pressed there, a press then taking it away.
struct AutomationTransformButton
{
    AutomationTransform transform = AutomationTransform::Invert;
    std::string_view label;
    std::string_view help;
};
[[nodiscard]] std::span<const AutomationTransformButton> automationTransformButtons() noexcept;
[[nodiscard]] std::string automationTransformButtonHelp(
    const talas::project::ScoreAutomationLane* lane, AutomationTransform transform);
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> automationLaneWithFunctionKept(
    const talas::project::ProjectScore& score, std::string_view laneId);
// Several selected points at once,
// `selected` the indices of the lane's points in increasing order: all set
// to one value; the first moved to a time, the rest keeping their offsets
// from it; deleted; or copied, as times from the first, and pasted at a
// time, replacing the points within their span.
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> setAutomationPointValues(
    talas::project::ScoreAutomationLane lane, const std::vector<std::size_t>& selected, double value);
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> moveAutomationPoints(
    talas::project::ScoreAutomationLane lane, const std::vector<std::size_t>& selected, double firstTime);
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> deleteAutomationPoints(
    talas::project::ScoreAutomationLane lane, const std::vector<std::size_t>& selected);
[[nodiscard]] std::vector<AutomationStrokePoint> copyAutomationPoints(
    const talas::project::ScoreAutomationLane& lane, const std::vector<std::size_t>& selected);
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> pasteAutomationPoints(
    talas::project::ScoreAutomationLane lane, const std::vector<AutomationStrokePoint>& copied, double at);

// Editing points in the parameter control editor. The value at a height in the
// lane, 0 at the bottom to 1 at the top, as the lane shows its points from its
// lowest value to its highest (a flat lane at its value); a point added there
// with Ctrl+Click; the time of a point as a number in its domain, seconds or
// ticks; and the indices of the points at the times given, to keep a moved
// selection selected.
[[nodiscard]] double automationValueAtHeight(const talas::project::ScoreAutomationLane& lane, double height);
// The vertical zoom of a lane's editor:
// the range of values its height shows, at first its points' lowest to
// highest (a flat lane its value less and more a half); each step out
// doubles the range about its middle, so that a function can be drawn past
// its highest value, each step in halves it. A value's height in the range
// and the value at a height, 0 at the bottom to 1 at the top, unclipped.
struct AutomationValueRange
{
    double low = 0.0;
    double high = 1.0;

    bool operator==(const AutomationValueRange&) const = default;
};
[[nodiscard]] AutomationValueRange automationValueRange(
    std::span<const talas::project::ScoreAutomationPoint> points, int zoomOutSteps);
[[nodiscard]] double automationHeightOfValue(const AutomationValueRange& range, double value) noexcept;
[[nodiscard]] double automationValueAtHeight(const AutomationValueRange& range, double height) noexcept;
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> addAutomationPoint(
    talas::project::ScoreAutomationLane lane, const talas::project::ScoreTime& position, double value);
[[nodiscard]] double automationPointTime(const talas::project::ScoreTime& position) noexcept;
// Dragging a point moves it in time and value, its
// value by the height moved over the lane, as a fraction of the lane's
// height, times the span of the lane's values (1 for a flat lane); with
// Shift held as the drag starts, only along the direction the drag first
// takes, horizontally its time alone, vertically its value alone.
enum class AutomationDragAxis
{
    Both,
    Time,
    Value,
};
[[nodiscard]] AutomationDragAxis automationDragAxis(bool shift, double horizontal, double vertical) noexcept;
[[nodiscard]] double automationValueDragged(
    const talas::project::ScoreAutomationLane& lane, double startValue, double heightMoved);
[[nodiscard]] std::vector<std::size_t> automationPointsAt(
    const talas::project::ScoreAutomationLane& lane, const std::vector<double>& times);

// A lane's takes, its Source of Control menu:
// the takes' names in order; New Take, a copy of the function driving the
// parameter now, named with the next free takeN and driving it, the take it
// was kept; another take chosen to drive the parameter; a take renamed; and
// a recording's function as a new take, the one it replaces kept.
[[nodiscard]] std::vector<std::string> automationTakeNames(const talas::project::ScoreAutomationLane& lane);
[[nodiscard]] std::string nextAutomationTakeName(const talas::project::ScoreAutomationLane& lane);
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> automationLaneWithNewTake(
    talas::project::ScoreAutomationLane lane,
    std::optional<std::vector<talas::project::ScoreAutomationPoint>> points = std::nullopt);
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> automationLaneWithTake(
    talas::project::ScoreAutomationLane lane, std::string_view name);
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> automationLaneWithTakeRenamed(
    talas::project::ScoreAutomationLane lane, std::string_view from, std::string_view to);

// Slaving: the State menu's Slave to, a lane
// slaved to one of its candidates, or freed with an empty ID keeping a
// local copy of its master's function (Unslave); New Master, a
// master control of the name given on the Master track, holding 0; and
// the points a slaved lane shows, its master's, in its own time.
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> automationLaneSlavedTo(
    const talas::project::ProjectScore& score, std::string_view laneId, std::string_view masterLaneId);
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> newMasterControl(
    const talas::project::ProjectScore& score, std::string_view name, std::string_view laneId);
[[nodiscard]] std::vector<talas::project::ScoreAutomationPoint> automationShownPoints(
    const talas::project::ProjectScore& score, const talas::project::ScoreAutomationLane& lane);

// The Timeline's lower half, its controls editor: the score above and the
// editor below the line between them, which is dragged up or down; each keeps
// at least `minimum` of the height where there is room. The line at a height
// gives the editor's share. The editor lists the Timeline's controls on the
// left and draws the selected one's function to their right: its value at
// `columns` times evenly from `from` to `to`, in the lane's own time (seconds
// or ticks), and the height each takes, 0 at the foot to 1 at the top of the
// lane's values.
struct TimelineControlsSplit
{
    int scoreHeight = 0;
    int editorHeight = 0;
};
[[nodiscard]] TimelineControlsSplit timelineControlsSplit(int height, double editorShare, int minimum) noexcept;
[[nodiscard]] double timelineControlsShare(int lineY, int height) noexcept;
struct TimelineControlsCurvePoint
{
    double time = 0.0;
    double value = 0.0;
    double height = 0.0;
};
[[nodiscard]] std::vector<TimelineControlsCurvePoint> timelineControlsCurve(const talas::project::ProjectScore& score,
    const talas::project::ScoreAutomationLane& lane, double from, double to, std::size_t columns);
// A Ctrl+Click in the editor's graph adds a point to the selected control at
// the time and value clicked, `across` and `up` from 0 to 1 over the graph
// showing `from` to `to`; its time in the lane's own domain, that of its
// points (seconds for a lane with none).
[[nodiscard]] std::optional<talas::project::ScoreAutomationLane> timelineControlsPointAdded(
    talas::project::ScoreAutomationLane lane, double from, double to, double across, double up);
// Promote to master, a new master control named after the lane
// and Master (Amp gives AmpMaster), playing the lane's function at its
// times in the Timeline and in its state, and the lane slaved to it: the
// master first, then the slaved lane.
[[nodiscard]] std::optional<std::pair<talas::project::ScoreAutomationLane, talas::project::ScoreAutomationLane>>
automationLanePromotedToMaster(const talas::project::ProjectScore& score, std::string_view laneId, std::string_view masterLaneId);

// Audio signals as controls: an envelope, a
// value a frame from time zero, as a controller's points in seconds: one
// every interval, linear to the next, those a line between their
// neighbours already passes within the tolerance (a fraction of the
// envelope's range) left out, the first and last kept.
[[nodiscard]] std::vector<talas::project::ScoreAutomationPoint> envelopeAutomationPoints(
    const std::vector<double>& values, double sampleRate, double intervalSeconds, double tolerance);

// Set selected Sounds' live controls to current values: each control a clip's
// Sound reads becomes a lane of the clip holding, from its start, the value
// the VCS shows now; a lane the clip had for it is replaced.
struct LiveControlValue
{
    std::string eventName;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double value = 0.0;
};

[[nodiscard]] std::optional<std::vector<talas::project::ScoreAutomationLane>> liveControlsAsLanes(
    const talas::project::ProjectScore& score, std::string_view clipId, const std::vector<LiveControlValue>& values);

// The Automation menu: a group of
// controls set Live, Automated or Record-enabled at once, the lanes of the
// clips' parameters or of their tracks' level and angle (Gain and Angle),
// for the clips given or, with none, every clip and track; the lanes whose
// state changes, for ProjectEditor's score edits.
enum class AutomationControlGroup : std::uint8_t
{
    ParameterControls,
    TrackLevelAndAngle,
};

[[nodiscard]] std::vector<talas::project::ScoreAutomationLane> lanesInState(
    const talas::project::ProjectScore& score,
    const std::optional<std::vector<std::string>>& clipIds,
    AutomationControlGroup group,
    talas::project::ScoreAutomationState state);

// Marking a Sound for caching arms its live
// parameters for recording, so that the control functions drawn match what
// the cache holds: the clip's Live parameter lanes set Record-enabled.
[[nodiscard]] std::vector<talas::project::ScoreAutomationLane> liveLanesArmedForCache(
    const talas::project::ProjectScore& score, std::string_view clipId);

// Only the Timeline's live controls show in the virtual control surface
//: the Event Values an Automated lane drives are left out of it.
[[nodiscard]] std::vector<std::string> automatedEventNames(const talas::project::ProjectScore& score);

// What a recording takes: the lanes Record-enabled.
[[nodiscard]] std::vector<const talas::project::ScoreAutomationLane*> recordEnabledLanes(
    const talas::project::ProjectScore& score);

// The Sounds a control can affect, whose time
// spans its editor shows on a darker background: a Sound's own control
// its Sound, a track control the Sounds whose output passes through the
// track, a master control every Sound.
[[nodiscard]] std::vector<std::string> automationLaneAffectedClips(
    const talas::project::ProjectScore& score, std::string_view laneId);
}
