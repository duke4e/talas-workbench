#include "talas/app/TalasKeyEquivalents.h"
#include "talas/app/TimelineInfo.h"
#include "talas/app/TimelineMarks.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectGlobalMap.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using talas::project::ScoreTime;
using talas::project::ScoreTimeDomain;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

ScoreTime seconds(double value)
{
    return {ScoreTimeDomain::Seconds, value, 0};
}

ScoreTime ticks(std::uint64_t value)
{
    return {ScoreTimeDomain::MusicalTicks, 0.0, value};
}

talas::project::ProjectDocument document()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "vocoder";
    definition.displayName = "Freq/Time Scaling Vocoder";
    document.soundDefinitions.push_back(definition);
    document.timeline.score.clips.push_back(
        {"clip", "track", "vocoder", seconds(4.0), seconds(10.0), seconds(0.0), seconds(0.0), seconds(0.0), 1});
    return document;
}

// The fields read seconds, minutes and seconds
// or time code, and bars, beats and ticks; they show what they read.
void testFieldsReadAndShowTimes()
{
    const talas::project::ProjectMusicalTime musical;
    require(talas::app::parseTimelineTime("12.5", ScoreTimeDomain::Seconds, musical) == seconds(12.5), "a plain number is seconds");
    require(talas::app::parseTimelineTime("1:02.5", ScoreTimeDomain::Seconds, musical) == seconds(62.5), "minutes and seconds");
    require(talas::app::parseTimelineTime("00:00:00:00", ScoreTimeDomain::Seconds, musical) == seconds(0.0),
        "00:00:00:00 starts a Sound at zero");
    require(talas::app::parseTimelineTime("00:00:60:00", ScoreTimeDomain::Seconds, musical) == seconds(60.0),
        "00:00:60:00 in time code is sixty seconds");
    require(talas::app::parseTimelineTime("00:00:01:15", ScoreTimeDomain::Seconds, musical) == seconds(1.5),
        "time code frames are thirtieths of a second");
    require(!talas::app::parseTimelineTime("-1", ScoreTimeDomain::Seconds, musical).has_value()
            && !talas::app::parseTimelineTime("soon", ScoreTimeDomain::Seconds, musical).has_value(),
        "a time before zero or text that is no time is refused");
    require(talas::app::timelineTimeText(seconds(2.5), musical) == "2.5" && talas::app::timelineTimeText(seconds(60.0), musical) == "60",
        "seconds show as a plain number");
    const auto bar = talas::app::parseTimelineTime("2.1.0", ScoreTimeDomain::MusicalTicks, musical);
    require(bar.has_value() && bar->tick == 4U * musical.ticksPerQuarter, "bar 2 begins four quarters in, in 4/4");
    require(talas::app::timelineTimeText(*bar, musical) == "2.1.0", "a musical position shows as bar.beat.tick");
    require(talas::app::parseTimelineTime("1.3", ScoreTimeDomain::MusicalTicks, musical)->tick == 2U * musical.ticksPerQuarter,
        "bar.beat reads the beat's start");
    require(!talas::app::parseTimelineTime("1.9.0", ScoreTimeDomain::MusicalTicks, musical).has_value(),
        "a beat the meter does not have is refused");
    require(talas::app::timelineTimeText(ticks(960), musical, true) == "960 ticks"
            && talas::app::parseTimelineTime("960 ticks", ScoreTimeDomain::MusicalTicks, musical, true) == ticks(960),
        "a musical length is its ticks");
    require(talas::app::parseTimelineTime("8.0.0", ScoreTimeDomain::MusicalTicks, musical, true) == ticks(8U * 4U * musical.ticksPerQuarter)
            && talas::app::parseTimelineTime("0.2.0", ScoreTimeDomain::MusicalTicks, musical, true) == ticks(2U * musical.ticksPerQuarter),
        "a length typed as 8.0.0 should be eight bars");
}

// Tab goes from one of a time's units to the next,
// from the seconds to the tenths of seconds, round to the first again;
// Shift+Tab back.
void testTabMovesBetweenATimesUnits()
{
    using Unit = talas::app::TimelineTimeUnit;
    require(talas::app::timelineTimeUnits("0:01:02.5") == std::vector<Unit>{{0, 1}, {2, 2}, {5, 2}, {8, 1}},
        "hours, minutes, seconds and tenths are each a unit");
    require(talas::app::adjacentTimelineTimeUnit("12.5", 1, true) == Unit{3, 1},
        "Tab should go from editing the seconds to editing the tenths of seconds");
    require(talas::app::adjacentTimelineTimeUnit("12.5", 3, true) == Unit{0, 2}
            && talas::app::adjacentTimelineTimeUnit("12.5", 0, false) == Unit{3, 1},
        "Tab after the last unit goes round to the first, Shift+Tab before the first to the last");
    require(talas::app::adjacentTimelineTimeUnit("00:00:60:00", 7, true) == Unit{9, 2}
            && talas::app::adjacentTimelineTimeUnit("2.1.480", 2, true) == Unit{4, 3},
        "time code's frames and a bar's ticks are units too");
    require(!talas::app::adjacentTimelineTimeUnit("soon", 0, true).has_value(), "text without digits has no unit");
}

// The selected Sound's name, cache state, start,
// anchor, duration and end; each field typed into changes the Sound.
void testTheSoundsInformationIsShownAndEdited()
{
    const auto project = document();
    const auto info = talas::app::timelineSoundInfo(project, "clip");
    require(info.has_value() && info->name == "Freq/Time Scaling Vocoder" && info->cacheState == "Live",
        "the information gives the Sound's name and whether it is cached");
    require(info->start == seconds(4.0) && info->anchor == seconds(4.0) && info->duration == seconds(10.0)
            && info->end == seconds(14.0),
        "by default the anchor is the start");
    const auto& clip = project.timeline.score.clips.front();
    const auto started = talas::app::clipWithSoundField(clip, talas::app::TimelineSoundField::Start, seconds(0.0));
    require(started.has_value() && started->start == seconds(0.0), "typing 0 in the Start field starts the Sound at zero");
    const auto ended = talas::app::clipWithSoundField(*started, talas::app::TimelineSoundField::End, seconds(60.0));
    require(ended.has_value() && ended->duration == seconds(60.0), "typing 60 seconds in the End field lengthens it");
    const auto anchored = talas::app::clipWithSoundField(*ended, talas::app::TimelineSoundField::Anchor, seconds(12.0));
    require(anchored.has_value() && talas::app::clipAnchorTime(*anchored) == seconds(12.0),
        "the anchor marks a time within the Sound");
    const auto moved = talas::app::clipWithSoundField(*anchored, talas::app::TimelineSoundField::Start, seconds(5.0));
    require(moved.has_value() && talas::app::clipAnchorTime(*moved) == seconds(17.0), "the anchor moves with its Sound");
    require(!talas::app::clipWithSoundField(*anchored, talas::app::TimelineSoundField::Anchor, seconds(61.0)).has_value()
            && !talas::app::clipWithSoundField(*anchored, talas::app::TimelineSoundField::End, seconds(0.0)).has_value()
            && !talas::app::clipWithSoundField(*anchored, talas::app::TimelineSoundField::Start, ticks(0)).has_value(),
        "an anchor outside the Sound, an end not after its start and a time in another domain are refused");
    const auto shortened = talas::app::clipWithSoundField(*anchored, talas::app::TimelineSoundField::Duration, seconds(8.0));
    require(shortened.has_value() && talas::app::clipAnchorTime(*shortened) == seconds(8.0),
        "an anchor past a new end comes back to it");
}

// Other Sounds align to begin or end at an anchor.
void testSoundsAlignToAnAnchor()
{
    auto project = document();
    auto anchored = *talas::app::clipWithSoundField(
        project.timeline.score.clips.front(), talas::app::TimelineSoundField::Anchor, seconds(7.0));
    talas::project::ScoreClip other{"other", "track", "vocoder", seconds(0.5), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 2};
    const auto begins = talas::app::clipAlignedTo(other, anchored, talas::app::ClipAlignment::StartToAnchor);
    require(begins.has_value() && begins->start == seconds(7.0), "a Sound aligned to begin at the anchor");
    const auto ends = talas::app::clipAlignedTo(other, anchored, talas::app::ClipAlignment::EndToAnchor);
    require(ends.has_value() && ends->start == seconds(3.0), "a Sound aligned to end at the anchor, its repeats taken");
    other.duration = seconds(5.0);
    require(!talas::app::clipAlignedTo(other, anchored, talas::app::ClipAlignment::EndToAnchor).has_value(),
        "a Sound cannot end at the anchor by starting before zero");
}

// Each marker has a program change, by default its
// number in time; a program change jumps the cursor to its marker.
// The time cursor's source menu, Free running or
// Time code, and the time code offset.
void testTheTimeCursorSource()
{
    auto project = document();
    std::string error;
    require(talas::app::timelineTimeSourceLabel(project) == "Free running", "where it now says Free running");
    const auto timeCode = talas::app::timelineTimeSource(project, true, {"mtc-in", "other"}, error);
    require(timeCode.has_value() && timeCode->sourceMode == talas::project::TransportSyncSourceMode::MidiTimeCodeExactInput
            && timeCode->sourceIdentifier == "mtc-in",
        "select Time code mode to control the time cursor from an external source");
    project.deviceSettings.transportSync = *timeCode;
    require(talas::app::timelineTimeSourceLabel(project) == "Time code", "the menu then reads Time code");
    project.deviceSettings.transportSync.sourceIdentifier = "other";
    require(talas::app::timelineTimeSource(project, true, {"mtc-in"}, error)->sourceIdentifier == "other",
        "the MIDI input already chosen stays");
    require(talas::app::timelineTimeSource(project, false, {}, error)->sourceMode == talas::project::TransportSyncSourceMode::Internal,
        "Free running, the internal clock");
    require(!talas::app::timelineTimeSource(document(), true, {}, error).has_value(), "time code needs a MIDI input");

    const auto offset = talas::app::timelineWithTimeCodeOffset(project, "01:00:00:00", 30, error);
    require(offset.has_value() && offset->timeline.timeCodeOffsetSeconds == 3600.0
            && talas::app::timelineTimeCodeOffsetText(*offset, 30) == "01:00:00:00",
        "the starting time code on your video, which rarely starts at 0: " + error);
    require(talas::app::timelineWithTimeCodeOffset(project, "00:59:59:15", 30, error)->timeline.timeCodeOffsetSeconds == 3599.5,
        "frames count at the frames a second given");
    require(!talas::app::timelineWithTimeCodeOffset(project, "soon", 30, error).has_value(), "an offset is a time code");
    talas::project::ProjectDocument saved;
    saved.timeline.timeCodeOffsetSeconds = offset->timeline.timeCodeOffsetSeconds;
    const auto loaded = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(saved));
    require(loaded.ok() && loaded.document.timeline.timeCodeOffsetSeconds == 3600.0, "the time code offset is saved");
}

// RustDrum80p8, 4 beats in 2.96961 s, dropped
// into the BPM controller.
// The Marker dialog's Enter places the marker at the
// cursor, not at the time typed; OK at the time typed.
void testTheMarkerDialog()
{
    using talas::app::MarkerDialogAnswer;
    const auto musical = document().timeline.musicalTime;
    require(talas::app::markerDialogFrame(MarkerDialogAnswer::Ok, "2", 48000, musical, 48000) == std::optional<std::uint64_t>{96000}
            && talas::app::markerDialogFrame(MarkerDialogAnswer::Enter, "2", 48000, musical, 48000) == std::optional<std::uint64_t>{48000}
            && talas::app::markerDialogFrame(MarkerDialogAnswer::Capture, "2", 48000, musical, 48000) == std::optional<std::uint64_t>{48000},
        "OK should place the marker at the time typed, Enter and Capture at the cursor");
    require(!talas::app::markerDialogFrame(MarkerDialogAnswer::Cancel, "2", 48000, musical, 48000).has_value()
            && !talas::app::markerDialogFrame(MarkerDialogAnswer::Ok, "soon", 48000, musical, 48000).has_value(),
        "Cancel, or a time OK cannot read, places no marker");
}

// Set Master BPM... gives the Timeline one
// tempo, 138 changed to 100 BPM.
void testSetMasterBpm()
{
    auto project = document();
    project.timeline.musicalTime.tempo = {{0, 434783}, {3840, 1000000}};
    std::string error;
    const auto slower = talas::app::timelineWithMasterBpm(project, 100.0, error);
    require(slower.has_value() && slower->timeline.musicalTime.tempo.size() == 1
            && slower->timeline.musicalTime.tempo.front().tick == 0
            && slower->timeline.musicalTime.tempo.front().microsecondsPerQuarter == 600000,
        "Set Master BPM to 100 should give the whole Timeline that one tempo: " + error);
    require(!talas::app::timelineWithMasterBpm(project, 0.0, error).has_value()
            && !talas::app::timelineWithMasterBpm(project, 5000.0, error).has_value(),
        "a tempo of 0 or past 1000 BPM should be refused");
}

void testTheTempoOfASample()
{
    auto project = document();
    project.timeline.musicalTime.tempo.push_back({3840, 1000000});
    std::string error;
    const auto beats = talas::app::timelineWithSoundTempo(project, 2.96961, 4.0, error);
    require(beats.has_value() && beats->timeline.musicalTime.tempo.size() == 1,
        "specify the duration of this Sound in beats, 4, and the BPM changes to match the sample's: " + error);
    const auto bpm = 60'000'000.0 / beats->timeline.musicalTime.tempo.front().microsecondsPerQuarter;
    require(std::abs(bpm - 80.82) < 0.005, "the BPM point should read 80.82");
    const auto bar = talas::project::musicalTickToSampleFrame(beats->timeline.musicalTime, 4 * 960, 48000);
    require(bar.has_value() && std::llabs(static_cast<long long>(*bar) - std::llround(2.96961 * 48000)) <= 1,
        "the sample lasts exactly one bar at the new BPM");
    const auto oneBar = talas::app::timelineWithSoundTempo(project, 2.96961, std::nullopt, error);
    require(oneBar.has_value() && oneBar->timeline.musicalTime.tempo == beats->timeline.musicalTime.tempo,
        "or assume that the sample is exactly one bar long, four beats of 4/4");
    project.timeline.musicalTime.meter.front() = {0, 3, 4};
    require(talas::app::timelineBeatsPerBar(project) == 3.0
            && std::abs(60'000'000.0 / talas::app::timelineWithSoundTempo(project, 3.0, std::nullopt, error)
                                         ->timeline.musicalTime.tempo.front().microsecondsPerQuarter - 60.0) < 0.01,
        "a bar of 3/4 is three beats");
    require(!talas::app::timelineWithSoundTempo(project, std::nullopt, 4.0, error).has_value()
            && !talas::app::timelineWithSoundTempo(project, 2.0, 0.0, error).has_value(),
        "a tempo needs the Sound's duration and some beats");
}

void testProgramChangesJumpToMarkers()
{
    auto project = document();
    project.timeline.score.markers = {
        {"late", "Coda", seconds(9.0), std::nullopt},
        {"early", "Intro", seconds(1.0), std::nullopt},
        {"middle", "Verse", seconds(4.0), std::nullopt},
    };
    require(talas::app::markerNumber(project, 48000, "early") == 1 && talas::app::markerNumber(project, 48000, "late") == 3,
        "markers are numbered in time");
    require(talas::app::markerProgramNumber(project, 48000, project.timeline.score.markers[2]) == 2,
        "by default the program change is the marker's number");
    require(talas::app::markerForProgramChange(project, 48000, 3)->markerId == "late",
        "program change 3 jumps the cursor to the third marker");
    auto verse = talas::app::markerWithProgramNumber(project, 48000, project.timeline.score.markers[2], 7);
    require(verse.programNumber == std::optional<std::uint8_t>{7}, "the program change can be changed");
    project.timeline.score.markers[2] = verse;
    require(talas::app::markerForProgramChange(project, 48000, 7)->markerId == "middle"
            && talas::app::markerForProgramChange(project, 48000, 2) == nullptr,
        "the marker answers its new program change only");
    require(!talas::app::markerWithProgramNumber(project, 48000, verse, 2).programNumber.has_value(),
        "a program change set back to the marker's number is its default again");
    require(!talas::app::timelineTakesProgramChange(project, 1), "with no channel chosen, program changes choose presets");
    project.timeline.markerProgramChannel = 16;
    require(talas::app::timelineTakesProgramChange(project, 16) && !talas::app::timelineTakesProgramChange(project, 1),
        "program changes on the Timeline's channel go to the Timeline, others to the presets");
}
}

// Sounds marked with a click, toggled with Shift,
// or marked by a box; Shift+Space plays the marked Sounds alone, compiling
// only them, and Space stays in that mode until Ctrl+Space plays them all.
void testMarkedSoundsPlayAlone()
{
    auto project = document();
    project.timeline.score.clips.push_back(
        {"clicks", "track", "vocoder", seconds(0.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1});
    project.timeline.score.clips.push_back(
        {"reverb", "track", "vocoder", seconds(1.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1});
    project.timeline.score.automation.push_back({"own", talas::project::ScoreAutomationScope::Clip, "clicks", "Gain",
        talas::expr::ValueDomain::Scalar, {{seconds(0.0), 0.5, talas::core::AutomationInterpolation::Step}}});
    project.timeline.score.automation.push_back({"track", talas::project::ScoreAutomationScope::Track, "track", "Gain",
        talas::expr::ValueDomain::Scalar, {{seconds(0.0), 0.5, talas::core::AutomationInterpolation::Step}}});
    talas::app::TimelineMarks marks;
    marks.mark("clicks");
    require(marks.marked("clicks") && !marks.marked("clip"), "a click with the modifier marks a Sound");
    marks.toggle("clicks");
    marks.toggle("reverb");
    require(!marks.marked("clicks") && marks.marked("reverb"), "Shift toggles a Sound between marked and unmarked");
    marks.markAll({"clicks", "clip"});
    require(marks.clipIds().size() == 3, "a box marks a group of Sounds");
    require(!marks.playedClips(project.timeline.score).has_value(), "the Timeline plays every Sound until asked otherwise");
    marks.clear();
    marks.mark("clicks");
    marks.playMarkedOnly();
    const auto played = marks.playedClips(project.timeline.score);
    require(played == std::optional<std::vector<std::string>>{std::vector<std::string>{"clicks"}},
        "Shift+Space plays the marked Sounds only");
    const auto score = talas::project::scoreWithPlayedClips(project.timeline.score, *played);
    require(score.clips.size() == 1 && score.clips.front().clipId == "clicks" && score.automation.size() == 2,
        "only the marked Sounds compile, their own controls and the tracks' with them");
    require(talas::project::scoreWithPlayedClips(project.timeline.score, {"reverb"}).automation.size() == 1,
        "an unmarked Sound's own controls go with it");
    require(marks.playsMarkedOnly(), "Space keeps playing the marked Sounds only");
    marks.playAll();
    require(!marks.playedClips(project.timeline.score).has_value(), "Ctrl+Space returns to playing them all");
    using talas::app::TimelineClipLook;
    require(talas::app::timelineClipOutline(false, false, true) == TimelineClipLook::Marked
            && talas::app::timelineClipText(true) == TimelineClipLook::Marked
            && talas::app::timelineClipOutline(false, false, false) == TimelineClipLook::Plain
            && talas::app::timelineClipOutline(true, true, true) == TimelineClipLook::Overlapping
            && talas::app::timelineClipOutline(false, true, true) == TimelineClipLook::Selected,
        "a marked Sound's outline and name should turn light grey");
}

// A group of Sounds selected: the earliest's name,
// the earliest start, the latest end and the group's whole duration; moved
// together by its start.
void testAGroupOfSounds()
{
    auto project = document();
    project.timeline.score.clips.push_back(
        {"clicks", "track2", "vocoder", seconds(1.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 2});
    project.timeline.score.clips.push_back(
        {"late", "track3", "vocoder", seconds(20.0), seconds(5.0), seconds(0.0), seconds(0.0), seconds(0.0), 1});
    const std::vector<std::string> group{"clip", "clicks", "late"};
    require(talas::app::earliestClip(project.timeline.score, group) == std::optional<std::string>{"clicks"},
        "the earliest Sound of the group");
    const auto info = talas::app::timelineGroupInfo(project, group);
    require(info.has_value() && info->name == "Freq/Time Scaling Vocoder" && info->start == seconds(1.0) && info->end == seconds(25.0)
            && info->duration == seconds(24.0),
        "the fields show the earliest start, the latest end and the total duration of the group");
    const auto moved = talas::app::clipsMovedTogether(project.timeline.score, group, seconds(0.0));
    require(moved.has_value() && moved->size() == 3 && (*moved)[0].start == seconds(3.0) && (*moved)[1].start == seconds(0.0)
            && (*moved)[2].start == seconds(19.0),
        "typing a start moves the group together");
    const auto one = talas::app::timelineGroupInfo(project, {"clip"});
    require(one.has_value() && one->duration == seconds(10.0), "a group of one is that Sound");
}

// The Cache menu is green for a live Sound, red,
// with the Sound, while it is being cached, and blue, the Sound gray, once
// it is; Live, Cache and Re-cache as each state allows.
void testTheCacheMenuShowsTheSoundsState()
{
    using talas::app::TimelineCacheLook;
    // The strongest of red, green and blue.
    const auto hue = [](std::uint32_t argb) {
        const auto red = (argb >> 16) & 0xffu;
        const auto green = (argb >> 8) & 0xffu;
        const auto blue = argb & 0xffu;
        return red > green && red > blue ? 'r' : green > blue ? 'g' : 'b';
    };
    require(hue(talas::app::timelineCacheMenuColour(TimelineCacheLook::Live)) == 'g'
            && hue(talas::app::timelineCacheMenuColour(TimelineCacheLook::Caching)) == 'r'
            && hue(talas::app::timelineCacheMenuColour(TimelineCacheLook::Cached)) == 'b',
        "the doughnut should be green live, red while caching and blue cached");
    // A Live lane's source of control is its MIDI
    // channel and controller, chosen from the lane's menu into the global
    // map; an automated lane's is its take.
    const std::string map = "!Volume is: (`MIDIController07 channel: 5; min: 0.5 max: 0.75 grid: 0.05; taper: #log).\n"
                            "!Index is: `MIDIController01.\n";
    const auto entriesOf = [](const std::optional<std::string>& text) {
        return text.has_value() ? talas::project::parseProjectGlobalMap(*text).entries : std::vector<talas::project::GlobalMapEntry>{};
    };
    const auto find = [](const std::vector<talas::project::GlobalMapEntry>& entries, std::string_view name) {
        const auto found = std::ranges::find(entries, name, &talas::project::GlobalMapEntry::eventName);
        return found == entries.end() ? std::optional<talas::project::GlobalMapEntry>{} : std::optional{*found};
    };
    const auto volume = find(entriesOf(talas::app::globalMapWithControlSource(map, "Volume", 2, 11)), "Volume");
    const auto index = find(entriesOf(talas::app::globalMapWithControlSource(map, "Index", 3, 20)), "Index");
    const auto morph = entriesOf(talas::app::globalMapWithControlSource(map, "Morph", 4, 30));
    require(volume.has_value() && volume->controller == 11 && volume->channel == std::optional<std::uint8_t>{2} && volume->minimum == 0.5
            && volume->taper == talas::project::MidiMapperMapTaper::Log && index.has_value() && index->controller == 20
            && index->channel == std::optional<std::uint8_t>{3} && morph.size() == 3 && find(morph, "Morph")->controller == 30
            && !talas::app::globalMapWithControlSource(map, "Volume", 17, 7).has_value(),
        "the channel and controller chosen should be the lane's in the global map, its other settings kept");
    talas::project::ProjectDocument mapped;
    mapped.globalMap = map;
    talas::project::ScoreAutomationLane lane;
    lane.eventName = "Volume";
    lane.takeName = "take5";
    const auto automated = talas::app::timelineLaneControlSource(mapped, lane);
    lane.state = talas::project::ScoreAutomationState::Live;
    require(automated == "take5" && talas::app::timelineLaneControlSource(mapped, lane) == "Ch 5: cc 7",
        "a live lane's source of control should be its MIDI channel and controller, an automated one's its take");
    // The right edge of a Sound drags its end, the left edge
    // trims its start, the middle moves it.
    require(talas::app::timelineClipGesture(305.0, 100.0, 300.0) == talas::app::TimelineClipGesture::ResizeEnd
            && talas::app::timelineClipGesture(96.0, 100.0, 300.0) == talas::app::TimelineClipGesture::TrimStart
            && talas::app::timelineClipGesture(200.0, 100.0, 300.0) == talas::app::TimelineClipGesture::Move
            && talas::app::timelineClipGesture(310.0, 100.0, 300.0) == talas::app::TimelineClipGesture::Move,
        "a press by a Sound's right edge should drag its end");
    const auto cursor = talas::app::timelineTimeCursorColour();
    require(((cursor >> 16) & 0xffu) > 0xe0u && ((cursor >> 8) & 0xffu) > 0xc0u && (cursor & 0xffu) < 0x80u,
        "the time cursor should be yellow");
    const auto gray = talas::app::timelineCachedSoundColour(TimelineCacheLook::Cached);
    require(!talas::app::timelineCachedSoundColour(TimelineCacheLook::Live).has_value()
            && talas::app::timelineCachedSoundColour(TimelineCacheLook::Caching)
                == talas::app::timelineCacheMenuColour(TimelineCacheLook::Caching)
            && gray.has_value() && ((*gray >> 16) & 0xffu) == (*gray & 0xffu) && ((*gray >> 8) & 0xffu) == (*gray & 0xffu),
        "a Sound being cached should turn red with the doughnut and a cached one gray");
    // The parameter names of a Sound being cached
    // turn red with it; a track's lanes and those of a Sound at rest keep
    // their colour.
    talas::project::ScoreAutomationLane soundLane;
    soundLane.scope = talas::project::ScoreAutomationScope::Clip;
    auto trackLane = soundLane;
    trackLane.scope = talas::project::ScoreAutomationScope::Track;
    require(talas::app::timelineLaneNameColour(soundLane, TimelineCacheLook::Caching)
                == talas::app::timelineCacheMenuColour(TimelineCacheLook::Caching)
            && !talas::app::timelineLaneNameColour(soundLane, TimelineCacheLook::Live).has_value()
            && !talas::app::timelineLaneNameColour(soundLane, TimelineCacheLook::Cached).has_value()
            && !talas::app::timelineLaneNameColour(trackLane, TimelineCacheLook::Caching).has_value(),
        "a Sound's parameter names should turn red while it is cached, and only then");
    using Item = talas::app::TimelineCacheMenuItem;
    require(talas::app::timelineCacheMenuItems(TimelineCacheLook::Live, true)
                == std::vector<Item>{{"Live", false, true}, {"Cache", true, false}, {"Re-cache", false, false}}
            && talas::app::timelineCacheMenuItems(TimelineCacheLook::Cached, true)
                == std::vector<Item>{{"Live", true, false}, {"Cache", false, true}, {"Re-cache", true, false}}
            && !talas::app::timelineCacheMenuItems(TimelineCacheLook::Live, false)[1].enabled,
        "Cache should cache a live Sound, Live and Re-cache a cached one");
}

// A Sound dropped below the last track makes a new
// track for it; on a track it goes there, in a lane it controls the lane.
void testASoundDroppedBelowTheTracksMakesANewOne()
{
    using talas::app::TimelineDropTarget;
    require(talas::app::timelineDropTarget(4, false, false, 4) == TimelineDropTarget::NewTrack
            && talas::app::timelineDropTarget(40, false, false, 4) == TimelineDropTarget::NewTrack
            && talas::app::timelineDropTarget(1, true, false, 4) == TimelineDropTarget::Track
            && talas::app::timelineDropTarget(2, false, true, 4) == TimelineDropTarget::Lane
            && talas::app::timelineDropTarget(std::nullopt, false, false, 4) == TimelineDropTarget::None
            && talas::app::timelineDropTarget(0, false, false, 4) == TimelineDropTarget::None,
        "a Sound should go onto its track, into its lane, or onto a new track below the last");
}

// The start field's left edge, the anchor's glow,
// the duration's top and sides and the end's right edge are coloured, and
// each field has help with the pointer in it.
void testTheTimeFieldsAreColouredAndHelped()
{
    using talas::app::TimelineTimeField;
    using Border = talas::app::TimelineTimeFieldBorder;
    require(talas::app::timelineTimeFieldBorder(TimelineTimeField::Start) == Border{true, false, false, false}
            && talas::app::timelineTimeFieldBorder(TimelineTimeField::Anchor) == Border{true, true, true, true}
            && talas::app::timelineTimeFieldBorder(TimelineTimeField::Duration) == Border{true, true, true, false}
            && talas::app::timelineTimeFieldBorder(TimelineTimeField::End) == Border{false, true, false, false},
        "the start's left, the anchor all round, the duration's top and sides, the end's right edge should be coloured");
    for (const auto field : {TimelineTimeField::Start, TimelineTimeField::Anchor, TimelineTimeField::Duration, TimelineTimeField::End})
        require(talas::app::timelineTimeFieldHelp(field).find("press Enter") != std::string_view::npos,
            "each time field should tell what it holds and how to change it");
}

// The marker name field takes no spaces, Space
// playing and pausing there; the Large window (Ctrl+L) takes a name with
// spaces into it.
void testMarkerNamesTakeSpacesInTheLargeWindowOnly()
{
    require(talas::app::timelineMarkerNameTyped("Verse two") == "Versetwo" && talas::app::timelineMarkerNameTyped(" ").empty()
            && talas::app::timelineMarkerNameTyped("Coda") == "Coda",
        "typing should put no spaces into a marker's name");
    require(talas::app::largeWindowResult({"Coda", "Verse two"}, true) == "Verse two",
        "the larger window should give the name its spaces");
}

// A Timeline is a Sound, a Mixer of SetDurations,
// that plays as the Timeline does; a muted track's clips are left out.
void testATimelineIsASound()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto tone = [](std::string id, std::string frequency, std::string amplitude) {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = id;
        sound.graph.nodes.push_back({"osc", "talas.oscillator", {{"frequency", std::move(frequency)}, {"amplitude", std::move(amplitude)}}});
        sound.outputs.push_back({"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
        return sound;
    };
    talas::project::ProjectDocument document;
    document.soundDefinitions = {tone("voice", "1000 hz", "0.5"), tone("bell", "700 hz", "0.25")};
    const auto track = [](std::string id) {
        return talas::project::ScoreTrack{id, id, talas::project::ScoreTrackKind::Track, ScoreTimeDomain::Seconds, "master", 1,
            talas::project::ScoreOverlapMode::Layer};
    };
    document.timeline.score.tracks = {track("t1"), track("t2"),
        {"master", "Master", talas::project::ScoreTrackKind::Master, ScoreTimeDomain::Seconds, {}, 1,
            talas::project::ScoreOverlapMode::Layer}};
    document.timeline.score.clips = {
        {"a", "t1", "voice", seconds(0.01), seconds(0.02), seconds(0.0), seconds(0.0), seconds(0.0), 1},
        {"b", "t2", "bell", seconds(0.02), seconds(0.02), seconds(0.0), seconds(0.0), seconds(0.0), 1}};
    const auto render = [&](const talas::project::ProjectDocument& played) {
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 48000.0;
        settings.maxBlockFrames = 64;
        const auto compiled = talas::runtime::compileProject(played, registry, std::move(settings));
        require(compiled.ok(), "the Timeline and its Sound should compile");
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 2400);
        require(offline.ok(), "the Timeline and its Sound should render");
        return offline.mono;
    };
    const auto asSound = [&](const talas::project::ProjectDocument& timeline) {
        const auto sound = talas::app::timelineAsSound(timeline, "Timeline");
        require(sound.has_value(), "a Timeline should have its equivalent Sound");
        auto played = timeline;
        played.timeline = {};
        played.soundDefinitions.push_back(*sound);
        played.graph.nodes = {{"timeline", {}, {}, std::string{"Timeline"}, {}}, {"out", "talas.audioOutput", {}}};
        played.graph.connections = {{"timeline", "audioOut", "out", "audioIn"}};
        talas::project::reconcileProjectPresentation(played);
        return render(played);
    };
    const auto timeline = render(document);
    const auto sound = asSound(document);
    require(timeline.size() == sound.size(), "the Timeline and its Sound should last alike here");
    bool heard = false;
    for (std::size_t frame = 0; frame < timeline.size(); ++frame)
    {
        require(std::abs(timeline[frame] - sound[frame]) < 1.0e-9,
            "the Timeline's Sound, a Mixer of SetDurations, should play as the Timeline, at frame "
                + std::to_string(frame));
        heard = heard || std::abs(timeline[frame]) > 0.1;
    }
    require(heard, "the Timeline should be heard");
    auto muted = document;
    muted.timeline.score.tracks[1].muted = true;
    const auto mutedSound = talas::app::timelineAsSound(muted, "Timeline");
    require(mutedSound.has_value() && std::ranges::count(mutedSound->graph.nodes, std::string{"talas.setDuration"},
                                          &talas::project::ProjectNode::typeId) == 1,
        "a muted track's clips should be left out of the Timeline's Sound");
    require(!talas::app::timelineAsSound(talas::project::ProjectDocument{}, "Timeline").has_value(),
        "a Timeline without a Sound to play should have none");
}

// Copy and Paste in a Timeline: the clips
// copied with their own controls, pasted on a track at a time, the
// earliest there and the rest keeping their offsets.
void testCopyAndPasteSounds()
{
    talas::project::ProjectScore score;
    score.tracks = {{"t1", "t1", talas::project::ScoreTrackKind::Track, ScoreTimeDomain::Seconds, "master", 1,
                        talas::project::ScoreOverlapMode::Layer},
        {"t3", "t3", talas::project::ScoreTrackKind::Track, ScoreTimeDomain::Seconds, "master", 1, talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master, ScoreTimeDomain::Seconds, {}, 1,
            talas::project::ScoreOverlapMode::Layer}};
    score.clips = {{"a", "t1", "voice", seconds(1.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1},
        {"b", "t1", "voice", seconds(4.0), seconds(1.0), seconds(0.0), seconds(0.0), seconds(0.0), 1}};
    score.automation = {{"aGain", talas::project::ScoreAutomationScope::Clip, "a", "Gain", talas::expr::ValueDomain::Scalar,
        {{seconds(0.0), 0.5, talas::core::AutomationInterpolation::Step}}}};
    const auto copied = talas::app::timelineClipsCopied(score, {"a", "b"});
    require(copied.clips.size() == 2 && copied.lanes.size() == 1, "Copy should take the Sounds with their own controls");
    require(talas::app::timelineCopiedSounds(copied) == std::vector<std::string>{"voice"},
        "the Sounds copied in a Timeline should paste into a Sound file window, each once");
    const auto pasted = talas::app::timelineClipsPasted(score, copied, "t3", seconds(10.0));
    require(pasted.has_value() && pasted->clips.size() == 2 && pasted->clips[0].clipId == "aCopy" && pasted->clips[0].trackId == "t3"
            && pasted->clips[0].start == seconds(10.0) && pasted->clips[1].start == seconds(13.0) && pasted->automation.size() == 1
            && pasted->automation[0].ownerId == "aCopy" && pasted->automation[0].laneId == "aGainCopy",
        "Paste should put the Sounds on the track clicked, the earliest at the time clicked, their controls with them");
    require(!talas::app::timelineClipsPasted(score, copied, "master", seconds(10.0)).has_value()
            && !talas::app::timelineClipsPasted(score, {}, "t3", seconds(10.0)).has_value()
            && !talas::app::timelineClipsPasted(score, copied, "t3", ticks(960)).has_value(),
        "nothing should paste onto the master, from an empty copy, or at a time of another domain");
}

// A Timeline's project: a track and the master, and the Sounds given.
talas::project::ProjectDocument timelineProject(std::vector<talas::project::ProjectSoundDefinition> sounds)
{
    talas::project::ProjectDocument project;
    project.soundDefinitions = std::move(sounds);
    project.timeline.score.tracks = {{"t1", "Trk 1", talas::project::ScoreTrackKind::Track, ScoreTimeDomain::Seconds, "master", 2,
                                         talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master, ScoreTimeDomain::Seconds, {}, 2,
            talas::project::ScoreOverlapMode::Layer}};
    return project;
}

talas::project::ProjectSoundDefinition soundOf(std::string id, std::string typeId, std::string field, std::string value)
{
    talas::project::ProjectSoundDefinition sound;
    sound.definitionId = id;
    sound.displayName = id;
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(field);
    parameter.expression = std::move(value);
    sound.graph.nodes.push_back({"sound", std::move(typeId), {parameter}});
    sound.outputs.push_back({"audioOut", talas::core::PortType::Audio, "sound", "audioOut"});
    return sound;
}

// Sounds move from one Timeline to another by
// Copy and Paste, each bringing the Sounds it plays; a drag only moves a
// Sound to a new time and track within its own Timeline.
void testSoundsMoveBetweenTimelinesByCopyAndPaste()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    // The first Timeline: voice, an oscillator at !Gain, and pad, voice
    // through a VCA; voice's Gain slaved to the master control GainMaster.
    auto voice = soundOf("voice", "talas.oscillator", "amplitude", "!Gain");
    auto pad = soundOf("pad", "talas.vca", "gain", "0.5");
    talas::project::ProjectNode inner;
    inner.instanceId = "inner";
    inner.soundDefinitionId = "voice";
    pad.graph.nodes.push_back(inner);
    pad.graph.connections.push_back({"inner", "audioOut", "sound", "audioIn"});
    auto first = timelineProject({voice, pad});
    first.timeline.score.clips = {{"a", "t1", "voice", seconds(1.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1},
        {"b", "t1", "pad", seconds(4.0), seconds(1.0), seconds(0.0), seconds(0.0), seconds(0.0), 1}};
    first.timeline.score.automation = {
        {"gainMaster", talas::project::ScoreAutomationScope::Master, "master", "GainMaster", talas::expr::ValueDomain::Scalar,
            {{seconds(1.0), 0.25, talas::core::AutomationInterpolation::Linear},
                {seconds(3.0), 0.75, talas::core::AutomationInterpolation::Linear}}},
        {"aGain", talas::project::ScoreAutomationScope::Clip, "a", "Gain", talas::expr::ValueDomain::Scalar,
            {{seconds(0.0), 0.5, talas::core::AutomationInterpolation::Step}}}};
    first.timeline.score.automation[1].masterLaneId = "gainMaster";

    // The other Timeline has a different Sound named voice of its own.
    const auto other = soundOf("voice", "talas.noise", "amplitude", "0.1");
    const auto second = timelineProject({other});

    const auto copied = talas::app::timelineClipsCopied(first.timeline.score, {"a", "b"});
    const auto pasted = talas::app::timelineClipsPastedFromAnotherTimeline(second, first, copied, "t1", seconds(10.0), registry);
    require(pasted.ok(), "Sounds copied in one Timeline should paste into another: " + pasted.diagnostic);
    const auto& moved = *pasted.document;
    require(moved.timeline.score.clips.size() == 2 && moved.timeline.score.clips[0].trackId == "t1"
            && moved.timeline.score.clips[0].start == seconds(10.0) && moved.timeline.score.clips[1].start == seconds(13.0),
        "Paste should put them on the track at the time, keeping their offsets");
    const auto definition = [&](const std::string& id) {
        const auto found = std::ranges::find(moved.soundDefinitions, id, &talas::project::ProjectSoundDefinition::definitionId);
        require(found != moved.soundDefinitions.end(), "the pasted clip's Sound " + id + " should be in the Timeline's project");
        return *found;
    };
    const auto voiceHere = definition(moved.timeline.score.clips[0].soundDefinitionId);
    const auto padHere = definition(moved.timeline.score.clips[1].soundDefinitionId);
    require(voiceHere.definitionId != "voice" && voiceHere.graph == voice.graph && definition("voice") == other,
        "the Sound comes with the clip, renamed beside the other Timeline's own voice, which stays itself");
    require(padHere.graph.nodes.size() == 2 && padHere.graph.nodes[1].soundDefinitionId == voiceHere.definitionId,
        "the Sounds a pasted Sound plays come with it, under the same new names");
    require(pasted.broughtSounds.size() == 2 && moved.soundDefinitions.size() == 3,
        "the two Sounds come in, once each: " + std::to_string(pasted.broughtSounds.size()));
    const auto lane = std::ranges::find(moved.timeline.score.automation, "Gain", &talas::project::ScoreAutomationLane::eventName);
    require(lane != moved.timeline.score.automation.end() && lane->ownerId == moved.timeline.score.clips[0].clipId
            && lane->masterLaneId.empty() && lane->points.size() == 2 && lane->points[0].position == seconds(0.0)
            && lane->points[0].value == 0.25 && lane->points[1].position == seconds(2.0) && lane->points[1].value == 0.75,
        "a control slaved to a master the other Timeline lacks keeps a copy of the master's function, in its own time: "
            + std::to_string(lane == moved.timeline.score.automation.end() ? 0 : lane->points.size()));
    require(first.timeline.score.clips.size() == 2 && first.soundDefinitions.size() == 2, "the first Timeline keeps its Sounds");

    // Within one Timeline nothing comes in: the clips paste as before.
    const auto again = talas::app::timelineClipsPastedFromAnotherTimeline(first, first, copied, "t1", seconds(10.0), registry);
    const auto before = talas::app::timelineClipsPasted(first.timeline.score, copied, "t1", seconds(10.0));
    require(again.ok() && again.broughtSounds.empty() && again.document->soundDefinitions == first.soundDefinitions
            && before.has_value() && again.batch.clips == before->clips && again.batch.automation == before->automation,
        "within one Timeline Paste should bring no Sounds, pasting the clips as it did");
    require(!talas::app::timelineClipsPastedFromAnotherTimeline(second, first, copied, "master", seconds(10.0), registry).ok()
            && !talas::app::timelineClipsPastedFromAnotherTimeline(second, first, {}, "t1", seconds(10.0), registry).ok(),
        "nothing pastes onto the master or from an empty copy");

    // A drag is a move to another time and track of the same
    // Timeline: the clip's place changes, the project's Sounds do not.
    talas::project::ProjectEditor editor(moved, registry);
    require(editor.addScoreTrack({"t2", "Trk 2", talas::project::ScoreTrackKind::Track, ScoreTimeDomain::Seconds, "master", 2,
                                    talas::project::ScoreOverlapMode::Layer})
                .ok(),
        "add a second track");
    auto dragged = editor.document().timeline.score.clips[0];
    dragged.trackId = "t2";
    dragged.start = seconds(20.0);
    require(editor.updateScoreClip(dragged).ok() && editor.document().soundDefinitions == moved.soundDefinitions
            && editor.document().timeline.score.clips.size() == 2 && editor.document().timeline.score.clips[0].trackId == "t2"
            && editor.document().timeline.score.clips[0].start == seconds(20.0),
        "a dragged Sound moves to a new time and track within its own Timeline");
}

// A Sound dragged into a Timeline takes a
// polyphony there, the notes it plays at once, and its track's MIDI input
// channel.
void testASoundsPolyphonyAndMidiChannelInTheTimeline()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto keys = soundOf("keys", "talas.oscillator", "frequency", "!KeyPitch");
    keys.graph.nodes.front().parameters.push_back({"amplitude", std::string{"!KeyDown * !KeyVelocity"}});
    const auto drone = soundOf("drone", "talas.oscillator", "amplitude", "0.25");
    auto project = timelineProject({keys, drone});
    project.timeline.score.clips = {{"a", "t1", "keys", seconds(0.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1},
        {"b", "t1", "keys", seconds(3.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1},
        {"c", "t1", "drone", seconds(6.0), seconds(2.0), seconds(0.0), seconds(0.0), seconds(0.0), 1}};
    require(!talas::app::timelineClipPolyphony(project, "a").has_value(), "a Sound dropped in plays as it is at first");

    // Four voices for clip a; its MIDI input channel is its track's.
    const auto four = talas::app::timelineWithClipPolyphony(project, "a", 4U);
    require(four.ok(), "the Timeline should set a Sound's polyphony: " + four.diagnostic);
    talas::project::ProjectEditor editor(project, registry);
    require(editor.applyDocumentEdit(*four.document).ok(), "the polyphony is a valid edit of the project");
    const auto& played = editor.document();
    require(talas::app::timelineClipPolyphony(played, "a") == std::optional<unsigned>{4U}
            && !talas::app::timelineClipPolyphony(played, "b").has_value(),
        "the polyphony is the clip's own, the other clip of the Sound keeping none");
    const auto clip = std::ranges::find(played.timeline.score.clips, std::string{"a"}, &talas::project::ScoreClip::clipId);
    const auto wrapper = std::ranges::find(played.soundDefinitions, clip->soundDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
    require(wrapper != played.soundDefinitions.end() && wrapper->displayName == "keys voices"
            && std::ranges::any_of(wrapper->graph.nodes, [](const talas::project::ProjectNode& node) { return node.soundDefinitionId == std::string{"keys"}; }),
        "the polyphony plays the clip's Sound itself");
    const auto voice = std::ranges::find(wrapper->graph.nodes, std::string{"talas.midiVoice"}, &talas::project::ProjectNode::typeId);
    require(voice != wrapper->graph.nodes.end()
            && std::ranges::none_of(voice->parameters, [](const auto& parameter) { return parameter.parameterId == "channel"; }),
        "the voices take their notes on the default channel, the track's MIDI input channel");
    const auto wrapperId = wrapper->definitionId;
    auto track = played.timeline.score.tracks.front();
    track.midiChannel = 3;
    require(editor.updateScoreTrack(track).ok() && editor.document().timeline.score.tracks.front().midiChannel == 3,
        "the track's MIDI input channel is set in the Timeline");

    // The polyphony changed again changes the clip's own voices, in place.
    const auto six = talas::app::timelineWithClipPolyphony(editor.document(), "a", 6U);
    require(six.ok() && talas::app::timelineClipPolyphony(*six.document, "a") == std::optional<unsigned>{6U}
            && six.document->soundDefinitions.size() == editor.document().soundDefinitions.size(),
        "a second polyphony changes the clip's own voices, not voices of voices");
    // The voices play: a MIDIVoice of four copies of the Sound.
    auto heard = editor.document();
    heard.timeline = {};
    heard.graph.nodes = {{"voices", {}, {}, wrapperId, {}}, {"out", "talas.audioOutput", {}}};
    heard.graph.connections = {{"voices", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(heard);
    talas::runtime::RenderSettings settings;
    settings.eventValues = {{"KeyPitch", 60.0, talas::expr::ValueDomain::NoteNumber}, {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
        {"KeyVelocity", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto compiled = talas::runtime::compileProject(heard, registry, std::move(settings));
    require(compiled.ok(), "the voices should compile: " + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    require(compiled.graph->plan().midiVoices.size() == 1 && compiled.graph->plan().midiVoices.front().voices.size() == 4,
        "the Sound plays as four voices: " + std::to_string(compiled.graph->plan().midiVoices.size()) + " "
            + std::to_string(compiled.graph->plan().midiVoices.empty() ? 0 : compiled.graph->plan().midiVoices.front().voices.size()));
    // Back to the Sound as it is, its voices gone.
    const auto none = talas::app::timelineWithClipPolyphony(editor.document(), "a", std::nullopt);
    require(none.ok() && !talas::app::timelineClipPolyphony(*none.document, "a").has_value()
            && none.document->soundDefinitions.size() == 2,
        "with no polyphony the clip plays its Sound as it is again");
    require(!talas::app::timelineWithClipPolyphony(project, "c", 4U).ok()
            && !talas::app::timelineWithClipPolyphony(project, "a", 0U).ok()
            && !talas::app::timelineWithClipPolyphony(project, "a", talas::app::maximumTimelineClipPolyphony + 1).ok()
            && !talas::app::timelineWithClipPolyphony(project, "missing", 4U).ok(),
        "a polyphony only for a Sound that expects MIDI notes, of 1 to 128 voices");
}

// The caches go beside the Timeline's file,
// in a folder of its name with Caches.
void testCachesGoBesideTheTimeline()
{
    require(talas::app::timelineCachesFolder("/music/haiku.talas.json") == std::filesystem::path{"/music/haikuCaches"},
        "haiku's caches should be in haikuCaches beside it");
    require(talas::app::timelineCachesFolder("/music/haiku.talaspkg/Project.talas.json")
            == std::filesystem::path{"/music/haiku.talaspkg/haikuCaches"},
        "a package's caches should be named after the package, inside it");
    require(talas::app::timelineCachesFolder("/music/old.kym") == std::filesystem::path{"/music/oldCaches"},
        "another file's caches should take its name without the extension");
}

// The information strip shows the selected marker's
// name at its far left, its time after it, and the selected Sound's times
// after the cursor's.
void testTheSelectedMarkersNameIsAtTheFarLeft()
{
    using talas::app::TimelineInfoStripPart;
    const auto parts = talas::app::timelineInfoStripParts();
    const auto at = [&parts](TimelineInfoStripPart part) { return std::ranges::find(parts, part) - parts.begin(); };
    require(!parts.empty() && parts.front() == TimelineInfoStripPart::MarkerName
            && at(TimelineInfoStripPart::MarkerTime) < at(TimelineInfoStripPart::Cursor)
            && at(TimelineInfoStripPart::Cursor) < at(TimelineInfoStripPart::SoundStart)
            && at(TimelineInfoStripPart::SoundName) < at(TimelineInfoStripPart::SoundStart)
            && at(TimelineInfoStripPart::SoundEnd) < static_cast<std::ptrdiff_t>(parts.size()),
        "the selected marker's name should be at the strip's far left");
}

// Format 146: the Timeline's own time code
// frames a second override the project's, and the colour menu colours the
// selected Sounds, both kept when the project is saved.
void testTheTimelinesFramesAndColours()
{
    auto project = document();
    project.expressionDefinitions.timecodeFramesPerSecond = 30;
    require(talas::app::timelineFramesPerSecond(project) == 30, "the Timeline follows the project's frames a second at first");
    const auto own = talas::app::timelineWithFramesPerSecond(project, 25);
    require(own.has_value() && talas::app::timelineFramesPerSecond(*own) == 25
            && talas::app::timelineFramesPerSecond(*talas::app::timelineWithFramesPerSecond(*own, 0)) == 30
            && !talas::app::timelineWithFramesPerSecond(project, 29).has_value(),
        "the Timeline's own frames a second should override the project's");
    const auto colours = talas::app::timelineClipColours();
    const auto red = std::ranges::find(colours, std::string_view{"Red"}, &talas::app::TimelineClipColour::name);
    const auto coloured = red == colours.end() ? std::nullopt : talas::app::timelineWithClipColour(*own, {"clip"}, red->rgb);
    require(colours.front().name == "Track" && !colours.front().rgb.has_value() && coloured.has_value()
            && coloured->timeline.score.clips.front().colour == red->rgb
            && !talas::app::timelineWithClipColour(*own, {"missing"}, red->rgb).has_value(),
        "the colour menu should colour the selected Sounds");

    // The Audio menu sets the Timeline's number of outputs.
    auto routed = project;
    routed.timeline.score.tracks = {
        {"master", "Master", talas::project::ScoreTrackKind::Master, ScoreTimeDomain::Seconds, {}, 2,
            talas::project::ScoreOverlapMode::Layer},
        {"track", "Track", talas::project::ScoreTrackKind::Track, ScoreTimeDomain::Seconds, "master", 2,
            talas::project::ScoreOverlapMode::Layer}};
    routed.timeline.score.tracks[1].outputChannels = {1, 2};
    const auto quad = talas::app::timelineWithOutputs(routed, 4);
    const auto mono = talas::app::timelineWithOutputs(routed, 1);
    require(talas::app::timelineOutputs(routed) == 2 && quad.has_value() && talas::app::timelineOutputs(*quad) == 4
            && quad->timeline.score.tracks[1].channelCount == 4 && mono.has_value()
            && mono->timeline.score.tracks[1].outputChannels == std::vector<std::uint32_t>{1}
            && !talas::app::timelineWithOutputs(routed, 9).has_value(),
        "the Audio menu should set the number of the Timeline's outputs");

    // The Audio menu's cache file format, mono 24-bit WAVs
    // at the current sample rate by default, or 16-bit.
    const auto sixteen = talas::app::timelineWithCacheWordSize(project, 16);
    require(project.timeline.cacheWordSize == 24 && sixteen.has_value() && sixteen->timeline.cacheWordSize == 16
            && !talas::app::timelineWithCacheWordSize(project, 20).has_value()
            && talas::app::timelineCacheFormatName(24) == "Mono 24-bit WAV at the current sample rate",
        "the Audio menu should set the caches' file format");

    // The Audio menu's Share recordings across tracks, off
    // by default as in projects before format 156, on and off again.
    const auto sharing = talas::app::timelineWithMemoryWritersSharedAcrossTracks(project, true);
    require(!project.timeline.sharingMemoryWritersAcrossTracks && sharing.timeline.sharingMemoryWritersAcrossTracks
            && !talas::app::timelineWithMemoryWritersSharedAcrossTracks(sharing, false)
                    .timeline.sharingMemoryWritersAcrossTracks
            && talas::app::timelineSharingAcrossTracksName() == "Share recordings across tracks",
        "the Audio menu should share the recordings across the Timeline's tracks");
}

int main()
{
    try
    {
        testFieldsReadAndShowTimes();
        testCachesGoBesideTheTimeline();
        testTabMovesBetweenATimesUnits();
        testTheSoundsInformationIsShownAndEdited();
        testSoundsAlignToAnAnchor();
        testProgramChangesJumpToMarkers();
        testTheTempoOfASample();
        testSetMasterBpm();
        testTheMarkerDialog();
        testTheTimeCursorSource();
        testMarkedSoundsPlayAlone();
        testAGroupOfSounds();
        testTheCacheMenuShowsTheSoundsState();
        testASoundDroppedBelowTheTracksMakesANewOne();
        testTheTimeFieldsAreColouredAndHelped();
        testMarkerNamesTakeSpacesInTheLargeWindowOnly();
        testATimelineIsASound();
        testCopyAndPasteSounds();
        testSoundsMoveBetweenTimelinesByCopyAndPaste();
        testASoundsPolyphonyAndMidiChannelInTheTimeline();
        testTheSelectedMarkersNameIsAtTheFarLeft();
        testTheTimelinesFramesAndColours();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
