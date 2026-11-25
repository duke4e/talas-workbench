#include "talas/app/TimelineGrid.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// Time units and quantization, the Grid menu, and
// the arrow keys nudging and changing tracks.
namespace
{
using talas::app::TimelineRulerMode;
using talas::project::ScoreTimeDomain;

void require(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error(std::string(message));
}

talas::project::ScoreTime seconds(double value)
{
    return {ScoreTimeDomain::Seconds, value, 0};
}

talas::project::ScoreTime ticks(std::uint64_t value)
{
    return {ScoreTimeDomain::MusicalTicks, 0.0, value};
}

bool near(double left, double right)
{
    return std::abs(left - right) < 1e-12;
}

void testQuantization()
{
    const talas::project::ProjectMusicalTime musical;
    const auto sixteenth = talas::app::TimelineQuantization{1, 16};
    require(near(talas::app::timelineQuantum(sixteenth, TimelineRulerMode::Seconds, ScoreTimeDomain::Seconds, musical, 30).seconds,
                 1.0 / 16),
        "the quantum is a fraction of a second");
    require(near(talas::app::timelineQuantum({1, 2}, TimelineRulerMode::Smpte, ScoreTimeDomain::Seconds, musical, 30).seconds,
                 1.0 / 60),
        "or a fraction of a frame");
    require(talas::app::timelineQuantum(sixteenth, TimelineRulerMode::BarsBeatsTicks, ScoreTimeDomain::MusicalTicks, musical, 30)
                    .tick
                == 60
            && near(talas::app::timelineQuantum(sixteenth, TimelineRulerMode::BarsBeatsTicks, ScoreTimeDomain::Seconds, musical, 30)
                        .seconds,
                0.5 / 16),
        "or a fraction of a beat, at 120 BPM in seconds");
    const auto choices = talas::app::timelineQuantizationChoices();
    require(choices.size() > 5 && std::ranges::find(choices, sixteenth) != choices.end(),
        "several commonly used fractions are supplied in the menu");
    require(talas::app::parseTimelineQuantization(" 1/7 ") == talas::app::TimelineQuantization{1, 7}
            && talas::app::parseTimelineQuantization("2") == talas::app::TimelineQuantization{2, 1}
            && !talas::app::parseTimelineQuantization("1/0").has_value() && !talas::app::parseTimelineQuantization("x").has_value()
            && talas::app::timelineQuantizationText({1, 7}) == "1/7",
        "or you can type in your own arbitrarily small fraction");
}

void testTheGridMenu()
{
    require(talas::app::timelineGridModeName(talas::app::TimelineGridMode::Free) == "Free grid"
            && talas::app::timelineGridModeName(talas::app::TimelineGridMode::Magnet) == "Magnet grid"
            && talas::app::timelineGridModeName(talas::app::TimelineGridMode::EnterTime) == "Enter beat or time",
        "the Grid menu button shows the current grid mode's name");
    require(talas::app::timelineGridTypesTimes(talas::app::TimelineGridMode::EnterTime)
            && !talas::app::timelineGridTypesTimes(talas::app::TimelineGridMode::Free)
            && !talas::app::timelineGridTypesTimes(talas::app::TimelineGridMode::Quantized)
            && !talas::app::timelineGridTypesTimes(talas::app::TimelineGridMode::Magnet),
        "Enter beat or time alone has a Sound's times typed in by hand");
    require(near(talas::app::timelineQuantized(seconds(1.03), seconds(0.25)).seconds, 1.0)
            && talas::app::timelineQuantized(ticks(130), ticks(60)).tick == 120,
        "a Quantized grid places objects on the quantization times only");

    talas::project::ProjectScore score;
    score.tracks = {{"trk1", "Trk 1", talas::project::ScoreTrackKind::Track, ScoreTimeDomain::Seconds, "master", 2,
        talas::project::ScoreOverlapMode::Reject}};
    require(talas::app::timelineMagnetStart(score, "trk1", seconds(3.2)) == seconds(0.0),
        "the first Sound dropped into a track jumps to time zero");
    score.clips = {{"a", "trk1", "s", seconds(0.0), seconds(2.0), seconds(0), seconds(0), seconds(0), 2},
        {"b", "trk1", "s", seconds(10.0), seconds(1.0), seconds(0), seconds(0), seconds(0), 1}};
    require(talas::app::timelineMagnetStart(score, "trk1", seconds(5.5)) == seconds(4.0)
            && talas::app::timelineMagnetStart(score, "trk1", seconds(12.0)) == seconds(11.0),
        "a Magnet grid makes a Sound stick to the previous Sound");
}

void testArrowKeys()
{
    const talas::project::ProjectMusicalTime musical;
    require(near(talas::app::timelineTimeUnit(TimelineRulerMode::Smpte, ScoreTimeDomain::Seconds, musical, 25).seconds, 0.04)
            && near(talas::app::timelineTimeUnit(TimelineRulerMode::BarsBeatsTicks, ScoreTimeDomain::Seconds, musical, 25).seconds, 0.5)
            && talas::app::timelineTimeUnit(TimelineRulerMode::BarsBeatsTicks, ScoreTimeDomain::MusicalTicks, musical, 25).tick == 960
            && near(talas::app::timelineTimeUnit(TimelineRulerMode::Seconds, ScoreTimeDomain::Seconds, musical, 25).seconds, 1.0),
        "Shift and an arrow move by one frame, one beat or one second");

    talas::project::ProjectScore score;
    const auto track = [](std::string id) {
        return talas::project::ScoreTrack{id, id, talas::project::ScoreTrackKind::Track, ScoreTimeDomain::Seconds, "master", 2,
            talas::project::ScoreOverlapMode::Reject};
    };
    score.tracks = {track("trk1"), track("trk2"), track("trk3"),
        {"master", "Master", talas::project::ScoreTrackKind::Master, ScoreTimeDomain::Seconds, {}, 2,
            talas::project::ScoreOverlapMode::Reject}};
    score.clips = {{"a", "trk1", "s", seconds(1.0), seconds(2.0), seconds(0), seconds(0), seconds(0), 1},
        {"b", "trk2", "s", seconds(2.0), seconds(2.0), seconds(0), seconds(0), seconds(0), 1}};
    require(talas::app::timelineTrackForMove(score, "a", 1) == std::optional<std::string>{"trk3"},
        "an occupied track is jumped over to the next one with room");
    require(talas::app::timelineTrackForMove(score, "b", -1) == std::optional<std::string>{},
        "the up arrow moves the Sound up a track, none when the way is full");
    score.clips[0].start = seconds(5.0);
    require(talas::app::timelineTrackForMove(score, "a", 1) == std::optional<std::string>{"trk2"},
        "the down arrow moves the selected Sound down a track");
}

// The zoom buttons; minutes and seconds.
void testZoomAndMinutesSeconds()
{
    require(talas::app::timelineVisibleLength(60.0, talas::app::timelineZoomedOut(1.0)) > 60.0,
        "the first button zooms out to show more time in the window");
    require(talas::app::timelineVisibleLength(60.0, talas::app::timelineZoomedIn(1.0)) < 60.0,
        "the second shows less time in the window");
    require(talas::app::timelineVisibleLength(60.0, talas::app::timelineZoomToFit()) == 60.0
            && talas::app::timelineZoomedOut(talas::app::timelineMinimumZoom) == talas::app::timelineMinimumZoom,
        "zoom to fit shows the entire Timeline without scrolling");
    require(talas::app::timelineMinutesSecondsText(62.5) == "1:02.500" && talas::app::timelineMinutesSecondsText(0.0) == "0:00.000",
        "time in minutes and seconds");
    const talas::project::ProjectMusicalTime musical;
    require(talas::app::timelineTimeUnit(TimelineRulerMode::MinutesSeconds, ScoreTimeDomain::Seconds, musical, 30)
                == talas::app::timelineTimeUnit(TimelineRulerMode::Seconds, ScoreTimeDomain::Seconds, musical, 30),
        "the groupings for Minutes/Seconds and Seconds are always the same");
}
}

int main()
{
    try
    {
        testQuantization();
        testTheGridMenu();
        testArrowKeys();
        testZoomAndMinutesSeconds();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
