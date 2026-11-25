#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace talas::app
{
inline constexpr std::size_t maximumTimelineViewportRows = 96;
inline constexpr std::size_t maximumTimelineViewportItems = 8192;
inline constexpr std::size_t maximumTimelineRulerTicks = 256;
inline constexpr double maximumTimelineViewportExtent = 1'000'000.0;
inline constexpr std::size_t maximumTimelineScoreMeters =
    talas::project::maximumScoreTracks;

enum class TimelineRulerMode
{
    Seconds,
    BarsBeatsTicks,
    Smpte,
    // Minutes and seconds, grouped as seconds are.
    MinutesSeconds,
};

enum class TimelineScoreSnapKind
{
    SecondsGrid,
    MusicalGrid,
};

struct TimelineScorePointerTime
{
    talas::project::ScoreTime exact;
    std::optional<talas::project::ScoreTime> snapCandidate;
    std::uint64_t exactFrame = 0;
    std::optional<std::uint64_t> snapFrame;
    TimelineScoreSnapKind snapKind = TimelineScoreSnapKind::SecondsGrid;
};

[[nodiscard]] std::optional<TimelineScorePointerTime>
timelineScorePointerTime(
    std::uint64_t frame,
    talas::project::ScoreTimeDomain domain,
    const talas::project::ProjectMusicalTime& musicalTime,
    std::uint32_t sampleRate,
    double secondsGrid = 0.25,
    std::uint64_t musicalTickGrid = 120) noexcept;

enum class TimelineScoreRowKind
{
    Track,
    Submix,
    Master,
    Automation,
};

enum class TimelineScoreItemKind
{
    Clip,
    AutomationSegment,
    AutomationPoint,
    Marker,
};

enum class TimelineScoreSelectionKind
{
    None,
    Track,
    Clip,
    AutomationLane,
    AutomationPoint,
    Marker,
};

enum class TimelineScoreCommand : std::uint32_t
{
    AddTrack = 1U << 0U,
    AddClip = 1U << 1U,
    AddAutomation = 1U << 2U,
    AddMarker = 1U << 3U,
    DeleteSelection = 1U << 4U,
    DuplicateSelection = 1U << 5U,
    ToggleMute = 1U << 6U,
    ToggleSolo = 1U << 7U,
    ChangeOutput = 1U << 8U,
};

using TimelineScoreCommandMask = std::uint32_t;

[[nodiscard]] constexpr TimelineScoreCommandMask timelineScoreCommandMask(
    TimelineScoreCommand command) noexcept
{
    return static_cast<TimelineScoreCommandMask>(command);
}

[[nodiscard]] constexpr bool timelineScoreHasCommand(
    TimelineScoreCommandMask mask,
    TimelineScoreCommand command) noexcept
{
    return (mask & timelineScoreCommandMask(command)) != 0U;
}

enum class TimelineExternalSyncKind
{
    Internal,
    MidiClock,
    MidiTimeCode,
};

enum class TimelineSmpteRate
{
    Fps24,
    Fps25,
    Fps2997Drop,
    Fps30,
};

struct TimelineScoreMeterValue
{
    std::string trackId;
    double peak = 0.0;
};

// This DTO deliberately contains no JUCE or runtime object. Workbench maps one
// coherent runtime telemetry snapshot into it before rebuilding the projection.
struct TimelineScoreRuntimeState
{
    std::uint64_t stageId = 0;
    std::uint64_t playheadFrame = 0;
    std::uint32_t sampleRate = 48000;
    bool playing = false;
    bool publicationValid = false;
    TimelineExternalSyncKind syncKind = TimelineExternalSyncKind::Internal;
    bool externalLocked = false;
    TimelineSmpteRate smpteRate = TimelineSmpteRate::Fps24;
    std::array<TimelineScoreMeterValue, maximumTimelineScoreMeters> meters{};
    std::size_t meterCount = 0;
    std::string externalAccessibilityText;
    // Each lane's vertical zoom, in steps out
    // (`talas::app::automationValueRange`); a lane not listed shows its own
    // range.
    std::vector<std::pair<std::string, int>> laneValueZooms;
};

struct TimelineScoreSelection
{
    TimelineScoreSelectionKind kind = TimelineScoreSelectionKind::None;
    std::string entityId;
    std::size_t pointIndex = 0;

    bool operator==(const TimelineScoreSelection&) const = default;
};

// The Timeline's score: across the top, the
// strip reserved for markers; below it the time ruler, the cursor's time
// displayed at its left, where the time cursor is placed; then the tracks'
// rows, each track's information at the left of its row. The top strips
// are `stripHeight` each and the track information `headerWidth` wide.
struct TimelineScoreArea
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] int right() const noexcept { return x + width; }
    [[nodiscard]] int bottom() const noexcept { return y + height; }
};
struct TimelineScoreLayout
{
    TimelineScoreArea markers;
    TimelineScoreArea timeDisplay;
    TimelineScoreArea ruler;
    TimelineScoreArea trackInformation;
    TimelineScoreArea rows;
};
[[nodiscard]] TimelineScoreLayout timelineScoreLayout(int width, int height, int headerWidth, int stripHeight) noexcept;
// Its scroll bars and zoom buttons: the vertical
// scroll bar down the right edge, the horizontal one along the foot, and
// the three zoom buttons (out, in, to fit) in the lower right corner below
// the vertical one, each `barWidth` square, for a score `width` by
// `height`.
struct TimelineScoreScrolling
{
    TimelineScoreArea vertical;
    TimelineScoreArea horizontal;
    TimelineScoreArea zoomButtons;
};
[[nodiscard]] TimelineScoreScrolling timelineScoreScrolling(int width, int height, int barWidth) noexcept;

// A drag in the score: the frame under the pointer
// `x` across the rows' `width`, held within the view's frames, so that a
// bar's edge goes no further by hand than the view's edge (zoom out, or
// type the time, for more); and a bar or point moved from `base` as the
// pointer moved from `origin` to `current`, no earlier than time 0.
[[nodiscard]] std::optional<std::uint64_t> timelineScoreFrameAtPointer(
    double x, double width, std::uint64_t viewStart, std::uint64_t viewEnd) noexcept;
[[nodiscard]] std::uint64_t timelineScoreDraggedFrame(
    std::uint64_t base, std::uint64_t origin, std::uint64_t current) noexcept;

struct TimelineScoreViewport
{
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
    std::size_t firstRow = 0;
    std::size_t rowCount = 0;
    double pixelWidth = 0.0;
    double rowHeight = 0.0;
    TimelineRulerMode rulerMode = TimelineRulerMode::Seconds;
};

struct TimelineScoreRulerTick
{
    std::uint64_t frame = 0;
    double x = 0.0;
    bool major = false;
    std::string label;
};

struct TimelineScoreRowProjection
{
    TimelineScoreRowKind kind = TimelineScoreRowKind::Track;
    std::string stableId;
    std::string ownerId;
    std::string displayName;
    std::string outputTrackId;
    std::string accessibilityText;
    std::uint32_t channelCount = 0;
    double meterPeak = 0.0;
    bool muted = false;
    bool solo = false;
    bool audible = false;
    bool selected = false;
    bool meterAvailable = false;
    // An automation row's state: a Live lane shows a green dot, a
    // Record-enabled one a red dot.
    talas::project::ScoreAutomationState automationState = talas::project::ScoreAutomationState::Automated;
    // A track's audio input, the submix it takes.
    std::string inputTrackId;
    // An automation row slaved to another lane, drawn pale.
    bool slaved = false;
};

struct TimelineScoreItemProjection
{
    TimelineScoreItemKind kind = TimelineScoreItemKind::Clip;
    std::string stableId;
    std::string ownerId;
    std::size_t sourceIndex = 0;
    std::size_t row = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
    double x = 0.0;
    double width = 0.0;
    double y = 0.0;
    double height = 0.0;
    double value = 0.0;
    double fadeInFraction = 0.0;
    double fadeOutFraction = 0.0;
    std::uint32_t repeatCount = 1;
    bool selected = false;
    bool overlaps = false;
    std::string label;
    std::string diagnostic;
    std::string accessibilityText;
    double endValue = 0.0;
    double displayValue = 0.0;
    double displayEndValue = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    talas::core::AutomationInterpolation interpolation =
        talas::core::AutomationInterpolation::Step;
    std::string focusIdentity;
};

enum class TimelineScoreSurfaceState
{
    Loading,
    Empty,
    Content,
    Error,
};

struct TimelineScoreViewportProjection
{
    TimelineScoreSurfaceState state = TimelineScoreSurfaceState::Loading;
    std::vector<TimelineScoreRulerTick> rulerTicks;
    std::vector<TimelineScoreRowProjection> rows;
    std::vector<TimelineScoreItemProjection> items;
    TimelineScoreCommandMask commands = 0;
    std::optional<double> playheadX;
    std::size_t candidateCount = 0;
    bool truncated = false;
    std::string diagnostic;
    std::string externalSyncText;
    std::string accessibilityText;
};

struct TimelineScoreHit
{
    TimelineScoreSelection selection;
    std::string focusIdentity;
};

class TimelineScoreProjection
{
public:
    TimelineScoreProjection(
        const talas::project::ProjectDocument& document,
        TimelineScoreRuntimeState runtime);
    ~TimelineScoreProjection();

    TimelineScoreProjection(const TimelineScoreProjection&) = delete;
    TimelineScoreProjection& operator=(const TimelineScoreProjection&) = delete;
    TimelineScoreProjection(TimelineScoreProjection&&) noexcept;
    TimelineScoreProjection& operator=(TimelineScoreProjection&&) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string_view diagnostic() const noexcept;
    [[nodiscard]] std::size_t rowCount() const noexcept;
    [[nodiscard]] std::uint64_t contentEndFrame() const noexcept;
    [[nodiscard]] const TimelineScoreRuntimeState& runtime() const noexcept;
    [[nodiscard]] bool containsSelection(
        const TimelineScoreSelection& selection) const noexcept;
    void updateRuntimeState(TimelineScoreRuntimeState runtime);

    [[nodiscard]] TimelineScoreViewportProjection query(
        const TimelineScoreViewport& viewport,
        const TimelineScoreSelection& selection = {}) const;

    [[nodiscard]] std::optional<TimelineScoreHit> hitTest(
        const TimelineScoreViewportProjection& viewport,
        double x,
        double y) const;

private:
    struct IndexedItem;
    struct IndexedRow;

    talas::project::ProjectMusicalTime musicalTime_;
    TimelineScoreRuntimeState runtime_;
    // The time code the Timeline's start reads.
    double timeCodeOffsetSeconds_ = 0.0;
    std::vector<IndexedRow> rows_;
    std::vector<IndexedItem> markers_;
    std::vector<std::uint64_t> markerPrefixMaximumEnd_;
    std::unordered_map<std::string, TimelineScoreSelectionKind> identities_;
    std::unordered_map<std::string, bool> routeableTracks_;
    std::unordered_map<std::string, bool> clipEligibleTracks_;
    std::unordered_map<std::string, bool> gainAutomationEligibleTracks_;
    std::unordered_map<std::string, bool> deletableTracks_;
    std::unordered_map<std::string, bool> deletableClips_;
    std::unordered_map<std::string, std::size_t> automationPointCounts_;
    std::size_t trackCount_ = 0;
    bool canAddGainAutomation_ = false;
    std::size_t clipCount_ = 0;
    std::size_t automationLaneCount_ = 0;
    std::size_t automationPointCount_ = 0;
    std::size_t markerCount_ = 0;
    std::uint64_t contentEndFrame_ = 0;
    std::string diagnostic_;
};
}
