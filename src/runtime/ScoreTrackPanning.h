#pragma once

#include "talas/project/SpeakerPlacement.h"
#include "talas/runtime/PreparedScore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// A Timeline track's Angle and Radius place
// its Sounds around the listener. Angle 0 is hard left, 0.5 straight ahead,
// 1 hard right and 1.5 behind, wrapping every 2; on two channels the rear
// half folds onto the front (what was in the rear speakers is mixed into
// the front two). Radius 0 is at the listener, both
// channels alike, 1 on the circle; beyond 1 the Sound is quieter by
// 1/Radius, up to 2. A negative Radius is the same distance behind. A
// track without Angle or Radius lanes is not panned; with them the defaults
// are 0.5 and 1. The balance is linear, so the channel on the
// position's side keeps its full level and a centred Sound is unchanged.
namespace talas::runtime
{
struct ScoreTrackPan
{
    double left = 1.0;
    double right = 1.0;
};

[[nodiscard]] inline std::optional<double> scoreLaneValueAt(
    const PreparedScoreSchedule& schedule, const PreparedScoreAutomationLane& lane, std::uint64_t frame) noexcept
{
    const auto points = schedule.automationPoints().subspan(lane.firstPoint, lane.pointCount);
    if (points.empty())
        return std::nullopt;
    const auto upper = std::upper_bound(points.begin(), points.end(), frame,
        [](std::uint64_t target, const PreparedScoreAutomationPoint& point) { return target < point.frame; });
    // Before its first point a lane holds its first value.
    if (upper == points.begin())
        return points.front().value;
    const auto current = upper - 1;
    if (upper == points.end() || current->interpolation != talas::core::AutomationInterpolation::Linear
        || upper->frame <= current->frame)
        return current->value;
    const auto ratio = static_cast<double>(frame - current->frame) / static_cast<double>(upper->frame - current->frame);
    return current->value + (upper->value - current->value) * ratio;
}

[[nodiscard]] inline bool scoreTrackIsPanned(const PreparedScoreSchedule& schedule, std::uint32_t trackIndex) noexcept
{
    return std::ranges::any_of(schedule.automationLanes(), [&](const PreparedScoreAutomationLane& lane) {
        return lane.scope == talas::project::ScoreAutomationScope::Track && lane.ownerIndex == trackIndex
            && (lane.eventName == std::string_view{"Angle"} || lane.eventName == std::string_view{"Radius"});
    });
}

[[nodiscard]] inline ScoreTrackPan scoreTrackPanFor(double angle, double radius) noexcept
{
    if (!std::isfinite(angle))
        angle = 0.5;
    if (!std::isfinite(radius))
        radius = 1.0;
    angle = std::fmod(angle, 2.0);
    if (angle < 0.0)
        angle += 2.0;
    // Left to right across the front; the rear mirrored onto it.
    const auto across = angle <= 1.0 ? angle : 2.0 - angle;
    const auto distance = std::min(std::abs(radius), 2.0);
    const auto position = 0.5 + (across - 0.5) * std::min(distance, 1.0);
    const auto level = distance > 1.0 ? 1.0 / distance : 1.0;
    return {std::clamp(2.0 * (1.0 - position), 0.0, 1.0) * level, std::clamp(2.0 * position, 0.0, 1.0) * level};
}

// On the speakers the Speaker Placement
// preferences set out, each an output channel, a panned Sound goes between
// the two speakers on either side of its angle as it goes between left and
// right on two channels, and spreads to all of them alike as its radius
// shrinks to 0; a negative radius is as far behind; a subwoofer takes
// none of it. Angle 0 is hard left, 0.5 ahead, 1 hard right, 1.5 behind,
// a speaker's degrees the angle times 180. On Stereo (0 and 180 degrees)
// it is scoreTrackPanFor's balance.
[[nodiscard]] inline std::array<double, talas::project::maximumSpeakers> scoreSpeakerGains(
    double angle, double radius, std::span<const talas::project::SpeakerPosition> speakers) noexcept
{
    std::array<double, talas::project::maximumSpeakers> gains{};
    if (!std::isfinite(angle))
        angle = 0.5;
    if (!std::isfinite(radius))
        radius = 1.0;
    const auto wrapped = [](double degrees) {
        degrees = std::fmod(degrees, 360.0);
        return degrees < 0.0 ? degrees + 360.0 : degrees;
    };
    auto direction = wrapped(angle * 180.0);
    if (radius < 0.0)
        direction = wrapped(360.0 - direction);
    const auto distance = std::min(std::abs(radius), 2.0);
    const auto level = distance > 1.0 ? 1.0 / distance : 1.0;
    const auto spread = std::min(distance, 1.0);
    const auto count = std::min(speakers.size(), gains.size());
    // The speakers on either side of the direction, round the circle.
    std::optional<std::size_t> before;
    std::optional<std::size_t> after;
    std::optional<std::size_t> lowest;
    std::optional<std::size_t> highest;
    for (std::size_t index = 0; index < count; ++index)
    {
        if (speakers[index].subwoofer)
            continue;
        const auto at = wrapped(speakers[index].angle);
        if (at <= direction && (!before || at >= wrapped(speakers[*before].angle)))
            before = index;
        if (at > direction && (!after || at < wrapped(speakers[*after].angle)))
            after = index;
        if (!lowest || at < wrapped(speakers[*lowest].angle))
            lowest = index;
        if (!highest || at >= wrapped(speakers[*highest].angle))
            highest = index;
    }
    if (!lowest.has_value())
        return gains;
    if (!before)
        before = highest;
    if (!after)
        after = lowest;
    std::array<double, talas::project::maximumSpeakers> pair{};
    if (*before == *after)
        pair[*before] = 1.0;
    else
    {
        const auto from = wrapped(speakers[*before].angle);
        auto arc = wrapped(wrapped(speakers[*after].angle) - from);
        if (arc <= 0.0)
            arc = 360.0;
        const auto fraction = std::clamp(wrapped(direction - from) / arc, 0.0, 1.0);
        pair[*before] = std::min(1.0, 2.0 * (1.0 - fraction));
        pair[*after] = std::min(1.0, 2.0 * fraction);
    }
    for (std::size_t index = 0; index < count; ++index)
    {
        if (!speakers[index].subwoofer)
            gains[index] = (spread * pair[index] + (1.0 - spread)) * level;
    }
    return gains;
}

[[nodiscard]] inline std::optional<ScoreTrackPan> scoreTrackPan(
    const PreparedScoreSchedule& schedule, std::uint32_t trackIndex, std::uint64_t frame) noexcept
{
    auto angle = 0.5;
    auto radius = 1.0;
    auto panned = false;
    for (const auto& lane : schedule.automationLanes())
    {
        if (lane.scope != talas::project::ScoreAutomationScope::Track || lane.ownerIndex != trackIndex)
            continue;
        const auto isAngle = lane.eventName == std::string_view{"Angle"};
        if (!isAngle && lane.eventName != std::string_view{"Radius"})
            continue;
        panned = true;
        if (const auto value = scoreLaneValueAt(schedule, lane, frame))
            (isAngle ? angle : radius) = *value;
    }
    if (!panned)
        return std::nullopt;
    return scoreTrackPanFor(angle, radius);
}

// Each Sound has a position relative to its
// track's, its Panner added to the track's Angle and its Distance to the
// track's Radius, both 0 at first; the track is the carrier, the Sound's
// position its deviation.
[[nodiscard]] inline bool scoreClipIsPanned(
    const PreparedScoreSchedule& schedule, std::uint32_t clipIndex, std::uint32_t trackIndex) noexcept
{
    // A track hard-wired to output channels is not panned.
    if (trackIndex < schedule.tracks().size() && !schedule.tracks()[trackIndex].outputChannels.empty())
        return false;
    return scoreTrackIsPanned(schedule, trackIndex)
        || std::ranges::any_of(schedule.automationLanes(), [&](const PreparedScoreAutomationLane& lane) {
               return lane.scope == talas::project::ScoreAutomationScope::Clip && lane.ownerIndex == clipIndex
                   && (lane.eventName == std::string_view{"Panner"} || lane.eventName == std::string_view{"Distance"});
           });
}

// The Sound's angle and radius at a frame, its track's with its own.
struct ScoreClipPanPosition
{
    double angle = 0.5;
    double radius = 1.0;
};

[[nodiscard]] inline std::optional<ScoreClipPanPosition> scoreClipPanPosition(const PreparedScoreSchedule& schedule,
    std::uint32_t clipIndex, std::uint32_t trackIndex, std::uint64_t frame) noexcept
{
    auto angle = 0.5;
    auto radius = 1.0;
    auto panned = false;
    for (const auto& lane : schedule.automationLanes())
    {
        const auto ownTrack = lane.scope == talas::project::ScoreAutomationScope::Track && lane.ownerIndex == trackIndex;
        const auto ownClip = lane.scope == talas::project::ScoreAutomationScope::Clip && lane.ownerIndex == clipIndex;
        const auto isAngle = ownTrack && lane.eventName == std::string_view{"Angle"};
        const auto isRadius = ownTrack && lane.eventName == std::string_view{"Radius"};
        const auto isPanner = ownClip && lane.eventName == std::string_view{"Panner"};
        const auto isDistance = ownClip && lane.eventName == std::string_view{"Distance"};
        if (!isAngle && !isRadius && !isPanner && !isDistance)
            continue;
        panned = true;
        const auto value = scoreLaneValueAt(schedule, lane, frame);
        if (!value.has_value())
            continue;
        if (isAngle)
            angle += *value - 0.5;
        else if (isRadius)
            radius += *value - 1.0;
        else if (isPanner)
            angle += *value;
        else
            radius += *value;
    }
    if (!panned)
        return std::nullopt;
    return ScoreClipPanPosition{angle, radius};
}

// The gain of each of a panned Sound's channels: on the speakers of the
// Speaker Placement, one a channel, those past them silent; with none set,
// on two channels, the left and right balance.
[[nodiscard]] inline std::optional<std::array<double, talas::project::maximumSpeakers>> scoreClipChannelGains(
    const PreparedScoreSchedule& schedule, std::uint32_t clipIndex, std::uint32_t trackIndex, std::uint64_t frame,
    std::span<const talas::project::SpeakerPosition> speakers, std::size_t channels) noexcept
{
    const auto position = scoreClipPanPosition(schedule, clipIndex, trackIndex, frame);
    if (!position.has_value())
        return std::nullopt;
    if (speakers.empty())
    {
        const auto pan = scoreTrackPanFor(position->angle, position->radius);
        return std::array<double, talas::project::maximumSpeakers>{pan.left, pan.right};
    }
    return scoreSpeakerGains(position->angle, position->radius, speakers.first(std::min(speakers.size(), channels)));
}

// Each Sound in a Timeline has a FadeInOut
// control, its amplitude envelope, times which it is heard (and sent); 1
// without a lane.
[[nodiscard]] inline bool scoreHasClipFades(const PreparedScoreSchedule& schedule) noexcept
{
    return std::ranges::any_of(schedule.automationLanes(), [](const PreparedScoreAutomationLane& lane) {
        return lane.scope == talas::project::ScoreAutomationScope::Clip && lane.eventName == std::string_view{"FadeInOut"};
    });
}

[[nodiscard]] inline double scoreClipFadeAt(
    const PreparedScoreSchedule& schedule, std::size_t clipIndex, std::uint64_t frame) noexcept
{
    double level = 1.0;
    for (const auto& lane : schedule.automationLanes())
    {
        if (lane.scope != talas::project::ScoreAutomationScope::Clip || lane.ownerIndex != clipIndex
            || lane.eventName != std::string_view{"FadeInOut"})
            continue;
        if (const auto value = scoreLaneValueAt(schedule, lane, frame); value.has_value() && std::isfinite(*value))
            level *= std::max(0.0, *value);
    }
    return level;
}

// A track's Level control, the fader its audio
// passes through on its way out, 1 without a lane. A Sound is heard at the
// Level of its track and of each submix its output route goes through,
// times its own fades; a send is taken before the track's Level.
[[nodiscard]] inline bool scoreHasTrackLevels(const PreparedScoreSchedule& schedule) noexcept
{
    return std::ranges::any_of(schedule.automationLanes(), [](const PreparedScoreAutomationLane& lane) {
        return lane.scope == talas::project::ScoreAutomationScope::Track && lane.eventName == std::string_view{"Level"};
    });
}

[[nodiscard]] inline double scoreTrackLevelAt(
    const PreparedScoreSchedule& schedule, std::uint32_t trackIndex, std::uint64_t frame) noexcept
{
    double level = 1.0;
    for (const auto& lane : schedule.automationLanes())
    {
        if (lane.scope != talas::project::ScoreAutomationScope::Track || lane.ownerIndex != trackIndex
            || lane.eventName != std::string_view{"Level"})
            continue;
        if (const auto value = scoreLaneValueAt(schedule, lane, frame); value.has_value() && std::isfinite(*value))
            level *= std::max(0.0, *value);
    }
    return level;
}

// The Levels from a track along its output route up to `untilTrack`
// (exclusive), or to the Master (inclusive) when it is none.
[[nodiscard]] inline double scoreRouteLevelAt(const PreparedScoreSchedule& schedule, std::uint32_t trackIndex,
    std::uint64_t frame, std::optional<std::uint32_t> untilTrack = std::nullopt) noexcept
{
    double level = 1.0;
    auto index = trackIndex;
    for (std::size_t depth = 0; depth < schedule.tracks().size() && index < schedule.tracks().size(); ++depth)
    {
        if (untilTrack.has_value() && index == *untilTrack)
            break;
        level *= scoreTrackLevelAt(schedule, index, frame);
        const auto& track = schedule.tracks()[index];
        if (track.kind == talas::project::ScoreTrackKind::Master)
            break;
        index = track.outputTrackIndex;
    }
    return level;
}
}
