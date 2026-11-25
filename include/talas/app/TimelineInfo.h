#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <vector>

// The information strip at the top of the Timeline: the selected Sound's
// start, anchor, duration and end, the cursor's time and the selected marker's
// name, program change and time, each a display and a field to type into; a
// Sound's anchor and the alignments it serves; and the markers a MIDI program
// change jumps to.
namespace talas::app
{
// A time as the strip shows it: seconds as a plain number ("2.5"), a musical
// position as bar.beat.tick, a musical length as its ticks ("960 ticks").
[[nodiscard]] std::string timelineTimeText(
    const talas::project::ScoreTime& time, const talas::project::ProjectMusicalTime& musicalTime, bool length = false);

// What the strip reads: in seconds, a plain number ("12.5"), minutes and
// seconds ("1:02.5"), hours, minutes and seconds ("0:01:02.5") or time code,
// hours:minutes:seconds:frames at the frames a second given ("00:00:60:00");
// a musical position as bar.beat.tick or bar.beat, a musical length as ticks
// or as whole bars, beats and ticks from the start ("8.0.0", eight bars).
// Nothing for text it cannot read or a time before zero.
[[nodiscard]] std::optional<talas::project::ScoreTime> parseTimelineTime(
    std::string_view text,
    talas::project::ScoreTimeDomain domain,
    const talas::project::ProjectMusicalTime& musicalTime,
    bool length = false,
    unsigned framesPerSecond = 30);

// Tab in a time field: a time's units as it is
// written (hours, minutes, seconds and tenths of seconds, frames, or bar,
// beat and tick), each the digits between its separators; the unit after the
// one the caret is in (Shift+Tab before it), round to the first again.
struct TimelineTimeUnit
{
    std::size_t start = 0;
    std::size_t length = 0;

    bool operator==(const TimelineTimeUnit&) const = default;
};

[[nodiscard]] std::vector<TimelineTimeUnit> timelineTimeUnits(std::string_view text);
[[nodiscard]] std::optional<TimelineTimeUnit> adjacentTimelineTimeUnit(std::string_view text, std::size_t caret, bool forward);

struct TimelineSoundInfo
{
    std::string name;
    // Live, Cached or Frozen.
    std::string cacheState;
    talas::project::ScoreTime start;
    talas::project::ScoreTime anchor;
    // One pass of the Sound; the end takes its repeats.
    talas::project::ScoreTime duration;
    talas::project::ScoreTime end;
};

[[nodiscard]] std::optional<TimelineSoundInfo> timelineSoundInfo(
    const talas::project::ProjectDocument& document, std::string_view clipId);

// A group of selected Sounds: the name and cache
// state of the earliest, the earliest start, the latest end and the whole
// group's duration (from the one to the other); the earliest of them; and
// the group moved together, the earliest to the start given and the others
// keeping their offsets from it. Sounds in another time domain than the
// earliest's are left out.
[[nodiscard]] std::optional<TimelineSoundInfo> timelineGroupInfo(
    const talas::project::ProjectDocument& document, const std::vector<std::string>& clipIds);
[[nodiscard]] std::optional<std::string> earliestClip(
    const talas::project::ProjectScore& score, const std::vector<std::string>& clipIds);
[[nodiscard]] std::optional<std::vector<talas::project::ScoreClip>> clipsMovedTogether(
    const talas::project::ProjectScore& score, const std::vector<std::string>& clipIds, const talas::project::ScoreTime& earliestStart);

enum class TimelineSoundField : std::uint8_t
{
    Start,
    Anchor,
    Duration,
    End,
};

// The clip a typed field gives: a new start moves the clip, its
// anchor with it; a new anchor marks a time within the clip; a new duration
// or end lengthens or shortens it, an anchor past the new end coming back to
// it. Nothing for a time in another domain, an anchor outside the clip, an
// end not after the start, or an end its repeats cannot divide.
[[nodiscard]] std::optional<talas::project::ScoreClip> clipWithSoundField(
    const talas::project::ScoreClip& clip, TimelineSoundField field, const talas::project::ScoreTime& value);

// A clip's anchor time: its start unless an anchor has been marked.
[[nodiscard]] talas::project::ScoreTime clipAnchorTime(const talas::project::ScoreClip& clip);

enum class ClipAlignment : std::uint8_t
{
    StartToAnchor,
    EndToAnchor,
};

// A clip moved to begin or end at another clip's anchor; nothing
// across time domains or before zero.
[[nodiscard]] std::optional<talas::project::ScoreClip> clipAlignedTo(
    const talas::project::ScoreClip& clip, const talas::project::ScoreClip& anchored, ClipAlignment alignment);

// The markers in time order, numbered from 1; a marker's program
// change is its own or else its number; the marker a program change jumps
// the cursor to, the first in time with it.
[[nodiscard]] std::vector<const talas::project::ScoreMarker*> markersInTimeOrder(
    const talas::project::ProjectDocument& document, std::uint32_t sampleRate);
[[nodiscard]] std::size_t markerNumber(
    const talas::project::ProjectDocument& document, std::uint32_t sampleRate, std::string_view markerId);
[[nodiscard]] std::uint8_t markerProgramNumber(
    const talas::project::ProjectDocument& document, std::uint32_t sampleRate, const talas::project::ScoreMarker& marker);
[[nodiscard]] const talas::project::ScoreMarker* markerForProgramChange(
    const talas::project::ProjectDocument& document, std::uint32_t sampleRate, std::uint8_t programNumber);

// Whether a program change on a MIDI channel is the Timeline's, jumping its
// cursor to a marker rather than choosing a preset.
[[nodiscard]] bool timelineTakesProgramChange(const talas::project::ProjectDocument& document, std::uint8_t channel) noexcept;

// The marker with a new program change: none when it is the marker's number.
[[nodiscard]] talas::project::ScoreMarker markerWithProgramNumber(
    const talas::project::ProjectDocument& document,
    std::uint32_t sampleRate,
    talas::project::ScoreMarker marker,
    std::uint8_t programNumber);

// A Sound dropped into the BPM controller sets
// the Timeline's tempo to its own, from its duration, taken as exactly one
// bar of the Timeline's meter or as the beats given, so it lasts that long
// at the new tempo; the tempo map becomes that one tempo. Nothing for a
// Sound without a duration or beats that are not positive.
[[nodiscard]] double timelineBeatsPerBar(const talas::project::ProjectDocument& document);
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithSoundTempo(
    const talas::project::ProjectDocument& document,
    std::optional<double> durationSeconds,
    std::optional<double> beats,
    std::string& error);

// Where the Marker dialog places the marker: OK at the
// time typed, Capture and Enter at the cursor whatever was typed; nothing
// for Cancel or a time it cannot read.
enum class MarkerDialogAnswer
{
    Ok,
    Capture,
    Enter,
    Cancel,
};

[[nodiscard]] std::optional<std::uint64_t> markerDialogFrame(MarkerDialogAnswer answer, std::string_view typed,
    std::uint64_t cursorFrame, const talas::project::ProjectMusicalTime& musicalTime, std::uint32_t sampleRate);

// The Automation menu's Set Master BPM... gives
// the whole Timeline one tempo; nothing for one not from 1 to 1000 BPM.
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithMasterBpm(
    const talas::project::ProjectDocument& document, double bpm, std::string& error);

// The time cursor's source of external control, the
// menu that reads Free running or Time code: Time code follows MIDI time
// code from the MIDI input already chosen, else the first one there is; and
// Set time code offset, the starting time code of the source typed as
// hours:minutes:seconds:frames at the frames a second given.
[[nodiscard]] std::string timelineTimeSourceLabel(const talas::project::ProjectDocument& document);
[[nodiscard]] std::optional<talas::project::ProjectTransportSyncSettings> timelineTimeSource(
    const talas::project::ProjectDocument& document,
    bool timeCode,
    const std::vector<std::string>& midiInputs,
    std::string& error);
[[nodiscard]] std::string timelineTimeCodeOffsetText(const talas::project::ProjectDocument& document, unsigned framesPerSecond);
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithTimeCodeOffset(
    const talas::project::ProjectDocument& document,
    std::string_view text,
    unsigned framesPerSecond,
    std::string& error);
// The Timeline's own time code frames a second,
// which override the project's (its Preferences' frames a second); the
// Timeline with its own set, 24, 25 or 30, or 0 to follow the project's.
[[nodiscard]] unsigned timelineFramesPerSecond(const talas::project::ProjectDocument& document) noexcept;
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithFramesPerSecond(
    const talas::project::ProjectDocument& document, unsigned framesPerSecond);
// The Timeline's Audio menu sets its number of
// outputs, the Master's channels, which each track and submix takes too,
// a hard-routed channel past them dropped; 1 to 8.
[[nodiscard]] std::uint32_t timelineOutputs(const talas::project::ProjectDocument& document) noexcept;
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithOutputs(
    const talas::project::ProjectDocument& document, std::uint32_t outputs);
// The Audio menu's cache file format, the format
// the Timeline's disk caches and frozen clips are recorded in: each
// channel a mono WAV at the current sample rate, 24 bits by default or
// 16; its name, and the Timeline with it.
[[nodiscard]] std::string timelineCacheFormatName(unsigned bits);
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithCacheWordSize(
    const talas::project::ProjectDocument& document, unsigned bits);
// The Audio menu's Share recordings across tracks,
// so a reader and its MemoryWriter in separate tracks with one start time
// play together, as in a Mixer (format 156); its label, and the Timeline
// with it on or off.
[[nodiscard]] std::string_view timelineSharingAcrossTracksName() noexcept;
[[nodiscard]] talas::project::ProjectDocument timelineWithMemoryWritersSharedAcrossTracks(
    const talas::project::ProjectDocument& document, bool shared);
// The colour menu at the right of the information
// strip, which colours the selected Sounds: its colours, by name and
// 0xRRGGBB, the first, Track, taking the track's again; and the Timeline
// with the clips given in a colour, or in their track's.
struct TimelineClipColour
{
    std::string_view name;
    std::optional<std::uint32_t> rgb;
};
[[nodiscard]] std::span<const TimelineClipColour> timelineClipColours() noexcept;
[[nodiscard]] std::optional<talas::project::ProjectDocument> timelineWithClipColour(
    const talas::project::ProjectDocument& document, const std::vector<std::string>& clipIds, std::optional<std::uint32_t> rgb);
// The information strip's parts from left to right,
// the selected marker's name at the far left; a part with nothing selected
// for it is left out where it is drawn.
enum class TimelineInfoStripPart : std::uint8_t
{
    MarkerName,
    MarkerProgram,
    MarkerTime,
    Cursor,
    MarkerButton,
    PointTime,
    PointValue,
    SoundName,
    SoundStart,
    SoundAnchor,
    SoundDuration,
    SoundEnd,
    AnchorButton,
};
[[nodiscard]] std::span<const TimelineInfoStripPart> timelineInfoStripParts() noexcept;
// The Cache menu, the doughnut at the top of the
// Timeline, of the selected Sound: green while the Sound plays live; red,
// the Sound too, while it is being cached or its cache no longer matches
// it and waits to be cached again; blue, the Sound gray, once it is cached
// (or frozen). Its items: Live brings the Sound back to life, Cache caches
// it, Re-cache records it again.
enum class TimelineCacheLook : std::uint8_t
{
    Live,
    Caching,
    Cached,
};

// A lane's source of control as its menu shows
// it: a Live or Record-enabled lane's MIDI channel and controller in the
// project's global map, as Ch 2: cc 7 (no MIDI where the map has none),
// else the take driving it.
[[nodiscard]] std::string timelineLaneControlSource(
    const talas::project::ProjectDocument& document, const talas::project::ScoreAutomationLane& lane);
// A lane's MIDI channel and controller chosen, the global map's text
// with !eventName's association on them, its other settings kept, or a new
// association; nothing where the map would not parse.
[[nodiscard]] std::optional<std::string> globalMapWithControlSource(
    std::string_view mapText, std::string_view eventName, std::uint8_t channel, std::uint8_t controller);

// A press on a Sound in a track within 7 points of
// its left edge trims its start, of its right edge drags its end to change
// its duration, and elsewhere moves it.
enum class TimelineClipGesture : std::uint8_t
{
    TrimStart,
    ResizeEnd,
    Move,
};
[[nodiscard]] TimelineClipGesture timelineClipGesture(double pointerX, double clipLeft, double clipRight) noexcept;

// The yellow time cursor showing where the Timeline
// is playing; as ARGB.
[[nodiscard]] std::uint32_t timelineTimeCursorColour() noexcept;
[[nodiscard]] std::uint32_t timelineCacheMenuColour(TimelineCacheLook look) noexcept;
// The Sound's own colour in the Timeline; none for a live one, which keeps
// the Timeline's.
[[nodiscard]] std::optional<std::uint32_t> timelineCachedSoundColour(TimelineCacheLook look) noexcept;
// The name of a parameter lane of a Sound being
// cached turns red with it; none for any other lane.
[[nodiscard]] std::optional<std::uint32_t> timelineLaneNameColour(
    const talas::project::ScoreAutomationLane& lane, TimelineCacheLook ownerLook) noexcept;

struct TimelineCacheMenuItem
{
    std::string name;
    bool enabled = false;
    bool ticked = false;

    bool operator==(const TimelineCacheMenuItem&) const = default;
};

// Live, Cache and Re-cache for a Sound that has its source to cache.
[[nodiscard]] std::vector<TimelineCacheMenuItem> timelineCacheMenuItems(TimelineCacheLook look, bool sourceAvailable);
// Where a Sound dropped in the Timeline goes:
// onto the track under it, into the controller lane under it, or, dropped
// below the last row, onto a new track made for it; there is no limit to
// the tracks. None above the rows.
enum class TimelineDropTarget : std::uint8_t
{
    None,
    Track,
    Lane,
    NewTrack,
};

// The row under the drop, counted from the first row shown, whether that
// row is a track or a controller lane, and how many rows there are.
[[nodiscard]] TimelineDropTarget timelineDropTarget(
    std::optional<std::size_t> rowUnder, bool trackRow, bool laneRow, std::size_t rowCount) noexcept;
// The selected Sound's time fields in the Timeline's strip: the edges given
// extra colour as a reminder of what each holds, the start's left edge, a glow
// all round the anchor, the duration's top and both sides, the end's right
// edge; and the help shown with the pointer in a field.
enum class TimelineTimeField : std::uint8_t
{
    Start,
    Anchor,
    Duration,
    End,
};

struct TimelineTimeFieldBorder
{
    bool left = false;
    bool right = false;
    bool top = false;
    bool bottom = false;

    bool operator==(const TimelineTimeFieldBorder&) const = default;
};

[[nodiscard]] TimelineTimeFieldBorder timelineTimeFieldBorder(TimelineTimeField field) noexcept;
[[nodiscard]] std::string_view timelineTimeFieldHelp(TimelineTimeField field) noexcept;

// The marker name field takes no spaces, Space
// playing and pausing the Timeline there; a name with spaces is typed in
// the Large window (Ctrl+L). What typing puts into the field.
[[nodiscard]] std::string timelineMarkerNameTyped(std::string_view typed);

// A Timeline is a Sound: its equivalent Sound, a
// Mixer of SetDurations, each clip's Sound started at the clip's start for
// the clip's duration, those on muted tracks left out; the Mixer's left
// output is the Sound's. Named `name`; the clips' fades, offsets and
// repeats, the tracks' levels and positions and the controls stay the
// Timeline's own. Nothing for a Timeline without a Sound to play, or a
// clip whose time does not convert to seconds or whose Sound has no
// output.
[[nodiscard]] std::optional<talas::project::ProjectSoundDefinition> timelineAsSound(
    const talas::project::ProjectDocument& document, std::string_view name);

// Copy and Paste in a Timeline: the clips
// copied with the controls of their own; pasted onto a Sound track at a
// time, the earliest copied clip starting there and the others keeping
// their offsets from it, each a new clip, with new controls, of its own on
// that track. Nothing to paste without clips, onto a track that is not a
// Sound track, or at a time in another domain than the clips'.
struct TimelineClipsCopy
{
    std::vector<talas::project::ScoreClip> clips;
    std::vector<talas::project::ScoreAutomationLane> lanes;

    bool operator==(const TimelineClipsCopy&) const = default;
};
[[nodiscard]] TimelineClipsCopy timelineClipsCopied(
    const talas::project::ProjectScore& score, const std::vector<std::string>& clipIds);
[[nodiscard]] std::optional<talas::project::ProjectScoreEditBatch> timelineClipsPasted(const talas::project::ProjectScore& score,
    const TimelineClipsCopy& copied, std::string_view trackId, const talas::project::ScoreTime& at);
// The Sounds the copied clips play, each once, in order: what Paste in a
// Sound file window copies.
[[nodiscard]] std::vector<std::string> timelineCopiedSounds(const TimelineClipsCopy& copied);

// A Sound in the Timeline that expects MIDI
// note events (it reads !KeyDown, !KeyPitch, !KeyVelocity, !KeyNumber or
// !KeyTimbre) plays as many notes at once as its polyphony, taking them on
// its track's MIDI channel, the MIDI input channel the track's MIDI status
// control sets. The polyphony is a MIDIVoice put around the clip's Sound: a
// Sound of the project named for it with "voices", of two Sounds, the
// clip's Sound and the MIDIVoice playing it, on the default channel 0, which
// is the track's. The clip's polyphony, nothing while it plays its Sound as
// it is; and the project with the clip playing that many voices of its Sound
// (1 to 128; nothing for the Sound as it is), a wrapper of this clip's
// own changed in place, another made otherwise; or nothing, and why.
inline constexpr unsigned maximumTimelineClipPolyphony = 128;
[[nodiscard]] std::optional<unsigned> timelineClipPolyphony(
    const talas::project::ProjectDocument& document, std::string_view clipId);
struct TimelineClipPolyphonySet
{
    std::optional<talas::project::ProjectDocument> document;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return document.has_value(); }
};
[[nodiscard]] TimelineClipPolyphonySet timelineWithClipPolyphony(
    const talas::project::ProjectDocument& document, std::string_view clipId, std::optional<unsigned> polyphony);

// Sounds move from one Timeline to another by
// Copy and Paste (a drag never leaves its Timeline: it moves a Sound to
// another time and track of its own). The clips copied in the source
// Timeline's project, pasted into the destination's as timelineClipsPasted
// pastes them, each Sound they play coming in with the Sounds and samples it
// needs, as the Sound Browser brings one in: itself where the destination
// has the same Sound, renamed where it has another of that name. A clip
// bound to a Sound of the source's signal flow plays its Sound alone, and a
// control slaved to a master the destination lacks keeps a copy of the
// master's function. Within one Timeline, its Sounds unchanged since the
// copy, nothing comes in. The project with the clips pasted, the score edits
// that paste them, and the Sounds that came in; or nothing, and why.
struct TimelineClipsPastedIntoAnother
{
    std::optional<talas::project::ProjectDocument> document;
    talas::project::ProjectScoreEditBatch batch;
    std::vector<std::string> broughtSounds;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return document.has_value(); }
};
[[nodiscard]] TimelineClipsPastedIntoAnother timelineClipsPastedFromAnotherTimeline(
    const talas::project::ProjectDocument& destination,
    const talas::project::ProjectDocument& source,
    const TimelineClipsCopy& copied,
    std::string_view trackId,
    const talas::project::ScoreTime& at,
    const talas::core::NodeRegistry& registry,
    std::optional<std::filesystem::path> sourceProjectFilePath = {},
    std::optional<std::filesystem::path> destinationProjectFilePath = {});

// The Timeline's caches go in the folder of
// its file, in a folder named after it with Caches, haikuCaches for
// haiku.talas.json (a package's own name inside the package), so a Timeline
// is saved before any of its Sounds is cached.
[[nodiscard]] std::filesystem::path timelineCachesFolder(const std::filesystem::path& projectFile);
}
