#pragma once

#include "talas/core/AutomationInterpolation.h"
#include "talas/expr/Expression.h"
#include "talas/project/ProjectMusicalTime.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
inline constexpr std::size_t maximumScoreTracks = 256;
inline constexpr std::size_t maximumScoreClips = 4096;
inline constexpr std::size_t maximumScoreAutomationLanes = 2048;
inline constexpr std::size_t maximumScoreAutomationPoints = 65536;
inline constexpr std::size_t maximumScoreMarkers = 4096;
inline constexpr std::size_t maximumScoreIdentifierBytes = 128;
inline constexpr std::size_t maximumScoreDisplayNameBytes = 256;
inline constexpr std::size_t maximumScoreMarkerTitleBytes = 256;
inline constexpr std::uint32_t maximumScoreChannels = 8;
// The hardware's audio inputs a track may take.
inline constexpr std::uint32_t maximumScoreInputChannels = 8;
inline constexpr std::uint32_t maximumScoreClipRepeats = 1024;
inline constexpr std::size_t maximumScoreEditBatchEntities = 8192;
inline constexpr std::size_t scoreClipRenderFingerprintBytes = 64;
inline constexpr std::size_t maximumScoreClipRenderAssetIdBytes = 512;
inline constexpr std::uint32_t scoreClipRenderArtifactRendererVersion = 1;
inline constexpr int scoreClipRenderArtifactProjectFormatVersion = 33;

enum class ScoreTimeDomain
{
    Seconds,
    MusicalTicks,
};

struct ScoreTime
{
    ScoreTimeDomain domain = ScoreTimeDomain::Seconds;
    double seconds = 0.0;
    std::uint64_t tick = 0;

    bool operator==(const ScoreTime &) const = default;
};

enum class ScoreTrackKind
{
    Track,
    Submix,
    Master,
};

enum class ScoreOverlapMode
{
    Reject,
    Layer,
    Replace,
};

struct ScoreTrack
{
    std::string trackId;
    std::string displayName;
    ScoreTrackKind kind = ScoreTrackKind::Track;
    ScoreTimeDomain timeDomain = ScoreTimeDomain::Seconds;
    std::string outputTrackId;
    std::uint32_t channelCount = 2;
    ScoreOverlapMode overlapMode = ScoreOverlapMode::Reject;
    bool muted = false;
    bool solo = false;
    bool sharingMemoryWriters = false;
    // Format 127: a submix this track's Sounds take as their audio input, by
    // its track ID; empty for no input.
    std::string inputTrackId;
    // Format 128: the submixes this Sound track sends to besides its output,
    // each at the level of its Track_To_Submix control, a track lane named by
    // scoreSendLevelName.
    std::vector<std::string> sendTrackIds;
    // Format 131: the output channels, from 1, the track is hard-wired to
    // instead of its pan controls, its channel k going to the (k mod n)th of
    // them; empty to use its pan controls.
    std::vector<std::uint32_t> outputChannels;
    // Format 132: the MIDI channel, 1 to 16, on which the Sounds in this track
    // expect to receive MIDI, their default channel; 0 for the project's.
    std::uint8_t midiChannel = 0;
    // Format 133: the audio input channel of the hardware, from 1, this Sound
    // track's Sounds take as their input instead of a submix; 0 for none.
    std::uint32_t inputChannel = 0;
    // Format 134: No output, a Sound track heard only through the submixes it
    // sends to.
    bool noOutput = false;

    bool operator==(const ScoreTrack &) const = default;
};

enum class ScoreClipRenderMode
{
    Cached,
    Frozen,
};

struct ScoreClipRenderArtifact
{
    ScoreClipRenderMode mode = ScoreClipRenderMode::Cached;
    std::string sourceFingerprint;
    std::uint32_t sampleRate = 0;
    std::uint64_t frameCount = 0;
    std::vector<std::string> channelAssetIds;

    bool operator==(const ScoreClipRenderArtifact&) const = default;
};

struct ScoreClip
{
    std::string clipId;
    std::string trackId;
    std::string soundDefinitionId;
    ScoreTime start;
    ScoreTime duration;
    ScoreTime contentOffset;
    ScoreTime fadeIn;
    ScoreTime fadeOut;
    std::uint32_t repeatCount = 1;
    std::optional<ScoreClipRenderArtifact> renderArtifact;
    // Optional exact root-graph Sound instance whose complete upstream graph
    // supplies this clip. Null supports input-free definition playback and
    // lossless historical compatibility; an unbound external-input Sound is
    // valid portable state but cannot be materialized until explicitly bound.
    std::optional<std::string> sourceInstanceId;
    // Format 124: the anchor, a time within the clip marking an event in it,
    // as an offset from its start in the start's domain; none leaves the
    // anchor at the start.
    std::optional<ScoreTime> anchorOffset;
    // Format 146: the colour the clip is drawn in, as 0xRRGGBB; none for its
    // track's.
    std::optional<std::uint32_t> colour;

    bool operator==(const ScoreClip &) const = default;
};

enum class ScoreAutomationScope
{
    Clip,
    Track,
    Master,
};

struct ScoreAutomationPoint
{
    ScoreTime position;
    double value = 0.0;
    talas::core::AutomationInterpolation interpolation = talas::core::AutomationInterpolation::Step;

    bool operator==(const ScoreAutomationPoint &) const = default;
};

// A parameter's state in the Timeline (format 123): Automated, its lane's
// function drives it; Live, its fader or MIDI controller does and the lane
// waits; Record, armed so the next recording takes its moves.
enum class ScoreAutomationState
{
    Automated,
    Live,
    Record,
};

// Format 129: a version of a lane's control function kept beside the one
// driving its parameter.
struct ScoreAutomationTake
{
    std::string name;
    std::vector<ScoreAutomationPoint> points;

    bool operator==(const ScoreAutomationTake &) const = default;
};

inline constexpr std::size_t maximumScoreAutomationTakes = 64;

// Format 136: the controller transformations, each a button that, pressed,
// passes the lane's function through it without changing its points, and,
// pressed again, takes it away; several combine, in the order they were
// pressed.
enum class ScoreAutomationTransformKind
{
    // Values mirrored about the value given.
    Invert,
    // The graph run backwards between its first and last points.
    Reverse,
    // The shape repeated, as often as fits, up to the time given.
    Loop,
    // Each value held to the next point.
    SquareOff,
    // Each value multiplied by the value given.
    Scale,
    // The value given added to each value.
    Offset,
    // 1 above the threshold given, 0 below it, a value crossing only when
    // it passes the threshold by half the hysteresis given (secondary).
    Threshold,
    // Times from the first point stretched by the factor given.
    TimeScale,
    // Random values, as large as the value given, added between the points
    // at the interval given (secondary).
    Spray,
    // The times from the first point to the last stretched or squeezed to
    // fit exactly once within the duration given, from 0.
    ScaleToFit,
    // Each point moved to the nearest multiple of the quantization given
    //.
    QuantizeEndpoints,
};

struct ScoreAutomationTransform
{
    ScoreAutomationTransformKind kind = ScoreAutomationTransformKind::Invert;
    double value = 0.0;
    double secondary = 0.0;
    std::uint64_t seed = 1;

    bool operator==(const ScoreAutomationTransform &) const = default;
};

inline constexpr std::size_t maximumScoreAutomationTransforms = 16;

// One transformation of a function, nothing when it cannot apply (no
// points, or a parameter out of range).
[[nodiscard]] std::optional<std::vector<ScoreAutomationPoint>> transformedScoreAutomationPoints(
    std::vector<ScoreAutomationPoint> points, const ScoreAutomationTransform &transform);
// A function through each of the transformations in turn; one that cannot
// apply leaves it as it was.
[[nodiscard]] std::vector<ScoreAutomationPoint> scoreAutomationEffectivePoints(
    std::vector<ScoreAutomationPoint> points, std::span<const ScoreAutomationTransform> transforms);

struct ScoreAutomationLane
{
    std::string laneId;
    ScoreAutomationScope scope = ScoreAutomationScope::Track;
    std::string ownerId;
    std::string eventName;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::vector<ScoreAutomationPoint> points;
    ScoreAutomationState state = ScoreAutomationState::Automated;
    // The take whose function `points` is, the Source of
    // Control, and the lane's other takes.
    std::string takeName = "take1";
    std::vector<ScoreAutomationTake> takes;
    // Format 130: the lane this one is slaved to, whose function and state
    // drive this lane's parameter; empty when it is not slaved.
    std::string masterLaneId;
    // Format 136: the transformations the lane's function passes through, the
    // function it plays; on a slaved lane, the master's function is
    // transformed for this lane alone.
    std::vector<ScoreAutomationTransform> transforms;

    bool operator==(const ScoreAutomationLane &) const = default;
};

struct ScoreMarker
{
    std::string markerId;
    std::string title;
    ScoreTime start;
    std::optional<ScoreTime> end;
    // Format 124: the MIDI program change, 1 to 128, that jumps the cursor
    // here; none uses the marker's number.
    std::optional<std::uint8_t> programNumber;

    bool operator==(const ScoreMarker &) const = default;
};

struct ProjectScore
{
    std::vector<ScoreTrack> tracks;
    std::vector<ScoreClip> clips;
    std::vector<ScoreAutomationLane> automation;
    std::vector<ScoreMarker> markers;

    bool operator==(const ProjectScore &) const = default;
};

enum class ScoreValidationCode
{
    TrackLimit,
    ClipLimit,
    AutomationLaneLimit,
    AutomationPointLimit,
    MarkerLimit,
    EditBatchLimit,
    InvalidIdentifier,
    DuplicateIdentifier,
    InvalidDisplayName,
    InvalidTrack,
    MissingMaster,
    MultipleMasters,
    UnknownRoute,
    InvalidRoute,
    RouteCycle,
    UnknownTrack,
    UnknownSoundDefinition,
    InvalidTime,
    TimeOverflow,
    InvalidClip,
    InvalidRenderArtifact,
    OverlapRejected,
    InvalidAutomation,
    DuplicateAutomationTarget,
    InvalidMarker,
};

struct ScoreValidationError
{
    ScoreValidationCode code = ScoreValidationCode::InvalidTrack;
    std::string entityId;
    std::string message;

    bool operator==(const ScoreValidationError &) const = default;
};

[[nodiscard]] bool isValidScoreIdentifier(std::string_view value) noexcept;
[[nodiscard]] bool isCanonicalScoreTime(const ScoreTime &value) noexcept;
[[nodiscard]] std::optional<std::uint64_t>
scoreTimeToSampleFrame(const ScoreTime &value, const ProjectMusicalTime &musicalTime,
                       std::uint32_t sampleRate) noexcept;
[[nodiscard]] std::optional<ScoreValidationError>
projectScoreValidationError(const ProjectScore &score,
                            std::span<const std::string_view> soundDefinitionIds = {});

struct ResolvedScoreTrack
{
    std::string trackId;
    bool audible = false;

    bool operator==(const ResolvedScoreTrack &) const = default;
};

[[nodiscard]] std::vector<ResolvedScoreTrack>
resolveScoreTrackAudibility(const ProjectScore &score);
[[nodiscard]] std::vector<std::string>
activeScoreClipIds(const ProjectScore &score, std::string_view trackId, const ScoreTime &position);

// Whether a track's output passes through
// another track (a submix or Master) on its way out; a track passes
// through itself.
[[nodiscard]] bool scoreTrackRoutesThrough(const ProjectScore &score, std::string_view trackId,
                                           std::string_view throughTrackId);
// The name of a track's send level control for a submix, To_ and
// the submix's track ID; its lane scales what the track sends there.
[[nodiscard]] std::string scoreSendLevelName(std::string_view submixId);
// Whether a track's audio reaches a submix, through its output route
// (unless it has No output) or through one of its sends.
[[nodiscard]] bool scoreTrackReaches(const ProjectScore &score, std::string_view trackId,
                                     std::string_view submixId);
// The submix a track's audio enters on its way to one it reaches: its
// output, or the send that leads there.
[[nodiscard]] std::optional<std::string> scoreTrackEntryTowards(const ProjectScore &score, std::string_view trackId,
                                                                std::string_view submixId);
// The Sound tracks whose output reaches a track's input submix.
[[nodiscard]] std::vector<std::string> scoreTrackInputFeeders(const ProjectScore &score, std::string_view trackId);
// Whether a track's output reaches another track's input, through any
// chain of submixes and tracks taking them as input.
[[nodiscard]] bool scoreTrackFeeds(const ProjectScore &score, std::string_view sourceTrackId,
                                   std::string_view targetTrackId);
// The lanes a lane may be slaved to: another
// control of the same Sound, a track control of its track, or a master
// control, none of them slaved itself.
[[nodiscard]] std::vector<std::string> scoreAutomationMasterCandidates(const ProjectScore &score,
                                                                     std::string_view laneId);
// The lane whose function and state drive a lane: its master when it is
// slaved, else itself.
[[nodiscard]] const ScoreAutomationLane *scoreAutomationDrivingLane(const ProjectScore &score,
                                                                    const ScoreAutomationLane &lane);

// The submixes a track may take as its input, leaving out those its
// own output would come back through.
[[nodiscard]] std::vector<std::string> scoreTrackInputChoices(const ProjectScore &score, std::string_view trackId);

struct ProjectScoreEditResult
{
    std::optional<ProjectScore> candidate;
    std::optional<ScoreValidationError> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return candidate.has_value() && !error.has_value();
    }
};

// The Score with the clips given alone, their own
// lanes with them; the tracks and their lanes stay.
[[nodiscard]] ProjectScore scoreWithPlayedClips(const ProjectScore &score, const std::vector<std::string> &clipIds);

struct ProjectScoreEditBatch
{
    std::vector<std::string> removeTrackIds;
    std::vector<std::string> removeClipIds;
    std::vector<std::string> removeAutomationLaneIds;
    std::vector<std::string> removeMarkerIds;
    std::vector<ScoreTrack> tracks;
    std::vector<ScoreClip> clips;
    std::vector<ScoreAutomationLane> automation;
    std::vector<ScoreMarker> markers;
};

[[nodiscard]] ProjectScoreEditResult
projectScoreWithEdits(const ProjectScore &score, ProjectScoreEditBatch edits,
                      std::span<const std::string_view> soundDefinitionIds = {});

[[nodiscard]] ProjectScoreEditResult
projectScoreWithTrack(const ProjectScore &score, ScoreTrack track,
                      std::span<const std::string_view> soundDefinitionIds = {});
[[nodiscard]] ProjectScoreEditResult
projectScoreWithoutTrack(const ProjectScore &score, std::string_view trackId,
                         std::span<const std::string_view> soundDefinitionIds = {});
[[nodiscard]] ProjectScoreEditResult
projectScoreWithClip(const ProjectScore &score, ScoreClip clip,
                     std::span<const std::string_view> soundDefinitionIds = {});
[[nodiscard]] ProjectScoreEditResult
projectScoreWithoutClip(const ProjectScore &score, std::string_view clipId,
                        std::span<const std::string_view> soundDefinitionIds = {});
[[nodiscard]] ProjectScoreEditResult
projectScoreWithAutomationLane(const ProjectScore &score, ScoreAutomationLane lane,
                               std::span<const std::string_view> soundDefinitionIds = {});
[[nodiscard]] ProjectScoreEditResult
projectScoreWithoutAutomationLane(const ProjectScore &score, std::string_view laneId,
                                  std::span<const std::string_view> soundDefinitionIds = {});
[[nodiscard]] ProjectScoreEditResult
projectScoreWithMarker(const ProjectScore &score, ScoreMarker marker,
                       std::span<const std::string_view> soundDefinitionIds = {});
[[nodiscard]] ProjectScoreEditResult
projectScoreWithoutMarker(const ProjectScore &score, std::string_view markerId,
                          std::span<const std::string_view> soundDefinitionIds = {});
} // namespace talas::project
