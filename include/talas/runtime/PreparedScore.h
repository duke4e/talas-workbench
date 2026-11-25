#pragma once

#include "talas/core/AutomationInterpolation.h"
#include "talas/expr/Expression.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace talas::runtime
{
struct PreparedScoreResult;
class PreparedScoreSchedule;

inline constexpr std::size_t maximumPreparedScoreClipIterations = 65536;
inline constexpr std::size_t maximumPreparedScoreEvents =
    talas::project::maximumScoreClips * 4U
    + maximumPreparedScoreClipIterations
    + talas::project::maximumScoreAutomationPoints;

struct PreparedScoreBinding
{
    std::uint64_t documentGeneration = 0;
    std::uint64_t stageGeneration = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t tempoGeneration = 0;
    std::uint64_t routingGeneration = 0;
    std::uint64_t assetGeneration = 0;
    std::uint64_t parameterGeneration = 0;
    std::uint64_t sourceGeneration = 0;

    bool operator==(const PreparedScoreBinding&) const = default;
};

struct PreparedScoreTrack
{
    std::string trackId;
    std::uint32_t outputTrackIndex = 0;
    std::uint32_t channelCount = 0;
    talas::project::ScoreTrackKind kind = talas::project::ScoreTrackKind::Track;
    talas::project::ScoreOverlapMode overlapMode =
        talas::project::ScoreOverlapMode::Reject;
    bool audible = false;
    bool sharingMemoryWriters = false;
    // The submix this track's Sounds take as
    // their audio input, the Sound tracks reaching it, and how many input
    // tracks deep this one is (0 for none), so the tracks feeding it are
    // rendered before it.
    std::optional<std::uint32_t> inputTrackIndex;
    std::vector<std::uint32_t> inputFeederTrackIndices;
    std::uint32_t inputDepth = 0;
    // For each feeding track, the submix its audio enters on the way,
    // whose To_ lane on that track is its send level.
    std::vector<std::uint32_t> inputFeederEntryTrackIndices;
    // The Master channels, from 0, the track is hard-wired to;
    // empty for its pan controls.
    std::vector<std::uint32_t> outputChannels;
    // The default MIDI channel of the track's Sounds, 0 for the
    // project's.
    std::uint8_t midiChannel = 0;
    // The hardware audio input, from 1, the track's Sounds take
    // as their input; 0 for none.
    std::uint32_t inputChannel = 0;
    // Heard only through its sends.
    bool noOutput = false;

    bool operator==(const PreparedScoreTrack&) const = default;
};

struct PreparedScoreClip
{
    std::string clipId;
    std::string soundDefinitionId;
    std::optional<std::string> sourceInstanceId;
    std::uint32_t trackIndex = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t durationFrames = 0;
    std::uint64_t endFrame = 0;
    std::uint64_t contentOffsetFrames = 0;
    std::uint64_t fadeInFrames = 0;
    std::uint64_t fadeOutFrames = 0;
    std::uint32_t repeatCount = 0;
    std::uint32_t firstIteration = 0;
    bool audible = false;

    bool operator==(const PreparedScoreClip&) const = default;
};

struct PreparedScoreClipIteration
{
    std::uint32_t clipIndex = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;

    bool operator==(const PreparedScoreClipIteration&) const = default;
};

// Immutable full-clip audio prepared off the audio callback. The channel
// vectors contain the complete repeated clip range with content offset,
// source-target automation, and fades already applied exactly once.
struct PreparedScoreClipAudio
{
    PreparedScoreBinding binding;
    std::string clipId;
    std::string sourceFingerprint;
    std::uint32_t sampleRate = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t frameCount = 0;
    std::vector<std::shared_ptr<const std::vector<double>>> channels;

    [[nodiscard]] bool valid() const noexcept;
};

using PreparedScoreClipAudioPtr =
    std::shared_ptr<const PreparedScoreClipAudio>;

enum class PreparedScoreEventKind : std::uint8_t
{
    ClipDeactivate,
    ClipRepeat,
    Automation,
    ClipActivate,
};

struct PreparedScoreEvent
{
    std::uint64_t frame = 0;
    PreparedScoreEventKind kind = PreparedScoreEventKind::ClipDeactivate;
    std::uint32_t entityIndex = 0;
    bool active = false;

    bool operator==(const PreparedScoreEvent&) const = default;
};

struct PreparedScoreAutomationLane
{
    std::string laneId;
    std::string eventName;
    talas::project::ScoreAutomationScope scope =
        talas::project::ScoreAutomationScope::Track;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::uint32_t ownerIndex = 0;
    std::uint32_t firstPoint = 0;
    std::uint32_t pointCount = 0;

    bool operator==(const PreparedScoreAutomationLane&) const = default;
};

struct PreparedScoreAutomationPoint
{
    std::uint64_t frame = 0;
    double value = 0.0;
    talas::core::AutomationInterpolation interpolation =
        talas::core::AutomationInterpolation::Step;
    std::uint32_t laneIndex = 0;

    bool operator==(const PreparedScoreAutomationPoint&) const = default;
};

class PreparedScorePlaybackCursor
{
public:
    explicit PreparedScorePlaybackCursor(
        std::shared_ptr<const PreparedScoreSchedule> schedule);

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool relocate(std::uint64_t frame) noexcept;
    [[nodiscard]] bool advanceTo(std::uint64_t frame) noexcept;
    [[nodiscard]] bool clipActive(std::size_t clipIndex) const noexcept;
    [[nodiscard]] std::optional<double> automationValue(
        std::size_t laneIndex,
        std::uint64_t frame) const noexcept;
    [[nodiscard]] std::uint64_t frame() const noexcept;
    [[nodiscard]] std::size_t eventCursor() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> nextEventFrame() const noexcept;

private:
    void apply(const PreparedScoreEvent& event) noexcept;

    std::shared_ptr<const PreparedScoreSchedule> schedule_;
    std::vector<std::uint8_t> activeClips_;
    std::size_t eventCursor_ = 0;
    std::uint64_t frame_ = 0;
    bool ready_ = false;
};

// Frames where a clip's audio reaches its track, derived from the
// ClipActivate/ClipDeactivate schedule (Replace tracks select one clip).
struct PreparedScoreClipWindow
{
    std::uint32_t clipIndex = 0;
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;

    bool operator==(const PreparedScoreClipWindow&) const = default;
};

struct PreparedScoreMarker
{
    std::string markerId;
    std::string title;
    std::uint64_t startFrame = 0;
    std::optional<std::uint64_t> endFrame;

    bool operator==(const PreparedScoreMarker&) const = default;
};

class PreparedScoreSchedule
{
public:
    [[nodiscard]] const PreparedScoreBinding& binding() const noexcept;
    [[nodiscard]] std::span<const PreparedScoreTrack> tracks() const noexcept;
    [[nodiscard]] std::span<const PreparedScoreClip> clips() const noexcept;
    [[nodiscard]] std::span<const PreparedScoreEvent> events() const noexcept;
    [[nodiscard]] std::span<const PreparedScoreClipIteration>
    clipIterations() const noexcept;
    [[nodiscard]] std::span<const PreparedScoreAutomationLane>
    automationLanes() const noexcept;
    [[nodiscard]] std::span<const PreparedScoreAutomationPoint>
    automationPoints() const noexcept;
    [[nodiscard]] std::span<const PreparedScoreMarker> markers() const noexcept;
    [[nodiscard]] std::span<const PreparedScoreClipWindow> clipWindows() const noexcept;
    [[nodiscard]] bool clipAudibleAt(
        std::size_t clipIndex, std::uint64_t frame) const noexcept;
    [[nodiscard]] std::size_t firstEventAtOrAfter(
        std::uint64_t frame) const noexcept;
    // Format 156: every Sound track shares one pool of MemoryWriter
    // recordings, and overlapping clips that write or read one render
    // together, writers first.
    [[nodiscard]] bool sharingMemoryWritersAcrossTracks() const noexcept;

private:
    PreparedScoreBinding binding_;
    bool sharingMemoryWritersAcrossTracks_ = false;
    std::vector<PreparedScoreTrack> tracks_;
    std::vector<PreparedScoreClip> clips_;
    std::vector<PreparedScoreEvent> events_;
    std::vector<PreparedScoreClipIteration> clipIterations_;
    std::vector<PreparedScoreAutomationLane> automationLanes_;
    std::vector<PreparedScoreAutomationPoint> automationPoints_;
    std::vector<PreparedScoreMarker> markers_;
    std::vector<PreparedScoreClipWindow> clipWindows_;

    friend struct PreparedScoreResult;
    friend PreparedScoreResult prepareProjectScore(
        const talas::project::ProjectDocument&,
        PreparedScoreBinding);
};

struct PreparedScoreResult
{
    std::shared_ptr<const PreparedScoreSchedule> schedule;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return schedule != nullptr && error.empty();
    }
};

[[nodiscard]] PreparedScoreResult prepareProjectScore(
    const talas::project::ProjectDocument& document,
    PreparedScoreBinding binding);

[[nodiscard]] PreparedScoreBinding preparedScoreBindingForDocument(
    const talas::project::ProjectDocument& document,
    std::uint64_t documentGeneration,
    std::uint64_t stageGeneration,
    std::uint32_t sampleRate,
    std::uint64_t sourceGeneration) noexcept;

// The transport's jumps from the time cursor, to the
// marker before or after it, and to the "important time" before or after
// it, the start or end of a Sound's time bar; nothing when there is none.
// Twice back is the jump from the first jump's frame.
[[nodiscard]] std::optional<std::uint64_t> previousScoreMarkerFrame(
    const PreparedScoreSchedule& schedule, std::uint64_t cursor) noexcept;
[[nodiscard]] std::optional<std::uint64_t> nextScoreMarkerFrame(
    const PreparedScoreSchedule& schedule, std::uint64_t cursor) noexcept;
[[nodiscard]] std::optional<std::uint64_t> previousScoreImportantFrame(
    const PreparedScoreSchedule& schedule, std::uint64_t cursor) noexcept;
[[nodiscard]] std::optional<std::uint64_t> nextScoreImportantFrame(
    const PreparedScoreSchedule& schedule, std::uint64_t cursor) noexcept;

// The apparent source of a track's sound in
// its Panning status, a point in the room around the listener at the
// centre, from its Angle and Radius at a frame: x from -1 (hard left) to 1
// (hard right), y from -1 (behind) to 1 (ahead), Radius 1 on the circle.
// A track without Angle or Radius lanes sounds from straight ahead.
struct ScoreTrackSourcePosition
{
    double x = 0.0;
    double y = 1.0;
};

[[nodiscard]] ScoreTrackSourcePosition scoreTrackSourcePosition(
    const PreparedScoreSchedule& schedule, std::uint32_t trackIndex, std::uint64_t frame) noexcept;
}
