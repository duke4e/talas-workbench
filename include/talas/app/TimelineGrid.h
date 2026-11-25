#pragma once

#include "talas/app/TimelineScoreProjection.h"
#include "talas/project/ProjectScore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Timeline's time units, quantization and Grid menu: the quantum, a
// fraction of a frame, a beat or a second as the time units are; the unit
// itself; the Grid menu's Free, Quantized and Magnet grids and Enter beat or
// time; the arrows nudging by a quantum (Shift by a unit) and moving a Sound
// up or down a track, over the occupied ones.
namespace talas::app
{
enum class TimelineGridMode : std::uint8_t
{
    Free,
    Quantized,
    Magnet,
    EnterTime,
};

[[nodiscard]] std::string_view timelineGridModeName(TimelineGridMode mode) noexcept;

// Enter beat or time: a Sound put into a track
// starts at the time cursor, its Start field open for its time to be typed
// by hand.
[[nodiscard]] bool timelineGridTypesTimes(TimelineGridMode mode) noexcept;

struct TimelineQuantization
{
    std::uint32_t numerator = 1;
    std::uint32_t denominator = 16;

    bool operator==(const TimelineQuantization&) const = default;
};

// The menu's common fractions, and one typed as n/d or a whole number.
[[nodiscard]] std::vector<TimelineQuantization> timelineQuantizationChoices();
[[nodiscard]] std::string timelineQuantizationText(const TimelineQuantization& quantization);
[[nodiscard]] std::optional<TimelineQuantization> parseTimelineQuantization(std::string_view text);

// A frame, a beat or a second, in the domain given: seconds use the frame
// rate and the first tempo, ticks a beat of the first meter.
[[nodiscard]] talas::project::ScoreTime timelineTimeUnit(
    TimelineRulerMode units,
    talas::project::ScoreTimeDomain domain,
    const talas::project::ProjectMusicalTime& musicalTime,
    double framesPerSecond);
[[nodiscard]] talas::project::ScoreTime timelineQuantum(
    const TimelineQuantization& quantization,
    TimelineRulerMode units,
    talas::project::ScoreTimeDomain domain,
    const talas::project::ProjectMusicalTime& musicalTime,
    double framesPerSecond);

// Minutes and seconds, as m:ss.mmm.
[[nodiscard]] std::string timelineMinutesSecondsText(double seconds);

// The zoom buttons: the first shows more time in the window, the
// second less, and zoom to fit shows the whole Timeline; at a zoom the
// window shows the Timeline's length divided by it.
inline constexpr double timelineMinimumZoom = 0.25;
inline constexpr double timelineMaximumZoom = 16.0;
[[nodiscard]] double timelineZoomedOut(double zoom) noexcept;
[[nodiscard]] double timelineZoomedIn(double zoom) noexcept;
[[nodiscard]] double timelineZoomToFit() noexcept;
[[nodiscard]] double timelineVisibleLength(double timelineLength, double zoom) noexcept;

// A Quantized grid places a time on the nearest quantum.
[[nodiscard]] talas::project::ScoreTime timelineQuantized(
    const talas::project::ScoreTime& time, const talas::project::ScoreTime& quantum);

// A Magnet grid starts a Sound dropped on a track where the Sound before
// it there ends; the first Sound on a track starts at zero.
[[nodiscard]] talas::project::ScoreTime timelineMagnetStart(
    const talas::project::ProjectScore& score,
    std::string_view trackId,
    const talas::project::ScoreTime& dropped,
    std::string_view movingClipId = {});

// The track a Sound moves to with the up or down arrow: the next Sound
// track that way with room for it, none at the end.
[[nodiscard]] std::optional<std::string> timelineTrackForMove(
    const talas::project::ProjectScore& score, std::string_view clipId, int direction);
}
