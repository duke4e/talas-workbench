#include "talas/app/TimelineGrid.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ranges>

namespace talas::app
{
namespace
{
using talas::project::ScoreTime;
using talas::project::ScoreTimeDomain;

[[nodiscard]] double valueOf(const ScoreTime& time) noexcept
{
    return time.domain == ScoreTimeDomain::Seconds ? time.seconds : static_cast<double>(time.tick);
}

[[nodiscard]] ScoreTime timeOf(ScoreTimeDomain domain, double value) noexcept
{
    value = std::max(0.0, value);
    if (domain == ScoreTimeDomain::Seconds)
        return {domain, value, 0};
    return {domain, 0.0, static_cast<std::uint64_t>(std::llround(value))};
}

[[nodiscard]] ScoreTime clipEnd(const talas::project::ScoreClip& clip) noexcept
{
    return timeOf(clip.start.domain, valueOf(clip.start) + valueOf(clip.duration) * clip.repeatCount);
}

[[nodiscard]] double secondsPerQuarter(const talas::project::ProjectMusicalTime& musicalTime) noexcept
{
    const auto tempo = musicalTime.tempo.empty() ? talas::project::defaultMicrosecondsPerQuarter
                                                 : musicalTime.tempo.front().microsecondsPerQuarter;
    return static_cast<double>(tempo) / 1'000'000.0;
}

[[nodiscard]] double quartersPerBeat(const talas::project::ProjectMusicalTime& musicalTime) noexcept
{
    const auto denominator = musicalTime.meter.empty() ? 4U : std::max<unsigned>(1, musicalTime.meter.front().denominator);
    return 4.0 / static_cast<double>(denominator);
}
}

std::string_view timelineGridModeName(TimelineGridMode mode) noexcept
{
    switch (mode)
    {
    case TimelineGridMode::Free: return "Free grid";
    case TimelineGridMode::Quantized: return "Quantized grid";
    case TimelineGridMode::Magnet: return "Magnet grid";
    case TimelineGridMode::EnterTime: return "Enter beat or time";
    }
    return "Free grid";
}

bool timelineGridTypesTimes(TimelineGridMode mode) noexcept
{
    return mode == TimelineGridMode::EnterTime;
}

std::vector<TimelineQuantization> timelineQuantizationChoices()
{
    return {{1, 1}, {1, 2}, {1, 3}, {1, 4}, {1, 6}, {1, 8}, {1, 12}, {1, 16}, {1, 32}, {1, 64}, {1, 100}};
}

std::string timelineQuantizationText(const TimelineQuantization& quantization)
{
    return std::to_string(quantization.numerator) + "/" + std::to_string(quantization.denominator);
}

std::optional<TimelineQuantization> parseTimelineQuantization(std::string_view text)
{
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    const auto slash = text.find('/');
    const auto number = [](std::string_view part, std::uint32_t& value) {
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == part.data() + part.size() && value > 0 && value <= 1'000'000U;
    };
    TimelineQuantization result{1, 1};
    if (slash == std::string_view::npos ? !number(text, result.numerator)
                                        : !number(text.substr(0, slash), result.numerator)
                || !number(text.substr(slash + 1), result.denominator))
        return std::nullopt;
    return result;
}

ScoreTime timelineTimeUnit(TimelineRulerMode units, ScoreTimeDomain domain,
    const talas::project::ProjectMusicalTime& musicalTime, double framesPerSecond)
{
    const auto fps = std::isfinite(framesPerSecond) && framesPerSecond > 0.0 ? framesPerSecond : 30.0;
    const auto beatSeconds = secondsPerQuarter(musicalTime) * quartersPerBeat(musicalTime);
    const auto seconds = units == TimelineRulerMode::Smpte ? 1.0 / fps
        : units == TimelineRulerMode::BarsBeatsTicks       ? beatSeconds
                                                           : 1.0;
    if (domain == ScoreTimeDomain::Seconds)
        return {domain, seconds, 0};
    const auto ticksPerQuarter = static_cast<double>(std::max<std::uint32_t>(1, musicalTime.ticksPerQuarter));
    const auto ticks = units == TimelineRulerMode::BarsBeatsTicks ? ticksPerQuarter * quartersPerBeat(musicalTime)
                                                                  : ticksPerQuarter * seconds / secondsPerQuarter(musicalTime);
    return {domain, 0.0, std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(ticks)))};
}

ScoreTime timelineQuantum(const TimelineQuantization& quantization, TimelineRulerMode units, ScoreTimeDomain domain,
    const talas::project::ProjectMusicalTime& musicalTime, double framesPerSecond)
{
    const auto unit = timelineTimeUnit(units, domain, musicalTime, framesPerSecond);
    const auto fraction = static_cast<double>(std::max<std::uint32_t>(1, quantization.numerator))
        / static_cast<double>(std::max<std::uint32_t>(1, quantization.denominator));
    if (domain == ScoreTimeDomain::Seconds)
        return {domain, unit.seconds * fraction, 0};
    return {domain, 0.0,
        std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(static_cast<double>(unit.tick) * fraction)))};
}

std::string timelineMinutesSecondsText(double seconds)
{
    const auto milliseconds = static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) * 1000.0));
    const auto minutes = milliseconds / 60'000;
    const auto rest = milliseconds % 60'000;
    char text[32];
    std::snprintf(text, sizeof(text), "%lld:%02lld.%03lld", static_cast<long long>(minutes),
        static_cast<long long>(rest / 1000), static_cast<long long>(rest % 1000));
    return text;
}

double timelineZoomedOut(double zoom) noexcept
{
    return std::clamp(zoom * 0.8, timelineMinimumZoom, timelineMaximumZoom);
}

double timelineZoomedIn(double zoom) noexcept
{
    return std::clamp(zoom * 1.25, timelineMinimumZoom, timelineMaximumZoom);
}

double timelineZoomToFit() noexcept
{
    return 1.0;
}

double timelineVisibleLength(double timelineLength, double zoom) noexcept
{
    return timelineLength / std::clamp(zoom, timelineMinimumZoom, timelineMaximumZoom);
}

ScoreTime timelineQuantized(const ScoreTime& time, const ScoreTime& quantum)
{
    const auto step = valueOf(quantum);
    if (time.domain != quantum.domain || !(step > 0.0))
        return time;
    return timeOf(time.domain, std::round(valueOf(time) / step) * step);
}

ScoreTime timelineMagnetStart(const talas::project::ProjectScore& score, std::string_view trackId,
    const ScoreTime& dropped, std::string_view movingClipId)
{
    const talas::project::ScoreClip* previous = nullptr;
    for (const auto& clip : score.clips)
    {
        if (clip.trackId != trackId || clip.clipId == movingClipId || clip.start.domain != dropped.domain
            || valueOf(clip.start) > valueOf(dropped))
            continue;
        if (previous == nullptr || valueOf(clip.start) > valueOf(previous->start))
            previous = &clip;
    }
    return previous == nullptr ? timeOf(dropped.domain, 0.0) : clipEnd(*previous);
}

std::optional<std::string> timelineTrackForMove(const talas::project::ProjectScore& score, std::string_view clipId, int direction)
{
    const auto clip = std::ranges::find(score.clips, clipId, &talas::project::ScoreClip::clipId);
    if (clip == score.clips.end() || direction == 0)
        return std::nullopt;
    const auto from = std::ranges::find(score.tracks, clip->trackId, &talas::project::ScoreTrack::trackId);
    if (from == score.tracks.end())
        return std::nullopt;
    const auto start = valueOf(clip->start);
    const auto end = valueOf(clipEnd(*clip));
    const auto occupied = [&](const talas::project::ScoreTrack& track) {
        return std::ranges::any_of(score.clips, [&](const talas::project::ScoreClip& other) {
            return other.trackId == track.trackId && other.clipId != clip->clipId
                && other.start.domain == clip->start.domain && valueOf(other.start) < end
                && valueOf(clipEnd(other)) > start;
        });
    };
    auto index = static_cast<std::ptrdiff_t>(from - score.tracks.begin());
    for (index += direction > 0 ? 1 : -1; index >= 0 && index < static_cast<std::ptrdiff_t>(score.tracks.size());
         index += direction > 0 ? 1 : -1)
    {
        const auto& track = score.tracks[static_cast<std::size_t>(index)];
        if (track.kind == talas::project::ScoreTrackKind::Track && track.channelCount == from->channelCount
            && track.timeDomain == from->timeDomain && !occupied(track))
            return track.trackId;
    }
    return std::nullopt;
}
}
